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
    logic [31:0] vtid_base_w;
    logic [3:0] tid_idx_w;
    logic [3:0] vtid_write_base_idx_w;
    logic [3:0] vtid_read_base_idx_w;
    logic       tid_write_valid_w;
    logic       vtid_write_valid_w;
    logic       vtid_read_valid_w;

    task automatic clear_all();
        for (int unsigned i = 0; i < 12; i++) begin
            reg_file[i] <= 16'h0000;
        end
    endtask

    always_comb begin
        tid_idx_w = 4'd0;
        vtid_write_base_idx_w = 4'd0;
        vtid_read_base_idx_w = 4'd0;
        vtid_base_w  = use_vcounter ? vcounter_reg : tid;
        tid_write_valid_w = (tid < 32'd12);
        vtid_write_valid_w = (tid < 32'd3);
        vtid_read_valid_w = (vtid_base_w < 32'd3);
        if (tid_write_valid_w) begin
            tid_idx_w = tid[3:0];
        end
        if (vtid_write_valid_w) begin
            vtid_write_base_idx_w = tid[3:0];
        end
        if (vtid_read_valid_w) begin
            vtid_read_base_idx_w = vtid_base_w[3:0];
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            clear_all();
        end else if (enable != 0) begin
            if (shift_en) begin
                case (shift_mode)
                    32'd0: begin
                        reg_file[0]  <= reg_file[1];
                        reg_file[1]  <= reg_file[2];
                        reg_file[2]  <= 16'h0000;
                        reg_file[3]  <= reg_file[4];
                        reg_file[4]  <= reg_file[5];
                        reg_file[5]  <= 16'h0000;
                        reg_file[6]  <= reg_file[7];
                        reg_file[7]  <= reg_file[8];
                        reg_file[8]  <= 16'h0000;
                        reg_file[9]  <= reg_file[10];
                        reg_file[10] <= reg_file[11];
                        reg_file[11] <= 16'h0000;
                    end
                    32'd1: begin
                        reg_file[0]  <= reg_file[1];
                        reg_file[1]  <= reg_file[2];
                        reg_file[2]  <= reg_file[3];
                        reg_file[3]  <= reg_file[4];
                        reg_file[4]  <= 16'h0000;
                        reg_file[5]  <= 16'h0000;
                        reg_file[6]  <= reg_file[7];
                        reg_file[7]  <= reg_file[8];
                        reg_file[8]  <= reg_file[9];
                        reg_file[9]  <= reg_file[10];
                        reg_file[10] <= 16'h0000;
                        reg_file[11] <= 16'h0000;
                    end
                    32'd2: begin
                        reg_file[0]  <= reg_file[1];
                        reg_file[1]  <= reg_file[2];
                        reg_file[2]  <= reg_file[3];
                        reg_file[3]  <= reg_file[4];
                        reg_file[4]  <= reg_file[5];
                        reg_file[5]  <= reg_file[6];
                        reg_file[6]  <= 16'h0000;
                        reg_file[7]  <= 16'h0000;
                        reg_file[8]  <= 16'h0000;
                        reg_file[9]  <= 16'h0000;
                        reg_file[10] <= 16'h0000;
                        reg_file[11] <= 16'h0000;
                    end
                    default: begin
                        clear_all();
                    end
                endcase
            end else if (tid_write_en) begin
                if (tid_write_valid_w) begin
                    reg_file[tid_idx_w] <= tid_in;
                end
            end else if (vtid_write_en) begin
                if (vtid_write_valid_w) begin
                    case (vtid_write_base_idx_w)
                        4'd0: begin
                            reg_file[0] <= vtid_in.lanes[0];
                            reg_file[3] <= vtid_in.lanes[1];
                            reg_file[6] <= vtid_in.lanes[2];
                            reg_file[9] <= vtid_in.lanes[3];
                        end
                        4'd1: begin
                            reg_file[1]  <= vtid_in.lanes[0];
                            reg_file[4]  <= vtid_in.lanes[1];
                            reg_file[7]  <= vtid_in.lanes[2];
                            reg_file[10] <= vtid_in.lanes[3];
                        end
                        4'd2: begin
                            reg_file[2]  <= vtid_in.lanes[0];
                            reg_file[5]  <= vtid_in.lanes[1];
                            reg_file[8]  <= vtid_in.lanes[2];
                            reg_file[11] <= vtid_in.lanes[3];
                        end
                        default: begin
                        end
                    endcase
                end
            end else if (clear_regs) begin
                clear_all();
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            vcounter_reg <= 32'd0;
        end else if (enable != 0) begin
            if (incr_vcounter) begin
                vcounter_reg <= vcounter_reg + tid;
            end else if (clear_vcounter) begin
                vcounter_reg <= 32'd0;
            end
        end
    end

    always_comb begin
        vtid_out = '0;
        if (enable != 0) begin
            if (vtid_read_valid_w) begin
                case (vtid_read_base_idx_w)
                    4'd0: begin
                        vtid_out.lanes[0] = reg_file[0];
                        vtid_out.lanes[1] = reg_file[3];
                        vtid_out.lanes[2] = reg_file[6];
                        vtid_out.lanes[3] = reg_file[9];
                    end
                    4'd1: begin
                        vtid_out.lanes[0] = reg_file[1];
                        vtid_out.lanes[1] = reg_file[4];
                        vtid_out.lanes[2] = reg_file[7];
                        vtid_out.lanes[3] = reg_file[10];
                    end
                    4'd2: begin
                        vtid_out.lanes[0] = reg_file[2];
                        vtid_out.lanes[1] = reg_file[5];
                        vtid_out.lanes[2] = reg_file[8];
                        vtid_out.lanes[3] = reg_file[11];
                    end
                    default: begin
                    end
                endcase
            end
        end
    end
endmodule
