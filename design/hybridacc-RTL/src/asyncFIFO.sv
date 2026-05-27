//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   asyncFIFO
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
module asyncFIFO #(
    parameter type IN_T = logic [63:0],
    parameter type OUT_T = logic [15:0],
    parameter int unsigned DEPTH = 4,
    parameter int unsigned CHUNKS_PER_PUSH = ($bits(IN_T) / $bits(OUT_T))
) (
    input  logic clk,
    input  logic reset_n,
    input  IN_T  data_in,
    input  logic [CHUNKS_PER_PUSH-1:0] mask_in,
    input  logic push,
    output OUT_T data_out,
    input  logic pop,
    output IN_T  data_out_set,
    input  logic pop_set,
    output logic set_valid,
    output logic empty,
    output logic full
);
    localparam int unsigned MAX_ELEMENTS = DEPTH * CHUNKS_PER_PUSH;
    localparam int unsigned OUT_T_BITS   = $bits(OUT_T);
    localparam int PTR_W = (MAX_ELEMENTS <= 1) ? 1 : $clog2(MAX_ELEMENTS);
    localparam logic [PTR_W-1:0] PTR_ONE  = {{(PTR_W-1){1'b0}}, 1'b1};
    localparam logic [PTR_W:0]   MAX_COUNT = (PTR_W + 1)'(MAX_ELEMENTS);
    localparam logic [PTR_W:0]   SET_COUNT = (PTR_W + 1)'(CHUNKS_PER_PUSH);
    localparam logic [PTR_W:0]   FULL_COUNT = (PTR_W + 1)'(MAX_ELEMENTS - CHUNKS_PER_PUSH);
    localparam logic [PTR_W:0]   CNT_ONE = {{PTR_W{1'b0}}, 1'b1};

    OUT_T mem [0:MAX_ELEMENTS-1];
    logic [PTR_W-1:0] wr_ptr_reg, rd_ptr_reg;
    logic [PTR_W:0] cnt_reg;

    logic             fifo_empty_w;
    logic             fifo_full_w;
    logic             set_valid_w;
    logic             do_pop_w;
    logic             do_pop_set_w;
    logic             do_push_w;
    logic [PTR_W:0]   mask_popcount_w;
    logic [PTR_W:0]   remaining_after_pop_w;
    logic [PTR_W-1:0] wr_ptr_next_w;
    logic [PTR_W-1:0] rd_ptr_next_w;
    logic [PTR_W:0]   cnt_next_w;

    function automatic logic [PTR_W-1:0] ptr_add(
        input logic [PTR_W-1:0] ptr,
        input logic [PTR_W-1:0] amount
    );
        return ptr + amount;
    endfunction

    always_comb begin
        fifo_empty_w = (cnt_reg == '0);
        fifo_full_w = (cnt_reg > FULL_COUNT);
        set_valid_w = (cnt_reg >= SET_COUNT);
        empty = fifo_empty_w;
        full = fifo_full_w;
        set_valid = set_valid_w;

        mask_popcount_w = '0;
        for (int unsigned i = 0; i < CHUNKS_PER_PUSH; i++) begin
            mask_popcount_w = mask_popcount_w + {{PTR_W{1'b0}}, mask_in[i]};
        end

        do_pop_set_w = pop_set && set_valid_w;
        do_pop_w = pop && !fifo_empty_w && !do_pop_set_w;

        remaining_after_pop_w = cnt_reg;
        if (do_pop_set_w) begin
            remaining_after_pop_w = cnt_reg - SET_COUNT;
        end else if (do_pop_w) begin
            remaining_after_pop_w = cnt_reg - CNT_ONE;
        end

        do_push_w = push && ((remaining_after_pop_w + mask_popcount_w) <= MAX_COUNT);

        wr_ptr_next_w = wr_ptr_reg;
        for (int unsigned i = 0; i < CHUNKS_PER_PUSH; i++) begin
            if (do_push_w && mask_in[i]) begin
                wr_ptr_next_w = wr_ptr_next_w + PTR_ONE;
            end
        end

        rd_ptr_next_w = rd_ptr_reg;
        if (do_pop_set_w) begin
            rd_ptr_next_w = rd_ptr_reg + SET_COUNT[PTR_W-1:0];
        end else if (do_pop_w) begin
            rd_ptr_next_w = rd_ptr_reg + PTR_ONE;
        end

        cnt_next_w = remaining_after_pop_w + (do_push_w ? mask_popcount_w : '0);
    end

    always_comb begin
        data_out = fifo_empty_w ? '0 : mem[rd_ptr_reg];
        data_out_set = '0;

        if (set_valid_w) begin
            for (int unsigned i = 0; i < CHUNKS_PER_PUSH; i++) begin
                data_out_set[(i*OUT_T_BITS) +: OUT_T_BITS] = mem[ptr_add(rd_ptr_reg, $bits(rd_ptr_reg)'(i))];
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            wr_ptr_reg <= '0;
            rd_ptr_reg <= '0;
            cnt_reg <= '0;
            for (int unsigned i = 0; i < MAX_ELEMENTS; i++) begin
                mem[i] <= '0;
            end
        end else begin
            logic [PTR_W-1:0] wr_write_ptr;

            wr_write_ptr = wr_ptr_reg;
            if (do_push_w) begin
                for (int unsigned i = 0; i < CHUNKS_PER_PUSH; i++) begin
                    if (mask_in[i]) begin
                        mem[wr_write_ptr] <= data_in[(i*OUT_T_BITS) +: OUT_T_BITS];
                        wr_write_ptr = wr_write_ptr + PTR_ONE;
                    end
                end
            end

            wr_ptr_reg <= wr_ptr_next_w;
            rd_ptr_reg <= rd_ptr_next_w;
            cnt_reg <= cnt_next_w;
        end
    end
endmodule
