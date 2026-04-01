//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_asyncfifo
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for asyncfifo module.
// Dependencies:  tb_common.svh, src/asyncFIFO.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "tb_common.svh"
`ifndef GATE_SIM
`include "../src/asyncFIFO.sv"
`endif

module tb_asyncfifo;

`ifdef GATE_SIM
initial begin
    $sdf_annotate("../syn/asyncFIFO/asyncFIFO.sdf", dut);
end
`endif

    logic clk, reset_n;
    logic [63:0] data_in;
    logic [3:0] mask_in;
    logic push;
    logic [15:0] data_out;
    logic pop;
    logic [63:0] data_out_set;
    logic pop_set;
    logic set_valid;
    logic empty, full;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    // DEPTH=2 => MAX_ELEMENTS = 2*4 = 8 chunks
    asyncFIFO #(.IN_T(logic [63:0]), .OUT_T(logic [15:0]), .DEPTH(2)) dut (
        .clk(clk), .reset_n(reset_n), .data_in(data_in), .mask_in(mask_in), .push(push),
        .data_out(data_out), .pop(pop), .data_out_set(data_out_set), .pop_set(pop_set),
        .set_valid(set_valid), .empty(empty), .full(full)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin
            $error("[FAIL] %s", test_name);
            fail_count++;
        end else begin
            $display("[PASS] %s", test_name);
            pass_count++;
        end
    endtask

    initial begin
        data_in='0; mask_in='0; push=0; pop=0; pop_set=0;
        @(posedge reset_n);
        @(posedge clk); #(`TB_SETTLE);

        // Test 1: Reset state
        check("Reset: empty=1", empty === 1'b1);
        check("Reset: full=0", full === 1'b0);
        check("Reset: set_valid=0", set_valid === 1'b0);

        // Test 2: Push with full mask, verify element-wise pop
        data_in = 64'h0004_0003_0002_0001;
        mask_in = 4'b1111;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("Push4: empty=0", empty === 1'b0);
        check("Push4: set_valid=1", set_valid === 1'b1);
        check("Push4: first chunk=0x0001", data_out === 16'h0001);

        // Pop one chunk at a time
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("Pop_chunk1: data=0x0002", data_out === 16'h0002);
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("Pop_chunk2: data=0x0003", data_out === 16'h0003);
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("Pop_chunk3: data=0x0004", data_out === 16'h0004);
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("Pop_all: empty=1", empty === 1'b1);

        // Test 3: pop_set (pop all 4 chunks at once)
        data_in = 64'hAAAA_BBBB_CCCC_DDDD;
        mask_in = 4'b1111;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("PopSet: set_valid=1", set_valid === 1'b1);
        // Check data_out_set BEFORE pop (after pop, rd_ptr advances and cnt drops)
        check("PopSet: data_out_set match", data_out_set === 64'hAAAA_BBBB_CCCC_DDDD);
        pop_set = 1; @(posedge clk); pop_set = 0; #(`TB_SETTLE);
        check("PopSet: empty=1", empty === 1'b1);

        // Test 4: Partial mask - only push masked chunks
        data_in = 64'h0008_0007_0006_0005;
        mask_in = 4'b1010; // only chunk[1]=0x0006 and chunk[3]=0x0008
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("PartialMask: empty=0", empty === 1'b0);
        check("PartialMask: first=0x0006", data_out === 16'h0006);
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("PartialMask: second=0x0008", data_out === 16'h0008);
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("PartialMask: empty=1", empty === 1'b1);

        // Test 5: Fill to full (MAX_ELEMENTS=8, push 2 full words)
        data_in = 64'h1111_2222_3333_4444;
        mask_in = 4'b1111;
        push = 1; @(posedge clk); #(`TB_SETTLE);
        data_in = 64'h5555_6666_7777_8888;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("Full: full=1", full === 1'b1);

        // Test 6: Push when full (should be rejected)
        data_in = 64'hFFFF_FFFF_FFFF_FFFF;
        mask_in = 4'b1111;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("PushFull: full still 1", full === 1'b1);
        check("PushFull: head preserved", data_out === 16'h4444);

        // Test 7: Drain via pop_set twice
        pop_set = 1; @(posedge clk); pop_set = 0; #(`TB_SETTLE);
        check("DrainSet1: set_valid=1 before", set_valid === 1'b1);
        pop_set = 1; @(posedge clk); pop_set = 0; #(`TB_SETTLE);
        check("DrainSet2: empty=1", empty === 1'b1);

        // Test 8: Zero mask push (nothing stored)
        data_in = 64'hAAAA_BBBB_CCCC_DDDD;
        mask_in = 4'b0000;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("ZeroMask: empty=1", empty === 1'b1);

        // Test 9: Simultaneous push + pop (push new data while popping head)
        data_in = 64'h000A_000B_000C_000D;
        mask_in = 4'b1111;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE); // load 4 chunks
        // Now push new data and pop simultaneously
        data_in = 64'h00E0_00F0_0010_0020;
        mask_in = 4'b1111;
        push = 1; pop = 1; @(posedge clk); push = 0; pop = 0; #(`TB_SETTLE);
        // After: one popped (0x000D), four pushed. cnt should be 4-1+4=7
        // full threshold: cnt > (MAX_ELEMENTS - CHUNKS_PER_PUSH) = cnt > 4, so 7 > 4 = full
        check("SimPushPop: not empty", empty === 1'b0);
        check("SimPushPop: full (7 of 8)", full === 1'b1);
        check("SimPushPop: head=0x000C", data_out === 16'h000C);
        // Drain
        repeat(7) begin pop = 1; @(posedge clk); pop = 0; end
        #(`TB_SETTLE);
        check("SimPushPop drain: empty=1", empty === 1'b1);

        // Test 10: Simultaneous push + pop_set
        data_in = 64'h0001_0002_0003_0004;
        mask_in = 4'b1111;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE); // load 4 chunks
        // Now push new data and pop_set simultaneously
        data_in = 64'h0005_0006_0007_0008;
        mask_in = 4'b1111;
        push = 1; pop_set = 1; @(posedge clk); push = 0; pop_set = 0; #(`TB_SETTLE);
        // pop_set removed 4, push added 4 => cnt=4
        check("SimPushPopSet: not empty", empty === 1'b0);
        check("SimPushPopSet: data_out=0x0008", data_out === 16'h0008);
        pop_set = 1; @(posedge clk); pop_set = 0; #(`TB_SETTLE);
        check("SimPushPopSet: drain empty", empty === 1'b1);

        $display("\n=== tb_asyncfifo Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_asyncfifo FAIL");
        else $display("tb_asyncfifo PASS");
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_asyncfifo"); $finish; end
endmodule
