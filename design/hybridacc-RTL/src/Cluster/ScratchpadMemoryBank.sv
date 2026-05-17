//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/05/12
// Design Name:   HybridAcc
// Module Name:   ScratchpadMemoryBank
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Bank-level wrapper for ScratchpadMemory.
//                The interface intentionally mirrors an SRAM macro bank so the
//                wrapper can later be replaced cleanly by a memory-compiler
//                generated hard macro or a library binding.
// Dependencies:  TS1N16ADFPCLLLVTA128X64M4SWSHOD
// Revision:
//   2026/05/12 - Split out from ScratchpadMemory.sv
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module ScratchpadMemoryBank #(
    parameter int unsigned BANK_DATA_WIDTH = 64,
    parameter int unsigned BANK_DEPTH      = 8192,
    parameter int unsigned MACRO_DEPTH     = 128,
    parameter int unsigned BANK_ROW_W      = (BANK_DEPTH <= 1) ? 1 : $clog2(BANK_DEPTH)
) (
    input  logic                       SLP,
    input  logic                       DSLP,
    input  logic                       SD,
    output logic                       PUDELAY,
    input  logic                       CLK,
    input  logic                       CEB,
    input  logic                       WEB,
    input  logic [BANK_ROW_W-1:0]      A,
    input  logic [BANK_DATA_WIDTH-1:0] D,
    input  logic [BANK_DATA_WIDTH-1:0] BWEB,
    input  logic [1:0]                 RTSEL,
    input  logic [1:0]                 WTSEL,
    output logic [BANK_DATA_WIDTH-1:0] Q
);
    localparam int unsigned MACRO_ADDR_W    = (MACRO_DEPTH <= 1) ? 1 : $clog2(MACRO_DEPTH);
    localparam int unsigned MACROS_PER_BANK = BANK_DEPTH / MACRO_DEPTH;
    localparam int unsigned MACRO_SEL_W     = (MACROS_PER_BANK <= 1) ? 1 : $clog2(MACROS_PER_BANK);
    localparam int          MACROS_PER_BANK_GEN = int'(MACROS_PER_BANK);

    typedef logic [BANK_DATA_WIDTH-1:0] bank_word_t;
    typedef logic [BANK_ROW_W-1:0]      bank_row_t;
    typedef logic [MACRO_ADDR_W-1:0]    macro_addr_t;
    typedef logic [MACRO_SEL_W-1:0]     macro_sel_t;

    logic        sram_ceb [MACROS_PER_BANK];
    logic        sram_web [MACROS_PER_BANK];
    bank_word_t  sram_bweb[MACROS_PER_BANK];
    macro_addr_t sram_addr[MACROS_PER_BANK];
    bank_word_t  sram_d   [MACROS_PER_BANK];
    bank_word_t  sram_q   [MACROS_PER_BANK];
    logic        sram_pudelay_unused[MACROS_PER_BANK];
    macro_sel_t  read_macro_sel_reg;

    function automatic macro_sel_t row_to_macro_sel(input bank_row_t row_idx);
        return macro_sel_t'(row_idx / MACRO_DEPTH);
    endfunction

    function automatic macro_addr_t row_to_macro_addr(input bank_row_t row_idx);
        return macro_addr_t'(row_idx % MACRO_DEPTH);
    endfunction

    always_comb begin
        PUDELAY = sram_pudelay_unused[0];
        Q       = sram_q[read_macro_sel_reg];
        for (int unsigned macro_idx = 0; macro_idx < MACROS_PER_BANK; macro_idx++) begin
            sram_ceb[macro_idx]  = 1'b1;
            sram_web[macro_idx]  = 1'b1;
            sram_bweb[macro_idx] = {BANK_DATA_WIDTH{1'b1}};
            sram_addr[macro_idx] = '0;
            sram_d[macro_idx]    = '0;
        end

        if (!CEB) begin
            macro_sel_t macro_sel;

            macro_sel = row_to_macro_sel(A);
            sram_ceb[macro_sel]  = 1'b0;
            sram_web[macro_sel]  = WEB;
            sram_addr[macro_sel] = row_to_macro_addr(A);
            if (!WEB) begin
                sram_bweb[macro_sel] = BWEB;
                sram_d[macro_sel]    = D;
            end
        end
    end

    // Match hard-macro behavior: the bank carries no explicit reset state.
    always_ff @(posedge CLK) begin
        if (!CEB) begin
            read_macro_sel_reg <= row_to_macro_sel(A);
        end
    end

    generate
        for (genvar macro = 0; macro < MACROS_PER_BANK_GEN; macro++) begin : gen_sram_macro
            TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram (
                .SLP    (SLP),
                .DSLP   (DSLP),
                .SD     (SD),
                .PUDELAY(sram_pudelay_unused[macro]),
                .CLK    (CLK),
                .CEB    (sram_ceb[macro]),
                .WEB    (sram_web[macro]),
                .A      (sram_addr[macro]),
                .D      (sram_d[macro]),
                .BWEB   (sram_bweb[macro]),
                .RTSEL  (RTSEL),
                .WTSEL  (WTSEL),
                .Q      (sram_q[macro])
            );
        end
    endgenerate

    // synopsys translate_off
    initial begin
        if ((BANK_DEPTH % MACRO_DEPTH) != 0) begin
            $error("ScratchpadMemoryBank requires BANK_DEPTH to be a multiple of %0d", MACRO_DEPTH);
        end
    end
    // synopsys translate_on
endmodule