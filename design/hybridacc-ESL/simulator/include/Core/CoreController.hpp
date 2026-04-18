#pragma once

/**
 * @file CoreController.hpp
 * @brief cc_core_controller — Top-level module instantiating and wiring all
 *        Core Controller submodules.
 *
 * @par Hierarchy
 *   CoreController
 *   ├── BootHostIf           (host AXI4-Lite slave control plane)
 *   ├── SectionLoader        (manifest consumer, DRAM → local SRAM)
 *   ├── Isram                (instruction SRAM)
 *   ├── DataSram             (data SRAM)
 *   ├── CoreMcu              (5-stage RV32I_Zmmul_Zicsr pipeline)
 *   ├── CmdFabric            (MMIO decoder / router / broadcast)
 *   ├── CoreLocalIrq         (MSIP / MTIP timer)
 *   ├── Plic                 (platform-level interrupt controller)
 *   ├── DmaEngine            (MMIO staging + command FIFO + transfer FSM)
 *   └── ClusterDataFabric    (DMA/NLU → per-cluster AXI4-Lite data arb)
 *
 * @par Spec reference
 *   Core.md §4.1  top-level block diagram
 *   Core.md §7    external interface summary
 */

#include <systemc>
#include <cstdint>
#include "Utils/utils.hpp"
#include "Core/Types.hpp"
#include "Core/BootHostIf.hpp"
#include "Core/SectionLoader.hpp"
#include "Core/Isram.hpp"
#include "Core/DataSram.hpp"
#include "Core/CoreMcu.hpp"
#include "Core/CmdFabric.hpp"
#include "Core/CoreLocalIrq.hpp"
#include "Core/Plic.hpp"
#include "Core/DmaEngine.hpp"
#include "Core/ClusterDataFabric.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

