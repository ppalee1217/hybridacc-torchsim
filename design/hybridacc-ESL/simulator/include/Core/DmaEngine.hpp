#pragma once

/**
 * @file DmaEngine.hpp
 * @brief cc_dma_engine — DMA engine with unified 4D strided copy, dual
 *        internal AGU (source + destination), transfer FIFO for read/write
 *        pipelining, and MMIO staging registers + command FIFO.
 *
 * Firmware writes staging registers, then commits with @c DMA_CTRL.submit.
 * The engine snapshots the staging set into an internal FIFO and processes
 * commands sequentially, driving DRAM AXI + cluster data fabric as needed.
 *
 * 4D address formula (per beat):
 *   addr = base + d0*stride_d0 + d1*stride_d1 + d2*stride_d2 + d3*stride_d3
 *
 * @par MMIO register map (base = 0x2000_1000)
 *   See Core.md §8.8 for the complete register definition.
 *
 * @par Spec reference
 *   Core.md §8.8  cc_dma_engine
 */

#include <systemc>
#include <cstdint>
#include <array>
#include <queue>
#include "Utils/utils.hpp"
#include "Utils/FIFO.hpp"
#include "Core/Types.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

SC_MODULE(DmaEngine) {

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- MMIO interface (from cc_cmd_fabric) ---
    sc_in<bool>          mmio_req_valid_i;
    sc_in<bool>          mmio_req_write_i;
    sc_in<sc_uint<32>>   mmio_req_addr_i;   ///< local offset within DMA window
    sc_in<sc_uint<32>>   mmio_req_wdata_i;
    sc_out<bool>         mmio_resp_valid_o;
    sc_out<sc_uint<32>>  mmio_resp_rdata_o;

    // --- DRAM AXI4 master (simplified read/write channels) ---
    // Write address
    sc_out<bool>         m_mem_axi_aw_valid_o;
    sc_in<bool>          m_mem_axi_aw_ready_i;
    sc_out<sc_uint<32>>  m_mem_axi_aw_addr_o;
    sc_out<sc_uint<8>>   m_mem_axi_aw_len_o;
    // Write data
    sc_out<bool>         m_mem_axi_w_valid_o;
    sc_in<bool>          m_mem_axi_w_ready_i;
    sc_out<sc_biguint<kMemAxiDataWidth>> m_mem_axi_w_data_o;
    sc_out<sc_uint<kMemAxiDataWidth / 8>> m_mem_axi_w_strb_o;
    sc_out<bool>         m_mem_axi_w_last_o;
    // Write response
    sc_in<bool>          m_mem_axi_b_valid_i;
    sc_out<bool>         m_mem_axi_b_ready_o;
    sc_in<sc_uint<2>>    m_mem_axi_b_resp_i;
    // Read address
    sc_out<bool>         m_mem_axi_ar_valid_o;
    sc_in<bool>          m_mem_axi_ar_ready_i;
    sc_out<sc_uint<32>>  m_mem_axi_ar_addr_o;
    sc_out<sc_uint<8>>   m_mem_axi_ar_len_o;
    // Read data
    sc_in<bool>          m_mem_axi_r_valid_i;
    sc_out<bool>         m_mem_axi_r_ready_o;
    sc_in<sc_biguint<kMemAxiDataWidth>> m_mem_axi_r_data_i;
    sc_in<sc_uint<2>>    m_mem_axi_r_resp_i;
    sc_in<bool>          m_mem_axi_r_last_i;

    // --- Cluster data fabric requester ---
    sc_out<bool>         cl_data_req_valid_o;
    sc_out<bool>         cl_data_req_write_o;
    sc_out<sc_uint<32>>  cl_data_req_cluster_id_o;
    sc_out<sc_uint<32>>  cl_data_req_addr_o;
    sc_out<sc_biguint<kClAxiDataWidth>>          cl_data_req_wdata_o;
    sc_out<sc_uint<kClAxiDataWidth / 8>>         cl_data_req_wstrb_o;
    sc_in<bool>          cl_data_req_ready_i;
    sc_in<bool>          cl_data_resp_valid_i;
    sc_in<sc_biguint<kClAxiDataWidth>>           cl_data_resp_rdata_i;
    sc_in<bool>          cl_data_resp_error_i;

    // --- IRQ output ---
    sc_out<bool>         dma_irq_o;

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
          cl_data_req_valid_o("cl_data_req_valid_o"),
          cl_data_req_write_o("cl_data_req_write_o"),
          cl_data_req_cluster_id_o("cl_data_req_cluster_id_o"),
          cl_data_req_addr_o("cl_data_req_addr_o"),
          cl_data_req_wdata_o("cl_data_req_wdata_o"),
          cl_data_req_wstrb_o("cl_data_req_wstrb_o"),
          cl_data_req_ready_i("cl_data_req_ready_i"),
          cl_data_resp_valid_i("cl_data_resp_valid_i"),
          cl_data_resp_rdata_i("cl_data_resp_rdata_i"),
          cl_data_resp_error_i("cl_data_resp_error_i"),
          dma_irq_o("dma_irq_o"),
          // Transfer FIFO
          u_xfer_fifo("u_xfer_fifo", kXferFifoDepth)
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Wire transfer FIFO
        u_xfer_fifo.clk(clk);
        u_xfer_fifo.reset_n(reset_n);
        u_xfer_fifo.data_in(sig_fifo_data_in);
        u_xfer_fifo.push(sig_fifo_push);
        u_xfer_fifo.data_out(sig_fifo_data_out);
        u_xfer_fifo.pop(sig_fifo_pop);
        u_xfer_fifo.empty(sig_fifo_empty);
        u_xfer_fifo.full(sig_fifo_full);
        u_xfer_fifo.clear(sig_fifo_clear);
    }

