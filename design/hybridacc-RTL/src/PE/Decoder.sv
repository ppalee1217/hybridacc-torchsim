//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   Decoder
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

module Decoder (
    input  pe_inst_t           inst_in,
    output pe_decode_signals_t decode_signals_out
);
    function automatic logic [1:0] getOpcode(input logic [15:0] w);
        return w[2:1];
    endfunction

    function automatic logic [1:0] getFunct2(input logic [15:0] w);
        return w[4:3];
    endfunction

    function automatic logic getFunc1(input logic [15:0] w);
        return w[5];
    endfunction

    function automatic logic [9:0] getPayload(input logic [15:0] w);
        return w[15:6];
    endfunction

    function automatic pe_decode_signals_t decode_inst(input pe_inst_t w);
        pe_decode_signals_t s;
        logic [1:0] opcode;
        logic [1:0] funct2;
        logic       func1;
        logic [9:0] payload;
        logic [2:0] func3;
        logic [4:0] reg5;
        logic [1:0] vtbits;

        s = pe_decode_signals_zero();
        opcode = getOpcode(w);
        funct2 = getFunct2(w);
        func1 = getFunc1(w);
        payload = getPayload(w);
        func3 = payload[2:0];
        reg5 = payload[9:5];
        vtbits = payload[4:3];

        s.inst = w;
        s.func3 = func3;
        s.loop_end = w[0];

        if ((opcode == 2) && (funct2 == 3) && (func1 == 0)) begin
            s.halt = 1'b1;
            s.loop_end = 1'b0;
            return s;
        end

        if ((opcode == 2) && (funct2 == 1) && (func1 == 1)) begin
            s.is_swap = payload[0];
            return s;
        end

        if ((opcode == 2) && (funct2 == 1) && (func1 == 0)) begin
            s.sys_sdma_act = payload[7];
            s.sys_sdma_rst = payload[6];
            s.sys_ldma_act = payload[5];
            s.sys_ldma_rst = payload[4];
            s.sys_rst_pid  = payload[3];
            s.sys_rst_tid  = payload[2];
            s.tr_clear_regs = payload[1];
            s.pr_clear_regs = payload[0];
            return s;
        end

        if ((opcode == 2) && (funct2 == 2) && (func1 == 0)) begin
            s.nop = 1'b1;
            return s;
        end

        if ((opcode == 2) && (funct2 == 0) && (func1 == 0)) begin
            s.imm = {6'b0, payload};
            s.loop_in = 1'b1;
            return s;
        end

        if ((opcode == 0) && (funct2 == 0)) begin
            s.imm = {6'b0, payload};
            s.DMA_setaddr = 1'b1;
            s.DMA_is_sdma = func1;
            return s;
        end

        if ((opcode == 0) && (funct2 == 1)) begin
            s.imm = {6'b0, payload};
            s.DMA_setlen = 1'b1;
            s.DMA_is_sdma = func1;
            return s;
        end

        if ((opcode == 0) && (funct2 == 2)) begin
            s.imm = {6'b0, payload};
            s.DMA_setloop = 1'b1;
            s.DMA_is_sdma = func1;
            return s;
        end

        if ((opcode == 0) && (funct2 == 3) && (func1 == 0)) begin
            s.imm = {13'b0, payload[5:3]};
            s.DMA_setmode = 1'b1;
            s.DMA_is_sdma = (func3 == 3'b111);
            return s;
        end

        if ((opcode == 0) && (funct2 == 3) && (func1 == 1)) begin
            if (func3 == 0) begin
                s.rid3 = {28'b0, payload[9:6]};
                s.tr_en = 1'b1;
                s.tr_write = 1'b1;
                s.pd_load = 1'b1;
            end else if (func3 == 1) begin
                s.rid3 = {30'b0, payload[4:3]};
                s.tr_en = 1'b1;
                s.tr_write_v = 1'b1;
                s.pd_load_v = 1'b1;
            end else if (func3 == 2) begin
                s.imm = {14'b0, payload[4:3]};
                s.tr_en = 1'b1;
                s.tr_shift = 1'b1;
            end
            return s;
        end

        if (opcode == 1) begin
            if (funct2 == 0) begin
                if (func3 == 0 || func3 == 1) begin
                    s.rid5 = {27'b0, reg5};
                    s.rid3 = {30'b0, vtbits};
                    s.pr_en = 1'b1;
                    s.pr_write = 1'b1;
                    s.pr_mode = 1'b0;
                    s.tr_en = 1'b1;
                    s.vaddu_en = 1'b1;
                    s.vaddu_mode = 32'd0;
                    s.LDMA_next = func1;
                    if (func3 == 1) begin
                        s.pr_use_vcounter = 1'b1;
                        s.tr_use_vcounter = 1'b1;
                        if (reg5 == 5'd31) s.sys_rst_pid = 1'b1;
                        else s.pr_incr_vcounter = 1'b1;
                        if (vtbits == 2'd3) s.sys_rst_tid = 1'b1;
                        else s.tr_incr_vcounter = 1'b1;
                    end
                end
                return s;
            end

            if (funct2 == 1) begin
                if (func3 == 0 || func3 == 1) begin
                    s.rid5 = {27'b0, reg5};
                    s.rid3 = {30'b0, vtbits};
                    s.pr_en = 1'b1;
                    s.pr_write = 1'b1;
                    s.pr_mode = 1'b1;
                    s.tr_en = 1'b1;
                    s.vaddu_en = 1'b1;
                    s.vaddu_mode = 32'd1;
                    s.LDMA_next = func1;
                    if (func3 == 1) begin
                        s.pr_use_vcounter = 1'b1;
                        s.tr_use_vcounter = 1'b1;
                        if (reg5 == 5'd31) s.sys_rst_pid = 1'b1;
                        else s.pr_incr_vcounter = 1'b1;
                        if (vtbits == 2'd3) s.sys_rst_tid = 1'b1;
                        else s.tr_incr_vcounter = 1'b1;
                    end
                end
                return s;
            end

            if (funct2 == 2) begin
                if (func3 == 0 || func3 == 1) begin
                    s.rid5 = {27'b0, reg5};
                    s.pli_plo_operation = 1'b1;
                    s.pr_en = 1'b1;
                    s.pr_mode = 1'b1;
                    s.vaddu_en = 1'b1;
                    s.vaddu_mode = 32'd1;
                    if (func3 == 1) begin
                        s.pr_use_vcounter = 1'b1;
                        if (reg5 == 5'd31) s.sys_rst_pid = 1'b1;
                        else s.pr_incr_vcounter = 1'b1;
                    end
                end
                return s;
            end

            if (funct2 == 3) begin
                if ((func3 == 0) || (func3 == 2)) begin
                    s.rid5 = {27'b0, reg5};
                    s.pli_plo_operation = 1'b1;
                    s.pr_en = 1'b1;
                    s.pr_mode = 1'b1;
                    s.vaddu_en = 1'b1;
                    s.vaddu_mode = 32'd1;
                end
                if ((func3 == 1) || (func3 == 3)) begin
                    s.rid5 = {27'b0, reg5};
                    s.pr_use_vcounter = 1'b1;
                    s.pli_plo_operation = 1'b1;
                    s.pr_en = 1'b1;
                    s.pr_mode = 1'b1;
                    s.vaddu_en = 1'b1;
                    s.vaddu_mode = 32'd1;
                    if (reg5 == 5'd31) s.sys_rst_pid = 1'b1;
                    else s.pr_incr_vcounter = 1'b1;
                end
                if ((func3 == 0) || (func3 == 1)) begin
                    s.rid3 = {30'b0, vtbits};
                    s.tr_en = 1'b1;
                    s.tr_write_v = 1'b1;
                    s.pd_load_v = 1'b1;
                end
                if ((func3 == 2) || (func3 == 3)) begin
                    s.imm = {14'b0, vtbits};
                    s.tr_en = 1'b1;
                    s.tr_shift = 1'b1;
                end
                return s;
            end
        end

        s.loop_end = 1'b0;
        s.nop = 1'b1;
        return s;
    endfunction

    always_comb begin
        if (inst_in == 16'h0000) begin
            decode_signals_out = pe_decode_signals_zero();
        end else begin
            decode_signals_out = decode_inst(inst_in);
        end
    end
endmodule
