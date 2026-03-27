// Module: EXE_A_Stage
// Function: Arithmetic execute stage that accumulates vector results and drives PLI/PLO side effects.
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
    pe_decode_signals_t decode_reg;
    v_fp16_t vmul_reg;
    v_fp16_t pli_reg;
    logic valid_reg;
    logic halted_reg;

    v_fp16_t vaddu_result;

    logic pr_enable, pr_vpid_write_en, pr_clear_regs, pr_use_pcounter, pr_clear_pcounter, pr_incr_pcounter;
    logic [31:0] pr_pid, pr_mode;
    fp16_t pr_p_in, pr_p_out;
    v_fp16_t pr_vp_in, pr_vp_out;

    VADDU vaddu (
        .op1(vmul_reg),
        .op2(decode_reg.pli_plo_operation ? pli_reg : pr_vp_out),
        .result(vaddu_result)
    );

    PsumRegFile PR (
        .clk(clk), .reset_n(reset_n),
        .enable(pr_enable), .pid(pr_pid), .p_in(pr_p_in), .vp_in(pr_vp_in),
        .vpid_write_en(pr_vpid_write_en), .mode(pr_mode),
        .p_out(pr_p_out), .vp_out(pr_vp_out),
        .clear_regs(pr_clear_regs), .use_pcounter(pr_use_pcounter), .clear_pcounter(pr_clear_pcounter), .incr_pcounter(pr_incr_pcounter)
    );

    always_comb begin
        stall_port_pli = valid_reg && decode_reg.pli_plo_operation && !pli_valid;
        stall_port_plo = valid_reg && decode_reg.pli_plo_operation && !plo_ready;

        ready_out = pe_running && !halted_reg && (!stall_port_pli) && (!stall_port_plo);

        pli_ready = valid_reg && decode_reg.pli_plo_operation && plo_ready;
        plo_data = v_fp16_to_u64(vaddu_result);
        plo_valid = valid_reg && decode_reg.pli_plo_operation;

        halted_out = halted_reg;

        pr_enable = pe_running;
        pr_pid = decode_reg.rid5;
        pr_mode = {31'b0, decode_reg.pr_mode};
        pr_p_in = vaddu_result.lanes[0];
        pr_vp_in = vaddu_result;
        pr_vpid_write_en = decode_reg.pr_write && valid_reg && ready_out;
        pr_clear_regs = decode_reg.pr_clear_regs && valid_reg && ready_out;
        pr_use_pcounter = decode_reg.pr_use_vcounter;
        pr_clear_pcounter = decode_reg.sys_rst_pid && valid_reg && ready_out;
        pr_incr_pcounter = decode_reg.pr_incr_vcounter && valid_reg && ready_out;
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            decode_reg <= pe_decode_signals_zero();
            vmul_reg <= '0;
            pli_reg <= '0;
            valid_reg <= 1'b0;
            halted_reg <= 1'b0;
        end else if (stage_reset || !pe_running) begin
            decode_reg <= pe_decode_signals_zero();
            vmul_reg <= '0;
            pli_reg <= '0;
            valid_reg <= 1'b0;
            halted_reg <= 1'b0;
        end else begin
            if (ready_out) begin
                decode_reg <= EXE_M_decode_signals_in;
                vmul_reg <= vmul_out_in;
                valid_reg <= valid_in;
                if (pli_valid) pli_reg <= u64_to_v_fp16(pli_data);
            end
            if (valid_in && ready_out && EXE_M_decode_signals_in.halt) halted_reg <= 1'b1;
        end
    end
endmodule
