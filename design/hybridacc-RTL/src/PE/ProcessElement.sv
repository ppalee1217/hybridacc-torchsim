//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   ProcessElement
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

module ProcessElement #(
    parameter int unsigned PE_FIFO_DEPTH = 4
) (
    input  logic       clk,
    input  logic       reset_n,
    input  logic       router_enable,
    input  PERouterMode router_mode,

    input  noc_request_t noc_ps_in_data,
    input  logic         noc_ps_in_valid,
    output logic         noc_ps_in_ready,

    input  noc_request_t noc_pd_in_data,
    input  logic         noc_pd_in_valid,
    output logic         noc_pd_in_ready,

    input  noc_request_t noc_pli_in_data,
    input  logic         noc_pli_in_valid,
    output logic         noc_pli_in_ready,

    input  noc_addr_req_t noc_plo_in_data,
    input  logic          noc_plo_in_valid,
    output logic          noc_plo_in_ready,

    output noc_response_t noc_plo_out_data,
    output logic          noc_plo_out_valid,
    input  logic          noc_plo_out_ready,

    output logic          pe_busy,

    input  logic [63:0] ln_pli_data,
    input  logic        ln_pli_valid,
    output logic        ln_pli_ready,
    output logic [63:0] ln_plo_data,
    output logic        ln_plo_valid,
    input  logic        ln_plo_ready
);
    logic router_pe_reset, router_pe_start, router_pe_program;
    logic router_im_write_en;
    logic [15:0] router_im_write_addr;
    pe_inst_t router_im_write_data;

    logic [63:0] ps_data, pd_set_data, pli_data, plo_data;
    logic [15:0] pd_data;
    logic ps_valid, pd_valid, pd_set_valid, pli_valid, plo_valid;
    logic ps_ready, pd_ready, pd_set_ready, pli_ready, plo_ready;

    logic pe_running_reg, pe_running_next;
    logic stage_reset_reg, stage_reset_next;

    pe_decode_signals_t if_id_to_exe_m_signals;
    logic if_id_to_exe_m_valid;
    logic exe_m_to_if_id_ready;
    v_fp16_t exe_m_to_exe_a_vmul;
    pe_decode_signals_t exe_m_to_exe_a_signals;
    logic exe_m_to_exe_a_valid;
    logic exe_a_to_exe_m_ready;

    logic [15:0] if_id_pc_sig;
    logic if_id_halted_sig, exe_m_halted_sig, exe_a_halted_sig;
    logic exe_m_stall_dl, exe_m_stall_ps, exe_m_stall_pd;
    logic exe_a_stall_port_pli, exe_a_stall_port_plo;

    PErouter #(.PE_FIFO_DEPTH(PE_FIFO_DEPTH)) router (
        .clk(clk), .reset_n(reset_n), .enable(router_enable), .route_mode(router_mode),
        .noc_ps_req_data(noc_ps_in_data), .noc_ps_req_valid(noc_ps_in_valid), .noc_ps_req_ready(noc_ps_in_ready),
        .noc_pd_req_data(noc_pd_in_data), .noc_pd_req_valid(noc_pd_in_valid), .noc_pd_req_ready(noc_pd_in_ready),
        .noc_pli_req_data(noc_pli_in_data), .noc_pli_req_valid(noc_pli_in_valid), .noc_pli_req_ready(noc_pli_in_ready),
        .noc_plo_req_data(noc_plo_in_data), .noc_plo_req_valid(noc_plo_in_valid), .noc_plo_req_ready(noc_plo_in_ready),
        .noc_plo_resp_data(noc_plo_out_data), .noc_plo_resp_valid(noc_plo_out_valid), .noc_plo_resp_ready(noc_plo_out_ready),
        .ln_pli_data(ln_pli_data), .ln_pli_valid(ln_pli_valid), .ln_pli_ready(ln_pli_ready),
        .ln_plo_data(ln_plo_data), .ln_plo_valid(ln_plo_valid), .ln_plo_ready(ln_plo_ready),
        .pe_reset(router_pe_reset), .pe_start(router_pe_start), .pe_program(router_pe_program),
        .im_write_en(router_im_write_en), .im_write_addr(router_im_write_addr), .im_write_data(router_im_write_data),
        .pe_ps_data(ps_data), .pe_ps_valid(ps_valid), .pe_ps_ready(ps_ready),
        .pe_pd_data(pd_data), .pe_pd_valid(pd_valid), .pe_pd_ready(pd_ready),
        .pe_pd_set_data(pd_set_data), .pe_pd_set_valid(pd_set_valid), .pe_pd_set_ready(pd_set_ready),
        .pe_pli_data(pli_data), .pe_pli_valid(pli_valid), .pe_pli_ready(pli_ready),
        .pe_plo_data(plo_data), .pe_plo_valid(plo_valid), .pe_plo_ready(plo_ready)
    );

    IF_ID_Stage if_id_stage (
        .clk(clk), .reset_n(reset_n), .ID_decode_signals_out(if_id_to_exe_m_signals), .valid_out(if_id_to_exe_m_valid),
        .pc_out(if_id_pc_sig), .halted_out(if_id_halted_sig),
        .stage_reset(stage_reset_reg), .pe_running(pe_running_reg), .ready_in(exe_m_to_if_id_ready),
        .pc_init_value(16'h0), .im_write_en(router_im_write_en), .im_write_addr(router_im_write_addr), .im_write_data(router_im_write_data)
    );

    EXE_M_Stage exe_m_stage (
        .clk(clk), .reset_n(reset_n), .stage_reset(stage_reset_reg), .pe_running(pe_running_reg),
        .ID_decode_signals_in(if_id_to_exe_m_signals), .valid_in(if_id_to_exe_m_valid),
        .ready_out(exe_m_to_if_id_ready), .vmul_out_out(exe_m_to_exe_a_vmul), .EXE_A_decode_signals_out(exe_m_to_exe_a_signals),
        .valid_out(exe_m_to_exe_a_valid), .ready_in(exe_a_to_exe_m_ready), .halted_out(exe_m_halted_sig),
        .stall_DL(exe_m_stall_dl), .stall_PS(exe_m_stall_ps), .stall_PD(exe_m_stall_pd),
        .ps_data(ps_data), .ps_valid(ps_valid), .ps_ready(ps_ready),
        .pd_data(pd_data), .pd_valid(pd_valid), .pd_ready(pd_ready),
        .pd_data_set(pd_set_data), .pd_set_valid(pd_set_valid), .pd_set_ready(pd_set_ready)
    );

    EXE_A_Stage exe_a_stage (
        .clk(clk), .reset_n(reset_n), .stage_reset(stage_reset_reg), .pe_running(pe_running_reg),
        .vmul_out_in(exe_m_to_exe_a_vmul), .EXE_M_decode_signals_in(exe_m_to_exe_a_signals), .valid_in(exe_m_to_exe_a_valid),
        .ready_out(exe_a_to_exe_m_ready), .halted_out(exe_a_halted_sig),
        .stall_port_pli(exe_a_stall_port_pli), .stall_port_plo(exe_a_stall_port_plo),
        .pli_data(pli_data), .pli_valid(pli_valid), .pli_ready(pli_ready),
        .plo_data(plo_data), .plo_valid(plo_valid), .plo_ready(plo_ready)
    );

    always_comb begin
        pe_running_next = pe_running_reg;
        stage_reset_next = 1'b0;

        if (router_pe_reset) stage_reset_next = 1'b1;
        if (router_pe_start && !pe_running_reg) begin
            stage_reset_next = 1'b1;
            pe_running_next = 1'b1;
        end
        if (router_pe_program && pe_running_reg) pe_running_next = 1'b0;

        if (pe_running_reg && if_id_halted_sig && exe_m_halted_sig && exe_a_halted_sig)
            pe_running_next = 1'b0;

        pe_busy = pe_running_reg;
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            pe_running_reg <= 1'b0;
            stage_reset_reg <= 1'b0;
        end else begin
            pe_running_reg <= pe_running_next;
            stage_reset_reg <= stage_reset_next;
        end
    end
endmodule
