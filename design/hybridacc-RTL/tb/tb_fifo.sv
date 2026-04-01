//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_fifo
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for fifo module.
// Dependencies:  tb_common.svh, src/FIFO.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "tb_common.svh"
`ifndef GATE_SIM
`include "../src/FIFO.sv"
`endif

module tb_fifo;

`ifdef GATE_SIM
initial begin
    $sdf_annotate("../syn/FIFO/FIFO.sdf", dut);
end
`endif

    logic clk, reset_n;
    logic [63:0] data_in;
    logic push;
    logic [63:0] data_out;
    logic pop;
    logic empty, full;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    FIFO #(.T(logic [63:0]), .DEPTH(4)) dut (
        .clk(clk), .reset_n(reset_n), .data_in(data_in), .push(push),
        .data_out(data_out), .pop(pop), .empty(empty), .full(full), .clear(1'b0)
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
        data_in = '0; push = 0; pop = 0;
        @(posedge reset_n);
        @(posedge clk); #(`TB_SETTLE);

        // Test 1: Initial state after reset
        check("Reset: empty=1", empty === 1'b1);
        check("Reset: full=0", full === 1'b0);
        check("Reset: data_out=0", data_out === 64'h0);

        // Test 2: Single push
        data_in = 64'hDEAD_BEEF_CAFE_BABE;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("Push1: empty=0", empty === 1'b0);
        check("Push1: full=0", full === 1'b0);
        check("Push1: data_out match", data_out === 64'hDEAD_BEEF_CAFE_BABE);

        // Test 3: Single pop
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("Pop1: empty=1", empty === 1'b1);
        check("Pop1: data_out=0", data_out === 64'h0);

        // Test 4: Fill to full (DEPTH=4)
        for (int i = 0; i < 4; i++) begin
            data_in = 64'h1000 + i;
            push = 1; @(posedge clk); #(`TB_SETTLE);
        end
        push = 0; #(`TB_SETTLE);
        check("Full: full=1", full === 1'b1);
        check("Full: empty=0", empty === 1'b0);

        // Test 5: Push when full (should be ignored)
        data_in = 64'hFFFF;
        push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        check("PushWhenFull: full=1", full === 1'b1);
        check("PushWhenFull: head preserved", data_out === 64'h1000);

        // Test 6: Pop when empty
        for (int i = 0; i < 4; i++) begin
            pop = 1; @(posedge clk); #(`TB_SETTLE);
        end
        pop = 0; #(`TB_SETTLE);
        check("Drain: empty=1", empty === 1'b1);
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        check("PopWhenEmpty: empty=1", empty === 1'b1);

        // Test 7: Simultaneous push+pop (not empty, not full)
        data_in = 64'hAAAA; push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
        data_in = 64'hBBBB; push = 1; pop = 1; @(posedge clk); push = 0; pop = 0; #(`TB_SETTLE);
        check("SimPushPop: empty=0", empty === 1'b0);
        check("SimPushPop: data_out=BBBB", data_out === 64'hBBBB);

        // Test 8: Simultaneous push+pop when full
        pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        for (int i = 0; i < 4; i++) begin
            data_in = 64'hC000 + i;
            push = 1; @(posedge clk); #(`TB_SETTLE);
        end
        push = 0; #(`TB_SETTLE);
        check("PreSimFull: full=1", full === 1'b1);
        data_in = 64'hDDDD;
        push = 1; pop = 1; @(posedge clk); push = 0; pop = 0; #(`TB_SETTLE);
        check("SimPushPopFull: full=1", full === 1'b1);
        check("SimPushPopFull: head advanced", data_out === 64'hC001);

        // Test 9: FIFO ordering
        for (int i = 0; i < 4; i++) begin
            pop = 1; @(posedge clk); #(`TB_SETTLE);
        end
        pop = 0; #(`TB_SETTLE);
        for (int i = 0; i < 4; i++) begin
            data_in = 64'h0A00 + i;
            push = 1; @(posedge clk); #(`TB_SETTLE);
        end
        push = 0;
        for (int i = 0; i < 4; i++) begin
            #(`TB_SETTLE);
            check($sformatf("FIFO_order[%0d]", i), data_out === (64'h0A00 + i));
            pop = 1; @(posedge clk);
        end
        pop = 0; #(`TB_SETTLE);
        check("FIFO_order: empty after drain", empty === 1'b1);

        // Test 10: Pointer wraparound
        for (int i = 0; i < 6; i++) begin
            data_in = 64'hF000 + i;
            push = 1; @(posedge clk); push = 0; #(`TB_SETTLE);
            pop = 1; @(posedge clk); pop = 0; #(`TB_SETTLE);
        end
        check("Wraparound: empty=1", empty === 1'b1);

        $display("\n=== tb_fifo Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_fifo FAIL");
        else $display("tb_fifo PASS");
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_fifo"); $finish; end
endmodule
