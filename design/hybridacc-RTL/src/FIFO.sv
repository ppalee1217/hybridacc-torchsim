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
    localparam logic [PTR_W-1:0] PTR_ONE = {{(PTR_W-1){1'b0}}, 1'b1};
    localparam logic [PTR_W:0]   DEPTH_COUNT = (PTR_W + 1)'(DEPTH);
    localparam logic [PTR_W:0]   CNT_ONE = {{PTR_W{1'b0}}, 1'b1};

    T mem [0:DEPTH-1];
    logic [PTR_W-1:0] wr_ptr_reg, rd_ptr_reg;
    logic [PTR_W:0]   cnt_reg;
    logic             fifo_empty_w;
    logic             fifo_full_w;
    logic             do_pop_w;
    logic             do_push_w;
    logic [PTR_W-1:0] wr_ptr_next_w;
    logic [PTR_W-1:0] rd_ptr_next_w;
    logic [PTR_W:0]   cnt_next_w;

    assign data_out = (cnt_reg > 0) ? mem[rd_ptr_reg] : '0;

    always_comb begin
        fifo_empty_w = (cnt_reg == '0);
        fifo_full_w  = (cnt_reg == DEPTH_COUNT);
        empty = fifo_empty_w;
        full  = fifo_full_w;

        do_pop_w = pop && !fifo_empty_w;
        do_push_w = push && (!fifo_full_w || pop);

        wr_ptr_next_w = wr_ptr_reg + (do_push_w ? PTR_ONE : '0);
        rd_ptr_next_w = rd_ptr_reg + (do_pop_w ? PTR_ONE : '0);
        cnt_next_w = cnt_reg;
        if (do_pop_w && !do_push_w) begin
            cnt_next_w = cnt_reg - CNT_ONE;
        end else if (do_push_w && !do_pop_w) begin
            cnt_next_w = cnt_reg + CNT_ONE;
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            wr_ptr_reg <= '0;
            rd_ptr_reg <= '0;
            cnt_reg <= '0;
            for (int unsigned i = 0; i < DEPTH; i++) begin
                mem[i] <= '0;
            end
        end else if (clear) begin
            wr_ptr_reg <= '0;
            rd_ptr_reg <= '0;
            cnt_reg <= '0;
            for (int unsigned i = 0; i < DEPTH; i++) begin
                mem[i] <= '0;
            end
        end else begin
            if (do_push_w) begin
                mem[wr_ptr_reg] <= data_in;
            end

            wr_ptr_reg <= wr_ptr_next_w;
            rd_ptr_reg <= rd_ptr_next_w;
            cnt_reg <= cnt_next_w;
        end
    end
endmodule
