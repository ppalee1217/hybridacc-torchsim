//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   FIFO
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
module FIFO #(
    parameter type T = logic [31:0],
    parameter int unsigned DEPTH = 4
) (
    input  logic clk,
    input  logic reset_n,
    input  T     data_in,
    input  logic push,
    output T     data_out,
    input  logic pop,
    output logic empty,
    output logic full,
    input  logic clear
);
    localparam int PTR_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH);

    T mem [0:DEPTH-1];
    logic [PTR_W-1:0] wr_ptr_reg, rd_ptr_reg;
    logic [PTR_W:0]   cnt_reg;

    assign data_out = (cnt_reg > 0) ? mem[rd_ptr_reg] : '0;

    always_ff @(posedge clk or negedge reset_n) begin
        logic do_pop, do_push;
        logic [PTR_W-1:0] wr_n, rd_n;
        logic [PTR_W:0] cnt_n;
        logic is_empty, is_full;

        if (!reset_n || clear) begin
            wr_ptr_reg <= '0;
            rd_ptr_reg <= '0;
            cnt_reg <= '0;
            for (int i = 0; i < DEPTH; i++) mem[i] <= '0;
        end else begin
            is_empty = (cnt_reg == 0);
            is_full  = (cnt_reg == DEPTH);

            do_pop = 1'b0;
            do_push = 1'b0;
            if (is_full) begin
                do_pop = pop;
                do_push = push && pop;
            end else if (is_empty) begin
                do_pop = 1'b0;
                do_push = push;
            end else begin
                do_pop = pop;
                do_push = push;
            end

            wr_n = wr_ptr_reg;
            rd_n = rd_ptr_reg;
            cnt_n = cnt_reg;

            if (do_pop) begin
                rd_n = (rd_ptr_reg == DEPTH-1) ? '0 : (rd_ptr_reg + 1'b1);
                cnt_n = cnt_n - 1'b1;
            end
            if (do_push) begin
                mem[wr_ptr_reg] <= data_in;
                wr_n = (wr_ptr_reg == DEPTH-1) ? '0 : (wr_ptr_reg + 1'b1);
                cnt_n = cnt_n + 1'b1;
            end

            wr_ptr_reg <= wr_n;
            rd_ptr_reg <= rd_n;
            cnt_reg <= cnt_n;
        end
    end

    always_comb begin
        empty = (cnt_reg == 0);
        full  = (cnt_reg == DEPTH);
    end
endmodule