template <unsigned NUM_CLUSTERS = 1, unsigned NUM_NLU = 0>
SC_MODULE(CoreController) {

    // ========================================================================
    // External Ports — §7
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- §7.2 Host control AXI4-Lite slave ---
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

    // --- §7.3 Shared DRAM AXI4 master (loader / DMA, muxed by load_phase) ---
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

    // --- §7.4 Per-cluster AXI4-Lite data masters ---
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

    // --- §7.5 Per-cluster AHB-Lite command masters ---
    sc_out<bool>         cl_cmd_req_valid_o[NUM_CLUSTERS];
    sc_out<bool>         cl_cmd_req_write_o[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  cl_cmd_req_addr_o[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  cl_cmd_req_wdata_o[NUM_CLUSTERS];
    sc_in<bool>          cl_cmd_resp_valid_i[NUM_CLUSTERS];
    sc_in<sc_uint<32>>   cl_cmd_resp_rdata_i[NUM_CLUSTERS];

    // --- §7.6 Per-NLU AHB-Lite command masters ---
    static constexpr unsigned kNluPorts = NUM_NLU > 0 ? NUM_NLU : 1;
    sc_out<bool>         nlu_cmd_req_valid_o[kNluPorts];
    sc_out<bool>         nlu_cmd_req_write_o[kNluPorts];
    sc_out<sc_uint<32>>  nlu_cmd_req_addr_o[kNluPorts];
    sc_out<sc_uint<32>>  nlu_cmd_req_wdata_o[kNluPorts];
    sc_in<bool>          nlu_cmd_resp_valid_i[kNluPorts];
    sc_in<sc_uint<32>>   nlu_cmd_resp_rdata_i[kNluPorts];

    // --- §7.7 Interrupt inputs ---
    sc_in<bool>          cluster_irq_i[NUM_CLUSTERS];
    sc_in<bool>          nlu_irq_i[kNluPorts];

    // --- §7.7 IRQ output to host ---
    sc_out<bool>         controller_irq_o;

    // --- Runtime debug flag (set before sc_start) ---
    bool core_debug = false;

    // --- NLU data AXI4-Lite requester interface (into ClusterDataFabric) ---
    sc_in<bool>          nlu_data_axi_aw_valid_i;
    sc_out<bool>         nlu_data_axi_aw_ready_o;
    sc_in<sc_uint<32>>   nlu_data_axi_aw_addr_i;
    sc_in<bool>          nlu_data_axi_w_valid_i;
    sc_out<bool>         nlu_data_axi_w_ready_o;
    sc_in<sc_biguint<kClAxiDataWidth>>  nlu_data_axi_w_data_i;
    sc_in<sc_uint<kClAxiDataWidth / 8>> nlu_data_axi_w_strb_i;
    sc_out<bool>         nlu_data_axi_b_valid_o;
    sc_in<bool>          nlu_data_axi_b_ready_i;
    sc_out<sc_uint<2>>   nlu_data_axi_b_resp_o;
    sc_in<bool>          nlu_data_axi_ar_valid_i;
    sc_out<bool>         nlu_data_axi_ar_ready_o;
    sc_in<sc_uint<32>>   nlu_data_axi_ar_addr_i;
    sc_out<bool>         nlu_data_axi_r_valid_o;
    sc_in<bool>          nlu_data_axi_r_ready_i;
    sc_out<sc_biguint<kClAxiDataWidth>>  nlu_data_axi_r_data_o;
    sc_out<sc_uint<2>>   nlu_data_axi_r_resp_o;

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(CoreController);

    CoreController(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          // Host AXI
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
          // Shared DRAM AXI
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
          // NLU data AXI4-Lite requester
          nlu_data_axi_aw_valid_i("nlu_data_axi_aw_valid_i"),
          nlu_data_axi_aw_ready_o("nlu_data_axi_aw_ready_o"),
          nlu_data_axi_aw_addr_i("nlu_data_axi_aw_addr_i"),
          nlu_data_axi_w_valid_i("nlu_data_axi_w_valid_i"),
          nlu_data_axi_w_ready_o("nlu_data_axi_w_ready_o"),
          nlu_data_axi_w_data_i("nlu_data_axi_w_data_i"),
          nlu_data_axi_w_strb_i("nlu_data_axi_w_strb_i"),
          nlu_data_axi_b_valid_o("nlu_data_axi_b_valid_o"),
          nlu_data_axi_b_ready_i("nlu_data_axi_b_ready_i"),
          nlu_data_axi_b_resp_o("nlu_data_axi_b_resp_o"),
          nlu_data_axi_ar_valid_i("nlu_data_axi_ar_valid_i"),
          nlu_data_axi_ar_ready_o("nlu_data_axi_ar_ready_o"),
          nlu_data_axi_ar_addr_i("nlu_data_axi_ar_addr_i"),
          nlu_data_axi_r_valid_o("nlu_data_axi_r_valid_o"),
          nlu_data_axi_r_ready_i("nlu_data_axi_r_ready_i"),
          nlu_data_axi_r_data_o("nlu_data_axi_r_data_o"),
          nlu_data_axi_r_resp_o("nlu_data_axi_r_resp_o"),
          controller_irq_o("controller_irq_o"),
          // Submodule instances
          u_boot_host_if("u_boot_host_if"),
          u_section_loader("u_section_loader"),
          u_isram("u_isram"),
          u_dsram("u_dsram"),
          u_core_mcu("u_core_mcu"),
          u_cmd_fabric("u_cmd_fabric"),
          u_core_local_irq("u_core_local_irq"),
          u_plic("u_plic"),
          u_dma_engine("u_dma_engine"),
          u_cluster_data_fabric("u_cluster_data_fabric")
    {
        // ====================================================================
        // BootHostIf ↔ external host AXI
        // ====================================================================
        u_boot_host_if.clk(clk);
        u_boot_host_if.reset_n(reset_n);
        u_boot_host_if.s_ctrl_aw_valid_i(s_ctrl_aw_valid_i);
        u_boot_host_if.s_ctrl_aw_ready_o(s_ctrl_aw_ready_o);
        u_boot_host_if.s_ctrl_aw_addr_i(s_ctrl_aw_addr_i);
        u_boot_host_if.s_ctrl_w_valid_i(s_ctrl_w_valid_i);
        u_boot_host_if.s_ctrl_w_ready_o(s_ctrl_w_ready_o);
        u_boot_host_if.s_ctrl_w_data_i(s_ctrl_w_data_i);
        u_boot_host_if.s_ctrl_w_strb_i(s_ctrl_w_strb_i);
        u_boot_host_if.s_ctrl_b_valid_o(s_ctrl_b_valid_o);
        u_boot_host_if.s_ctrl_b_ready_i(s_ctrl_b_ready_i);
        u_boot_host_if.s_ctrl_b_resp_o(s_ctrl_b_resp_o);
        u_boot_host_if.s_ctrl_ar_valid_i(s_ctrl_ar_valid_i);
        u_boot_host_if.s_ctrl_ar_ready_o(s_ctrl_ar_ready_o);
        u_boot_host_if.s_ctrl_ar_addr_i(s_ctrl_ar_addr_i);
        u_boot_host_if.s_ctrl_r_valid_o(s_ctrl_r_valid_o);
        u_boot_host_if.s_ctrl_r_ready_i(s_ctrl_r_ready_i);
        u_boot_host_if.s_ctrl_r_data_o(s_ctrl_r_data_o);
        u_boot_host_if.s_ctrl_r_resp_o(s_ctrl_r_resp_o);

        // BootHostIf control outputs → internal signals
        u_boot_host_if.core_enable_o(sig_core_enable);
        u_boot_host_if.core_haltreq_o(sig_core_haltreq);
        u_boot_host_if.boot_addr_o(sig_boot_addr);
        u_boot_host_if.load_phase_o(sig_boot_load_phase);
        u_boot_host_if.loader_kick_o(sig_loader_kick);
        u_boot_host_if.manifest_addr_lo_o(sig_manifest_addr_lo);
        u_boot_host_if.manifest_addr_hi_o(sig_manifest_addr_hi);
        u_boot_host_if.manifest_size_o(sig_manifest_size);
        u_boot_host_if.cluster_mask_lo_o(sig_cluster_mask_lo);
        u_boot_host_if.cluster_mask_hi_o(sig_cluster_mask_hi);

        // BootHostIf status inputs
        u_boot_host_if.loader_busy_i(sig_loader_busy);
        u_boot_host_if.loader_done_i(sig_loader_done);
        u_boot_host_if.loader_status_i(sig_loader_status);
        u_boot_host_if.loader_err_code_i(sig_loader_err_code);
        u_boot_host_if.loader_err_info_i(sig_loader_err_info);
        u_boot_host_if.core_halted_i(sig_core_halted);
        u_boot_host_if.core_running_i(sig_core_running);
        u_boot_host_if.core_pc_snapshot_i(sig_retire_pc);
        u_boot_host_if.core_cause_snapshot_i(sig_core_cause);
        u_boot_host_if.plic_pending_any_i(sig_plic_pending_any);
        u_boot_host_if.fabric_last_target_i(sig_fabric_last_target);
        u_boot_host_if.fabric_last_addr_i(sig_fabric_last_addr);
        u_boot_host_if.controller_irq_o(controller_irq_o);

        // ====================================================================
        // SectionLoader ↔ BootHostIf + external DRAM + local SRAMs
        // ====================================================================
        u_section_loader.clk(clk);
        u_section_loader.reset_n(reset_n);
        u_section_loader.kick_i(sig_loader_kick);
        u_section_loader.manifest_addr_lo_i(sig_manifest_addr_lo);
        u_section_loader.manifest_addr_hi_i(sig_manifest_addr_hi);
        u_section_loader.manifest_size_i(sig_manifest_size);

        // Loader DRAM AXI read master → internal (muxed to external)
        u_section_loader.m_mem_axi_ar_valid_o(sig_ldr_mem_ar_valid);
        u_section_loader.m_mem_axi_ar_ready_i(sig_ldr_mem_ar_ready);
        u_section_loader.m_mem_axi_ar_addr_o(sig_ldr_mem_ar_addr);
        u_section_loader.m_mem_axi_ar_len_o(sig_ldr_mem_ar_len);
        u_section_loader.m_mem_axi_r_valid_i(sig_ldr_mem_r_valid);
        u_section_loader.m_mem_axi_r_ready_o(sig_ldr_mem_r_ready);
        u_section_loader.m_mem_axi_r_data_i(sig_ldr_mem_r_data);
        u_section_loader.m_mem_axi_r_resp_i(sig_ldr_mem_r_resp);
        u_section_loader.m_mem_axi_r_last_i(sig_ldr_mem_r_last);

        // Loader → Isram write port
        u_section_loader.isram_wr_en_o(sig_ldr_isram_wr_en);
        u_section_loader.isram_wr_addr_o(sig_ldr_isram_wr_addr);
        u_section_loader.isram_wr_data_o(sig_ldr_isram_wr_data);
        u_section_loader.isram_wr_strb_o(sig_ldr_isram_wr_strb);

        // Loader → DataSram write port
        u_section_loader.dsram_wr_en_o(sig_ldr_dsram_wr_en);
        u_section_loader.dsram_wr_addr_o(sig_ldr_dsram_wr_addr);
        u_section_loader.dsram_wr_data_o(sig_ldr_dsram_wr_data);
        u_section_loader.dsram_wr_strb_o(sig_ldr_dsram_wr_strb);

        u_section_loader.load_phase_o(sig_load_phase);
        u_section_loader.busy_o(sig_loader_busy);
        u_section_loader.done_o(sig_loader_done);
        u_section_loader.status_o(sig_loader_status);
        u_section_loader.err_code_o(sig_loader_err_code);
        u_section_loader.err_info_o(sig_loader_err_info);

        // ====================================================================
        // Isram
        // ====================================================================
        u_isram.clk(clk);
        u_isram.reset_n(reset_n);
        u_isram.mcu_im_valid_i(sig_mcu_if_req_valid);
        u_isram.mcu_im_addr_i(sig_mcu_if_addr);
        u_isram.mcu_im_rdata_o(sig_mcu_if_rdata);
        u_isram.loader_wr_valid_i(sig_ldr_isram_wr_en);
        u_isram.loader_wr_addr_i(sig_ldr_isram_wr_addr);
        u_isram.loader_wr_data_i(sig_ldr_isram_wr_data);
        u_isram.loader_wr_strb_i(sig_ldr_isram_wr_strb);
        u_isram.loader_wr_ready_o(sig_isram_ldr_ready);
        u_isram.load_phase_i(sig_load_phase);

        // ====================================================================
        // DataSram
        // ====================================================================
        u_dsram.clk(clk);
        u_dsram.reset_n(reset_n);
        u_dsram.mcu_dm_valid_i(sig_mcu_ls_req_valid);
        u_dsram.mcu_dm_write_i(sig_mcu_ls_req_write);
        u_dsram.mcu_dm_addr_i(sig_mcu_ls_req_addr);
        u_dsram.mcu_dm_wdata_i(sig_mcu_ls_req_wdata);
        u_dsram.mcu_dm_wstrb_i(sig_mcu_ls_req_wstrb);
        u_dsram.mcu_dm_rdata_o(sig_mcu_ls_resp_rdata);
        u_dsram.loader_wr_valid_i(sig_ldr_dsram_wr_en);
        u_dsram.loader_wr_addr_i(sig_ldr_dsram_wr_addr);
        u_dsram.loader_wr_data_i(sig_ldr_dsram_wr_data);
        u_dsram.loader_wr_strb_i(sig_ldr_dsram_wr_strb);
        u_dsram.loader_wr_ready_o(sig_dsram_ldr_ready);
        u_dsram.load_phase_i(sig_load_phase);

        // ====================================================================
        // CoreMcu ↔ SRAMs + CmdFabric + IRQs
        // ====================================================================
        u_core_mcu.clk(clk);
        u_core_mcu.reset_n(reset_n);
        // Fetch ↔ Isram
        u_core_mcu.if_req_valid_o(sig_mcu_if_req_valid);
        u_core_mcu.if_addr_o(sig_mcu_if_addr);
        u_core_mcu.if_rdata_i(sig_mcu_if_rdata);
        // Load/Store ↔ DataSram
        u_core_mcu.ls_req_valid_o(sig_mcu_ls_req_valid);
        u_core_mcu.ls_req_write_o(sig_mcu_ls_req_write);
        u_core_mcu.ls_req_addr_o(sig_mcu_ls_req_addr);
        u_core_mcu.ls_req_wdata_o(sig_mcu_ls_req_wdata);
        u_core_mcu.ls_req_wstrb_o(sig_mcu_ls_req_wstrb);
        u_core_mcu.ls_resp_rdata_i(sig_mcu_ls_resp_rdata);
        // MMIO ↔ CmdFabric
        u_core_mcu.mmio_req_valid_o(sig_mcu_mmio_req_valid);
        u_core_mcu.mmio_req_write_o(sig_mcu_mmio_req_write);
        u_core_mcu.mmio_req_addr_o(sig_mcu_mmio_req_addr);
        u_core_mcu.mmio_req_wdata_o(sig_mcu_mmio_req_wdata);
        u_core_mcu.mmio_req_wstrb_o(sig_mcu_mmio_req_wstrb);
        u_core_mcu.mmio_resp_valid_i(sig_mcu_mmio_resp_valid);
        u_core_mcu.mmio_resp_rdata_i(sig_mcu_mmio_resp_rdata);
        // IRQ inputs
        u_core_mcu.irq_meip_i(sig_meip);
        u_core_mcu.irq_msip_i(sig_msip);
        u_core_mcu.irq_mtip_i(sig_mtip);
        // Boot / control
        u_core_mcu.boot_addr_i(sig_boot_addr);
        u_core_mcu.core_enable_i(sig_core_enable);
        // Trace
        u_core_mcu.retire_valid_o(sig_retire_valid);
        u_core_mcu.retire_pc_o(sig_retire_pc);
        u_core_mcu.halted_o(sig_core_halted);

        // ====================================================================
        // CmdFabric ↔ CoreMcu + DMA + PLIC + Timer + Clusters + NLUs
        // ====================================================================
        u_cmd_fabric.clk(clk);
        u_cmd_fabric.reset_n(reset_n);
        // Core MMIO
        u_cmd_fabric.core_mmio_req_valid_i(sig_mcu_mmio_req_valid);
        u_cmd_fabric.core_mmio_req_write_i(sig_mcu_mmio_req_write);
        u_cmd_fabric.core_mmio_req_addr_i(sig_mcu_mmio_req_addr);
        u_cmd_fabric.core_mmio_req_wdata_i(sig_mcu_mmio_req_wdata);
        u_cmd_fabric.core_mmio_req_wstrb_i(sig_mcu_mmio_req_wstrb);
        u_cmd_fabric.core_mmio_resp_valid_o(sig_mcu_mmio_resp_valid);
        u_cmd_fabric.core_mmio_resp_rdata_o(sig_mcu_mmio_resp_rdata);
        // DMA MMIO route
        u_cmd_fabric.dma_mmio_req_valid_o(sig_dma_mmio_req_valid);
        u_cmd_fabric.dma_mmio_req_write_o(sig_dma_mmio_req_write);
        u_cmd_fabric.dma_mmio_req_addr_o(sig_dma_mmio_req_addr);
        u_cmd_fabric.dma_mmio_req_wdata_o(sig_dma_mmio_req_wdata);
        u_cmd_fabric.dma_mmio_resp_valid_i(sig_dma_mmio_resp_valid);
        u_cmd_fabric.dma_mmio_resp_rdata_i(sig_dma_mmio_resp_rdata);
        // PLIC MMIO route
        u_cmd_fabric.plic_mmio_req_valid_o(sig_plic_mmio_req_valid);
        u_cmd_fabric.plic_mmio_req_write_o(sig_plic_mmio_req_write);
        u_cmd_fabric.plic_mmio_req_addr_o(sig_plic_mmio_req_addr);
        u_cmd_fabric.plic_mmio_req_wdata_o(sig_plic_mmio_req_wdata);
        u_cmd_fabric.plic_mmio_resp_valid_i(sig_plic_mmio_resp_valid);
        u_cmd_fabric.plic_mmio_resp_rdata_i(sig_plic_mmio_resp_rdata);
        // Timer MMIO route
        u_cmd_fabric.timer_mmio_req_valid_o(sig_timer_mmio_req_valid);
        u_cmd_fabric.timer_mmio_req_write_o(sig_timer_mmio_req_write);
        u_cmd_fabric.timer_mmio_req_addr_o(sig_timer_mmio_req_addr);
        u_cmd_fabric.timer_mmio_req_wdata_o(sig_timer_mmio_req_wdata);
        u_cmd_fabric.timer_mmio_resp_valid_i(sig_timer_mmio_resp_valid);
        u_cmd_fabric.timer_mmio_resp_rdata_i(sig_timer_mmio_resp_rdata);
        // Per-cluster command → external
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            u_cmd_fabric.cl_cmd_req_valid_o[c](cl_cmd_req_valid_o[c]);
            u_cmd_fabric.cl_cmd_req_write_o[c](cl_cmd_req_write_o[c]);
            u_cmd_fabric.cl_cmd_req_addr_o[c](cl_cmd_req_addr_o[c]);
            u_cmd_fabric.cl_cmd_req_wdata_o[c](cl_cmd_req_wdata_o[c]);
            u_cmd_fabric.cl_cmd_resp_valid_i[c](cl_cmd_resp_valid_i[c]);
            u_cmd_fabric.cl_cmd_resp_rdata_i[c](cl_cmd_resp_rdata_i[c]);
        }
        // Per-NLU command → external
        for (unsigned n = 0; n < kNluPorts; ++n) {
            u_cmd_fabric.nlu_cmd_req_valid_o[n](nlu_cmd_req_valid_o[n]);
            u_cmd_fabric.nlu_cmd_req_write_o[n](nlu_cmd_req_write_o[n]);
            u_cmd_fabric.nlu_cmd_req_addr_o[n](nlu_cmd_req_addr_o[n]);
            u_cmd_fabric.nlu_cmd_req_wdata_o[n](nlu_cmd_req_wdata_o[n]);
            u_cmd_fabric.nlu_cmd_resp_valid_i[n](nlu_cmd_resp_valid_i[n]);
            u_cmd_fabric.nlu_cmd_resp_rdata_i[n](nlu_cmd_resp_rdata_i[n]);
        }

        // ====================================================================
        // CoreLocalIrq ↔ CmdFabric + CoreMcu
        // ====================================================================
        u_core_local_irq.clk(clk);
        u_core_local_irq.reset_n(reset_n);
        u_core_local_irq.mmio_req_valid_i(sig_timer_mmio_req_valid);
        u_core_local_irq.mmio_req_write_i(sig_timer_mmio_req_write);
        u_core_local_irq.mmio_req_addr_i(sig_timer_mmio_req_addr);
        u_core_local_irq.mmio_req_wdata_i(sig_timer_mmio_req_wdata);
        u_core_local_irq.mmio_resp_valid_o(sig_timer_mmio_resp_valid);
        u_core_local_irq.mmio_resp_rdata_o(sig_timer_mmio_resp_rdata);
        u_core_local_irq.irq_msip_o(sig_msip);
        u_core_local_irq.irq_mtip_o(sig_mtip);

        // ====================================================================
        // Plic ↔ CmdFabric + IRQ sources + CoreMcu
        // ====================================================================
        u_plic.clk(clk);
        u_plic.reset_n(reset_n);
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            u_plic.cluster_irq_i[c](cluster_irq_i[c]);
        }
        for (unsigned n = 0; n < kNluPorts; ++n) {
            u_plic.nlu_irq_i[n](nlu_irq_i[n]);
        }
        u_plic.dma_irq_i(sig_dma_irq);
        u_plic.loader_fault_i(sig_loader_fault);
        u_plic.fabric_fault_i(sig_fabric_fault);
        u_plic.meip_o(sig_meip);
        u_plic.mmio_req_valid_i(sig_plic_mmio_req_valid);
        u_plic.mmio_req_write_i(sig_plic_mmio_req_write);
        u_plic.mmio_req_addr_i(sig_plic_mmio_req_addr);
        u_plic.mmio_req_wdata_i(sig_plic_mmio_req_wdata);
        u_plic.mmio_resp_valid_o(sig_plic_mmio_resp_valid);
        u_plic.mmio_resp_rdata_o(sig_plic_mmio_resp_rdata);
        u_plic.pending_lo_o(sig_plic_pending_lo);
        u_plic.pending_hi_o(sig_plic_pending_hi);

        // ====================================================================
        // DmaEngine ↔ CmdFabric + DRAM AXI + ClusterDataFabric
        // ====================================================================
        u_dma_engine.clk(clk);
        u_dma_engine.reset_n(reset_n);
        // DMA MMIO
        u_dma_engine.mmio_req_valid_i(sig_dma_mmio_req_valid);
        u_dma_engine.mmio_req_write_i(sig_dma_mmio_req_write);
        u_dma_engine.mmio_req_addr_i(sig_dma_mmio_req_addr);
        u_dma_engine.mmio_req_wdata_i(sig_dma_mmio_req_wdata);
        u_dma_engine.mmio_resp_valid_o(sig_dma_mmio_resp_valid);
        u_dma_engine.mmio_resp_rdata_o(sig_dma_mmio_resp_rdata);
        // DMA DRAM AXI master → internal (muxed to external)
        u_dma_engine.m_mem_axi_aw_valid_o(sig_dma_mem_aw_valid);
        u_dma_engine.m_mem_axi_aw_ready_i(sig_dma_mem_aw_ready);
        u_dma_engine.m_mem_axi_aw_addr_o(sig_dma_mem_aw_addr);
        u_dma_engine.m_mem_axi_aw_len_o(sig_dma_mem_aw_len);
        u_dma_engine.m_mem_axi_w_valid_o(sig_dma_mem_w_valid);
        u_dma_engine.m_mem_axi_w_ready_i(sig_dma_mem_w_ready);
        u_dma_engine.m_mem_axi_w_data_o(sig_dma_mem_w_data);
        u_dma_engine.m_mem_axi_w_strb_o(sig_dma_mem_w_strb);
        u_dma_engine.m_mem_axi_w_last_o(sig_dma_mem_w_last);
        u_dma_engine.m_mem_axi_b_valid_i(sig_dma_mem_b_valid);
        u_dma_engine.m_mem_axi_b_ready_o(sig_dma_mem_b_ready);
        u_dma_engine.m_mem_axi_b_resp_i(sig_dma_mem_b_resp);
        u_dma_engine.m_mem_axi_ar_valid_o(sig_dma_mem_ar_valid);
        u_dma_engine.m_mem_axi_ar_ready_i(sig_dma_mem_ar_ready);
        u_dma_engine.m_mem_axi_ar_addr_o(sig_dma_mem_ar_addr);
        u_dma_engine.m_mem_axi_ar_len_o(sig_dma_mem_ar_len);
        u_dma_engine.m_mem_axi_r_valid_i(sig_dma_mem_r_valid);
        u_dma_engine.m_mem_axi_r_ready_o(sig_dma_mem_r_ready);
        u_dma_engine.m_mem_axi_r_data_i(sig_dma_mem_r_data);
        u_dma_engine.m_mem_axi_r_resp_i(sig_dma_mem_r_resp);
        u_dma_engine.m_mem_axi_r_last_i(sig_dma_mem_r_last);
        // DMA ↔ ClusterDataFabric (AXI4-Lite)
        u_dma_engine.m_cl_axi_aw_valid_o(sig_dma_cl_aw_valid);
        u_dma_engine.m_cl_axi_aw_ready_i(sig_dma_cl_aw_ready);
        u_dma_engine.m_cl_axi_aw_addr_o(sig_dma_cl_aw_addr);
        u_dma_engine.m_cl_axi_w_valid_o(sig_dma_cl_w_valid);
        u_dma_engine.m_cl_axi_w_ready_i(sig_dma_cl_w_ready);
        u_dma_engine.m_cl_axi_w_data_o(sig_dma_cl_w_data);
        u_dma_engine.m_cl_axi_w_strb_o(sig_dma_cl_w_strb);
        u_dma_engine.m_cl_axi_b_valid_i(sig_dma_cl_b_valid);
        u_dma_engine.m_cl_axi_b_ready_o(sig_dma_cl_b_ready);
        u_dma_engine.m_cl_axi_b_resp_i(sig_dma_cl_b_resp);
        u_dma_engine.m_cl_axi_ar_valid_o(sig_dma_cl_ar_valid);
        u_dma_engine.m_cl_axi_ar_ready_i(sig_dma_cl_ar_ready);
        u_dma_engine.m_cl_axi_ar_addr_o(sig_dma_cl_ar_addr);
        u_dma_engine.m_cl_axi_r_valid_i(sig_dma_cl_r_valid);
        u_dma_engine.m_cl_axi_r_ready_o(sig_dma_cl_r_ready);
        u_dma_engine.m_cl_axi_r_data_i(sig_dma_cl_r_data);
        u_dma_engine.m_cl_axi_r_resp_i(sig_dma_cl_r_resp);
        u_dma_engine.dma_irq_o(sig_dma_irq);

        // ====================================================================
        // ClusterDataFabric ↔ DmaEngine + NLU + external cluster data ports
        // ====================================================================
        u_cluster_data_fabric.clk(clk);
        u_cluster_data_fabric.reset_n(reset_n);
        // DMA requester (AXI4-Lite)
        u_cluster_data_fabric.s_dma_axi_aw_valid_i(sig_dma_cl_aw_valid);
        u_cluster_data_fabric.s_dma_axi_aw_ready_o(sig_dma_cl_aw_ready);
        u_cluster_data_fabric.s_dma_axi_aw_addr_i(sig_dma_cl_aw_addr);
        u_cluster_data_fabric.s_dma_axi_w_valid_i(sig_dma_cl_w_valid);
        u_cluster_data_fabric.s_dma_axi_w_ready_o(sig_dma_cl_w_ready);
        u_cluster_data_fabric.s_dma_axi_w_data_i(sig_dma_cl_w_data);
        u_cluster_data_fabric.s_dma_axi_w_strb_i(sig_dma_cl_w_strb);
        u_cluster_data_fabric.s_dma_axi_b_valid_o(sig_dma_cl_b_valid);
        u_cluster_data_fabric.s_dma_axi_b_ready_i(sig_dma_cl_b_ready);
        u_cluster_data_fabric.s_dma_axi_b_resp_o(sig_dma_cl_b_resp);
        u_cluster_data_fabric.s_dma_axi_ar_valid_i(sig_dma_cl_ar_valid);
        u_cluster_data_fabric.s_dma_axi_ar_ready_o(sig_dma_cl_ar_ready);
        u_cluster_data_fabric.s_dma_axi_ar_addr_i(sig_dma_cl_ar_addr);
        u_cluster_data_fabric.s_dma_axi_r_valid_o(sig_dma_cl_r_valid);
        u_cluster_data_fabric.s_dma_axi_r_ready_i(sig_dma_cl_r_ready);
        u_cluster_data_fabric.s_dma_axi_r_data_o(sig_dma_cl_r_data);
        u_cluster_data_fabric.s_dma_axi_r_resp_o(sig_dma_cl_r_resp);
        // NLU requester → external (AXI4-Lite)
        u_cluster_data_fabric.s_nlu_axi_aw_valid_i(nlu_data_axi_aw_valid_i);
        u_cluster_data_fabric.s_nlu_axi_aw_ready_o(nlu_data_axi_aw_ready_o);
        u_cluster_data_fabric.s_nlu_axi_aw_addr_i(nlu_data_axi_aw_addr_i);
        u_cluster_data_fabric.s_nlu_axi_w_valid_i(nlu_data_axi_w_valid_i);
        u_cluster_data_fabric.s_nlu_axi_w_ready_o(nlu_data_axi_w_ready_o);
        u_cluster_data_fabric.s_nlu_axi_w_data_i(nlu_data_axi_w_data_i);
        u_cluster_data_fabric.s_nlu_axi_w_strb_i(nlu_data_axi_w_strb_i);
        u_cluster_data_fabric.s_nlu_axi_b_valid_o(nlu_data_axi_b_valid_o);
        u_cluster_data_fabric.s_nlu_axi_b_ready_i(nlu_data_axi_b_ready_i);
        u_cluster_data_fabric.s_nlu_axi_b_resp_o(nlu_data_axi_b_resp_o);
        u_cluster_data_fabric.s_nlu_axi_ar_valid_i(nlu_data_axi_ar_valid_i);
        u_cluster_data_fabric.s_nlu_axi_ar_ready_o(nlu_data_axi_ar_ready_o);
        u_cluster_data_fabric.s_nlu_axi_ar_addr_i(nlu_data_axi_ar_addr_i);
        u_cluster_data_fabric.s_nlu_axi_r_valid_o(nlu_data_axi_r_valid_o);
        u_cluster_data_fabric.s_nlu_axi_r_ready_i(nlu_data_axi_r_ready_i);
        u_cluster_data_fabric.s_nlu_axi_r_data_o(nlu_data_axi_r_data_o);
        u_cluster_data_fabric.s_nlu_axi_r_resp_o(nlu_data_axi_r_resp_o);
        // Per-cluster AXI4-Lite data → external
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            u_cluster_data_fabric.m_cl_data_aw_valid_o[c](m_cl_data_aw_valid_o[c]);
            u_cluster_data_fabric.m_cl_data_aw_ready_i[c](m_cl_data_aw_ready_i[c]);
            u_cluster_data_fabric.m_cl_data_aw_addr_o[c](m_cl_data_aw_addr_o[c]);
            u_cluster_data_fabric.m_cl_data_w_valid_o[c](m_cl_data_w_valid_o[c]);
            u_cluster_data_fabric.m_cl_data_w_ready_i[c](m_cl_data_w_ready_i[c]);
            u_cluster_data_fabric.m_cl_data_w_data_o[c](m_cl_data_w_data_o[c]);
            u_cluster_data_fabric.m_cl_data_w_strb_o[c](m_cl_data_w_strb_o[c]);
            u_cluster_data_fabric.m_cl_data_b_valid_i[c](m_cl_data_b_valid_i[c]);
            u_cluster_data_fabric.m_cl_data_b_ready_o[c](m_cl_data_b_ready_o[c]);
            u_cluster_data_fabric.m_cl_data_b_resp_i[c](m_cl_data_b_resp_i[c]);
            u_cluster_data_fabric.m_cl_data_ar_valid_o[c](m_cl_data_ar_valid_o[c]);
            u_cluster_data_fabric.m_cl_data_ar_ready_i[c](m_cl_data_ar_ready_i[c]);
            u_cluster_data_fabric.m_cl_data_ar_addr_o[c](m_cl_data_ar_addr_o[c]);
            u_cluster_data_fabric.m_cl_data_r_valid_i[c](m_cl_data_r_valid_i[c]);
            u_cluster_data_fabric.m_cl_data_r_ready_o[c](m_cl_data_r_ready_o[c]);
            u_cluster_data_fabric.m_cl_data_r_data_i[c](m_cl_data_r_data_i[c]);
            u_cluster_data_fabric.m_cl_data_r_resp_i[c](m_cl_data_r_resp_i[c]);
        }

        // ====================================================================
        // Derived signals via SC_METHOD
        // ====================================================================
        SC_METHOD(comb_derived);
        sensitive << sig_loader_err_code << sig_plic_pending_lo << sig_plic_pending_hi
                  << sig_core_enable << sig_retire_valid << sig_core_halted;

        // ====================================================================
        // DRAM AXI mux: load_phase → SectionLoader, else → DmaEngine
        // ====================================================================
        SC_METHOD(mux_mem_axi);
        sensitive << sig_boot_load_phase
                  // Loader outputs
                  << sig_ldr_mem_ar_valid << sig_ldr_mem_ar_addr << sig_ldr_mem_ar_len
                  << sig_ldr_mem_r_ready
                  // DMA outputs
                  << sig_dma_mem_aw_valid << sig_dma_mem_aw_addr << sig_dma_mem_aw_len
                  << sig_dma_mem_w_valid << sig_dma_mem_w_data
                  << sig_dma_mem_w_strb << sig_dma_mem_w_last
                  << sig_dma_mem_b_ready
                  << sig_dma_mem_ar_valid << sig_dma_mem_ar_addr << sig_dma_mem_ar_len
                  << sig_dma_mem_r_ready
                  // External responses
                  << m_mem_axi_aw_ready_i << m_mem_axi_w_ready_i
                  << m_mem_axi_b_valid_i << m_mem_axi_b_resp_i
                  << m_mem_axi_ar_ready_i
                  << m_mem_axi_r_valid_i << m_mem_axi_r_data_i
                  << m_mem_axi_r_resp_i << m_mem_axi_r_last_i;
    }

    /// Propagate runtime config to sub-modules after elaboration.
    void end_of_elaboration() override {
        u_core_mcu.core_debug = core_debug;

        // Install direct SRAM read callback into the pipeline MEM stage.
        // This bypasses sc_signal delta-cycle ordering so that loads
        // read the current mem[] content within the same posedge.
        u_core_mcu.set_sram_read_cb([this](uint32_t addr) -> uint32_t {
            const uint32_t base = addr & (kDataSramBytes - 1);
            uint32_t val = 0;
            for (unsigned b = 0; b < 4; ++b) {
                if (base + b < kDataSramBytes)
                    val |= static_cast<uint32_t>(u_dsram.read_byte(base + b))
                           << (b * 8);
            }
            return val;
        });

        // Install ISRAM read callback for data-port reads from instruction RAM.
        u_core_mcu.set_isram_read_cb([this](uint32_t addr) -> uint32_t {
            const uint32_t base = addr & (kIsramBytes - 1);
            uint32_t val = 0;
            for (unsigned b = 0; b < 4; ++b) {
                if (base + b < kIsramBytes)
                    val |= static_cast<uint32_t>(u_isram.read_byte(base + b))
                           << (b * 8);
            }
            return val;
        });
    }

    /// Read a 32-bit word from Data SRAM (byte address, relative to DSRAM base).
    uint32_t dsram_read_word(uint32_t byte_addr) const {
        const uint32_t base = byte_addr & (kDataSramBytes - 1);
        uint32_t val = 0;
        for (unsigned b = 0; b < 4; ++b) {
            if (base + b < kDataSramBytes)
                val |= static_cast<uint32_t>(u_dsram.read_byte(base + b)) << (b * 8);
        }
        return val;
    }

private:
    // ========================================================================
    // Submodule instances
    // ========================================================================

    BootHostIf<NUM_CLUSTERS, NUM_NLU>      u_boot_host_if;
    SectionLoader<>                        u_section_loader;
    Isram<>                                u_isram;
    DataSram<>                             u_dsram;
    CoreMcu                                u_core_mcu;
    CmdFabric<NUM_CLUSTERS, NUM_NLU>       u_cmd_fabric;
    CoreLocalIrq                           u_core_local_irq;
    Plic<NUM_CLUSTERS, NUM_NLU>            u_plic;
    DmaEngine                              u_dma_engine;
    ClusterDataFabric<NUM_CLUSTERS>        u_cluster_data_fabric;

    // ========================================================================
    // Internal signals — SectionLoader DRAM AXI (read-only, into mux)
    // ========================================================================

    sc_signal<bool>         sig_ldr_mem_ar_valid{"sig_ldr_mem_ar_valid"};
    sc_signal<bool>         sig_ldr_mem_ar_ready{"sig_ldr_mem_ar_ready"};
    sc_signal<sc_uint<32>>  sig_ldr_mem_ar_addr{"sig_ldr_mem_ar_addr"};
    sc_signal<sc_uint<8>>   sig_ldr_mem_ar_len{"sig_ldr_mem_ar_len"};
    sc_signal<bool>         sig_ldr_mem_r_valid{"sig_ldr_mem_r_valid"};
    sc_signal<bool>         sig_ldr_mem_r_ready{"sig_ldr_mem_r_ready"};
    sc_signal<sc_biguint<kMemAxiDataWidth>> sig_ldr_mem_r_data{"sig_ldr_mem_r_data"};
    sc_signal<sc_uint<2>>   sig_ldr_mem_r_resp{"sig_ldr_mem_r_resp"};
    sc_signal<bool>         sig_ldr_mem_r_last{"sig_ldr_mem_r_last"};

    // ========================================================================
    // Internal signals — DmaEngine DRAM AXI (full, into mux)
    // ========================================================================

    sc_signal<bool>         sig_dma_mem_aw_valid{"sig_dma_mem_aw_valid"};
    sc_signal<bool>         sig_dma_mem_aw_ready{"sig_dma_mem_aw_ready"};
    sc_signal<sc_uint<32>>  sig_dma_mem_aw_addr{"sig_dma_mem_aw_addr"};
    sc_signal<sc_uint<8>>   sig_dma_mem_aw_len{"sig_dma_mem_aw_len"};
    sc_signal<bool>         sig_dma_mem_w_valid{"sig_dma_mem_w_valid"};
    sc_signal<bool>         sig_dma_mem_w_ready{"sig_dma_mem_w_ready"};
    sc_signal<sc_biguint<kMemAxiDataWidth>> sig_dma_mem_w_data{"sig_dma_mem_w_data"};
    sc_signal<sc_uint<kMemAxiDataWidth / 8>> sig_dma_mem_w_strb{"sig_dma_mem_w_strb"};
    sc_signal<bool>         sig_dma_mem_w_last{"sig_dma_mem_w_last"};
    sc_signal<bool>         sig_dma_mem_b_valid{"sig_dma_mem_b_valid"};
    sc_signal<bool>         sig_dma_mem_b_ready{"sig_dma_mem_b_ready"};
    sc_signal<sc_uint<2>>   sig_dma_mem_b_resp{"sig_dma_mem_b_resp"};
    sc_signal<bool>         sig_dma_mem_ar_valid{"sig_dma_mem_ar_valid"};
    sc_signal<bool>         sig_dma_mem_ar_ready{"sig_dma_mem_ar_ready"};
    sc_signal<sc_uint<32>>  sig_dma_mem_ar_addr{"sig_dma_mem_ar_addr"};
    sc_signal<sc_uint<8>>   sig_dma_mem_ar_len{"sig_dma_mem_ar_len"};
    sc_signal<bool>         sig_dma_mem_r_valid{"sig_dma_mem_r_valid"};
    sc_signal<bool>         sig_dma_mem_r_ready{"sig_dma_mem_r_ready"};
    sc_signal<sc_biguint<kMemAxiDataWidth>> sig_dma_mem_r_data{"sig_dma_mem_r_data"};
    sc_signal<sc_uint<2>>   sig_dma_mem_r_resp{"sig_dma_mem_r_resp"};
    sc_signal<bool>         sig_dma_mem_r_last{"sig_dma_mem_r_last"};

    // ========================================================================
    // Internal signals — BootHostIf ↔ SectionLoader
    // ========================================================================

    sc_signal<bool>         sig_loader_kick{"sig_loader_kick"};
    sc_signal<sc_uint<32>>  sig_manifest_addr_lo{"sig_manifest_addr_lo"};
    sc_signal<sc_uint<32>>  sig_manifest_addr_hi{"sig_manifest_addr_hi"};
    sc_signal<sc_uint<32>>  sig_manifest_size{"sig_manifest_size"};
    sc_signal<bool>         sig_loader_busy{"sig_loader_busy"};
    sc_signal<bool>         sig_loader_done{"sig_loader_done"};
    sc_signal<sc_uint<32>>  sig_loader_status{"sig_loader_status"};
    sc_signal<sc_uint<32>>  sig_loader_err_code{"sig_loader_err_code"};
    sc_signal<sc_uint<32>>  sig_loader_err_info{"sig_loader_err_info"};
    sc_signal<bool>         sig_load_phase{"sig_load_phase"};
    sc_signal<bool>         sig_boot_load_phase{"sig_boot_load_phase"};

    // ========================================================================
    // Internal signals — SectionLoader → SRAMs
    // ========================================================================

    sc_signal<bool>         sig_ldr_isram_wr_en{"sig_ldr_isram_wr_en"};
    sc_signal<sc_uint<32>>  sig_ldr_isram_wr_addr{"sig_ldr_isram_wr_addr"};
    sc_signal<sc_uint<32>>  sig_ldr_isram_wr_data{"sig_ldr_isram_wr_data"};
    sc_signal<sc_uint<4>>   sig_ldr_isram_wr_strb{"sig_ldr_isram_wr_strb"};
    sc_signal<bool>         sig_isram_ldr_ready{"sig_isram_ldr_ready"};

    sc_signal<bool>         sig_ldr_dsram_wr_en{"sig_ldr_dsram_wr_en"};
    sc_signal<sc_uint<32>>  sig_ldr_dsram_wr_addr{"sig_ldr_dsram_wr_addr"};
    sc_signal<sc_uint<32>>  sig_ldr_dsram_wr_data{"sig_ldr_dsram_wr_data"};
    sc_signal<sc_uint<4>>   sig_ldr_dsram_wr_strb{"sig_ldr_dsram_wr_strb"};
    sc_signal<bool>         sig_dsram_ldr_ready{"sig_dsram_ldr_ready"};

    // ========================================================================
    // Internal signals — CoreMcu ↔ SRAMs
    // ========================================================================

    sc_signal<bool>         sig_mcu_if_req_valid{"sig_mcu_if_req_valid"};
    sc_signal<sc_uint<32>>  sig_mcu_if_addr{"sig_mcu_if_addr"};
    sc_signal<sc_uint<32>>  sig_mcu_if_rdata{"sig_mcu_if_rdata"};

    sc_signal<bool>         sig_mcu_ls_req_valid{"sig_mcu_ls_req_valid"};
    sc_signal<bool>         sig_mcu_ls_req_write{"sig_mcu_ls_req_write"};
    sc_signal<sc_uint<32>>  sig_mcu_ls_req_addr{"sig_mcu_ls_req_addr"};
    sc_signal<sc_uint<32>>  sig_mcu_ls_req_wdata{"sig_mcu_ls_req_wdata"};
    sc_signal<sc_uint<4>>   sig_mcu_ls_req_wstrb{"sig_mcu_ls_req_wstrb"};
    sc_signal<sc_uint<32>>  sig_mcu_ls_resp_rdata{"sig_mcu_ls_resp_rdata"};

    // ========================================================================
    // Internal signals — CoreMcu ↔ CmdFabric
    // ========================================================================

    sc_signal<bool>         sig_mcu_mmio_req_valid{"sig_mcu_mmio_req_valid"};
    sc_signal<bool>         sig_mcu_mmio_req_write{"sig_mcu_mmio_req_write"};
    sc_signal<sc_uint<32>>  sig_mcu_mmio_req_addr{"sig_mcu_mmio_req_addr"};
    sc_signal<sc_uint<32>>  sig_mcu_mmio_req_wdata{"sig_mcu_mmio_req_wdata"};
    sc_signal<sc_uint<4>>   sig_mcu_mmio_req_wstrb{"sig_mcu_mmio_req_wstrb"};
    sc_signal<bool>         sig_mcu_mmio_resp_valid{"sig_mcu_mmio_resp_valid"};
    sc_signal<sc_uint<32>>  sig_mcu_mmio_resp_rdata{"sig_mcu_mmio_resp_rdata"};

    // ========================================================================
    // Internal signals — CmdFabric ↔ DMA MMIO
    // ========================================================================

    sc_signal<bool>         sig_dma_mmio_req_valid{"sig_dma_mmio_req_valid"};
    sc_signal<bool>         sig_dma_mmio_req_write{"sig_dma_mmio_req_write"};
    sc_signal<sc_uint<32>>  sig_dma_mmio_req_addr{"sig_dma_mmio_req_addr"};
    sc_signal<sc_uint<32>>  sig_dma_mmio_req_wdata{"sig_dma_mmio_req_wdata"};
    sc_signal<bool>         sig_dma_mmio_resp_valid{"sig_dma_mmio_resp_valid"};
    sc_signal<sc_uint<32>>  sig_dma_mmio_resp_rdata{"sig_dma_mmio_resp_rdata"};

    // ========================================================================
    // Internal signals — CmdFabric ↔ PLIC MMIO
    // ========================================================================

    sc_signal<bool>         sig_plic_mmio_req_valid{"sig_plic_mmio_req_valid"};
    sc_signal<bool>         sig_plic_mmio_req_write{"sig_plic_mmio_req_write"};
    sc_signal<sc_uint<32>>  sig_plic_mmio_req_addr{"sig_plic_mmio_req_addr"};
    sc_signal<sc_uint<32>>  sig_plic_mmio_req_wdata{"sig_plic_mmio_req_wdata"};
    sc_signal<bool>         sig_plic_mmio_resp_valid{"sig_plic_mmio_resp_valid"};
    sc_signal<sc_uint<32>>  sig_plic_mmio_resp_rdata{"sig_plic_mmio_resp_rdata"};

    // ========================================================================
    // Internal signals — CmdFabric ↔ Timer MMIO
    // ========================================================================

    sc_signal<bool>         sig_timer_mmio_req_valid{"sig_timer_mmio_req_valid"};
    sc_signal<bool>         sig_timer_mmio_req_write{"sig_timer_mmio_req_write"};
    sc_signal<sc_uint<32>>  sig_timer_mmio_req_addr{"sig_timer_mmio_req_addr"};
    sc_signal<sc_uint<32>>  sig_timer_mmio_req_wdata{"sig_timer_mmio_req_wdata"};
    sc_signal<bool>         sig_timer_mmio_resp_valid{"sig_timer_mmio_resp_valid"};
    sc_signal<sc_uint<32>>  sig_timer_mmio_resp_rdata{"sig_timer_mmio_resp_rdata"};

    // ========================================================================
    // Internal signals — IRQs
    // ========================================================================

    sc_signal<bool>         sig_meip{"sig_meip"};
    sc_signal<bool>         sig_msip{"sig_msip"};
    sc_signal<bool>         sig_mtip{"sig_mtip"};
    sc_signal<bool>         sig_dma_irq{"sig_dma_irq"};
    sc_signal<bool>         sig_loader_fault{"sig_loader_fault"};
    sc_signal<bool>         sig_fabric_fault{"sig_fabric_fault"};
    sc_signal<sc_uint<32>>  sig_plic_pending_lo{"sig_plic_pending_lo"};
    sc_signal<sc_uint<32>>  sig_plic_pending_hi{"sig_plic_pending_hi"};
    sc_signal<bool>         sig_plic_pending_any{"sig_plic_pending_any"};

    // ========================================================================
    // Internal signals — DmaEngine ↔ ClusterDataFabric (AXI4-Lite)
    // ========================================================================

    sc_signal<bool>         sig_dma_cl_aw_valid{"sig_dma_cl_aw_valid"};
    sc_signal<bool>         sig_dma_cl_aw_ready{"sig_dma_cl_aw_ready"};
    sc_signal<sc_uint<32>>  sig_dma_cl_aw_addr{"sig_dma_cl_aw_addr"};
    sc_signal<bool>         sig_dma_cl_w_valid{"sig_dma_cl_w_valid"};
    sc_signal<bool>         sig_dma_cl_w_ready{"sig_dma_cl_w_ready"};
    sc_signal<sc_biguint<kClAxiDataWidth>>          sig_dma_cl_w_data{"sig_dma_cl_w_data"};
    sc_signal<sc_uint<kClAxiDataWidth / 8>>         sig_dma_cl_w_strb{"sig_dma_cl_w_strb"};
    sc_signal<bool>         sig_dma_cl_b_valid{"sig_dma_cl_b_valid"};
    sc_signal<bool>         sig_dma_cl_b_ready{"sig_dma_cl_b_ready"};
    sc_signal<sc_uint<2>>   sig_dma_cl_b_resp{"sig_dma_cl_b_resp"};
    sc_signal<bool>         sig_dma_cl_ar_valid{"sig_dma_cl_ar_valid"};
    sc_signal<bool>         sig_dma_cl_ar_ready{"sig_dma_cl_ar_ready"};
    sc_signal<sc_uint<32>>  sig_dma_cl_ar_addr{"sig_dma_cl_ar_addr"};
    sc_signal<bool>         sig_dma_cl_r_valid{"sig_dma_cl_r_valid"};
    sc_signal<bool>         sig_dma_cl_r_ready{"sig_dma_cl_r_ready"};
    sc_signal<sc_biguint<kClAxiDataWidth>>          sig_dma_cl_r_data{"sig_dma_cl_r_data"};
    sc_signal<sc_uint<2>>   sig_dma_cl_r_resp{"sig_dma_cl_r_resp"};

    // ========================================================================
    // Internal signals — BootHostIf control / status
    // ========================================================================

    sc_signal<bool>         sig_core_enable{"sig_core_enable"};
    sc_signal<bool>         sig_core_haltreq{"sig_core_haltreq"};
    sc_signal<sc_uint<32>>  sig_boot_addr{"sig_boot_addr"};
    sc_signal<sc_uint<32>>  sig_cluster_mask_lo{"sig_cluster_mask_lo"};
    sc_signal<sc_uint<32>>  sig_cluster_mask_hi{"sig_cluster_mask_hi"};
    sc_signal<bool>         sig_core_halted{"sig_core_halted"};
    sc_signal<bool>         sig_core_running{"sig_core_running"};
    sc_signal<bool>         sig_retire_valid{"sig_retire_valid"};
    sc_signal<sc_uint<32>>  sig_retire_pc{"sig_retire_pc"};
    sc_signal<sc_uint<32>>  sig_core_cause{"sig_core_cause"};
    sc_signal<sc_uint<32>>  sig_fabric_last_target{"sig_fabric_last_target"};
    sc_signal<sc_uint<32>>  sig_fabric_last_addr{"sig_fabric_last_addr"};

    // ========================================================================
    // Combinational derived signals
    // ========================================================================

    void comb_derived() {
        // loader_fault = loader error code nonzero
        sig_loader_fault.write(sig_loader_err_code.read().to_uint() != 0);
        // plic_pending_any = any pending bit set
        sig_plic_pending_any.write(
            sig_plic_pending_lo.read().to_uint() != 0 ||
            sig_plic_pending_hi.read().to_uint() != 0);
        // core_running = core_enable active and not halted
        sig_core_running.write(sig_core_enable.read() && !sig_core_halted.read());
        // core_halted is now driven by u_core_mcu.halted_o binding (not placeholder)
        // fabric_fault placeholder (would come from CmdFabric decode error)
        sig_fabric_fault.write(false);
        // core_cause: 3 = breakpoint when halted
        sig_core_cause.write(sig_core_halted.read() ? 3u : 0u);
        // fabric_last_target/addr placeholders
        sig_fabric_last_target.write(0);
        sig_fabric_last_addr.write(0);
    }

    // ========================================================================
    // DRAM AXI mux: load_phase selects SectionLoader, else DmaEngine
    // ========================================================================

    void mux_mem_axi() {
        if (sig_boot_load_phase.read()) {
            // === Load phase: SectionLoader owns read channels ===
            // AR channel → loader
            m_mem_axi_ar_valid_o.write(sig_ldr_mem_ar_valid.read());
            m_mem_axi_ar_addr_o.write(sig_ldr_mem_ar_addr.read());
            m_mem_axi_ar_len_o.write(sig_ldr_mem_ar_len.read());
            sig_ldr_mem_ar_ready.write(m_mem_axi_ar_ready_i.read());
            // R channel → loader
            m_mem_axi_r_ready_o.write(sig_ldr_mem_r_ready.read());
            sig_ldr_mem_r_valid.write(m_mem_axi_r_valid_i.read());
            sig_ldr_mem_r_data.write(m_mem_axi_r_data_i.read());
            sig_ldr_mem_r_resp.write(m_mem_axi_r_resp_i.read());
            sig_ldr_mem_r_last.write(m_mem_axi_r_last_i.read());
            // Write channels idle on external port
            m_mem_axi_aw_valid_o.write(false);
            m_mem_axi_aw_addr_o.write(0);
            m_mem_axi_aw_len_o.write(0);
            m_mem_axi_w_valid_o.write(false);
            m_mem_axi_w_data_o.write(0);
            m_mem_axi_w_strb_o.write(0);
            m_mem_axi_w_last_o.write(false);
            m_mem_axi_b_ready_o.write(false);
            // DMA sees idle responses
            sig_dma_mem_aw_ready.write(false);
            sig_dma_mem_w_ready.write(false);
            sig_dma_mem_b_valid.write(false);
            sig_dma_mem_b_resp.write(0);
            sig_dma_mem_ar_ready.write(false);
            sig_dma_mem_r_valid.write(false);
            sig_dma_mem_r_data.write(0);
            sig_dma_mem_r_resp.write(0);
            sig_dma_mem_r_last.write(false);
        } else {
            // === Run phase: DMA engine owns all channels ===
            // AW channel
            m_mem_axi_aw_valid_o.write(sig_dma_mem_aw_valid.read());
            m_mem_axi_aw_addr_o.write(sig_dma_mem_aw_addr.read());
            m_mem_axi_aw_len_o.write(sig_dma_mem_aw_len.read());
            sig_dma_mem_aw_ready.write(m_mem_axi_aw_ready_i.read());
            // W channel
            m_mem_axi_w_valid_o.write(sig_dma_mem_w_valid.read());
            m_mem_axi_w_data_o.write(sig_dma_mem_w_data.read());
            m_mem_axi_w_strb_o.write(sig_dma_mem_w_strb.read());
            m_mem_axi_w_last_o.write(sig_dma_mem_w_last.read());
            sig_dma_mem_w_ready.write(m_mem_axi_w_ready_i.read());
            // B channel
            m_mem_axi_b_ready_o.write(sig_dma_mem_b_ready.read());
            sig_dma_mem_b_valid.write(m_mem_axi_b_valid_i.read());
            sig_dma_mem_b_resp.write(m_mem_axi_b_resp_i.read());
            // AR channel
            m_mem_axi_ar_valid_o.write(sig_dma_mem_ar_valid.read());
            m_mem_axi_ar_addr_o.write(sig_dma_mem_ar_addr.read());
            m_mem_axi_ar_len_o.write(sig_dma_mem_ar_len.read());
            sig_dma_mem_ar_ready.write(m_mem_axi_ar_ready_i.read());
            // R channel
            m_mem_axi_r_ready_o.write(sig_dma_mem_r_ready.read());
            sig_dma_mem_r_valid.write(m_mem_axi_r_valid_i.read());
            sig_dma_mem_r_data.write(m_mem_axi_r_data_i.read());
            sig_dma_mem_r_resp.write(m_mem_axi_r_resp_i.read());
            sig_dma_mem_r_last.write(m_mem_axi_r_last_i.read());
            // Loader sees idle
            sig_ldr_mem_ar_ready.write(false);
            sig_ldr_mem_r_valid.write(false);
            sig_ldr_mem_r_data.write(0);
            sig_ldr_mem_r_resp.write(0);
            sig_ldr_mem_r_last.write(false);
        }
    }
};

} // namespace core
} // namespace hybridacc
