`include "../tb_common.svh"
`include "../../src/PE/LoopController.sv"

module tb_loopcontroller;
    logic clk, reset_n;
    logic [15:0] pc_in, count_in;
    logic loop_in_en, loop_end_en;
    logic [15:0] pc_out;
    logic jump;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    LoopController dut (
        .clk(clk), .reset_n(reset_n), .pc_in(pc_in), .count_in(count_in),
        .loop_in_en(loop_in_en), .loop_end_en(loop_end_en), .pc_out(pc_out), .jump(jump)
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
        pc_in=0; count_in=0; loop_in_en=0; loop_end_en=0;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: Initial state - no jump when stack empty
        check("Reset: jump=0", jump === 1'b0);
        check("Reset: pc_out=0", pc_out === 16'h0000);

        // Test 2: Push loop (count=2 => 3 iterations due to count+1 internal)
        pc_in = 16'h0010; count_in = 16'd2;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        // Need extra cycle for top_pc_reg/top_remaining_reg to reflect NBA writes
        @(posedge clk); #1;
        check("Push: pc_out=0x0010", pc_out === 16'h0010);

        // Test 3: First loop_end -> should jump (remaining=3>1)
        // top_remaining_reg lags array by 1 cycle due to NBA read-before-write
        loop_end_en = 1; #1;
        check("LoopEnd1: jump=1", jump === 1'b1);
        @(posedge clk); #1;

        // Test 4: After 1st edge: array rem=2, top_remaining still shows 3 (lag)
        check("LoopEnd2: jump=1", jump === 1'b1);
        @(posedge clk); #1;

        // Test 4b: After 2nd edge: array rem=1, top_remaining shows 2 (lag), still jump
        check("LoopEnd2b: jump=1", jump === 1'b1);
        @(posedge clk); #1;

        // Test 5: After 3rd edge: array rem=0, frame popped, stack empty -> no jump
        check("LoopEnd3: jump=0", jump === 1'b0);
        @(posedge clk); loop_end_en = 0; #1;

        // Test 6: Stack should be empty now
        check("AfterPop: pc_out=0", pc_out === 16'h0000);

        // Test 7: Pop on empty stack (should be safe no-op)
        loop_end_en = 1; @(posedge clk); loop_end_en = 0; #1;
        check("PopEmpty: jump=0", jump === 1'b0);

        // Test 8: Nested loops
        // Outer loop: count=1 (remaining=2), PC=0x0020
        pc_in = 16'h0020; count_in = 16'd1;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        @(posedge clk); #1; // extra cycle for top regs to settle
        // Inner loop: count=1 (remaining=2), PC=0x0030
        pc_in = 16'h0030; count_in = 16'd1;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        @(posedge clk); #1; // extra cycle for top regs to settle
        check("Nested: pc_out=inner 0x0030", pc_out === 16'h0030);

        // Exhaust inner loop then outer loop (4 total edges needed)
        // inner remaining=2, outer remaining=2
        loop_end_en = 1;
        @(posedge clk); #1; // edge1: inner rem 2->1, top_rem=2(lag), jump=1
        check("InnerEnd1: jump=1", jump === 1'b1);
        @(posedge clk); #1; // edge2: inner rem 1->0, pop to outer, top_rem=outer(2), jump=1
        check("InnerEnd2: jump=1", jump === 1'b1);
        check("InnerEnd2: pc_out switches to outer", pc_out === 16'h0020);
        @(posedge clk); #1; // edge3: outer rem 2->1, top_rem=2(lag), jump=1
        check("OuterEnd1: jump=1", jump === 1'b1);
        @(posedge clk); #1; // edge4: outer rem 1->0, pop, stack empty, jump=0
        check("OuterEnd2: jump=0", jump === 1'b0);
        @(posedge clk); loop_end_en = 0; #1;
        check("AfterNested: pc_out=0", pc_out === 16'h0000);

        // Test 9: Zero count (treated as 1 iteration due to count!=0 guard)
        pc_in = 16'h0050; count_in = 16'd0;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0; #1;
        // count_in=0 should NOT push (guard: count_in != 0)
        check("ZeroCount: stack empty, pc_out=0", pc_out === 16'h0000);

        $display("\n=== tb_loopcontroller Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_loopcontroller FAIL");
        else $display("tb_loopcontroller PASS");
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_loopcontroller"); $finish; end
endmodule
