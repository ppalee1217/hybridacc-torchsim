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
module Isram import core_pkg::*; #(
    parameter int unsigned SRAM_BYTES = ISRAM_BYTES
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        mcu_im_valid_i,
    input  logic [31:0] mcu_im_addr_i,
    output logic        mcu_im_resp_valid_o,
    output logic [31:0] mcu_im_rdata_o,
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
    localparam int          NUM_MACROS_GEN   = int'(NUM_MACROS);

    typedef logic [MACRO_DATA_WIDTH-1:0] macro_word_t;
    typedef logic [MACRO_ADDR_W-1:0]     macro_addr_t;
    typedef logic [MACRO_SEL_W-1:0]      macro_sel_t;

    logic        sram_ceb   [NUM_MACROS];
    logic        sram_web   [NUM_MACROS];
    macro_word_t sram_bweb  [NUM_MACROS];
    macro_addr_t sram_addr  [NUM_MACROS];
    macro_word_t sram_d     [NUM_MACROS];
    macro_word_t sram_q     [NUM_MACROS];
    logic        sram_pudelay_unused [NUM_MACROS];
    logic        sram_pudelay_unused_reduce;

    logic        im_resp_valid_reg;
    logic        im_resp_upper32_reg;
    macro_sel_t  im_resp_macro_sel_reg;

    function automatic logic [31:0] wrap_byte_addr(input logic [31:0] byte_addr);
        return byte_addr & (SRAM_BYTES - 1);
    endfunction

    function automatic macro_sel_t byte_addr_to_macro_sel(input logic [31:0] byte_addr);
        return macro_sel_t'(wrap_byte_addr(byte_addr) / MACRO_BYTES);
    endfunction

    function automatic macro_addr_t byte_addr_to_macro_addr(input logic [31:0] byte_addr);
        return macro_addr_t'((wrap_byte_addr(byte_addr) % MACRO_BYTES) >> 3);
    endfunction

    function automatic logic byte_addr_upper32(input logic [31:0] byte_addr);
        return byte_addr[2];
    endfunction

    function automatic macro_word_t word32_to_macro_data(
        input logic [31:0] byte_addr,
        input logic [31:0] word_data
    );
        macro_word_t data_word;
        begin
            data_word = '0;
            if (byte_addr_upper32(byte_addr)) begin
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
        int unsigned lane_lsb;
        begin
            bweb_word = {MACRO_DATA_WIDTH{1'b1}};
            lane_lsb = byte_addr_upper32(byte_addr) ? 32'd32 : 32'd0;
            for (int unsigned byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if (strb[byte_idx]) begin
                    bweb_word[lane_lsb + byte_idx*8 +: 8] = 8'h00;
                end
            end
            return bweb_word;
        end
    endfunction

    always_comb begin
        sram_pudelay_unused_reduce = 1'b0;
        for (int unsigned macro_idx = 0; macro_idx < NUM_MACROS; macro_idx++) begin
            sram_pudelay_unused_reduce ^= sram_pudelay_unused[macro_idx];
        end
    end

    assign loader_wr_ready_o = load_phase_i
                             && (sram_pudelay_unused_reduce === sram_pudelay_unused_reduce);
    assign mcu_im_resp_valid_o = im_resp_valid_reg;

    always_comb begin
        for (int unsigned macro_idx = 0; macro_idx < NUM_MACROS; macro_idx++) begin
            sram_ceb[macro_idx]  = 1'b1;
            sram_web[macro_idx]  = 1'b1;
            sram_bweb[macro_idx] = {MACRO_DATA_WIDTH{1'b1}};
            sram_addr[macro_idx] = '0;
            sram_d[macro_idx]    = '0;
        end

        if (load_phase_i && loader_wr_valid_i) begin
            sram_ceb[byte_addr_to_macro_sel(loader_wr_addr_i)]  = 1'b0;
            sram_web[byte_addr_to_macro_sel(loader_wr_addr_i)]  = 1'b0;
            sram_bweb[byte_addr_to_macro_sel(loader_wr_addr_i)] = strb32_to_macro_bweb(loader_wr_addr_i, loader_wr_strb_i);
            sram_addr[byte_addr_to_macro_sel(loader_wr_addr_i)] = byte_addr_to_macro_addr(loader_wr_addr_i);
            sram_d[byte_addr_to_macro_sel(loader_wr_addr_i)]    = word32_to_macro_data(loader_wr_addr_i, loader_wr_data_i);
        end else if (mcu_im_valid_i) begin
            sram_ceb[byte_addr_to_macro_sel(mcu_im_addr_i)]  = 1'b0;
            sram_addr[byte_addr_to_macro_sel(mcu_im_addr_i)] = byte_addr_to_macro_addr(mcu_im_addr_i);
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            im_resp_valid_reg     <= 1'b0;
            im_resp_upper32_reg   <= 1'b0;
            im_resp_macro_sel_reg <= '0;
        end else begin
            im_resp_valid_reg <= 1'b0;
            if (!load_phase_i && mcu_im_valid_i) begin
                im_resp_valid_reg     <= 1'b1;
                im_resp_upper32_reg   <= byte_addr_upper32(mcu_im_addr_i);
                im_resp_macro_sel_reg <= byte_addr_to_macro_sel(mcu_im_addr_i);
            end
        end
    end

    always_comb begin
        mcu_im_rdata_o = 32'h0;
        if (im_resp_valid_reg) begin
            mcu_im_rdata_o = im_resp_upper32_reg
                           ? sram_q[im_resp_macro_sel_reg][63:32]
                           : sram_q[im_resp_macro_sel_reg][31:0];
        end
    end

    // Debug logic to track byte-level writes and detect read-before-write scenarios.
    // synopsys translate_off
    bit debug_byte_written [0:SRAM_BYTES-1];

    always_ff @(posedge clk) begin : proc_read_before_write_debug
        logic [31:0] debug_write_base;
        logic [31:0] debug_read_base;
        logic [3:0]  debug_missing_mask;

        debug_write_base   = 32'h0;
        debug_read_base    = 32'h0;
        debug_missing_mask = 4'h0;

        if ((load_phase_i === 1'b1)
            && (loader_wr_valid_i === 1'b1)
            && (loader_wr_addr_i === loader_wr_addr_i)
            && (loader_wr_strb_i === loader_wr_strb_i)) begin
            debug_write_base = wrap_byte_addr(loader_wr_addr_i & ~32'h3);
            for (int unsigned byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if (loader_wr_strb_i[byte_idx]) begin
                    debug_byte_written[wrap_byte_addr(debug_write_base + byte_idx)] <= 1'b1;
                end
            end
        end

        if ((reset_n === 1'b1)
            && (load_phase_i === 1'b0)
            && (mcu_im_valid_i === 1'b1)
            && (mcu_im_addr_i === mcu_im_addr_i)) begin
            debug_read_base = wrap_byte_addr(mcu_im_addr_i & ~32'h3);
            for (int unsigned byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if (!debug_byte_written[wrap_byte_addr(debug_read_base + byte_idx)]) begin
                    debug_missing_mask[byte_idx] = 1'b1;
                end
            end
            if (debug_missing_mask != 4'h0) begin
                $display("[%0t] [WARN][Isram] read-before-write req=0x%08x aligned=0x%08x missing_byte_mask=0x%1x",
                         $time,
                         mcu_im_addr_i,
                         debug_read_base,
                         debug_missing_mask);
            end
        end
    end
    // synopsys translate_on

    generate
        for (genvar macro = 0; macro < NUM_MACROS_GEN; macro++) begin : gen_isram_macro
            TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram (
                .SLP    (1'b0),
                .DSLP   (1'b0),
                .SD     (1'b0),
                .PUDELAY(sram_pudelay_unused[macro]),
                .CLK    (clk),
                .CEB    (sram_ceb[macro]),
                .WEB    (sram_web[macro]),
                .A      (sram_addr[macro]),
                .D      (sram_d[macro]),
                .BWEB   (sram_bweb[macro]),
                .RTSEL  (2'b01),
                .WTSEL  (2'b01),
                .Q      (sram_q[macro])
            );
        end
    endgenerate

    // synopsys translate_off
    initial begin
        if ((SRAM_BYTES % MACRO_BYTES) != 0) begin
            $error("Isram requires SRAM_BYTES to be a multiple of %0d", MACRO_BYTES);
        end
    end
    // synopsys translate_on

endmodule