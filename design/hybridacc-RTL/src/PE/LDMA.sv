//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   LDMA
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

module LDMA (
    input  logic       clk,
    input  logic       reset_n,
    input  logic [15:0] imm,
    input  logic       set_addr,
    input  logic       set_len,
    input  logic       set_loop,
    input  logic       set_mode,
    input  logic [15:0] mode,
    input  logic       active,
    input  logic       next,
    input  logic       reset_active,
    output logic       busy,
    output logic       done,
    output logic       dl_stall_out,
    output logic [15:0] dm_read_addr,
    input  logic [63:0] dm_read_data,
    output v_fp16_t    dmrv_out
);
    typedef enum logic [1:0] { LOAD_BYTE, LOAD_HALF, LOAD_WORD, LOAD_DWORD } LDMARequestType;
    typedef enum logic [2:0] { IDLE, LOAD_PRE, LOAD_WAIT, LOAD_PIPELINE, DONE } LDMAState;

    // State registers
    LDMAState state_reg;
    v_fp16_t dmrv_reg;

    // Static configuration registers
    logic [15:0] dma_base_static_reg, dma_len_static_reg, dma_stride_static_reg, dma_loop_static_reg;
    logic dma_broadcast_static_reg;
    LDMARequestType request_type_static_reg;

    // Active runtime registers
    logic [15:0] dma_base_reg, dma_offset_reg, dma_len_reg, dma_stride_reg, dma_loop_reg;
    logic dma_broadcast_reg;
    LDMARequestType request_type_reg;

    // Next-state signals (combinational)
    LDMAState state_next;
    v_fp16_t dmrv_next;
    logic [15:0] dma_base_static_next, dma_len_static_next, dma_stride_static_next, dma_loop_static_next;
    logic dma_broadcast_static_next;
    LDMARequestType request_type_static_next;
    logic [15:0] dma_base_next, dma_offset_next, dma_len_next, dma_stride_next, dma_loop_next;
    logic dma_broadcast_next;
    LDMARequestType request_type_next;

    function automatic logic [63:0] mask_and_broadcast(
        input logic [63:0] v,
        input LDMARequestType request_type,
        input logic dma_broadcast
    );
        logic [63:0] out_v;
        logic [63:0] b;
        out_v = v;
        b = 64'h0;
        case (request_type)
            LOAD_BYTE: begin
                out_v = v & 64'hFF;
                if (dma_broadcast) begin
                    for (int i = 0; i < 4; i++) b |= (out_v << (i*16));
                    out_v = b;
                end
            end
            LOAD_HALF: begin
                out_v = v & 64'hFFFF;
                if (dma_broadcast) begin
                    for (int i = 0; i < 4; i++) b |= (out_v << (i*16));
                    out_v = b;
                end
            end
            LOAD_WORD: begin
                out_v = v & 64'hFFFF_FFFF;
                if (dma_broadcast) begin
                    for (int i = 0; i < 2; i++) b |= (out_v << (i*32));
                    out_v = b;
                end
            end
            default: begin
                out_v = v;
            end
        endcase
        return out_v;
    endfunction

    // Next-state combinational logic
    always_comb begin
        // Default: hold current values
        state_next = state_reg;
        dmrv_next = dmrv_reg;
        dma_base_static_next = dma_base_static_reg;
        dma_len_static_next = dma_len_static_reg;
        dma_stride_static_next = dma_stride_static_reg;
        dma_loop_static_next = dma_loop_static_reg;
        dma_broadcast_static_next = dma_broadcast_static_reg;
        request_type_static_next = request_type_static_reg;
        dma_base_next = dma_base_reg;
        dma_offset_next = dma_offset_reg;
        dma_len_next = dma_len_reg;
        dma_stride_next = dma_stride_reg;
        dma_loop_next = dma_loop_reg;
        dma_broadcast_next = dma_broadcast_reg;
        request_type_next = request_type_reg;

        // Static configuration updates
        if (set_addr) dma_base_static_next = imm;
        if (set_len) dma_len_static_next = imm;
        if (set_loop) dma_loop_static_next = imm;
        if (set_mode) begin
            dma_stride_static_next = imm;
            case (mode[2:0])
                3'd0: begin request_type_static_next = LOAD_BYTE;  dma_broadcast_static_next = 1'b0; end
                3'd1: begin request_type_static_next = LOAD_HALF;  dma_broadcast_static_next = 1'b0; end
                3'd2: begin request_type_static_next = LOAD_WORD;  dma_broadcast_static_next = 1'b0; end
                3'd3: begin request_type_static_next = LOAD_DWORD; dma_broadcast_static_next = 1'b0; end
                3'd4: begin request_type_static_next = LOAD_BYTE;  dma_broadcast_static_next = 1'b1; end
                3'd5: begin request_type_static_next = LOAD_HALF;  dma_broadcast_static_next = 1'b1; end
                3'd6: begin request_type_static_next = LOAD_WORD;  dma_broadcast_static_next = 1'b1; end
                default: begin request_type_static_next = LOAD_DWORD; dma_broadcast_static_next = 1'b0; end
            endcase
        end

        // Reset active runtime registers
        if (reset_active) begin
            state_next = IDLE;
            dma_base_next = 16'h0;
            dma_stride_next = 16'h0;
            dma_len_next = 16'h0;
            dma_loop_next = 16'h0;
            request_type_next = LOAD_DWORD;
            dma_broadcast_next = 1'b0;
            dma_offset_next = 16'h0;
        end

        // FSM state transitions
        case (state_reg)
            IDLE: begin
                if (active) begin
                    dma_base_next = dma_base_static_reg;
                    dma_stride_next = dma_stride_static_reg;
                    dma_len_next = dma_len_static_reg;
                    dma_loop_next = dma_loop_static_reg;
                    request_type_next = request_type_static_reg;
                    dma_broadcast_next = dma_broadcast_static_reg;
                    dma_offset_next = 16'h0;
                    if (dma_len_static_reg == 16'h0) state_next = IDLE;
                    else begin
                        dma_offset_next = dma_stride_static_reg * 16'd2;
                        state_next = LOAD_WAIT;
                    end
                end
            end
            LOAD_PRE: begin
                state_next = LOAD_WAIT;
                dma_offset_next = dma_offset_reg + dma_stride_reg * 16'd2;
            end
            LOAD_WAIT: begin
                state_next = LOAD_PIPELINE;
                dma_len_next = dma_len_reg - 16'd1;
                dmrv_next = u64_to_v_fp16(mask_and_broadcast(dm_read_data, request_type_reg, dma_broadcast_reg));
            end
            LOAD_PIPELINE: begin
                if (next) begin
                    if (dma_len_reg == 16'h0) begin
                        if (dma_loop_reg > 16'd1) begin
                            dma_loop_next = dma_loop_reg - 16'd1;
                            dma_offset_next = 16'h0;
                            dma_len_next = dma_len_static_reg;
                            state_next = LOAD_PRE;
                        end else begin
                            state_next = DONE;
                        end
                    end else begin
                        state_next = LOAD_PIPELINE;
                        dma_offset_next = dma_offset_reg + dma_stride_reg * 16'd2;
                        dma_len_next = dma_len_reg - 16'd1;
                    end
                    dmrv_next = u64_to_v_fp16(mask_and_broadcast(dm_read_data, request_type_reg, dma_broadcast_reg));
                end
            end
            DONE: begin
                state_next = IDLE;
            end
            default: state_next = IDLE;
        endcase
    end

    // Register update
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state_reg <= IDLE;
            dmrv_reg <= '0;
            dma_base_static_reg <= 16'h0;
            dma_len_static_reg <= 16'h0;
            dma_stride_static_reg <= 16'h0;
            dma_loop_static_reg <= 16'h0;
            dma_broadcast_static_reg <= 1'b0;
            request_type_static_reg <= LOAD_DWORD;
            dma_base_reg <= 16'h0;
            dma_offset_reg <= 16'h0;
            dma_len_reg <= 16'h0;
            dma_stride_reg <= 16'h0;
            dma_broadcast_reg <= 1'b0;
            request_type_reg <= LOAD_DWORD;
            dma_loop_reg <= 16'h0;
        end else begin
            state_reg <= state_next;
            dmrv_reg <= dmrv_next;
            dma_base_static_reg <= dma_base_static_next;
            dma_len_static_reg <= dma_len_static_next;
            dma_stride_static_reg <= dma_stride_static_next;
            dma_loop_static_reg <= dma_loop_static_next;
            dma_broadcast_static_reg <= dma_broadcast_static_next;
            request_type_static_reg <= request_type_static_next;
            dma_base_reg <= dma_base_next;
            dma_offset_reg <= dma_offset_next;
            dma_len_reg <= dma_len_next;
            dma_stride_reg <= dma_stride_next;
            dma_broadcast_reg <= dma_broadcast_next;
            request_type_reg <= request_type_next;
            dma_loop_reg <= dma_loop_next;
        end
    end

    // Output combinational logic
    always_comb begin
        busy = 1'b0;
        done = 1'b0;
        dm_read_addr = dma_base_reg + dma_offset_reg;
        dmrv_out = dmrv_reg;
        dl_stall_out = (state_reg != IDLE) && (state_reg != DONE) && (!next);

        case (state_reg)
            IDLE: begin
                if (active && (dma_len_static_reg != 16'h0)) begin
                    busy = 1'b1;
                    dm_read_addr = dma_base_static_reg;
                end
            end
            LOAD_PRE, LOAD_WAIT: busy = 1'b1;
            LOAD_PIPELINE: begin
                busy = 1'b1;
                if (next) begin
                    dm_read_addr = dma_base_reg + dma_offset_next;
                end
            end
            DONE: done = 1'b1;
            default: ;
        endcase
    end
endmodule
