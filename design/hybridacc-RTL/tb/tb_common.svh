`timescale 1ns/1ps

`define TB_ASSERT(cond, msg) \
    if (!(cond)) begin \
        $error("[TB_ASSERT] %s", msg); \
        $fatal(1); \
    end

module tb_clock_reset #(
    parameter CLK_PERIOD_NS = 10,
    parameter RESET_CYCLES = 4
) (
    output logic clk,
    output logic reset_n
);
    initial begin
        clk = 1'b0;
        forever #(CLK_PERIOD_NS/2) clk = ~clk;
    end

    initial begin
        reset_n = 1'b0;
        repeat (RESET_CYCLES) @(posedge clk);
        reset_n = 1'b1;
    end
endmodule
