//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   HybridAcc
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Top-level HybridAcc SoC baseline integrating CoreController
//                and an array of ComputeCluster instances.
// Dependencies:  src/Core/*.sv, src/Cluster/*.sv, src/NetworkOnChip.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module HybridAcc #(
    parameter int unsigned NUM_CLUSTERS = 1
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
    output logic        controller_irq_o
);
    logic        cl_cmd_req_valid [NUM_CLUSTERS];
    logic        cl_cmd_req_write [NUM_CLUSTERS];
    logic [31:0] cl_cmd_req_addr  [NUM_CLUSTERS];
    logic [31:0] cl_cmd_req_wdata [NUM_CLUSTERS];
    logic [3:0]  cl_cmd_req_wstrb [NUM_CLUSTERS];
    logic        cl_cmd_req_ready [NUM_CLUSTERS];
    logic        cl_cmd_resp_valid[NUM_CLUSTERS];
    logic [31:0] cl_cmd_resp_rdata[NUM_CLUSTERS];
    logic        cl_cmd_resp_err  [NUM_CLUSTERS];

    logic        cl_data_aw_valid [NUM_CLUSTERS];
    logic        cl_data_aw_ready [NUM_CLUSTERS];
    logic [31:0] cl_data_aw_addr  [NUM_CLUSTERS];
    logic        cl_data_w_valid  [NUM_CLUSTERS];
    logic        cl_data_w_ready  [NUM_CLUSTERS];
    logic [CL_AXI_DATA_WIDTH-1:0] cl_data_w_data[NUM_CLUSTERS];
    logic [CL_AXI_DATA_WIDTH/8-1:0] cl_data_w_strb[NUM_CLUSTERS];
    logic        cl_data_b_valid  [NUM_CLUSTERS];
    logic        cl_data_b_ready  [NUM_CLUSTERS];
    logic [1:0]  cl_data_b_resp   [NUM_CLUSTERS];
    logic        cl_data_ar_valid [NUM_CLUSTERS];
    logic        cl_data_ar_ready [NUM_CLUSTERS];
    logic [31:0] cl_data_ar_addr  [NUM_CLUSTERS];
    logic        cl_data_r_valid  [NUM_CLUSTERS];
    logic        cl_data_r_ready  [NUM_CLUSTERS];
    logic [CL_AXI_DATA_WIDTH-1:0] cl_data_r_data[NUM_CLUSTERS];
    logic [1:0]  cl_data_r_resp   [NUM_CLUSTERS];
    logic        cluster_irq      [NUM_CLUSTERS];

    logic        nlu_cmd_req_valid[1];
    logic        nlu_cmd_req_write[1];
    logic [31:0] nlu_cmd_req_addr [1];
    logic [31:0] nlu_cmd_req_wdata[1];
    logic        nlu_cmd_resp_valid[1];
    logic [31:0] nlu_cmd_resp_rdata[1];
    logic        nlu_irq[1];
    logic        nlu_data_axi_aw_valid;
    logic        nlu_data_axi_aw_ready;
    logic [31:0] nlu_data_axi_aw_addr;
    logic        nlu_data_axi_w_valid;
    logic        nlu_data_axi_w_ready;
    logic [CL_AXI_DATA_WIDTH-1:0] nlu_data_axi_w_data;
    logic [CL_AXI_DATA_WIDTH/8-1:0] nlu_data_axi_w_strb;
    logic        nlu_data_axi_b_valid;
    logic        nlu_data_axi_b_ready;
    logic [1:0]  nlu_data_axi_b_resp;
    logic        nlu_data_axi_ar_valid;
    logic        nlu_data_axi_ar_ready;
    logic [31:0] nlu_data_axi_ar_addr;
    logic        nlu_data_axi_r_valid;
    logic        nlu_data_axi_r_ready;
    logic [CL_AXI_DATA_WIDTH-1:0] nlu_data_axi_r_data;
    logic [1:0]  nlu_data_axi_r_resp;

    CoreController #(
        .NUM_CLUSTERS(NUM_CLUSTERS),
        .NUM_NLU(0)
    ) core_ctrl (
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
        .m_mem_axi_aw_valid_o(m_mem_axi_aw_valid_o),
        .m_mem_axi_aw_ready_i(m_mem_axi_aw_ready_i),
        .m_mem_axi_aw_addr_o(m_mem_axi_aw_addr_o),
        .m_mem_axi_aw_len_o(m_mem_axi_aw_len_o),
        .m_mem_axi_w_valid_o(m_mem_axi_w_valid_o),
        .m_mem_axi_w_ready_i(m_mem_axi_w_ready_i),
        .m_mem_axi_w_data_o(m_mem_axi_w_data_o),
        .m_mem_axi_w_strb_o(m_mem_axi_w_strb_o),
        .m_mem_axi_w_last_o(m_mem_axi_w_last_o),
        .m_mem_axi_b_valid_i(m_mem_axi_b_valid_i),
        .m_mem_axi_b_ready_o(m_mem_axi_b_ready_o),
        .m_mem_axi_b_resp_i(m_mem_axi_b_resp_i),
        .m_mem_axi_ar_valid_o(m_mem_axi_ar_valid_o),
        .m_mem_axi_ar_ready_i(m_mem_axi_ar_ready_i),
        .m_mem_axi_ar_addr_o(m_mem_axi_ar_addr_o),
        .m_mem_axi_ar_len_o(m_mem_axi_ar_len_o),
        .m_mem_axi_r_valid_i(m_mem_axi_r_valid_i),
        .m_mem_axi_r_ready_o(m_mem_axi_r_ready_o),
        .m_mem_axi_r_data_i(m_mem_axi_r_data_i),
        .m_mem_axi_r_resp_i(m_mem_axi_r_resp_i),
        .m_mem_axi_r_last_i(m_mem_axi_r_last_i),
        .m_cl_data_aw_valid_o(cl_data_aw_valid),
        .m_cl_data_aw_ready_i(cl_data_aw_ready),
        .m_cl_data_aw_addr_o(cl_data_aw_addr),
        .m_cl_data_w_valid_o(cl_data_w_valid),
        .m_cl_data_w_ready_i(cl_data_w_ready),
        .m_cl_data_w_data_o(cl_data_w_data),
        .m_cl_data_w_strb_o(cl_data_w_strb),
        .m_cl_data_b_valid_i(cl_data_b_valid),
        .m_cl_data_b_ready_o(cl_data_b_ready),
        .m_cl_data_b_resp_i(cl_data_b_resp),
        .m_cl_data_ar_valid_o(cl_data_ar_valid),
        .m_cl_data_ar_ready_i(cl_data_ar_ready),
        .m_cl_data_ar_addr_o(cl_data_ar_addr),
        .m_cl_data_r_valid_i(cl_data_r_valid),
        .m_cl_data_r_ready_o(cl_data_r_ready),
        .m_cl_data_r_data_i(cl_data_r_data),
        .m_cl_data_r_resp_i(cl_data_r_resp),
        .cl_cmd_req_valid_o(cl_cmd_req_valid),
        .cl_cmd_req_write_o(cl_cmd_req_write),
        .cl_cmd_req_addr_o(cl_cmd_req_addr),
        .cl_cmd_req_wdata_o(cl_cmd_req_wdata),
        .cl_cmd_req_wstrb_o(cl_cmd_req_wstrb),
        .cl_cmd_req_ready_i(cl_cmd_req_ready),
        .cl_cmd_resp_valid_i(cl_cmd_resp_valid),
        .cl_cmd_resp_rdata_i(cl_cmd_resp_rdata),
        .cl_cmd_resp_err_i(cl_cmd_resp_err),
        .nlu_cmd_req_valid_o(nlu_cmd_req_valid),
        .nlu_cmd_req_write_o(nlu_cmd_req_write),
        .nlu_cmd_req_addr_o(nlu_cmd_req_addr),
        .nlu_cmd_req_wdata_o(nlu_cmd_req_wdata),
        .nlu_cmd_resp_valid_i(nlu_cmd_resp_valid),
        .nlu_cmd_resp_rdata_i(nlu_cmd_resp_rdata),
        .cluster_irq_i(cluster_irq),
        .nlu_irq_i(nlu_irq),
        .controller_irq_o(controller_irq_o),
        .nlu_data_axi_aw_valid_i(nlu_data_axi_aw_valid),
        .nlu_data_axi_aw_ready_o(nlu_data_axi_aw_ready),
        .nlu_data_axi_aw_addr_i(nlu_data_axi_aw_addr),
        .nlu_data_axi_w_valid_i(nlu_data_axi_w_valid),
        .nlu_data_axi_w_ready_o(nlu_data_axi_w_ready),
        .nlu_data_axi_w_data_i(nlu_data_axi_w_data),
        .nlu_data_axi_w_strb_i(nlu_data_axi_w_strb),
        .nlu_data_axi_b_valid_o(nlu_data_axi_b_valid),
        .nlu_data_axi_b_ready_i(nlu_data_axi_b_ready),
        .nlu_data_axi_b_resp_o(nlu_data_axi_b_resp),
        .nlu_data_axi_ar_valid_i(nlu_data_axi_ar_valid),
        .nlu_data_axi_ar_ready_o(nlu_data_axi_ar_ready),
        .nlu_data_axi_ar_addr_i(nlu_data_axi_ar_addr),
        .nlu_data_axi_r_valid_o(nlu_data_axi_r_valid),
        .nlu_data_axi_r_ready_i(nlu_data_axi_r_ready),
        .nlu_data_axi_r_data_o(nlu_data_axi_r_data),
        .nlu_data_axi_r_resp_o(nlu_data_axi_r_resp)
    );

    generate
        for (genvar idx = 0; idx < NUM_CLUSTERS; idx++) begin : gen_clusters
            ComputeCluster cluster (
                .clk(clk),
                .reset_n(reset_n),
                .power_enable_i(1'b1),
                .interrupt_o(cluster_irq[idx]),
                .cmd_req_valid_i(cl_cmd_req_valid[idx]),
                .cmd_req_write_i(cl_cmd_req_write[idx]),
                .cmd_req_addr_i(cl_cmd_req_addr[idx]),
                .cmd_req_wdata_i(cl_cmd_req_wdata[idx]),
                .cmd_req_wstrb_i(cl_cmd_req_wstrb[idx]),
                .cmd_req_ready_o(cl_cmd_req_ready[idx]),
                .cmd_resp_valid_o(cl_cmd_resp_valid[idx]),
                .cmd_resp_rdata_o(cl_cmd_resp_rdata[idx]),
                .cmd_resp_err_o(cl_cmd_resp_err[idx]),
                .s_axi_awvalid_i(cl_data_aw_valid[idx]),
                .s_axi_awready_o(cl_data_aw_ready[idx]),
                .s_axi_awaddr_i(cl_data_aw_addr[idx]),
                .s_axi_wvalid_i(cl_data_w_valid[idx]),
                .s_axi_wready_o(cl_data_w_ready[idx]),
                .s_axi_wdata_i(cl_data_w_data[idx]),
                .s_axi_wstrb_i(cl_data_w_strb[idx]),
                .s_axi_bvalid_o(cl_data_b_valid[idx]),
                .s_axi_bready_i(cl_data_b_ready[idx]),
                .s_axi_bresp_o(cl_data_b_resp[idx]),
                .s_axi_arvalid_i(cl_data_ar_valid[idx]),
                .s_axi_arready_o(cl_data_ar_ready[idx]),
                .s_axi_araddr_i(cl_data_ar_addr[idx]),
                .s_axi_rvalid_o(cl_data_r_valid[idx]),
                .s_axi_rready_i(cl_data_r_ready[idx]),
                .s_axi_rdata_o(cl_data_r_data[idx]),
                .s_axi_rresp_o(cl_data_r_resp[idx]),
                .hsel_i(1'b0),
                .haddr_i(32'h0),
                .hwrite_i(1'b0),
                .htrans_i(2'b00),
                .hsize_i(3'b010),
                .hburst_i(3'b000),
                .hprot_i(4'b0011),
                .hready_i(1'b1),
                .hwdata_i(32'h0),
                .hready_o(),
                .hresp_o(),
                .hrdata_o()
            );
        end
    endgenerate

    always_comb begin
        nlu_cmd_resp_valid[0] = 1'b0;
        nlu_cmd_resp_rdata[0] = 32'h0;
        nlu_irq[0] = 1'b0;
        nlu_data_axi_aw_valid = 1'b0;
        nlu_data_axi_aw_addr = 32'h0;
        nlu_data_axi_w_valid = 1'b0;
        nlu_data_axi_w_data = '0;
        nlu_data_axi_w_strb = '0;
        nlu_data_axi_b_ready = 1'b0;
        nlu_data_axi_ar_valid = 1'b0;
        nlu_data_axi_ar_addr = 32'h0;
        nlu_data_axi_r_ready = 1'b0;
    end

endmodule