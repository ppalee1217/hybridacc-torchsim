#pragma once

/**
 * @file DmaEngine.hpp
 * @brief cc_dma_engine — 4D strided DMA with bounded-outstanding AXI-Lite
 *        cluster interface and single-beat DRAM AXI master.
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

    static constexpr unsigned kDmaMaxOutstanding    = 16;
    static constexpr unsigned kDataBufferDepth      = 16;
    static constexpr unsigned kTicketFifoDepth      = 16;
    static constexpr unsigned kRespFifoDepth        = 16;

    static constexpr unsigned kClusterAddrBits      = 24; 
    static constexpr uint32_t kClusterAddrMask      = (1u << kClusterAddrBits) - 1;

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    sc_in<bool>          mmio_req_valid_i;
    sc_in<bool>          mmio_req_write_i;
    sc_in<sc_uint<32>>   mmio_req_addr_i;
    sc_in<sc_uint<32>>   mmio_req_wdata_i;
    sc_out<bool>         mmio_resp_valid_o;
    sc_out<sc_uint<32>>  mmio_resp_rdata_o;

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

    sc_out<bool>         dma_irq_o;

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

    struct AguPipe {
        struct Stage1 {
            bool valid = false;
            uint32_t prods[4] = {0, 0, 0, 0};
        };
        struct Stage2 {
            bool valid = false;
            uint32_t sum_prod = 0;
        };
        struct Stage3 {
            bool valid = false;
            uint32_t addr = 0;
        };

        Stage1 s1;
        Stage2 s2;
        Stage3 s3;

        void reset() {
            s1.valid = false;
            for (int i = 0; i < 4; ++i) s1.prods[i] = 0;
            s2.valid = false;
            s2.sum_prod = 0;
            s3.valid = false;
            s3.addr = 0;
        }
    };

    static uint32_t encode_cluster_fabric_addr(uint32_t cluster_id, uint32_t local_addr) {
        return (cluster_id << kClusterAddrBits) | (local_addr & kClusterAddrMask);
    }

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
    std::deque<DmaCommand> cmd_fifo_reg;
    DmaCommand active_cmd_reg;
    DmaState   state_reg = DmaState::IDLE;

    uint32_t agu_idx_reg[4] = {}; 
    uint32_t beats_src_issued_reg = 0;
    uint32_t beats_src_retired_reg = 0;
    uint32_t beats_dst_issued_reg = 0;
    uint32_t beats_dst_retired_reg = 0;

    std::deque<DataBeat> data_buffer_reg;

    unsigned dram_rd_inflight_cnt_reg = 0;
    unsigned dram_wr_inflight_cnt_reg = 0;
    std::deque<DramRdResp> dram_rd_resp_fifo_reg;

    unsigned cl_rd_inflight_cnt_reg = 0;
    unsigned cl_wr_inflight_cnt_reg = 0;
    std::deque<DataBeat> cl_rd_resp_fifo_reg;
    unsigned cl_wr_b_pending_reg = 0;

    bool     cl_aw_send_valid_reg = false;
    uint32_t cl_aw_send_addr_reg = 0;
    bool     cl_w_send_valid_reg = false;
    sc_biguint<kClAxiDataWidth> cl_w_send_data_reg{0};
    uint8_t  cl_w_send_strb_reg = 0;
    bool     cl_ar_send_valid_reg = false;
    uint32_t cl_ar_send_addr_reg = 0;

    bool     dram_aw_send_valid_reg = false;
    uint32_t dram_aw_send_addr_reg = 0;
    bool     dram_w_send_valid_reg = false;
    sc_biguint<kMemAxiDataWidth> dram_w_send_data_reg{0};
    uint8_t  dram_w_send_strb_reg = 0;
    bool     dram_ar_send_valid_reg = false;
    uint32_t dram_ar_send_addr_reg = 0;

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
    uint32_t mmio_status_reg = 0x1;
    uint32_t mmio_error_code_reg = 0;
    uint32_t mmio_done_tag_reg = 0;
    bool     mmio_irq_en_reg = false;

    uint64_t pmu_dram_rd_issue_cnt_reg = 0;
    uint64_t pmu_dram_wr_issue_cnt_reg = 0;
    uint64_t pmu_cl_aw_issue_cnt_reg = 0;
    uint64_t pmu_cl_w_issue_cnt_reg = 0;
    uint64_t pmu_cl_ar_issue_cnt_reg = 0;
    uint64_t pmu_cl_b_retire_cnt_reg = 0;
    uint64_t pmu_cl_r_retire_cnt_reg = 0;
    uint64_t pmu_cl_aw_stall_cnt_reg = 0;

    bool dram_b_prev_ready_reg = false;
    bool dram_r_prev_ready_reg = false;
    bool cl_b_prev_ready_reg = false;
    bool cl_r_prev_ready_reg = false;

    bool dram_ar_prev_valid_reg = false;
    bool dram_aw_prev_valid_reg = false;
    bool dram_w_prev_valid_reg  = false;
    bool cl_aw_prev_valid_reg   = false;
    bool cl_w_prev_valid_reg    = false;

    void reset_all_outputs() {
        mmio_resp_valid_o.write(false);
        mmio_resp_rdata_o.write(0);
        m_mem_axi_aw_valid_o.write(false);
        m_mem_axi_aw_addr_o.write(0);
        m_mem_axi_aw_len_o.write(0);
        m_mem_axi_w_valid_o.write(false);
        m_mem_axi_w_data_o.write(0);
        m_mem_axi_w_strb_o.write(0);
        m_mem_axi_w_last_o.write(true);
        m_mem_axi_b_ready_o.write(false);
        m_mem_axi_ar_valid_o.write(false);
        m_mem_axi_ar_addr_o.write(0);
        m_mem_axi_ar_len_o.write(0);
        m_mem_axi_r_ready_o.write(false);
        m_cl_axi_aw_valid_o.write(false);
        m_cl_axi_aw_addr_o.write(0);
        m_cl_axi_w_valid_o.write(false);
        m_cl_axi_w_data_o.write(0);
        m_cl_axi_w_strb_o.write(0);
        m_cl_axi_b_ready_o.write(false);
        m_cl_axi_ar_valid_o.write(false);
        m_cl_axi_ar_addr_o.write(0);
        m_cl_axi_r_ready_o.write(false);
        dma_irq_o.write(false);
    }

    void reset_internal_state() {
        cmd_fifo_reg.clear();
        active_cmd_reg = DmaCommand{};
        state_reg = DmaState::IDLE;
        reset_execution_state();
        mmio_status_reg = 0x1;
        mmio_error_code_reg = 0;
    }

    void reset_execution_state() {
        for (auto& v : agu_idx_reg) v = 0;
        for (auto& v : dst_agu_idx_reg) v = 0;
        beats_src_issued_reg = 0;
        beats_src_retired_reg = 0;
        beats_dst_issued_reg = 0;
        beats_dst_retired_reg = 0;
        beats_src_queued_reg = 0;
        beats_dst_queued_reg = 0;
        src_agu_pipe_reg.reset();
        dst_agu_pipe_reg.reset();
        src_addr_fifo.clear();
        dst_addr_fifo.clear();
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
        dram_ar_prev_valid_reg = false;
        dram_aw_prev_valid_reg = false;
        dram_w_prev_valid_reg  = false;
        cl_aw_prev_valid_reg   = false;
        cl_w_prev_valid_reg    = false;
        dram_b_prev_ready_reg = false;
        dram_r_prev_ready_reg = false;
        cl_b_prev_ready_reg = false;
        cl_r_prev_ready_reg = false;
    }

    void advance_src_agu() {
        for (int d = 0; d < 4; ++d) {
            uint32_t lim = (active_cmd_reg.count[d] == 0) ? 1 : active_cmd_reg.count[d];
            agu_idx_reg[d]++;
            if (agu_idx_reg[d] < lim) return;
            agu_idx_reg[d] = 0;
        }
    }

    uint32_t dst_agu_idx_reg[4] = {};
    void advance_dst_agu() {
        for (int d = 0; d < 4; ++d) {
            uint32_t lim = (active_cmd_reg.count[d] == 0) ? 1 : active_cmd_reg.count[d];
            dst_agu_idx_reg[d]++;
            if (dst_agu_idx_reg[d] < lim) return;
            dst_agu_idx_reg[d] = 0;
        }
    }

    AguPipe src_agu_pipe_reg;
    AguPipe dst_agu_pipe_reg;
    std::deque<uint32_t> src_addr_fifo;
    std::deque<uint32_t> dst_addr_fifo;
    uint32_t beats_src_queued_reg = 0;
    uint32_t beats_dst_queued_reg = 0;

    void update_agu_pipelines() {
        const uint32_t total = active_cmd_reg.total_beats();
        if (src_addr_fifo.size() < 8) {
            if (src_agu_pipe_reg.s3.valid) src_addr_fifo.push_back(src_agu_pipe_reg.s3.addr);
            src_agu_pipe_reg.s3.valid = src_agu_pipe_reg.s2.valid;
            src_agu_pipe_reg.s3.addr = active_cmd_reg.src_addr_lo + src_agu_pipe_reg.s2.sum_prod;
            src_agu_pipe_reg.s2.valid = src_agu_pipe_reg.s1.valid;
            src_agu_pipe_reg.s2.sum_prod = 0;
            for (int i = 0; i < 4; ++i) src_agu_pipe_reg.s2.sum_prod += src_agu_pipe_reg.s1.prods[i];
            if (beats_src_queued_reg < total) {
                src_agu_pipe_reg.s1.valid = true;
                for (int i = 0; i < 4; ++i) src_agu_pipe_reg.s1.prods[i] = agu_idx_reg[i] * active_cmd_reg.src_stride[i];
                advance_src_agu();
                beats_src_queued_reg++;
            } else src_agu_pipe_reg.s1.valid = false;
        }
        if (dst_addr_fifo.size() < 8) {
            if (dst_agu_pipe_reg.s3.valid) dst_addr_fifo.push_back(dst_agu_pipe_reg.s3.addr);
            dst_agu_pipe_reg.s3.valid = dst_agu_pipe_reg.s2.valid;
            dst_agu_pipe_reg.s3.addr = active_cmd_reg.dst_addr_lo + dst_agu_pipe_reg.s2.sum_prod;
            dst_agu_pipe_reg.s2.valid = dst_agu_pipe_reg.s1.valid;
            dst_agu_pipe_reg.s2.sum_prod = 0;
            for (int i = 0; i < 4; ++i) dst_agu_pipe_reg.s2.sum_prod += dst_agu_pipe_reg.s1.prods[i];
            if (beats_dst_queued_reg < total) {
                dst_agu_pipe_reg.s1.valid = true;
                for (int i = 0; i < 4; ++i) dst_agu_pipe_reg.s1.prods[i] = dst_agu_idx_reg[i] * active_cmd_reg.dst_stride[i];
                advance_dst_agu();
                beats_dst_queued_reg++;
            } else dst_agu_pipe_reg.s1.valid = false;
        }
    }

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
                case 0x008: {
                    mmio_irq_en_reg = (wdata & 0x8) != 0;
                    if (wdata & 0x1) {
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
            switch (offset) {
                case 0x000: mmio_resp_rdata_o.write(0x444D4100); break;
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
                case 0x068: mmio_resp_rdata_o.write(0); break;
                case 0x06C: mmio_resp_rdata_o.write(static_cast<uint32_t>(state_reg)); break; 
                default: mmio_resp_rdata_o.write(0); break;
            }
        }
    }

    void seq_process() {
        reset_all_outputs();
        reset_internal_state();
        wait();
        DmaState prev_state = state_reg;
        while (true) {
            reset_all_outputs();
            handle_mmio();
            switch (state_reg) {
            case DmaState::IDLE: {
                mmio_status_reg = 0x1;
                if (!cmd_fifo_reg.empty()) {
                    active_cmd_reg = cmd_fifo_reg.front();
                    cmd_fifo_reg.pop_front();
                    state_reg = DmaState::VALIDATE;
                }
                break;
            }
            case DmaState::VALIDATE: {
                const uint32_t total = active_cmd_reg.total_beats();
                if (total == 0) {
                    mmio_error_code_reg = static_cast<uint32_t>(DmaError::DMA_ERR_ZERO_LENGTH);
                    state_reg = DmaState::ERROR;
                    break;
                }
                TRACE_EVENT("dma_transfer", "DMA", TRACE_BEGIN, 0, 1, "{}");
                reset_execution_state();
                state_reg = DmaState::RUN;
                mmio_status_reg = 0x2; 
                break;
            }
            case DmaState::RUN: {
                const uint32_t total = active_cmd_reg.total_beats();
                bool src_is_dram = (active_cmd_reg.src_kind == DmaEndpoint::DRAM);
                bool dst_is_dram = (active_cmd_reg.dst_kind == DmaEndpoint::DRAM);
                update_agu_pipelines();
                if (src_is_dram) run_src_retire_dram(); else run_src_retire_cluster();
                if (dst_is_dram) run_dst_retire_dram(); else run_dst_retire_cluster();
                drive_dram_axi_outputs();
                if (src_is_dram) run_src_issue_dram(beats_src_issued_reg < total);
                else             run_src_issue_cluster(beats_src_issued_reg < total);
                if (dst_is_dram) run_dst_issue_dram(); else run_dst_issue_cluster(beats_dst_issued_reg < total);
                drive_cluster_axi_outputs();
                if (beats_dst_retired_reg >= total &&
                    dram_rd_inflight_cnt_reg == 0 && dram_wr_inflight_cnt_reg == 0 &&
                    cl_rd_inflight_cnt_reg == 0 && cl_wr_inflight_cnt_reg == 0 &&
                    data_buffer_reg.empty() && src_addr_fifo.empty() && dst_addr_fifo.empty() &&
                    !src_agu_pipe_reg.s1.valid && !src_agu_pipe_reg.s2.valid && !src_agu_pipe_reg.s3.valid &&
                    !dst_agu_pipe_reg.s1.valid && !dst_agu_pipe_reg.s2.valid && !dst_agu_pipe_reg.s3.valid &&
                    dram_rd_resp_fifo_reg.empty() && cl_rd_resp_fifo_reg.empty()) {
                    state_reg = DmaState::DONE;
                }
                break;
            }
            case DmaState::DRAIN: {
                drive_dram_axi_outputs();
                bool src_is_dram = (active_cmd_reg.src_kind == DmaEndpoint::DRAM);
                run_src_issue_dram(false);
                run_dst_issue_cluster(false);
                drive_cluster_axi_outputs();
                if (src_is_dram) run_src_retire_dram(); else run_src_retire_cluster();
                if (active_cmd_reg.dst_kind == DmaEndpoint::DRAM) run_dst_retire_dram(); else run_dst_retire_cluster();
                if (dram_rd_inflight_cnt_reg == 0 && dram_wr_inflight_cnt_reg == 0 &&
                    cl_rd_inflight_cnt_reg == 0 && cl_wr_inflight_cnt_reg == 0) {
                    state_reg = DmaState::DONE;
                }
                break;
            }
            case DmaState::DONE: {
                TRACE_EVENT("dma_transfer", "DMA", TRACE_END, 0, 1, "{}");
                mmio_done_tag_reg = active_cmd_reg.cmd_tag;
                mmio_status_reg = 0x1;
                if (mmio_irq_en_reg) dma_irq_o.write(true);
                state_reg = DmaState::IDLE;
                break;
            }
            case DmaState::ERROR: {
                TRACE_EVENT("dma_transfer", "DMA", TRACE_END, 0, 1, "{}");
                mmio_done_tag_reg = active_cmd_reg.cmd_tag;
                mmio_status_reg = 0x1;
                if (mmio_irq_en_reg) dma_irq_o.write(true);
                state_reg = DmaState::IDLE;
                break;
            }
            default: state_reg = DmaState::IDLE; break;
            }
            if (state_reg != prev_state) {
                DEBUG_PRINTF(DEBUG_LEVEL_CORE_COMPONENTS, "DMA state %s -> %s\n", dma_state_name(prev_state), dma_state_name(state_reg));
                prev_state = state_reg;
            }
            wait();
        }
    }

    void run_src_issue_dram(bool allow_issue) {
        const bool ar_done = dram_ar_send_valid_reg && m_mem_axi_ar_ready_i.read();
        bool next_valid = dram_ar_send_valid_reg && !ar_done;
        uint32_t next_addr = dram_ar_send_addr_reg;
        if (allow_issue && !next_valid && (data_buffer_reg.size() + dram_rd_inflight_cnt_reg < kDataBufferDepth) && 
            dram_rd_inflight_cnt_reg < kDmaMaxOutstanding && !src_addr_fifo.empty()) {
            next_valid = true;
            next_addr = src_addr_fifo.front();
            src_addr_fifo.pop_front();
            dram_rd_inflight_cnt_reg++;
            beats_src_issued_reg++;
            pmu_dram_rd_issue_cnt_reg++;
        }
        dram_ar_send_valid_reg = next_valid;
        dram_ar_send_addr_reg = next_addr;
        m_mem_axi_ar_valid_o.write(next_valid);
        m_mem_axi_ar_addr_o.write(next_addr);
        m_mem_axi_ar_len_o.write(0);
    }

    void run_src_issue_cluster(bool allow_issue) {
        const bool ar_done = cl_ar_send_valid_reg && m_cl_axi_ar_ready_i.read();
        bool next_valid = cl_ar_send_valid_reg && !ar_done;
        uint32_t next_addr = cl_ar_send_addr_reg;
        if (allow_issue && !next_valid && (data_buffer_reg.size() + cl_rd_inflight_cnt_reg < kDataBufferDepth) && 
            cl_rd_inflight_cnt_reg < kDmaMaxOutstanding && !src_addr_fifo.empty()) {
            next_valid = true;
            next_addr = encode_cluster_fabric_addr(active_cmd_reg.src_cluster_id, src_addr_fifo.front());
            src_addr_fifo.pop_front();
            cl_rd_inflight_cnt_reg++;
            beats_src_issued_reg++;
        }
        cl_ar_send_valid_reg = next_valid;
        cl_ar_send_addr_reg = next_addr;
        m_cl_axi_ar_valid_o.write(next_valid);
        m_cl_axi_ar_addr_o.write(next_addr);
    }

    void run_src_retire_dram() {
        bool can_accept = (dram_rd_inflight_cnt_reg > 0) && (dram_rd_resp_fifo_reg.size() < kRespFifoDepth);
        if (can_accept) m_mem_axi_r_ready_o.write(true);
        if (m_mem_axi_r_valid_i.read() && dram_r_prev_ready_reg) {
            DramRdResp resp; resp.data = m_mem_axi_r_data_i.read(); resp.resp = m_mem_axi_r_resp_i.read().to_uint();
            dram_rd_resp_fifo_reg.push_back(resp);
            dram_rd_inflight_cnt_reg--;
        }
        dram_r_prev_ready_reg = can_accept;
        if (!dram_rd_resp_fifo_reg.empty() && data_buffer_reg.size() < kDataBufferDepth) {
            auto& front = dram_rd_resp_fifo_reg.front();
            DataBeat beat; beat.data = sc_biguint<kClAxiDataWidth>(front.data.to_uint64()); beat.strb = 0xFF;
            data_buffer_reg.push_back(beat);
            dram_rd_resp_fifo_reg.pop_front();
            beats_src_retired_reg++;
        }
    }

    void run_src_retire_cluster() {
        bool can_accept = (cl_rd_inflight_cnt_reg > 0) && (cl_rd_resp_fifo_reg.size() < kRespFifoDepth);
        if (can_accept) m_cl_axi_r_ready_o.write(true);
        if (m_cl_axi_r_valid_i.read() && cl_r_prev_ready_reg) {
            DataBeat beat; beat.data = m_cl_axi_r_data_i.read(); beat.strb = 0xFF;
            cl_rd_resp_fifo_reg.push_back(beat);
            cl_rd_inflight_cnt_reg--;
            pmu_cl_r_retire_cnt_reg++;
        }
        cl_r_prev_ready_reg = can_accept;
        if (!cl_rd_resp_fifo_reg.empty() && data_buffer_reg.size() < kDataBufferDepth) {
            auto& front = cl_rd_resp_fifo_reg.front();
            data_buffer_reg.push_back(front);
            cl_rd_resp_fifo_reg.pop_front();
            beats_src_retired_reg++;
        }
    }

    void run_dst_issue_dram() {
        if (dram_aw_send_valid_reg && m_mem_axi_aw_ready_i.read()) dram_aw_send_valid_reg = false;
        if (dram_w_send_valid_reg && m_mem_axi_w_ready_i.read()) dram_w_send_valid_reg = false;
        if (!dram_aw_send_valid_reg && !dram_w_send_valid_reg && (beats_dst_issued_reg < active_cmd_reg.total_beats()) && 
            dram_wr_inflight_cnt_reg < kDmaMaxOutstanding && !data_buffer_reg.empty() && !dst_addr_fifo.empty()) {
            dram_aw_send_addr_reg = dst_addr_fifo.front(); dst_addr_fifo.pop_front();
            auto beat = data_buffer_reg.front(); data_buffer_reg.pop_front();
            dram_aw_send_valid_reg = true;
            dram_w_send_valid_reg = true;
            dram_w_send_data_reg = sc_biguint<kMemAxiDataWidth>(beat.data.to_uint64());
            dram_w_send_strb_reg = beat.strb;
            dram_wr_inflight_cnt_reg++;
            beats_dst_issued_reg++;
            pmu_dram_wr_issue_cnt_reg++;
        }
        m_mem_axi_aw_valid_o.write(dram_aw_send_valid_reg);
        m_mem_axi_aw_addr_o.write(dram_aw_send_addr_reg);
        m_mem_axi_aw_len_o.write(0);
        m_mem_axi_w_valid_o.write(dram_w_send_valid_reg);
        m_mem_axi_w_data_o.write(dram_w_send_data_reg);
        m_mem_axi_w_strb_o.write(dram_w_send_strb_reg);
        m_mem_axi_w_last_o.write(dram_w_send_valid_reg);
    }

    void run_dst_issue_cluster(bool allow_refill) {
        const bool aw_done = cl_aw_prev_valid_reg && m_cl_axi_aw_ready_i.read();
        const bool w_done = cl_w_prev_valid_reg && m_cl_axi_w_ready_i.read();
        bool next_aw_valid = cl_aw_send_valid_reg && !aw_done;
        bool next_w_valid = cl_w_send_valid_reg && !w_done;
        if (allow_refill && !next_aw_valid && !next_w_valid && !data_buffer_reg.empty() && 
            (cl_wr_inflight_cnt_reg < kDmaMaxOutstanding) && !dst_addr_fifo.empty()) {
            cl_aw_send_addr_reg = encode_cluster_fabric_addr(active_cmd_reg.dst_cluster_id, dst_addr_fifo.front());
            dst_addr_fifo.pop_front();
            auto beat = data_buffer_reg.front(); data_buffer_reg.pop_front();
            next_aw_valid = true;
            next_w_valid = true;
            cl_w_send_data_reg = beat.data;
            cl_w_send_strb_reg = beat.strb;
            cl_wr_inflight_cnt_reg++;
            beats_dst_issued_reg++;
            pmu_cl_aw_issue_cnt_reg++;
            pmu_cl_w_issue_cnt_reg++;
        }
        cl_aw_send_valid_reg = next_aw_valid;
        cl_w_send_valid_reg = next_w_valid;
        cl_aw_prev_valid_reg = next_aw_valid;
        cl_w_prev_valid_reg = next_w_valid;
        m_cl_axi_aw_valid_o.write(next_aw_valid);
        m_cl_axi_aw_addr_o.write(cl_aw_send_addr_reg);
        m_cl_axi_w_valid_o.write(next_w_valid);
        m_cl_axi_w_data_o.write(cl_w_send_data_reg);
        m_cl_axi_w_strb_o.write(cl_w_send_strb_reg);
    }

    void run_dst_retire_dram() {
        bool can_accept = (dram_wr_inflight_cnt_reg > 0);
        if (can_accept) m_mem_axi_b_ready_o.write(true);
        if (m_mem_axi_b_valid_i.read() && dram_b_prev_ready_reg) {
            dram_wr_inflight_cnt_reg--;
            beats_dst_retired_reg++;
        }
        dram_b_prev_ready_reg = can_accept;
    }

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

    void drive_dram_axi_outputs() {
        if (dram_aw_prev_valid_reg && m_mem_axi_aw_ready_i.read()) dram_aw_send_valid_reg = false;
        if (dram_aw_send_valid_reg) {
            m_mem_axi_aw_valid_o.write(true);
            m_mem_axi_aw_addr_o.write(dram_aw_send_addr_reg);
            m_mem_axi_aw_len_o.write(0);
        }
        dram_aw_prev_valid_reg = dram_aw_send_valid_reg;
        if (dram_w_prev_valid_reg && m_mem_axi_w_ready_i.read()) dram_w_send_valid_reg = false;
        if (dram_w_send_valid_reg) {
            m_mem_axi_w_valid_o.write(true);
            m_mem_axi_w_data_o.write(dram_w_send_data_reg);
            m_mem_axi_w_strb_o.write(dram_w_send_strb_reg);
            m_mem_axi_w_last_o.write(true);
        }
        dram_w_prev_valid_reg = dram_w_send_valid_reg;
    }

    void drive_cluster_axi_outputs() {}
};

} // namespace core
} // namespace hybridacc
