//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   PsumRegFile
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

module PsumRegFile (
    input  logic       clk,
    input  logic       reset_n,
    input  logic       enable,
    input  logic [31:0] pid,
    input  fp16_t      p_in,
    input  v_fp16_t    vp_in,
    input  logic       vpid_write_en,
    input  logic [31:0] mode,
    output fp16_t      p_out,
    output v_fp16_t    vp_out,
    input  logic       clear_regs,
    input  logic       use_pcounter,
    input  logic       clear_pcounter,
    input  logic       incr_pcounter
);
    fp16_t   p_regs   [0:31];
    v_fp16_t vp64_regs[0:23];
    logic [31:0] pcounter_reg;
    logic write_toggle;

    task automatic clear_all();
        for (int i = 0; i < 32; i++) p_regs[i] <= 16'h0000;
        for (int i = 0; i < 24; i++) vp64_regs[i] <= '0;
    endtask

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            clear_all();
            pcounter_reg <= 32'd0;
            write_toggle <= 1'b0;
        end else begin
            if (clear_regs) begin
                clear_all();
            end else if (vpid_write_en) begin
                logic [31:0] write_pid;
                write_toggle <= ~write_toggle;
                write_pid = use_pcounter ? pcounter_reg : pid;
                if (mode == 0) begin
                    if (write_pid < 32) p_regs[write_pid] <= p_in;
                end else if (mode == 1) begin
                    if (write_pid < 8) begin
                        for (int i = 0; i < 4; i++) begin
                            p_regs[write_pid*4 + i] <= vp_in.lanes[i];
                        end
                    end else if (write_pid < 32) begin
                        vp64_regs[write_pid - 8] <= vp_in;
                    end
                end
            end

            if (clear_pcounter) begin
                pcounter_reg <= 32'd0;
            end else if (incr_pcounter) begin
                pcounter_reg <= pcounter_reg + pid;
            end
        end
    end

    always_comb begin
        logic [31:0] read_pid;
        p_out = 16'h0000;
        vp_out = '0;

        if (enable) begin
            read_pid = use_pcounter ? pcounter_reg : pid;
            if (mode == 0) begin
                if (read_pid < 32) p_out = p_regs[read_pid];
            end else if (mode == 1) begin
                if (read_pid < 8) begin
                    for (int i = 0; i < 4; i++) begin
                        vp_out.lanes[i] = p_regs[read_pid*4 + i];
                    end
                end else if (read_pid < 32) begin
                    vp_out = vp64_regs[read_pid - 8];
                end
            end
        end
    end
endmodule
