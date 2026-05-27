//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_spm
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Smoke-level testbench for ScratchpadMemory.
//                Covers:
//                  * linear write/read
//                  * parallel write/read
//                  * config_map port->group remap
//                  * DMA write/read
//                  * response backpressure retention
//                  * soft reset clears outstanding response
// Dependencies:  ../tb_common.svh, src/hybridacc_utils_pkg.sv,
//                src/Cluster/cluster_pkg.sv,
//                src/Cluster/ScratchpadMemoryBank.sv,
//                src/Cluster/ScratchpadMemory.sv
// Revision:
//   2026/04/27 - Initial version (M1 cluster datapath rewrite)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/Cluster/cluster_pkg.sv"
`include "../../src/Cluster/ScratchpadMemoryBank.sv"
`include "../../src/Cluster/ScratchpadMemory.sv"
`endif

module tb_spm;
    import hybridacc_utils_pkg::*;
    import cluster_pkg::*;

    localparam int NUM_PORTS = 4;

    logic clk, reset_n;
    logic pmu_rst_i;
    logic soft_reset_i;
    logic [7:0] config_map_i;
    logic config_update_i;

    logic            spm_req_valid_i [NUM_PORTS];
    logic            spm_req_ready_o [NUM_PORTS];
    spm_req_32_192_t spm_req_i       [NUM_PORTS];
    logic            spm_resp_valid_o[NUM_PORTS];
    logic            spm_resp_ready_i[NUM_PORTS];
    spm_resp_192_t   spm_resp_o      [NUM_PORTS];

    logic         s_axi_awvalid_i;
    logic         s_axi_awready_o;
    logic [31:0]  s_axi_awaddr_i;
    logic         s_axi_wvalid_i;
    logic         s_axi_wready_o;
    logic [63:0]  s_axi_wdata_i;
    logic [7:0]   s_axi_wstrb_i;
    logic         s_axi_bvalid_o;
    logic         s_axi_bready_i;
    logic [1:0]   s_axi_bresp_o;
    logic         s_axi_arvalid_i;
    logic         s_axi_arready_o;
    logic [31:0]  s_axi_araddr_i;
    logic         s_axi_rvalid_o;
    logic         s_axi_rready_i;
    logic [63:0]  s_axi_rdata_o;
    logic [1:0]   s_axi_rresp_o;
    logic [63:0]  pmu_cycle_cnt_o;
    logic [63:0]  pmu_port_txn_cnt_o[NUM_PORTS];
    logic [63:0]  pmu_arb_stall_cnt_o;
    logic [63:0]  pmu_credit_stall_cnt_o;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    ScratchpadMemory dut (
        .clk(clk),
        .reset_n(reset_n),
        .pmu_rst_i(pmu_rst_i),
        .soft_reset_i(soft_reset_i),
        .config_map_i(config_map_i),
        .config_update_i(config_update_i),
        .spm_req_valid_i(spm_req_valid_i),
        .spm_req_ready_o(spm_req_ready_o),
        .spm_req_i(spm_req_i),
        .spm_resp_valid_o(spm_resp_valid_o),
        .spm_resp_ready_i(spm_resp_ready_i),
        .spm_resp_o(spm_resp_o),
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
        .pmu_cycle_cnt_o(pmu_cycle_cnt_o),
        .pmu_port_txn_cnt_o(pmu_port_txn_cnt_o),
        .pmu_arb_stall_cnt_o(pmu_arb_stall_cnt_o),
        .pmu_credit_stall_cnt_o(pmu_credit_stall_cnt_o)
    );

    task automatic clear_noc_port(input int port);
        spm_req_valid_i[port] = 1'b0;
        spm_req_i[port] = '0;
        spm_resp_ready_i[port] = 1'b0;
    endtask

    task automatic noc_write_linear(input int port, input logic [31:0] laddr, input logic [63:0] data64);
        spm_req_i[port].addr  = laddr;
        spm_req_i[port].wdata = '0;
        spm_req_i[port].wdata[63:0] = data64;
        spm_req_i[port].wen   = 1'b1;
        spm_req_valid_i[port] = 1'b1;
        while (!spm_req_ready_o[port]) @(posedge clk);
        @(posedge clk);
        spm_req_valid_i[port] = 1'b0;
        spm_resp_ready_i[port] = 1'b1;
        while (!spm_resp_valid_o[port]) @(posedge clk);
        @(posedge clk);
        spm_resp_ready_i[port] = 1'b0;
    endtask

    task automatic noc_read_linear(input int port, input logic [31:0] laddr, input logic [63:0] expected);
        spm_req_i[port].addr  = laddr;
        spm_req_i[port].wdata = '0;
        spm_req_i[port].wen   = 1'b0;
        spm_req_valid_i[port] = 1'b1;
        while (!spm_req_ready_o[port]) @(posedge clk);
        @(posedge clk);
        spm_req_valid_i[port] = 1'b0;
        spm_resp_ready_i[port] = 1'b1;
        while (!spm_resp_valid_o[port]) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL($sformatf("Linear read p%0d low64", port), spm_resp_o[port].rdata[63:0], expected)
        `CHECK_VAL($sformatf("Linear read p%0d high128", port), spm_resp_o[port].rdata[191:64], 128'h0)
        @(posedge clk);
        spm_resp_ready_i[port] = 1'b0;
    endtask

    task automatic noc_write_parallel(input int port, input logic [31:0] row, input logic [191:0] payload);
        spm_req_i[port].addr  = 32'(3*8192 + row);
        spm_req_i[port].wdata = payload;
        spm_req_i[port].wen   = 1'b1;
        spm_req_valid_i[port] = 1'b1;
        while (!spm_req_ready_o[port]) @(posedge clk);
        @(posedge clk);
        spm_req_valid_i[port] = 1'b0;
        spm_resp_ready_i[port] = 1'b1;
        while (!spm_resp_valid_o[port]) @(posedge clk);
        @(posedge clk);
        spm_resp_ready_i[port] = 1'b0;
    endtask

    task automatic noc_read_parallel(input int port, input logic [31:0] row, input logic [191:0] expected);
        spm_req_i[port].addr  = 32'(3*8192 + row);
        spm_req_i[port].wdata = '0;
        spm_req_i[port].wen   = 1'b0;
        spm_req_valid_i[port] = 1'b1;
        while (!spm_req_ready_o[port]) @(posedge clk);
        @(posedge clk);
        spm_req_valid_i[port] = 1'b0;
        spm_resp_ready_i[port] = 1'b1;
        while (!spm_resp_valid_o[port]) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL($sformatf("Parallel read p%0d 192b", port), spm_resp_o[port].rdata, expected)
        @(posedge clk);
        spm_resp_ready_i[port] = 1'b0;
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
        #(`TB_SETTLE);
        `CHECK_VAL("DMA write bresp=OK", s_axi_bresp_o, 2'b00)
        @(posedge clk);
        s_axi_bready_i = 1'b0;
    endtask

    task automatic dma_read64(input logic [31:0] byte_addr, input logic [63:0] expected);
        @(negedge clk);
        s_axi_araddr_i  = byte_addr;
        s_axi_arvalid_i = 1'b1;
        while (!s_axi_arready_o) @(posedge clk);
        @(posedge clk);
        s_axi_arvalid_i = 1'b0;

        s_axi_rready_i = 1'b1;
        while (!s_axi_rvalid_o) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL("DMA read rresp=OK", s_axi_rresp_o, 2'b00)
        `CHECK_VAL("DMA read rdata", s_axi_rdata_o, expected)
        @(posedge clk);
        s_axi_rready_i = 1'b0;
    endtask

    initial begin
        pmu_rst_i = 0;
        soft_reset_i = 0;
        config_map_i = 8'hE4; // default: p0->0, p1->1, p2->2, p3->3
        config_update_i = 0;
        s_axi_awvalid_i = 0;
        s_axi_awaddr_i = 0;
        s_axi_wvalid_i = 0;
        s_axi_wdata_i = 0;
        s_axi_wstrb_i = 0;
        s_axi_bready_i = 0;
        s_axi_arvalid_i = 0;
        s_axi_araddr_i = 0;
        s_axi_rready_i = 0;
        for (int p = 0; p < NUM_PORTS; p++) begin
            clear_noc_port(p);
        end

        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // -------------------------------------------------------------
        // Test 1: linear write/read on port0 -> group0
        // -------------------------------------------------------------
        noc_write_linear(0, 32'd5, 64'h1122_3344_5566_7788);
        noc_read_linear(0, 32'd5, 64'h1122_3344_5566_7788);

        // -------------------------------------------------------------
        // Test 2: parallel write/read on port0 group0
        // -------------------------------------------------------------
        noc_write_parallel(0, 32'd7, 192'hCCCC_CCCC_DDDD_DDDD_EEEE_EEEE_FFFF_FFFF_1111_1111_2222_2222);
        noc_read_parallel(0, 32'd7, 192'hCCCC_CCCC_DDDD_DDDD_EEEE_EEEE_FFFF_FFFF_1111_1111_2222_2222);

        // -------------------------------------------------------------
        // Test 3: config_map remap port1 -> group0 then cross-port observe
        // -------------------------------------------------------------
        @(negedge clk);
        config_map_i = 8'hE0; // p0->0, p1->0, p2->2, p3->3
        config_update_i = 1'b1;
        @(posedge clk);
        @(negedge clk);
        config_update_i = 1'b0;
        noc_write_linear(1, 32'd9, 64'hA5A5_5A5A_1234_5678);
        noc_read_linear(0, 32'd9, 64'hA5A5_5A5A_1234_5678);

        // -------------------------------------------------------------
        // Test 4: DMA write/read and NoC observe shared storage
        // target: group0 bank1 row3 => local linear addr = 8192 + 3, byte addr * 8
        // -------------------------------------------------------------
        dma_write64((32'd8195) * 8, 64'hDEAD_BEEF_CAFE_1234);
        dma_read64((32'd8195) * 8, 64'hDEAD_BEEF_CAFE_1234);
        noc_read_linear(0, 32'd8195, 64'hDEAD_BEEF_CAFE_1234);

        // -------------------------------------------------------------
        // Test 5: response backpressure hold
        // -------------------------------------------------------------
        spm_req_i[0].addr  = 32'd5;
        spm_req_i[0].wdata = '0;
        spm_req_i[0].wen   = 1'b0;
        spm_req_valid_i[0] = 1'b1;
        while (!spm_req_ready_o[0]) @(posedge clk);
        @(posedge clk);
        spm_req_valid_i[0] = 1'b0;
        spm_resp_ready_i[0] = 1'b0;
        while (!spm_resp_valid_o[0]) @(posedge clk);
        repeat (3) begin
            @(posedge clk); #(`TB_SETTLE);
            `CHECK_BIT("Backpressure: resp_valid held", spm_resp_valid_o[0], 1'b1)
        end
        spm_resp_ready_i[0] = 1'b1;
        @(posedge clk);
        spm_resp_ready_i[0] = 1'b0;

        // -------------------------------------------------------------
        // Test 6: soft reset clears outstanding response
        // -------------------------------------------------------------
        spm_req_i[0].addr  = 32'd5;
        spm_req_i[0].wdata = '0;
        spm_req_i[0].wen   = 1'b0;
        spm_req_valid_i[0] = 1'b1;
        while (!spm_req_ready_o[0]) @(posedge clk);
        @(posedge clk);
        spm_req_valid_i[0] = 1'b0;
        while (!spm_resp_valid_o[0]) @(posedge clk);
        @(negedge clk);
        soft_reset_i = 1'b1;
        @(posedge clk); #(`TB_SETTLE);
        `CHECK_BIT("Soft reset clears resp_valid", spm_resp_valid_o[0], 1'b0)
        @(negedge clk);
        soft_reset_i = 1'b0;

        // Simple PMU sanity: cycle counter moved and port0 saw traffic.
        `CHECK_COND("PMU cycle counter increments", pmu_cycle_cnt_o > 0, pmu_cycle_cnt_o)
        `CHECK_COND("PMU port0 txn counter nonzero", pmu_port_txn_cnt_o[0] > 0, pmu_port_txn_cnt_o[0])

        `TB_SUMMARY("tb_spm")
        $finish;
    end

    initial begin
        #400000;
        $error("[TB_TIMEOUT] tb_spm did not finish in time");
        `TB_SUMMARY("tb_spm")
        $finish;
    end

endmodule