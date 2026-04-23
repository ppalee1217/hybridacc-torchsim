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
    // Stack storage — holds levels below the current top
    logic [15:0] loop_start_pc [0:LOOP_STACK_DEPTH-1];
    logic [15:0] loop_remaining[0:LOOP_STACK_DEPTH-1];
    logic [$clog2(LOOP_STACK_DEPTH+1)-1:0] loop_size_reg;

    // Authoritative top-of-stack (directly maintained, not a stale copy)
    logic [15:0] top_pc_reg;
    logic [15:0] top_remaining_reg;

    // ---- Pre-computed speculative values (independent of loop_end_en) ----
    // Available every cycle before loop_end_en arrives
    logic [15:0] top_rem_dec;        // remaining - 1 (for decrement)
    logic        top_at_last;        // will pop on next loop_end?

    assign top_rem_dec = top_remaining_reg - 16'd1;
    assign top_at_last = (top_remaining_reg <= 16'd1);

    // ---- Outputs: only gate registered values with loop_end_en ----
    always_comb begin
        pc_out = (loop_size_reg > 0) ? top_pc_reg : 16'h0000;
        jump   = loop_end_en && (loop_size_reg > 0) && !top_at_last;
    end

    // ---- Stack update ----
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int idx = 0; idx < LOOP_STACK_DEPTH; idx++) begin
                loop_start_pc[idx]  <= 16'h0000;
                loop_remaining[idx] <= 16'h0000;
            end
            loop_size_reg     <= '0;
            top_pc_reg        <= 16'h0000;
            top_remaining_reg <= 16'h0000;

        end else if (loop_in_en && (count_in != 16'h0000) &&
                     (loop_size_reg < LOOP_STACK_DEPTH[$clog2(LOOP_STACK_DEPTH+1)-1:0])) begin
            // === Push: save current top to array, new entry becomes top ===
            if (loop_size_reg > 0) begin
                loop_remaining[loop_size_reg - 1] <= top_remaining_reg;
                loop_start_pc[loop_size_reg - 1]  <= top_pc_reg;
            end
            top_remaining_reg <= count_in + 16'd1;
            top_pc_reg        <= pc_in;
            loop_size_reg     <= loop_size_reg + 1'b1;

        end else if (loop_end_en && (loop_size_reg > 0)) begin
            if (top_at_last) begin
                // === Pop: restore sub-top from array ===
                loop_size_reg <= loop_size_reg - 1'b1;
                if (loop_size_reg >= 2) begin
                    top_remaining_reg <= loop_remaining[loop_size_reg - 2];
                    top_pc_reg        <= loop_start_pc[loop_size_reg - 2];
                end else begin
                    top_remaining_reg <= 16'h0000;
                    top_pc_reg        <= 16'h0000;
                end
            end else begin
                // === Decrement: use pre-computed top_rem_dec ===
                top_remaining_reg                 <= top_rem_dec;
                loop_remaining[loop_size_reg - 1] <= top_rem_dec;
            end
        end
    end
endmodule
