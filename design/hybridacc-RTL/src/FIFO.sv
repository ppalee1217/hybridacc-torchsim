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
    localparam logic [PTR_W:0]   DEPTH_COUNT = DEPTH;
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

    function automatic logic [PTR_W:0] count_add(
        input logic [PTR_W:0] lhs,
        input logic [PTR_W:0] rhs
    );
        logic [PTR_W:0] result;
        logic           carry;

        carry = 1'b0;
        for (int i = 0; i <= PTR_W; i++) begin
            result[i] = lhs[i] ^ rhs[i] ^ carry;
            carry = (lhs[i] & rhs[i]) | (lhs[i] & carry) | (rhs[i] & carry);
        end
        return result;
    endfunction

    function automatic logic [PTR_W:0] count_sub(
        input logic [PTR_W:0] lhs,
        input logic [PTR_W:0] rhs
    );
        logic [PTR_W:0] result;
        logic           borrow;

        borrow = 1'b0;
        for (int i = 0; i <= PTR_W; i++) begin
            result[i] = lhs[i] ^ rhs[i] ^ borrow;
            borrow = (~lhs[i] & rhs[i]) | (~(lhs[i] ^ rhs[i]) & borrow);
        end
        return result;
    endfunction

    function automatic logic [PTR_W-1:0] ptr_inc_if(
        input logic [PTR_W-1:0] ptr,
        input logic             enable
    );
        logic [PTR_W-1:0] result;
        logic             carry;

        carry = enable;
        for (int i = 0; i < PTR_W; i++) begin
            result[i] = ptr[i] ^ carry;
            carry = ptr[i] & carry;
        end
        return result;
    endfunction

    assign data_out = (cnt_reg > 0) ? mem[rd_ptr_reg] : '0;

    always_comb begin
        fifo_empty_w = (cnt_reg == '0);
        fifo_full_w  = (cnt_reg == DEPTH_COUNT);
        empty = fifo_empty_w;
        full  = fifo_full_w;

        do_pop_w = pop && !fifo_empty_w;
        do_push_w = push && (!fifo_full_w || pop);

        wr_ptr_next_w = ptr_inc_if(wr_ptr_reg, do_push_w);
        rd_ptr_next_w = ptr_inc_if(rd_ptr_reg, do_pop_w);
        cnt_next_w = count_add(
            count_sub(cnt_reg, {(PTR_W+1){do_pop_w}} & CNT_ONE),
            {(PTR_W+1){do_push_w}} & CNT_ONE
        );
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            wr_ptr_reg <= '0;
            rd_ptr_reg <= '0;
            cnt_reg <= '0;
            for (int unsigned i = 0; i < DEPTH; i++) mem[i] <= '0;
        end else if (clear) begin
            wr_ptr_reg <= '0;
            rd_ptr_reg <= '0;
            cnt_reg <= '0;
            for (int unsigned i = 0; i < DEPTH; i++) mem[i] <= '0;
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
