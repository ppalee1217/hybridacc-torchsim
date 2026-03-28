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
    localparam int PTR_W = (MAX_ELEMENTS <= 1) ? 1 : $clog2(MAX_ELEMENTS);

    OUT_T mem [0:MAX_ELEMENTS-1];
    logic [PTR_W-1:0] wr_ptr_reg, rd_ptr_reg;
    logic [PTR_W:0] cnt_reg;

    always_comb begin
        IN_T set_value;
        set_value = '0;

        data_out = (cnt_reg > 0) ? mem[rd_ptr_reg] : '0;
        set_valid = (cnt_reg >= CHUNKS_PER_PUSH);

        if (set_valid) begin
            for (int i = 0; i < CHUNKS_PER_PUSH; i++) begin
                int idx;
                idx = rd_ptr_reg + i;
                if (idx >= MAX_ELEMENTS) idx -= MAX_ELEMENTS;
                set_value[(i*$bits(OUT_T)) +: $bits(OUT_T)] = mem[idx];
            end
        end

        data_out_set = set_value;
    end

    always_ff @(posedge clk or negedge reset_n) begin
        logic do_pop, do_pop_set, do_push;
        logic [PTR_W-1:0] wr_n, rd_n;
        logic [PTR_W:0] cnt_n;
        logic [31:0] mask_popcount;
        logic [31:0] remaining_after_pop;

        if (!reset_n) begin
            wr_ptr_reg <= '0;
            rd_ptr_reg <= '0;
            cnt_reg <= '0;
            for (int i = 0; i < MAX_ELEMENTS; i++) mem[i] <= '0;
        end else begin
            mask_popcount = 0;
            for (int i = 0; i < CHUNKS_PER_PUSH; i++) begin
                if (mask_in[i]) mask_popcount++;
            end

            do_pop = 1'b0;
            do_pop_set = 1'b0;
            do_push = 1'b0;

            if (pop_set && (cnt_reg >= CHUNKS_PER_PUSH)) do_pop_set = 1'b1;
            else if (pop && (cnt_reg > 0)) do_pop = 1'b1;

            remaining_after_pop = cnt_reg - (do_pop ? 1 : 0) - (do_pop_set ? CHUNKS_PER_PUSH : 0);
            if (push && ((remaining_after_pop + mask_popcount) <= MAX_ELEMENTS)) do_push = 1'b1;

            wr_n = wr_ptr_reg;
            rd_n = rd_ptr_reg;
            cnt_n = cnt_reg;

            if (do_push) begin
                for (int i = 0; i < CHUNKS_PER_PUSH; i++) begin
                    if (mask_in[i]) begin
                        mem[wr_n] <= data_in[(i*$bits(OUT_T)) +: $bits(OUT_T)];
                        wr_n = (wr_n == MAX_ELEMENTS-1) ? '0 : (wr_n + 1'b1);
                    end
                end
                cnt_n = cnt_n + mask_popcount;
            end

            if (do_pop_set) begin
                if ((rd_n + CHUNKS_PER_PUSH) >= MAX_ELEMENTS) rd_n = rd_n + CHUNKS_PER_PUSH - MAX_ELEMENTS;
                else rd_n = rd_n + CHUNKS_PER_PUSH;
                cnt_n = cnt_n - CHUNKS_PER_PUSH;
            end else if (do_pop) begin
                rd_n = (rd_n == MAX_ELEMENTS-1) ? '0 : (rd_n + 1'b1);
                cnt_n = cnt_n - 1'b1;
            end

            wr_ptr_reg <= wr_n;
            rd_ptr_reg <= rd_n;
            cnt_reg <= cnt_n;
        end
    end

    always_comb begin
        empty = (cnt_reg == 0);
        full  = (cnt_reg > (MAX_ELEMENTS - CHUNKS_PER_PUSH));
    end
endmodule