private:
    // ========================================================================
    // Internal types
    // ========================================================================

    enum class DmaState : uint32_t {
        IDLE          = 0,
        VALIDATE      = 1,
        // Source read states
        SRC_READ_ISSUE   = 2,   ///< issue read to source (DRAM AR or cluster)
        DRAM_AR_WAIT     = 3,   ///< wait for AR handshake
        DRAM_R_WAIT      = 4,   ///< wait for R channel data, push FIFO
        CL_RD_REQ_WAIT   = 5,   ///< wait for cluster read ready
        CL_RD_RESP_WAIT  = 6,   ///< wait for cluster read response
        // Destination write states
        DST_WRITE_ISSUE  = 7,   ///< pop FIFO, issue write to dest
        DRAM_AW_WAIT     = 8,   ///< wait for AW handshake
        DRAM_W_WAIT      = 9,   ///< wait for W handshake
        DRAM_B_WAIT      = 10,  ///< wait for B response
        CL_WR_REQ_WAIT   = 11,  ///< wait for cluster write ready
        CL_WR_RESP_WAIT  = 12,  ///< wait for cluster write response
        // Terminal
        DONE          = 13,
        ERROR         = 14,
    };

    // ========================================================================
    // 4D AGU — internal address generator (inline logic)
    // ========================================================================

    struct Agu4D {
        uint32_t base_addr;
        uint32_t count[4];    ///< normalized counts (0→1)
        uint32_t stride[4];
        uint32_t idx[4];      ///< current loop indices
        uint32_t current_addr;
        bool     done;

        void reset(uint32_t base, const uint32_t cnt[4], const uint32_t str[4]) {
            base_addr = base;
            for (int i = 0; i < 4; ++i) {
                count[i]  = (cnt[i] == 0) ? 1 : cnt[i];
                stride[i] = str[i];
                idx[i]    = 0;
            }
            current_addr = base;
            done = false;
        }

        uint32_t compute_addr() const {
            uint32_t addr = base_addr;
            for (int i = 0; i < 4; ++i)
                addr += idx[i] * stride[i];
            return addr;
        }

        /// Advance to next beat. Returns true if more beats remain.
        bool advance() {
            if (done) return false;
            // Innermost d0 first
            for (int d = 0; d < 4; ++d) {
                idx[d]++;
                if (idx[d] < count[d]) {
                    current_addr = compute_addr();
                    return true;
                }
                idx[d] = 0;
            }
            // All dimensions exhausted
            done = true;
            return false;
        }
    };

    // ========================================================================
    // Transfer FIFO: 256-beat deep, cluster-width data
    // ========================================================================

    static constexpr unsigned kXferFifoDepth = 256;

    FIFO<sc_biguint<kClAxiDataWidth>> u_xfer_fifo;

    sc_signal<sc_biguint<kClAxiDataWidth>> sig_fifo_data_in;
    sc_signal<bool> sig_fifo_push;
    sc_signal<sc_biguint<kClAxiDataWidth>> sig_fifo_data_out;
    sc_signal<bool> sig_fifo_pop;
    sc_signal<bool> sig_fifo_empty;
    sc_signal<bool> sig_fifo_full;
    sc_signal<bool> sig_fifo_clear;

    // ========================================================================
    // Staging registers
    // ========================================================================

    uint32_t stg_src_kind_     = 0;
    uint32_t stg_dst_kind_     = 0;
    uint32_t stg_src_addr_lo_  = 0;
    uint32_t stg_src_addr_hi_  = 0;
    uint32_t stg_dst_addr_lo_  = 0;
    uint32_t stg_dst_addr_hi_  = 0;
    uint32_t stg_src_cluster_  = 0;
    uint32_t stg_dst_cluster_  = 0;
    uint32_t stg_count_[4]     = {};
    uint32_t stg_src_stride_[4]= {};
    uint32_t stg_dst_stride_[4]= {};
    uint32_t stg_cmd_tag_      = 0;

    // ========================================================================
    // Status registers
    // ========================================================================

    uint32_t status_reg_   = 0x01; ///< bit0=idle
    uint32_t ctrl_reg_     = 0;
    uint32_t done_tag_     = 0;
    uint32_t err_code_     = 0;
    uint32_t err_info_     = 0;
    uint32_t debug_state_  = 0;

    // ========================================================================
    // Command FIFO
    // ========================================================================

    std::queue<DmaCommand> cmd_fifo_;

    // ========================================================================
    // AXI beat buffer
    // ========================================================================

    static constexpr unsigned kBeatBytes = kMemAxiDataWidth / 8;
    static constexpr unsigned kClBeatBytes = kClAxiDataWidth / 8;

    // ========================================================================
    // Helpers
    // ========================================================================

    void reset_axi_outputs() {
        m_mem_axi_aw_valid_o.write(false);
        m_mem_axi_aw_addr_o.write(0);
        m_mem_axi_aw_len_o.write(0);
        m_mem_axi_w_valid_o.write(false);
        m_mem_axi_w_data_o.write(0);
        m_mem_axi_w_strb_o.write(0);
        m_mem_axi_w_last_o.write(false);
        m_mem_axi_b_ready_o.write(false);
        m_mem_axi_ar_valid_o.write(false);
        m_mem_axi_ar_addr_o.write(0);
        m_mem_axi_ar_len_o.write(0);
        m_mem_axi_r_ready_o.write(false);
    }

    void reset_cl_outputs() {
        cl_data_req_valid_o.write(false);
        cl_data_req_write_o.write(false);
        cl_data_req_cluster_id_o.write(0);
        cl_data_req_addr_o.write(0);
        cl_data_req_wdata_o.write(0);
        cl_data_req_wstrb_o.write(0);
    }

    uint32_t make_status(DmaState st, bool fifo_full) const {
        uint32_t s = 0;
        if (st == DmaState::IDLE && cmd_fifo_.empty()) s |= 0x01; // idle
        if (st != DmaState::IDLE || !cmd_fifo_.empty()) s |= 0x02; // busy
        if (fifo_full) s |= 0x04;
        if (done_tag_ != 0) s |= 0x08; // done_pending (simplified)
        if (err_code_ != 0) s |= 0x10; // err_pending
        return s;
    }

    uint32_t validate_cmd(const DmaCommand& cmd) const {
        if (static_cast<uint32_t>(cmd.src_kind) > 1 ||
            static_cast<uint32_t>(cmd.dst_kind) > 1)
            return static_cast<uint32_t>(DmaError::DMA_ERR_BAD_ENDPOINT);
        if (cmd.total_beats() == 0)
            return static_cast<uint32_t>(DmaError::DMA_ERR_ZERO_LENGTH);
        if ((cmd.src_addr_lo & 0x3) || (cmd.dst_addr_lo & 0x3))
            return static_cast<uint32_t>(DmaError::DMA_ERR_ADDR_ALIGN);
        return static_cast<uint32_t>(DmaError::DMA_ERR_NONE);
    }

    /// Narrow DRAM-width data to cluster-width (low bytes)
    static sc_biguint<kClAxiDataWidth> dram_to_cl(const sc_biguint<kMemAxiDataWidth>& d) {
        sc_biguint<kClAxiDataWidth> cl = 0;
        for (unsigned b = 0; b < kClBeatBytes; ++b)
            cl.range(b*8+7, b*8) = d.range(b*8+7, b*8);
        return cl;
    }

    /// Widen cluster-width data to DRAM-width (zero-extended)
    static sc_biguint<kMemAxiDataWidth> cl_to_dram(const sc_biguint<kClAxiDataWidth>& c) {
        sc_biguint<kMemAxiDataWidth> d = 0;
        for (unsigned b = 0; b < kClBeatBytes; ++b)
            d.range(b*8+7, b*8) = c.range(b*8+7, b*8);
        return d;
    }

    // ========================================================================
    // Main sequential process
    // ========================================================================

    void seq_process() {
        // ---- Reset ----
        mmio_resp_valid_o.write(false);
        mmio_resp_rdata_o.write(0);
        dma_irq_o.write(false);
        reset_axi_outputs();
        reset_cl_outputs();
        sig_fifo_push.write(false);
        sig_fifo_pop.write(false);
        sig_fifo_clear.write(true);
        sig_fifo_data_in.write(0);
        status_reg_ = 0x01;
        ctrl_reg_ = 0;
        err_code_ = 0;
        err_info_ = 0;
        done_tag_ = 0;
        debug_state_ = 0;
        stg_src_kind_ = 0; stg_dst_kind_ = 0;
        stg_src_addr_lo_ = 0; stg_src_addr_hi_ = 0;
        stg_dst_addr_lo_ = 0; stg_dst_addr_hi_ = 0;
        stg_src_cluster_ = 0; stg_dst_cluster_ = 0;
        for (int i = 0; i < 4; ++i) {
            stg_count_[i] = 1;
            stg_src_stride_[i] = 0;
            stg_dst_stride_[i] = 0;
        }
        stg_cmd_tag_ = 0;
        while (!cmd_fifo_.empty()) cmd_fifo_.pop();
        wait();

        // Release FIFO clear after first cycle
        sig_fifo_clear.write(false);
        wait();

        DmaState fsm_state = DmaState::IDLE;
        DmaCommand active_cmd{};
        Agu4D src_agu{};
        Agu4D dst_agu{};
        uint32_t beats_read = 0;
        uint32_t beats_written = 0;
        uint32_t total_beats = 0;
        bool irq_en = false;

        // Buffered DRAM write data (survive across reset_axi_outputs)
        sc_biguint<kMemAxiDataWidth> dram_wr_data = 0;
        uint32_t dram_wr_addr = 0;
        uint32_t dram_wr_strb = 0;

        while (true) {
            mmio_resp_valid_o.write(false);
            reset_axi_outputs();
            reset_cl_outputs();
            sig_fifo_push.write(false);
            sig_fifo_pop.write(false);
            sig_fifo_clear.write(false);

            // ================================================================
            // MMIO register access
            // ================================================================

            if (mmio_req_valid_i.read()) {
                const uint32_t off   = mmio_req_addr_i.read().to_uint();
                const uint32_t wdata = mmio_req_wdata_i.read().to_uint();
                const bool     wr    = mmio_req_write_i.read();
                uint32_t rdata = 0;

                bool do_submit = false;
                bool do_abort  = false;

                if (!wr) {
                    // ---------- READ ----------
                    switch (off) {
                        case kDmaCap0:         rdata = 0x04; break; // 4D copy
                        case kDmaStatus:       rdata = make_status(fsm_state, cmd_fifo_.size() >= kDmaCmdFifoDepth); break;
                        case kDmaCtrl:         rdata = ctrl_reg_; break;
                        case kDmaSrcKind:      rdata = stg_src_kind_; break;
                        case kDmaDstKind:      rdata = stg_dst_kind_; break;
                        case kDmaSrcAddrLo:    rdata = stg_src_addr_lo_; break;
                        case kDmaSrcAddrHi:    rdata = stg_src_addr_hi_; break;
                        case kDmaDstAddrLo:    rdata = stg_dst_addr_lo_; break;
                        case kDmaDstAddrHi:    rdata = stg_dst_addr_hi_; break;
                        case kDmaSrcClusterId: rdata = stg_src_cluster_; break;
                        case kDmaDstClusterId: rdata = stg_dst_cluster_; break;
                        case kDmaCountD0:      rdata = stg_count_[0]; break;
                        case kDmaCountD1:      rdata = stg_count_[1]; break;
                        case kDmaCountD2:      rdata = stg_count_[2]; break;
                        case kDmaCountD3:      rdata = stg_count_[3]; break;
                        case kDmaSrcStrideD0:  rdata = stg_src_stride_[0]; break;
                        case kDmaSrcStrideD1:  rdata = stg_src_stride_[1]; break;
                        case kDmaSrcStrideD2:  rdata = stg_src_stride_[2]; break;
                        case kDmaSrcStrideD3:  rdata = stg_src_stride_[3]; break;
                        case kDmaDstStrideD0:  rdata = stg_dst_stride_[0]; break;
                        case kDmaDstStrideD1:  rdata = stg_dst_stride_[1]; break;
                        case kDmaDstStrideD2:  rdata = stg_dst_stride_[2]; break;
                        case kDmaDstStrideD3:  rdata = stg_dst_stride_[3]; break;
                        case kDmaCmdTag:       rdata = stg_cmd_tag_; break;
                        case kDmaDoneTag:      rdata = done_tag_; break;
                        case kDmaErrCode:      rdata = err_code_; break;
                        case kDmaErrInfo:      rdata = err_info_; break;
                        case kDmaDebugState:   rdata = static_cast<uint32_t>(fsm_state); break;
                        default: break;
                    }
                } else {
                    // ---------- WRITE ----------
                    switch (off) {
                        case kDmaCtrl: {
                            ctrl_reg_ = wdata;
                            if (wdata & 0x01) do_submit = true;
                            if (wdata & 0x02) do_abort  = true;
                            irq_en = (wdata >> 3) & 1;
                            break;
                        }
                        case kDmaSrcKind:      stg_src_kind_       = wdata; break;
                        case kDmaDstKind:      stg_dst_kind_       = wdata; break;
                        case kDmaSrcAddrLo:    stg_src_addr_lo_    = wdata; break;
                        case kDmaSrcAddrHi:    stg_src_addr_hi_    = wdata; break;
                        case kDmaDstAddrLo:    stg_dst_addr_lo_    = wdata; break;
                        case kDmaDstAddrHi:    stg_dst_addr_hi_    = wdata; break;
                        case kDmaSrcClusterId: stg_src_cluster_    = wdata; break;
                        case kDmaDstClusterId: stg_dst_cluster_    = wdata; break;
                        case kDmaCountD0:      stg_count_[0]       = wdata; break;
                        case kDmaCountD1:      stg_count_[1]       = wdata; break;
                        case kDmaCountD2:      stg_count_[2]       = wdata; break;
                        case kDmaCountD3:      stg_count_[3]       = wdata; break;
                        case kDmaSrcStrideD0:  stg_src_stride_[0]  = wdata; break;
                        case kDmaSrcStrideD1:  stg_src_stride_[1]  = wdata; break;
                        case kDmaSrcStrideD2:  stg_src_stride_[2]  = wdata; break;
                        case kDmaSrcStrideD3:  stg_src_stride_[3]  = wdata; break;
                        case kDmaDstStrideD0:  stg_dst_stride_[0]  = wdata; break;
                        case kDmaDstStrideD1:  stg_dst_stride_[1]  = wdata; break;
                        case kDmaDstStrideD2:  stg_dst_stride_[2]  = wdata; break;
                        case kDmaDstStrideD3:  stg_dst_stride_[3]  = wdata; break;
                        case kDmaCmdTag:       stg_cmd_tag_        = wdata; break;
                        case kDmaErrCode:
                            err_code_ &= ~wdata; // W1C
                            break;
                        default: break;
                    }
                }

                // Submit: atomic snapshot to FIFO
                if (do_submit) {
                    if (cmd_fifo_.size() >= kDmaCmdFifoDepth) {
                        err_code_ = static_cast<uint32_t>(DmaError::DMA_ERR_SUBMIT_WHEN_FULL);
                        err_info_ = stg_cmd_tag_;
                        if (irq_en) dma_irq_o.write(true);
                    } else {
                        DmaCommand cmd;
                        cmd.src_kind      = static_cast<DmaEndpoint>(stg_src_kind_);
                        cmd.dst_kind      = static_cast<DmaEndpoint>(stg_dst_kind_);
                        cmd.src_addr_lo   = stg_src_addr_lo_;
                        cmd.src_addr_hi   = stg_src_addr_hi_;
                        cmd.dst_addr_lo   = stg_dst_addr_lo_;
                        cmd.dst_addr_hi   = stg_dst_addr_hi_;
                        cmd.src_cluster_id= stg_src_cluster_;
                        cmd.dst_cluster_id= stg_dst_cluster_;
                        for (int i = 0; i < 4; ++i) {
                            cmd.count[i]      = stg_count_[i];
                            cmd.src_stride[i] = stg_src_stride_[i];
                            cmd.dst_stride[i] = stg_dst_stride_[i];
                        }
                        cmd.cmd_tag       = stg_cmd_tag_;
                        cmd_fifo_.push(cmd);
                        DEBUG_MSG("DmaEngine: submit tag=" << cmd.cmd_tag
                                  << " beats=" << cmd.total_beats(),
                                  DEBUG_LEVEL_CLUSTER_COMPONENTS);
                    }
                }

                if (do_abort && fsm_state != DmaState::IDLE) {
                    fsm_state = DmaState::ERROR;
                    err_code_ = static_cast<uint32_t>(DmaError::DMA_ERR_ABORTED);
                }

                mmio_resp_valid_o.write(true);
                mmio_resp_rdata_o.write(rdata);
            }

            // ================================================================
            // FSM
            // ================================================================

            switch (fsm_state) {
            // ----------------------------------------------------------------
            case DmaState::IDLE: {
                dma_irq_o.write(false);
                if (!cmd_fifo_.empty()) {
                    active_cmd = cmd_fifo_.front();
                    cmd_fifo_.pop();
                    fsm_state = DmaState::VALIDATE;
                }
                break;
            }
            // ----------------------------------------------------------------
            case DmaState::VALIDATE: {
                uint32_t ve = validate_cmd(active_cmd);
                if (ve != 0) {
                    err_code_ = ve;
                    err_info_ = active_cmd.cmd_tag;
                    fsm_state = DmaState::ERROR;
                } else {
                    // Initialize dual AGUs
                    src_agu.reset(active_cmd.src_addr_lo,
                                  active_cmd.count, active_cmd.src_stride);
                    dst_agu.reset(active_cmd.dst_addr_lo,
                                  active_cmd.count, active_cmd.dst_stride);
                    total_beats = active_cmd.total_beats();
                    beats_read = 0;
                    beats_written = 0;

                    // Clear transfer FIFO
                    sig_fifo_clear.write(true);

                    // Start reading from source
                    fsm_state = DmaState::SRC_READ_ISSUE;
                }
                break;
            }
            // ================================================================
            // Source read phase: read one beat, push into FIFO
            // ================================================================
            case DmaState::SRC_READ_ISSUE: {
                if (beats_read >= total_beats) {
                    // All source beats read, switch to drain mode
                    fsm_state = DmaState::DST_WRITE_ISSUE;
                } else if (sig_fifo_full.read()) {
                    // FIFO full: drain destination first
                    fsm_state = DmaState::DST_WRITE_ISSUE;
                } else {
                    uint32_t addr = src_agu.current_addr;
                    if (active_cmd.src_kind == DmaEndpoint::DRAM) {
                        m_mem_axi_ar_valid_o.write(true);
                        m_mem_axi_ar_addr_o.write(addr);
                        m_mem_axi_ar_len_o.write(0); // single beat
                        m_mem_axi_r_ready_o.write(true);
                        fsm_state = DmaState::DRAM_AR_WAIT;
                    } else {
                        // Cluster SPM read
                        cl_data_req_valid_o.write(true);
                        cl_data_req_write_o.write(false);
                        cl_data_req_cluster_id_o.write(active_cmd.src_cluster_id);
                        cl_data_req_addr_o.write(addr);
                        fsm_state = DmaState::CL_RD_REQ_WAIT;
                    }
                }
                break;
            }
            case DmaState::DRAM_AR_WAIT: {
                m_mem_axi_ar_valid_o.write(true);
                m_mem_axi_ar_addr_o.write(src_agu.current_addr);
                m_mem_axi_ar_len_o.write(0);
                m_mem_axi_r_ready_o.write(true);
                if (m_mem_axi_ar_ready_i.read()) {
                    fsm_state = DmaState::DRAM_R_WAIT;
                }
                break;
            }
            case DmaState::DRAM_R_WAIT: {
                m_mem_axi_r_ready_o.write(true);
                if (m_mem_axi_r_valid_i.read()) {
                    if (m_mem_axi_r_resp_i.read().to_uint() != 0) {
                        err_code_ = static_cast<uint32_t>(DmaError::DMA_ERR_DRAM_AXI);
                        err_info_ = src_agu.current_addr;
                        fsm_state = DmaState::ERROR;
                    } else {
                        // Push data into transfer FIFO
                        sc_biguint<kClAxiDataWidth> cl_data = dram_to_cl(m_mem_axi_r_data_i.read());
                        sig_fifo_data_in.write(cl_data);
                        sig_fifo_push.write(true);
                        src_agu.advance();
                        beats_read++;
                        // Continue reading or switch to write
                        if (beats_read >= total_beats || sig_fifo_full.read()) {
                            fsm_state = DmaState::DST_WRITE_ISSUE;
                        } else {
                            fsm_state = DmaState::SRC_READ_ISSUE;
                        }
                    }
                }
                break;
            }
            case DmaState::CL_RD_REQ_WAIT: {
                if (cl_data_req_ready_i.read()) {
                    fsm_state = DmaState::CL_RD_RESP_WAIT;
                }
                break;
            }
            case DmaState::CL_RD_RESP_WAIT: {
                if (cl_data_resp_valid_i.read()) {
                    if (cl_data_resp_error_i.read()) {
                        err_code_ = static_cast<uint32_t>(DmaError::DMA_ERR_CLUSTER_RESP);
                        err_info_ = src_agu.current_addr;
                        fsm_state = DmaState::ERROR;
                    } else {
                        // Push cluster data into FIFO
                        sig_fifo_data_in.write(cl_data_resp_rdata_i.read());
                        sig_fifo_push.write(true);
                        src_agu.advance();
                        beats_read++;
                        if (beats_read >= total_beats || sig_fifo_full.read()) {
                            fsm_state = DmaState::DST_WRITE_ISSUE;
                        } else {
                            fsm_state = DmaState::SRC_READ_ISSUE;
                        }
                    }
                }
                break;
            }
            // ================================================================
            // Destination write phase: pop FIFO, write one beat
            // ================================================================
            case DmaState::DST_WRITE_ISSUE: {
                if (beats_written >= total_beats) {
                    fsm_state = DmaState::DONE;
                } else if (sig_fifo_empty.read()) {
                    // FIFO empty: go back to reading
                    if (beats_read < total_beats) {
                        fsm_state = DmaState::SRC_READ_ISSUE;
                    }
                    // else: wait for FIFO data (should not happen in correct flow)
                } else {
                    // Pop from FIFO
                    sc_biguint<kClAxiDataWidth> data = sig_fifo_data_out.read();
                    sig_fifo_pop.write(true);

                    uint32_t addr = dst_agu.current_addr;
                    if (active_cmd.dst_kind == DmaEndpoint::DRAM) {
                        dram_wr_addr = addr;
                        dram_wr_data = cl_to_dram(data);
                        dram_wr_strb = (1u << kBeatBytes) - 1;
                        m_mem_axi_aw_valid_o.write(true);
                        m_mem_axi_aw_addr_o.write(dram_wr_addr);
                        m_mem_axi_aw_len_o.write(0);
                        m_mem_axi_w_valid_o.write(true);
                        m_mem_axi_w_data_o.write(dram_wr_data);
                        m_mem_axi_w_strb_o.write(dram_wr_strb);
                        m_mem_axi_w_last_o.write(true);
                        fsm_state = DmaState::DRAM_AW_WAIT;
                    } else {
                        // Cluster SPM write
                        cl_data_req_valid_o.write(true);
                        cl_data_req_write_o.write(true);
                        cl_data_req_cluster_id_o.write(active_cmd.dst_cluster_id);
                        cl_data_req_addr_o.write(addr);
                        cl_data_req_wdata_o.write(data);
                        cl_data_req_wstrb_o.write((1u << kClBeatBytes) - 1);
                        fsm_state = DmaState::CL_WR_REQ_WAIT;
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            // DRAM write path
            // ----------------------------------------------------------------
            case DmaState::DRAM_AW_WAIT: {
                m_mem_axi_aw_valid_o.write(true);
                m_mem_axi_aw_addr_o.write(dram_wr_addr);
                m_mem_axi_aw_len_o.write(0);
                if (m_mem_axi_aw_ready_i.read()) {
                    fsm_state = DmaState::DRAM_W_WAIT;
                }
                break;
            }
            case DmaState::DRAM_W_WAIT: {
                m_mem_axi_w_valid_o.write(true);
                m_mem_axi_w_data_o.write(dram_wr_data);
                m_mem_axi_w_strb_o.write(dram_wr_strb);
                m_mem_axi_w_last_o.write(true);
                if (m_mem_axi_w_ready_i.read()) {
                    m_mem_axi_b_ready_o.write(true);
                    fsm_state = DmaState::DRAM_B_WAIT;
                }
                break;
            }
            case DmaState::DRAM_B_WAIT: {
                m_mem_axi_b_ready_o.write(true);
                if (m_mem_axi_b_valid_i.read()) {
                    if (m_mem_axi_b_resp_i.read().to_uint() != 0) {
                        err_code_ = static_cast<uint32_t>(DmaError::DMA_ERR_DRAM_AXI);
                        err_info_ = dram_wr_addr;
                        fsm_state = DmaState::ERROR;
                    } else {
                        dst_agu.advance();
                        beats_written++;
                        if (beats_written >= total_beats) {
                            fsm_state = DmaState::DONE;
                        } else if (!sig_fifo_empty.read()) {
                            fsm_state = DmaState::DST_WRITE_ISSUE;
                        } else if (beats_read < total_beats) {
                            fsm_state = DmaState::SRC_READ_ISSUE;
                        } else {
                            fsm_state = DmaState::DST_WRITE_ISSUE;
                        }
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            // Cluster write path
            // ----------------------------------------------------------------
            case DmaState::CL_WR_REQ_WAIT: {
                if (cl_data_req_ready_i.read()) {
                    fsm_state = DmaState::CL_WR_RESP_WAIT;
                }
                break;
            }
            case DmaState::CL_WR_RESP_WAIT: {
                if (cl_data_resp_valid_i.read()) {
                    if (cl_data_resp_error_i.read()) {
                        err_code_ = static_cast<uint32_t>(DmaError::DMA_ERR_CLUSTER_RESP);
                        err_info_ = dst_agu.current_addr;
                        fsm_state = DmaState::ERROR;
                    } else {
                        dst_agu.advance();
                        beats_written++;
                        if (beats_written >= total_beats) {
                            fsm_state = DmaState::DONE;
                        } else if (!sig_fifo_empty.read()) {
                            fsm_state = DmaState::DST_WRITE_ISSUE;
                        } else if (beats_read < total_beats) {
                            fsm_state = DmaState::SRC_READ_ISSUE;
                        } else {
                            fsm_state = DmaState::DST_WRITE_ISSUE;
                        }
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            // Terminal states
            // ----------------------------------------------------------------
            case DmaState::DONE: {
                done_tag_ = active_cmd.cmd_tag;
                DEBUG_MSG("DmaEngine: done tag=" << active_cmd.cmd_tag,
                          DEBUG_LEVEL_CLUSTER_COMPONENTS);
                if (irq_en) dma_irq_o.write(true);
                fsm_state = DmaState::IDLE;
                break;
            }
            case DmaState::ERROR: {
                done_tag_ = active_cmd.cmd_tag;
                DEBUG_MSG("DmaEngine: error code=" << err_code_ << " tag=" << active_cmd.cmd_tag,
                          DEBUG_LEVEL_CLUSTER_COMPONENTS);
                if (irq_en) dma_irq_o.write(true);
                // Drain FIFO on fatal
                sig_fifo_clear.write(true);
                while (!cmd_fifo_.empty()) cmd_fifo_.pop();
                fsm_state = DmaState::IDLE;
                break;
            }
            default:
                fsm_state = DmaState::IDLE;
                break;
            } // switch

            debug_state_ = static_cast<uint32_t>(fsm_state);
            status_reg_ = make_status(fsm_state, cmd_fifo_.size() >= kDmaCmdFifoDepth);

            wait();
        } // while
    }
};

} // namespace core
} // namespace hybridacc
