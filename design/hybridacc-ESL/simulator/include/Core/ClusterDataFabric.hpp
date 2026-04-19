#pragma once

/**
 * @file ClusterDataFabric.hpp
 * @brief cc_cluster_data_fabric — Dual AXI4-Lite ingress (DMA + NLU) with
 *        per-direction RR arbiter, per-requester per-direction ROB, and
 *        per-cluster AXI4-Lite master egress.
 *
 * @par Key design decisions (per report §5)
 *   - DMA and NLU ingress are both standard AXI4-Lite slave (AW/W/B/AR/R).
 *   - Two per-direction strict RR arbiters (write / read).
 *   - Per-requester, per-direction ROB for in-order retire.
 *   - Per-cluster issue queue and inflight counter.
 *   - Outstanding limit = 4 (bring-up default).
 *   - No cluster_id sideband; cluster selection via address decode.
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

template <unsigned NUM_CLUSTERS = 1>
SC_MODULE(ClusterDataFabric) {

    // ========================================================================
    // Compile-time parameters
    // ========================================================================

    static constexpr unsigned kMaxOutstanding   = 8;
    static constexpr unsigned kIngressDepth     = 8;
    static constexpr unsigned kRobDepth         = 8;
    static constexpr unsigned kClusterAddrBits  = 24;
    static constexpr uint32_t kClusterAddrMask  = (1u << kClusterAddrBits) - 1;

    // ========================================================================
    // External ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- DMA AXI4-Lite slave ingress ---
    sc_in<bool>          s_dma_axi_aw_valid_i;
    sc_out<bool>         s_dma_axi_aw_ready_o;
    sc_in<sc_uint<32>>   s_dma_axi_aw_addr_i;
    sc_in<bool>          s_dma_axi_w_valid_i;
    sc_out<bool>         s_dma_axi_w_ready_o;
    sc_in<sc_biguint<kClAxiDataWidth>>  s_dma_axi_w_data_i;
    sc_in<sc_uint<kClAxiDataWidth / 8>> s_dma_axi_w_strb_i;
    sc_out<bool>         s_dma_axi_b_valid_o;
    sc_in<bool>          s_dma_axi_b_ready_i;
    sc_out<sc_uint<2>>   s_dma_axi_b_resp_o;
    sc_in<bool>          s_dma_axi_ar_valid_i;
    sc_out<bool>         s_dma_axi_ar_ready_o;
    sc_in<sc_uint<32>>   s_dma_axi_ar_addr_i;
    sc_out<bool>         s_dma_axi_r_valid_o;
    sc_in<bool>          s_dma_axi_r_ready_i;
    sc_out<sc_biguint<kClAxiDataWidth>>  s_dma_axi_r_data_o;
    sc_out<sc_uint<2>>   s_dma_axi_r_resp_o;

    // --- NLU AXI4-Lite slave ingress ---
    sc_in<bool>          s_nlu_axi_aw_valid_i;
    sc_out<bool>         s_nlu_axi_aw_ready_o;
    sc_in<sc_uint<32>>   s_nlu_axi_aw_addr_i;
    sc_in<bool>          s_nlu_axi_w_valid_i;
    sc_out<bool>         s_nlu_axi_w_ready_o;
    sc_in<sc_biguint<kClAxiDataWidth>>  s_nlu_axi_w_data_i;
    sc_in<sc_uint<kClAxiDataWidth / 8>> s_nlu_axi_w_strb_i;
    sc_out<bool>         s_nlu_axi_b_valid_o;
    sc_in<bool>          s_nlu_axi_b_ready_i;
    sc_out<sc_uint<2>>   s_nlu_axi_b_resp_o;
    sc_in<bool>          s_nlu_axi_ar_valid_i;
    sc_out<bool>         s_nlu_axi_ar_ready_o;
    sc_in<sc_uint<32>>   s_nlu_axi_ar_addr_i;
    sc_out<bool>         s_nlu_axi_r_valid_o;
    sc_in<bool>          s_nlu_axi_r_ready_i;
    sc_out<sc_biguint<kClAxiDataWidth>>  s_nlu_axi_r_data_o;
    sc_out<sc_uint<2>>   s_nlu_axi_r_resp_o;

    // --- Per-cluster AXI4-Lite master egress ---
    sc_out<bool>         m_cl_data_aw_valid_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_aw_ready_i[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  m_cl_data_aw_addr_o[NUM_CLUSTERS];
    sc_out<bool>         m_cl_data_w_valid_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_w_ready_i[NUM_CLUSTERS];
    sc_out<sc_biguint<kClAxiDataWidth>>         m_cl_data_w_data_o[NUM_CLUSTERS];
    sc_out<sc_uint<kClAxiDataWidth / 8>>        m_cl_data_w_strb_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_b_valid_i[NUM_CLUSTERS];
    sc_out<bool>         m_cl_data_b_ready_o[NUM_CLUSTERS];
    sc_in<sc_uint<2>>    m_cl_data_b_resp_i[NUM_CLUSTERS];
    sc_out<bool>         m_cl_data_ar_valid_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_ar_ready_i[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  m_cl_data_ar_addr_o[NUM_CLUSTERS];
    sc_in<bool>          m_cl_data_r_valid_i[NUM_CLUSTERS];
    sc_out<bool>         m_cl_data_r_ready_o[NUM_CLUSTERS];
    sc_in<sc_biguint<kClAxiDataWidth>>          m_cl_data_r_data_i[NUM_CLUSTERS];
    sc_in<sc_uint<2>>    m_cl_data_r_resp_i[NUM_CLUSTERS];

    // ========================================================================
    // Internal types
    // ========================================================================

    enum class Requester : uint8_t { DMA = 0, NLU = 1 };

    struct InternalReq {
        Requester owner = Requester::DMA;
        bool      write = false;
        unsigned  cluster_id = 0;
        uint32_t  local_addr = 0;
        sc_biguint<kClAxiDataWidth> data{0};
        uint8_t   strb = 0;
        uint32_t  seq = 0;
    };

    struct RobEntry {
        bool     valid = false;
        bool     complete = false;
        sc_biguint<kClAxiDataWidth> data{0};
        uint8_t  resp = 0;
    };

    // ========================================================================
    // Address decode
    // ========================================================================

    static unsigned decode_cluster_id(uint32_t global_addr) {
        unsigned cid = global_addr >> kClusterAddrBits;
        return (cid < NUM_CLUSTERS) ? cid : 0;
    }

    static uint32_t decode_local_addr(uint32_t global_addr) {
        return global_addr & kClusterAddrMask;
    }

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(ClusterDataFabric);

    ClusterDataFabric(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          // DMA slave
          s_dma_axi_aw_valid_i("s_dma_axi_aw_valid_i"),
          s_dma_axi_aw_ready_o("s_dma_axi_aw_ready_o"),
          s_dma_axi_aw_addr_i("s_dma_axi_aw_addr_i"),
          s_dma_axi_w_valid_i("s_dma_axi_w_valid_i"),
          s_dma_axi_w_ready_o("s_dma_axi_w_ready_o"),
          s_dma_axi_w_data_i("s_dma_axi_w_data_i"),
          s_dma_axi_w_strb_i("s_dma_axi_w_strb_i"),
          s_dma_axi_b_valid_o("s_dma_axi_b_valid_o"),
          s_dma_axi_b_ready_i("s_dma_axi_b_ready_i"),
          s_dma_axi_b_resp_o("s_dma_axi_b_resp_o"),
          s_dma_axi_ar_valid_i("s_dma_axi_ar_valid_i"),
          s_dma_axi_ar_ready_o("s_dma_axi_ar_ready_o"),
          s_dma_axi_ar_addr_i("s_dma_axi_ar_addr_i"),
          s_dma_axi_r_valid_o("s_dma_axi_r_valid_o"),
          s_dma_axi_r_ready_i("s_dma_axi_r_ready_i"),
          s_dma_axi_r_data_o("s_dma_axi_r_data_o"),
          s_dma_axi_r_resp_o("s_dma_axi_r_resp_o"),
          // NLU slave
          s_nlu_axi_aw_valid_i("s_nlu_axi_aw_valid_i"),
          s_nlu_axi_aw_ready_o("s_nlu_axi_aw_ready_o"),
          s_nlu_axi_aw_addr_i("s_nlu_axi_aw_addr_i"),
          s_nlu_axi_w_valid_i("s_nlu_axi_w_valid_i"),
          s_nlu_axi_w_ready_o("s_nlu_axi_w_ready_o"),
          s_nlu_axi_w_data_i("s_nlu_axi_w_data_i"),
          s_nlu_axi_w_strb_i("s_nlu_axi_w_strb_i"),
          s_nlu_axi_b_valid_o("s_nlu_axi_b_valid_o"),
          s_nlu_axi_b_ready_i("s_nlu_axi_b_ready_i"),
          s_nlu_axi_b_resp_o("s_nlu_axi_b_resp_o"),
          s_nlu_axi_ar_valid_i("s_nlu_axi_ar_valid_i"),
          s_nlu_axi_ar_ready_o("s_nlu_axi_ar_ready_o"),
          s_nlu_axi_ar_addr_i("s_nlu_axi_ar_addr_i"),
          s_nlu_axi_r_valid_o("s_nlu_axi_r_valid_o"),
          s_nlu_axi_r_ready_i("s_nlu_axi_r_ready_i"),
          s_nlu_axi_r_data_o("s_nlu_axi_r_data_o"),
          s_nlu_axi_r_resp_o("s_nlu_axi_r_resp_o")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    // ========================================================================
    // DMA ingress FIFOs (after AXI capture)
    // ========================================================================

    struct AwEntry { uint32_t addr; };
    struct WEntry  { sc_biguint<kClAxiDataWidth> data; uint8_t strb; };
    struct ArEntry { uint32_t addr; };

    std::deque<AwEntry> dma_aw_fifo_reg;
    std::deque<WEntry>  dma_w_fifo_reg;
    std::deque<ArEntry> dma_ar_fifo_reg;

    // DMA merged request queues
    std::deque<InternalReq> dma_wr_req_fifo_reg;
    std::deque<InternalReq> dma_rd_req_fifo_reg;

    // ========================================================================
    // NLU ingress FIFOs
    // ========================================================================

    std::deque<AwEntry> nlu_aw_fifo_reg;
    std::deque<WEntry>  nlu_w_fifo_reg;
    std::deque<ArEntry> nlu_ar_fifo_reg;

    std::deque<InternalReq> nlu_wr_req_fifo_reg;
    std::deque<InternalReq> nlu_rd_req_fifo_reg;

    // ========================================================================
    // Per-requester, per-direction ROB
    // ========================================================================

    std::deque<RobEntry> dma_wr_rob_reg;
    uint32_t dma_wr_seq_reg = 0;
    uint32_t dma_wr_retire_head_seq_reg = 0;

    std::deque<RobEntry> dma_rd_rob_reg;
    uint32_t dma_rd_seq_reg = 0;
    uint32_t dma_rd_retire_head_seq_reg = 0;

    std::deque<RobEntry> nlu_wr_rob_reg;
    uint32_t nlu_wr_seq_reg = 0;
    uint32_t nlu_wr_retire_head_seq_reg = 0;

    std::deque<RobEntry> nlu_rd_rob_reg;
    uint32_t nlu_rd_seq_reg = 0;
    uint32_t nlu_rd_retire_head_seq_reg = 0;

    // ========================================================================
    // RR arbiter state
    // ========================================================================

    Requester wr_rr_last_grant_reg = Requester::NLU; // start with NLU so DMA gets first
    Requester rd_rr_last_grant_reg = Requester::NLU;

    // ========================================================================
    // Per-cluster issue/inflight queues
    // ========================================================================

    struct ClusterInflight {
        Requester owner;
        bool      write;
        uint32_t  rob_seq;
    };

    std::deque<InternalReq> cluster_wr_issue_fifo_reg[NUM_CLUSTERS];
    std::deque<InternalReq> cluster_rd_issue_fifo_reg[NUM_CLUSTERS];
    std::deque<ClusterInflight> cluster_wr_inflight_reg[NUM_CLUSTERS];
    std::deque<ClusterInflight> cluster_rd_inflight_reg[NUM_CLUSTERS];
    unsigned cl_wr_inflight_cnt_reg[NUM_CLUSTERS] = {};
    unsigned cl_rd_inflight_cnt_reg[NUM_CLUSTERS] = {};

    // Cluster AXI send registers
    bool     cl_aw_send_valid_reg[NUM_CLUSTERS] = {};
    uint32_t cl_aw_send_addr_reg[NUM_CLUSTERS] = {};
    bool     cl_w_send_valid_reg[NUM_CLUSTERS] = {};
    sc_biguint<kClAxiDataWidth> cl_w_send_data_reg[NUM_CLUSTERS];
    uint8_t  cl_w_send_strb_reg[NUM_CLUSTERS] = {};
    bool     cl_ar_send_valid_reg[NUM_CLUSTERS] = {};
    uint32_t cl_ar_send_addr_reg[NUM_CLUSTERS] = {};

    // Previous-cycle ready tracking (prevents double-capture in SC_CTHREAD)
    bool dma_aw_prev_ready_reg = false;
    bool dma_w_prev_ready_reg = false;
    bool dma_ar_prev_ready_reg = false;
    bool nlu_aw_prev_ready_reg = false;
    bool nlu_w_prev_ready_reg = false;
    bool nlu_ar_prev_ready_reg = false;

    // Per-cluster B/R prev_ready (for response collection from clusters)
    bool cl_b_prev_ready_reg[NUM_CLUSTERS] = {};
    bool cl_r_prev_ready_reg[NUM_CLUSTERS] = {};

    // Per-cluster prev_valid tracking for master-side send registers.
    // Same SC_CTHREAD timing issue as DmaEngine: the slave (FakeClusterSpm)
    // pre-asserts ready in its idle loop; clearing the send register on
    // the first cycle we drive valid would let the request vanish before
    // the slave samples it.  We only clear when prev_valid AND ready.
    bool cl_aw_prev_valid_reg[NUM_CLUSTERS] = {};
    bool cl_w_prev_valid_reg[NUM_CLUSTERS] = {};
    bool cl_ar_prev_valid_reg[NUM_CLUSTERS] = {};

    // B/R retire send registers (driving responses back to DMA/NLU)
    bool    dma_b_send_valid_reg = false;
    uint8_t dma_b_send_resp_reg = 0;
    bool    dma_r_send_valid_reg = false;
    sc_biguint<kClAxiDataWidth> dma_r_send_data_reg{0};
    uint8_t dma_r_send_resp_reg = 0;
    bool    nlu_b_send_valid_reg = false;
    uint8_t nlu_b_send_resp_reg = 0;
    bool    nlu_r_send_valid_reg = false;
    sc_biguint<kClAxiDataWidth> nlu_r_send_data_reg{0};
    uint8_t nlu_r_send_resp_reg = 0;

    // ========================================================================
    // PMU counters
    // ========================================================================

    uint64_t pmu_dma_aw_accept_cnt_reg = 0;
    uint64_t pmu_dma_w_accept_cnt_reg = 0;
    uint64_t pmu_dma_ar_accept_cnt_reg = 0;
    uint64_t pmu_dma_b_retire_cnt_reg = 0;
    uint64_t pmu_dma_r_retire_cnt_reg = 0;
    uint64_t pmu_nlu_aw_accept_cnt_reg = 0;
    uint64_t pmu_nlu_w_accept_cnt_reg = 0;
    uint64_t pmu_nlu_ar_accept_cnt_reg = 0;
    uint64_t pmu_nlu_b_retire_cnt_reg = 0;
    uint64_t pmu_nlu_r_retire_cnt_reg = 0;
    uint64_t pmu_wr_rr_grant_dma_cnt_reg = 0;
    uint64_t pmu_wr_rr_grant_nlu_cnt_reg = 0;
    uint64_t pmu_rd_rr_grant_dma_cnt_reg = 0;
    uint64_t pmu_rd_rr_grant_nlu_cnt_reg = 0;
    uint64_t pmu_cl_wr_refill_cnt_reg[NUM_CLUSTERS] = {};
    uint64_t pmu_cl_wr_slot_busy_cnt_reg[NUM_CLUSTERS] = {};
    uint64_t pmu_cl_wr_inflight_hwm_reg[NUM_CLUSTERS] = {};
    uint64_t pmu_cl_rd_refill_cnt_reg[NUM_CLUSTERS] = {};
    uint64_t pmu_cl_rd_slot_busy_cnt_reg[NUM_CLUSTERS] = {};
    uint64_t pmu_cl_rd_inflight_hwm_reg[NUM_CLUSTERS] = {};

    // ========================================================================
    // Reset helper
    // ========================================================================

    void reset_all_outputs() {
        // DMA slave
        s_dma_axi_aw_ready_o.write(false);
        s_dma_axi_w_ready_o.write(false);
        s_dma_axi_b_valid_o.write(false);
        s_dma_axi_b_resp_o.write(0);
        s_dma_axi_ar_ready_o.write(false);
        s_dma_axi_r_valid_o.write(false);
        s_dma_axi_r_data_o.write(0);
        s_dma_axi_r_resp_o.write(0);
        // NLU slave
        s_nlu_axi_aw_ready_o.write(false);
        s_nlu_axi_w_ready_o.write(false);
        s_nlu_axi_b_valid_o.write(false);
        s_nlu_axi_b_resp_o.write(0);
        s_nlu_axi_ar_ready_o.write(false);
        s_nlu_axi_r_valid_o.write(false);
        s_nlu_axi_r_data_o.write(0);
        s_nlu_axi_r_resp_o.write(0);
        // Per-cluster egress
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

    void reset_internal_state() {
        dma_aw_fifo_reg.clear();
        dma_w_fifo_reg.clear();
        dma_ar_fifo_reg.clear();
        dma_wr_req_fifo_reg.clear();
        dma_rd_req_fifo_reg.clear();
        nlu_aw_fifo_reg.clear();
        nlu_w_fifo_reg.clear();
        nlu_ar_fifo_reg.clear();
        nlu_wr_req_fifo_reg.clear();
        nlu_rd_req_fifo_reg.clear();
        dma_wr_rob_reg.clear();
        dma_rd_rob_reg.clear();
        nlu_wr_rob_reg.clear();
        nlu_rd_rob_reg.clear();
        dma_wr_seq_reg = 0;
        dma_wr_retire_head_seq_reg = 0;
        dma_rd_seq_reg = 0;
        dma_rd_retire_head_seq_reg = 0;
        nlu_wr_seq_reg = 0;
        nlu_wr_retire_head_seq_reg = 0;
        nlu_rd_seq_reg = 0;
        nlu_rd_retire_head_seq_reg = 0;
        wr_rr_last_grant_reg = Requester::NLU;
        rd_rr_last_grant_reg = Requester::NLU;
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            cluster_wr_issue_fifo_reg[c].clear();
            cluster_rd_issue_fifo_reg[c].clear();
            cluster_wr_inflight_reg[c].clear();
            cluster_rd_inflight_reg[c].clear();
            cl_wr_inflight_cnt_reg[c] = 0;
            cl_rd_inflight_cnt_reg[c] = 0;
            cl_aw_send_valid_reg[c] = false;
            cl_w_send_valid_reg[c] = false;
            cl_ar_send_valid_reg[c] = false;
            pmu_cl_wr_refill_cnt_reg[c] = 0;
            pmu_cl_wr_slot_busy_cnt_reg[c] = 0;
            pmu_cl_wr_inflight_hwm_reg[c] = 0;
            pmu_cl_rd_refill_cnt_reg[c] = 0;
            pmu_cl_rd_slot_busy_cnt_reg[c] = 0;
            pmu_cl_rd_inflight_hwm_reg[c] = 0;
        }
        dma_aw_prev_ready_reg = false;
        dma_w_prev_ready_reg = false;
        dma_ar_prev_ready_reg = false;
        nlu_aw_prev_ready_reg = false;
        nlu_w_prev_ready_reg = false;
        nlu_ar_prev_ready_reg = false;
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            cl_b_prev_ready_reg[c] = false;
            cl_r_prev_ready_reg[c] = false;
            cl_aw_prev_valid_reg[c] = false;
            cl_w_prev_valid_reg[c] = false;
            cl_ar_prev_valid_reg[c] = false;
        }
        dma_b_send_valid_reg = false;
        dma_r_send_valid_reg = false;
        nlu_b_send_valid_reg = false;
        nlu_r_send_valid_reg = false;
    }

    // ========================================================================
    // Main sequential process
    // ========================================================================

    void seq_process() {
        // --- Reset ---
        reset_all_outputs();
        reset_internal_state();
        wait();

        // --- Main loop ---
        while (true) {
            reset_all_outputs();

            // ================================================================
            // Block 1: DMA AXI ingress capture
            // ================================================================
            capture_dma_ingress();

            // ================================================================
            // Block 2: NLU AXI ingress capture
            // ================================================================
            capture_nlu_ingress();

            // ================================================================
            // Block 1.5 / 2.5: AW+W merge for both requesters
            // ================================================================
            merge_dma_aw_w();
            merge_nlu_aw_w();

            // ================================================================
            // Block 3: RR requester arbitration → classify + issue queue
            // ================================================================
            rr_arbitrate_write();
            rr_arbitrate_read();

            // ================================================================
            // Block 4: Collect cluster responses first so issue sees the
            // newest inflight counters in the same clock edge.
            // ================================================================
            for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
                collect_cluster_write_resp(c);
                collect_cluster_read_resp(c);
            }

            // ================================================================
            // Block 5: Per-cluster AXI service
            // ================================================================
            for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
                issue_cluster_write(c);
                issue_cluster_read(c, true);
            }

            drive_cluster_outputs();

            // ================================================================
            // Block 6: Requester retire
            // ================================================================
            retire_dma_write_resp();
            retire_dma_read_resp();
            retire_nlu_write_resp();
            retire_nlu_read_resp();

            wait();
        }
    }

    // ========================================================================
    // Block 1: DMA AXI ingress capture
    // ========================================================================

    void capture_dma_ingress() {
        // AW capture — only commit when valid AND we previously advertised ready
        bool aw_can_accept = dma_aw_fifo_reg.size() < kIngressDepth;
        if (s_dma_axi_aw_valid_i.read() && dma_aw_prev_ready_reg) {
            dma_aw_fifo_reg.push_back({s_dma_axi_aw_addr_i.read().to_uint()});
            pmu_dma_aw_accept_cnt_reg++;
        }
        s_dma_axi_aw_ready_o.write(aw_can_accept);
        dma_aw_prev_ready_reg = aw_can_accept;

        // W capture
        bool w_can_accept = dma_w_fifo_reg.size() < kIngressDepth;
        if (s_dma_axi_w_valid_i.read() && dma_w_prev_ready_reg) {
            dma_w_fifo_reg.push_back({s_dma_axi_w_data_i.read(),
                                      static_cast<uint8_t>(s_dma_axi_w_strb_i.read().to_uint())});
            pmu_dma_w_accept_cnt_reg++;
        }
        s_dma_axi_w_ready_o.write(w_can_accept);
        dma_w_prev_ready_reg = w_can_accept;

        // AR capture
        bool ar_can_accept = dma_ar_fifo_reg.size() < kIngressDepth;
        if (s_dma_axi_ar_valid_i.read() && dma_ar_prev_ready_reg) {
            dma_ar_fifo_reg.push_back({s_dma_axi_ar_addr_i.read().to_uint()});
            pmu_dma_ar_accept_cnt_reg++;
        }
        s_dma_axi_ar_ready_o.write(ar_can_accept);
        dma_ar_prev_ready_reg = ar_can_accept;
    }

    // ========================================================================
    // Block 2: NLU AXI ingress capture
    // ========================================================================

    void capture_nlu_ingress() {
        bool aw_can_accept = nlu_aw_fifo_reg.size() < kIngressDepth;
        if (s_nlu_axi_aw_valid_i.read() && nlu_aw_prev_ready_reg) {
            nlu_aw_fifo_reg.push_back({s_nlu_axi_aw_addr_i.read().to_uint()});
            pmu_nlu_aw_accept_cnt_reg++;
        }
        s_nlu_axi_aw_ready_o.write(aw_can_accept);
        nlu_aw_prev_ready_reg = aw_can_accept;

        bool w_can_accept = nlu_w_fifo_reg.size() < kIngressDepth;
        if (s_nlu_axi_w_valid_i.read() && nlu_w_prev_ready_reg) {
            nlu_w_fifo_reg.push_back({s_nlu_axi_w_data_i.read(),
                                      static_cast<uint8_t>(s_nlu_axi_w_strb_i.read().to_uint())});
            pmu_nlu_w_accept_cnt_reg++;
        }
        s_nlu_axi_w_ready_o.write(w_can_accept);
        nlu_w_prev_ready_reg = w_can_accept;

        bool ar_can_accept = nlu_ar_fifo_reg.size() < kIngressDepth;
        if (s_nlu_axi_ar_valid_i.read() && nlu_ar_prev_ready_reg) {
            nlu_ar_fifo_reg.push_back({s_nlu_axi_ar_addr_i.read().to_uint()});
            pmu_nlu_ar_accept_cnt_reg++;
        }
        s_nlu_axi_ar_ready_o.write(ar_can_accept);
        nlu_ar_prev_ready_reg = ar_can_accept;
    }

    // ========================================================================
    // AW+W merge
    // ========================================================================

    void merge_dma_aw_w() {
        if (!dma_aw_fifo_reg.empty() && !dma_w_fifo_reg.empty() &&
            dma_wr_req_fifo_reg.size() < kIngressDepth) {
            auto& aw = dma_aw_fifo_reg.front();
            auto& w  = dma_w_fifo_reg.front();
            InternalReq req;
            req.owner = Requester::DMA;
            req.write = true;
            req.cluster_id = decode_cluster_id(aw.addr);
            req.local_addr = decode_local_addr(aw.addr);
            req.data = w.data;
            req.strb = w.strb;
            req.seq = dma_wr_seq_reg++;
            dma_wr_req_fifo_reg.push_back(req);
            // Allocate ROB entry
            dma_wr_rob_reg.push_back({true, false, sc_biguint<kClAxiDataWidth>(0), 0});
            dma_aw_fifo_reg.pop_front();
            dma_w_fifo_reg.pop_front();
        }

        // DMA read merge (AR only)
        if (!dma_ar_fifo_reg.empty() && dma_rd_req_fifo_reg.size() < kIngressDepth) {
            auto& ar = dma_ar_fifo_reg.front();
            InternalReq req;
            req.owner = Requester::DMA;
            req.write = false;
            req.cluster_id = decode_cluster_id(ar.addr);
            req.local_addr = decode_local_addr(ar.addr);
            req.seq = dma_rd_seq_reg++;
            dma_rd_req_fifo_reg.push_back(req);
            dma_rd_rob_reg.push_back({true, false, sc_biguint<kClAxiDataWidth>(0), 0});
            dma_ar_fifo_reg.pop_front();
        }
    }

    void merge_nlu_aw_w() {
        if (!nlu_aw_fifo_reg.empty() && !nlu_w_fifo_reg.empty() &&
            nlu_wr_req_fifo_reg.size() < kIngressDepth) {
            auto& aw = nlu_aw_fifo_reg.front();
            auto& w  = nlu_w_fifo_reg.front();
            InternalReq req;
            req.owner = Requester::NLU;
            req.write = true;
            req.cluster_id = decode_cluster_id(aw.addr);
            req.local_addr = decode_local_addr(aw.addr);
            req.data = w.data;
            req.strb = w.strb;
            req.seq = nlu_wr_seq_reg++;
            nlu_wr_req_fifo_reg.push_back(req);
            nlu_wr_rob_reg.push_back({true, false, sc_biguint<kClAxiDataWidth>(0), 0});
            nlu_aw_fifo_reg.pop_front();
            nlu_w_fifo_reg.pop_front();
        }

        if (!nlu_ar_fifo_reg.empty() && nlu_rd_req_fifo_reg.size() < kIngressDepth) {
            auto& ar = nlu_ar_fifo_reg.front();
            InternalReq req;
            req.owner = Requester::NLU;
            req.write = false;
            req.cluster_id = decode_cluster_id(ar.addr);
            req.local_addr = decode_local_addr(ar.addr);
            req.seq = nlu_rd_seq_reg++;
            nlu_rd_req_fifo_reg.push_back(req);
            nlu_rd_rob_reg.push_back({true, false, sc_biguint<kClAxiDataWidth>(0), 0});
            nlu_ar_fifo_reg.pop_front();
        }
    }

    // ========================================================================
    // Block 3: RR arbitration
    // ========================================================================

    void rr_arbitrate_write() {
        bool dma_has = !dma_wr_req_fifo_reg.empty();
        bool nlu_has = !nlu_wr_req_fifo_reg.empty();
        if (!dma_has && !nlu_has) return;

        Requester preferred = (wr_rr_last_grant_reg == Requester::DMA) ?
                              Requester::NLU : Requester::DMA;

        InternalReq* winner = nullptr;
        std::deque<InternalReq>* winner_fifo = nullptr;
        Requester winner_id = Requester::DMA;

        // Try preferred first
        if (preferred == Requester::DMA && dma_has) {
            auto& req = dma_wr_req_fifo_reg.front();
            if (cluster_wr_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                winner = &req;
                winner_fifo = &dma_wr_req_fifo_reg;
                winner_id = Requester::DMA;
            }
        } else if (preferred == Requester::NLU && nlu_has) {
            auto& req = nlu_wr_req_fifo_reg.front();
            if (cluster_wr_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                winner = &req;
                winner_fifo = &nlu_wr_req_fifo_reg;
                winner_id = Requester::NLU;
            }
        }

        // If preferred couldn't go, try other
        if (!winner) {
            if (preferred != Requester::DMA && dma_has) {
                auto& req = dma_wr_req_fifo_reg.front();
                if (cluster_wr_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                    winner = &req;
                    winner_fifo = &dma_wr_req_fifo_reg;
                    winner_id = Requester::DMA;
                }
            }
            if (!winner && preferred != Requester::NLU && nlu_has) {
                auto& req = nlu_wr_req_fifo_reg.front();
                if (cluster_wr_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                    winner = &req;
                    winner_fifo = &nlu_wr_req_fifo_reg;
                    winner_id = Requester::NLU;
                }
            }
        }

        if (winner) {
            cluster_wr_issue_fifo_reg[winner->cluster_id].push_back(*winner);
            winner_fifo->pop_front();
            wr_rr_last_grant_reg = winner_id;
            if (winner_id == Requester::DMA) pmu_wr_rr_grant_dma_cnt_reg++;
            else                              pmu_wr_rr_grant_nlu_cnt_reg++;
        }
    }

    void rr_arbitrate_read() {
        bool dma_has = !dma_rd_req_fifo_reg.empty();
        bool nlu_has = !nlu_rd_req_fifo_reg.empty();
        if (!dma_has && !nlu_has) return;

        Requester preferred = (rd_rr_last_grant_reg == Requester::DMA) ?
                              Requester::NLU : Requester::DMA;

        InternalReq* winner = nullptr;
        std::deque<InternalReq>* winner_fifo = nullptr;
        Requester winner_id = Requester::DMA;

        if (preferred == Requester::DMA && dma_has) {
            auto& req = dma_rd_req_fifo_reg.front();
            if (cluster_rd_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                winner = &req;
                winner_fifo = &dma_rd_req_fifo_reg;
                winner_id = Requester::DMA;
            }
        } else if (preferred == Requester::NLU && nlu_has) {
            auto& req = nlu_rd_req_fifo_reg.front();
            if (cluster_rd_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                winner = &req;
                winner_fifo = &nlu_rd_req_fifo_reg;
                winner_id = Requester::NLU;
            }
        }

        if (!winner) {
            if (preferred != Requester::DMA && dma_has) {
                auto& req = dma_rd_req_fifo_reg.front();
                if (cluster_rd_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                    winner = &req;
                    winner_fifo = &dma_rd_req_fifo_reg;
                    winner_id = Requester::DMA;
                }
            }
            if (!winner && preferred != Requester::NLU && nlu_has) {
                auto& req = nlu_rd_req_fifo_reg.front();
                if (cluster_rd_issue_fifo_reg[req.cluster_id].size() < kIngressDepth) {
                    winner = &req;
                    winner_fifo = &nlu_rd_req_fifo_reg;
                    winner_id = Requester::NLU;
                }
            }
        }

        if (winner) {
            cluster_rd_issue_fifo_reg[winner->cluster_id].push_back(*winner);
            winner_fifo->pop_front();
            rd_rr_last_grant_reg = winner_id;
            if (winner_id == Requester::DMA) pmu_rd_rr_grant_dma_cnt_reg++;
            else                              pmu_rd_rr_grant_nlu_cnt_reg++;
        }
    }

    // ========================================================================
    // Block 4: Per-cluster AXI issue
    // ========================================================================

    void issue_cluster_write(unsigned c) {
        const bool cur_aw_valid = cl_aw_send_valid_reg[c];
        const uint32_t cur_aw_addr = cl_aw_send_addr_reg[c];
        const bool cur_w_valid = cl_w_send_valid_reg[c];
        const sc_biguint<kClAxiDataWidth> cur_w_data = cl_w_send_data_reg[c];
        const uint8_t cur_w_strb = cl_w_send_strb_reg[c];

        const bool aw_done = cl_aw_prev_valid_reg[c] && m_cl_data_aw_ready_i[c].read();
        const bool w_done = cl_w_prev_valid_reg[c] && m_cl_data_w_ready_i[c].read();

        bool next_aw_valid = cur_aw_valid && !aw_done;
        uint32_t next_aw_addr = cur_aw_addr;
        bool next_w_valid = cur_w_valid && !w_done;
        sc_biguint<kClAxiDataWidth> next_w_data = cur_w_data;
        uint8_t next_w_strb = cur_w_strb;

        const bool slots_busy = next_aw_valid || next_w_valid;
        const bool has_issue_req = !cluster_wr_issue_fifo_reg[c].empty();

        if (has_issue_req && (cl_wr_inflight_cnt_reg[c] < kMaxOutstanding) && slots_busy) {
            pmu_cl_wr_slot_busy_cnt_reg[c]++;
        }

        const bool can_refill = has_issue_req &&
                                !slots_busy &&
                                (cl_wr_inflight_cnt_reg[c] < kMaxOutstanding);

        if (can_refill) {
            const InternalReq req = cluster_wr_issue_fifo_reg[c].front();
            next_aw_valid = true;
            next_aw_addr = req.local_addr;
            next_w_valid = true;
            next_w_data = req.data;
            next_w_strb = req.strb;

            ClusterInflight inf;
            inf.owner = req.owner;
            inf.write = true;
            inf.rob_seq = req.seq;
            cluster_wr_inflight_reg[c].push_back(inf);
            cl_wr_inflight_cnt_reg[c]++;
            cluster_wr_issue_fifo_reg[c].pop_front();
            pmu_cl_wr_refill_cnt_reg[c]++;
            if (cl_wr_inflight_cnt_reg[c] > pmu_cl_wr_inflight_hwm_reg[c]) {
                pmu_cl_wr_inflight_hwm_reg[c] = cl_wr_inflight_cnt_reg[c];
            }
        }

        cl_aw_send_valid_reg[c] = next_aw_valid;
        cl_aw_send_addr_reg[c] = next_aw_addr;
        cl_w_send_valid_reg[c] = next_w_valid;
        cl_w_send_data_reg[c] = next_w_data;
        cl_w_send_strb_reg[c] = next_w_strb;
        cl_aw_prev_valid_reg[c] = next_aw_valid;
        cl_w_prev_valid_reg[c] = next_w_valid;

        if (next_aw_valid) {
            m_cl_data_aw_valid_o[c].write(true);
            m_cl_data_aw_addr_o[c].write(next_aw_addr);
        }
        if (next_w_valid) {
            m_cl_data_w_valid_o[c].write(true);
            m_cl_data_w_data_o[c].write(next_w_data);
            m_cl_data_w_strb_o[c].write(next_w_strb);
        }
    }

    void issue_cluster_read(unsigned c, bool allow_refill) {
        const bool cur_ar_valid = cl_ar_send_valid_reg[c];
        const uint32_t cur_ar_addr = cl_ar_send_addr_reg[c];
        const bool ar_done = cl_ar_prev_valid_reg[c] && m_cl_data_ar_ready_i[c].read();

        bool next_ar_valid = cur_ar_valid && !ar_done;
        uint32_t next_ar_addr = cur_ar_addr;

        const bool has_issue_req = !cluster_rd_issue_fifo_reg[c].empty();

        if (allow_refill && has_issue_req &&
            (cl_rd_inflight_cnt_reg[c] < kMaxOutstanding) && next_ar_valid) {
            pmu_cl_rd_slot_busy_cnt_reg[c]++;
        }

        const bool can_refill = allow_refill &&
                                has_issue_req &&
                                !next_ar_valid &&
                                (cl_rd_inflight_cnt_reg[c] < kMaxOutstanding);

        if (can_refill) {
            const InternalReq req = cluster_rd_issue_fifo_reg[c].front();
            next_ar_valid = true;
            next_ar_addr = req.local_addr;

            ClusterInflight inf;
            inf.owner = req.owner;
            inf.write = false;
            inf.rob_seq = req.seq;
            cluster_rd_inflight_reg[c].push_back(inf);
            cl_rd_inflight_cnt_reg[c]++;
            cluster_rd_issue_fifo_reg[c].pop_front();
            pmu_cl_rd_refill_cnt_reg[c]++;
            if (cl_rd_inflight_cnt_reg[c] > pmu_cl_rd_inflight_hwm_reg[c]) {
                pmu_cl_rd_inflight_hwm_reg[c] = cl_rd_inflight_cnt_reg[c];
            }
        }

        cl_ar_send_valid_reg[c] = next_ar_valid;
        cl_ar_send_addr_reg[c] = next_ar_addr;
        cl_ar_prev_valid_reg[c] = next_ar_valid;

        if (next_ar_valid) {
            m_cl_data_ar_valid_o[c].write(true);
            m_cl_data_ar_addr_o[c].write(next_ar_addr);
        }
    }

    // ========================================================================
    // Block 5: Drive cluster AXI outputs
    // ========================================================================

    void drive_cluster_outputs() {
        // Cluster AR is serviced in issue_cluster_read().
    }

    // ========================================================================
    // Block 6: Response collect + requester retire
    // ========================================================================

    void collect_cluster_write_resp(unsigned c) {
        bool can_accept = !cluster_wr_inflight_reg[c].empty();
        if (can_accept) m_cl_data_b_ready_o[c].write(true);
        if (m_cl_data_b_valid_i[c].read() && cl_b_prev_ready_reg[c]) {
            auto& inf = cluster_wr_inflight_reg[c].front();
            uint8_t resp = m_cl_data_b_resp_i[c].read().to_uint();
            if (inf.owner == Requester::DMA) {
                unsigned idx = inf.rob_seq - dma_wr_retire_head_seq_reg;
                if (idx < dma_wr_rob_reg.size()) {
                    dma_wr_rob_reg[idx].complete = true;
                    dma_wr_rob_reg[idx].resp = resp;
                }
            } else {
                unsigned idx = inf.rob_seq - nlu_wr_retire_head_seq_reg;
                if (idx < nlu_wr_rob_reg.size()) {
                    nlu_wr_rob_reg[idx].complete = true;
                    nlu_wr_rob_reg[idx].resp = resp;
                }
            }
            cluster_wr_inflight_reg[c].pop_front();
            cl_wr_inflight_cnt_reg[c]--;
        }
        cl_b_prev_ready_reg[c] = can_accept;
    }

    void collect_cluster_read_resp(unsigned c) {
        bool can_accept = !cluster_rd_inflight_reg[c].empty();
        if (can_accept) m_cl_data_r_ready_o[c].write(true);
        if (m_cl_data_r_valid_i[c].read() && cl_r_prev_ready_reg[c]) {
            auto& inf = cluster_rd_inflight_reg[c].front();
            auto rdata = m_cl_data_r_data_i[c].read();
            uint8_t resp = m_cl_data_r_resp_i[c].read().to_uint();
            if (inf.owner == Requester::DMA) {
                unsigned idx = inf.rob_seq - dma_rd_retire_head_seq_reg;
                if (idx < dma_rd_rob_reg.size()) {
                    dma_rd_rob_reg[idx].complete = true;
                    dma_rd_rob_reg[idx].data = rdata;
                    dma_rd_rob_reg[idx].resp = resp;
                }
            } else {
                unsigned idx = inf.rob_seq - nlu_rd_retire_head_seq_reg;
                if (idx < nlu_rd_rob_reg.size()) {
                    nlu_rd_rob_reg[idx].complete = true;
                    nlu_rd_rob_reg[idx].data = rdata;
                    nlu_rd_rob_reg[idx].resp = resp;
                }
            }
            cluster_rd_inflight_reg[c].pop_front();
            cl_rd_inflight_cnt_reg[c]--;
        }
        cl_r_prev_ready_reg[c] = can_accept;
    }

    // --- DMA B retire (send register pattern) ---
    void retire_dma_write_resp() {
        // Check if previous B was accepted
        if (dma_b_send_valid_reg && s_dma_axi_b_ready_i.read()) {
            dma_b_send_valid_reg = false;
            dma_wr_rob_reg.pop_front();
            dma_wr_retire_head_seq_reg++;
            pmu_dma_b_retire_cnt_reg++;
        }
        // Load next B into send register
        if (!dma_b_send_valid_reg && !dma_wr_rob_reg.empty() &&
            dma_wr_rob_reg.front().complete) {
            dma_b_send_valid_reg = true;
            dma_b_send_resp_reg = dma_wr_rob_reg.front().resp;
        }
        // Drive
        if (dma_b_send_valid_reg) {
            s_dma_axi_b_valid_o.write(true);
            s_dma_axi_b_resp_o.write(dma_b_send_resp_reg);
        }
    }

    // --- DMA R retire (send register pattern) ---
    void retire_dma_read_resp() {
        if (dma_r_send_valid_reg && s_dma_axi_r_ready_i.read()) {
            dma_r_send_valid_reg = false;
            dma_rd_rob_reg.pop_front();
            dma_rd_retire_head_seq_reg++;
            pmu_dma_r_retire_cnt_reg++;
        }
        if (!dma_r_send_valid_reg && !dma_rd_rob_reg.empty() &&
            dma_rd_rob_reg.front().complete) {
            dma_r_send_valid_reg = true;
            dma_r_send_data_reg = dma_rd_rob_reg.front().data;
            dma_r_send_resp_reg = dma_rd_rob_reg.front().resp;
        }
        if (dma_r_send_valid_reg) {
            s_dma_axi_r_valid_o.write(true);
            s_dma_axi_r_data_o.write(dma_r_send_data_reg);
            s_dma_axi_r_resp_o.write(dma_r_send_resp_reg);
        }
    }

    // --- NLU B retire (send register pattern) ---
    void retire_nlu_write_resp() {
        if (nlu_b_send_valid_reg && s_nlu_axi_b_ready_i.read()) {
            nlu_b_send_valid_reg = false;
            nlu_wr_rob_reg.pop_front();
            nlu_wr_retire_head_seq_reg++;
            pmu_nlu_b_retire_cnt_reg++;
        }
        if (!nlu_b_send_valid_reg && !nlu_wr_rob_reg.empty() &&
            nlu_wr_rob_reg.front().complete) {
            nlu_b_send_valid_reg = true;
            nlu_b_send_resp_reg = nlu_wr_rob_reg.front().resp;
        }
        if (nlu_b_send_valid_reg) {
            s_nlu_axi_b_valid_o.write(true);
            s_nlu_axi_b_resp_o.write(nlu_b_send_resp_reg);
        }
    }

    // --- NLU R retire (send register pattern) ---
    void retire_nlu_read_resp() {
        if (nlu_r_send_valid_reg && s_nlu_axi_r_ready_i.read()) {
            nlu_r_send_valid_reg = false;
            nlu_rd_rob_reg.pop_front();
            nlu_rd_retire_head_seq_reg++;
            pmu_nlu_r_retire_cnt_reg++;
        }
        if (!nlu_r_send_valid_reg && !nlu_rd_rob_reg.empty() &&
            nlu_rd_rob_reg.front().complete) {
            nlu_r_send_valid_reg = true;
            nlu_r_send_data_reg = nlu_rd_rob_reg.front().data;
            nlu_r_send_resp_reg = nlu_rd_rob_reg.front().resp;
        }
        if (nlu_r_send_valid_reg) {
            s_nlu_axi_r_valid_o.write(true);
            s_nlu_axi_r_data_o.write(nlu_r_send_data_reg);
            s_nlu_axi_r_resp_o.write(nlu_r_send_resp_reg);
        }
    }
};

} // namespace core
} // namespace hybridacc
