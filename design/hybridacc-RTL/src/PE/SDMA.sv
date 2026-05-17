//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   SDMA
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Common utility package with type definitions, FP16 arithmetic, and shared constants.
// Dependencies:  hybridacc_utils_pkg
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
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
    output logic       dl_stall_out,
    output logic       bank_sel,
    input  v_fp16_t    ps_data,
    input  logic       ps_valid,
    output logic       ps_ready,
    output logic       dm_write_en,
    output logic [8:0] dm_write_addr,
    output logic [63:0] dm_write_data,
    output logic [7:0]  dm_write_mask
);
    typedef enum logic [1:0] { IDLE, RUN, WAIT_SWAP, FINISH } SDMAState;

    SDMAState state_reg;

    // Configuration registers
    logic [15:0] dma_base_static_reg, dma_len_static_reg, dma_stride_static_reg, dma_loop_static_reg;
    logic        reset_init_done_reg;

    // Runtime registers
    logic [15:0] dma_offset_active_reg, dma_len_active_rem_reg, dma_loops_active_rem_reg;
    logic bank_sel_active_reg;
    logic write_pending_reg;
    logic [8:0] write_addr_reg;
    logic [63:0] write_data_reg;

    // Next-state signals (combinational)
    SDMAState state_next;
    logic [15:0] dma_base_static_next, dma_len_static_next, dma_stride_static_next, dma_loop_static_next;
    logic [15:0] dma_offset_active_next, dma_len_active_rem_next, dma_loops_active_rem_next;
    logic bank_sel_active_next;
    logic write_pending_next;
    logic [8:0] write_addr_next;
    logic [63:0] write_data_next;
    logic       fire_w;

    function automatic logic [15:0] inc16(input logic [15:0] value);
        logic [15:0] result;
        logic        carry;

        carry = 1'b1;
        for (int i = 0; i < 16; i++) begin
            result[i] = value[i] ^ carry;
            carry = value[i] & carry;
        end
        return result;
    endfunction

    function automatic logic [15:0] dec16(input logic [15:0] value);
        logic [15:0] result;
        logic        borrow;

        borrow = 1'b1;
        for (int i = 0; i < 16; i++) begin
            result[i] = value[i] ^ borrow;
            borrow = ~value[i] & borrow;
        end
        return result;
    endfunction

    // Next-state combinational logic
    always_comb begin
        fire_w = 1'b0;

        // Default: hold current values
        state_next = state_reg;
        dma_base_static_next = dma_base_static_reg;
        dma_len_static_next = dma_len_static_reg;
        dma_stride_static_next = dma_stride_static_reg;
        dma_loop_static_next = dma_loop_static_reg;
        dma_offset_active_next = dma_offset_active_reg;
        dma_len_active_rem_next = dma_len_active_rem_reg;
        dma_loops_active_rem_next = dma_loops_active_rem_reg;
        bank_sel_active_next = bank_sel_active_reg;
        write_pending_next = 1'b0;
        write_addr_next = write_addr_reg;
        write_data_next = write_data_reg;

        fire_w = (state_reg == RUN) && ps_valid && (dma_len_active_rem_reg > 16'd0);

        // Reset active runtime registers
        if (reset_active) begin
            state_next = IDLE;
            dma_offset_active_next = 16'h0;
            dma_len_active_rem_next = 16'h0;
            dma_loops_active_rem_next = 16'h0;
            bank_sel_active_next = 1'b0;
            write_pending_next = 1'b0;
            write_addr_next = 9'h0;
            write_data_next = 64'h0;
        end

        // FSM state transitions
        case (state_reg)
            IDLE: begin
                // Configuration (only in IDLE)
                if (set_addr) dma_base_static_next = imm;
                else if (set_len) dma_len_static_next = inc16(imm);
                else if (set_loop) dma_loop_static_next = inc16(imm) | {15'h0, (&imm)};
                else if (set_mode) dma_stride_static_next = imm;

                // Allow swap even when idle
                if (swap_in) bank_sel_active_next = ~bank_sel_active_reg;

                // Start background store task
                if (active) begin
                    dma_offset_active_next = 16'h0;
                    dma_loops_active_rem_next = dma_loop_static_reg;
                    dma_len_active_rem_next = dma_len_static_reg;

                    if (dma_len_static_reg == 16'h0) begin
                        dma_loops_active_rem_next = dec16(dma_loop_static_reg);
                        state_next = WAIT_SWAP;
                    end else begin
                        state_next = RUN;
                    end
                end
            end

            RUN: begin
                if (fire_w) begin
                    // Buffer the write payload so DM sees registered data/address.
                    write_pending_next = 1'b1;
                    write_addr_next = dma_base_static_reg[8:0] + dma_offset_active_reg[8:0];
                    write_data_next = v_fp16_to_u64(ps_data);
                    dma_offset_active_next = dma_offset_active_reg + dma_stride_static_reg * 16'd2;
                    dma_len_active_rem_next = dma_len_active_rem_reg - 16'd1;
                    if (dma_len_active_rem_reg == 16'd1) begin
                        if (dma_loops_active_rem_reg > 16'd0)
                            dma_loops_active_rem_next = dec16(dma_loops_active_rem_reg);
                        state_next = WAIT_SWAP;
                    end
                end
            end

            WAIT_SWAP: begin
                if (swap_in) begin
                    bank_sel_active_next = ~bank_sel_active_reg;

                    if (dma_loops_active_rem_reg > 16'd0) begin
                        dma_offset_active_next = 16'h0;
                        dma_len_active_rem_next = dma_len_static_reg;

                        if (dma_len_static_reg == 16'h0) begin
                            dma_loops_active_rem_next = dec16(dma_loops_active_rem_reg);
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
        endcase
    end

    // Register update
    always_ff @(posedge clk or negedge reset_n) begin
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
            write_pending_reg <= 1'b0;
            write_addr_reg <= 9'h0;
            write_data_reg <= 64'h0;
            reset_init_done_reg <= 1'b0;
        end else if (!reset_init_done_reg) begin
            state_reg <= IDLE;
            dma_base_static_reg <= 16'h0;
            dma_len_static_reg <= 16'h0;
            dma_stride_static_reg <= 16'h0;
            dma_loop_static_reg <= 16'd1;
            dma_offset_active_reg <= 16'h0;
            dma_len_active_rem_reg <= 16'h0;
            dma_loops_active_rem_reg <= 16'h0;
            bank_sel_active_reg <= 1'b0;
            write_pending_reg <= 1'b0;
            write_addr_reg <= 9'h0;
            write_data_reg <= 64'h0;
            reset_init_done_reg <= 1'b1;
        end else begin
            state_reg <= state_next;
            dma_base_static_reg <= dma_base_static_next;
            dma_len_static_reg <= dma_len_static_next;
            dma_stride_static_reg <= dma_stride_static_next;
            dma_loop_static_reg <= dma_loop_static_next;
            dma_offset_active_reg <= dma_offset_active_next;
            dma_len_active_rem_reg <= dma_len_active_rem_next;
            dma_loops_active_rem_reg <= dma_loops_active_rem_next;
            bank_sel_active_reg <= bank_sel_active_next;
            write_pending_reg <= write_pending_next;
            write_addr_reg <= write_addr_next;
            write_data_reg <= write_data_next;
        end
    end

    // Output combinational logic
    always_comb begin
        busy = 1'b0;
        dm_write_en = write_pending_reg;
        dm_write_addr = write_addr_reg;
        dm_write_data = write_data_reg;
        dm_write_mask = write_pending_reg ? 8'hFF : 8'h00;
        bank_sel = bank_sel_active_reg;

        case (state_reg)
            RUN: busy = 1'b1;
            default: ;
        endcase

        ps_ready = (state_reg == RUN) && (dma_len_active_rem_reg > 16'd0);
        dl_stall_out = 1'b0;
    end
endmodule
