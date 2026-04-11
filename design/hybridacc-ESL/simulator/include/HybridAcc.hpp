/**
 * @file HybridAcc.hpp
 * @brief Top-level HybridAcc SoC module integrating CoreController, CmdToAhbBridge,
 *        and ComputeCluster arrays.
 *
 * External interfaces:
 *   - Host AXI4-Lite slave (32-bit control plane)
 *   - Shared DRAM AXI4 master (64-bit data)
 *   - controller_irq_o (interrupt to host)
 *
 * NLU ports are tied off internally (NUM_NLU = 0).
 */

#pragma once

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include <systemc>
#include <cstdint>
#include <memory>
#include <string>

#include "Core/CoreController.hpp"
#include "Core/CmdToAhbBridge.hpp"
#include "ComputeCluster.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {

using namespace core;

template <unsigned NUM_CLUSTERS = 1>
SC_MODULE(HybridAcc) {

    static constexpr unsigned NUM_NLU = 0;
    static constexpr unsigned kNluPorts = 1; // minimum 1 for array sizing

    // ========================================================================
    // External Ports
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

    // --- Shared DRAM AXI4 master (64-bit) ---
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

    // --- IRQ output ---
    sc_out<bool>         controller_irq_o;

    // --- Runtime flags (set before sc_start) ---
    bool core_debug = false;

    // ========================================================================
    // Submodules
    // ========================================================================

    CoreController<NUM_CLUSTERS, NUM_NLU> core_ctrl;
    std::unique_ptr<CmdToAhbBridge>   cmd_bridge[NUM_CLUSTERS];
    std::unique_ptr<ComputeCluster<>> cluster[NUM_CLUSTERS];

    // ========================================================================
    // Internal signals
    // ========================================================================

    // Core → Bridge cmd signals (per cluster)
    sc_signal<bool>         sig_cl_cmd_req_valid[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_cmd_req_write[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  sig_cl_cmd_req_addr[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  sig_cl_cmd_req_wdata[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_cmd_resp_valid[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  sig_cl_cmd_resp_rdata[NUM_CLUSTERS];

    // Bridge → Cluster AHB signals (per cluster)
    sc_signal<bool>         sig_hsel[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  sig_haddr[NUM_CLUSTERS];
    sc_signal<bool>         sig_hwrite[NUM_CLUSTERS];
    sc_signal<sc_uint<2>>   sig_htrans[NUM_CLUSTERS];
    sc_signal<sc_uint<3>>   sig_hsize[NUM_CLUSTERS];
    sc_signal<sc_uint<3>>   sig_hburst[NUM_CLUSTERS];
    sc_signal<sc_uint<4>>   sig_hprot[NUM_CLUSTERS];
    sc_signal<bool>         sig_hready_m2s[NUM_CLUSTERS]; // master→slave
    sc_signal<sc_uint<32>>  sig_hwdata[NUM_CLUSTERS];
    sc_signal<bool>         sig_hready_s2m[NUM_CLUSTERS]; // slave→master
    sc_signal<bool>         sig_hresp[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  sig_hrdata[NUM_CLUSTERS];

    // Core → Cluster data AXI (per cluster)
    sc_signal<bool>         sig_cl_data_aw_valid[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_aw_ready[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  sig_cl_data_aw_addr[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_w_valid[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_w_ready[NUM_CLUSTERS];
    sc_signal<sc_biguint<kClAxiDataWidth>> sig_cl_data_w_data[NUM_CLUSTERS];
    sc_signal<sc_uint<kClAxiDataWidth / 8>> sig_cl_data_w_strb[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_b_valid[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_b_ready[NUM_CLUSTERS];
    sc_signal<sc_uint<2>>   sig_cl_data_b_resp[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_ar_valid[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_ar_ready[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  sig_cl_data_ar_addr[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_r_valid[NUM_CLUSTERS];
    sc_signal<bool>         sig_cl_data_r_ready[NUM_CLUSTERS];
    sc_signal<sc_biguint<kClAxiDataWidth>> sig_cl_data_r_data[NUM_CLUSTERS];
    sc_signal<sc_uint<2>>   sig_cl_data_r_resp[NUM_CLUSTERS];

    // Cluster IRQ (cluster→core)
    sc_signal<bool>         sig_cluster_irq[NUM_CLUSTERS];

    // Cluster power enable
    sc_signal<bool>         sig_cluster_power_en[NUM_CLUSTERS];

    // NLU tie-off signals (internal, unused)
    sc_signal<bool>         sig_nlu_cmd_req_valid[kNluPorts];
    sc_signal<bool>         sig_nlu_cmd_req_write[kNluPorts];
    sc_signal<sc_uint<32>>  sig_nlu_cmd_req_addr[kNluPorts];
    sc_signal<sc_uint<32>>  sig_nlu_cmd_req_wdata[kNluPorts];
    sc_signal<bool>         sig_nlu_cmd_resp_valid[kNluPorts];
    sc_signal<sc_uint<32>>  sig_nlu_cmd_resp_rdata[kNluPorts];
    sc_signal<bool>         sig_nlu_irq[kNluPorts];
    sc_signal<bool>         sig_nlu_data_req_valid;
    sc_signal<bool>         sig_nlu_data_req_write;
    sc_signal<sc_uint<32>>  sig_nlu_data_req_cluster_id;
    sc_signal<sc_uint<32>>  sig_nlu_data_req_addr;
    sc_signal<sc_biguint<kClAxiDataWidth>> sig_nlu_data_req_wdata;
    sc_signal<sc_uint<kClAxiDataWidth / 8>> sig_nlu_data_req_wstrb;
    sc_signal<bool>         sig_nlu_data_req_ready;
    sc_signal<bool>         sig_nlu_data_resp_valid;
    sc_signal<sc_biguint<kClAxiDataWidth>> sig_nlu_data_resp_rdata;
    sc_signal<bool>         sig_nlu_data_resp_error;

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(HybridAcc);

    HybridAcc(sc_module_name name)
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
          controller_irq_o("controller_irq_o"),
          core_ctrl("core_ctrl"),
          sig_nlu_data_req_valid("sig_nlu_data_req_valid"),
          sig_nlu_data_req_write("sig_nlu_data_req_write"),
          sig_nlu_data_req_cluster_id("sig_nlu_data_req_cluster_id"),
          sig_nlu_data_req_addr("sig_nlu_data_req_addr"),
          sig_nlu_data_req_wdata("sig_nlu_data_req_wdata"),
          sig_nlu_data_req_wstrb("sig_nlu_data_req_wstrb"),
          sig_nlu_data_req_ready("sig_nlu_data_req_ready"),
          sig_nlu_data_resp_valid("sig_nlu_data_resp_valid"),
          sig_nlu_data_resp_rdata("sig_nlu_data_resp_rdata"),
          sig_nlu_data_resp_error("sig_nlu_data_resp_error")
    {
        // ---- CoreController wiring ----
        core_ctrl.clk(clk);
        core_ctrl.reset_n(reset_n);

        // Host AXI pass-through
        core_ctrl.s_ctrl_aw_valid_i(s_ctrl_aw_valid_i);
        core_ctrl.s_ctrl_aw_ready_o(s_ctrl_aw_ready_o);
        core_ctrl.s_ctrl_aw_addr_i(s_ctrl_aw_addr_i);
        core_ctrl.s_ctrl_w_valid_i(s_ctrl_w_valid_i);
        core_ctrl.s_ctrl_w_ready_o(s_ctrl_w_ready_o);
        core_ctrl.s_ctrl_w_data_i(s_ctrl_w_data_i);
        core_ctrl.s_ctrl_w_strb_i(s_ctrl_w_strb_i);
        core_ctrl.s_ctrl_b_valid_o(s_ctrl_b_valid_o);
        core_ctrl.s_ctrl_b_ready_i(s_ctrl_b_ready_i);
        core_ctrl.s_ctrl_b_resp_o(s_ctrl_b_resp_o);
        core_ctrl.s_ctrl_ar_valid_i(s_ctrl_ar_valid_i);
        core_ctrl.s_ctrl_ar_ready_o(s_ctrl_ar_ready_o);
        core_ctrl.s_ctrl_ar_addr_i(s_ctrl_ar_addr_i);
        core_ctrl.s_ctrl_r_valid_o(s_ctrl_r_valid_o);
        core_ctrl.s_ctrl_r_ready_i(s_ctrl_r_ready_i);
        core_ctrl.s_ctrl_r_data_o(s_ctrl_r_data_o);
        core_ctrl.s_ctrl_r_resp_o(s_ctrl_r_resp_o);

        // DRAM AXI pass-through
        core_ctrl.m_mem_axi_aw_valid_o(m_mem_axi_aw_valid_o);
        core_ctrl.m_mem_axi_aw_ready_i(m_mem_axi_aw_ready_i);
        core_ctrl.m_mem_axi_aw_addr_o(m_mem_axi_aw_addr_o);
        core_ctrl.m_mem_axi_aw_len_o(m_mem_axi_aw_len_o);
        core_ctrl.m_mem_axi_w_valid_o(m_mem_axi_w_valid_o);
        core_ctrl.m_mem_axi_w_ready_i(m_mem_axi_w_ready_i);
        core_ctrl.m_mem_axi_w_data_o(m_mem_axi_w_data_o);
        core_ctrl.m_mem_axi_w_strb_o(m_mem_axi_w_strb_o);
        core_ctrl.m_mem_axi_w_last_o(m_mem_axi_w_last_o);
        core_ctrl.m_mem_axi_b_valid_i(m_mem_axi_b_valid_i);
        core_ctrl.m_mem_axi_b_ready_o(m_mem_axi_b_ready_o);
        core_ctrl.m_mem_axi_b_resp_i(m_mem_axi_b_resp_i);
        core_ctrl.m_mem_axi_ar_valid_o(m_mem_axi_ar_valid_o);
        core_ctrl.m_mem_axi_ar_ready_i(m_mem_axi_ar_ready_i);
        core_ctrl.m_mem_axi_ar_addr_o(m_mem_axi_ar_addr_o);
        core_ctrl.m_mem_axi_ar_len_o(m_mem_axi_ar_len_o);
        core_ctrl.m_mem_axi_r_valid_i(m_mem_axi_r_valid_i);
        core_ctrl.m_mem_axi_r_ready_o(m_mem_axi_r_ready_o);
        core_ctrl.m_mem_axi_r_data_i(m_mem_axi_r_data_i);
        core_ctrl.m_mem_axi_r_resp_i(m_mem_axi_r_resp_i);
        core_ctrl.m_mem_axi_r_last_i(m_mem_axi_r_last_i);

        // IRQ
        core_ctrl.controller_irq_o(controller_irq_o);

        // NLU tie-off (all inactive)
        for (unsigned n = 0; n < kNluPorts; ++n) {
            core_ctrl.nlu_cmd_req_valid_o[n](sig_nlu_cmd_req_valid[n]);
            core_ctrl.nlu_cmd_req_write_o[n](sig_nlu_cmd_req_write[n]);
            core_ctrl.nlu_cmd_req_addr_o[n](sig_nlu_cmd_req_addr[n]);
            core_ctrl.nlu_cmd_req_wdata_o[n](sig_nlu_cmd_req_wdata[n]);
            core_ctrl.nlu_cmd_resp_valid_i[n](sig_nlu_cmd_resp_valid[n]);
            core_ctrl.nlu_cmd_resp_rdata_i[n](sig_nlu_cmd_resp_rdata[n]);
            core_ctrl.nlu_irq_i[n](sig_nlu_irq[n]);
        }
        core_ctrl.nlu_data_req_valid_i(sig_nlu_data_req_valid);
        core_ctrl.nlu_data_req_write_i(sig_nlu_data_req_write);
        core_ctrl.nlu_data_req_cluster_id_i(sig_nlu_data_req_cluster_id);
        core_ctrl.nlu_data_req_addr_i(sig_nlu_data_req_addr);
        core_ctrl.nlu_data_req_wdata_i(sig_nlu_data_req_wdata);
        core_ctrl.nlu_data_req_wstrb_i(sig_nlu_data_req_wstrb);
        core_ctrl.nlu_data_req_ready_o(sig_nlu_data_req_ready);
        core_ctrl.nlu_data_resp_valid_o(sig_nlu_data_resp_valid);
        core_ctrl.nlu_data_resp_rdata_o(sig_nlu_data_resp_rdata);
        core_ctrl.nlu_data_resp_error_o(sig_nlu_data_resp_error);

        // ---- Per-cluster wiring ----
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            std::string bridge_name = "cmd_bridge_" + std::to_string(c);
            std::string cluster_name = "cluster_" + std::to_string(c);
            cmd_bridge[c] = std::make_unique<CmdToAhbBridge>(bridge_name.c_str());
            cluster[c]    = std::make_unique<ComputeCluster<>>(cluster_name.c_str(), NetWorkOnChipConfig{4, 4});

            // Core → internal cmd signals
            core_ctrl.cl_cmd_req_valid_o[c](sig_cl_cmd_req_valid[c]);
            core_ctrl.cl_cmd_req_write_o[c](sig_cl_cmd_req_write[c]);
            core_ctrl.cl_cmd_req_addr_o[c](sig_cl_cmd_req_addr[c]);
            core_ctrl.cl_cmd_req_wdata_o[c](sig_cl_cmd_req_wdata[c]);
            core_ctrl.cl_cmd_resp_valid_i[c](sig_cl_cmd_resp_valid[c]);
            core_ctrl.cl_cmd_resp_rdata_i[c](sig_cl_cmd_resp_rdata[c]);

            // Core → internal data AXI signals
            core_ctrl.m_cl_data_aw_valid_o[c](sig_cl_data_aw_valid[c]);
            core_ctrl.m_cl_data_aw_ready_i[c](sig_cl_data_aw_ready[c]);
            core_ctrl.m_cl_data_aw_addr_o[c](sig_cl_data_aw_addr[c]);
            core_ctrl.m_cl_data_w_valid_o[c](sig_cl_data_w_valid[c]);
            core_ctrl.m_cl_data_w_ready_i[c](sig_cl_data_w_ready[c]);
            core_ctrl.m_cl_data_w_data_o[c](sig_cl_data_w_data[c]);
            core_ctrl.m_cl_data_w_strb_o[c](sig_cl_data_w_strb[c]);
            core_ctrl.m_cl_data_b_valid_i[c](sig_cl_data_b_valid[c]);
            core_ctrl.m_cl_data_b_ready_o[c](sig_cl_data_b_ready[c]);
            core_ctrl.m_cl_data_b_resp_i[c](sig_cl_data_b_resp[c]);
            core_ctrl.m_cl_data_ar_valid_o[c](sig_cl_data_ar_valid[c]);
            core_ctrl.m_cl_data_ar_ready_i[c](sig_cl_data_ar_ready[c]);
            core_ctrl.m_cl_data_ar_addr_o[c](sig_cl_data_ar_addr[c]);
            core_ctrl.m_cl_data_r_valid_i[c](sig_cl_data_r_valid[c]);
            core_ctrl.m_cl_data_r_ready_o[c](sig_cl_data_r_ready[c]);
            core_ctrl.m_cl_data_r_data_i[c](sig_cl_data_r_data[c]);
            core_ctrl.m_cl_data_r_resp_i[c](sig_cl_data_r_resp[c]);

            // Cluster IRQ → Core
            core_ctrl.cluster_irq_i[c](sig_cluster_irq[c]);

            // ---- CmdToAhbBridge wiring ----
            cmd_bridge[c]->clk(clk);
            cmd_bridge[c]->reset_n(reset_n);

            // Bridge ← Core cmd
            cmd_bridge[c]->cmd_req_valid(sig_cl_cmd_req_valid[c]);
            cmd_bridge[c]->cmd_req_write(sig_cl_cmd_req_write[c]);
            cmd_bridge[c]->cmd_req_addr(sig_cl_cmd_req_addr[c]);
            cmd_bridge[c]->cmd_req_wdata(sig_cl_cmd_req_wdata[c]);
            cmd_bridge[c]->cmd_resp_valid(sig_cl_cmd_resp_valid[c]);
            cmd_bridge[c]->cmd_resp_rdata(sig_cl_cmd_resp_rdata[c]);

            // Bridge → Cluster AHB
            cmd_bridge[c]->hsel_o(sig_hsel[c]);
            cmd_bridge[c]->haddr_o(sig_haddr[c]);
            cmd_bridge[c]->hwrite_o(sig_hwrite[c]);
            cmd_bridge[c]->htrans_o(sig_htrans[c]);
            cmd_bridge[c]->hsize_o(sig_hsize[c]);
            cmd_bridge[c]->hburst_o(sig_hburst[c]);
            cmd_bridge[c]->hprot_o(sig_hprot[c]);
            cmd_bridge[c]->hready_o(sig_hready_m2s[c]);
            cmd_bridge[c]->hwdata_o(sig_hwdata[c]);
            cmd_bridge[c]->hready_i(sig_hready_s2m[c]);
            cmd_bridge[c]->hresp_i(sig_hresp[c]);
            cmd_bridge[c]->hrdata_i(sig_hrdata[c]);

            // ---- ComputeCluster wiring ----
            cluster[c]->clk(clk);
            cluster[c]->reset_n(reset_n);
            cluster[c]->power_enable_i(sig_cluster_power_en[c]);
            cluster[c]->interrupt_o(sig_cluster_irq[c]);

            // Cluster ← Bridge AHB
            cluster[c]->hsel_i(sig_hsel[c]);
            cluster[c]->haddr_i(sig_haddr[c]);
            cluster[c]->hwrite_i(sig_hwrite[c]);
            cluster[c]->htrans_i(sig_htrans[c]);
            cluster[c]->hsize_i(sig_hsize[c]);
            cluster[c]->hburst_i(sig_hburst[c]);
            cluster[c]->hprot_i(sig_hprot[c]);
            cluster[c]->hready_i(sig_hready_m2s[c]);
            cluster[c]->hwdata_i(sig_hwdata[c]);
            cluster[c]->hready_o(sig_hready_s2m[c]);
            cluster[c]->hresp_o(sig_hresp[c]);
            cluster[c]->hrdata_o(sig_hrdata[c]);

            // Cluster ← Core data AXI
            cluster[c]->s_axi_awvalid_i(sig_cl_data_aw_valid[c]);
            cluster[c]->s_axi_awready_o(sig_cl_data_aw_ready[c]);
            cluster[c]->s_axi_awaddr_i(sig_cl_data_aw_addr[c]);
            cluster[c]->s_axi_wvalid_i(sig_cl_data_w_valid[c]);
            cluster[c]->s_axi_wready_o(sig_cl_data_w_ready[c]);
            cluster[c]->s_axi_wdata_i(sig_cl_data_w_data[c]);
            cluster[c]->s_axi_wstrb_i(sig_cl_data_w_strb[c]);
            cluster[c]->s_axi_bvalid_o(sig_cl_data_b_valid[c]);
            cluster[c]->s_axi_bready_i(sig_cl_data_b_ready[c]);
            cluster[c]->s_axi_bresp_o(sig_cl_data_b_resp[c]);
            cluster[c]->s_axi_arvalid_i(sig_cl_data_ar_valid[c]);
            cluster[c]->s_axi_arready_o(sig_cl_data_ar_ready[c]);
            cluster[c]->s_axi_araddr_i(sig_cl_data_ar_addr[c]);
            cluster[c]->s_axi_rvalid_o(sig_cl_data_r_valid[c]);
            cluster[c]->s_axi_rready_i(sig_cl_data_r_ready[c]);
            cluster[c]->s_axi_rdata_o(sig_cl_data_r_data[c]);
            cluster[c]->s_axi_rresp_o(sig_cl_data_r_resp[c]);
        }

        // NLU tie-off: keep resp_valid=false, irq=false
        SC_METHOD(nlu_tieoff);
        sensitive << clk.pos();
    }

    // ========================================================================
    // Debug helpers (delegated to CoreController)
    // ========================================================================

    uint32_t dsram_read_word(uint32_t byte_addr) const { return core_ctrl.dsram_read_word(byte_addr); }

    void set_cluster_power_enable(unsigned c, bool en) {
        if (c < NUM_CLUSTERS) sig_cluster_power_en[c].write(en);
    }

private:
    void nlu_tieoff() {
        for (unsigned n = 0; n < kNluPorts; ++n) {
            sig_nlu_cmd_resp_valid[n].write(false);
            sig_nlu_cmd_resp_rdata[n].write(0);
            sig_nlu_irq[n].write(false);
        }
        sig_nlu_data_req_valid.write(false);
        sig_nlu_data_req_write.write(false);
        sig_nlu_data_req_cluster_id.write(0);
        sig_nlu_data_req_addr.write(0);
        sig_nlu_data_req_wdata.write(0);
        sig_nlu_data_req_wstrb.write(0);
    }
};

} // namespace hybridacc
