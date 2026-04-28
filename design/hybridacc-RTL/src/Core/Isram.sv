//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   Isram
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Instruction SRAM for CoreController baseline.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module Isram #(
    parameter int unsigned SRAM_BYTES = ISRAM_BYTES
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        mcu_im_valid_i,
    input  logic [31:0] mcu_im_addr_i,
    output logic [31:0] mcu_im_rdata_o,
    input  logic        mcu_dm_valid_i,
    input  logic [31:0] mcu_dm_addr_i,
    output logic [31:0] mcu_dm_rdata_o,
    input  logic        loader_wr_valid_i,
    input  logic [31:0] loader_wr_addr_i,
    input  logic [31:0] loader_wr_data_i,
    input  logic [3:0]  loader_wr_strb_i,
    output logic        loader_wr_ready_o,
    input  logic        load_phase_i
);
    logic [7:0] mem [0:SRAM_BYTES-1];

    assign loader_wr_ready_o = load_phase_i;

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int idx = 0; idx < SRAM_BYTES; idx++) begin
                mem[idx] <= 8'h00;
            end
        end else if (load_phase_i && loader_wr_valid_i) begin
            logic [31:0] base;
            base = loader_wr_addr_i & (SRAM_BYTES - 1);
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if (loader_wr_strb_i[byte_idx] && ((base + byte_idx) < SRAM_BYTES)) begin
                    mem[base + byte_idx] <= loader_wr_data_i[byte_idx*8 +: 8];
                end
            end
        end
    end

    always_comb begin
        mcu_im_rdata_o = 32'h0;
        mcu_dm_rdata_o = 32'h0;
        if (!load_phase_i && mcu_im_valid_i) begin
            logic [31:0] base;
            base = mcu_im_addr_i & (SRAM_BYTES - 1);
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if ((base + byte_idx) < SRAM_BYTES) begin
                    mcu_im_rdata_o[byte_idx*8 +: 8] = mem[base + byte_idx];
                end
            end
        end
        if (!load_phase_i && mcu_dm_valid_i) begin
            logic [31:0] base;
            base = (mcu_dm_addr_i & ~32'h3) & (SRAM_BYTES - 1);
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if ((base + byte_idx) < SRAM_BYTES) begin
                    mcu_dm_rdata_o[byte_idx*8 +: 8] = mem[base + byte_idx];
                end
            end
        end
    end

endmodule