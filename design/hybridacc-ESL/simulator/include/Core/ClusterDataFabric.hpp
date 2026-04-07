#pragma once

/**
 * @file ClusterDataFabric.hpp
 * @brief cc_cluster_data_fabric — Arbitrates DMA and NLU data requests
 *        to per-cluster AXI4-Lite data master ports.
 *
 * Round-robin arbiter, non-preemptive, single outstanding transaction.
 * Routes by @c cluster_id to the appropriate AXI4-Lite data master.
 * Encodes AXI resp != OKAY as @c error.
 *
 * @par FSM
 *   IDLE → GRANT → ADDR_PHASE → DATA_PHASE (write) / WAIT_RESP (read) →
 *   WAIT_RESP → COMPLETE → IDLE
 *
 * @par Spec reference
 *   Core.md §8.9  cc_cluster_data_fabric
 */

#include <systemc>
#include <cstdint>
#include "Utils/utils.hpp"
#include "Core/Types.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

template <unsigned NUM_CLUSTERS = 1>
SC_MODULE(ClusterDataFabric) {

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- DMA requester interface ---
    sc_in<bool>          dma_req_valid_i;
    sc_in<bool>          dma_req_write_i;
    sc_in<sc_uint<32>>   dma_req_cluster_id_i;
    sc_in<sc_uint<32>>   dma_req_addr_i;
    sc_in<sc_biguint<kClAxiDataWidth>>          dma_req_wdata_i;
    sc_in<sc_uint<kClAxiDataWidth / 8>>         dma_req_wstrb_i;
    sc_out<bool>         dma_req_ready_o;
    sc_out<bool>         dma_resp_valid_o;
    sc_out<sc_biguint<kClAxiDataWidth>>         dma_resp_rdata_o;
    sc_out<bool>         dma_resp_error_o;

    // --- NLU requester interface ---
    sc_in<bool>          nlu_req_valid_i;
    sc_in<bool>          nlu_req_write_i;
    sc_in<sc_uint<32>>   nlu_req_cluster_id_i;
    sc_in<sc_uint<32>>   nlu_req_addr_i;
    sc_in<sc_biguint<kClAxiDataWidth>>          nlu_req_wdata_i;
    sc_in<sc_uint<kClAxiDataWidth / 8>>         nlu_req_wstrb_i;
    sc_out<bool>         nlu_req_ready_o;
    sc_out<bool>         nlu_resp_valid_o;
    sc_out<sc_biguint<kClAxiDataWidth>>         nlu_resp_rdata_o;
    sc_out<bool>         nlu_resp_error_o;

    // --- Per-cluster AXI4-Lite data master (×NUM_CLUSTERS) ---
    // Write address channel
    sc_out<bool>         m_cl_data_aw_valid_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_aw_ready_i[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  m_cl_data_aw_addr_o[NUM_CLUSTERS];
    // Write data channel
    sc_out<bool>         m_cl_data_w_valid_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_w_ready_i[NUM_CLUSTERS];
    sc_out<sc_biguint<kClAxiDataWidth>>         m_cl_data_w_data_o[NUM_CLUSTERS];
    sc_out<sc_uint<kClAxiDataWidth / 8>>        m_cl_data_w_strb_o[NUM_CLUSTERS];
    // Write response channel
    sc_in<bool>          m_cl_data_b_valid_i[NUM_CLUSTERS];
    sc_out<bool>         m_cl_data_b_ready_o[NUM_CLUSTERS];
    sc_in<sc_uint<2>>    m_cl_data_b_resp_i[NUM_CLUSTERS];
    // Read address channel
    sc_out<bool>         m_cl_data_ar_valid_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_ar_ready_i[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  m_cl_data_ar_addr_o[NUM_CLUSTERS];
    // Read data channel
    sc_in<bool>          m_cl_data_r_valid_i[NUM_CLUSTERS];
    sc_out<bool>         m_cl_data_r_ready_o[NUM_CLUSTERS];
    sc_in<sc_biguint<kClAxiDataWidth>>          m_cl_data_r_data_i[NUM_CLUSTERS];
    sc_in<sc_uint<2>>    m_cl_data_r_resp_i[NUM_CLUSTERS];

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(ClusterDataFabric);

    ClusterDataFabric(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          dma_req_valid_i("dma_req_valid_i"),
          dma_req_write_i("dma_req_write_i"),
          dma_req_cluster_id_i("dma_req_cluster_id_i"),
          dma_req_addr_i("dma_req_addr_i"),
          dma_req_wdata_i("dma_req_wdata_i"),
          dma_req_wstrb_i("dma_req_wstrb_i"),
          dma_req_ready_o("dma_req_ready_o"),
          dma_resp_valid_o("dma_resp_valid_o"),
          dma_resp_rdata_o("dma_resp_rdata_o"),
          dma_resp_error_o("dma_resp_error_o"),
          nlu_req_valid_i("nlu_req_valid_i"),
          nlu_req_write_i("nlu_req_write_i"),
          nlu_req_cluster_id_i("nlu_req_cluster_id_i"),
          nlu_req_addr_i("nlu_req_addr_i"),
          nlu_req_wdata_i("nlu_req_wdata_i"),
          nlu_req_wstrb_i("nlu_req_wstrb_i"),
          nlu_req_ready_o("nlu_req_ready_o"),
          nlu_resp_valid_o("nlu_resp_valid_o"),
          nlu_resp_rdata_o("nlu_resp_rdata_o"),
          nlu_resp_error_o("nlu_resp_error_o")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    // ========================================================================
    // FSM states
    // ========================================================================

    enum class FabState : uint32_t {
        IDLE       = 0,
        GRANT      = 1,
        ADDR_PHASE = 2,
        DATA_PHASE = 3,  ///< write data channel (writes only)
        WAIT_RESP  = 4,  ///< wait for B (write) or R (read)
        COMPLETE   = 5,
    };

    // ========================================================================
    // Requester tag
    // ========================================================================

    enum class Requester : uint8_t {
        NONE = 0,
        DMA  = 1,
        NLU  = 2,
    };

    // ========================================================================
    // Captured transaction context
    // ========================================================================

    struct TxnCtx {
        Requester owner     = Requester::NONE;
        bool      write     = false;
        uint32_t  cluster_id= 0;
        uint32_t  addr      = 0;
        sc_biguint<kClAxiDataWidth> wdata;
        sc_uint<kClAxiDataWidth / 8> wstrb;
    };

    // ========================================================================
    // Port reset helpers
    // ========================================================================

    void reset_all_outputs() {
        dma_req_ready_o.write(false);
        dma_resp_valid_o.write(false);
        dma_resp_rdata_o.write(0);
        dma_resp_error_o.write(false);
        nlu_req_ready_o.write(false);
        nlu_resp_valid_o.write(false);
        nlu_resp_rdata_o.write(0);
        nlu_resp_error_o.write(false);
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            m_cl_data_aw_valid_o[c].write(false);
            m_cl_data_aw_addr_o[c].write(0);
            m_cl_data_w_valid_o[c].write(false);
            m_cl_data_w_data_o[c].write(0);
            m_cl_data_w_strb_o[c].write(0);
            m_cl_data_b_ready_o[c].write(false);
            m_cl_data_ar_valid_o[c].write(false);
            m_cl_data_ar_addr_o[c].write(0);
            m_cl_data_r_ready_o[c].write(false);
        }
    }

    void deassert_cluster_ports() {
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            m_cl_data_aw_valid_o[c].write(false);
            m_cl_data_w_valid_o[c].write(false);
            m_cl_data_b_ready_o[c].write(false);
            m_cl_data_ar_valid_o[c].write(false);
            m_cl_data_r_ready_o[c].write(false);
        }
    }

    // ========================================================================
    // Main sequential process
    // ========================================================================

    void seq_process() {
        // ---- Reset ----
        reset_all_outputs();
        wait();

        FabState  state = FabState::IDLE;
        Requester rr_last_winner = Requester::NONE; ///< round-robin pointer
        TxnCtx    txn;

        while (true) {
            // Default: deassert response valids each cycle
            dma_req_ready_o.write(false);
            dma_resp_valid_o.write(false);
            dma_resp_rdata_o.write(0);
            dma_resp_error_o.write(false);
            nlu_req_ready_o.write(false);
            nlu_resp_valid_o.write(false);
            nlu_resp_rdata_o.write(0);
            nlu_resp_error_o.write(false);
            deassert_cluster_ports();

            switch (state) {
            // ----------------------------------------------------------------
            case FabState::IDLE: {
                const bool dma_v = dma_req_valid_i.read();
                const bool nlu_v = nlu_req_valid_i.read();
                if (!dma_v && !nlu_v) break;

                // Round-robin: if last winner was DMA, prefer NLU this time.
                Requester winner = Requester::NONE;
                if (dma_v && !nlu_v)       winner = Requester::DMA;
                else if (!dma_v && nlu_v)  winner = Requester::NLU;
                else {
                    // Both requesting — round-robin
                    winner = (rr_last_winner == Requester::DMA)
                                 ? Requester::NLU : Requester::DMA;
                }

                rr_last_winner = winner;
                state = FabState::GRANT;

                // Capture transaction
                if (winner == Requester::DMA) {
                    txn.owner      = Requester::DMA;
                    txn.write      = dma_req_write_i.read();
                    txn.cluster_id = dma_req_cluster_id_i.read().to_uint();
                    txn.addr       = dma_req_addr_i.read().to_uint();
                    txn.wdata      = dma_req_wdata_i.read();
                    txn.wstrb      = dma_req_wstrb_i.read();
                    dma_req_ready_o.write(true);
                } else {
                    txn.owner      = Requester::NLU;
                    txn.write      = nlu_req_write_i.read();
                    txn.cluster_id = nlu_req_cluster_id_i.read().to_uint();
                    txn.addr       = nlu_req_addr_i.read().to_uint();
                    txn.wdata      = nlu_req_wdata_i.read();
                    txn.wstrb      = nlu_req_wstrb_i.read();
                    nlu_req_ready_o.write(true);
                }
                break;
            }
            // ----------------------------------------------------------------
            case FabState::GRANT: {
                // Validate cluster_id range
                if (txn.cluster_id >= NUM_CLUSTERS) {
                    // Immediate error — no external transaction
                    deliver_error_resp(txn);
                    state = FabState::IDLE;
                    DEBUG_MSG("ClusterDataFabric: cluster_id " << txn.cluster_id
                              << " out of range", DEBUG_LEVEL_CLUSTER_COMPONENTS);
                    break;
                }
                state = FabState::ADDR_PHASE;
                break;
            }
            // ----------------------------------------------------------------
            case FabState::ADDR_PHASE: {
                const unsigned ci = txn.cluster_id;
                if (txn.write) {
                    m_cl_data_aw_valid_o[ci].write(true);
                    m_cl_data_aw_addr_o[ci].write(txn.addr);
                    if (m_cl_data_aw_ready_i[ci].read()) {
                        state = FabState::DATA_PHASE;
                    }
                } else {
                    m_cl_data_ar_valid_o[ci].write(true);
                    m_cl_data_ar_addr_o[ci].write(txn.addr);
                    if (m_cl_data_ar_ready_i[ci].read()) {
                        state = FabState::WAIT_RESP;
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            case FabState::DATA_PHASE: {
                const unsigned ci = txn.cluster_id;
                m_cl_data_w_valid_o[ci].write(true);
                m_cl_data_w_data_o[ci].write(txn.wdata);
                m_cl_data_w_strb_o[ci].write(txn.wstrb);
                if (m_cl_data_w_ready_i[ci].read()) {
                    m_cl_data_b_ready_o[ci].write(true);
                    state = FabState::WAIT_RESP;
                }
                break;
            }
            // ----------------------------------------------------------------
            case FabState::WAIT_RESP: {
                const unsigned ci = txn.cluster_id;
                if (txn.write) {
                    m_cl_data_b_ready_o[ci].write(true);
                    if (m_cl_data_b_valid_i[ci].read()) {
                        bool err = (m_cl_data_b_resp_i[ci].read().to_uint() != 0);
                        deliver_resp(txn, 0, err);
                        state = FabState::COMPLETE;
                    }
                } else {
                    m_cl_data_r_ready_o[ci].write(true);
                    if (m_cl_data_r_valid_i[ci].read()) {
                        bool err = (m_cl_data_r_resp_i[ci].read().to_uint() != 0);
                        deliver_resp(txn, m_cl_data_r_data_i[ci].read(), err);
                        state = FabState::COMPLETE;
                    }
                }
                break;
            }
            // ----------------------------------------------------------------
            case FabState::COMPLETE: {
                state = FabState::IDLE;
                break;
            }
            default:
                state = FabState::IDLE;
                break;
            } // switch

            wait();
        } // while
    }

    // ========================================================================
    // Response delivery helpers
    // ========================================================================

    void deliver_resp(const TxnCtx& txn, sc_biguint<kClAxiDataWidth> rdata, bool err) {
        if (txn.owner == Requester::DMA) {
            dma_resp_valid_o.write(true);
            dma_resp_rdata_o.write(rdata);
            dma_resp_error_o.write(err);
        } else {
            nlu_resp_valid_o.write(true);
            nlu_resp_rdata_o.write(rdata);
            nlu_resp_error_o.write(err);
        }
    }

    void deliver_error_resp(const TxnCtx& txn) {
        deliver_resp(txn, 0, true);
    }
};

} // namespace core
} // namespace hybridacc
