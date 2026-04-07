#pragma once

/**
 * @file CoreLocalIrq.hpp
 * @brief cc_core_local_irq — Machine timer (MTIP) and software (MSIP)
 *        interrupt controller.
 *
 * Provides:
 *   - `MSIP` register  → machine software interrupt pending.
 *   - `mtime` / `mtimecmp` → machine timer interrupt.
 *   - `TIMER_CTRL.timer_en` gating.
 *
 * These interrupts bypass cc_plic and directly drive `mip.MSIP` / `mip.MTIP`.
 *
 * @par MMIO map (base = 0x2000_2000)
 *   | Offset | Name | RW |
 *   |--------|------|----|
 *   | 0x000  | MSIP         | R/W |
 *   | 0x004  | MTIMECMP_LO  | R/W |
 *   | 0x008  | MTIMECMP_HI  | R/W |
 *   | 0x00C  | MTIME_LO     | R/W |
 *   | 0x010  | MTIME_HI     | R/W |
 *   | 0x014  | TIMER_CTRL   | R/W |
 *
 * @par Spec reference
 *   Core.md §8.6  cc_core_local_irq
 */

#include <systemc>
#include <cstdint>
#include "Utils/utils.hpp"
#include "Core/Types.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

SC_MODULE(CoreLocalIrq) {

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- MMIO interface (from cc_cmd_fabric) ---
    sc_in<bool>          mmio_req_valid_i;
    sc_in<bool>          mmio_req_write_i;
    sc_in<sc_uint<32>>   mmio_req_addr_i;   ///< local offset within window
    sc_in<sc_uint<32>>   mmio_req_wdata_i;
    sc_out<bool>         mmio_resp_valid_o;
    sc_out<sc_uint<32>>  mmio_resp_rdata_o;

    // --- IRQ outputs to core ---
    sc_out<bool>  irq_msip_o;   ///< machine software interrupt pending
    sc_out<bool>  irq_mtip_o;   ///< machine timer interrupt pending

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(CoreLocalIrq);

    CoreLocalIrq(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          mmio_req_valid_i("mmio_req_valid_i"),
          mmio_req_write_i("mmio_req_write_i"),
          mmio_req_addr_i("mmio_req_addr_i"),
          mmio_req_wdata_i("mmio_req_wdata_i"),
          mmio_resp_valid_o("mmio_resp_valid_o"),
          mmio_resp_rdata_o("mmio_resp_rdata_o"),
          irq_msip_o("irq_msip_o"),
          irq_mtip_o("irq_mtip_o")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        SC_METHOD(comb_irq_process);
        sensitive << msip_reg << timer_en_reg << mtime_lo_reg << mtime_hi_reg
                  << mtimecmp_lo_reg << mtimecmp_hi_reg;
    }

private:
    // ========================================================================
    // Registers
    // ========================================================================

    sc_signal<bool>         msip_reg;
    sc_signal<sc_uint<32>>  mtimecmp_lo_reg;
    sc_signal<sc_uint<32>>  mtimecmp_hi_reg;
    sc_signal<sc_uint<32>>  mtime_lo_reg;
    sc_signal<sc_uint<32>>  mtime_hi_reg;
    sc_signal<bool>         timer_en_reg;

    // ========================================================================
    // Processes
    // ========================================================================

    /**
     * @brief Sequential — increment mtime; handle MMIO read/write.
     */
    void seq_process() {
        // Reset
        msip_reg.write(false);
        mtimecmp_lo_reg.write(0xFFFFFFFF);
        mtimecmp_hi_reg.write(0xFFFFFFFF);
        mtime_lo_reg.write(0);
        mtime_hi_reg.write(0);
        timer_en_reg.write(false);
        mmio_resp_valid_o.write(false);
        mmio_resp_rdata_o.write(0);
        wait();

        while (true) {
            mmio_resp_valid_o.write(false);

            // Increment mtime if enabled
            if (timer_en_reg.read()) {
                uint32_t lo = mtime_lo_reg.read().to_uint();
                uint32_t hi = mtime_hi_reg.read().to_uint();
                uint64_t t  = (static_cast<uint64_t>(hi) << 32) | lo;
                t++;
                mtime_lo_reg.write(static_cast<uint32_t>(t));
                mtime_hi_reg.write(static_cast<uint32_t>(t >> 32));
            }

            // MMIO access
            if (mmio_req_valid_i.read()) {
                const uint32_t offset = mmio_req_addr_i.read().to_uint();
                const uint32_t wdata  = mmio_req_wdata_i.read().to_uint();
                const bool     wr     = mmio_req_write_i.read();
                uint32_t rdata = 0;

                if (wr) {
                    switch (offset) {
                        case kTimerMsip:       msip_reg.write(wdata & 1); break;
                        case kTimerMtimecmpLo: mtimecmp_lo_reg.write(wdata); break;
                        case kTimerMtimecmpHi: mtimecmp_hi_reg.write(wdata); break;
                        case kTimerMtimeLo:    mtime_lo_reg.write(wdata); break;
                        case kTimerMtimeHi:    mtime_hi_reg.write(wdata); break;
                        case kTimerCtrl:       timer_en_reg.write(wdata & 1); break;
                        default: break;
                    }
                } else {
                    switch (offset) {
                        case kTimerMsip:       rdata = msip_reg.read() ? 1 : 0; break;
                        case kTimerMtimecmpLo: rdata = mtimecmp_lo_reg.read().to_uint(); break;
                        case kTimerMtimecmpHi: rdata = mtimecmp_hi_reg.read().to_uint(); break;
                        case kTimerMtimeLo:    rdata = mtime_lo_reg.read().to_uint(); break;
                        case kTimerMtimeHi:    rdata = mtime_hi_reg.read().to_uint(); break;
                        case kTimerCtrl:       rdata = timer_en_reg.read() ? 1 : 0; break;
                        default: break;
                    }
                }

                mmio_resp_valid_o.write(true);
                mmio_resp_rdata_o.write(rdata);
            }

            wait();
        }
    }

    /**
     * @brief Combinational — generate IRQ outputs.
     *
     * MTIP asserts when timer_en=1 and mtime >= mtimecmp.
     * MSIP directly reflects the msip register bit.
     */
    void comb_irq_process() {
        irq_msip_o.write(msip_reg.read());

        bool mtip = false;
        if (timer_en_reg.read()) {
            const uint64_t mtime = (static_cast<uint64_t>(mtime_hi_reg.read().to_uint()) << 32)
                                 | mtime_lo_reg.read().to_uint();
            const uint64_t cmp   = (static_cast<uint64_t>(mtimecmp_hi_reg.read().to_uint()) << 32)
                                 | mtimecmp_lo_reg.read().to_uint();
            mtip = (mtime >= cmp);
        }
        irq_mtip_o.write(mtip);
    }
};

} // namespace core
} // namespace hybridacc
