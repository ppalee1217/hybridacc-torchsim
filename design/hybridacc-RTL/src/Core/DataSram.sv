//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   DataSram
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Data SRAM for CoreController baseline.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module DataSram #(
    parameter int unsigned SRAM_BYTES = DATA_SRAM_BYTES
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        mcu_dm_valid_i,
    input  logic        mcu_dm_write_i,
    input  logic [31:0] mcu_dm_addr_i,
    input  logic [31:0] mcu_dm_wdata_i,
    input  logic [3:0]  mcu_dm_wstrb_i,
    output logic        mcu_dm_resp_valid_o,
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

    logic        sram_ceb  [NUM_MACROS];
    logic        sram_web  [NUM_MACROS];
    macro_word_t sram_bweb [NUM_MACROS];
    macro_addr_t sram_addr [NUM_MACROS];
    macro_word_t sram_d    [NUM_MACROS];
    macro_word_t sram_q    [NUM_MACROS];

    logic        dm_resp_valid_reg;
    logic        dm_resp_upper32_reg;
    macro_sel_t  dm_resp_macro_sel_reg;

`ifdef HACC_SIM_DEBUG_READBACK
    logic [7:0] debug_mem [0:SRAM_BYTES-1];

    function automatic logic [31:0] debug_read_word(input logic [31:0] byte_addr);
        logic [31:0] value;
        logic [31:0] base;
        begin
            value = 32'h0;
            base = wrap_byte_addr(byte_addr & ~32'h3);
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if ((base + byte_idx) < SRAM_BYTES) begin
                    value[byte_idx*8 +: 8] = debug_mem[base + byte_idx];
                end
            end
            return value;
        end
    endfunction
`endif

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
        logic [31:0] wrapped_addr;
        begin
            wrapped_addr = wrap_byte_addr(byte_addr);
            return wrapped_addr[2];
        end
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
        int lane_lsb;
        begin
            bweb_word = {MACRO_DATA_WIDTH{1'b1}};
            lane_lsb = byte_addr_upper32(byte_addr) ? 32 : 0;
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                if (strb[byte_idx]) begin
                    bweb_word[lane_lsb + byte_idx*8 +: 8] = 8'h00;
                end
            end
            return bweb_word;
        end
    endfunction

    assign loader_wr_ready_o = load_phase_i;
    assign mcu_dm_resp_valid_o = dm_resp_valid_reg;

    always_comb begin
        macro_sel_t  macro_sel;
        logic        wr_en;
        logic [31:0] wr_addr;
        logic [31:0] wr_data;
        logic [3:0]  wr_strb;

        for (int macro_idx = 0; macro_idx < NUM_MACROS; macro_idx++) begin
            sram_ceb[macro_idx]  = 1'b1;
            sram_web[macro_idx]  = 1'b1;
            sram_bweb[macro_idx] = {MACRO_DATA_WIDTH{1'b1}};
            sram_addr[macro_idx] = '0;
            sram_d[macro_idx]    = '0;
        end

        wr_en   = 1'b0;
        wr_addr = '0;
        wr_data = '0;
        wr_strb = '0;

        if (load_phase_i && loader_wr_valid_i) begin
            wr_en   = 1'b1;
            wr_addr = loader_wr_addr_i;
            wr_data = loader_wr_data_i;
            wr_strb = loader_wr_strb_i;
        end else if (!load_phase_i && mcu_dm_valid_i && mcu_dm_write_i) begin
            wr_en   = 1'b1;
            wr_addr = mcu_dm_addr_i;
            wr_data = mcu_dm_wdata_i;
            wr_strb = mcu_dm_wstrb_i;
        end

        if (wr_en) begin
            macro_sel = byte_addr_to_macro_sel(wr_addr);
            sram_ceb[macro_sel]  = 1'b0;
            sram_web[macro_sel]  = 1'b0;
            sram_bweb[macro_sel] = strb32_to_macro_bweb(wr_addr, wr_strb);
            sram_addr[macro_sel] = byte_addr_to_macro_addr(wr_addr);
            sram_d[macro_sel]    = word32_to_macro_data(wr_addr, wr_data);
        end else if (!load_phase_i && mcu_dm_valid_i && !mcu_dm_write_i) begin
            macro_sel = byte_addr_to_macro_sel(mcu_dm_addr_i);
            sram_ceb[macro_sel]  = 1'b0;
            sram_addr[macro_sel] = byte_addr_to_macro_addr(mcu_dm_addr_i);
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            dm_resp_valid_reg     <= 1'b0;
            dm_resp_upper32_reg   <= 1'b0;
            dm_resp_macro_sel_reg <= '0;
`ifdef HACC_SIM_DEBUG_READBACK
            for (int idx = 0; idx < SRAM_BYTES; idx++) begin
                debug_mem[idx] <= 8'h00;
            end
`endif
        end else begin
            logic        wr_en;
            logic [31:0] wr_addr;
            logic [31:0] wr_data;
            logic [3:0]  wr_strb;
`ifdef HACC_SIM_DEBUG_READBACK
            logic [31:0] debug_base;
`endif

            dm_resp_valid_reg <= 1'b0;
            wr_en   = 1'b0;
            wr_addr = 32'h0;
            wr_data = 32'h0;
            wr_strb = 4'h0;
`ifdef HACC_SIM_DEBUG_READBACK
            debug_base = 32'h0;
`endif

            if (load_phase_i && loader_wr_valid_i) begin
                wr_en   = 1'b1;
                wr_addr = loader_wr_addr_i;
                wr_data = loader_wr_data_i;
                wr_strb = loader_wr_strb_i;
            end else if (!load_phase_i && mcu_dm_valid_i && mcu_dm_write_i) begin
                wr_en   = 1'b1;
                wr_addr = mcu_dm_addr_i;
                wr_data = mcu_dm_wdata_i;
                wr_strb = mcu_dm_wstrb_i;
            end else if (!load_phase_i && mcu_dm_valid_i && !mcu_dm_write_i) begin
                dm_resp_valid_reg     <= 1'b1;
                dm_resp_upper32_reg   <= byte_addr_upper32(mcu_dm_addr_i);
                dm_resp_macro_sel_reg <= byte_addr_to_macro_sel(mcu_dm_addr_i);
            end

            if (wr_en) begin
`ifdef HACC_SIM_DEBUG_READBACK
                debug_base = wrap_byte_addr(wr_addr & ~32'h3);
                for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                    if (wr_strb[byte_idx] && ((debug_base + byte_idx) < SRAM_BYTES)) begin
                        debug_mem[debug_base + byte_idx] <= wr_data[byte_idx*8 +: 8];
                    end
                end
`endif
            end
        end
    end

    always_comb begin
        mcu_dm_rdata_o = 32'h0;
        if (dm_resp_valid_reg) begin
            mcu_dm_rdata_o = dm_resp_upper32_reg
                           ? sram_q[dm_resp_macro_sel_reg][63:32]
                           : sram_q[dm_resp_macro_sel_reg][31:0];
        end
    end

    generate
        for (genvar macro = 0; macro < NUM_MACROS; macro++) begin : gen_dsram_macro
            TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram (
                .SLP    (1'b0),
                .DSLP   (1'b0),
                .SD     (1'b0),
                .PUDELAY(),
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

    initial begin
        if ((SRAM_BYTES % MACRO_BYTES) != 0) begin
            $error("DataSram requires SRAM_BYTES to be a multiple of %0d", MACRO_BYTES);
        end
    end

endmodule