#pragma once

/**
 * @file Plic.hpp
 * @brief cc_plic — Compact single-hart Platform-Level Interrupt Controller.
 *
 * External level-triggered IRQ sources are synchronised, arbitrated by
 * priority, and presented to the core as `MEIP` (machine external
 * interrupt pending).  Firmware interacts via claim/complete MMIO.
 *
 * @par Source id assignment
 *   | ID | Source |
 *   |----|--------|
 *   | 0  | reserved (no-pending sentinel) |
 *   | 1..NUM_CLUSTERS | cluster_irq[0..N-1] |
 *   | NUM_CLUSTERS+1  | dma_irq |
 *   | +2..+NLU+1      | nlu_irq[0..M-1] |
 *   | +NLU+2          | loader_fault |
 *   | +NLU+3          | fabric_fault |
 *
 * @par MMIO map (base = 0x0C00_0000)
 *   See Core.md §8.7 for complete register table.
 *
 * @par Spec reference
 *   Core.md §8.7  cc_plic
 */

#include <systemc>
#include <cstdint>
#include <array>
#include "Utils/utils.hpp"
#include "Core/Types.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

template <unsigned NUM_CLUSTERS = 1, unsigned NUM_NLU = 0>
SC_MODULE(Plic) {

    static constexpr unsigned kNumSources = plic_num_sources(NUM_CLUSTERS, NUM_NLU);
    static constexpr unsigned kMaxSources = 64; ///< upper bound for arrays

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- External level IRQ inputs ---
    sc_in<bool>  cluster_irq_i[NUM_CLUSTERS];
    sc_in<bool>  nlu_irq_i[NUM_NLU > 0 ? NUM_NLU : 1];
    sc_in<bool>  dma_irq_i;
    sc_in<bool>  loader_fault_i;
    sc_in<bool>  fabric_fault_i;

    // --- MEIP to core ---
    sc_out<bool> meip_o;

    // --- MMIO interface (from cc_cmd_fabric) ---
    sc_in<bool>          mmio_req_valid_i;
    sc_in<bool>          mmio_req_write_i;
    sc_in<sc_uint<32>>   mmio_req_addr_i;   ///< local offset within PLIC window
    sc_in<sc_uint<32>>   mmio_req_wdata_i;
    sc_out<bool>         mmio_resp_valid_o;
    sc_out<sc_uint<32>>  mmio_resp_rdata_o;

    // --- Pending bitmap export (for host snapshot) ---
    sc_out<sc_uint<32>>  pending_lo_o;
    sc_out<sc_uint<32>>  pending_hi_o;

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(Plic);

    Plic(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          dma_irq_i("dma_irq_i"),
          loader_fault_i("loader_fault_i"),
          fabric_fault_i("fabric_fault_i"),
          meip_o("meip_o"),
          mmio_req_valid_i("mmio_req_valid_i"),
          mmio_req_write_i("mmio_req_write_i"),
          mmio_req_addr_i("mmio_req_addr_i"),
          mmio_req_wdata_i("mmio_req_wdata_i"),
          mmio_resp_valid_o("mmio_resp_valid_o"),
          mmio_resp_rdata_o("mmio_resp_rdata_o"),
          pending_lo_o("pending_lo_o"),
          pending_hi_o("pending_hi_o")
    {
        priority_.fill(1); // default priority = 1
        enable_lo_ = 0;
        enable_hi_ = 0;
        threshold_ = 0;
        pending_lo_ = 0;
        pending_hi_ = 0;
        claimed_mask_lo_ = 0;
        claimed_mask_hi_ = 0;

        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        SC_METHOD(comb_meip);
        sensitive << pending_lo_sig << pending_hi_sig
                  << enable_lo_sig << enable_hi_sig << threshold_sig;
    }

private:
    // ========================================================================
    // State
    // ========================================================================

    std::array<uint32_t, kMaxSources + 1> priority_;
    uint32_t enable_lo_;
    uint32_t enable_hi_;
    uint32_t threshold_;
    uint32_t pending_lo_;
    uint32_t pending_hi_;
    uint32_t claimed_mask_lo_;
    uint32_t claimed_mask_hi_;

    sc_signal<sc_uint<32>> pending_lo_sig;
    sc_signal<sc_uint<32>> pending_hi_sig;
    sc_signal<sc_uint<32>> enable_lo_sig;
    sc_signal<sc_uint<32>> enable_hi_sig;
    sc_signal<sc_uint<32>> threshold_sig;

    // ========================================================================
    // Source-level sampling helpers
    // ========================================================================

    bool sample_source(unsigned id) const {
        if (id == 0) return false;
        // cluster
        if (id >= 1 && id <= NUM_CLUSTERS)
            return cluster_irq_i[id - 1].read();
        // dma
        if (id == NUM_CLUSTERS + 1)
            return dma_irq_i.read();
        // nlu
        if (NUM_NLU > 0 && id >= NUM_CLUSTERS + 2 && id <= NUM_CLUSTERS + NUM_NLU + 1)
            return nlu_irq_i[id - NUM_CLUSTERS - 2].read();
        // loader_fault
        if (id == NUM_CLUSTERS + NUM_NLU + 2)
            return loader_fault_i.read();
        // fabric_fault
        if (id == NUM_CLUSTERS + NUM_NLU + 3)
            return fabric_fault_i.read();
        return false;
    }

    void set_pending(unsigned id) {
        if (id < 32)      pending_lo_ |= (1u << id);
        else if (id < 64) pending_hi_ |= (1u << (id - 32));
    }

    void clear_pending(unsigned id) {
        if (id < 32)      pending_lo_ &= ~(1u << id);
        else if (id < 64) pending_hi_ &= ~(1u << (id - 32));
    }

    bool is_pending(unsigned id) const {
        if (id < 32)      return (pending_lo_ >> id) & 1;
        else if (id < 64) return (pending_hi_ >> (id - 32)) & 1;
        return false;
    }

    bool is_enabled(unsigned id) const {
        if (id < 32)      return (enable_lo_ >> id) & 1;
        else if (id < 64) return (enable_hi_ >> (id - 32)) & 1;
        return false;
    }

    // ========================================================================
    // Claim: find highest-priority enabled pending source
    // ========================================================================

    unsigned claim() const {
        unsigned best_id  = 0;
        uint32_t best_pri = 0;
        for (unsigned s = 1; s <= kNumSources; ++s) {
            if (is_pending(s) && is_enabled(s) && priority_[s] > threshold_) {
                if (priority_[s] > best_pri ||
                    (priority_[s] == best_pri && (best_id == 0 || s < best_id))) {
                    best_pri = priority_[s];
                    best_id  = s;
                }
            }
        }
        return best_id;
    }

    // ========================================================================
    // Processes
    // ========================================================================

    /**
     * @brief Sequential — sync external levels, update pending, handle MMIO.
     */
    void seq_process() {
        // Reset
        pending_lo_ = 0; pending_hi_ = 0;
        enable_lo_  = 0; enable_hi_  = 0;
        threshold_  = 0;
        claimed_mask_lo_ = 0; claimed_mask_hi_ = 0;
        priority_.fill(1);
        pending_lo_sig.write(0);
        pending_hi_sig.write(0);
        enable_lo_sig.write(0);
        enable_hi_sig.write(0);
        threshold_sig.write(0);
        mmio_resp_valid_o.write(false);
        mmio_resp_rdata_o.write(0);
        pending_lo_o.write(0);
        pending_hi_o.write(0);
        wait();

        while (true) {
            mmio_resp_valid_o.write(false);

            // --- Sync external levels → sticky pending ---
            for (unsigned s = 1; s <= kNumSources; ++s) {
                if (sample_source(s)) {
                    set_pending(s);
                }
            }

            // --- MMIO access ---
            if (mmio_req_valid_i.read()) {
                const uint32_t off   = mmio_req_addr_i.read().to_uint();
                const uint32_t wdata = mmio_req_wdata_i.read().to_uint();
                const bool     wr    = mmio_req_write_i.read();
                uint32_t rdata = 0;

                if (off < 0x0800) {
                    // PRIORITY[s] at offset 4*s
                    const unsigned s = off / 4;
                    if (s >= 1 && s <= kNumSources) {
                        if (wr) priority_[s] = wdata;
                        else    rdata = priority_[s];
                    }
                } else if (off == kPlicPendingLo) {
                    rdata = pending_lo_;
                } else if (off == kPlicPendingHi) {
                    rdata = pending_hi_;
                } else if (off == kPlicEnableLo) {
                    if (wr) enable_lo_ = wdata;
                    else    rdata = enable_lo_;
                } else if (off == kPlicEnableHi) {
                    if (wr) enable_hi_ = wdata;
                    else    rdata = enable_hi_;
                } else if (off == kPlicThreshold) {
                    if (wr) threshold_ = wdata;
                    else    rdata = threshold_;
                } else if (off == kPlicClaimComplete) {
                    if (!wr) {
                        // Claim
                        rdata = claim();
                        if (rdata != 0) {
                            clear_pending(rdata);
                            // Mark as claimed
                            if (rdata < 32) claimed_mask_lo_ |= (1u << rdata);
                            else            claimed_mask_hi_ |= (1u << (rdata - 32));
                        }
                    } else {
                        // Complete
                        const unsigned id = wdata;
                        if (id >= 1 && id <= kNumSources) {
                            if (id < 32) claimed_mask_lo_ &= ~(1u << id);
                            else         claimed_mask_hi_ &= ~(1u << (id - 32));
                            // Re-check level: if still asserted, re-pend
                            if (sample_source(id)) {
                                set_pending(id);
                            }
                        } else {
                            SC_REPORT_WARNING("Plic", "complete with invalid source id");
                        }
                    }
                } else if (off == kPlicMaxSourceId) {
                    rdata = kNumSources;
                }

                mmio_resp_valid_o.write(true);
                mmio_resp_rdata_o.write(rdata);
            }

            // Update combinational signals
            pending_lo_sig.write(pending_lo_);
            pending_hi_sig.write(pending_hi_);
            enable_lo_sig.write(enable_lo_);
            enable_hi_sig.write(enable_hi_);
            threshold_sig.write(threshold_);
            pending_lo_o.write(pending_lo_);
            pending_hi_o.write(pending_hi_);

            wait();
        }
    }

    /**
     * @brief Combinational — assert MEIP when any enabled pending source
     *        exceeds threshold.
     */
    void comb_meip() {
        const uint32_t plo = pending_lo_sig.read().to_uint();
        const uint32_t phi = pending_hi_sig.read().to_uint();
        const uint32_t elo = enable_lo_sig.read().to_uint();
        const uint32_t ehi = enable_hi_sig.read().to_uint();
        const uint32_t thr = threshold_sig.read().to_uint();

        bool any = false;
        for (unsigned s = 1; s <= kNumSources; ++s) {
            bool pend = (s < 32) ? ((plo >> s) & 1) : ((phi >> (s - 32)) & 1);
            bool en   = (s < 32) ? ((elo >> s) & 1) : ((ehi >> (s - 32)) & 1);
            if (pend && en && priority_[s] > thr) {
                any = true;
                break;
            }
        }
        meip_o.write(any);
    }
};

} // namespace core
} // namespace hybridacc
