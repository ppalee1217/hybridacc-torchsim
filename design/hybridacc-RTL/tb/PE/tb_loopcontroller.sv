//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_loopcontroller
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for loopcontroller module.
// Dependencies:  tb_common.svh, src/PE/LoopController.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/PE/LoopController.sv"
`endif

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

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/LoopController/LoopController.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;


    initial begin
        pc_in=0; count_in=0; loop_in_en=0; loop_end_en=0;
        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // Test 1: Initial state - no jump when stack empty
        `CHECK_BIT("Reset: jump=0", jump, 1'b0)
        `CHECK_VAL("Reset: pc_out=0", pc_out, 16'h0000)

        // Test 2: Push loop (count=2 => 3 iterations due to count+1 internal)
        pc_in = 16'h0010; count_in = 16'd2;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        // Need extra cycle for top_pc_reg/top_remaining_reg to reflect NBA writes
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Push: pc_out=0x0010", pc_out, 16'h0010)

        // Test 3: First loop_end -> should jump (remaining=3>1)
        // top_remaining_reg lags array by 1 cycle due to NBA read-before-write.
        // @(negedge clk) crosses one posedge, so the first check is after 1 edge.
        // With count_in=2 → internal remaining=3, we get 3 posedges before pop:
        //   After P1: array rem=2, top_remaining_reg=3(lag) → jump=1
        //   After P2: array rem=1, top_remaining_reg=2(lag) → jump=1
        //   After P3: array rem=0, pop, stack empty       → jump=0
        loop_end_en = 1; @(negedge clk);
        `CHECK_BIT("LoopEnd1: jump=1", jump, 1'b1)
        @(posedge clk); @(negedge clk);

        // Test 4: After 2nd edge: top_remaining shows 2 (lag), still jump
        `CHECK_BIT("LoopEnd2: jump=1", jump, 1'b1)
        @(posedge clk); @(negedge clk);

        // Test 5: After 3rd edge: array rem=0, frame popped, stack empty -> no jump
        `CHECK_BIT("LoopEnd3: jump=0", jump, 1'b0)
        @(posedge clk); loop_end_en = 0; @(negedge clk);

        // Test 6: Stack should be empty now
        `CHECK_VAL("AfterPop: pc_out=0", pc_out, 16'h0000)

        // Test 7: Pop on empty stack (should be safe no-op)
        loop_end_en = 1; @(posedge clk); loop_end_en = 0; @(negedge clk);
        `CHECK_BIT("PopEmpty: jump=0", jump, 1'b0)

        // Test 8: Nested loops
        // Outer loop: count=1 (remaining=2), PC=0x0020
        pc_in = 16'h0020; count_in = 16'd1;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        @(posedge clk); @(negedge clk); // extra cycle for top regs to settle
        // Inner loop: count=1 (remaining=2), PC=0x0030
        pc_in = 16'h0030; count_in = 16'd1;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        @(posedge clk); @(negedge clk); // extra cycle for top regs to settle
        `CHECK_VAL("Nested: pc_out=inner 0x0030", pc_out, 16'h0030)

        // Exhaust inner loop then outer loop (4 total edges needed)
        // inner remaining=2, outer remaining=2
        loop_end_en = 1;
        @(posedge clk); @(negedge clk); // edge1: inner rem 2->1, top_rem=2(lag), jump=1
        `CHECK_BIT("InnerEnd1: jump=1", jump, 1'b1)
        @(posedge clk); @(negedge clk); // edge2: inner rem 1->0, pop to outer, top_rem=outer(2), jump=1
        `CHECK_BIT("InnerEnd2: jump=1", jump, 1'b1)
        `CHECK_VAL("InnerEnd2: pc_out switches to outer", pc_out, 16'h0020)
        @(posedge clk); @(negedge clk); // edge3: outer rem 2->1, top_rem=2(lag), jump=1
        `CHECK_BIT("OuterEnd1: jump=1", jump, 1'b1)
        @(posedge clk); @(negedge clk); // edge4: outer rem 1->0, pop, stack empty, jump=0
        `CHECK_BIT("OuterEnd2: jump=0", jump, 1'b0)
        @(posedge clk); loop_end_en = 0; @(negedge clk);
        `CHECK_VAL("AfterNested: pc_out=0", pc_out, 16'h0000)

        // Test 9: Zero count (treated as 1 iteration due to count!=0 guard)
        pc_in = 16'h0050; count_in = 16'd0;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0; @(negedge clk);
        // count_in=0 should NOT push (guard: count_in != 0)
        `CHECK_VAL("ZeroCount: stack empty, pc_out=0", pc_out, 16'h0000)

        `TB_SUMMARY("tb_loopcontroller")
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_loopcontroller"); $finish; end
endmodule
