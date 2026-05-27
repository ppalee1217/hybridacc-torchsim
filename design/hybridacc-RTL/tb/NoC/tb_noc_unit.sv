//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_noc_unit
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for NoCRouter module — exercises FIFOs, ultra/
//                broadcast, scan-chain, PLO response collection, and
//                backpressure.
// Dependencies:  tb_common.svh, hybridacc_utils_pkg.sv, FIFO.sv, NoCRouter.sv
// Revision:
//   2026/03/28 - Initial version
//   2026/04/02 - Full rewrite for FIFO-based NoCRouter
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/hybridacc_utils_pkg.sv"
`endif
`ifndef GATE_SIM
`include "../../src/FIFO.sv"
`include "../../src/NoC/NoCRouter.sv"
`endif

module tb_noc_unit;
    import hybridacc_utils_pkg::*;

    localparam int NP = 3;
    localparam int PW = 64;
    localparam int FD = 4; // FIFO depth

    logic clk, reset_n;
    logic command_mode;
    logic [31:0] command_data;

    // PS
    logic [NP*PW-1:0] noc_ps_in_data;
    logic [15:0]       noc_ps_in_addr;
    logic [63:0]       noc_ps_in_mask;
    logic noc_ps_in_valid, noc_ps_in_ready;
    // PD
    logic [NP*PW-1:0] noc_pd_in_data;
    logic [15:0]       noc_pd_in_addr;
    logic [63:0]       noc_pd_in_mask;
    logic noc_pd_in_valid, noc_pd_in_ready;
    // PLI
    logic [NP*PW-1:0] noc_pli_in_data;
    logic [15:0]       noc_pli_in_addr;
    logic [63:0]       noc_pli_in_mask;
    logic noc_pli_in_valid, noc_pli_in_ready;

    noc_addr_req_t noc_plo_in_data;
    logic noc_plo_in_valid, noc_plo_in_ready;

    logic [NP*PW-1:0] noc_plo_out_data;
    NOC_RESPONSE_STATUS noc_plo_out_status;
    logic noc_plo_out_valid, noc_plo_out_ready;

    noc_request_t noc_ps_to_bus_req_data[NP];
    logic noc_ps_to_bus_req_valid[NP], noc_ps_to_bus_req_ready[NP];
    noc_request_t noc_pd_to_bus_req_data[NP];
    logic noc_pd_to_bus_req_valid[NP], noc_pd_to_bus_req_ready[NP];
    noc_request_t noc_pli_to_bus_req_data[NP];
    logic noc_pli_to_bus_req_valid[NP], noc_pli_to_bus_req_ready[NP];
    noc_addr_req_t noc_plo_to_bus_req_data[NP];
    logic noc_plo_to_bus_req_valid[NP], noc_plo_to_bus_req_ready[NP];

    noc_response_t bus_to_noc_plo_resp_data[NP];
    logic bus_to_noc_plo_resp_valid[NP], bus_to_noc_plo_resp_ready[NP];

    logic scan_chain_enable;
    ScanChainFormat scan_chain_in[NP], scan_chain_out[NP];

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

`ifdef GATE_SIM
    // ---- Flat-wire intermediates for gate-level NoCRouter (NUM_PORTS=3) ----
    localparam int NRQ = $bits(noc_request_t);   // 144
    localparam int NAQ = $bits(noc_addr_req_t);  // 16
    localparam int NRS = $bits(noc_response_t);  // 66
    localparam int SCW = $bits(ScanChainFormat);  // 27

    // DUT output arrays
    wire [NP*NRQ-1:0]    g_noc_ps_to_bus_req_data;
    wire [0:NP-1]         g_noc_ps_to_bus_req_valid;
    wire [NP*NRQ-1:0]    g_noc_pd_to_bus_req_data;
    wire [0:NP-1]         g_noc_pd_to_bus_req_valid;
    wire [NP*NRQ-1:0]    g_noc_pli_to_bus_req_data;
    wire [0:NP-1]         g_noc_pli_to_bus_req_valid;
    wire [NP*NAQ-1:0]    g_noc_plo_to_bus_req_data;
    wire [0:NP-1]         g_noc_plo_to_bus_req_valid;
    wire [0:NP-1]         g_bus_to_noc_plo_resp_ready;
    wire [NP*SCW-1:0]    g_scan_chain_out;
    // DUT input arrays
    wire [0:NP-1]         g_noc_ps_to_bus_req_ready;
    wire [0:NP-1]         g_noc_pd_to_bus_req_ready;
    wire [0:NP-1]         g_noc_pli_to_bus_req_ready;
    wire [0:NP-1]         g_noc_plo_to_bus_req_ready;
    wire [NP*NRS-1:0]    g_bus_to_noc_plo_resp_data;
    wire [0:NP-1]         g_bus_to_noc_plo_resp_valid;
    wire [NP*SCW-1:0]    g_scan_chain_in;
    // Enum output
    wire [1:0]            g_noc_plo_out_status;
    assign noc_plo_out_status = NOC_RESPONSE_STATUS'(g_noc_plo_out_status);

    genvar gi;
    generate for (gi = 0; gi < NP; gi++) begin : g_conv
        // DUT outputs → TB
        assign noc_ps_to_bus_req_data[gi]    = g_noc_ps_to_bus_req_data[(NP-1-gi)*NRQ +: NRQ];
        assign noc_ps_to_bus_req_valid[gi]   = g_noc_ps_to_bus_req_valid[gi];
        assign noc_pd_to_bus_req_data[gi]    = g_noc_pd_to_bus_req_data[(NP-1-gi)*NRQ +: NRQ];
        assign noc_pd_to_bus_req_valid[gi]   = g_noc_pd_to_bus_req_valid[gi];
        assign noc_pli_to_bus_req_data[gi]   = g_noc_pli_to_bus_req_data[(NP-1-gi)*NRQ +: NRQ];
        assign noc_pli_to_bus_req_valid[gi]  = g_noc_pli_to_bus_req_valid[gi];
        assign noc_plo_to_bus_req_data[gi]   = g_noc_plo_to_bus_req_data[(NP-1-gi)*NAQ +: NAQ];
        assign noc_plo_to_bus_req_valid[gi]  = g_noc_plo_to_bus_req_valid[gi];
        assign bus_to_noc_plo_resp_ready[gi] = g_bus_to_noc_plo_resp_ready[gi];
        assign scan_chain_out[gi]            = g_scan_chain_out[(NP-1-gi)*SCW +: SCW];
        // TB → DUT inputs
        assign g_noc_ps_to_bus_req_ready[gi]              = noc_ps_to_bus_req_ready[gi];
        assign g_noc_pd_to_bus_req_ready[gi]              = noc_pd_to_bus_req_ready[gi];
        assign g_noc_pli_to_bus_req_ready[gi]             = noc_pli_to_bus_req_ready[gi];
        assign g_noc_plo_to_bus_req_ready[gi]             = noc_plo_to_bus_req_ready[gi];
        assign g_bus_to_noc_plo_resp_data[(NP-1-gi)*NRS +: NRS]  = bus_to_noc_plo_resp_data[gi];
        assign g_bus_to_noc_plo_resp_valid[gi]            = bus_to_noc_plo_resp_valid[gi];
        assign g_scan_chain_in[(NP-1-gi)*SCW +: SCW]             = scan_chain_in[gi];
    end endgenerate

    NoCRouter dut (
        .clk(clk), .reset_n(reset_n), .command_mode(command_mode), .command_data(command_data),
        .noc_ps_in_data(noc_ps_in_data), .noc_ps_in_addr(noc_ps_in_addr), .noc_ps_in_mask(noc_ps_in_mask),
        .noc_ps_in_valid(noc_ps_in_valid), .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data), .noc_pd_in_addr(noc_pd_in_addr), .noc_pd_in_mask(noc_pd_in_mask),
        .noc_pd_in_valid(noc_pd_in_valid), .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data), .noc_pli_in_addr(noc_pli_in_addr), .noc_pli_in_mask(noc_pli_in_mask),
        .noc_pli_in_valid(noc_pli_in_valid), .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data), .noc_plo_in_valid(noc_plo_in_valid), .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data), .noc_plo_out_status(g_noc_plo_out_status),
        .noc_plo_out_valid(noc_plo_out_valid), .noc_plo_out_ready(noc_plo_out_ready),
        .noc_ps_to_bus_req_data(g_noc_ps_to_bus_req_data), .noc_ps_to_bus_req_valid(g_noc_ps_to_bus_req_valid), .noc_ps_to_bus_req_ready(g_noc_ps_to_bus_req_ready),
        .noc_pd_to_bus_req_data(g_noc_pd_to_bus_req_data), .noc_pd_to_bus_req_valid(g_noc_pd_to_bus_req_valid), .noc_pd_to_bus_req_ready(g_noc_pd_to_bus_req_ready),
        .noc_pli_to_bus_req_data(g_noc_pli_to_bus_req_data), .noc_pli_to_bus_req_valid(g_noc_pli_to_bus_req_valid), .noc_pli_to_bus_req_ready(g_noc_pli_to_bus_req_ready),
        .noc_plo_to_bus_req_data(g_noc_plo_to_bus_req_data), .noc_plo_to_bus_req_valid(g_noc_plo_to_bus_req_valid), .noc_plo_to_bus_req_ready(g_noc_plo_to_bus_req_ready),
        .bus_to_noc_plo_resp_data(g_bus_to_noc_plo_resp_data), .bus_to_noc_plo_resp_valid(g_bus_to_noc_plo_resp_valid), .bus_to_noc_plo_resp_ready(g_bus_to_noc_plo_resp_ready),
        .scan_chain_enable(scan_chain_enable), .scan_chain_in(g_scan_chain_in), .scan_chain_out(g_scan_chain_out)
    );
