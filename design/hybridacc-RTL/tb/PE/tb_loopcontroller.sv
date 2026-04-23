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

        // Test 2: Push loop (count=3 → internal remaining=4, so 3 iterations / 2 jumps)
        pc_in = 16'h0010; count_in = 16'd3;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Push: pc_out=0x0010", pc_out, 16'h0010)

        // Test 3–6: loop_end iterations.
        // top_remaining_reg is the authoritative top-of-stack (no lag).
        // With count_in=3 → remaining=4:
        //   Edge 1: rem 4→3, jump=1  (3>1)
        //   Edge 2: rem 3→2, jump=1  (2>1)
        //   Edge 3: rem 2→1, jump=0  (at_last)
        //   Edge 4: rem=1,  pop,  stack empty, jump=0
        loop_end_en = 1; @(negedge clk);
        `CHECK_BIT("LoopEnd1: jump=1", jump, 1'b1)
        @(posedge clk); @(negedge clk);

        `CHECK_BIT("LoopEnd2: jump=1", jump, 1'b1)
        @(posedge clk); @(negedge clk);

        // remaining decremented to 1 → top_at_last, no jump
        `CHECK_BIT("LoopEnd3: jump=0", jump, 1'b0)
        @(posedge clk); @(negedge clk);

        // pop completed, stack empty
        `CHECK_BIT("LoopEnd4: jump=0", jump, 1'b0)
        @(posedge clk); loop_end_en = 0; @(negedge clk);

        // Test 7: Stack should be empty now
        `CHECK_VAL("AfterPop: pc_out=0", pc_out, 16'h0000)

        // Test 8: Pop on empty stack (should be safe no-op)
        loop_end_en = 1; @(posedge clk); loop_end_en = 0; @(negedge clk);
        `CHECK_BIT("PopEmpty: jump=0", jump, 1'b0)

        // Test 9: Nested loops
        // Outer loop: count=2 (remaining=3), PC=0x0020
        pc_in = 16'h0020; count_in = 16'd2;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        @(posedge clk); @(negedge clk);
        // Inner loop: count=2 (remaining=3), PC=0x0030
        pc_in = 16'h0030; count_in = 16'd2;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Nested: pc_out=inner 0x0030", pc_out, 16'h0030)

        // Exhaust inner then outer (6 edges total with loop_end_en held high)
        // inner remaining=3, outer remaining=3
        loop_end_en = 1;
        @(posedge clk); @(negedge clk); // edge1: inner rem 3→2, jump=1
        `CHECK_BIT("InnerEnd1: jump=1", jump, 1'b1)
        `CHECK_VAL("InnerEnd1: pc_out=inner", pc_out, 16'h0030)
        @(posedge clk); @(negedge clk); // edge2: inner rem 2→1, at_last, jump=0
        `CHECK_BIT("InnerEnd2: jump=0", jump, 1'b0)
        @(posedge clk); @(negedge clk); // edge3: inner pop → outer restored (rem=3), jump=1
        `CHECK_BIT("InnerPop: jump=1", jump, 1'b1)
        `CHECK_VAL("InnerPop: pc_out=outer 0x0020", pc_out, 16'h0020)
        @(posedge clk); @(negedge clk); // edge4: outer rem 3→2, jump=1
        `CHECK_BIT("OuterEnd1: jump=1", jump, 1'b1)
        @(posedge clk); @(negedge clk); // edge5: outer rem 2→1, at_last, jump=0
        `CHECK_BIT("OuterEnd2: jump=0", jump, 1'b0)
        @(posedge clk); @(negedge clk); // edge6: outer pop, stack empty, jump=0
        `CHECK_BIT("OuterPop: jump=0", jump, 1'b0)
        @(posedge clk); loop_end_en = 0; @(negedge clk);
        `CHECK_VAL("AfterNested: pc_out=0", pc_out, 16'h0000)

        // Test 10: Zero count — should NOT push (guard: count_in != 0)
        pc_in = 16'h0050; count_in = 16'd0;
        loop_in_en = 1; @(posedge clk); loop_in_en = 0; @(negedge clk);
        `CHECK_VAL("ZeroCount: stack empty, pc_out=0", pc_out, 16'h0000)

        `TB_SUMMARY("tb_loopcontroller")
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_loopcontroller"); $finish; end
endmodule
