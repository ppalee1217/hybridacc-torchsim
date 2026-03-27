// Module: SDMA
// Function: Store DMA engine that writes PE output vectors back into DataMemory.
import hybridacc_utils_pkg::*;

module SDMA (
    input  logic       clk,
    input  logic       reset_n,
    input  logic [15:0] imm,
    input  logic       set_addr,
    input  logic       set_len,
    input  logic       set_loop,
    input  logic       set_mode,
    input  logic       swap_in,
    input  logic       active,
    input  logic       reset_active,
    output logic       busy,
    output logic       done,
    output logic       dl_stall_out,
    output logic       bank_sel,
    input  v_fp16_t    ps_data,
    input  logic       ps_valid,
    output logic       ps_ready,
    output logic       dm_write_en,
    output logic [15:0] dm_write_addr,
    output logic [63:0] dm_write_data,
    output logic [7:0]  dm_write_mask
);
    typedef enum logic [1:0] { IDLE, RUN, WAIT_SWAP, FINISH } SDMAState;

    SDMAState state_reg, state_next;

    logic [15:0] dma_base_static_reg, dma_len_static_reg, dma_stride_static_reg, dma_loop_static_reg;
    logic [15:0] dma_offset_active_reg, dma_len_active_rem_reg, dma_loops_active_rem_reg;
    logic bank_sel_active_reg;
    logic [1:0] bank_valid_active_reg;

    function automatic logic [15:0] normalize_loop_count(input logic [15:0] v);
        return (v == 16'h0) ? 16'd1 : v;
    endfunction

    // Using always @(*) instead of always_comb because config/state registers
    // are conditionally latched here and also reset in always_ff.
    always @(*) begin
        logic fire;
        logic [1:0] mask;

        state_next = state_reg;
        fire = (state_reg == RUN) && ps_valid && (dma_len_active_rem_reg > 0);
        mask = bank_sel_active_reg ? 2'b10 : 2'b01;

        if (reset_active) begin
            state_next = IDLE;
            dma_offset_active_reg = 16'h0;
            dma_len_active_rem_reg = 16'h0;
            dma_loops_active_rem_reg = 16'h0;
            bank_sel_active_reg = 1'b0;
            bank_valid_active_reg = 2'b00;
        end

        case (state_reg)
            IDLE: begin
                if (set_addr) dma_base_static_reg = imm;
                else if (set_len) dma_len_static_reg = imm + 16'd1;
                else if (set_loop) dma_loop_static_reg = imm + 16'd1;
                else if (set_mode) dma_stride_static_reg = imm;

                if (swap_in) bank_sel_active_reg = ~bank_sel_active_reg;

                if (active) begin
                    dma_offset_active_reg = 16'h0;
                    dma_loops_active_rem_reg = normalize_loop_count(dma_loop_static_reg);
                    dma_len_active_rem_reg = dma_len_static_reg;
                    bank_valid_active_reg = bank_valid_active_reg & (~mask);

                    if (dma_len_static_reg == 16'h0) begin
                        if (dma_loops_active_rem_reg > 0)
                            dma_loops_active_rem_reg = dma_loops_active_rem_reg - 16'd1;
                        state_next = WAIT_SWAP;
                        bank_valid_active_reg = bank_valid_active_reg | mask;
                    end else begin
                        state_next = RUN;
                    end
                end
            end

            RUN: begin
                if (fire) begin
                    dma_offset_active_reg = dma_offset_active_reg + dma_stride_static_reg * 16'd2;
                    dma_len_active_rem_reg = dma_len_active_rem_reg - 16'd1;
                    if (dma_len_active_rem_reg == 16'd1) begin
                        if (dma_loops_active_rem_reg > 0)
                            dma_loops_active_rem_reg = dma_loops_active_rem_reg - 16'd1;
                        bank_valid_active_reg = bank_valid_active_reg | mask;
                        state_next = WAIT_SWAP;
                    end
                end
            end

            WAIT_SWAP: begin
                if (swap_in) begin
                    logic new_bank;
                    logic [1:0] new_mask;
                    new_bank = ~bank_sel_active_reg;
                    bank_sel_active_reg = new_bank;
                    new_mask = new_bank ? 2'b10 : 2'b01;

                    if (dma_loops_active_rem_reg > 0) begin
                        dma_offset_active_reg = 16'h0;
                        dma_len_active_rem_reg = dma_len_static_reg;
                        bank_valid_active_reg = bank_valid_active_reg & (~new_mask);

                        if (dma_len_static_reg == 16'h0) begin
                            dma_loops_active_rem_reg = dma_loops_active_rem_reg - 16'd1;
                            bank_valid_active_reg = bank_valid_active_reg | new_mask;
                            state_next = WAIT_SWAP;
                        end else begin
                            state_next = RUN;
                        end
                    end else begin
                        state_next = FINISH;
                    end
                end
            end

            FINISH: state_next = IDLE;
            default: state_next = IDLE;
        endcase
    end

    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state_reg <= IDLE;
            dma_base_static_reg <= 16'h0;
            dma_len_static_reg <= 16'h0;
            dma_stride_static_reg <= 16'h0;
            dma_loop_static_reg <= 16'h0;
            dma_offset_active_reg <= 16'h0;
            dma_len_active_rem_reg <= 16'h0;
            dma_loops_active_rem_reg <= 16'h0;
            bank_sel_active_reg <= 1'b0;
            bank_valid_active_reg <= 2'b00;
        end else begin
            state_reg <= state_next;
        end
    end

    always_comb begin
        logic fire;
        fire = (state_reg == RUN) && ps_valid && (dma_len_active_rem_reg > 0);

        busy = 1'b0;
        done = 1'b0;
        dm_write_en = 1'b0;
        dm_write_addr = dma_base_static_reg + dma_offset_active_reg;
        dm_write_data = 64'h0;
        dm_write_mask = 8'h00;
        bank_sel = bank_sel_active_reg;

        case (state_reg)
            RUN: begin
                busy = 1'b1;
                if (fire) begin
                    dm_write_en = 1'b1;
                    dm_write_addr = dma_base_static_reg + dma_offset_active_reg;
                    dm_write_data = v_fp16_to_u64(ps_data);
                    dm_write_mask = 8'hFF;
                end
            end
            FINISH: done = 1'b1;
            default: ;
        endcase

        ps_ready = (state_reg == RUN) && (dma_len_active_rem_reg > 0);
        dl_stall_out = 1'b0;
    end
endmodule
