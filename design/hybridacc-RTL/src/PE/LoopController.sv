//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   LoopController
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Common utility package with type definitions, FP16 arithmetic, and shared constants.
// Dependencies:  None
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module LoopController #(
    parameter int unsigned LOOP_STACK_DEPTH = 16
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic [15:0] pc_in,
    input  logic [15:0] count_in,
    input  logic        loop_in_en,
    input  logic        loop_end_en,
    output logic [15:0] pc_out,
    output logic        jump
);
    logic [15:0] loop_start_pc [0:LOOP_STACK_DEPTH-1];
    logic [15:0] loop_remaining[0:LOOP_STACK_DEPTH-1];
    logic [$clog2(LOOP_STACK_DEPTH+1)-1:0] loop_size_reg;

    logic [15:0] top_pc_reg;
    logic [15:0] top_remaining_reg;

    always_comb begin
        if (loop_size_reg == 0) begin
            pc_out = 16'h0000;
        end else begin
            pc_out = top_pc_reg;
        end

        if (loop_end_en && (loop_size_reg > 0) && (top_remaining_reg > 16'd1)) begin
            jump = 1'b1;
        end else begin
            jump = 1'b0;
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        int idx;
        logic [$clog2(LOOP_STACK_DEPTH+1)-1:0] next_size;
        if (!reset_n) begin
            for (idx = 0; idx < LOOP_STACK_DEPTH; idx++) begin
                loop_start_pc[idx]  <= 16'h0000;
                loop_remaining[idx] <= 16'h0000;
            end
            loop_size_reg     <= '0;
            top_pc_reg        <= 16'h0000;
            top_remaining_reg <= 16'h0000;
        end else begin
            next_size = loop_size_reg;

            if (loop_in_en && (count_in != 16'h0000) && (loop_size_reg < LOOP_STACK_DEPTH)) begin
                loop_start_pc[loop_size_reg]  <= pc_in;
                loop_remaining[loop_size_reg] <= count_in + 16'd1;
                next_size = loop_size_reg + 1'b1;
            end

            if (loop_end_en && (next_size > 0)) begin
                logic [15:0] rem;
                rem = loop_remaining[next_size-1];
                if (rem > 16'd0) begin
                    rem = rem - 16'd1;
                    loop_remaining[next_size-1] <= rem;
                end
                if (rem == 16'd0) begin
                    next_size = next_size - 1'b1;
                end
            end

            loop_size_reg <= next_size;
            if (next_size > 0) begin
                top_pc_reg        <= loop_start_pc[next_size-1];
                top_remaining_reg <= loop_remaining[next_size-1];
            end else begin
                top_pc_reg        <= 16'h0000;
                top_remaining_reg <= 16'h0000;
            end
        end
    end
endmodule
