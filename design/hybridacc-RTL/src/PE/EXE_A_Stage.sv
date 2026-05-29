//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   EXE_A_Stage
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   EXE_A pipeline stage — 8-state FSM with VMAC 3-stage reduction
//                pipeline, PLI/PLO multi-cycle handshake, and PLO output buffer.
//                Faithfully matches EXE_A_stage.hpp ESL behaviour.
// Dependencies:  hybridacc_utils_pkg, VADDU, PsumRegFile
// Revision:
//   2026/03/28 - Initial version
//   2026/03/29 - Rewrite: add FSM (RTL-006), VMAC pipeline (RTL-007),
//                PLO buffer (RTL-008) to match ESL
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;

module EXE_A_Stage (
    input  logic             clk,
    input  logic             reset_n,
    input  logic             stage_reset,
    input  logic             pe_running,
    input  v_fp16_t          vmul_out_in,
    input  pe_decode_signals_t EXE_M_decode_signals_in,
    input  logic             valid_in,
    output logic             ready_out,
    output logic             halted_out,
    output logic             stall_port_pli,
    output logic             stall_port_plo,
    input  logic [63:0]      pli_data,
    input  logic             pli_valid,
    output logic             pli_ready,
    output logic [63:0]      plo_data,
    output logic             plo_valid,
    input  logic             plo_ready
);

    // =========================================================================
    // FSM State Encoding
    // =========================================================================
    typedef enum logic [2:0] {
        S_IDLE            = 3'd0,
        S_NORMAL_MODE     = 3'd1,
        S_VMAC_S1         = 3'd2,
        S_VMAC_S2         = 3'd3,
        S_VMAC_S3         = 3'd4,
        S_WAIT_PLI        = 3'd5,
        S_EXEC_PLI_VADDU  = 3'd6,
        S_WAIT_PLO        = 3'd7
    } exe_a_state_t;

    // =========================================================================
    // Pipeline Registers
    // =========================================================================
    exe_a_state_t     state_reg;
    pe_decode_signals_t decode_reg, decode_s1_reg, decode_s2_reg;
    v_fp16_t          vmul_data_reg;
    v_fp16_t          pli_data_reg;
    v_fp16_t          vaddu_result_reg;
    logic             halted_reg;

    // PLO output buffer
    logic             plo_buf_valid_reg;
    logic [63:0]      plo_buf_data_reg;
    logic             plo_pending_valid_reg;

    // VMAC 3-stage pipeline registers
    fp16_t            s1_reg0, s1_reg1, s2_reg_r;

    // =========================================================================
    // Next-State Signals
    // =========================================================================
    exe_a_state_t     state_next;
    pe_decode_signals_t decode_next, decode_s1_next, decode_s2_next;
    v_fp16_t          vmul_data_next;
    v_fp16_t          pli_data_next;
    v_fp16_t          vaddu_result_next;
    logic             halted_next;

    logic             plo_buf_valid_next;
    logic [63:0]      plo_buf_data_next;
    logic             plo_pending_valid_next;
    logic             plo_buf_do_pop_w;
    logic             plo_buf_can_push_w;
    logic             plo_buf_produce_w;
    logic [63:0]      plo_buf_vpsum_data_w;

    fp16_t            s1_reg0_next, s1_reg1_next, s2_reg_next;

    // =========================================================================
    // VADDU Interface Signals
    // =========================================================================
    v_fp16_t vaddu_op1, vaddu_op2, vaddu_result_sig;

    // =========================================================================
    // PsumRegFile Interface Signals
    // =========================================================================
    logic             pr_enable;
    logic [31:0]      pr_pid;
    logic             pr_vpid_write_en;
    logic [31:0]      pr_mode;
    fp16_t            pr_p_in;
    v_fp16_t          pr_vp_in;
    fp16_t            pr_p_out;
    v_fp16_t          pr_vp_out;
    logic             pr_clear_regs;
    logic             pr_use_pcounter;
    logic             pr_clear_pcounter;
    logic             pr_incr_pcounter;

    // =========================================================================
    // Internal wire for ready_out (used in combinational logic)
    // =========================================================================
    logic ready_out_w;

    // =========================================================================
    // Sub-module Instantiation
    // =========================================================================
    VADDU vaddu (
        .op1    (vaddu_op1),
        .op2    (vaddu_op2),
        .result (vaddu_result_sig)
    );

    PsumRegFile PR (
        .clk            (clk),
        .reset_n        (reset_n),
        .enable         (pr_enable),
        .pid            (pr_pid),
        .p_in           (pr_p_in),
        .vp_in          (pr_vp_in),
        .vpid_write_en  (pr_vpid_write_en),
        .mode           (pr_mode),
        .p_out          (pr_p_out),
        .vp_out         (pr_vp_out),
        .clear_regs     (pr_clear_regs),
        .use_pcounter   (pr_use_pcounter),
        .clear_pcounter (pr_clear_pcounter),
        .incr_pcounter  (pr_incr_pcounter)
    );

    // =========================================================================
    // Combinational: Ready Out Logic (Backpressure)
    // =========================================================================
    always_comb begin
        ready_out_w = 1'b0;
        case (state_reg)
            S_IDLE, S_NORMAL_MODE: begin
                // Accept unless incoming VPSUM would overflow PLO buffer
                if (valid_in && EXE_M_decode_signals_in.pli_plo_operation
                    && plo_buf_valid_reg && !plo_ready)
                    ready_out_w = 1'b0;
                else
                    ready_out_w = 1'b1;
            end
            S_VMAC_S1: begin
                if (valid_in) begin
                    // If next is VMAC, pipeline it. Otherwise stall to drain.
                    if (EXE_M_decode_signals_in.vaddu_en
                        && EXE_M_decode_signals_in.vaddu_mode == 32'd0)
                        ready_out_w = 1'b1;
                    else
                        ready_out_w = 1'b0;
                end else begin
                    ready_out_w = 1'b1;
                end
            end
            S_EXEC_PLI_VADDU: begin
                ready_out_w = !plo_buf_valid_reg || plo_ready;
            end
            S_WAIT_PLO: begin
                ready_out_w = 1'b0;
            end
            default: begin  // S_VMAC_S2, S_VMAC_S3, S_WAIT_PLI
                ready_out_w = 1'b0;
            end
        endcase
    end

    assign ready_out = ready_out_w;

    // =========================================================================
    // Combinational: VADDU Operand Muxing
    // =========================================================================
    always_comb begin
        vaddu_op1 = '0;
        vaddu_op2 = '0;

        case (state_reg)
            S_VMAC_S1, S_VMAC_S2, S_VMAC_S3: begin
                // VMAC reduction: pair-wise add across 4 lanes
                vaddu_op1.lanes[0] = vmul_data_reg.lanes[0];
                vaddu_op1.lanes[1] = vmul_data_reg.lanes[2];
                vaddu_op1.lanes[2] = s1_reg0;
                vaddu_op1.lanes[3] = pr_p_out;

                vaddu_op2.lanes[0] = vmul_data_reg.lanes[1];
                vaddu_op2.lanes[1] = vmul_data_reg.lanes[3];
                vaddu_op2.lanes[2] = s1_reg1;
                vaddu_op2.lanes[3] = s2_reg_r;
            end
            S_NORMAL_MODE: begin
                vaddu_op1 = vmul_data_reg;
                vaddu_op2 = pr_vp_out;
            end
            S_EXEC_PLI_VADDU, S_WAIT_PLI, S_WAIT_PLO: begin
                vaddu_op1 = pli_data_reg;
                vaddu_op2 = pr_vp_out;
            end
            default: begin
                vaddu_op1 = '0;
                vaddu_op2 = '0;
            end
        endcase
    end

    // =========================================================================
    // Combinational: PsumRegFile Control
    // =========================================================================
    always_comb begin
        pr_enable         = decode_reg.pr_en;
        pr_pid            = decode_reg.rid5;
        pr_use_pcounter   = decode_reg.pr_use_vcounter;
        pr_clear_regs     = decode_reg.pr_clear_regs;
        pr_clear_pcounter = decode_reg.sys_rst_pid;
        pr_mode           = {31'b0, decode_reg.pr_mode};
        pr_p_in           = 16'h0000;
        pr_vp_in          = '0;
        pr_vpid_write_en = 1'b0;
        pr_incr_pcounter = 1'b0;
        case (state_reg)
            S_VMAC_S1, S_VMAC_S2, S_VMAC_S3: begin
                pr_enable         = decode_s2_reg.pr_en;
                pr_pid            = decode_s2_reg.rid5;
                pr_use_pcounter   = decode_s2_reg.pr_use_vcounter;
                pr_clear_regs     = decode_s2_reg.pr_clear_regs;
                pr_clear_pcounter = decode_s2_reg.sys_rst_pid;
                pr_mode           = 32'd0;
                pr_p_in           = vaddu_result_sig.lanes[3];
                pr_vpid_write_en  = decode_s2_reg.pr_write;
                pr_incr_pcounter  = decode_s2_reg.pr_incr_vcounter;
            end
            S_NORMAL_MODE: begin
                pr_mode          = 32'd1;
                pr_vp_in         = vaddu_result_sig;
                pr_vpid_write_en = decode_reg.pr_write;
                pr_incr_pcounter = decode_reg.pr_incr_vcounter;
            end
            S_EXEC_PLI_VADDU: begin
                pr_incr_pcounter = decode_reg.pr_incr_vcounter;
            end
        endcase
    end

    // =========================================================================
    // Combinational: PLO Buffer Next-State
    // =========================================================================
    always_comb begin
        plo_buf_do_pop_w = plo_buf_valid_reg && plo_ready;
        plo_buf_can_push_w = !plo_buf_valid_reg || plo_ready;
        plo_buf_produce_w =
            ((state_reg == S_EXEC_PLI_VADDU) && plo_buf_can_push_w)
            || ((state_reg == S_WAIT_PLO) && !plo_buf_valid_reg && plo_pending_valid_reg);
        plo_buf_vpsum_data_w = (state_reg == S_WAIT_PLO)
            ? v_fp16_to_u64(vaddu_result_reg)
            : v_fp16_to_u64(vaddu_result_sig);
    end

    // =========================================================================
    // Combinational: PLO Buffer Next-State
    // =========================================================================
    always_comb begin
        plo_buf_valid_next = plo_buf_valid_reg;
        plo_buf_data_next  = plo_buf_data_reg;

        if (stage_reset) begin
            plo_buf_valid_next = 1'b0;
            plo_buf_data_next  = 64'd0;
        end else begin
            if (plo_buf_do_pop_w && !plo_buf_produce_w) begin
                plo_buf_valid_next = 1'b0;
            end else if (!plo_buf_do_pop_w && plo_buf_produce_w) begin
                plo_buf_valid_next = 1'b1;
                plo_buf_data_next  = plo_buf_vpsum_data_w;
            end else if (plo_buf_do_pop_w && plo_buf_produce_w) begin
                plo_buf_valid_next = 1'b1;
                plo_buf_data_next  = plo_buf_vpsum_data_w;
            end
        end
    end

    // =========================================================================
    // Combinational: PLI Handshake
    // =========================================================================
    always_comb begin
        logic accepting_new, next_is_vpsum, accept_vpsum;

        accepting_new = ready_out_w && valid_in;
        next_is_vpsum = EXE_M_decode_signals_in.pli_plo_operation;
        accept_vpsum  = accepting_new && next_is_vpsum;

        pli_ready = 1'b0;
        if (state_reg == S_WAIT_PLI) begin
            pli_ready = 1'b1;
        end else if (state_reg == S_IDLE
                  || state_reg == S_EXEC_PLI_VADDU) begin
            pli_ready = accept_vpsum;
        end
    end

    // =========================================================================
    // Combinational: Output Signals
    // =========================================================================
    always_comb begin
        halted_out     = halted_reg;
        stall_port_pli = (state_reg == S_WAIT_PLI);
        stall_port_plo = (state_reg == S_WAIT_PLO);
        plo_valid      = plo_buf_valid_reg;
        plo_data       = plo_buf_data_reg;
    end

    // =========================================================================
    // Combinational: Next-State Logic (Main FSM)
    // =========================================================================
    always_comb begin
        // Default: hold current values
        state_next         = state_reg;
        decode_next        = decode_reg;
        decode_s1_next     = decode_s1_reg;
        decode_s2_next     = decode_s2_reg;
        vmul_data_next     = vmul_data_reg;
        pli_data_next      = pli_data_reg;
        vaddu_result_next  = vaddu_result_reg;
        halted_next        = halted_reg;
        plo_pending_valid_next = plo_pending_valid_reg;
        s1_reg0_next       = s1_reg0;
        s1_reg1_next       = s1_reg1;
        s2_reg_next        = s2_reg_r;

        if (stage_reset) begin
            state_next     = S_IDLE;
            decode_next    = pe_decode_signals_zero();
            decode_s1_next = pe_decode_signals_zero();
            decode_s2_next = pe_decode_signals_zero();
            halted_next    = 1'b0;
            plo_pending_valid_next = 1'b0;
            s1_reg0_next   = 16'h0000;
            s1_reg1_next   = 16'h0000;
            s2_reg_next    = 16'h0000;
        end else if (pe_running && !halted_reg) begin
            case (state_reg)
                // ---------------------------------------------------------
                S_IDLE: begin
                    if (valid_in && ready_out_w) begin
                        decode_next    = EXE_M_decode_signals_in;
                        vmul_data_next = vmul_out_in;
                        decode_s1_next = pe_decode_signals_zero();
                        decode_s2_next = pe_decode_signals_zero();

                        if (EXE_M_decode_signals_in.halt) begin
                            halted_next = 1'b1;
                            state_next  = S_IDLE;
                        end
                        else if (EXE_M_decode_signals_in.pli_plo_operation) begin
                            if (pli_valid) begin
                                pli_data_next = u64_to_v_fp16(pli_data);
                                state_next    = S_EXEC_PLI_VADDU;
                            end else begin
                                state_next    = S_WAIT_PLI;
                            end
                        end
                        else if (EXE_M_decode_signals_in.vaddu_en
                                 && EXE_M_decode_signals_in.vaddu_mode == 32'd0) begin
                            state_next = S_VMAC_S1;
                        end
                        else if (EXE_M_decode_signals_in.vaddu_en) begin
                            state_next = S_NORMAL_MODE;
                        end
                        else begin
                            state_next = S_IDLE;
                        end
                    end
                end

                // ---------------------------------------------------------
                S_VMAC_S1: begin
                    // Pipeline shift
                    s1_reg0_next   = vaddu_result_sig.lanes[0];
                    s1_reg1_next   = vaddu_result_sig.lanes[1];
                    s2_reg_next    = vaddu_result_sig.lanes[2];
                    decode_s1_next = decode_reg;
                    decode_s2_next = decode_s1_reg;

                    if (valid_in && ready_out_w) begin
                        // Next is VMAC → stay in S1 (pipeline)
                        if (EXE_M_decode_signals_in.vaddu_en
                            && EXE_M_decode_signals_in.vaddu_mode == 32'd0) begin
                            decode_next    = EXE_M_decode_signals_in;
                            vmul_data_next = vmul_out_in;
                            state_next     = S_VMAC_S1;
                        end else begin
                            // Not VMAC → drain pipeline
                            decode_next = pe_decode_signals_zero();
                            state_next  = S_VMAC_S2;
                        end
                    end else begin
                        // No valid input → drain
                        decode_next = pe_decode_signals_zero();
                        state_next  = S_VMAC_S2;
                    end
                end

                // ---------------------------------------------------------
                S_VMAC_S2: begin
                    // Pipeline shift
                    s1_reg0_next   = vaddu_result_sig.lanes[0];
                    s1_reg1_next   = vaddu_result_sig.lanes[1];
                    s2_reg_next    = vaddu_result_sig.lanes[2];
                    decode_s1_next = decode_reg;
                    decode_s2_next = decode_s1_reg;
                    decode_next    = pe_decode_signals_zero();
                    state_next     = S_VMAC_S3;
                end

                // ---------------------------------------------------------
                S_VMAC_S3: begin
                    decode_s2_next = decode_s1_reg;
                    state_next     = S_IDLE;
                end

                // ---------------------------------------------------------
                S_NORMAL_MODE: begin
                    if (valid_in && ready_out_w) begin
                        decode_next    = EXE_M_decode_signals_in;
                        vmul_data_next = vmul_out_in;
                        decode_s1_next = pe_decode_signals_zero();
                        decode_s2_next = pe_decode_signals_zero();

                        if (EXE_M_decode_signals_in.halt) begin
                            halted_next = 1'b1;
                            state_next  = S_IDLE;
                        end
                        else if (EXE_M_decode_signals_in.pli_plo_operation) begin
                            if (pli_valid) begin
                                pli_data_next = u64_to_v_fp16(pli_data);
                                state_next    = S_EXEC_PLI_VADDU;
                            end else begin
                                state_next    = S_WAIT_PLI;
                            end
                        end
                        else if (EXE_M_decode_signals_in.vaddu_en
                                 && EXE_M_decode_signals_in.vaddu_mode == 32'd0)
                            state_next = S_VMAC_S1;
                        else if (EXE_M_decode_signals_in.vaddu_en)
                            state_next = S_NORMAL_MODE;
                        else
                            state_next = S_IDLE;
                    end else begin
                        state_next = S_IDLE;
                    end
                end

                // ---------------------------------------------------------
                S_WAIT_PLI: begin
                    if (pli_valid) begin
                        pli_data_next = u64_to_v_fp16(pli_data);
                        state_next    = S_EXEC_PLI_VADDU;
                    end
                end

                // ---------------------------------------------------------
                S_EXEC_PLI_VADDU: begin
                    vaddu_result_next = vaddu_result_sig;

                    if (!(!plo_buf_valid_reg || plo_ready)) begin
                        // PLO buffer full → wait
                        state_next             = S_WAIT_PLO;
                        plo_pending_valid_next = 1'b1;
                    end else begin
                        if (ready_out_w && valid_in) begin
                            decode_next    = EXE_M_decode_signals_in;
                            vmul_data_next = vmul_out_in;
                            decode_s1_next = pe_decode_signals_zero();
                            decode_s2_next = pe_decode_signals_zero();

                            if (EXE_M_decode_signals_in.halt) begin
                                halted_next = 1'b1;
                                state_next  = S_IDLE;
                            end
                            else if (EXE_M_decode_signals_in.pli_plo_operation) begin
                                if (pli_valid) begin
                                    pli_data_next = u64_to_v_fp16(pli_data);
                                    state_next    = S_EXEC_PLI_VADDU;
                                end else begin
                                    state_next    = S_WAIT_PLI;
                                end
                            end
                            else if (EXE_M_decode_signals_in.vaddu_en
                                     && EXE_M_decode_signals_in.vaddu_mode == 32'd0)
                                state_next = S_VMAC_S1;
                            else if (EXE_M_decode_signals_in.vaddu_en)
                                state_next = S_NORMAL_MODE;
                            else
                                state_next = S_IDLE;
                        end else begin
                            state_next = S_IDLE;
                        end
                    end
                end

                // ---------------------------------------------------------
                S_WAIT_PLO: begin
                    if (plo_buf_valid_reg) begin
                        state_next = S_WAIT_PLO;
                    end else if (plo_pending_valid_reg) begin
                        plo_pending_valid_next = 1'b0;
                        state_next             = S_IDLE;
                    end else begin
                        state_next = S_IDLE;
                    end
                end

            endcase
        end
    end

    // =========================================================================
    // Sequential Process: Register Updates
    // =========================================================================
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state_reg         <= S_IDLE;
            decode_reg        <= pe_decode_signals_zero();
            decode_s1_reg     <= pe_decode_signals_zero();
            decode_s2_reg     <= pe_decode_signals_zero();
            vmul_data_reg     <= '0;
            pli_data_reg      <= '0;
            vaddu_result_reg  <= '0;
            halted_reg        <= 1'b0;
            plo_buf_valid_reg <= 1'b0;
            plo_buf_data_reg  <= 64'd0;
            plo_pending_valid_reg <= 1'b0;
            s1_reg0           <= 16'h0000;
            s1_reg1           <= 16'h0000;
            s2_reg_r          <= 16'h0000;
        end else begin
            state_reg         <= state_next;
            decode_reg        <= decode_next;
            decode_s1_reg     <= decode_s1_next;
            decode_s2_reg     <= decode_s2_next;
            vmul_data_reg     <= vmul_data_next;
            pli_data_reg      <= pli_data_next;
            vaddu_result_reg  <= vaddu_result_next;
            halted_reg        <= halted_next;
            plo_buf_valid_reg <= plo_buf_valid_next;
            plo_buf_data_reg  <= plo_buf_data_next;
            plo_pending_valid_reg <= plo_pending_valid_next;
            s1_reg0           <= s1_reg0_next;
            s1_reg1           <= s1_reg1_next;
            s2_reg_r          <= s2_reg_next;
        end
    end

endmodule
