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
    localparam int unsigned MACRO_DATA_WIDTH = 64;
    localparam int unsigned MACRO_DEPTH      = 128;
    localparam int unsigned MACRO_ADDR_W     = 7;
    localparam int unsigned MACRO_BYTES      = (MACRO_DATA_WIDTH / 8) * MACRO_DEPTH;
    localparam int unsigned NUM_MACROS       = SRAM_BYTES / MACRO_BYTES;
    localparam int unsigned MACRO_SEL_W      = (NUM_MACROS <= 1) ? 1 : $clog2(NUM_MACROS);

    typedef logic [MACRO_DATA_WIDTH-1:0] macro_word_t;
    typedef logic [MACRO_ADDR_W-1:0]     macro_addr_t;
    typedef logic [MACRO_SEL_W-1:0]      macro_sel_t;

    // Simulation shadow keeps the current zero-wait-state CoreMcu contract
    // while the real TSMC hard macros are instantiated and exercised.
    logic [7:0] mem [0:SRAM_BYTES-1];

    logic       if_sram_ceb  [NUM_MACROS];
    logic       if_sram_web  [NUM_MACROS];
    macro_word_t if_sram_bweb[NUM_MACROS];
    macro_addr_t if_sram_addr[NUM_MACROS];
    macro_word_t if_sram_d   [NUM_MACROS];
    macro_word_t if_sram_q   [NUM_MACROS];

    logic       dm_sram_ceb  [NUM_MACROS];
    logic       dm_sram_web  [NUM_MACROS];
    macro_word_t dm_sram_bweb[NUM_MACROS];
    macro_addr_t dm_sram_addr[NUM_MACROS];
    macro_word_t dm_sram_d   [NUM_MACROS];
    macro_word_t dm_sram_q   [NUM_MACROS];

    function automatic logic [31:0] wrap_byte_addr(input logic [31:0] byte_addr);
        return byte_addr & (SRAM_BYTES - 1);
    endfunction

    function automatic macro_sel_t byte_addr_to_macro_sel(input logic [31:0] byte_addr);
        return macro_sel_t'(wrap_byte_addr(byte_addr) / MACRO_BYTES);
    endfunction

    function automatic macro_addr_t byte_addr_to_macro_addr(input logic [31:0] byte_addr);
        return macro_addr_t'((wrap_byte_addr(byte_addr) % MACRO_BYTES) >> 3);
    endfunction

    function automatic macro_word_t word32_to_macro_data(
        input logic [31:0] byte_addr,
        input logic [31:0] word_data
    );
        macro_word_t data_word;
        begin
            data_word = '0;
            if (wrap_byte_addr(byte_addr)[2]) begin
                data_word[63:32] = word_data;
            end else begin
                data_word[31:0] = word_data;
            end
            return data_word;
        end
    endfunction

    function automatic macro_word_t strb32_to_macro_bweb(
        input logic [31:0] byte_addr,
        input logic [3:0]  strb
    );
        macro_word_t bweb_word;
        int lane_lsb;
        begin
            bweb_word = {MACRO_DATA_WIDTH{1'b1}};
            lane_lsb = wrap_byte_addr(byte_addr)[2] ? 32 : 0;
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if (strb[byte_idx]) begin
                    bweb_word[lane_lsb + byte_idx*8 +: 8] = 8'h00;
                end
            end
            return bweb_word;
        end
    endfunction

    assign loader_wr_ready_o = load_phase_i;

    always_comb begin
        macro_sel_t  macro_sel;
        macro_addr_t macro_addr;
        macro_word_t macro_data;
        macro_word_t macro_bweb;

        for (int macro_idx = 0; macro_idx < NUM_MACROS; macro_idx++) begin
            if_sram_ceb[macro_idx]  = 1'b1;
            if_sram_web[macro_idx]  = 1'b1;
            if_sram_bweb[macro_idx] = {MACRO_DATA_WIDTH{1'b1}};
            if_sram_addr[macro_idx] = '0;
            if_sram_d[macro_idx]    = '0;

            dm_sram_ceb[macro_idx]  = 1'b1;
            dm_sram_web[macro_idx]  = 1'b1;
            dm_sram_bweb[macro_idx] = {MACRO_DATA_WIDTH{1'b1}};
            dm_sram_addr[macro_idx] = '0;
            dm_sram_d[macro_idx]    = '0;
        end

        if (load_phase_i && loader_wr_valid_i) begin
            macro_sel  = byte_addr_to_macro_sel(loader_wr_addr_i);
            macro_addr = byte_addr_to_macro_addr(loader_wr_addr_i);
            macro_data = word32_to_macro_data(loader_wr_addr_i, loader_wr_data_i);
            macro_bweb = strb32_to_macro_bweb(loader_wr_addr_i, loader_wr_strb_i);

            if_sram_ceb[macro_sel]  = 1'b0;
            if_sram_web[macro_sel]  = 1'b0;
            if_sram_bweb[macro_sel] = macro_bweb;
            if_sram_addr[macro_sel] = macro_addr;
            if_sram_d[macro_sel]    = macro_data;

            dm_sram_ceb[macro_sel]  = 1'b0;
            dm_sram_web[macro_sel]  = 1'b0;
            dm_sram_bweb[macro_sel] = macro_bweb;
            dm_sram_addr[macro_sel] = macro_addr;
            dm_sram_d[macro_sel]    = macro_data;
        end else begin
            if (mcu_im_valid_i) begin
                macro_sel = byte_addr_to_macro_sel(mcu_im_addr_i);
                if_sram_ceb[macro_sel]  = 1'b0;
                if_sram_addr[macro_sel] = byte_addr_to_macro_addr(mcu_im_addr_i);
            end
            if (mcu_dm_valid_i) begin
                macro_sel = byte_addr_to_macro_sel(mcu_dm_addr_i);
                dm_sram_ceb[macro_sel]  = 1'b0;
                dm_sram_addr[macro_sel] = byte_addr_to_macro_addr(mcu_dm_addr_i);
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int idx = 0; idx < SRAM_BYTES; idx++) begin
                mem[idx] <= 8'h00;
            end
        end else if (load_phase_i && loader_wr_valid_i) begin
            logic [31:0] base;
            base = wrap_byte_addr(loader_wr_addr_i);
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
            base = wrap_byte_addr(mcu_im_addr_i);
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if ((base + byte_idx) < SRAM_BYTES) begin
                    mcu_im_rdata_o[byte_idx*8 +: 8] = mem[base + byte_idx];
                end
            end
        end
        if (!load_phase_i && mcu_dm_valid_i) begin
            logic [31:0] base;
            base = wrap_byte_addr(mcu_dm_addr_i & ~32'h3);
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if ((base + byte_idx) < SRAM_BYTES) begin
                    mcu_dm_rdata_o[byte_idx*8 +: 8] = mem[base + byte_idx];
                end
            end
        end
    end

    generate
        for (genvar macro = 0; macro < NUM_MACROS; macro++) begin : gen_isram_macro
            TS1N16ADFPCLLLVTA128X64M4SWSHOD u_if_sram (
                .SLP    (1'b0),
                .DSLP   (1'b0),
                .SD     (1'b0),
                .PUDELAY(),
                .CLK    (clk),
                .CEB    (if_sram_ceb[macro]),
                .WEB    (if_sram_web[macro]),
                .A      (if_sram_addr[macro]),
                .D      (if_sram_d[macro]),
                .BWEB   (if_sram_bweb[macro]),
                .RTSEL  (2'b01),
                .WTSEL  (2'b01),
                .Q      (if_sram_q[macro])
            );

            TS1N16ADFPCLLLVTA128X64M4SWSHOD u_dm_sram (
                .SLP    (1'b0),
                .DSLP   (1'b0),
                .SD     (1'b0),
                .PUDELAY(),
                .CLK    (clk),
                .CEB    (dm_sram_ceb[macro]),
                .WEB    (dm_sram_web[macro]),
                .A      (dm_sram_addr[macro]),
                .D      (dm_sram_d[macro]),
                .BWEB   (dm_sram_bweb[macro]),
                .RTSEL  (2'b01),
                .WTSEL  (2'b01),
                .Q      (dm_sram_q[macro])
            );
        end
    endgenerate

    initial begin
        if ((SRAM_BYTES % MACRO_BYTES) != 0) begin
            $error("Isram requires SRAM_BYTES to be a multiple of %0d", MACRO_BYTES);
        end
    end

endmodule