//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_common
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Common testbench macros and utilities.
// Dependencies:  None
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`ifndef TB_COMMON_SVH
`define TB_COMMON_SVH

`timescale 1ns/1ps

// Settling delay (ns) used after clock edges for combinational outputs to stabilise.
// Must be strictly less than half the clock period.  Override with +define+TB_SETTLE=<val>.
`ifndef TB_SETTLE
`define TB_SETTLE 0.1
`endif

`define TB_ASSERT(cond, msg) \
    if (!(cond)) begin \
        $error("[TB_ASSERT] %s", msg); \
        $fatal(1); \
    end

// ---------------------------------------------------------------------------
// X-aware checking macros for gate-level (post-sim) diagnostics.
// Each TB must declare: int pass_count=0, fail_count=0, x_fail_count=0;
// ---------------------------------------------------------------------------
// Value check (any width). Reports X/Z vs wrong-but-defined values.
`define CHECK_VAL(test_name, actual, expected) \
    begin \
        if ($isunknown(actual)) begin \
            $error("[FAIL-X] %s — got=0x%0h (contains X/Z), want=0x%0h", test_name, actual, expected); \
            fail_count = fail_count + 1; x_fail_count = x_fail_count + 1; \
        end else if ((actual) !== (expected)) begin \
            $error("[FAIL-LOGIC] %s — got=0x%0h, want=0x%0h", test_name, actual, expected); \
            fail_count = fail_count + 1; \
        end else begin \
            $display("[PASS] %s", test_name); \
            pass_count = pass_count + 1; \
        end \
    end

`define CHECK_VAL_OR_X(test_name, actual, expected) \
    begin \
        if ($isunknown(actual)) begin \
            $display("[PASS-X] %s — got=0x%0h (contains X/Z), accepted for unwritten SRAM read", test_name, actual); \
            pass_count = pass_count + 1; \
        end else if ((actual) !== (expected)) begin \
            $error("[FAIL-LOGIC] %s — got=0x%0h, want=0x%0h", test_name, actual, expected); \
            fail_count = fail_count + 1; \
        end else begin \
            $display("[PASS] %s", test_name); \
            pass_count = pass_count + 1; \
        end \
    end

// 1-bit check. Reports X/Z clearly.
`define CHECK_BIT(test_name, actual, expected) \
    begin \
        if ((actual) === 1'bx || (actual) === 1'bz) begin \
            $error("[FAIL-X] %s — got=x/z, want=%0b", test_name, expected); \
            fail_count = fail_count + 1; x_fail_count = x_fail_count + 1; \
        end else if ((actual) !== (expected)) begin \
            $error("[FAIL-LOGIC] %s — got=%0b, want=%0b", test_name, actual, expected); \
            fail_count = fail_count + 1; \
        end else begin \
            $display("[PASS] %s", test_name); \
            pass_count = pass_count + 1; \
        end \
    end

// Compound-condition check with a debug signal for X detection.
`define CHECK_COND(test_name, cond, dbg_signal) \
    begin \
        if ($isunknown(dbg_signal)) begin \
            $error("[FAIL-X] %s — signal has X/Z bits (0x%0h)", test_name, dbg_signal); \
            fail_count = fail_count + 1; x_fail_count = x_fail_count + 1; \
        end else if (!(cond)) begin \
            $error("[FAIL-LOGIC] %s", test_name); \
            fail_count = fail_count + 1; \
        end else begin \
            $display("[PASS] %s", test_name); \
            pass_count = pass_count + 1; \
        end \
    end

// Summary with X/timing vs logic breakdown.
`define TB_SUMMARY(tb_name) \
    $display("\n=== %s Summary: %0d PASSED, %0d FAILED (%0d X/timing, %0d logic) ===", \
        tb_name, pass_count, fail_count, x_fail_count, fail_count - x_fail_count); \
    if (fail_count > 0) begin \
        if (x_fail_count == fail_count) \
            $display("%s FAIL (all failures are X/timing — likely SDF delay > TB_SETTLE)", tb_name); \
        else if (x_fail_count > 0) \
            $display("%s FAIL (mixed: %0d X/timing + %0d logic errors)", tb_name, x_fail_count, fail_count - x_fail_count); \
        else \
            $display("%s FAIL (all failures are logic errors)", tb_name); \
    end else \
        $display("%s PASS", tb_name);

module tb_clock_reset #(
    parameter CLK_PERIOD_NS = 1,
    parameter RESET_CYCLES = 4
) (
    output logic clk,
    output logic reset_n
);
    real clock_period_ns;
    real half_period_ns;
    string clock_period_arg;

    initial begin
        clock_period_ns = CLK_PERIOD_NS;
        if ($value$plusargs("CLOCK_PERIOD_NS=%s", clock_period_arg)) begin
            if ($sscanf(clock_period_arg, "%f", clock_period_ns) != 1) begin
                $display("[TB] Invalid CLOCK_PERIOD_NS=%0s, fallback to %0d", clock_period_arg, CLK_PERIOD_NS);
                clock_period_ns = CLK_PERIOD_NS;
            end
        end
        if (clock_period_ns <= 0.0) begin
            $display("[TB] Invalid CLOCK_PERIOD_NS=%0f, fallback to %0d", clock_period_ns, CLK_PERIOD_NS);
            clock_period_ns = CLK_PERIOD_NS;
        end
        half_period_ns = clock_period_ns / 2.0;
        clk = 1'b0;
        forever #(half_period_ns) clk = ~clk;
    end

    initial begin
        reset_n = 1'b0;
        repeat (RESET_CYCLES) @(posedge clk);
        reset_n = 1'b1;
    end
endmodule

`endif // TB_COMMON_SVH
