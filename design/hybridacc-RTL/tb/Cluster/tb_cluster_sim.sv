//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_cluster_sim
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Cluster top smoke test via AHB-lite + DMA.
// Dependencies:  ../tb_common.svh, Cluster source stack.
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "cluster_rtl_stack.svh"
`endif

module tb_cluster_sim;
    import cluster_pkg::*;

    logic clk, reset_n;
    logic power_enable_i;
    logic interrupt_o;
    logic cmd_req_valid_i;
    logic cmd_req_write_i;
    logic [31:0] cmd_req_addr_i;
    logic [31:0] cmd_req_wdata_i;
    logic [3:0]  cmd_req_wstrb_i;
    logic cmd_req_ready_o;
    logic cmd_resp_valid_o;
    logic [31:0] cmd_resp_rdata_o;
    logic cmd_resp_err_o;
    logic s_axi_awvalid_i;
    logic s_axi_awready_o;
    logic [31:0] s_axi_awaddr_i;
    logic s_axi_wvalid_i;
    logic s_axi_wready_o;
    logic [63:0] s_axi_wdata_i;
    logic [7:0]  s_axi_wstrb_i;
    logic s_axi_bvalid_o;
    logic s_axi_bready_i;
    logic [1:0] s_axi_bresp_o;
    logic s_axi_arvalid_i;
    logic s_axi_arready_o;
    logic [31:0] s_axi_araddr_i;
    logic s_axi_rvalid_o;
    logic s_axi_rready_i;
    logic [63:0] s_axi_rdata_o;
    logic [1:0] s_axi_rresp_o;
    logic hsel_i;
    logic [31:0] haddr_i;
    logic hwrite_i;
    logic [1:0] htrans_i;
    logic [2:0] hsize_i;
    logic [2:0] hburst_i;
    logic [3:0] hprot_i;
    logic hready_i;
    logic [31:0] hwdata_i;
    logic hready_o;
    logic hresp_o;
    logic [31:0] hrdata_o;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    ComputeCluster dut (
        .clk(clk),
        .reset_n(reset_n),
        .power_enable_i(power_enable_i),
        .interrupt_o(interrupt_o),
        .cmd_req_valid_i(cmd_req_valid_i),
        .cmd_req_write_i(cmd_req_write_i),
        .cmd_req_addr_i(cmd_req_addr_i),
        .cmd_req_wdata_i(cmd_req_wdata_i),
        .cmd_req_wstrb_i(cmd_req_wstrb_i),
        .cmd_req_ready_o(cmd_req_ready_o),
        .cmd_resp_valid_o(cmd_resp_valid_o),
        .cmd_resp_rdata_o(cmd_resp_rdata_o),
        .cmd_resp_err_o(cmd_resp_err_o),
        .s_axi_awvalid_i(s_axi_awvalid_i),
        .s_axi_awready_o(s_axi_awready_o),
        .s_axi_awaddr_i(s_axi_awaddr_i),
        .s_axi_wvalid_i(s_axi_wvalid_i),
        .s_axi_wready_o(s_axi_wready_o),
        .s_axi_wdata_i(s_axi_wdata_i),
        .s_axi_wstrb_i(s_axi_wstrb_i),
        .s_axi_bvalid_o(s_axi_bvalid_o),
        .s_axi_bready_i(s_axi_bready_i),
        .s_axi_bresp_o(s_axi_bresp_o),
        .s_axi_arvalid_i(s_axi_arvalid_i),
        .s_axi_arready_o(s_axi_arready_o),
        .s_axi_araddr_i(s_axi_araddr_i),
        .s_axi_rvalid_o(s_axi_rvalid_o),
        .s_axi_rready_i(s_axi_rready_i),
        .s_axi_rdata_o(s_axi_rdata_o),
        .s_axi_rresp_o(s_axi_rresp_o),
        .hsel_i(hsel_i),
        .haddr_i(haddr_i),
        .hwrite_i(hwrite_i),
        .htrans_i(htrans_i),
        .hsize_i(hsize_i),
        .hburst_i(hburst_i),
        .hprot_i(hprot_i),
        .hready_i(hready_i),
        .hwdata_i(hwdata_i),
        .hready_o(hready_o),
        .hresp_o(hresp_o),
        .hrdata_o(hrdata_o)
    );

    task automatic ahb_write(input logic [31:0] addr, input logic [31:0] data);
        @(negedge clk);
        hsel_i   = 1'b1;
        haddr_i  = addr;
        hwrite_i = 1'b1;
        htrans_i = 2'b10;
        hwdata_i = data;
        @(posedge clk);
        @(negedge clk);
        hsel_i   = 1'b0;
        hwrite_i = 1'b0;
        htrans_i = 2'b00;
        hwdata_i = 32'h0;
    endtask

    task automatic ahb_read(input logic [31:0] addr, output logic [31:0] data);
        @(negedge clk);
        hsel_i   = 1'b1;
        haddr_i  = addr;
        hwrite_i = 1'b0;
        htrans_i = 2'b10;
        #(`TB_SETTLE);
        data = hrdata_o;
        @(posedge clk);
        @(negedge clk);
        hsel_i   = 1'b0;
        htrans_i = 2'b00;
    endtask

    task automatic dma_write64(input logic [31:0] byte_addr, input logic [63:0] data64);
        @(negedge clk);
        s_axi_awaddr_i  = byte_addr;
        s_axi_awvalid_i = 1'b1;
        while (!s_axi_awready_o) @(posedge clk);
        @(posedge clk);
        s_axi_awvalid_i = 1'b0;

        @(negedge clk);
        s_axi_wdata_i  = data64;
        s_axi_wstrb_i  = 8'hFF;
        s_axi_wvalid_i = 1'b1;
        while (!s_axi_wready_o) @(posedge clk);
        @(posedge clk);
        s_axi_wvalid_i = 1'b0;
        s_axi_bready_i = 1'b1;
        while (!s_axi_bvalid_o) @(posedge clk);
        @(posedge clk);
        s_axi_bready_i = 1'b0;
    endtask

    task automatic dma_read64(input logic [31:0] byte_addr, output logic [63:0] data64);
        @(negedge clk);
        s_axi_araddr_i  = byte_addr;
        s_axi_arvalid_i = 1'b1;
        while (!s_axi_arready_o) @(posedge clk);
        @(posedge clk);
        s_axi_arvalid_i = 1'b0;
        s_axi_rready_i = 1'b1;
        while (!s_axi_rvalid_o) @(posedge clk);
        #(`TB_SETTLE);
        data64 = s_axi_rdata_o;
        @(posedge clk);
        s_axi_rready_i = 1'b0;
    endtask

    initial begin
        power_enable_i = 1'b0;
        cmd_req_valid_i = 1'b0;
        cmd_req_write_i = 1'b0;
        cmd_req_addr_i = 32'h0;
        cmd_req_wdata_i = 32'h0;
        cmd_req_wstrb_i = 4'h0;
        s_axi_awvalid_i = 1'b0;
        s_axi_awaddr_i = 32'h0;
        s_axi_wvalid_i = 1'b0;
        s_axi_wdata_i = 64'h0;
        s_axi_wstrb_i = 8'h0;
        s_axi_bready_i = 1'b0;
        s_axi_arvalid_i = 1'b0;
        s_axi_araddr_i = 32'h0;
        s_axi_rready_i = 1'b0;
        hsel_i = 1'b0;
        haddr_i = 32'h0;
        hwrite_i = 1'b0;
        htrans_i = 2'b00;
        hsize_i = 3'b010;
        hburst_i = 3'b000;
        hprot_i = 4'b0011;
        hready_i = 1'b1;
        hwdata_i = 32'h0;

        @(posedge reset_n);
        @(posedge clk); @(negedge clk);
        power_enable_i = 1'b1;
        @(posedge clk); @(negedge clk);

        begin
            logic [31:0] r32;
            logic [63:0] r64;

            ahb_write(32'h0000_0000, 32'h0000_00C6);
            ahb_read(32'h0000_0000, r32);
            `CHECK_VAL("Cluster AHB SPM cfg_map", r32, 32'h0000_00C6)

            ahb_read(32'h0000_1810, r32);
            `CHECK_VAL("Cluster AHB HDDU NUM_PLANES", r32, 32'd4)

            ahb_write(32'h0000_2100, MODE_DIRECT_DEBUG);
            ahb_read(32'h0000_2100, r32);
            `CHECK_VAL("Cluster AHB mode readback", r32, MODE_DIRECT_DEBUG)

            ahb_write(32'h0000_2000, 32'h0000_0003);
            ahb_read(32'h0000_2000, r32);
            `CHECK_VAL("Cluster AHB noc cmd mirror", r32, 32'h0000_0003)

            dma_write64(32'd64, 64'h0123_4567_89AB_CDEF);
            dma_read64(32'd64, r64);
            `CHECK_VAL("Cluster AHB+DMA path", r64, 64'h0123_4567_89AB_CDEF)
        end

        `TB_SUMMARY("tb_cluster_sim")
        $finish;
    end

    initial begin
        #1000000;
        $error("[TB_TIMEOUT] tb_cluster_sim did not finish in time");
        `TB_SUMMARY("tb_cluster_sim")
        $finish;
    end
endmodule