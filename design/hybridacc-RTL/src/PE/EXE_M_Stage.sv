//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   EXE_M_Stage
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

module EXE_M_Stage (
    input  logic             clk,
    input  logic             reset_n,
    input  logic             stage_reset,
    input  logic             pe_running,
    input  pe_decode_signals_t ID_decode_signals_in,
    input  logic             valid_in,
    output logic             ready_out,
    output v_fp16_t          vmul_out_out,
    output pe_decode_signals_t EXE_A_decode_signals_out,
    output logic             valid_out,
    input  logic             ready_in,
    output logic             halted_out,
    output logic             stall_DL,
    output logic             stall_PS,
    output logic             stall_PD,
    input  logic [63:0]      ps_data,
    input  logic             ps_valid,
    output logic             ps_ready,
    input  logic [15:0]      pd_data,
    input  logic             pd_valid,
    output logic             pd_ready,
    input  logic [63:0]      pd_data_set,
    input  logic             pd_set_valid,
    output logic             pd_set_ready
);
    pe_decode_signals_t decode_reg;
    logic valid_reg;
    logic halted_reg;

    v_fp16_t tr_vtid_out;
    v_fp16_t ldma_dmrv_out;
    v_fp16_t ps_data_vec;
    v_fp16_t vmul_result;

    logic [31:0] tr_enable;
    logic tr_shift_en, tr_tid_write_en, tr_vtid_write_en;
    logic [31:0] tr_shift_mode, tr_tid;
    fp16_t tr_tid_in;
    v_fp16_t tr_vtid_in;
    logic tr_clear_regs, tr_use_vcounter, tr_clear_vcounter, tr_incr_vcounter;

    logic ldma_set_addr, ldma_set_len, ldma_set_loop, ldma_set_mode;
    logic [15:0] ldma_imm, ldma_mode;
    logic ldma_active, ldma_next, ldma_reset_active;
    logic ldma_busy, ldma_done, ldma_stall;
    logic [15:0] dm_read_addr;
    logic [63:0] dm_read_data;

    logic sdma_set_addr, sdma_set_len, sdma_set_loop, sdma_set_mode, sdma_swap;
    logic [15:0] sdma_imm;
    logic sdma_active, sdma_reset_active;
    logic sdma_busy, sdma_done, sdma_stall;
    logic sdma_bank_sel;
    logic sdma_ps_ready;

    logic dm_write_en;
    logic [15:0] dm_write_addr;
    logic [63:0] dm_write_data;
    logic [7:0]  dm_write_mask;

    always_comb begin
        ps_data_vec = u64_to_v_fp16(ps_data);
    end

    TransformRegFile TR (
        .clk(clk), .reset_n(reset_n),
        .enable(tr_enable), .shift_en(tr_shift_en), .shift_mode(tr_shift_mode),
        .tid(tr_tid), .tid_in(tr_tid_in), .tid_write_en(tr_tid_write_en),
        .vtid_in(tr_vtid_in), .vtid_write_en(tr_vtid_write_en), .vtid_out(tr_vtid_out),
        .clear_regs(tr_clear_regs), .use_vcounter(tr_use_vcounter),
        .clear_vcounter(tr_clear_vcounter), .incr_vcounter(tr_incr_vcounter)
    );

    VMULU vmul (
        .op1(tr_vtid_out),
        .op2(ldma_dmrv_out),
        .result(vmul_result)
    );

    LDMA ldma (
        .clk(clk), .reset_n(reset_n),
        .imm(ldma_imm), .set_addr(ldma_set_addr), .set_len(ldma_set_len), .set_loop(ldma_set_loop),
        .set_mode(ldma_set_mode), .mode(ldma_mode), .active(ldma_active), .next(ldma_next),
        .reset_active(ldma_reset_active), .busy(ldma_busy), .done(ldma_done), .dl_stall_out(ldma_stall),
        .dm_read_addr(dm_read_addr), .dm_read_data(dm_read_data), .dmrv_out(ldma_dmrv_out)
    );

    SDMA sdma (
        .clk(clk), .reset_n(reset_n),
        .imm(sdma_imm), .set_addr(sdma_set_addr), .set_len(sdma_set_len), .set_loop(sdma_set_loop),
        .set_mode(sdma_set_mode), .swap_in(sdma_swap), .active(sdma_active), .reset_active(sdma_reset_active),
        .busy(sdma_busy), .done(sdma_done), .dl_stall_out(sdma_stall), .bank_sel(sdma_bank_sel),
        .ps_data(ps_data_vec), .ps_valid(ps_valid), .ps_ready(sdma_ps_ready),
        .dm_write_en(dm_write_en), .dm_write_addr(dm_write_addr), .dm_write_data(dm_write_data), .dm_write_mask(dm_write_mask)
    );

    DataMemory DM (
        .clk(clk), .reset_n(reset_n), .bank_sel(sdma_bank_sel),
        .dm_write_en(dm_write_en), .dm_write_addr(dm_write_addr), .dm_write_data(dm_write_data), .dm_write_mask(dm_write_mask),
        .dm_read_addr(dm_read_addr), .dm_read_data(dm_read_data)
    );

    logic swap_stall;

    always_comb begin
        swap_stall = valid_reg && decode_reg.is_swap && sdma_busy;
        stall_DL = ldma_stall | sdma_stall | swap_stall;
        stall_PS = valid_reg && decode_reg.sys_sdma_act && !ps_valid;
        stall_PD = valid_reg && ((decode_reg.pd_load && !pd_valid) || (decode_reg.pd_load_v && !pd_set_valid));

        ready_out = pe_running && !halted_reg && !stall_DL && !stall_PS && !stall_PD && ready_in;
        valid_out = valid_reg;
        halted_out = halted_reg;

        EXE_A_decode_signals_out = decode_reg;
        vmul_out_out = vmul_result;

        tr_enable = {31'b0, pe_running};
        tr_shift_en = decode_reg.tr_shift && valid_reg && ready_in;
        tr_shift_mode = {29'h0, decode_reg.imm[2:0]};
        tr_tid = decode_reg.rid3;
        tr_tid_in = pd_data;
        tr_tid_write_en = decode_reg.tr_write && decode_reg.pd_load && pd_valid && valid_reg && ready_in;
        tr_vtid_in = decode_reg.pd_load_v ? u64_to_v_fp16(pd_data_set) : ldma_dmrv_out;
        tr_vtid_write_en = decode_reg.tr_write_v && valid_reg && ready_in;
        tr_clear_regs = decode_reg.tr_clear_regs && valid_reg && ready_in;
        tr_use_vcounter = decode_reg.tr_use_vcounter;
        tr_clear_vcounter = decode_reg.sys_rst_tid && valid_reg && ready_in;
        tr_incr_vcounter = decode_reg.tr_incr_vcounter && valid_reg && ready_in;

        ldma_imm = decode_reg.imm;
        ldma_set_addr = decode_reg.DMA_setaddr && !decode_reg.DMA_is_sdma && valid_reg && ready_in;
        ldma_set_len = decode_reg.DMA_setlen && !decode_reg.DMA_is_sdma && valid_reg && ready_in;
        ldma_set_loop = decode_reg.DMA_setloop && !decode_reg.DMA_is_sdma && valid_reg && ready_in;
        ldma_set_mode = decode_reg.DMA_setmode && !decode_reg.DMA_is_sdma && valid_reg && ready_in;
        ldma_mode = decode_reg.func3[15:0];
        ldma_active = decode_reg.sys_ldma_act && valid_reg && ready_in;
        ldma_next = decode_reg.LDMA_next && valid_reg && ready_in;
        ldma_reset_active = decode_reg.sys_ldma_rst && valid_reg && ready_in;

        sdma_imm = decode_reg.imm;
        sdma_set_addr = decode_reg.DMA_setaddr && decode_reg.DMA_is_sdma && valid_reg && ready_in;
        sdma_set_len = decode_reg.DMA_setlen && decode_reg.DMA_is_sdma && valid_reg && ready_in;
        sdma_set_loop = decode_reg.DMA_setloop && decode_reg.DMA_is_sdma && valid_reg && ready_in;
        sdma_set_mode = decode_reg.DMA_setmode && decode_reg.DMA_is_sdma && valid_reg && ready_in;
        sdma_swap = decode_reg.is_swap && valid_reg && ready_out;
        sdma_active = decode_reg.sys_sdma_act && valid_reg && ready_in;
        sdma_reset_active = decode_reg.sys_sdma_rst && valid_reg && ready_in;

        ps_ready = sdma_ps_ready;
        pd_ready = pe_running && valid_reg && decode_reg.pd_load && ready_in;
        pd_set_ready = pe_running && valid_reg && decode_reg.pd_load_v && ready_in;
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            decode_reg <= pe_decode_signals_zero();
            valid_reg <= 1'b0;
            halted_reg <= 1'b0;
        end else if (stage_reset || !pe_running) begin
            decode_reg <= pe_decode_signals_zero();
            valid_reg <= 1'b0;
            halted_reg <= 1'b0;
        end else begin
            if (ready_out) begin
                decode_reg <= ID_decode_signals_in;
                valid_reg <= valid_in;
            end
            if (valid_in && ready_out && ID_decode_signals_in.halt) halted_reg <= 1'b1;
        end
    end
endmodule
