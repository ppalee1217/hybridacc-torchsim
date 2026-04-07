#pragma once

/**
 * @file CmdFabric.hpp
 * @brief cc_cmd_fabric — MMIO address decoder and router for cc_core_mcu.
 *
 * Decodes core MMIO requests and routes to:
 *   - Core local control register bank
 *   - DMA MMIO
 *   - PLIC MMIO
 *   - Core local timer/MSIP MMIO
 *   - Cluster unicast / masked-broadcast AHB
 *   - NLU AHB
 *
 * Provides deterministic blocking completion and cluster broadcast sequencing.
 *
 * @par Spec reference
 *   Core.md §8.5  cc_cmd_fabric
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
SC_MODULE(CmdFabric) {

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- Core MMIO request (from MEM stage) ---
    sc_in<bool>          core_mmio_req_valid_i;
    sc_in<bool>          core_mmio_req_write_i;
    sc_in<sc_uint<32>>   core_mmio_req_addr_i;
    sc_in<sc_uint<32>>   core_mmio_req_wdata_i;
    sc_in<sc_uint<4>>    core_mmio_req_wstrb_i;
    sc_out<bool>         core_mmio_resp_valid_o;
    sc_out<sc_uint<32>>  core_mmio_resp_rdata_o;

    // --- DMA MMIO route ---
    sc_out<bool>         dma_mmio_req_valid_o;
    sc_out<bool>         dma_mmio_req_write_o;
    sc_out<sc_uint<32>>  dma_mmio_req_addr_o;
    sc_out<sc_uint<32>>  dma_mmio_req_wdata_o;
    sc_in<bool>          dma_mmio_resp_valid_i;
    sc_in<sc_uint<32>>   dma_mmio_resp_rdata_i;

    // --- PLIC MMIO route ---
    sc_out<bool>         plic_mmio_req_valid_o;
    sc_out<bool>         plic_mmio_req_write_o;
    sc_out<sc_uint<32>>  plic_mmio_req_addr_o;
    sc_out<sc_uint<32>>  plic_mmio_req_wdata_o;
    sc_in<bool>          plic_mmio_resp_valid_i;
    sc_in<sc_uint<32>>   plic_mmio_resp_rdata_i;

    // --- Core local timer route ---
    sc_out<bool>         timer_mmio_req_valid_o;
    sc_out<bool>         timer_mmio_req_write_o;
    sc_out<sc_uint<32>>  timer_mmio_req_addr_o;
    sc_out<sc_uint<32>>  timer_mmio_req_wdata_o;
    sc_in<bool>          timer_mmio_resp_valid_i;
    sc_in<sc_uint<32>>   timer_mmio_resp_rdata_i;

    // --- Cluster AHB command route (unicast, one per cluster) ---
    // For ESL, simplified to valid/ready + data interface per cluster
    sc_out<bool>         cl_cmd_req_valid_o[NUM_CLUSTERS];
    sc_out<bool>         cl_cmd_req_write_o[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  cl_cmd_req_addr_o[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  cl_cmd_req_wdata_o[NUM_CLUSTERS];
    sc_in<bool>          cl_cmd_resp_valid_i[NUM_CLUSTERS];
    sc_in<sc_uint<32>>   cl_cmd_resp_rdata_i[NUM_CLUSTERS];

    // --- NLU AHB route ---
    sc_out<bool>         nlu_cmd_req_valid_o[NUM_NLU > 0 ? NUM_NLU : 1];
    sc_out<bool>         nlu_cmd_req_write_o[NUM_NLU > 0 ? NUM_NLU : 1];
    sc_out<sc_uint<32>>  nlu_cmd_req_addr_o[NUM_NLU > 0 ? NUM_NLU : 1];
    sc_out<sc_uint<32>>  nlu_cmd_req_wdata_o[NUM_NLU > 0 ? NUM_NLU : 1];
    sc_in<bool>          nlu_cmd_resp_valid_i[NUM_NLU > 0 ? NUM_NLU : 1];
    sc_in<sc_uint<32>>   nlu_cmd_resp_rdata_i[NUM_NLU > 0 ? NUM_NLU : 1];

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(CmdFabric);

    CmdFabric(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          core_mmio_req_valid_i("core_mmio_req_valid_i"),
          core_mmio_req_write_i("core_mmio_req_write_i"),
          core_mmio_req_addr_i("core_mmio_req_addr_i"),
          core_mmio_req_wdata_i("core_mmio_req_wdata_i"),
          core_mmio_req_wstrb_i("core_mmio_req_wstrb_i"),
          core_mmio_resp_valid_o("core_mmio_resp_valid_o"),
          core_mmio_resp_rdata_o("core_mmio_resp_rdata_o"),
          dma_mmio_req_valid_o("dma_mmio_req_valid_o"),
          dma_mmio_req_write_o("dma_mmio_req_write_o"),
          dma_mmio_req_addr_o("dma_mmio_req_addr_o"),
          dma_mmio_req_wdata_o("dma_mmio_req_wdata_o"),
          dma_mmio_resp_valid_i("dma_mmio_resp_valid_i"),
          dma_mmio_resp_rdata_i("dma_mmio_resp_rdata_i"),
          plic_mmio_req_valid_o("plic_mmio_req_valid_o"),
          plic_mmio_req_write_o("plic_mmio_req_write_o"),
          plic_mmio_req_addr_o("plic_mmio_req_addr_o"),
          plic_mmio_req_wdata_o("plic_mmio_req_wdata_o"),
          plic_mmio_resp_valid_i("plic_mmio_resp_valid_i"),
          plic_mmio_resp_rdata_i("plic_mmio_resp_rdata_i"),
          timer_mmio_req_valid_o("timer_mmio_req_valid_o"),
          timer_mmio_req_write_o("timer_mmio_req_write_o"),
          timer_mmio_req_addr_o("timer_mmio_req_addr_o"),
          timer_mmio_req_wdata_o("timer_mmio_req_wdata_o"),
          timer_mmio_resp_valid_i("timer_mmio_resp_valid_i"),
          timer_mmio_resp_rdata_i("timer_mmio_resp_rdata_i")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    // ========================================================================
    // Target identification
    // ========================================================================

    enum class Target : uint8_t {
        LOCAL_CTRL, DMA, PLIC, TIMER, CLUSTER, CLUSTER_BCAST, NLU, DECODE_FAULT
    };

    // ========================================================================
    // Local control register bank
    // ========================================================================

    uint32_t cluster_mask_lo_reg = 0;
    uint32_t cluster_mask_hi_reg = 0;
    uint32_t mmio_err_status_reg = 0;
    uint32_t last_target_id_reg  = 0;
    uint32_t last_fault_addr_reg = 0;
    uint32_t last_fault_info_reg = 0;
    uint32_t boot_reason_reg     = 0;

    // ========================================================================
    // Address decode
    // ========================================================================

    struct DecodeResult {
        Target   target;
        uint32_t local_offset;  ///< offset within target window
        unsigned target_id;     ///< cluster/NLU index
    };

    static DecodeResult decode_addr(uint32_t addr) {
        DecodeResult r;
        r.target_id = 0;
        r.local_offset = 0;

        if (addr >= kBaseLocalCtrl && addr <= kEndLocalCtrl) {
            r.target = Target::LOCAL_CTRL;
            r.local_offset = addr - kBaseLocalCtrl;
        } else if (addr >= kBaseDmaMmio && addr <= kEndDmaMmio) {
            r.target = Target::DMA;
            r.local_offset = addr - kBaseDmaMmio;
        } else if (addr >= kBaseLocalTimer && addr <= kEndLocalTimer) {
            r.target = Target::TIMER;
            r.local_offset = addr - kBaseLocalTimer;
        } else if (addr >= kBasePlic && addr <= kEndPlic) {
            r.target = Target::PLIC;
            r.local_offset = addr - kBasePlic;
        } else if (addr >= kBaseClusterUnicast && addr <= kEndClusterUnicast) {
            r.target = Target::CLUSTER;
            r.target_id    = (addr - kBaseClusterUnicast) / kClusterStride;
            r.local_offset = (addr - kBaseClusterUnicast) % kClusterStride;
        } else if (addr >= kBaseClusterBcast && addr <= kEndClusterBcast) {
            r.target = Target::CLUSTER_BCAST;
            r.local_offset = addr - kBaseClusterBcast;
        } else if (addr >= kBaseNlu && addr <= kEndNlu) {
            r.target = Target::NLU;
            r.target_id    = (addr - kBaseNlu) / kNluStride;
            r.local_offset = (addr - kBaseNlu) % kNluStride;
        } else {
            r.target = Target::DECODE_FAULT;
        }
        return r;
    }

    // ========================================================================
    // FSM
    // ========================================================================

    enum class FabricState : uint8_t {
        IDLE,
        WAIT_DMA,
        WAIT_PLIC,
        WAIT_TIMER,
        WAIT_CLUSTER,
        WAIT_NLU,
        BCAST_SEQ,   ///< broadcasting to multiple clusters sequentially
    };

    FabricState state_reg        = FabricState::IDLE;
    unsigned    bcast_idx_reg    = 0;    ///< current broadcast cluster index
    uint32_t    bcast_addr_reg   = 0;
    uint32_t    bcast_wdata_reg  = 0;
    bool        bcast_write_reg  = false;
    bool        bcast_any_error  = false;

    // ========================================================================
    // Processes
    // ========================================================================

    /**
     * @brief Sequential — decode MMIO address, route to target, collect response.
     *
     * Implements blocking MMIO semantics: one outstanding transaction at a time.
     * Cluster broadcast is serialised across all mask-hit clusters.
     */
    void seq_process() {
        // Reset all outputs
        core_mmio_resp_valid_o.write(false);
        core_mmio_resp_rdata_o.write(0);
        dma_mmio_req_valid_o.write(false);
        plic_mmio_req_valid_o.write(false);
        timer_mmio_req_valid_o.write(false);
        for (unsigned i = 0; i < NUM_CLUSTERS; ++i) {
            cl_cmd_req_valid_o[i].write(false);
        }
        for (unsigned i = 0; i < (NUM_NLU > 0 ? NUM_NLU : 1u); ++i) {
            nlu_cmd_req_valid_o[i].write(false);
        }
        cluster_mask_lo_reg = 0;
        cluster_mask_hi_reg = 0;
        mmio_err_status_reg = 0;
        state_reg = FabricState::IDLE;
        wait();

        while (true) {
            // Default de-assert
            core_mmio_resp_valid_o.write(false);
            dma_mmio_req_valid_o.write(false);
            plic_mmio_req_valid_o.write(false);
            timer_mmio_req_valid_o.write(false);
            for (unsigned i = 0; i < NUM_CLUSTERS; ++i)
                cl_cmd_req_valid_o[i].write(false);
            for (unsigned i = 0; i < (NUM_NLU > 0 ? NUM_NLU : 1u); ++i)
                nlu_cmd_req_valid_o[i].write(false);

            switch (state_reg) {

            // ----------------------------------------------------------
            case FabricState::IDLE: {
                if (core_mmio_req_valid_i.read()) {
                    const uint32_t addr  = core_mmio_req_addr_i.read().to_uint();
                    const uint32_t wdata = core_mmio_req_wdata_i.read().to_uint();
                    const bool     wr    = core_mmio_req_write_i.read();
                    const auto     dec   = decode_addr(addr);

                    switch (dec.target) {

                    case Target::LOCAL_CTRL: {
                        // Handled locally in 1 cycle
                        uint32_t rdata = 0;
                        if (wr) {
                            switch (dec.local_offset) {
                                case kLocalClusterMaskLo: cluster_mask_lo_reg = wdata; break;
                                case kLocalClusterMaskHi: cluster_mask_hi_reg = wdata; break;
                                case kLocalMmioErrStatus:
                                    mmio_err_status_reg &= ~wdata; // W1C
                                    break;
                                default: break;
                            }
                        } else {
                            switch (dec.local_offset) {
                                case kLocalClusterMaskLo: rdata = cluster_mask_lo_reg; break;
                                case kLocalClusterMaskHi: rdata = cluster_mask_hi_reg; break;
                                case kLocalMmioErrStatus: rdata = mmio_err_status_reg; break;
                                case kLocalLastTargetId:  rdata = last_target_id_reg;  break;
                                case kLocalLastFaultAddr: rdata = last_fault_addr_reg; break;
                                case kLocalLastFaultInfo: rdata = last_fault_info_reg; break;
                                case kLocalBootReason:    rdata = boot_reason_reg;     break;
                                case kLocalFabricCap0:
                                    rdata = 0x1; // broadcast capable
                                    break;
                                default: break;
                            }
                        }
                        core_mmio_resp_valid_o.write(true);
                        core_mmio_resp_rdata_o.write(rdata);
                        break;
                    }

                    case Target::DMA:
                        dma_mmio_req_valid_o.write(true);
                        dma_mmio_req_write_o.write(wr);
                        dma_mmio_req_addr_o.write(dec.local_offset);
                        dma_mmio_req_wdata_o.write(wdata);
                        state_reg = FabricState::WAIT_DMA;
                        break;

                    case Target::PLIC:
                        plic_mmio_req_valid_o.write(true);
                        plic_mmio_req_write_o.write(wr);
                        plic_mmio_req_addr_o.write(dec.local_offset);
                        plic_mmio_req_wdata_o.write(wdata);
                        state_reg = FabricState::WAIT_PLIC;
                        break;

                    case Target::TIMER:
                        timer_mmio_req_valid_o.write(true);
                        timer_mmio_req_write_o.write(wr);
                        timer_mmio_req_addr_o.write(dec.local_offset);
                        timer_mmio_req_wdata_o.write(wdata);
                        state_reg = FabricState::WAIT_TIMER;
                        break;

                    case Target::CLUSTER:
                        if (dec.target_id < NUM_CLUSTERS) {
                            cl_cmd_req_valid_o[dec.target_id].write(true);
                            cl_cmd_req_write_o[dec.target_id].write(wr);
                            cl_cmd_req_addr_o[dec.target_id].write(dec.local_offset);
                            cl_cmd_req_wdata_o[dec.target_id].write(wdata);
                            state_reg = FabricState::WAIT_CLUSTER;
                            last_target_id_reg = dec.target_id;
                        } else {
                            // Out of range
                            mmio_err_status_reg |= 0x3; // pending + decode_fault
                            last_fault_addr_reg = addr;
                            core_mmio_resp_valid_o.write(true);
                            core_mmio_resp_rdata_o.write(0);
                        }
                        break;

                    case Target::CLUSTER_BCAST: {
                        const uint64_t mask = (static_cast<uint64_t>(cluster_mask_hi_reg) << 32)
                                            | cluster_mask_lo_reg;
                        if (mask == 0) {
                            // mask=0: no op, immediate completion
                            core_mmio_resp_valid_o.write(true);
                            core_mmio_resp_rdata_o.write(0);
                        } else if (!wr) {
                            // Broadcast read with popcount(mask)!=1 is a fault
                            unsigned popcnt = 0;
                            for (unsigned i = 0; i < NUM_CLUSTERS; ++i)
                                if (mask & (1ull << i)) popcnt++;
                            if (popcnt != 1) {
                                mmio_err_status_reg |= 0x5; // pending + broadcast_read_fault
                                last_fault_addr_reg = addr;
                                core_mmio_resp_valid_o.write(true);
                                core_mmio_resp_rdata_o.write(0);
                            } else {
                                // Single cluster read via broadcast alias
                                for (unsigned i = 0; i < NUM_CLUSTERS; ++i) {
                                    if (mask & (1ull << i)) {
                                        cl_cmd_req_valid_o[i].write(true);
                                        cl_cmd_req_write_o[i].write(false);
                                        cl_cmd_req_addr_o[i].write(dec.local_offset);
                                        cl_cmd_req_wdata_o[i].write(0);
                                        last_target_id_reg = i;
                                        break;
                                    }
                                }
                                state_reg = FabricState::WAIT_CLUSTER;
                            }
                        } else {
                            // Broadcast write: serialise
                            bcast_addr_reg  = dec.local_offset;
                            bcast_wdata_reg = wdata;
                            bcast_write_reg = true;
                            bcast_any_error = false;
                            bcast_idx_reg   = 0;
                            state_reg = FabricState::BCAST_SEQ;
                        }
                        break;
                    }

                    case Target::NLU:
                        if (NUM_NLU > 0 && dec.target_id < NUM_NLU) {
                            nlu_cmd_req_valid_o[dec.target_id].write(true);
                            nlu_cmd_req_write_o[dec.target_id].write(wr);
                            nlu_cmd_req_addr_o[dec.target_id].write(dec.local_offset);
                            nlu_cmd_req_wdata_o[dec.target_id].write(wdata);
                            state_reg = FabricState::WAIT_NLU;
                            last_target_id_reg = dec.target_id;
                        } else {
                            mmio_err_status_reg |= 0x3;
                            last_fault_addr_reg = addr;
                            core_mmio_resp_valid_o.write(true);
                            core_mmio_resp_rdata_o.write(0);
                        }
                        break;

                    case Target::DECODE_FAULT:
                        mmio_err_status_reg |= 0x3; // pending + decode_fault
                        last_fault_addr_reg = addr;
                        core_mmio_resp_valid_o.write(true);
                        core_mmio_resp_rdata_o.write(0);
                        SC_REPORT_WARNING("CmdFabric", "MMIO decode fault");
                        break;
                    }
                }
                break;
            }

            // ----------------------------------------------------------
            case FabricState::WAIT_DMA:
                if (dma_mmio_resp_valid_i.read()) {
                    core_mmio_resp_valid_o.write(true);
                    core_mmio_resp_rdata_o.write(dma_mmio_resp_rdata_i.read());
                    state_reg = FabricState::IDLE;
                }
                break;

            case FabricState::WAIT_PLIC:
                if (plic_mmio_resp_valid_i.read()) {
                    core_mmio_resp_valid_o.write(true);
                    core_mmio_resp_rdata_o.write(plic_mmio_resp_rdata_i.read());
                    state_reg = FabricState::IDLE;
                }
                break;

            case FabricState::WAIT_TIMER:
                if (timer_mmio_resp_valid_i.read()) {
                    core_mmio_resp_valid_o.write(true);
                    core_mmio_resp_rdata_o.write(timer_mmio_resp_rdata_i.read());
                    state_reg = FabricState::IDLE;
                }
                break;

            case FabricState::WAIT_CLUSTER: {
                const unsigned tid = last_target_id_reg;
                if (tid < NUM_CLUSTERS && cl_cmd_resp_valid_i[tid].read()) {
                    core_mmio_resp_valid_o.write(true);
                    core_mmio_resp_rdata_o.write(cl_cmd_resp_rdata_i[tid].read());
                    state_reg = FabricState::IDLE;
                }
                break;
            }

            case FabricState::WAIT_NLU: {
                const unsigned tid = last_target_id_reg;
                if (NUM_NLU > 0 && tid < NUM_NLU && nlu_cmd_resp_valid_i[tid].read()) {
                    core_mmio_resp_valid_o.write(true);
                    core_mmio_resp_rdata_o.write(nlu_cmd_resp_rdata_i[tid].read());
                    state_reg = FabricState::IDLE;
                }
                break;
            }

            // ----------------------------------------------------------
            case FabricState::BCAST_SEQ: {
                // Find next cluster in mask starting from bcast_idx_reg
                const uint64_t mask = (static_cast<uint64_t>(cluster_mask_hi_reg) << 32)
                                    | cluster_mask_lo_reg;
                bool found = false;
                for (unsigned i = bcast_idx_reg; i < NUM_CLUSTERS; ++i) {
                    if (mask & (1ull << i)) {
                        cl_cmd_req_valid_o[i].write(true);
                        cl_cmd_req_write_o[i].write(bcast_write_reg);
                        cl_cmd_req_addr_o[i].write(bcast_addr_reg);
                        cl_cmd_req_wdata_o[i].write(bcast_wdata_reg);
                        last_target_id_reg = i;
                        bcast_idx_reg = i;
                        found = true;
                        // Wait for response next cycle
                        state_reg = FabricState::WAIT_CLUSTER;
                        break;
                    }
                }
                if (!found) {
                    // All clusters done
                    core_mmio_resp_valid_o.write(true);
                    core_mmio_resp_rdata_o.write(0);
                    if (bcast_any_error)
                        mmio_err_status_reg |= 0x9; // pending + target_bus_fault
                    state_reg = FabricState::IDLE;
                }
                break;
            }

            } // switch

            wait();
        }
    }
};

} // namespace core
} // namespace hybridacc
