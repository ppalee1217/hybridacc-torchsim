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
    logic [31:0] selected_pid_w;
    logic [31:0] write_pid_w;
    logic [31:0] read_pid_w;
    logic [4:0]  write_scalar_idx_w;
    logic [4:0]  write_vector_base_idx_w;
    logic [4:0]  write_vp64_idx_w;
    logic        write_scalar_valid_w;
    logic        write_vector_valid_w;
    logic        write_vp64_valid_w;
    logic [4:0]  read_scalar_idx_w;
    logic [4:0]  read_vector_base_idx_w;
    logic [4:0]  read_vp64_idx_w;
    logic        read_scalar_valid_w;
    logic        read_vector_valid_w;
    logic        read_vp64_valid_w;

    task automatic clear_all();
        for (int unsigned i = 0; i < 32; i++) begin
            p_regs[i] <= 16'h0000;
        end
        for (int unsigned i = 0; i < 24; i++) begin
            vp64_regs[i] <= '0;
        end
    endtask

    always_comb begin
        selected_pid_w = use_pcounter ? pcounter_reg : pid;
        write_pid_w = selected_pid_w;
        write_scalar_idx_w = 5'd0;
        write_vector_base_idx_w = 5'd0;
        write_vp64_idx_w = 5'd0;
        write_scalar_valid_w = (write_pid_w < 32'd32);
        write_vector_valid_w = (write_pid_w < 32'd8);
        write_vp64_valid_w = (write_pid_w >= 32'd8) && (write_pid_w < 32'd32);
        if (write_scalar_valid_w) begin
            write_scalar_idx_w = write_pid_w[4:0];
        end
        if (write_vector_valid_w) begin
            write_vector_base_idx_w = {write_pid_w[2:0], 2'b00};
        end
        if (write_vp64_valid_w) begin
            write_vp64_idx_w = write_pid_w[4:0] - 5'd8;
        end

        read_pid_w = 32'd0;
        if (enable) begin
            read_pid_w = selected_pid_w;
        end
        read_scalar_idx_w = 5'd0;
        read_vector_base_idx_w = 5'd0;
        read_vp64_idx_w = 5'd0;
        read_scalar_valid_w = (read_pid_w < 32'd32);
        read_vector_valid_w = (read_pid_w < 32'd8);
        read_vp64_valid_w = (read_pid_w >= 32'd8) && (read_pid_w < 32'd32);
        if (read_scalar_valid_w) begin
            read_scalar_idx_w = read_pid_w[4:0];
        end
        if (read_vector_valid_w) begin
            read_vector_base_idx_w = {read_pid_w[2:0], 2'b00};
        end
        if (read_vp64_valid_w) begin
            read_vp64_idx_w = read_pid_w[4:0] - 5'd8;
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            clear_all();
        end else begin
            if (clear_regs) begin
                clear_all();
            end else if (vpid_write_en) begin
                if (mode == 0) begin
                    if (write_scalar_valid_w) begin
                        p_regs[write_scalar_idx_w] <= p_in;
                    end
                end else if (mode == 1) begin
                    if (write_vector_valid_w) begin
                        p_regs[write_vector_base_idx_w + 5'd0] <= vp_in.lanes[0];
                        p_regs[write_vector_base_idx_w + 5'd1] <= vp_in.lanes[1];
                        p_regs[write_vector_base_idx_w + 5'd2] <= vp_in.lanes[2];
                        p_regs[write_vector_base_idx_w + 5'd3] <= vp_in.lanes[3];
                    end else if (write_vp64_valid_w) begin
                        vp64_regs[write_vp64_idx_w] <= vp_in;
                    end
                end
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            pcounter_reg <= 32'd0;
        end else begin
            if (clear_pcounter) begin
                pcounter_reg <= 32'd0;
            end else if (incr_pcounter) begin
                pcounter_reg <= pcounter_reg + pid;
            end
        end
    end

    always_comb begin
        p_out = 16'h0000;
        vp_out = '0;

        if (enable) begin
            if (mode == 0) begin
                if (read_scalar_valid_w) begin
                    p_out = p_regs[read_scalar_idx_w];
                end
            end else if (mode == 1) begin
                if (read_vector_valid_w) begin
                    vp_out.lanes[0] = p_regs[read_vector_base_idx_w + 5'd0];
                    vp_out.lanes[1] = p_regs[read_vector_base_idx_w + 5'd1];
                    vp_out.lanes[2] = p_regs[read_vector_base_idx_w + 5'd2];
                    vp_out.lanes[3] = p_regs[read_vector_base_idx_w + 5'd3];
                end else if (read_vp64_valid_w) begin
                    vp_out = vp64_regs[read_vp64_idx_w];
                end
            end
        end
    end
endmodule