`else
    NoCRouter #(.NUM_PORTS(NP), .PORT_WIDTH_BITS(PW), .FIFO_DEPTH(FD)) dut (
        .clk(clk), .reset_n(reset_n), .command_mode(command_mode), .command_data(command_data),
        .noc_ps_in_data(noc_ps_in_data), .noc_ps_in_addr(noc_ps_in_addr), .noc_ps_in_mask(noc_ps_in_mask),
        .noc_ps_in_valid(noc_ps_in_valid), .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data), .noc_pd_in_addr(noc_pd_in_addr), .noc_pd_in_mask(noc_pd_in_mask),
        .noc_pd_in_valid(noc_pd_in_valid), .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data), .noc_pli_in_addr(noc_pli_in_addr), .noc_pli_in_mask(noc_pli_in_mask),
        .noc_pli_in_valid(noc_pli_in_valid), .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data), .noc_plo_in_valid(noc_plo_in_valid), .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data), .noc_plo_out_status(noc_plo_out_status),
        .noc_plo_out_valid(noc_plo_out_valid), .noc_plo_out_ready(noc_plo_out_ready),
        .noc_ps_to_bus_req_data(noc_ps_to_bus_req_data), .noc_ps_to_bus_req_valid(noc_ps_to_bus_req_valid), .noc_ps_to_bus_req_ready(noc_ps_to_bus_req_ready),
        .noc_pd_to_bus_req_data(noc_pd_to_bus_req_data), .noc_pd_to_bus_req_valid(noc_pd_to_bus_req_valid), .noc_pd_to_bus_req_ready(noc_pd_to_bus_req_ready),
        .noc_pli_to_bus_req_data(noc_pli_to_bus_req_data), .noc_pli_to_bus_req_valid(noc_pli_to_bus_req_valid), .noc_pli_to_bus_req_ready(noc_pli_to_bus_req_ready),
        .noc_plo_to_bus_req_data(noc_plo_to_bus_req_data), .noc_plo_to_bus_req_valid(noc_plo_to_bus_req_valid), .noc_plo_to_bus_req_ready(noc_plo_to_bus_req_ready),
        .bus_to_noc_plo_resp_data(bus_to_noc_plo_resp_data), .bus_to_noc_plo_resp_valid(bus_to_noc_plo_resp_valid), .bus_to_noc_plo_resp_ready(bus_to_noc_plo_resp_ready),
        .scan_chain_enable(scan_chain_enable), .scan_chain_in(scan_chain_in), .scan_chain_out(scan_chain_out)
    );
`endif

    task automatic clear_inputs();
        command_mode = 1'b0;
        command_data = 32'h0;
        noc_ps_in_data = '0; noc_ps_in_addr = '0; noc_ps_in_mask = '0;
        noc_ps_in_valid = 1'b0;
        noc_pd_in_data = '0; noc_pd_in_addr = '0; noc_pd_in_mask = '0;
        noc_pd_in_valid = 1'b0;
        noc_pli_in_data = '0; noc_pli_in_addr = '0; noc_pli_in_mask = '0;
        noc_pli_in_valid = 1'b0;
        noc_plo_in_data = '0;
        noc_plo_in_valid = 1'b0;
        noc_plo_out_ready = 1'b1;

        for (int i = 0; i < NP; i++) begin
            noc_ps_to_bus_req_ready[i] = 1'b1;
            noc_pd_to_bus_req_ready[i] = 1'b1;
            noc_pli_to_bus_req_ready[i] = 1'b1;
            noc_plo_to_bus_req_ready[i] = 1'b1;
            bus_to_noc_plo_resp_data[i] = '0;
            bus_to_noc_plo_resp_valid[i] = 1'b0;
            scan_chain_in[i] = '0;
        end
    endtask

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    // Helper: send a request and wait for acceptance (handshake)
    task automatic send_ps_req(input logic [NP*PW-1:0] data, input logic [15:0] addr, input logic [63:0] mask);
        noc_ps_in_data  = data;
        noc_ps_in_addr  = addr;
        noc_ps_in_mask  = mask;
        noc_ps_in_valid = 1'b1;
        @(posedge clk);
        while (!noc_ps_in_ready) @(posedge clk);
        noc_ps_in_valid = 1'b0;
    endtask

    task automatic send_plo_req(input logic [15:0] addr);
        noc_plo_in_data.addr = addr;
        noc_plo_in_valid = 1'b1;
        @(posedge clk);
        while (!noc_plo_in_ready) @(posedge clk);
        noc_plo_in_valid = 1'b0;
    endtask

    initial begin
        clear_inputs();
        @(posedge reset_n);
        repeat (2) @(posedge clk);
        #(`TB_SETTLE);

        // ========================================
        // Test 1: Ready signals after reset (FIFO empty → accept)
        // ========================================
        $display("\n=== Test 1: Ready after reset ===");
        check("Ready: PS",  noc_ps_in_ready  === 1'b0); // no valid → ready=0 (must have valid)
        // Actually, ready = valid && !full. With valid=0, ready=0. That's correct, but
        // ESL sets ready = !full when valid is high — matching.
        // Let's assert valid temporarily:
        noc_ps_in_valid = 1'b1; #(`TB_SETTLE);
        check("Ready: PS (valid=1)", noc_ps_in_ready === 1'b1);
        noc_ps_in_valid = 1'b0;
        noc_pd_in_valid = 1'b1; #(`TB_SETTLE);
        check("Ready: PD (valid=1)", noc_pd_in_ready === 1'b1);
        noc_pd_in_valid = 1'b0;
        noc_pli_in_valid = 1'b1; #(`TB_SETTLE);
        check("Ready: PLI (valid=1)", noc_pli_in_ready === 1'b1);
        noc_pli_in_valid = 1'b0;
        noc_plo_in_valid = 1'b1; #(`TB_SETTLE);
        check("Ready: PLO (valid=1)", noc_plo_in_ready === 1'b1);
        noc_plo_in_valid = 1'b0;

        // ========================================
        // Test 2: Scan-chain command
        // ========================================
        $display("\n=== Test 2: Scan-chain command ===");
        command_mode = 1'b1;
        command_data = {28'h0, CMD_NOC_SCAN_CHAIN};
        @(posedge clk); // FF captures scan_chain_enable_next=1 → reg=1
        #(`TB_SETTLE);
        check("ScanChain: enable asserted after 1 cycle", scan_chain_enable === 1'b1);
        command_mode = 1'b0;
        @(posedge clk); // FF captures scan_chain_enable_next=0 → reg=0
        #(`TB_SETTLE);
        check("ScanChain: enable deasserts", scan_chain_enable === 1'b0);

        // ========================================
        // Test 3: PS broadcast — data goes through FIFO to bus ports
        // ========================================
        $display("\n=== Test 3: PS broadcast through FIFO ===");
        clear_inputs();
        for (int i = 0; i < NP; i++) noc_ps_to_bus_req_ready[i] = 1'b1;

        // Send a broadcast request (addr[6]=0)
        noc_ps_in_data  = 192'h0000_0000_0000_1111_0000_0000_0000_2222_0000_0000_0000_3333;
        noc_ps_in_addr  = 16'h0005; // tag=5, ultra=0
        noc_ps_in_mask  = 64'hF;
        noc_ps_in_valid = 1'b1;
        @(posedge clk); // FIFO push happens; after edge: FIFO not empty → Block C drives bus
        #(`TB_SETTLE);
        // In broadcast mode, all ports get lane 0 data (0x3333)
        for (int i = 0; i < NP; i++) begin
            check($sformatf("PS broadcast port%0d valid", i), noc_ps_to_bus_req_valid[i] === 1'b1);
            check($sformatf("PS broadcast port%0d data", i),
                  noc_ps_to_bus_req_data[i].data === 64'h0000_0000_0000_3333);
            check($sformatf("PS broadcast port%0d addr", i),
                  noc_ps_to_bus_req_data[i].addr === 16'h0005);
        end
        noc_ps_in_valid = 1'b0;
        @(posedge clk); // pop happens, FIFO empty
        #(`TB_SETTLE);
        for (int i = 0; i < NP; i++)
            check($sformatf("PS empty port%0d valid=0", i), noc_ps_to_bus_req_valid[i] === 1'b0);

        // ========================================
        // Test 4: PS ultra mode — per-port data slicing
        // ========================================
        $display("\n=== Test 4: PS ultra mode ===");
        noc_ps_in_data  = {64'hAAAA_BBBB_CCCC_DDDD, 64'h1111_2222_3333_4444, 64'h5555_6666_7777_8888};
        noc_ps_in_addr  = 16'h0043; // ultra=1 (bit6), tag=3
        noc_ps_in_mask  = 64'hFF;
        noc_ps_in_valid = 1'b1;
        @(posedge clk); // FIFO push; after edge: Block C drives bus
        #(`TB_SETTLE);
        check("PS ultra port0 data", noc_ps_to_bus_req_data[0].data === 64'h5555_6666_7777_8888);
        check("PS ultra port1 data", noc_ps_to_bus_req_data[1].data === 64'h1111_2222_3333_4444);
        check("PS ultra port2 data", noc_ps_to_bus_req_data[2].data === 64'hAAAA_BBBB_CCCC_DDDD);
        check("PS ultra addr", noc_ps_to_bus_req_data[0].addr === 16'h0003); // bit6 stripped
        noc_ps_in_valid = 1'b0;
        @(posedge clk);

        // ========================================
        // Test 5: PLO read + response collection (broadcast)
        // ========================================
        $display("\n=== Test 5: PLO broadcast read + response ===");
        clear_inputs();
        for (int i = 0; i < NP; i++) begin
            noc_ps_to_bus_req_ready[i] = 1'b1;
            noc_pd_to_bus_req_ready[i] = 1'b1;
            noc_pli_to_bus_req_ready[i] = 1'b1;
            noc_plo_to_bus_req_ready[i] = 1'b1;
        end
        noc_plo_out_ready = 1'b1;

        // Send PLO read request (broadcast, addr[6]=0)
        send_plo_req(16'h0002);

        // After send_plo_req: past posedge A.  Block F drives request on bus
        // and sets pending_read_next=1.
        @(posedge clk); // posedge B: plo_fifo pop, pending_read_reg ← 1
        // Block G now active, waiting for bus responses.
        // Provide response from port 1:
        bus_to_noc_plo_resp_data[1].data   = 64'hDEAD_BEEF_0123_4567;
        bus_to_noc_plo_resp_data[1].status = NOC_OK;
        bus_to_noc_plo_resp_valid[1] = 1'b1;
        #(`TB_SETTLE);
        // Block G combinationally processes → resp_fifo_push=1

        @(posedge clk); // posedge C: resp_fifo push completes
        #(`TB_SETTLE);
        // Block B: resp_fifo not empty → valid=1
        check("PLO resp: valid", noc_plo_out_valid === 1'b1);
        check("PLO resp: status OK", noc_plo_out_status === NOC_OK);
        check("PLO resp: data lane1",
              noc_plo_out_data[63:0] === 64'hDEAD_BEEF_0123_4567);
        bus_to_noc_plo_resp_valid[1] = 1'b0;
        repeat (2) @(posedge clk);

        // ========================================
        // Test 6: PE command bypass on PS channel
        // ========================================
        $display("\n=== Test 6: PE command bypass ===");
        clear_inputs();
        for (int i = 0; i < NP; i++) noc_ps_to_bus_req_ready[i] = 1'b1;

        command_mode = 1'b1;
        command_data = {28'hABC_DEF0, CMD_INIT}; // Not scan-chain → PE command
        #(`TB_SETTLE);
        for (int i = 0; i < NP; i++) begin
            check($sformatf("PE cmd port%0d valid", i), noc_ps_to_bus_req_valid[i] === 1'b1);
            check($sformatf("PE cmd port%0d addr=0x40", i),
                  noc_ps_to_bus_req_data[i].addr === 16'h0040);
        end
        command_mode = 1'b0;

        // ========================================
        // Summary
        // ========================================
        repeat (5) @(posedge clk);
        $display("\n=== tb_noc_unit Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_noc_unit FAIL");
        else $display("tb_noc_unit PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_noc_unit"); $finish; end
endmodule
