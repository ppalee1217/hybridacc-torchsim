#pragma once

/**
 * @file BootHostIf.hpp
 * @brief cc_boot_host_if — Host-visible AXI4-Lite slave control plane.
 *
 * Provides the host with:
 *   - Capability / status registers
 *   - Boot / halt / resume control
 *   - Manifest base/size/kick for @c cc_section_loader
 *   - Loader status and error reporting
 *   - PC / cause snapshot
 *   - Trace control registers
 *   - Shared cluster mask mirror
 *   - IRQ summary → controller_irq
 *
 * @par Host-visible CSR bank
 *   See Core.md §8.1 for the full offset table (0x0000–0x006C).
 *
 * @par Spec reference
 *   Core.md §8.1  cc_boot_host_if
 */

#include <systemc>
#include <cstdint>
#include "Utils/utils.hpp"
#include "Core/Types.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

template <unsigned NUM_CLUSTERS = 1, unsigned NUM_NLU = 0>
SC_MODULE(BootHostIf) {

    // ========================================================================
    // Host-visible CSR offsets
    // ========================================================================

    static constexpr uint32_t kHaccCap0          = 0x0000;
    static constexpr uint32_t kHaccCap1          = 0x0004;
    static constexpr uint32_t kHaccCtrl          = 0x0008;
    static constexpr uint32_t kHaccStatus        = 0x000C;
    static constexpr uint32_t kCoreBootAddr      = 0x0010;
    static constexpr uint32_t kCoreTrapVector    = 0x0014;
    static constexpr uint32_t kCorePcSnapshot    = 0x0018;
    static constexpr uint32_t kCoreCauseSnapshot = 0x001C;
    static constexpr uint32_t kManifestAddrLo    = 0x0020;
    static constexpr uint32_t kManifestAddrHi    = 0x0024;
    static constexpr uint32_t kManifestSize      = 0x0028;
    static constexpr uint32_t kManifestKick      = 0x002C;
    static constexpr uint32_t kLoaderStatus      = 0x0030;
    static constexpr uint32_t kLoaderErrCode     = 0x0034;
    static constexpr uint32_t kLoaderErrInfo     = 0x0038;
    static constexpr uint32_t kIrqSummary        = 0x0040;
    static constexpr uint32_t kIrqForceAck       = 0x0044;
    static constexpr uint32_t kClusterMaskLo     = 0x0050;
    static constexpr uint32_t kClusterMaskHi     = 0x0054;
    static constexpr uint32_t kLastMmioTarget    = 0x0058;
    static constexpr uint32_t kLastMmioAddr      = 0x005C;
    static constexpr uint32_t kTraceBase         = 0x0060;
    static constexpr uint32_t kTraceSize         = 0x0064;
    static constexpr uint32_t kTraceCtrl         = 0x0068;
    static constexpr uint32_t kTraceStatus       = 0x006C;

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- Host AXI4-Lite slave ---
    sc_in<bool>          s_ctrl_aw_valid_i;
    sc_out<bool>         s_ctrl_aw_ready_o;
    sc_in<sc_uint<32>>   s_ctrl_aw_addr_i;
    sc_in<bool>          s_ctrl_w_valid_i;
    sc_out<bool>         s_ctrl_w_ready_o;
    sc_in<sc_uint<32>>   s_ctrl_w_data_i;
    sc_in<sc_uint<4>>    s_ctrl_w_strb_i;
    sc_out<bool>         s_ctrl_b_valid_o;
    sc_in<bool>          s_ctrl_b_ready_i;
    sc_out<sc_uint<2>>   s_ctrl_b_resp_o;
    sc_in<bool>          s_ctrl_ar_valid_i;
    sc_out<bool>         s_ctrl_ar_ready_o;
    sc_in<sc_uint<32>>   s_ctrl_ar_addr_i;
    sc_out<bool>         s_ctrl_r_valid_o;
    sc_in<bool>          s_ctrl_r_ready_i;
    sc_out<sc_uint<32>>  s_ctrl_r_data_o;
    sc_out<sc_uint<2>>   s_ctrl_r_resp_o;

    // --- Control outputs to core subsystem ---
    sc_out<bool>         core_enable_o;         ///< HACC_CTRL.core_en
    sc_out<bool>         core_haltreq_o;        ///< HACC_CTRL.core_haltreq
    sc_out<sc_uint<32>>  boot_addr_o;           ///< CORE_BOOT_ADDR
    sc_out<bool>         load_phase_o;          ///< loader is active
    sc_out<bool>         loader_kick_o;         ///< W1P pulse to section loader
    sc_out<sc_uint<32>>  manifest_addr_lo_o;
    sc_out<sc_uint<32>>  manifest_addr_hi_o;
    sc_out<sc_uint<32>>  manifest_size_o;
    sc_out<sc_uint<32>>  cluster_mask_lo_o;     ///< shared mirror
    sc_out<sc_uint<32>>  cluster_mask_hi_o;     ///< shared mirror

    // --- Status inputs from subsystems ---
    sc_in<bool>          loader_busy_i;
    sc_in<bool>          loader_done_i;
    sc_in<sc_uint<32>>   loader_status_i;       ///< LOADER_STATUS shadow
    sc_in<sc_uint<32>>   loader_err_code_i;
    sc_in<sc_uint<32>>   loader_err_info_i;
    sc_in<bool>          core_halted_i;
    sc_in<bool>          core_running_i;
    sc_in<sc_uint<32>>   core_pc_snapshot_i;
    sc_in<sc_uint<32>>   core_cause_snapshot_i;
    sc_in<bool>          plic_pending_any_i;
    sc_in<sc_uint<32>>   fabric_last_target_i;
    sc_in<sc_uint<32>>   fabric_last_addr_i;

    // --- IRQ output to host ---
    sc_out<bool>         controller_irq_o;

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(BootHostIf);

    BootHostIf(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          s_ctrl_aw_valid_i("s_ctrl_aw_valid_i"),
          s_ctrl_aw_ready_o("s_ctrl_aw_ready_o"),
          s_ctrl_aw_addr_i("s_ctrl_aw_addr_i"),
          s_ctrl_w_valid_i("s_ctrl_w_valid_i"),
          s_ctrl_w_ready_o("s_ctrl_w_ready_o"),
          s_ctrl_w_data_i("s_ctrl_w_data_i"),
          s_ctrl_w_strb_i("s_ctrl_w_strb_i"),
          s_ctrl_b_valid_o("s_ctrl_b_valid_o"),
          s_ctrl_b_ready_i("s_ctrl_b_ready_i"),
          s_ctrl_b_resp_o("s_ctrl_b_resp_o"),
          s_ctrl_ar_valid_i("s_ctrl_ar_valid_i"),
          s_ctrl_ar_ready_o("s_ctrl_ar_ready_o"),
          s_ctrl_ar_addr_i("s_ctrl_ar_addr_i"),
          s_ctrl_r_valid_o("s_ctrl_r_valid_o"),
          s_ctrl_r_ready_i("s_ctrl_r_ready_i"),
          s_ctrl_r_data_o("s_ctrl_r_data_o"),
          s_ctrl_r_resp_o("s_ctrl_r_resp_o"),
          core_enable_o("core_enable_o"),
          core_haltreq_o("core_haltreq_o"),
          boot_addr_o("boot_addr_o"),
          load_phase_o("load_phase_o"),
          loader_kick_o("loader_kick_o"),
          manifest_addr_lo_o("manifest_addr_lo_o"),
          manifest_addr_hi_o("manifest_addr_hi_o"),
          manifest_size_o("manifest_size_o"),
          cluster_mask_lo_o("cluster_mask_lo_o"),
          cluster_mask_hi_o("cluster_mask_hi_o"),
          loader_busy_i("loader_busy_i"),
          loader_done_i("loader_done_i"),
          loader_status_i("loader_status_i"),
          loader_err_code_i("loader_err_code_i"),
          loader_err_info_i("loader_err_info_i"),
          core_halted_i("core_halted_i"),
          core_running_i("core_running_i"),
          core_pc_snapshot_i("core_pc_snapshot_i"),
          core_cause_snapshot_i("core_cause_snapshot_i"),
          plic_pending_any_i("plic_pending_any_i"),
          fabric_last_target_i("fabric_last_target_i"),
          fabric_last_addr_i("fabric_last_addr_i"),
          controller_irq_o("controller_irq_o")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    // ========================================================================
    // Internal state
    // ========================================================================

    // Writable registers
    uint32_t ctrl_reg_          = 0;
    uint32_t boot_addr_         = 0;
    uint32_t trap_vector_       = 0;
    uint32_t manifest_lo_       = 0;
    uint32_t manifest_hi_       = 0;
    uint32_t manifest_size_     = 0;
    uint32_t cluster_mask_lo_   = 0;
    uint32_t cluster_mask_hi_   = 0;
    uint32_t trace_base_        = 0;
    uint32_t trace_size_        = 0;
    uint32_t trace_ctrl_        = 0;
    uint32_t irq_summary_sticky_= 0;

    // ========================================================================
    // AXI4-Lite FSM
    // ========================================================================

    enum class AxiState : uint32_t {
        IDLE       = 0,
        WR_DATA    = 1,  ///< got AW, waiting for W
        WR_RESP    = 2,  ///< got W, sending B
        RD_RESP    = 3,  ///< got AR, sending R
    };

    // ========================================================================
    // Capability encoding
    // ========================================================================

    static constexpr uint32_t cap0_value() {
        return 0x01 | // RV32I_Zmmul_Zicsr
               0x02 | // DMA
               0x04 | // PLIC
               0x08;  // broadcast
    }
    static constexpr uint32_t cap1_value() {
        return (NUM_CLUSTERS & 0xFF) | ((NUM_NLU & 0xFF) << 8);
    }

    uint32_t hacc_status() const {
        uint32_t s = 0;
        if (loader_busy_i.read()) s |= 0x01;
        if (loader_done_i.read()) s |= 0x02;
        if (core_halted_i.read()) s |= 0x04;
        if (core_running_i.read()) s |= 0x08;
        // bit4=faulted: loader error nonzero
        if (loader_err_code_i.read().to_uint() != 0) s |= 0x10;
        if (loader_busy_i.read()) s |= 0x20; // load_phase
        return s;
    }

    uint32_t irq_summary() const {
        uint32_t s = irq_summary_sticky_;
        if (loader_done_i.read())                  s |= 0x01;
        if (loader_err_code_i.read().to_uint() != 0) s |= 0x02;
        // bit2=core_fault (cause snapshot nonzero can indicate)
        if (core_cause_snapshot_i.read().to_uint() != 0) s |= 0x04;
        if (core_halted_i.read()) s |= 0x08;
        if (plic_pending_any_i.read()) s |= 0x10;
        return s;
    }

    // ========================================================================
    // Main sequential process
    // ========================================================================

    void seq_process() {
        // ---- Reset ----
        s_ctrl_aw_ready_o.write(false);
        s_ctrl_w_ready_o.write(false);
        s_ctrl_b_valid_o.write(false);
        s_ctrl_b_resp_o.write(0);
        s_ctrl_ar_ready_o.write(false);
        s_ctrl_r_valid_o.write(false);
        s_ctrl_r_data_o.write(0);
        s_ctrl_r_resp_o.write(0);
        core_enable_o.write(false);
        core_haltreq_o.write(false);
        boot_addr_o.write(0);
        load_phase_o.write(false);
        loader_kick_o.write(false);
        manifest_addr_lo_o.write(0);
        manifest_addr_hi_o.write(0);
        manifest_size_o.write(0);
        cluster_mask_lo_o.write(0);
        cluster_mask_hi_o.write(0);
        controller_irq_o.write(false);
        ctrl_reg_ = 0; boot_addr_ = 0; trap_vector_ = 0;
        manifest_lo_ = 0; manifest_hi_ = 0; manifest_size_ = 0;
        cluster_mask_lo_ = 0; cluster_mask_hi_ = 0;
        trace_base_ = 0; trace_size_ = 0; trace_ctrl_ = 0;
        irq_summary_sticky_ = 0;
        wait();

        AxiState axi_state = AxiState::IDLE;
        uint32_t wr_addr  = 0;
        uint32_t rd_rdata = 0;

        while (true) {
            // One-pulse signals
            loader_kick_o.write(false);

            // Default deassertion for handshake
            s_ctrl_aw_ready_o.write(false);
            s_ctrl_w_ready_o.write(false);
            s_ctrl_b_valid_o.write(false);
            s_ctrl_ar_ready_o.write(false);
            s_ctrl_r_valid_o.write(false);

            switch (axi_state) {
            // ----------------------------------------------------------------
            case AxiState::IDLE: {
                // Prioritise writes over reads
                if (s_ctrl_aw_valid_i.read()) {
                    wr_addr = s_ctrl_aw_addr_i.read().to_uint();
                    s_ctrl_aw_ready_o.write(true);
                    axi_state = AxiState::WR_DATA;
                } else if (s_ctrl_ar_valid_i.read()) {
                    uint32_t addr = s_ctrl_ar_addr_i.read().to_uint();
                    s_ctrl_ar_ready_o.write(true);
                    rd_rdata = read_csr(addr);
                    axi_state = AxiState::RD_RESP;
                }
                break;
            }
            // ----------------------------------------------------------------
            case AxiState::WR_DATA: {
                s_ctrl_w_ready_o.write(true);
                if (s_ctrl_w_valid_i.read()) {
                    uint32_t wdata = s_ctrl_w_data_i.read().to_uint();
                    write_csr(wr_addr, wdata);
                    axi_state = AxiState::WR_RESP;
                }
                break;
            }
            // ----------------------------------------------------------------
            case AxiState::WR_RESP: {
                s_ctrl_b_valid_o.write(true);
                s_ctrl_b_resp_o.write(0); // OKAY
                if (s_ctrl_b_ready_i.read()) {
                    axi_state = AxiState::IDLE;
                }
                break;
            }
            // ----------------------------------------------------------------
            case AxiState::RD_RESP: {
                s_ctrl_r_valid_o.write(true);
                s_ctrl_r_data_o.write(rd_rdata);
                s_ctrl_r_resp_o.write(0); // OKAY
                if (s_ctrl_r_ready_i.read()) {
                    axi_state = AxiState::IDLE;
                }
                break;
            }
            default:
                axi_state = AxiState::IDLE;
                break;
            }

            // Propagate control outputs
            core_enable_o.write(ctrl_reg_ & 0x01);
            core_haltreq_o.write((ctrl_reg_ >> 1) & 0x01);
            boot_addr_o.write(boot_addr_);
            load_phase_o.write(loader_busy_i.read());
            manifest_addr_lo_o.write(manifest_lo_);
            manifest_addr_hi_o.write(manifest_hi_);
            manifest_size_o.write(manifest_size_);
            cluster_mask_lo_o.write(cluster_mask_lo_);
            cluster_mask_hi_o.write(cluster_mask_hi_);

            // IRQ aggregation
            controller_irq_o.write(irq_summary() != 0);

            wait();
        }
    }

    // ========================================================================
    // CSR read helper
    // ========================================================================

    uint32_t read_csr(uint32_t off) const {
        switch (off) {
            case kHaccCap0:          return cap0_value();
            case kHaccCap1:          return cap1_value();
            case kHaccCtrl:          return ctrl_reg_;
            case kHaccStatus:        return hacc_status();
            case kCoreBootAddr:      return boot_addr_;
            case kCoreTrapVector:    return trap_vector_;
            case kCorePcSnapshot:    return core_pc_snapshot_i.read().to_uint();
            case kCoreCauseSnapshot: return core_cause_snapshot_i.read().to_uint();
            case kManifestAddrLo:    return manifest_lo_;
            case kManifestAddrHi:    return manifest_hi_;
            case kManifestSize:      return manifest_size_;
            case kLoaderStatus:      return loader_status_i.read().to_uint();
            case kLoaderErrCode:     return loader_err_code_i.read().to_uint();
            case kLoaderErrInfo:     return loader_err_info_i.read().to_uint();
            case kIrqSummary:        return irq_summary();
            case kClusterMaskLo:     return cluster_mask_lo_;
            case kClusterMaskHi:     return cluster_mask_hi_;
            case kLastMmioTarget:    return fabric_last_target_i.read().to_uint();
            case kLastMmioAddr:      return fabric_last_addr_i.read().to_uint();
            case kTraceBase:         return trace_base_;
            case kTraceSize:         return trace_size_;
            case kTraceCtrl:         return trace_ctrl_;
            case kTraceStatus:       return 0; // placeholder
            default:                 return 0;
        }
    }

    // ========================================================================
    // CSR write helper
    // ========================================================================

    void write_csr(uint32_t off, uint32_t val) {
        switch (off) {
            case kHaccCtrl: {
                // bit0=core_en, bit1=haltreq are sticky
                ctrl_reg_ = val & 0x03;
                // bit2=core_resume(W1P), bit3=sw_reset(W1P),
                // bit4=loader_abort(W1P), bit5=auto_boot_after_load
                // W1P bits handled as one-shot; only core_en/haltreq persist.
                if (val & 0x20) ctrl_reg_ |= 0x20; // auto_boot sticky
                break;
            }
            case kCoreBootAddr:      boot_addr_     = val; break;
            case kCoreTrapVector:    trap_vector_   = val; break;
            case kManifestAddrLo:    manifest_lo_   = val; break;
            case kManifestAddrHi:    manifest_hi_   = val; break;
            case kManifestSize:      manifest_size_ = val; break;
            case kManifestKick:
                loader_kick_o.write(true);
                DEBUG_MSG("BootHostIf: MANIFEST_KICK", DEBUG_LEVEL_CLUSTER_COMPONENTS);
                break;
            case kLoaderErrCode:
                // W1C — clear bits written as 1
                // Actual clear must feed back to loader; for ESL we track locally
                break;
            case kIrqForceAck:
                irq_summary_sticky_ &= ~val; // W1C
                break;
            case kClusterMaskLo:
                // Only allow when core not running or halted
                if (!core_running_i.read() || core_halted_i.read())
                    cluster_mask_lo_ = val;
                break;
            case kClusterMaskHi:
                if (!core_running_i.read() || core_halted_i.read())
                    cluster_mask_hi_ = val;
                break;
            case kTraceBase:         trace_base_ = val; break;
            case kTraceSize:         trace_size_ = val; break;
            case kTraceCtrl:         trace_ctrl_ = val; break;
            default:
                break;
        }
    }
};

} // namespace core
} // namespace hybridacc
