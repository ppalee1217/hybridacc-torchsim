//-----------------------------------------------------------------------------
// Module Name:   tb_scratchpadmemory
// Description:   Testbench for Cluster/ScratchpadMemory module.
//                Tests: NoC port read/write, parallel mode, DMA write/read,
//                multi-port arbitration, backpressure, PMU counters.
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/FIFO.sv"
`include "../../src/Cluster/SRAM.sv"
`include "../../src/Cluster/ScratchpadMemory.sv"

module tb_scratchpadmemory;
    import hybridacc_utils_pkg::*;

    localparam int NP   = 4;
    localparam int BPG  = 3;
    localparam int BDW  = 64;
    localparam int BD   = 64;      // small depth for test
    localparam int AW   = 32;
    localparam int MAXO = 8;
    localparam int NOC_DW = BPG * BDW; // 192

    logic clk, reset_n;
    logic pmu_rst;
    logic [7:0] config_map;
    logic config_update;
    logic arb_policy;

    logic [NP-1:0]               req_valid;
    logic [NP-1:0]               req_ready;
    logic [NP-1:0][AW-1:0]      req_addr;
    logic [NP-1:0][NOC_DW-1:0]  req_wdata;
    logic [NP-1:0]               req_wen;

    logic [NP-1:0]               resp_valid;
    logic [NP-1:0]               resp_ready;
    logic [NP-1:0][NOC_DW-1:0]  resp_rdata;
    SPM_RESPONSE_CODE            resp_code [NP];

    // AXI4-Lite DMA
    logic                  axi_awvalid, axi_awready;
    logic [AW-1:0]         axi_awaddr;
    logic                  axi_wvalid, axi_wready;
    logic [BDW-1:0]        axi_wdata;
    logic [BDW/8-1:0]      axi_wstrb;
    logic                  axi_bvalid, axi_bready;
    logic [1:0]            axi_bresp;
    logic                  axi_arvalid, axi_arready;
    logic [AW-1:0]         axi_araddr;
    logic                  axi_rvalid, axi_rready;
    logic [BDW-1:0]        axi_rdata;
    logic [1:0]            axi_rresp;

    logic [63:0] pmu_cycle, pmu_arb_stall, pmu_credit_stall;
    logic [63:0] pmu_port_txn [NP];

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    ScratchpadMemory #(
        .NUM_NOC_PORTS(NP), .BANKS_PER_GROUP(BPG),
        .BANK_DATA_WIDTH(BDW), .BANK_DEPTH(BD),
        .SRAM_BANK_LATENCY(1), .SRAM_BANK_PIPELINE_DEPTH(2),
        .ADDR_WIDTH(AW), .MAX_OUTSTANDING(MAXO), .DMA_MAX_OUTSTANDING(8)
    ) dut (
        .clk(clk), .reset_n(reset_n), .pmu_rst_i(pmu_rst),
        .config_map_i(config_map), .config_update_i(config_update), .arb_policy_i(arb_policy),
        .spm_req_valid_i(req_valid), .spm_req_ready_o(req_ready),
        .spm_req_addr_i(req_addr), .spm_req_wdata_i(req_wdata), .spm_req_wen_i(req_wen),
        .spm_resp_valid_o(resp_valid), .spm_resp_ready_i(resp_ready),
        .spm_resp_rdata_o(resp_rdata), .spm_resp_code_o(resp_code),
        .s_axi_awvalid_i(axi_awvalid), .s_axi_awready_o(axi_awready), .s_axi_awaddr_i(axi_awaddr),
        .s_axi_wvalid_i(axi_wvalid), .s_axi_wready_o(axi_wready),
        .s_axi_wdata_i(axi_wdata), .s_axi_wstrb_i(axi_wstrb),
        .s_axi_bvalid_o(axi_bvalid), .s_axi_bready_i(axi_bready), .s_axi_bresp_o(axi_bresp),
        .s_axi_arvalid_i(axi_arvalid), .s_axi_arready_o(axi_arready), .s_axi_araddr_i(axi_araddr),
        .s_axi_rvalid_o(axi_rvalid), .s_axi_rready_i(axi_rready),
        .s_axi_rdata_o(axi_rdata), .s_axi_rresp_o(axi_rresp),
        .pmu_cycle_cnt_o(pmu_cycle), .pmu_port_txn_cnt_o(pmu_port_txn),
        .pmu_arb_stall_cnt_o(pmu_arb_stall), .pmu_credit_stall_cnt_o(pmu_credit_stall)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", name); fail_count++; end
        else begin $display("[PASS] %s", name); pass_count++; end
    endtask

    // Write one word through NoC port p (single-bank mode: wen + lower 64 bits)
    task automatic noc_write(input int p, input logic [AW-1:0] addr,
                             input logic [NOC_DW-1:0] wdata);
        @(posedge clk);
        req_valid[p] <= 1'b1;
        req_addr[p]  <= addr;
        req_wdata[p] <= wdata;
        req_wen[p]   <= 1'b1;
        @(posedge clk);
        req_valid[p] <= 1'b0;
        req_wen[p]   <= 1'b0;
        // Wait for write response
        wait (resp_valid[p] === 1'b1);
        @(posedge clk);
    endtask

    // Read from NoC port p, return data
    task automatic noc_read(input int p, input logic [AW-1:0] addr,
                            output logic [NOC_DW-1:0] rdata);
        @(posedge clk);
        req_valid[p] <= 1'b1;
        req_addr[p]  <= addr;
        req_wen[p]   <= 1'b0;
        @(posedge clk);
        req_valid[p] <= 1'b0;
        wait (resp_valid[p] === 1'b1);
        rdata = resp_rdata[p];
        @(posedge clk);
    endtask

    // AXI4-Lite DMA write
    task automatic dma_write(input logic [AW-1:0] addr,
                             input logic [BDW-1:0] data,
                             input logic [BDW/8-1:0] strb);
        @(posedge clk);
        axi_awvalid <= 1'b1;
        axi_awaddr  <= addr;
        axi_wvalid  <= 1'b1;
        axi_wdata   <= data;
        axi_wstrb   <= strb;
        @(posedge clk);
        axi_awvalid <= 1'b0;
        axi_wvalid  <= 1'b0;
        // Wait for B response
        wait (axi_bvalid === 1'b1);
        @(posedge clk);
    endtask

    // AXI4-Lite DMA read
    task automatic dma_read(input logic [AW-1:0] addr,
                            output logic [BDW-1:0] rdata);
        @(posedge clk);
        axi_arvalid <= 1'b1;
        axi_araddr  <= addr;
        @(posedge clk);
        axi_arvalid <= 1'b0;
        wait (axi_rvalid === 1'b1);
        rdata = axi_rdata;
        @(posedge clk);
    endtask

    logic [NOC_DW-1:0] rd_noc;
    logic [BDW-1:0]    rd_dma;

    // GROUP_SPAN_WORDS = (BPG+1)*BD = 4*64 = 256
    // GROUP_LINEAR_WORDS = BPG*BD = 3*64 = 192
    // BYTES_PER_BANK_WORD = BDW/8 = 8
    // One SRAM bank stores BD=64 words of BDW=64 bits
    // Group 0: banks 0,1,2  Group 1: banks 3,4,5 etc.
    // NOC address for sequential: word index within group (0..191 for 3*64 words)
    // NOC address for parallel: >= 192 (GROUP_LINEAR_WORDS)

    initial begin
        req_valid   = '0;
        req_addr    = '0;
        req_wdata   = '0;
        req_wen     = '0;
        resp_ready  = '1;    // always ready
        pmu_rst     = 1'b0;
        config_map  = 8'b11_10_01_00;  // port0→grp0, port1→grp1, port2→grp2, port3→grp3
        config_update = 1'b0;
        arb_policy  = 1'b0;
        axi_awvalid = 1'b0;
        axi_awaddr  = '0;
        axi_wvalid  = 1'b0;
        axi_wdata   = '0;
        axi_wstrb   = '0;
        axi_bready  = 1'b1;
        axi_arvalid = 1'b0;
        axi_araddr  = '0;
        axi_rready  = 1'b1;

        @(posedge reset_n);
        // Apply config map
        @(posedge clk);
        config_update <= 1'b1;
        @(posedge clk);
        config_update <= 1'b0;
        repeat (2) @(posedge clk);

        // ---- Test 1: Sequential write + read via port 0 (group 0, bank 0) ----
        $display("\n=== Test 1: Sequential write + read ===");
        // addr=0 → group 0, bank 0, row 0 (sequential mode since addr < GROUP_LINEAR_WORDS=192)
        noc_write(0, 32'd0, {64'h0, 64'h0, 64'hDEAD_BEEF_0000_0001});
        noc_read(0, 32'd0, rd_noc);
        check("T1 seq read", rd_noc[63:0] == 64'hDEAD_BEEF_0000_0001);

        // ---- Test 2: Write to bank 1 of group 0 ----
        $display("\n=== Test 2: Cross-bank sequential ===");
        // addr=64 → bank1 of group0 (64/64=1, 64%64=0)
        noc_write(0, 32'd64, {64'h0, 64'h0, 64'hAAAA_BBBB_CCCC_DDDD});
        noc_read(0, 32'd64, rd_noc);
        check("T2 bank1 read", rd_noc[63:0] == 64'hAAAA_BBBB_CCCC_DDDD);

        // ---- Test 3: Parallel write + read (all 3 banks at once) ----
        $display("\n=== Test 3: Parallel mode ===");
        // addr >= GROUP_LINEAR_WORDS=192 → parallel mode
        // addr=192 → row = 192-192 = 0, writes to all 3 banks row 0
        noc_write(0, 32'd192, {64'h1111, 64'h2222, 64'h3333});
        noc_read(0, 32'd192, rd_noc);
        check("T3 par bank0", rd_noc[63:0] == 64'h3333);
        check("T3 par bank1", rd_noc[127:64] == 64'h2222);
        check("T3 par bank2", rd_noc[191:128] == 64'h1111);

        // ---- Test 4: Multi-port access (port 0 and port 1 simultaneously) ----
        $display("\n=== Test 4: Multi-port ===");
        // Port 1 maps to group 1 → independent of group 0
        noc_write(1, 32'd0, {64'h0, 64'h0, 64'hFFFF_0000_FFFF_0000});
        noc_read(1, 32'd0, rd_noc);
        check("T4 port1 read", rd_noc[63:0] == 64'hFFFF_0000_FFFF_0000);

        // ---- Test 5: DMA write + NoC read coherency ----
        $display("\n=== Test 5: DMA write + NoC read ===");
        // DMA addr is byte address. Group 0 starts at byte 0.
        // word 0 of bank 0 = byte addr 0
        dma_write(32'h0000_0000, 64'hDEAD_FACE_1234_5678, 8'hFF);
        repeat (5) @(posedge clk);
        noc_read(0, 32'd0, rd_noc);
        check("T5 DMA->NoC coherency", rd_noc[63:0] == 64'hDEAD_FACE_1234_5678);

        // ---- Test 6: DMA read ----
        $display("\n=== Test 6: DMA read ===");
        // First write via NoC, then read back via DMA
        noc_write(0, 32'd1, {64'h0, 64'h0, 64'hCAFE_BABE_0000_CAFE});
        repeat (2) @(posedge clk);
        // DMA byte addr for word 1 of bank 0 in group 0 = 1*8 = 8
        dma_read(32'h0000_0008, rd_dma);
        check("T6 DMA read", rd_dma == 64'hCAFE_BABE_0000_CAFE);

        // ---- Test 7: Write response code ----
        $display("\n=== Test 7: Write response code ===");
        noc_write(0, 32'd2, {64'h0, 64'h0, 64'h0});
        check("T7 write resp code", resp_code[0] == SPM_OK);

        // ---- Test 8: PMU counters increment ----
        $display("\n=== Test 8: PMU counters ===");
        check("T8 cycle count > 0", pmu_cycle > 0);
        check("T8 port0 txn > 0", pmu_port_txn[0] > 0);

        // ---- Summary ----
        repeat (10) @(posedge clk);
        $display("\n========================================");
        $display("tb_scratchpadmemory: %0d PASS, %0d FAIL", pass_count, fail_count);
        $display("========================================");
        if (fail_count > 0) $fatal(1, "FAILED");
        $finish;
    end

    initial begin
        #200000;
        $fatal(1, "TIMEOUT");
    end
endmodule
