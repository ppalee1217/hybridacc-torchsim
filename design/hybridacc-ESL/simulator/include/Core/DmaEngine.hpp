#pragma once

/**
 * @file DmaEngine.hpp
 * @brief cc_dma_engine — 4D strided DMA with bounded-outstanding AXI-Lite
 *        cluster interface and single-beat DRAM AXI master.
 *
 * @par Key design decisions (per report §4)
 *   - DRAM side: single-beat AXI4 (ARLEN/AWLEN=0), bounded outstanding.
 *   - Cluster side: standard AXI4-Lite 5-channel (AW/W/B/AR/R) master.
 *   - No cluster_id sideband; cluster routing encoded in AWADDR/ARADDR.
 *   - Four parallel sub-engines: src_issue, src_retire, dst_issue, dst_retire.
 *   - Per-direction ticket FIFO + inflight counter + response FIFO.
 *   - Outstanding limit = 4 (bring-up default).
 */

#include <systemc>
#include <cstdint>
#include <deque>
#include <cassert>
#include "Core/Types.hpp"
#include "Utils/utils.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

SC_MODULE(DmaEngine) {

    // ========================================================================
    // Compile-time parameters
    // ========================================================================

    static constexpr unsigned kDmaMaxOutstanding    = 4;
    static constexpr unsigned kDataBufferDepth      = 8;
    static constexpr unsigned kTicketFifoDepth      = 4;
    static constexpr unsigned kRespFifoDepth        = 4;

    // Cluster fabric address encoding: upper bits = cluster_id
    static constexpr unsigned kClusterAddrBits      = 24; // bits for local addr
    static constexpr uint32_t kClusterAddrMask      = (1u << kClusterAddrBits) - 1;

    // ========================================================================
    // External ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- MMIO control interface (from CmdFabric) ---
    sc_in<bool>          mmio_req_valid_i;
    sc_in<bool>          mmio_req_write_i;
    sc_in<sc_uint<32>>   mmio_req_addr_i;
    sc_in<sc_uint<32>>   mmio_req_wdata_i;
    sc_out<bool>         mmio_resp_valid_o;
    sc_out<sc_uint<32>>  mmio_resp_rdata_o;

    // --- DRAM AXI4 master (single-beat, ARLEN/AWLEN=0) ---
    sc_out<bool>         m_mem_axi_aw_valid_o;
    sc_in<bool>          m_mem_axi_aw_ready_i;
    sc_out<sc_uint<32>>  m_mem_axi_aw_addr_o;
    sc_out<sc_uint<8>>   m_mem_axi_aw_len_o;
    sc_out<bool>         m_mem_axi_w_valid_o;
    sc_in<bool>          m_mem_axi_w_ready_i;
    sc_out<sc_biguint<kMemAxiDataWidth>> m_mem_axi_w_data_o;
    sc_out<sc_uint<kMemAxiDataWidth / 8>> m_mem_axi_w_strb_o;
    sc_out<bool>         m_mem_axi_w_last_o;
    sc_in<bool>          m_mem_axi_b_valid_i;
    sc_out<bool>         m_mem_axi_b_ready_o;
    sc_in<sc_uint<2>>    m_mem_axi_b_resp_i;
    sc_out<bool>         m_mem_axi_ar_valid_o;
    sc_in<bool>          m_mem_axi_ar_ready_i;
    sc_out<sc_uint<32>>  m_mem_axi_ar_addr_o;
    sc_out<sc_uint<8>>   m_mem_axi_ar_len_o;
    sc_in<bool>          m_mem_axi_r_valid_i;
    sc_out<bool>         m_mem_axi_r_ready_o;
    sc_in<sc_biguint<kMemAxiDataWidth>> m_mem_axi_r_data_i;
    sc_in<sc_uint<2>>    m_mem_axi_r_resp_i;
    sc_in<bool>          m_mem_axi_r_last_i;

    // --- Cluster AXI4-Lite master (to ClusterDataFabric) ---
    sc_out<bool>         m_cl_axi_aw_valid_o;
    sc_in<bool>          m_cl_axi_aw_ready_i;
    sc_out<sc_uint<32>>  m_cl_axi_aw_addr_o;
    sc_out<bool>         m_cl_axi_w_valid_o;
    sc_in<bool>          m_cl_axi_w_ready_i;
    sc_out<sc_biguint<kClAxiDataWidth>> m_cl_axi_w_data_o;
    sc_out<sc_uint<kClAxiDataWidth / 8>> m_cl_axi_w_strb_o;
    sc_in<bool>          m_cl_axi_b_valid_i;
    sc_out<bool>         m_cl_axi_b_ready_o;
    sc_in<sc_uint<2>>    m_cl_axi_b_resp_i;
    sc_out<bool>         m_cl_axi_ar_valid_o;
    sc_in<bool>          m_cl_axi_ar_ready_i;
    sc_out<sc_uint<32>>  m_cl_axi_ar_addr_o;
    sc_in<bool>          m_cl_axi_r_valid_i;
    sc_out<bool>         m_cl_axi_r_ready_o;
    sc_in<sc_biguint<kClAxiDataWidth>> m_cl_axi_r_data_i;
    sc_in<sc_uint<2>>    m_cl_axi_r_resp_i;

    // --- IRQ output ---
    sc_out<bool>         dma_irq_o;

    // ========================================================================
    // Internal types
    // ========================================================================

    enum class DmaState : uint8_t {
        IDLE, LOAD_CMD, VALIDATE, RUN, DRAIN, DONE, ERROR
    };

    friend std::ostream& operator<<(std::ostream& os, DmaState s) {
        switch (s) {
            case DmaState::IDLE:     return os << "IDLE";
            case DmaState::LOAD_CMD: return os << "LOAD_CMD";
            case DmaState::VALIDATE: return os << "VALIDATE";
            case DmaState::RUN:      return os << "RUN";
            case DmaState::DRAIN:    return os << "DRAIN";
            case DmaState::DONE:     return os << "DONE";
            case DmaState::ERROR:    return os << "ERROR";
            default:                 return os << "??";
        }
    }

    static const char* dma_state_name(DmaState state) {
        switch (state) {
            case DmaState::IDLE:     return "IDLE";
            case DmaState::LOAD_CMD: return "LOAD_CMD";
            case DmaState::VALIDATE: return "VALIDATE";
            case DmaState::RUN:      return "RUN";
            case DmaState::DRAIN:    return "DRAIN";
            case DmaState::DONE:     return "DONE";
            case DmaState::ERROR:    return "ERROR";
            default:                 return "??";
        }
    }

    struct DataBeat {
        sc_biguint<kClAxiDataWidth> data{0};
        uint8_t strb{0};
        bool operator==(const DataBeat& o) const { return data == o.data && strb == o.strb; }
    };

    struct DramRdResp {
        sc_biguint<kMemAxiDataWidth> data{0};
        uint8_t resp{0};
        bool operator==(const DramRdResp& o) const { return data == o.data && resp == o.resp; }
    };

    // ========================================================================
    // Address helpers
    // ========================================================================

    /// Encode cluster_id + local_addr into a global fabric address.
    static uint32_t encode_cluster_fabric_addr(uint32_t cluster_id, uint32_t local_addr) {
        return (cluster_id << kClusterAddrBits) | (local_addr & kClusterAddrMask);
    }

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(DmaEngine);

    DmaEngine(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          mmio_req_valid_i("mmio_req_valid_i"),
          mmio_req_write_i("mmio_req_write_i"),
          mmio_req_addr_i("mmio_req_addr_i"),
          mmio_req_wdata_i("mmio_req_wdata_i"),
          mmio_resp_valid_o("mmio_resp_valid_o"),
          mmio_resp_rdata_o("mmio_resp_rdata_o"),
          m_mem_axi_aw_valid_o("m_mem_axi_aw_valid_o"),
          m_mem_axi_aw_ready_i("m_mem_axi_aw_ready_i"),
          m_mem_axi_aw_addr_o("m_mem_axi_aw_addr_o"),
          m_mem_axi_aw_len_o("m_mem_axi_aw_len_o"),
          m_mem_axi_w_valid_o("m_mem_axi_w_valid_o"),
          m_mem_axi_w_ready_i("m_mem_axi_w_ready_i"),
          m_mem_axi_w_data_o("m_mem_axi_w_data_o"),
          m_mem_axi_w_strb_o("m_mem_axi_w_strb_o"),
          m_mem_axi_w_last_o("m_mem_axi_w_last_o"),
          m_mem_axi_b_valid_i("m_mem_axi_b_valid_i"),
          m_mem_axi_b_ready_o("m_mem_axi_b_ready_o"),
          m_mem_axi_b_resp_i("m_mem_axi_b_resp_i"),
          m_mem_axi_ar_valid_o("m_mem_axi_ar_valid_o"),
          m_mem_axi_ar_ready_i("m_mem_axi_ar_ready_i"),
          m_mem_axi_ar_addr_o("m_mem_axi_ar_addr_o"),
          m_mem_axi_ar_len_o("m_mem_axi_ar_len_o"),
          m_mem_axi_r_valid_i("m_mem_axi_r_valid_i"),
          m_mem_axi_r_ready_o("m_mem_axi_r_ready_o"),
          m_mem_axi_r_data_i("m_mem_axi_r_data_i"),
          m_mem_axi_r_resp_i("m_mem_axi_r_resp_i"),
          m_mem_axi_r_last_i("m_mem_axi_r_last_i"),
          m_cl_axi_aw_valid_o("m_cl_axi_aw_valid_o"),
          m_cl_axi_aw_ready_i("m_cl_axi_aw_ready_i"),
          m_cl_axi_aw_addr_o("m_cl_axi_aw_addr_o"),
          m_cl_axi_w_valid_o("m_cl_axi_w_valid_o"),
          m_cl_axi_w_ready_i("m_cl_axi_w_ready_i"),
          m_cl_axi_w_data_o("m_cl_axi_w_data_o"),
          m_cl_axi_w_strb_o("m_cl_axi_w_strb_o"),
          m_cl_axi_b_valid_i("m_cl_axi_b_valid_i"),
          m_cl_axi_b_ready_o("m_cl_axi_b_ready_o"),
          m_cl_axi_b_resp_i("m_cl_axi_b_resp_i"),
          m_cl_axi_ar_valid_o("m_cl_axi_ar_valid_o"),
          m_cl_axi_ar_ready_i("m_cl_axi_ar_ready_i"),
          m_cl_axi_ar_addr_o("m_cl_axi_ar_addr_o"),
          m_cl_axi_r_valid_i("m_cl_axi_r_valid_i"),
          m_cl_axi_r_ready_o("m_cl_axi_r_ready_o"),
          m_cl_axi_r_data_i("m_cl_axi_r_data_i"),
          m_cl_axi_r_resp_i("m_cl_axi_r_resp_i"),
          dma_irq_o("dma_irq_o")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    // ========================================================================
    // MMIO staging registers (firmware-visible)
    // ========================================================================

    // Command FIFO (staging → active)
    std::deque<DmaCommand> cmd_fifo_reg;

    // Active command
    DmaCommand active_cmd_reg;
    DmaState   state_reg = DmaState::IDLE;

    // ========================================================================
    // 4D AGU state
    // ========================================================================

    uint32_t agu_src_addr_reg = 0;
    uint32_t agu_dst_addr_reg = 0;
    uint32_t agu_idx_reg[4] = {};       // 4D iteration counters
    uint32_t beats_src_issued_reg = 0;
    uint32_t beats_src_retired_reg = 0;
    uint32_t beats_dst_issued_reg = 0;
    uint32_t beats_dst_retired_reg = 0;

    // ========================================================================
    // Data buffer FIFO (source retire → destination issue)
    // ========================================================================

    std::deque<DataBeat> data_buffer_reg;

    // ========================================================================
    // DRAM side inflight tracking
    // ========================================================================

    unsigned dram_rd_inflight_cnt_reg = 0;
    unsigned dram_wr_inflight_cnt_reg = 0;
    std::deque<DramRdResp> dram_rd_resp_fifo_reg;

    // ========================================================================
    // Cluster AXI side inflight tracking
    // ========================================================================

    unsigned cl_rd_inflight_cnt_reg = 0;
    unsigned cl_wr_inflight_cnt_reg = 0;
    std::deque<DataBeat> cl_rd_resp_fifo_reg;
    unsigned cl_wr_b_pending_reg = 0;

    // Cluster AXI send registers (hold until handshake)
    bool     cl_aw_send_valid_reg = false;
    uint32_t cl_aw_send_addr_reg = 0;
    bool     cl_w_send_valid_reg = false;
    sc_biguint<kClAxiDataWidth> cl_w_send_data_reg{0};
    uint8_t  cl_w_send_strb_reg = 0;
    bool     cl_ar_send_valid_reg = false;
    uint32_t cl_ar_send_addr_reg = 0;

    // DRAM AXI send registers
    bool     dram_aw_send_valid_reg = false;
    uint32_t dram_aw_send_addr_reg = 0;
    bool     dram_w_send_valid_reg = false;
    sc_biguint<kMemAxiDataWidth> dram_w_send_data_reg{0};
    uint8_t  dram_w_send_strb_reg = 0;
    bool     dram_ar_send_valid_reg = false;
    uint32_t dram_ar_send_addr_reg = 0;

    // ========================================================================
    // MMIO register file (DMA configuration)
    // ========================================================================

    uint32_t mmio_src_kind_reg = 0;
    uint32_t mmio_dst_kind_reg = 0;
    uint32_t mmio_src_addr_lo_reg = 0;
    uint32_t mmio_src_addr_hi_reg = 0;
    uint32_t mmio_dst_addr_lo_reg = 0;
    uint32_t mmio_dst_addr_hi_reg = 0;
    uint32_t mmio_src_cluster_id_reg = 0;
    uint32_t mmio_dst_cluster_id_reg = 0;
    uint32_t mmio_count_reg[4] = {};
    uint32_t mmio_src_stride_reg[4] = {};
    uint32_t mmio_dst_stride_reg[4] = {};
    uint32_t mmio_cmd_tag_reg = 0;
    uint32_t mmio_status_reg = 0;  // bit0=IDLE(1), bit1=BUSY(0)
    uint32_t mmio_error_code_reg = 0;
    uint32_t mmio_done_tag_reg = 0;
    bool     mmio_irq_en_reg = false;

    // ========================================================================
    // PMU counters
    // ========================================================================

    uint64_t pmu_dram_rd_issue_cnt_reg = 0;
    uint64_t pmu_dram_wr_issue_cnt_reg = 0;
    uint64_t pmu_cl_aw_issue_cnt_reg = 0;
    uint64_t pmu_cl_w_issue_cnt_reg = 0;
    uint64_t pmu_cl_ar_issue_cnt_reg = 0;
    uint64_t pmu_cl_b_retire_cnt_reg = 0;
    uint64_t pmu_cl_r_retire_cnt_reg = 0;
    uint64_t pmu_dram_rd_stall_cnt_reg = 0;
    uint64_t pmu_dram_wr_stall_cnt_reg = 0;
    uint64_t pmu_cl_aw_stall_cnt_reg = 0;
    uint64_t pmu_cl_ar_stall_cnt_reg = 0;

    // Previous-cycle ready tracking (prevents double-process in SC_CTHREAD)
    bool dram_b_prev_ready_reg = false;
    bool dram_r_prev_ready_reg = false;
    bool cl_b_prev_ready_reg = false;
    bool cl_r_prev_ready_reg = false;

    // Previous-cycle valid tracking for master-side send registers.
    // In SC_CTHREAD the slave sees our valid one cycle late; clearing the
    // send register the same cycle we first drive valid would let it
    // disappear before the slave samples it.  We only clear when
    // prev_valid (= we drove valid last cycle) AND ready is high now.
    bool dram_ar_prev_valid_reg = false;
    bool dram_aw_prev_valid_reg = false;
    bool dram_w_prev_valid_reg  = false;
    bool cl_ar_prev_valid_reg   = false;
    bool cl_aw_prev_valid_reg   = false;
    bool cl_w_prev_valid_reg    = false;

    // ========================================================================
    // Reset helpers
    // ========================================================================

    void reset_all_outputs() {
        // MMIO
        mmio_resp_valid_o.write(false);
        mmio_resp_rdata_o.write(0);
        // DRAM AXI
        m_mem_axi_aw_valid_o.write(false);
        m_mem_axi_aw_addr_o.write(0);
        m_mem_axi_aw_len_o.write(0);
        m_mem_axi_w_valid_o.write(false);
        m_mem_axi_w_data_o.write(0);
        m_mem_axi_w_strb_o.write(0);
        m_mem_axi_w_last_o.write(true);  // single-beat: always last
        m_mem_axi_b_ready_o.write(false);
        m_mem_axi_ar_valid_o.write(false);
        m_mem_axi_ar_addr_o.write(0);
        m_mem_axi_ar_len_o.write(0);
        m_mem_axi_r_ready_o.write(false);
        // Cluster AXI-Lite
        m_cl_axi_aw_valid_o.write(false);
        m_cl_axi_aw_addr_o.write(0);
        m_cl_axi_w_valid_o.write(false);
        m_cl_axi_w_data_o.write(0);
        m_cl_axi_w_strb_o.write(0);
        m_cl_axi_b_ready_o.write(false);
        m_cl_axi_ar_valid_o.write(false);
        m_cl_axi_ar_addr_o.write(0);
        m_cl_axi_r_ready_o.write(false);
        // IRQ
        dma_irq_o.write(false);
    }

    void reset_internal_state() {
        cmd_fifo_reg.clear();
        active_cmd_reg = DmaCommand{};
        state_reg = DmaState::IDLE;
        for (auto& v : agu_idx_reg) v = 0;
        agu_src_addr_reg = 0;
        agu_dst_addr_reg = 0;
        beats_src_issued_reg = 0;
        beats_src_retired_reg = 0;
        beats_dst_issued_reg = 0;
        beats_dst_retired_reg = 0;
        data_buffer_reg.clear();
        dram_rd_inflight_cnt_reg = 0;
        dram_wr_inflight_cnt_reg = 0;
        dram_rd_resp_fifo_reg.clear();
        cl_rd_inflight_cnt_reg = 0;
        cl_wr_inflight_cnt_reg = 0;
        cl_rd_resp_fifo_reg.clear();
        cl_wr_b_pending_reg = 0;
        cl_aw_send_valid_reg = false;
        cl_w_send_valid_reg = false;
        cl_ar_send_valid_reg = false;
        dram_aw_send_valid_reg = false;
        dram_w_send_valid_reg = false;
        dram_ar_send_valid_reg = false;
        dram_b_prev_ready_reg = false;
        dram_r_prev_ready_reg = false;
        cl_b_prev_ready_reg = false;
        cl_r_prev_ready_reg = false;
        dram_ar_prev_valid_reg = false;
        dram_aw_prev_valid_reg = false;
        dram_w_prev_valid_reg  = false;
        cl_ar_prev_valid_reg   = false;
        cl_aw_prev_valid_reg   = false;
        cl_w_prev_valid_reg    = false;
        mmio_status_reg = 0x1; // IDLE
        mmio_error_code_reg = 0;
    }

    // ========================================================================
    // 4D AGU: advance source address
    // ========================================================================

    uint32_t compute_src_addr() const {
        uint32_t addr = active_cmd_reg.src_addr_lo;
        for (int d = 0; d < 4; ++d)
            addr += agu_idx_reg[d] * active_cmd_reg.src_stride[d];
        return addr;
    }

    uint32_t compute_dst_addr() const {
        uint32_t addr = active_cmd_reg.dst_addr_lo;
        for (int d = 0; d < 4; ++d)
            addr += agu_idx_reg[d] * active_cmd_reg.dst_stride[d];
        return addr;
    }

    void advance_src_agu() {
        for (int d = 0; d < 4; ++d) {
            uint32_t lim = (active_cmd_reg.count[d] == 0) ? 1 : active_cmd_reg.count[d];
            agu_idx_reg[d]++;
            if (agu_idx_reg[d] < lim) return;
            agu_idx_reg[d] = 0;
        }
    }

    // Separate dst AGU index (re-uses same 4D loop, but tracks dst beats)
    uint32_t dst_agu_idx_reg[4] = {};

    uint32_t compute_dst_addr_from_dst_agu() const {
        uint32_t addr = active_cmd_reg.dst_addr_lo;
        for (int d = 0; d < 4; ++d)
            addr += dst_agu_idx_reg[d] * active_cmd_reg.dst_stride[d];
        return addr;
    }

    void advance_dst_agu() {
        for (int d = 0; d < 4; ++d) {
            uint32_t lim = (active_cmd_reg.count[d] == 0) ? 1 : active_cmd_reg.count[d];
            dst_agu_idx_reg[d]++;
            if (dst_agu_idx_reg[d] < lim) return;
            dst_agu_idx_reg[d] = 0;
        }
    }

    // ========================================================================
    // MMIO handler
    // ========================================================================

    void handle_mmio() {
        mmio_resp_valid_o.write(false);
        mmio_resp_rdata_o.write(0);

        if (!mmio_req_valid_i.read()) return;

        const uint32_t offset = mmio_req_addr_i.read().to_uint() & 0x7FF;
        const bool is_write = mmio_req_write_i.read();
        const uint32_t wdata = mmio_req_wdata_i.read().to_uint();

        mmio_resp_valid_o.write(true);

        if (is_write) {
            switch (offset) {
                // 0x000: CAP0 (RO, ignore write)
                // 0x004: STATUS (RO, ignore write)
                case 0x008: { // DMA_CTRL — bit0=SUBMIT, bit3=IRQ_EN
                    mmio_irq_en_reg = (wdata & 0x8) != 0;
                    if (wdata & 0x1) { // SUBMIT
                        if (cmd_fifo_reg.size() < kDmaCmdFifoDepth) {
                            DmaCommand cmd{};
                            cmd.src_kind = static_cast<DmaEndpoint>(mmio_src_kind_reg);
                            cmd.dst_kind = static_cast<DmaEndpoint>(mmio_dst_kind_reg);
                            cmd.src_addr_lo = mmio_src_addr_lo_reg;
                            cmd.src_addr_hi = mmio_src_addr_hi_reg;
                            cmd.dst_addr_lo = mmio_dst_addr_lo_reg;
                            cmd.dst_addr_hi = mmio_dst_addr_hi_reg;
                            cmd.src_cluster_id = mmio_src_cluster_id_reg;
                            cmd.dst_cluster_id = mmio_dst_cluster_id_reg;
                            for (int i = 0; i < 4; ++i) {
                                cmd.count[i] = mmio_count_reg[i];
                                cmd.src_stride[i] = mmio_src_stride_reg[i];
                                cmd.dst_stride[i] = mmio_dst_stride_reg[i];
                            }
                            cmd.cmd_tag = mmio_cmd_tag_reg;
                            cmd_fifo_reg.push_back(cmd);
                        }
                    }
                    break;
                }
                case 0x00C: mmio_src_kind_reg = wdata; break;
                case 0x010: mmio_dst_kind_reg = wdata; break;
                case 0x014: mmio_src_addr_lo_reg = wdata; break;
                case 0x018: mmio_src_addr_hi_reg = wdata; break;
                case 0x01C: mmio_dst_addr_lo_reg = wdata; break;
                case 0x020: mmio_dst_addr_hi_reg = wdata; break;
                case 0x024: mmio_src_cluster_id_reg = wdata; break;
                case 0x028: mmio_dst_cluster_id_reg = wdata; break;
                case 0x02C: mmio_count_reg[0] = wdata; break;
                case 0x030: mmio_count_reg[1] = wdata; break;
                case 0x034: mmio_count_reg[2] = wdata; break;
                case 0x038: mmio_count_reg[3] = wdata; break;
                case 0x03C: mmio_src_stride_reg[0] = wdata; break;
                case 0x040: mmio_src_stride_reg[1] = wdata; break;
                case 0x044: mmio_src_stride_reg[2] = wdata; break;
                case 0x048: mmio_src_stride_reg[3] = wdata; break;
                case 0x04C: mmio_dst_stride_reg[0] = wdata; break;
                case 0x050: mmio_dst_stride_reg[1] = wdata; break;
                case 0x054: mmio_dst_stride_reg[2] = wdata; break;
                case 0x058: mmio_dst_stride_reg[3] = wdata; break;
                case 0x05C: mmio_cmd_tag_reg = wdata; break;
                default: break;
            }
        } else {
            // Read
            switch (offset) {
                case 0x000: mmio_resp_rdata_o.write(0x444D4100); break; // DMA_CAP0 magic
                case 0x004: mmio_resp_rdata_o.write(mmio_status_reg); break;
                case 0x00C: mmio_resp_rdata_o.write(mmio_src_kind_reg); break;
                case 0x010: mmio_resp_rdata_o.write(mmio_dst_kind_reg); break;
                case 0x014: mmio_resp_rdata_o.write(mmio_src_addr_lo_reg); break;
                case 0x018: mmio_resp_rdata_o.write(mmio_src_addr_hi_reg); break;
                case 0x01C: mmio_resp_rdata_o.write(mmio_dst_addr_lo_reg); break;
                case 0x020: mmio_resp_rdata_o.write(mmio_dst_addr_hi_reg); break;
                case 0x024: mmio_resp_rdata_o.write(mmio_src_cluster_id_reg); break;
                case 0x028: mmio_resp_rdata_o.write(mmio_dst_cluster_id_reg); break;
                case 0x02C: mmio_resp_rdata_o.write(mmio_count_reg[0]); break;
                case 0x030: mmio_resp_rdata_o.write(mmio_count_reg[1]); break;
                case 0x034: mmio_resp_rdata_o.write(mmio_count_reg[2]); break;
                case 0x038: mmio_resp_rdata_o.write(mmio_count_reg[3]); break;
                case 0x03C: mmio_resp_rdata_o.write(mmio_src_stride_reg[0]); break;
                case 0x040: mmio_resp_rdata_o.write(mmio_src_stride_reg[1]); break;
                case 0x044: mmio_resp_rdata_o.write(mmio_src_stride_reg[2]); break;
                case 0x048: mmio_resp_rdata_o.write(mmio_src_stride_reg[3]); break;
                case 0x04C: mmio_resp_rdata_o.write(mmio_dst_stride_reg[0]); break;
                case 0x050: mmio_resp_rdata_o.write(mmio_dst_stride_reg[1]); break;
                case 0x054: mmio_resp_rdata_o.write(mmio_dst_stride_reg[2]); break;
                case 0x058: mmio_resp_rdata_o.write(mmio_dst_stride_reg[3]); break;
                case 0x05C: mmio_resp_rdata_o.write(mmio_cmd_tag_reg); break;
                case 0x060: mmio_resp_rdata_o.write(mmio_done_tag_reg); break;
                case 0x064: mmio_resp_rdata_o.write(mmio_error_code_reg); break;
                case 0x068: mmio_resp_rdata_o.write(0); break; // ERR_INFO
                case 0x06C: mmio_resp_rdata_o.write(
                    static_cast<uint32_t>(state_reg)); break; // DEBUG_STATE
                default: mmio_resp_rdata_o.write(0); break;
            }
        }
    }

    // ========================================================================
    // Main sequential process
    // ========================================================================

    void seq_process() {
        // --- Reset ---
        reset_all_outputs();
        reset_internal_state();
        for (auto& v : dst_agu_idx_reg) v = 0;
        wait();

        // --- Main loop ---
        DmaState prev_state = state_reg;
        while (true) {
            // Default output values each cycle
            reset_all_outputs();

            // Always service MMIO
            handle_mmio();

            switch (state_reg) {
            case DmaState::IDLE: {
                mmio_status_reg = 0x1; // bit0=IDLE
                if (!cmd_fifo_reg.empty()) {
                    active_cmd_reg = cmd_fifo_reg.front();
                    cmd_fifo_reg.pop_front();
                    state_reg = DmaState::VALIDATE;
                }
                break;
            }

            case DmaState::VALIDATE: {
                // Basic validation
                const uint32_t total = active_cmd_reg.total_beats();
                if (total == 0) {
                    mmio_error_code_reg = static_cast<uint32_t>(DmaError::DMA_ERR_ZERO_LENGTH);
                    state_reg = DmaState::ERROR;
                    break;
                }
                // Initialize AGU
                for (auto& v : agu_idx_reg) v = 0;
                for (auto& v : dst_agu_idx_reg) v = 0;
                beats_src_issued_reg = 0;
                beats_src_retired_reg = 0;
                beats_dst_issued_reg = 0;
                beats_dst_retired_reg = 0;
                data_buffer_reg.clear();
                dram_rd_resp_fifo_reg.clear();
                cl_rd_resp_fifo_reg.clear();
                dram_rd_inflight_cnt_reg = 0;
                dram_wr_inflight_cnt_reg = 0;
                cl_rd_inflight_cnt_reg = 0;
                cl_wr_inflight_cnt_reg = 0;
                cl_wr_b_pending_reg = 0;
                cl_aw_send_valid_reg = false;
                cl_w_send_valid_reg = false;
                cl_ar_send_valid_reg = false;
                dram_aw_send_valid_reg = false;
                dram_w_send_valid_reg = false;
                dram_ar_send_valid_reg = false;
                mmio_status_reg = 0x2; // bit1=BUSY
                state_reg = DmaState::RUN;
                break;
            }

            case DmaState::RUN: {
                const uint32_t total = active_cmd_reg.total_beats();
                bool src_is_dram = (active_cmd_reg.src_kind == DmaEndpoint::DRAM);
                bool dst_is_dram = (active_cmd_reg.dst_kind == DmaEndpoint::DRAM);

                // === Source issue engine ===
                if (beats_src_issued_reg < total) {
                    if (src_is_dram) {
                        run_src_issue_dram();
                    } else {
                        run_src_issue_cluster();
                    }
                }

                // === Source retire engine ===
                if (src_is_dram) {
                    run_src_retire_dram();
                } else {
                    run_src_retire_cluster();
                }

                // === Destination issue engine ===
                if (beats_dst_issued_reg < total) {
                    if (dst_is_dram) {
                        run_dst_issue_dram();
                    } else {
                        run_dst_issue_cluster();
                    }
                }

                // === Destination retire engine ===
                if (dst_is_dram) {
                    run_dst_retire_dram();
                } else {
                    run_dst_retire_cluster();
                }

                // === Drive AXI outputs from send registers ===
                drive_dram_axi_outputs();
                drive_cluster_axi_outputs();

                // === Check completion ===
                if (beats_dst_retired_reg >= total &&
                    dram_rd_inflight_cnt_reg == 0 &&
                    dram_wr_inflight_cnt_reg == 0 &&
                    cl_rd_inflight_cnt_reg == 0 &&
                    cl_wr_inflight_cnt_reg == 0 &&
                    data_buffer_reg.empty() &&
                    dram_rd_resp_fifo_reg.empty() &&
                    cl_rd_resp_fifo_reg.empty()) {
                    state_reg = DmaState::DONE;
                }
                break;
            }

            case DmaState::DRAIN: {
                // Wait for all inflight to retire
                drive_dram_axi_outputs();
                drive_cluster_axi_outputs();

                bool src_is_dram = (active_cmd_reg.src_kind == DmaEndpoint::DRAM);
                bool dst_is_dram = (active_cmd_reg.dst_kind == DmaEndpoint::DRAM);
                if (src_is_dram) run_src_retire_dram();
                else             run_src_retire_cluster();
                if (dst_is_dram) run_dst_retire_dram();
                else             run_dst_retire_cluster();

                if (dram_rd_inflight_cnt_reg == 0 &&
                    dram_wr_inflight_cnt_reg == 0 &&
                    cl_rd_inflight_cnt_reg == 0 &&
                    cl_wr_inflight_cnt_reg == 0) {
                    state_reg = DmaState::DONE;
                }
                break;
            }

            case DmaState::DONE: {
                mmio_done_tag_reg = active_cmd_reg.cmd_tag;
                mmio_status_reg = 0x1; // back to IDLE
                mmio_error_code_reg = 0;
                if (mmio_irq_en_reg) dma_irq_o.write(true);
                state_reg = DmaState::IDLE;
                break;
            }

            case DmaState::ERROR: {
                mmio_done_tag_reg = active_cmd_reg.cmd_tag;
                mmio_status_reg = 0x1; // back to IDLE (with error)
                if (mmio_irq_en_reg) dma_irq_o.write(true);
                state_reg = DmaState::IDLE;
                break;
            }

            default:
                state_reg = DmaState::IDLE;
                break;
            }

            if (state_reg != prev_state) {
                DEBUG_PRINTF(
                    DEBUG_LEVEL_CORE_COMPONENTS,
                    "DMA state %s -> %s tag=0x%08x total=%u src_kind=%u dst_kind=%u src_cl=%u dst_cl=%u src_iss=%u src_ret=%u dst_iss=%u dst_ret=%u inflight(dram_r=%u dram_w=%u cl_r=%u cl_w=%u) databuf=%llu err=0x%08x\n",
                    dma_state_name(prev_state),
                    dma_state_name(state_reg),
                    active_cmd_reg.cmd_tag,
                    active_cmd_reg.total_beats(),
                    static_cast<unsigned>(active_cmd_reg.src_kind),
                    static_cast<unsigned>(active_cmd_reg.dst_kind),
                    active_cmd_reg.src_cluster_id,
                    active_cmd_reg.dst_cluster_id,
                    beats_src_issued_reg,
                    beats_src_retired_reg,
                    beats_dst_issued_reg,
                    beats_dst_retired_reg,
                    dram_rd_inflight_cnt_reg,
                    dram_wr_inflight_cnt_reg,
                    cl_rd_inflight_cnt_reg,
                    cl_wr_inflight_cnt_reg,
                    static_cast<unsigned long long>(data_buffer_reg.size()),
                    mmio_error_code_reg);
                prev_state = state_reg;
            }

            wait();
        }
    }

    // ========================================================================
    // Sub-engines
    // ========================================================================

    /// Source issue: DRAM read path — issue AR to DRAM.
    void run_src_issue_dram() {
        if (dram_ar_send_valid_reg) return; // waiting for handshake
        if (dram_rd_inflight_cnt_reg >= kDmaMaxOutstanding) {
            pmu_dram_rd_stall_cnt_reg++;
            return;
        }
        if (data_buffer_reg.size() + dram_rd_inflight_cnt_reg >= kDataBufferDepth) return;

        uint32_t addr = compute_src_addr();
        dram_ar_send_valid_reg = true;
        dram_ar_send_addr_reg = addr;
        dram_rd_inflight_cnt_reg++;
        beats_src_issued_reg++;
        advance_src_agu();
        pmu_dram_rd_issue_cnt_reg++;
    }

    /// Source issue: cluster read path — issue AR to fabric.
    void run_src_issue_cluster() {
        if (cl_ar_send_valid_reg) return;
        if (cl_rd_inflight_cnt_reg >= kDmaMaxOutstanding) {
            pmu_cl_ar_stall_cnt_reg++;
            return;
        }
        if (data_buffer_reg.size() + cl_rd_inflight_cnt_reg >= kDataBufferDepth) return;

        uint32_t local_addr = compute_src_addr();
        uint32_t global_addr = encode_cluster_fabric_addr(
            active_cmd_reg.src_cluster_id, local_addr);
        cl_ar_send_valid_reg = true;
        cl_ar_send_addr_reg = global_addr;
        cl_rd_inflight_cnt_reg++;
        beats_src_issued_reg++;
        advance_src_agu();
        pmu_cl_ar_issue_cnt_reg++;
    }

    /// Source retire: DRAM R capture → data_buffer.
    void run_src_retire_dram() {
        // Pre-assert r_ready when can accept
        bool can_accept = (dram_rd_inflight_cnt_reg > 0) &&
                          (dram_rd_resp_fifo_reg.size() < kRespFifoDepth);
        if (can_accept) m_mem_axi_r_ready_o.write(true);
        // Only process when valid AND we previously advertised ready
        if (m_mem_axi_r_valid_i.read() && dram_r_prev_ready_reg) {
            DramRdResp resp;
            resp.data = m_mem_axi_r_data_i.read();
            resp.resp = m_mem_axi_r_resp_i.read().to_uint();
            dram_rd_resp_fifo_reg.push_back(resp);
            dram_rd_inflight_cnt_reg--;
        }
        dram_r_prev_ready_reg = can_accept;
        // Move from resp FIFO to data buffer
        if (!dram_rd_resp_fifo_reg.empty() && data_buffer_reg.size() < kDataBufferDepth) {
            auto& front = dram_rd_resp_fifo_reg.front();
            DataBeat beat;
            beat.data = sc_biguint<kClAxiDataWidth>(front.data.to_uint64());
            beat.strb = 0xFF; // full strobe
            data_buffer_reg.push_back(beat);
            dram_rd_resp_fifo_reg.pop_front();
            beats_src_retired_reg++;
        }
    }

    /// Source retire: cluster R capture → data_buffer.
    void run_src_retire_cluster() {
        bool can_accept = (cl_rd_inflight_cnt_reg > 0) &&
                          (cl_rd_resp_fifo_reg.size() < kRespFifoDepth);
        if (can_accept) m_cl_axi_r_ready_o.write(true);
        if (m_cl_axi_r_valid_i.read() && cl_r_prev_ready_reg) {
            DataBeat beat;
            beat.data = m_cl_axi_r_data_i.read();
            beat.strb = 0xFF;
            cl_rd_resp_fifo_reg.push_back(beat);
            cl_rd_inflight_cnt_reg--;
            pmu_cl_r_retire_cnt_reg++;
        }
        cl_r_prev_ready_reg = can_accept;
        // Move from resp FIFO to data buffer
        if (!cl_rd_resp_fifo_reg.empty() && data_buffer_reg.size() < kDataBufferDepth) {
            auto& front = cl_rd_resp_fifo_reg.front();
            data_buffer_reg.push_back(front);
            cl_rd_resp_fifo_reg.pop_front();
            beats_src_retired_reg++;
        }
    }

    /// Destination issue: DRAM write path — issue AW/W.
    void run_dst_issue_dram() {
        if (dram_aw_send_valid_reg || dram_w_send_valid_reg) return;
        if (dram_wr_inflight_cnt_reg >= kDmaMaxOutstanding) {
            pmu_dram_wr_stall_cnt_reg++;
            return;
        }
        if (data_buffer_reg.empty()) return;

        uint32_t addr = compute_dst_addr_from_dst_agu();
        auto beat = data_buffer_reg.front();
        data_buffer_reg.pop_front();

        dram_aw_send_valid_reg = true;
        dram_aw_send_addr_reg = addr;
        dram_w_send_valid_reg = true;
        dram_w_send_data_reg = sc_biguint<kMemAxiDataWidth>(beat.data.to_uint64());
        dram_w_send_strb_reg = beat.strb;
        dram_wr_inflight_cnt_reg++;
        beats_dst_issued_reg++;
        advance_dst_agu();
        pmu_dram_wr_issue_cnt_reg++;
    }

    /// Destination issue: cluster write path — issue AW/W.
    void run_dst_issue_cluster() {
        if (cl_aw_send_valid_reg || cl_w_send_valid_reg) return;
        if (cl_wr_inflight_cnt_reg >= kDmaMaxOutstanding) {
            pmu_cl_aw_stall_cnt_reg++;
            return;
        }
        if (data_buffer_reg.empty()) return;

        uint32_t local_addr = compute_dst_addr_from_dst_agu();
        uint32_t global_addr = encode_cluster_fabric_addr(
            active_cmd_reg.dst_cluster_id, local_addr);
        auto beat = data_buffer_reg.front();
        data_buffer_reg.pop_front();

        cl_aw_send_valid_reg = true;
        cl_aw_send_addr_reg = global_addr;
        cl_w_send_valid_reg = true;
        cl_w_send_data_reg = beat.data;
        cl_w_send_strb_reg = beat.strb;
        cl_wr_inflight_cnt_reg++;
        beats_dst_issued_reg++;
        advance_dst_agu();
        pmu_cl_aw_issue_cnt_reg++;
        pmu_cl_w_issue_cnt_reg++;
    }

    /// Destination retire: DRAM B capture.
    void run_dst_retire_dram() {
        bool can_accept = (dram_wr_inflight_cnt_reg > 0);
        if (can_accept) m_mem_axi_b_ready_o.write(true);
        if (m_mem_axi_b_valid_i.read() && dram_b_prev_ready_reg) {
            dram_wr_inflight_cnt_reg--;
            beats_dst_retired_reg++;
        }
        dram_b_prev_ready_reg = can_accept;
    }

    /// Destination retire: cluster B capture.
    void run_dst_retire_cluster() {
        bool can_accept = (cl_wr_inflight_cnt_reg > 0);
        if (can_accept) m_cl_axi_b_ready_o.write(true);
        if (m_cl_axi_b_valid_i.read() && cl_b_prev_ready_reg) {
            cl_wr_inflight_cnt_reg--;
            beats_dst_retired_reg++;
            pmu_cl_b_retire_cnt_reg++;
        }
        cl_b_prev_ready_reg = can_accept;
    }

    // ========================================================================
    // AXI output drivers (from send registers)
    // ========================================================================

    void drive_dram_axi_outputs() {
        // AR channel — clear only when slave has had a cycle to see valid
        if (dram_ar_prev_valid_reg && m_mem_axi_ar_ready_i.read()) {
            dram_ar_send_valid_reg = false;
        }
        if (dram_ar_send_valid_reg) {
            m_mem_axi_ar_valid_o.write(true);
            m_mem_axi_ar_addr_o.write(dram_ar_send_addr_reg);
            m_mem_axi_ar_len_o.write(0); // single-beat
        }
        dram_ar_prev_valid_reg = dram_ar_send_valid_reg;

        // AW channel
        if (dram_aw_prev_valid_reg && m_mem_axi_aw_ready_i.read()) {
            dram_aw_send_valid_reg = false;
        }
        if (dram_aw_send_valid_reg) {
            m_mem_axi_aw_valid_o.write(true);
            m_mem_axi_aw_addr_o.write(dram_aw_send_addr_reg);
            m_mem_axi_aw_len_o.write(0);
        }
        dram_aw_prev_valid_reg = dram_aw_send_valid_reg;

        // W channel
        if (dram_w_prev_valid_reg && m_mem_axi_w_ready_i.read()) {
            dram_w_send_valid_reg = false;
        }
        if (dram_w_send_valid_reg) {
            m_mem_axi_w_valid_o.write(true);
            m_mem_axi_w_data_o.write(dram_w_send_data_reg);
            m_mem_axi_w_strb_o.write(dram_w_send_strb_reg);
            m_mem_axi_w_last_o.write(true);
        }
        dram_w_prev_valid_reg = dram_w_send_valid_reg;
    }

    void drive_cluster_axi_outputs() {
        // AR channel
        if (cl_ar_prev_valid_reg && m_cl_axi_ar_ready_i.read()) {
            cl_ar_send_valid_reg = false;
        }
        if (cl_ar_send_valid_reg) {
            m_cl_axi_ar_valid_o.write(true);
            m_cl_axi_ar_addr_o.write(cl_ar_send_addr_reg);
        }
        cl_ar_prev_valid_reg = cl_ar_send_valid_reg;

        // AW channel
        if (cl_aw_prev_valid_reg && m_cl_axi_aw_ready_i.read()) {
            cl_aw_send_valid_reg = false;
        }
        if (cl_aw_send_valid_reg) {
            m_cl_axi_aw_valid_o.write(true);
            m_cl_axi_aw_addr_o.write(cl_aw_send_addr_reg);
        }
        cl_aw_prev_valid_reg = cl_aw_send_valid_reg;

        // W channel
        if (cl_w_prev_valid_reg && m_cl_axi_w_ready_i.read()) {
            cl_w_send_valid_reg = false;
        }
        if (cl_w_send_valid_reg) {
            m_cl_axi_w_valid_o.write(true);
            m_cl_axi_w_data_o.write(cl_w_send_data_reg);
            m_cl_axi_w_strb_o.write(cl_w_send_strb_reg);
        }
        cl_w_prev_valid_reg = cl_w_send_valid_reg;
    }
};

} // namespace core
} // namespace hybridacc
