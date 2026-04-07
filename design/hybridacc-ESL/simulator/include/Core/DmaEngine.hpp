#pragma once

/**
 * @file DmaEngine.hpp
 * @brief cc_dma_engine — DMA engine with MMIO staging registers, command
 *        FIFO, and linear/2D copy execution.
 *
 * Firmware writes staging registers, then commits with @c DMA_CTRL.submit.
 * The engine snapshots the staging set into an internal FIFO and processes
 * commands sequentially, driving DRAM AXI + cluster data fabric as needed.
 *
 * @par MMIO register map (base = 0x2000_1000)
 *   See Core.md §8.8 for the complete 22-register definition.
 *
 * @par Spec reference
 *   Core.md §8.8  cc_dma_engine
 */

#include <systemc>
#include <cstdint>
#include <array>
#include <queue>
#include "Utils/utils.hpp"
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
          dma_irq_o("dma_irq_o")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    // ========================================================================
    // Internal types
    // ========================================================================

    enum class DmaState : uint32_t {
        IDLE          = 0,
        VALIDATE      = 1,
        DRAM_READ     = 2,   ///< read from DRAM, write to cluster SPM
        DRAM_WRITE    = 3,   ///< read from cluster SPM, write to DRAM
        CL_READ       = 4,   ///< cluster SPM read beat
        CL_WRITE      = 5,   ///< cluster SPM write beat
        DRAM_AR_WAIT  = 6,   ///< wait for AR handshake
        DRAM_R_WAIT   = 7,   ///< wait for R channel data
        DRAM_AW_WAIT  = 8,   ///< wait for AW handshake
        DRAM_W_WAIT   = 9,   ///< wait for W handshake
        DRAM_B_WAIT   = 10,  ///< wait for B response
        CL_REQ_WAIT   = 11,  ///< wait for cluster fabric ready
        CL_RESP_WAIT  = 12,  ///< wait for cluster fabric response
        DONE          = 13,
        ERROR         = 14,
    };

    // ========================================================================
    // Staging registers
    // ========================================================================

    uint32_t stg_op_kind_      = 0;
    uint32_t stg_src_kind_     = 0;
    uint32_t stg_dst_kind_     = 0;
    uint32_t stg_src_addr_lo_  = 0;
    uint32_t stg_src_addr_hi_  = 0;
    uint32_t stg_dst_addr_lo_  = 0;
    uint32_t stg_dst_addr_hi_  = 0;
    uint32_t stg_src_cluster_  = 0;
    uint32_t stg_dst_cluster_  = 0;
    uint32_t stg_bytes_        = 0;
    uint32_t stg_line_bytes_   = 0;
    uint32_t stg_line_count_   = 0;
    uint32_t stg_src_stride_   = 0;
    uint32_t stg_dst_stride_   = 0;
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

    /**
     * @brief Validate a DMA command; return DmaError code.
     */
    uint32_t validate_cmd(const DmaCommand& cmd) const {
        if (static_cast<uint32_t>(cmd.op_kind) > 1)
            return static_cast<uint32_t>(DmaError::DMA_ERR_BAD_OP_KIND);
        if (static_cast<uint32_t>(cmd.src_kind) > 1 ||
            static_cast<uint32_t>(cmd.dst_kind) > 1)
            return static_cast<uint32_t>(DmaError::DMA_ERR_BAD_ENDPOINT);

        uint32_t total = (cmd.op_kind == DmaOpKind::LINEAR_COPY)
                             ? cmd.bytes
                             : cmd.line_bytes * cmd.line_count;
        if (total == 0)
            return static_cast<uint32_t>(DmaError::DMA_ERR_ZERO_LENGTH);
        // Simplified alignment check: low 2 bits must be 0
        if ((cmd.src_addr_lo & 0x3) || (cmd.dst_addr_lo & 0x3))
            return static_cast<uint32_t>(DmaError::DMA_ERR_ADDR_ALIGN);

        return static_cast<uint32_t>(DmaError::DMA_ERR_NONE);
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
        status_reg_ = 0x01;
        ctrl_reg_ = 0;
        err_code_ = 0;
        err_info_ = 0;
        done_tag_ = 0;
        debug_state_ = 0;
        stg_op_kind_ = 0; stg_src_kind_ = 0; stg_dst_kind_ = 0;
        stg_src_addr_lo_ = 0; stg_src_addr_hi_ = 0;
        stg_dst_addr_lo_ = 0; stg_dst_addr_hi_ = 0;
        stg_src_cluster_ = 0; stg_dst_cluster_ = 0;
        stg_bytes_ = 0; stg_line_bytes_ = 0; stg_line_count_ = 0;
        stg_src_stride_ = 0; stg_dst_stride_ = 0; stg_cmd_tag_ = 0;
        while (!cmd_fifo_.empty()) cmd_fifo_.pop();
        wait();

        DmaState fsm_state = DmaState::IDLE;
        DmaCommand active_cmd{};
        uint32_t bytes_remaining = 0;
        uint32_t src_addr = 0;
        uint32_t dst_addr = 0;
        uint32_t line_idx = 0;
        uint32_t line_byte_remaining = 0;
        bool     irq_en = false;

        while (true) {
            mmio_resp_valid_o.write(false);
            reset_axi_outputs();
            reset_cl_outputs();

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
                        case kDmaCap0:      rdata = 0x03; break; // linear + 2D
                        case kDmaStatus:    rdata = make_status(fsm_state, cmd_fifo_.size() >= kDmaCmdFifoDepth); break;
                        case kDmaCtrl:      rdata = ctrl_reg_; break;
                        case kDmaOpKind:    rdata = stg_op_kind_; break;
                        case kDmaSrcKind:   rdata = stg_src_kind_; break;
                        case kDmaDstKind:   rdata = stg_dst_kind_; break;
                        case kDmaSrcAddrLo: rdata = stg_src_addr_lo_; break;
                        case kDmaSrcAddrHi: rdata = stg_src_addr_hi_; break;
                        case kDmaDstAddrLo: rdata = stg_dst_addr_lo_; break;
                        case kDmaDstAddrHi: rdata = stg_dst_addr_hi_; break;
                        case kDmaSrcClusterId: rdata = stg_src_cluster_; break;
                        case kDmaDstClusterId: rdata = stg_dst_cluster_; break;
                        case kDmaBytes:     rdata = stg_bytes_; break;
                        case kDmaLineBytes: rdata = stg_line_bytes_; break;
                        case kDmaLineCount: rdata = stg_line_count_; break;
                        case kDmaSrcStride: rdata = stg_src_stride_; break;
                        case kDmaDstStride: rdata = stg_dst_stride_; break;
                        case kDmaCmdTag:    rdata = stg_cmd_tag_; break;
                        case kDmaDoneTag:   rdata = done_tag_; break;
                        case kDmaErrCode:   rdata = err_code_; break;
                        case kDmaErrInfo:   rdata = err_info_; break;
                        case kDmaDebugState:rdata = static_cast<uint32_t>(fsm_state); break;
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
                        case kDmaOpKind:    stg_op_kind_     = wdata; break;
                        case kDmaSrcKind:   stg_src_kind_    = wdata; break;
                        case kDmaDstKind:   stg_dst_kind_    = wdata; break;
                        case kDmaSrcAddrLo: stg_src_addr_lo_ = wdata; break;
                        case kDmaSrcAddrHi: stg_src_addr_hi_ = wdata; break;
                        case kDmaDstAddrLo: stg_dst_addr_lo_ = wdata; break;
                        case kDmaDstAddrHi: stg_dst_addr_hi_ = wdata; break;
                        case kDmaSrcClusterId: stg_src_cluster_ = wdata; break;
                        case kDmaDstClusterId: stg_dst_cluster_ = wdata; break;
                        case kDmaBytes:     stg_bytes_        = wdata; break;
                        case kDmaLineBytes: stg_line_bytes_   = wdata; break;
                        case kDmaLineCount: stg_line_count_   = wdata; break;
                        case kDmaSrcStride: stg_src_stride_   = wdata; break;
                        case kDmaDstStride: stg_dst_stride_   = wdata; break;
                        case kDmaCmdTag:    stg_cmd_tag_      = wdata; break;
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
                        cmd.op_kind       = static_cast<DmaOpKind>(stg_op_kind_);
                        cmd.src_kind      = static_cast<DmaEndpoint>(stg_src_kind_);
                        cmd.dst_kind      = static_cast<DmaEndpoint>(stg_dst_kind_);
                        cmd.src_addr_lo   = stg_src_addr_lo_;
                        cmd.src_addr_hi   = stg_src_addr_hi_;
                        cmd.dst_addr_lo   = stg_dst_addr_lo_;
                        cmd.dst_addr_hi   = stg_dst_addr_hi_;
                        cmd.src_cluster_id= stg_src_cluster_;
                        cmd.dst_cluster_id= stg_dst_cluster_;
                        cmd.bytes         = stg_bytes_;
                        cmd.line_bytes    = stg_line_bytes_;
                        cmd.line_count    = stg_line_count_;
                        cmd.src_stride    = stg_src_stride_;
                        cmd.dst_stride    = stg_dst_stride_;
                        cmd.cmd_tag       = stg_cmd_tag_;
                        cmd_fifo_.push(cmd);
                        DEBUG_MSG("DmaEngine: submit tag=" << cmd.cmd_tag,
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
                    // Set up transfer pointers
                    src_addr = active_cmd.src_addr_lo;
                    dst_addr = active_cmd.dst_addr_lo;
                    if (active_cmd.op_kind == DmaOpKind::LINEAR_COPY) {
                        bytes_remaining = active_cmd.bytes;
                        line_idx = 0;
                        line_byte_remaining = active_cmd.bytes;
                    } else {
                        bytes_remaining = active_cmd.line_bytes * active_cmd.line_count;
                        line_idx = 0;
                        line_byte_remaining = active_cmd.line_bytes;
                    }

                    // Determine transfer direction
                    if (active_cmd.src_kind == DmaEndpoint::DRAM &&
                        active_cmd.dst_kind == DmaEndpoint::CLUSTER_SPM) {
                        fsm_state = DmaState::DRAM_READ;
                    } else if (active_cmd.src_kind == DmaEndpoint::CLUSTER_SPM &&
                               active_cmd.dst_kind == DmaEndpoint::DRAM) {
                        fsm_state = DmaState::CL_READ;
                    } else if (active_cmd.src_kind == DmaEndpoint::DRAM &&
                               active_cmd.dst_kind == DmaEndpoint::DRAM) {
                        fsm_state = DmaState::DRAM_READ;
                    } else {
                        // CLUSTER_SPM → CLUSTER_SPM: read from src cluster
                        fsm_state = DmaState::CL_READ;
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            // DRAM → Cluster SPM  (or DRAM → DRAM read phase)
            // ----------------------------------------------------------------
            case DmaState::DRAM_READ: {
                // Issue AR
                m_mem_axi_ar_valid_o.write(true);
                m_mem_axi_ar_addr_o.write(src_addr);
                m_mem_axi_ar_len_o.write(0); // single beat
                m_mem_axi_r_ready_o.write(true);
                fsm_state = DmaState::DRAM_AR_WAIT;
                break;
            }
            case DmaState::DRAM_AR_WAIT: {
                m_mem_axi_ar_valid_o.write(true);
                m_mem_axi_ar_addr_o.write(src_addr);
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
                        err_info_ = src_addr;
                        fsm_state = DmaState::ERROR;
                    } else {
                        // Got data — route to destination
                        if (active_cmd.dst_kind == DmaEndpoint::CLUSTER_SPM) {
                            // Write to cluster SPM via fabric
                            cl_data_req_valid_o.write(true);
                            cl_data_req_write_o.write(true);
                            cl_data_req_cluster_id_o.write(active_cmd.dst_cluster_id);
                            cl_data_req_addr_o.write(dst_addr);
                            // Narrow the DRAM-width data to cluster-width
                            sc_biguint<kMemAxiDataWidth> dram_data = m_mem_axi_r_data_i.read();
                            sc_biguint<kClAxiDataWidth> cl_wd = 0;
                            for (unsigned b = 0; b < kClBeatBytes; ++b) {
                                cl_wd.range(b*8+7, b*8) = dram_data.range(b*8+7, b*8);
                            }
                            cl_data_req_wdata_o.write(cl_wd);
                            cl_data_req_wstrb_o.write((1u << kClBeatBytes) - 1);
                            fsm_state = DmaState::CL_REQ_WAIT;
                        } else {
                            // DRAM → DRAM: write phase
                            // Buffer data for write
                            m_mem_axi_aw_valid_o.write(true);
                            m_mem_axi_aw_addr_o.write(dst_addr);
                            m_mem_axi_aw_len_o.write(0);
                            m_mem_axi_w_valid_o.write(true);
                            m_mem_axi_w_data_o.write(m_mem_axi_r_data_i.read());
                            m_mem_axi_w_strb_o.write((1u << kBeatBytes) - 1);
                            m_mem_axi_w_last_o.write(true);
                            fsm_state = DmaState::DRAM_AW_WAIT;
                        }
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            // Cluster SPM read → DRAM write  (or cluster → cluster)
            // ----------------------------------------------------------------
            case DmaState::CL_READ: {
                cl_data_req_valid_o.write(true);
                cl_data_req_write_o.write(false);
                cl_data_req_cluster_id_o.write(active_cmd.src_cluster_id);
                cl_data_req_addr_o.write(src_addr);
                cl_data_req_wdata_o.write(0);
                cl_data_req_wstrb_o.write(0);
                fsm_state = DmaState::CL_REQ_WAIT;
                break;
            }
            // ----------------------------------------------------------------
            // Cluster fabric handshake
            // ----------------------------------------------------------------
            case DmaState::CL_REQ_WAIT: {
                // Maintain request outputs until ready
                if (cl_data_req_ready_i.read()) {
                    fsm_state = DmaState::CL_RESP_WAIT;
                }
                break;
            }
            case DmaState::CL_RESP_WAIT: {
                if (cl_data_resp_valid_i.read()) {
                    if (cl_data_resp_error_i.read()) {
                        err_code_ = static_cast<uint32_t>(DmaError::DMA_ERR_CLUSTER_RESP);
                        err_info_ = src_addr;
                        fsm_state = DmaState::ERROR;
                    } else {
                        // Advance pointers
                        uint32_t beat = kClBeatBytes;
                        if (beat > bytes_remaining) beat = bytes_remaining;
                        src_addr += beat;
                        dst_addr += beat;
                        bytes_remaining -= beat;
                        line_byte_remaining -= beat;

                        // 2D stride handling
                        if (active_cmd.op_kind == DmaOpKind::STRIDED_2D &&
                            line_byte_remaining == 0 && bytes_remaining > 0) {
                            ++line_idx;
                            src_addr = active_cmd.src_addr_lo + line_idx * active_cmd.src_stride;
                            dst_addr = active_cmd.dst_addr_lo + line_idx * active_cmd.dst_stride;
                            line_byte_remaining = active_cmd.line_bytes;
                        }

                        if (bytes_remaining == 0) {
                            fsm_state = DmaState::DONE;
                        } else {
                            // Continue: determine next read source
                            if (active_cmd.src_kind == DmaEndpoint::DRAM) {
                                fsm_state = DmaState::DRAM_READ;
                            } else if (active_cmd.dst_kind == DmaEndpoint::DRAM) {
                                // cluster → DRAM: now do DRAM write with captured data
                                sc_biguint<kMemAxiDataWidth> wd = 0;
                                sc_biguint<kClAxiDataWidth> cl_rd = cl_data_resp_rdata_i.read();
                                for (unsigned b = 0; b < kClBeatBytes; ++b) {
                                    wd.range(b*8+7, b*8) = cl_rd.range(b*8+7, b*8);
                                }
                                m_mem_axi_aw_valid_o.write(true);
                                m_mem_axi_aw_addr_o.write(dst_addr - beat);
                                m_mem_axi_aw_len_o.write(0);
                                m_mem_axi_w_valid_o.write(true);
                                m_mem_axi_w_data_o.write(wd);
                                m_mem_axi_w_strb_o.write((1u << kBeatBytes) - 1);
                                m_mem_axi_w_last_o.write(true);
                                fsm_state = DmaState::DRAM_AW_WAIT;
                            } else {
                                // cluster → cluster: next cluster read
                                fsm_state = DmaState::CL_READ;
                            }
                        }
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            // DRAM write path
            // ----------------------------------------------------------------
            case DmaState::DRAM_AW_WAIT: {
                m_mem_axi_aw_valid_o.write(true);
                m_mem_axi_aw_addr_o.write(dst_addr);
                m_mem_axi_aw_len_o.write(0);
                if (m_mem_axi_aw_ready_i.read()) {
                    fsm_state = DmaState::DRAM_W_WAIT;
                }
                break;
            }
            case DmaState::DRAM_W_WAIT: {
                m_mem_axi_w_valid_o.write(true);
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
                        err_info_ = dst_addr;
                        fsm_state = DmaState::ERROR;
                    } else {
                        // Advance pointers (if coming from DRAM→DRAM path)
                        uint32_t beat = kBeatBytes;
                        if (active_cmd.dst_kind == DmaEndpoint::DRAM &&
                            active_cmd.src_kind == DmaEndpoint::DRAM) {
                            if (beat > bytes_remaining) beat = bytes_remaining;
                            src_addr += beat;
                            dst_addr += beat;
                            bytes_remaining -= beat;
                            line_byte_remaining -= beat;
                            if (active_cmd.op_kind == DmaOpKind::STRIDED_2D &&
                                line_byte_remaining == 0 && bytes_remaining > 0) {
                                ++line_idx;
                                src_addr = active_cmd.src_addr_lo + line_idx * active_cmd.src_stride;
                                dst_addr = active_cmd.dst_addr_lo + line_idx * active_cmd.dst_stride;
                                line_byte_remaining = active_cmd.line_bytes;
                            }
                        }

                        if (bytes_remaining == 0) {
                            fsm_state = DmaState::DONE;
                        } else {
                            if (active_cmd.src_kind == DmaEndpoint::DRAM) {
                                fsm_state = DmaState::DRAM_READ;
                            } else {
                                fsm_state = DmaState::CL_READ;
                            }
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
