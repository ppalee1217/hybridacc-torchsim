//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   TransformRegFile
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

module TransformRegFile (
    input  logic       clk,
    input  logic       reset_n,
    input  logic [31:0] enable,
    input  logic       shift_en,
    input  logic [31:0] shift_mode,
    input  logic [31:0] tid,
    input  fp16_t      tid_in,
    input  logic       tid_write_en,
    input  v_fp16_t    vtid_in,
    input  logic       vtid_write_en,
    output v_fp16_t    vtid_out,
    input  logic       clear_regs,
    input  logic       use_vcounter,
    input  logic       clear_vcounter,
    input  logic       incr_vcounter
);
    fp16_t reg_file [0:11];
    logic [31:0] vcounter_reg;

    function automatic logic [11:0] shift_mask(input logic [31:0] mode);
        case (mode)
            32'd0: return 12'b0110_1101_1011;
            32'd1: return 12'b0011_1100_1111;
            32'd2: return 12'b0000_0011_1111;
            default: return 12'b0000_0000_0000;
        endcase
    endfunction

    task automatic clear_all();
        for (int i = 0; i < 12; i++) begin
            reg_file[i] = 16'h0000;
        end
    endtask

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            clear_all();
            vcounter_reg <= 32'd0;
        end else begin
            if (enable != 0) begin
                if (shift_en) begin
                    logic [11:0] mask;
                    mask = shift_mask(shift_mode);
                    for (int i = 0; i < 11; i++) begin
                        if (mask[i]) reg_file[i] <= reg_file[i+1];
                        else reg_file[i] <= 16'h0000;
                    end
                    reg_file[11] <= 16'h0000;
                end else if (tid_write_en) begin
                    if (tid < 12) reg_file[tid] <= tid_in;
                end else if (vtid_write_en) begin
                    if (tid + 9 < 12) begin
                        for (int i = 0; i < 4; i++) begin
                            reg_file[tid + i*3] <= vtid_in.lanes[i];
                        end
                    end
                end else if (clear_regs) begin
                    clear_all();
                end

                if (incr_vcounter) begin
                    vcounter_reg <= vcounter_reg + tid;
                end else if (clear_vcounter) begin
                    vcounter_reg <= 32'd0;
                end
            end
        end
    end

    always_comb begin
        vtid_out = '0;
        if (reset_n && (enable != 0)) begin
            logic [31:0] base;
            base = use_vcounter ? vcounter_reg : tid;
            if (base + 9 < 12) begin
                for (int i = 0; i < 4; i++) begin
                    vtid_out.lanes[i] = reg_file[base + i*3];
                end
            end
        end
    end
endmodule
