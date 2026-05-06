//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   CoreController
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Top-level Core controller baseline integration.
// Dependencies:  src/Core/core_pkg.sv and Core submodules.
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module CoreController #(
    parameter int unsigned NUM_CLUSTERS = 1,
    parameter int unsigned NUM_NLU = 0
) (
    input  logic clk,
    input  logic reset_n,
    input  logic        s_ctrl_aw_valid_i,
    output logic        s_ctrl_aw_ready_o,
    input  logic [31:0] s_ctrl_aw_addr_i,
    input  logic        s_ctrl_w_valid_i,
    output logic        s_ctrl_w_ready_o,
    input  logic [31:0] s_ctrl_w_data_i,
    input  logic [3:0]  s_ctrl_w_strb_i,
    output logic        s_ctrl_b_valid_o,
    input  logic        s_ctrl_b_ready_i,
    output logic [1:0]  s_ctrl_b_resp_o,
    input  logic        s_ctrl_ar_valid_i,
    output logic        s_ctrl_ar_ready_o,
    input  logic [31:0] s_ctrl_ar_addr_i,
    output logic        s_ctrl_r_valid_o,
    input  logic        s_ctrl_r_ready_i,
    output logic [31:0] s_ctrl_r_data_o,
    output logic [1:0]  s_ctrl_r_resp_o,
    output logic        m_mem_axi_aw_valid_o,
    input  logic        m_mem_axi_aw_ready_i,
    output logic [31:0] m_mem_axi_aw_addr_o,
    output logic [7:0]  m_mem_axi_aw_len_o,
    output logic        m_mem_axi_w_valid_o,
    input  logic        m_mem_axi_w_ready_i,
    output logic [MEM_AXI_DATA_WIDTH-1:0] m_mem_axi_w_data_o,
    output logic [MEM_AXI_DATA_WIDTH/8-1:0] m_mem_axi_w_strb_o,
    output logic        m_mem_axi_w_last_o,
    input  logic        m_mem_axi_b_valid_i,
    output logic        m_mem_axi_b_ready_o,
    input  logic [1:0]  m_mem_axi_b_resp_i,
    output logic        m_mem_axi_ar_valid_o,
    input  logic        m_mem_axi_ar_ready_i,
    output logic [31:0] m_mem_axi_ar_addr_o,
    output logic [7:0]  m_mem_axi_ar_len_o,
    input  logic        m_mem_axi_r_valid_i,
    output logic        m_mem_axi_r_ready_o,
    input  logic [MEM_AXI_DATA_WIDTH-1:0] m_mem_axi_r_data_i,
    input  logic [1:0]  m_mem_axi_r_resp_i,
    input  logic        m_mem_axi_r_last_i,
    output logic        m_cl_data_aw_valid_o[NUM_CLUSTERS],
    input  logic        m_cl_data_aw_ready_i[NUM_CLUSTERS],
    output logic [31:0] m_cl_data_aw_addr_o[NUM_CLUSTERS],
    output logic        m_cl_data_w_valid_o[NUM_CLUSTERS],
    input  logic        m_cl_data_w_ready_i[NUM_CLUSTERS],
    output logic [CL_AXI_DATA_WIDTH-1:0] m_cl_data_w_data_o[NUM_CLUSTERS],
    output logic [CL_AXI_DATA_WIDTH/8-1:0] m_cl_data_w_strb_o[NUM_CLUSTERS],
    input  logic        m_cl_data_b_valid_i[NUM_CLUSTERS],
    output logic        m_cl_data_b_ready_o[NUM_CLUSTERS],
    input  logic [1:0]  m_cl_data_b_resp_i[NUM_CLUSTERS],
    output logic        m_cl_data_ar_valid_o[NUM_CLUSTERS],
    input  logic        m_cl_data_ar_ready_i[NUM_CLUSTERS],
    output logic [31:0] m_cl_data_ar_addr_o[NUM_CLUSTERS],
    input  logic        m_cl_data_r_valid_i[NUM_CLUSTERS],
    output logic        m_cl_data_r_ready_o[NUM_CLUSTERS],
    input  logic [CL_AXI_DATA_WIDTH-1:0] m_cl_data_r_data_i[NUM_CLUSTERS],
    input  logic [1:0]  m_cl_data_r_resp_i[NUM_CLUSTERS],
    output logic        cl_cmd_req_valid_o[NUM_CLUSTERS],
    output logic        cl_cmd_req_write_o[NUM_CLUSTERS],
    output logic [31:0] cl_cmd_req_addr_o[NUM_CLUSTERS],
    output logic [31:0] cl_cmd_req_wdata_o[NUM_CLUSTERS],
    output logic [3:0]  cl_cmd_req_wstrb_o[NUM_CLUSTERS],
    input  logic        cl_cmd_req_ready_i[NUM_CLUSTERS],
    input  logic        cl_cmd_resp_valid_i[NUM_CLUSTERS],
    input  logic [31:0] cl_cmd_resp_rdata_i[NUM_CLUSTERS],
    input  logic        cl_cmd_resp_err_i[NUM_CLUSTERS],
    output logic        nlu_cmd_req_valid_o[NUM_NLU > 0 ? NUM_NLU : 1],
    output logic        nlu_cmd_req_write_o[NUM_NLU > 0 ? NUM_NLU : 1],
    output logic [31:0] nlu_cmd_req_addr_o[NUM_NLU > 0 ? NUM_NLU : 1],
    output logic [31:0] nlu_cmd_req_wdata_o[NUM_NLU > 0 ? NUM_NLU : 1],
    input  logic        nlu_cmd_resp_valid_i[NUM_NLU > 0 ? NUM_NLU : 1],
    input  logic [31:0] nlu_cmd_resp_rdata_i[NUM_NLU > 0 ? NUM_NLU : 1],
    input  logic        cluster_irq_i[NUM_CLUSTERS],
    input  logic        nlu_irq_i[NUM_NLU > 0 ? NUM_NLU : 1],
    output logic        controller_irq_o,
    input  logic        nlu_data_axi_aw_valid_i,
    output logic        nlu_data_axi_aw_ready_o,
    input  logic [31:0] nlu_data_axi_aw_addr_i,
    input  logic        nlu_data_axi_w_valid_i,
    output logic        nlu_data_axi_w_ready_o,
    input  logic [CL_AXI_DATA_WIDTH-1:0] nlu_data_axi_w_data_i,
    input  logic [CL_AXI_DATA_WIDTH/8-1:0] nlu_data_axi_w_strb_i,
    output logic        nlu_data_axi_b_valid_o,
    input  logic        nlu_data_axi_b_ready_i,
    output logic [1:0]  nlu_data_axi_b_resp_o,
    input  logic        nlu_data_axi_ar_valid_i,
    output logic        nlu_data_axi_ar_ready_o,
    input  logic [31:0] nlu_data_axi_ar_addr_i,
    output logic        nlu_data_axi_r_valid_o,
    input  logic        nlu_data_axi_r_ready_i,
    output logic [CL_AXI_DATA_WIDTH-1:0] nlu_data_axi_r_data_o,
    output logic [1:0]  nlu_data_axi_r_resp_o
);
    localparam int unsigned NLU_PORTS = (NUM_NLU > 0) ? NUM_NLU : 1;

    logic        boot_core_enable_w;
    logic        boot_core_haltreq_w;
    logic [31:0] boot_addr_w;
    logic        boot_loader_kick_w;
    logic [31:0] boot_manifest_lo_w;
    logic [31:0] boot_manifest_hi_w;
    logic [31:0] boot_manifest_size_w;
    logic [31:0] boot_cluster_mask_lo_w;
    logic [31:0] boot_cluster_mask_hi_w;

    logic        loader_busy_w;
    logic        loader_done_w;
    logic [31:0] loader_status_w;
    logic [31:0] loader_err_code_w;
    logic [31:0] loader_err_info_w;
    logic        loader_load_phase_w;
    logic        loader_isram_wr_en_w;
    logic [31:0] loader_isram_wr_addr_w;
    logic [31:0] loader_isram_wr_data_w;
    logic [3:0]  loader_isram_wr_strb_w;
    logic        loader_dsram_wr_en_w;
    logic [31:0] loader_dsram_wr_addr_w;
    logic [31:0] loader_dsram_wr_data_w;
    logic [3:0]  loader_dsram_wr_strb_w;

    logic        mcu_if_valid_w;
    logic [31:0] mcu_if_addr_w;
    logic        mcu_if_resp_valid_w;
    logic [31:0] isram_rdata_w;
    logic        mcu_ls_valid_w;
    logic        mcu_ls_write_w;
    logic [31:0] mcu_ls_addr_w;
    logic [31:0] mcu_ls_wdata_w;
    logic [3:0]  mcu_ls_wstrb_w;
    logic        mcu_ls_resp_valid_w;
    logic [31:0] dsram_rdata_w;
    logic        mcu_ls_is_dsram_w;
    logic        dsram_resp_valid_w;
    logic        mcu_mmio_valid_w;
    logic        mcu_mmio_write_w;
    logic [31:0] mcu_mmio_addr_w;
    logic [31:0] mcu_mmio_wdata_w;
    logic [3:0]  mcu_mmio_wstrb_w;
    logic        mcu_mmio_resp_valid_w;
    logic [31:0] mcu_mmio_resp_rdata_w;
    logic        mcu_retire_valid_w;
    logic [31:0] mcu_retire_pc_w;
    logic        mcu_running_w;
    logic        mcu_halted_w;
    logic [31:0] mcu_pc_snapshot_w;
    logic [31:0] mcu_cause_snapshot_w;

    logic        dma_mmio_req_valid_w;
    logic        dma_mmio_req_write_w;
    logic [31:0] dma_mmio_req_addr_w;
    logic [31:0] dma_mmio_req_wdata_w;
    logic        dma_mmio_resp_valid_w;
    logic [31:0] dma_mmio_resp_rdata_w;
    logic        plic_mmio_req_valid_w;
    logic        plic_mmio_req_write_w;
    logic [31:0] plic_mmio_req_addr_w;
    logic [31:0] plic_mmio_req_wdata_w;
    logic        plic_mmio_resp_valid_w;
    logic [31:0] plic_mmio_resp_rdata_w;
    logic        timer_mmio_req_valid_w;
    logic        timer_mmio_req_write_w;
    logic [31:0] timer_mmio_req_addr_w;
    logic [31:0] timer_mmio_req_wdata_w;
    logic        timer_mmio_resp_valid_w;
    logic [31:0] timer_mmio_resp_rdata_w;
    logic [31:0] fabric_last_target_w;
    logic [31:0] fabric_last_addr_w;
    logic [31:0] fabric_mmio_err_status_w;

    logic        irq_msip_w;
    logic        irq_mtip_w;
    logic        irq_meip_w;
    logic [31:0] plic_pending_lo_w;
    logic [31:0] plic_pending_hi_w;
    logic        plic_pending_any_w;
    logic        dma_irq_w;

    logic        dma_mem_aw_valid_w;
    logic [31:0] dma_mem_aw_addr_w;
    logic [7:0]  dma_mem_aw_len_w;
    logic        dma_mem_w_valid_w;
    logic [MEM_AXI_DATA_WIDTH-1:0] dma_mem_w_data_w;
    logic [MEM_AXI_DATA_WIDTH/8-1:0] dma_mem_w_strb_w;
    logic        dma_mem_w_last_w;
    logic        dma_mem_b_ready_w;
    logic        dma_mem_ar_valid_w;
    logic [31:0] dma_mem_ar_addr_w;
    logic [7:0]  dma_mem_ar_len_w;
    logic        dma_mem_r_ready_w;
    logic        dma_mem_aw_ready_w;
    logic        dma_mem_w_ready_w;
    logic        dma_mem_b_valid_w;
    logic [1:0]  dma_mem_b_resp_w;
    logic        dma_mem_ar_ready_w;
    logic        dma_mem_r_valid_w;
    logic [MEM_AXI_DATA_WIDTH-1:0] dma_mem_r_data_w;
    logic [1:0]  dma_mem_r_resp_w;
    logic        dma_mem_r_last_w;

    logic        loader_mem_ar_valid_w;
    logic [31:0] loader_mem_ar_addr_w;
    logic [7:0]  loader_mem_ar_len_w;
    logic        loader_mem_r_ready_w;
    logic        loader_mem_ar_ready_w;
    logic        loader_mem_r_valid_w;
    logic [MEM_AXI_DATA_WIDTH-1:0] loader_mem_r_data_w;
    logic [1:0]  loader_mem_r_resp_w;
    logic        loader_mem_r_last_w;

    logic        dma_cl_aw_valid_w;
    logic        dma_cl_aw_ready_w;
    logic [31:0] dma_cl_aw_addr_w;
    logic        dma_cl_w_valid_w;
    logic        dma_cl_w_ready_w;
    logic [CL_AXI_DATA_WIDTH-1:0] dma_cl_w_data_w;
    logic [CL_AXI_DATA_WIDTH/8-1:0] dma_cl_w_strb_w;
    logic        dma_cl_b_valid_w;
    logic        dma_cl_b_ready_w;
    logic [1:0]  dma_cl_b_resp_w;
    logic        dma_cl_ar_valid_w;
    logic        dma_cl_ar_ready_w;
    logic [31:0] dma_cl_ar_addr_w;
    logic        dma_cl_r_valid_w;
    logic        dma_cl_r_ready_w;
    logic [CL_AXI_DATA_WIDTH-1:0] dma_cl_r_data_w;
    logic [1:0]  dma_cl_r_resp_w;

    assign plic_pending_any_w = |plic_pending_lo_w || |plic_pending_hi_w;

    assign m_mem_axi_aw_valid_o = dma_mem_aw_valid_w;
    assign m_mem_axi_aw_addr_o  = dma_mem_aw_addr_w;
    assign m_mem_axi_aw_len_o   = dma_mem_aw_len_w;
    assign m_mem_axi_w_valid_o  = dma_mem_w_valid_w;
    assign m_mem_axi_w_data_o   = dma_mem_w_data_w;
    assign m_mem_axi_w_strb_o   = dma_mem_w_strb_w;
    assign m_mem_axi_w_last_o   = dma_mem_w_last_w;
    assign m_mem_axi_b_ready_o  = dma_mem_b_ready_w;
    assign m_mem_axi_ar_valid_o = loader_busy_w ? loader_mem_ar_valid_w : dma_mem_ar_valid_w;
    assign m_mem_axi_ar_addr_o  = loader_busy_w ? loader_mem_ar_addr_w  : dma_mem_ar_addr_w;
    assign m_mem_axi_ar_len_o   = loader_busy_w ? loader_mem_ar_len_w   : dma_mem_ar_len_w;
    assign m_mem_axi_r_ready_o  = loader_busy_w ? loader_mem_r_ready_w  : dma_mem_r_ready_w;

    assign mcu_ls_is_dsram_w = (mcu_ls_addr_w >= BASE_DATA_RAM) && (mcu_ls_addr_w <= END_DATA_RAM);
    assign mcu_ls_resp_valid_w = dsram_resp_valid_w;

    assign loader_mem_ar_ready_w = loader_busy_w ? m_mem_axi_ar_ready_i : 1'b0;
    assign loader_mem_r_valid_w  = loader_busy_w ? m_mem_axi_r_valid_i  : 1'b0;
    assign loader_mem_r_data_w   = loader_busy_w ? m_mem_axi_r_data_i   : '0;
    assign loader_mem_r_resp_w   = loader_busy_w ? m_mem_axi_r_resp_i   : 2'b00;
    assign loader_mem_r_last_w   = loader_busy_w ? m_mem_axi_r_last_i   : 1'b0;

    assign dma_mem_aw_ready_w = m_mem_axi_aw_ready_i;
    assign dma_mem_w_ready_w  = m_mem_axi_w_ready_i;
    assign dma_mem_b_valid_w  = m_mem_axi_b_valid_i;
    assign dma_mem_b_resp_w   = m_mem_axi_b_resp_i;
    assign dma_mem_ar_ready_w = loader_busy_w ? 1'b0 : m_mem_axi_ar_ready_i;
    assign dma_mem_r_valid_w  = loader_busy_w ? 1'b0 : m_mem_axi_r_valid_i;
    assign dma_mem_r_data_w   = loader_busy_w ? '0   : m_mem_axi_r_data_i;
    assign dma_mem_r_resp_w   = loader_busy_w ? 2'b00 : m_mem_axi_r_resp_i;
    assign dma_mem_r_last_w   = loader_busy_w ? 1'b0 : m_mem_axi_r_last_i;

    BootHostIf #(
        .NUM_CLUSTERS(NUM_CLUSTERS),
        .NUM_NLU(NUM_NLU)
    ) boot_host_if (
        .clk(clk),
        .reset_n(reset_n),
        .s_ctrl_aw_valid_i(s_ctrl_aw_valid_i),
        .s_ctrl_aw_ready_o(s_ctrl_aw_ready_o),
        .s_ctrl_aw_addr_i(s_ctrl_aw_addr_i),
        .s_ctrl_w_valid_i(s_ctrl_w_valid_i),
        .s_ctrl_w_ready_o(s_ctrl_w_ready_o),
        .s_ctrl_w_data_i(s_ctrl_w_data_i),
        .s_ctrl_w_strb_i(s_ctrl_w_strb_i),
        .s_ctrl_b_valid_o(s_ctrl_b_valid_o),
        .s_ctrl_b_ready_i(s_ctrl_b_ready_i),
        .s_ctrl_b_resp_o(s_ctrl_b_resp_o),
        .s_ctrl_ar_valid_i(s_ctrl_ar_valid_i),
        .s_ctrl_ar_ready_o(s_ctrl_ar_ready_o),
        .s_ctrl_ar_addr_i(s_ctrl_ar_addr_i),
        .s_ctrl_r_valid_o(s_ctrl_r_valid_o),
        .s_ctrl_r_ready_i(s_ctrl_r_ready_i),
        .s_ctrl_r_data_o(s_ctrl_r_data_o),
        .s_ctrl_r_resp_o(s_ctrl_r_resp_o),
        .core_enable_o(boot_core_enable_w),
        .core_haltreq_o(boot_core_haltreq_w),
        .boot_addr_o(boot_addr_w),
        .load_phase_o(),
        .loader_kick_o(boot_loader_kick_w),
        .manifest_addr_lo_o(boot_manifest_lo_w),
        .manifest_addr_hi_o(boot_manifest_hi_w),
        .manifest_size_o(boot_manifest_size_w),
        .cluster_mask_lo_o(boot_cluster_mask_lo_w),
        .cluster_mask_hi_o(boot_cluster_mask_hi_w),
        .loader_busy_i(loader_busy_w),
        .loader_done_i(loader_done_w),
        .loader_status_i(loader_status_w),
        .loader_err_code_i(loader_err_code_w),
        .loader_err_info_i(loader_err_info_w),
        .core_halted_i(mcu_halted_w),
        .core_running_i(mcu_running_w),
        .core_pc_snapshot_i(mcu_pc_snapshot_w),
        .core_cause_snapshot_i(mcu_cause_snapshot_w),
        .plic_pending_any_i(plic_pending_any_w),
        .fabric_last_target_i(fabric_last_target_w),
        .fabric_last_addr_i(fabric_last_addr_w),
        .controller_irq_o(controller_irq_o)
    );

    SectionLoader loader (
        .clk(clk),
        .reset_n(reset_n),
        .kick_i(boot_loader_kick_w),
        .manifest_addr_lo_i(boot_manifest_lo_w),
        .manifest_addr_hi_i(boot_manifest_hi_w),
        .manifest_size_i(boot_manifest_size_w),
        .m_mem_axi_ar_valid_o(loader_mem_ar_valid_w),
        .m_mem_axi_ar_ready_i(loader_mem_ar_ready_w),
        .m_mem_axi_ar_addr_o(loader_mem_ar_addr_w),
        .m_mem_axi_ar_len_o(loader_mem_ar_len_w),
        .m_mem_axi_r_valid_i(loader_mem_r_valid_w),
        .m_mem_axi_r_ready_o(loader_mem_r_ready_w),
        .m_mem_axi_r_data_i(loader_mem_r_data_w),
        .m_mem_axi_r_resp_i(loader_mem_r_resp_w),
        .m_mem_axi_r_last_i(loader_mem_r_last_w),
        .isram_wr_en_o(loader_isram_wr_en_w),
        .isram_wr_addr_o(loader_isram_wr_addr_w),
        .isram_wr_data_o(loader_isram_wr_data_w),
        .isram_wr_strb_o(loader_isram_wr_strb_w),
        .dsram_wr_en_o(loader_dsram_wr_en_w),
        .dsram_wr_addr_o(loader_dsram_wr_addr_w),
        .dsram_wr_data_o(loader_dsram_wr_data_w),
        .dsram_wr_strb_o(loader_dsram_wr_strb_w),
        .load_phase_o(loader_load_phase_w),
        .busy_o(loader_busy_w),
        .done_o(loader_done_w),
        .status_o(loader_status_w),
        .err_code_o(loader_err_code_w),
        .err_info_o(loader_err_info_w)
    );

    Isram isram (
        .clk(clk),
        .reset_n(reset_n),
        .mcu_im_valid_i(mcu_if_valid_w),
        .mcu_im_addr_i(mcu_if_addr_w),
        .mcu_im_resp_valid_o(mcu_if_resp_valid_w),
        .mcu_im_rdata_o(isram_rdata_w),
        .loader_wr_valid_i(loader_isram_wr_en_w),
        .loader_wr_addr_i(loader_isram_wr_addr_w),
        .loader_wr_data_i(loader_isram_wr_data_w),
        .loader_wr_strb_i(loader_isram_wr_strb_w),
        .loader_wr_ready_o(),
        .load_phase_i(loader_load_phase_w)
    );

    DataSram dsram (
        .clk(clk),
        .reset_n(reset_n),
        .mcu_dm_valid_i(mcu_ls_valid_w && mcu_ls_is_dsram_w),
        .mcu_dm_write_i(mcu_ls_write_w),
        .mcu_dm_addr_i(mcu_ls_addr_w - BASE_DATA_RAM),
        .mcu_dm_wdata_i(mcu_ls_wdata_w),
        .mcu_dm_wstrb_i(mcu_ls_wstrb_w),
        .mcu_dm_resp_valid_o(dsram_resp_valid_w),
        .mcu_dm_rdata_o(dsram_rdata_w),
        .loader_wr_valid_i(loader_dsram_wr_en_w),
        .loader_wr_addr_i(loader_dsram_wr_addr_w),
        .loader_wr_data_i(loader_dsram_wr_data_w),
        .loader_wr_strb_i(loader_dsram_wr_strb_w),
        .loader_wr_ready_o(),
        .load_phase_i(loader_load_phase_w)
    );

    CoreMcu core_mcu (
        .clk(clk),
        .reset_n(reset_n),
        .boot_addr_i(boot_addr_w),
        .core_enable_i(boot_core_enable_w),
        .core_haltreq_i(boot_core_haltreq_w),
        .if_req_valid_o(mcu_if_valid_w),
        .if_addr_o(mcu_if_addr_w),
        .if_resp_valid_i(mcu_if_resp_valid_w),
        .if_rdata_i(isram_rdata_w),
        .ls_req_valid_o(mcu_ls_valid_w),
        .ls_req_write_o(mcu_ls_write_w),
        .ls_req_addr_o(mcu_ls_addr_w),
        .ls_req_wdata_o(mcu_ls_wdata_w),
        .ls_req_wstrb_o(mcu_ls_wstrb_w),
        .ls_resp_valid_i(mcu_ls_resp_valid_w),
        .ls_resp_rdata_i(dsram_rdata_w),
        .mmio_req_valid_o(mcu_mmio_valid_w),
        .mmio_req_write_o(mcu_mmio_write_w),
        .mmio_req_addr_o(mcu_mmio_addr_w),
        .mmio_req_wdata_o(mcu_mmio_wdata_w),
        .mmio_req_wstrb_o(mcu_mmio_wstrb_w),
        .mmio_resp_valid_i(mcu_mmio_resp_valid_w),
        .mmio_resp_rdata_i(mcu_mmio_resp_rdata_w),
        .irq_meip_i(irq_meip_w),
        .irq_msip_i(irq_msip_w),
        .irq_mtip_i(irq_mtip_w),
        .retire_valid_o(mcu_retire_valid_w),
        .retire_pc_o(mcu_retire_pc_w),
        .core_running_o(mcu_running_w),
        .core_halted_o(mcu_halted_w),
        .pc_snapshot_o(mcu_pc_snapshot_w),
        .cause_snapshot_o(mcu_cause_snapshot_w)
    );

    CmdFabric #(
        .NUM_CLUSTERS(NUM_CLUSTERS),
        .NUM_NLU(NUM_NLU)
    ) cmd_fabric (
        .clk(clk),
        .reset_n(reset_n),
        .core_mmio_req_valid_i(mcu_mmio_valid_w),
        .core_mmio_req_write_i(mcu_mmio_write_w),
        .core_mmio_req_addr_i(mcu_mmio_addr_w),
        .core_mmio_req_wdata_i(mcu_mmio_wdata_w),
        .core_mmio_req_wstrb_i(mcu_mmio_wstrb_w),
        .core_mmio_resp_valid_o(mcu_mmio_resp_valid_w),
        .core_mmio_resp_rdata_o(mcu_mmio_resp_rdata_w),
        .dma_mmio_req_valid_o(dma_mmio_req_valid_w),
        .dma_mmio_req_write_o(dma_mmio_req_write_w),
        .dma_mmio_req_addr_o(dma_mmio_req_addr_w),
        .dma_mmio_req_wdata_o(dma_mmio_req_wdata_w),
        .dma_mmio_resp_valid_i(dma_mmio_resp_valid_w),
        .dma_mmio_resp_rdata_i(dma_mmio_resp_rdata_w),
        .plic_mmio_req_valid_o(plic_mmio_req_valid_w),
        .plic_mmio_req_write_o(plic_mmio_req_write_w),
        .plic_mmio_req_addr_o(plic_mmio_req_addr_w),
        .plic_mmio_req_wdata_o(plic_mmio_req_wdata_w),
        .plic_mmio_resp_valid_i(plic_mmio_resp_valid_w),
        .plic_mmio_resp_rdata_i(plic_mmio_resp_rdata_w),
        .timer_mmio_req_valid_o(timer_mmio_req_valid_w),
        .timer_mmio_req_write_o(timer_mmio_req_write_w),
        .timer_mmio_req_addr_o(timer_mmio_req_addr_w),
        .timer_mmio_req_wdata_o(timer_mmio_req_wdata_w),
        .timer_mmio_resp_valid_i(timer_mmio_resp_valid_w),
        .timer_mmio_resp_rdata_i(timer_mmio_resp_rdata_w),
        .cl_cmd_req_valid_o(cl_cmd_req_valid_o),
        .cl_cmd_req_write_o(cl_cmd_req_write_o),
        .cl_cmd_req_addr_o(cl_cmd_req_addr_o),
        .cl_cmd_req_wdata_o(cl_cmd_req_wdata_o),
        .cl_cmd_req_wstrb_o(cl_cmd_req_wstrb_o),
        .cl_cmd_req_ready_i(cl_cmd_req_ready_i),
        .cl_cmd_resp_valid_i(cl_cmd_resp_valid_i),
        .cl_cmd_resp_rdata_i(cl_cmd_resp_rdata_i),
        .cl_cmd_resp_err_i(cl_cmd_resp_err_i),
        .nlu_cmd_req_valid_o(nlu_cmd_req_valid_o),
        .nlu_cmd_req_write_o(nlu_cmd_req_write_o),
        .nlu_cmd_req_addr_o(nlu_cmd_req_addr_o),
        .nlu_cmd_req_wdata_o(nlu_cmd_req_wdata_o),
        .nlu_cmd_resp_valid_i(nlu_cmd_resp_valid_i),
        .nlu_cmd_resp_rdata_i(nlu_cmd_resp_rdata_i),
        .cluster_mask_lo_i(boot_cluster_mask_lo_w),
        .cluster_mask_hi_i(boot_cluster_mask_hi_w),
        .fabric_last_target_o(fabric_last_target_w),
        .fabric_last_addr_o(fabric_last_addr_w),
        .fabric_mmio_err_status_o(fabric_mmio_err_status_w)
    );

    CoreLocalIrq core_local_irq (
        .clk(clk),
        .reset_n(reset_n),
        .mmio_req_valid_i(timer_mmio_req_valid_w),
        .mmio_req_write_i(timer_mmio_req_write_w),
        .mmio_req_addr_i(timer_mmio_req_addr_w),
        .mmio_req_wdata_i(timer_mmio_req_wdata_w),
        .mmio_resp_valid_o(timer_mmio_resp_valid_w),
        .mmio_resp_rdata_o(timer_mmio_resp_rdata_w),
        .irq_msip_o(irq_msip_w),
        .irq_mtip_o(irq_mtip_w)
    );

    Plic #(
        .NUM_CLUSTERS(NUM_CLUSTERS),
        .NUM_NLU(NUM_NLU)
    ) plic (
        .clk(clk),
        .reset_n(reset_n),
        .cluster_irq_i(cluster_irq_i),
        .nlu_irq_i(nlu_irq_i),
        .dma_irq_i(dma_irq_w),
        .loader_fault_i(loader_err_code_w != 32'h0),
        .fabric_fault_i(fabric_mmio_err_status_w != 32'h0),
        .meip_o(irq_meip_w),
        .mmio_req_valid_i(plic_mmio_req_valid_w),
        .mmio_req_write_i(plic_mmio_req_write_w),
        .mmio_req_addr_i(plic_mmio_req_addr_w),
        .mmio_req_wdata_i(plic_mmio_req_wdata_w),
        .mmio_resp_valid_o(plic_mmio_resp_valid_w),
        .mmio_resp_rdata_o(plic_mmio_resp_rdata_w),
        .pending_lo_o(plic_pending_lo_w),
        .pending_hi_o(plic_pending_hi_w)
    );

    DmaEngine dma_engine (
        .clk(clk),
        .reset_n(reset_n),
        .mmio_req_valid_i(dma_mmio_req_valid_w),
        .mmio_req_write_i(dma_mmio_req_write_w),
        .mmio_req_addr_i(dma_mmio_req_addr_w),
        .mmio_req_wdata_i(dma_mmio_req_wdata_w),
        .mmio_resp_valid_o(dma_mmio_resp_valid_w),
        .mmio_resp_rdata_o(dma_mmio_resp_rdata_w),
        .m_mem_axi_aw_valid_o(dma_mem_aw_valid_w),
        .m_mem_axi_aw_ready_i(dma_mem_aw_ready_w),
        .m_mem_axi_aw_addr_o(dma_mem_aw_addr_w),
        .m_mem_axi_aw_len_o(dma_mem_aw_len_w),
        .m_mem_axi_w_valid_o(dma_mem_w_valid_w),
        .m_mem_axi_w_ready_i(dma_mem_w_ready_w),
        .m_mem_axi_w_data_o(dma_mem_w_data_w),
        .m_mem_axi_w_strb_o(dma_mem_w_strb_w),
        .m_mem_axi_w_last_o(dma_mem_w_last_w),
        .m_mem_axi_b_valid_i(dma_mem_b_valid_w),
        .m_mem_axi_b_ready_o(dma_mem_b_ready_w),
        .m_mem_axi_b_resp_i(dma_mem_b_resp_w),
        .m_mem_axi_ar_valid_o(dma_mem_ar_valid_w),
        .m_mem_axi_ar_ready_i(dma_mem_ar_ready_w),
        .m_mem_axi_ar_addr_o(dma_mem_ar_addr_w),
        .m_mem_axi_ar_len_o(dma_mem_ar_len_w),
        .m_mem_axi_r_valid_i(dma_mem_r_valid_w),
        .m_mem_axi_r_ready_o(dma_mem_r_ready_w),
        .m_mem_axi_r_data_i(dma_mem_r_data_w),
        .m_mem_axi_r_resp_i(dma_mem_r_resp_w),
        .m_mem_axi_r_last_i(dma_mem_r_last_w),
        .m_cl_axi_aw_valid_o(dma_cl_aw_valid_w),
        .m_cl_axi_aw_ready_i(dma_cl_aw_ready_w),
        .m_cl_axi_aw_addr_o(dma_cl_aw_addr_w),
        .m_cl_axi_w_valid_o(dma_cl_w_valid_w),
        .m_cl_axi_w_ready_i(dma_cl_w_ready_w),
        .m_cl_axi_w_data_o(dma_cl_w_data_w),
        .m_cl_axi_w_strb_o(dma_cl_w_strb_w),
        .m_cl_axi_b_valid_i(dma_cl_b_valid_w),
        .m_cl_axi_b_ready_o(dma_cl_b_ready_w),
        .m_cl_axi_b_resp_i(dma_cl_b_resp_w),
        .m_cl_axi_ar_valid_o(dma_cl_ar_valid_w),
        .m_cl_axi_ar_ready_i(dma_cl_ar_ready_w),
        .m_cl_axi_ar_addr_o(dma_cl_ar_addr_w),
        .m_cl_axi_r_valid_i(dma_cl_r_valid_w),
        .m_cl_axi_r_ready_o(dma_cl_r_ready_w),
        .m_cl_axi_r_data_i(dma_cl_r_data_w),
        .m_cl_axi_r_resp_i(dma_cl_r_resp_w),
        .dma_irq_o(dma_irq_w)
    );

    ClusterDataFabric #(
        .NUM_CLUSTERS(NUM_CLUSTERS)
    ) cluster_data_fabric (
        .clk(clk),
        .reset_n(reset_n),
        .s_dma_axi_aw_valid_i(dma_cl_aw_valid_w),
        .s_dma_axi_aw_ready_o(dma_cl_aw_ready_w),
        .s_dma_axi_aw_addr_i(dma_cl_aw_addr_w),
        .s_dma_axi_w_valid_i(dma_cl_w_valid_w),
        .s_dma_axi_w_ready_o(dma_cl_w_ready_w),
        .s_dma_axi_w_data_i(dma_cl_w_data_w),
        .s_dma_axi_w_strb_i(dma_cl_w_strb_w),
        .s_dma_axi_b_valid_o(dma_cl_b_valid_w),
        .s_dma_axi_b_ready_i(dma_cl_b_ready_w),
        .s_dma_axi_b_resp_o(dma_cl_b_resp_w),
        .s_dma_axi_ar_valid_i(dma_cl_ar_valid_w),
        .s_dma_axi_ar_ready_o(dma_cl_ar_ready_w),
        .s_dma_axi_ar_addr_i(dma_cl_ar_addr_w),
        .s_dma_axi_r_valid_o(dma_cl_r_valid_w),
        .s_dma_axi_r_ready_i(dma_cl_r_ready_w),
        .s_dma_axi_r_data_o(dma_cl_r_data_w),
        .s_dma_axi_r_resp_o(dma_cl_r_resp_w),
        .s_nlu_axi_aw_valid_i(nlu_data_axi_aw_valid_i),
        .s_nlu_axi_aw_ready_o(nlu_data_axi_aw_ready_o),
        .s_nlu_axi_aw_addr_i(nlu_data_axi_aw_addr_i),
        .s_nlu_axi_w_valid_i(nlu_data_axi_w_valid_i),
        .s_nlu_axi_w_ready_o(nlu_data_axi_w_ready_o),
        .s_nlu_axi_w_data_i(nlu_data_axi_w_data_i),
        .s_nlu_axi_w_strb_i(nlu_data_axi_w_strb_i),
        .s_nlu_axi_b_valid_o(nlu_data_axi_b_valid_o),
        .s_nlu_axi_b_ready_i(nlu_data_axi_b_ready_i),
        .s_nlu_axi_b_resp_o(nlu_data_axi_b_resp_o),
        .s_nlu_axi_ar_valid_i(nlu_data_axi_ar_valid_i),
        .s_nlu_axi_ar_ready_o(nlu_data_axi_ar_ready_o),
        .s_nlu_axi_ar_addr_i(nlu_data_axi_ar_addr_i),
        .s_nlu_axi_r_valid_o(nlu_data_axi_r_valid_o),
        .s_nlu_axi_r_ready_i(nlu_data_axi_r_ready_i),
        .s_nlu_axi_r_data_o(nlu_data_axi_r_data_o),
        .s_nlu_axi_r_resp_o(nlu_data_axi_r_resp_o),
        .m_cl_data_aw_valid_o(m_cl_data_aw_valid_o),
        .m_cl_data_aw_ready_i(m_cl_data_aw_ready_i),
        .m_cl_data_aw_addr_o(m_cl_data_aw_addr_o),
        .m_cl_data_w_valid_o(m_cl_data_w_valid_o),
        .m_cl_data_w_ready_i(m_cl_data_w_ready_i),
        .m_cl_data_w_data_o(m_cl_data_w_data_o),
        .m_cl_data_w_strb_o(m_cl_data_w_strb_o),
        .m_cl_data_b_valid_i(m_cl_data_b_valid_i),
        .m_cl_data_b_ready_o(m_cl_data_b_ready_o),
        .m_cl_data_b_resp_i(m_cl_data_b_resp_i),
        .m_cl_data_ar_valid_o(m_cl_data_ar_valid_o),
        .m_cl_data_ar_ready_i(m_cl_data_ar_ready_i),
        .m_cl_data_ar_addr_o(m_cl_data_ar_addr_o),
        .m_cl_data_r_valid_i(m_cl_data_r_valid_i),
        .m_cl_data_r_ready_o(m_cl_data_r_ready_o),
        .m_cl_data_r_data_i(m_cl_data_r_data_i),
        .m_cl_data_r_resp_i(m_cl_data_r_resp_i)
    );

endmodule