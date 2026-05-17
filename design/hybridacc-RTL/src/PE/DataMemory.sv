//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   DataMemory
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Dual-bank data memory using TSMC SRAM hard macros.
//                Each bank: TS1N16ADFPCLLLVTA128X64M4SWSHOD (128×64-bit SP SRAM).
//                bank_sel controls which bank is written vs. read:
//                  bank_sel=0 → write bank0, read bank1
//                  bank_sel=1 → write bank1, read bank0
//                PE DMA addresses are byte offsets and may issue overlapping
//                64-bit windows at 2-byte stride, so pre-sim relies on the
//                shared SRAM behavioral model to preserve byte-window reads.
// Dependencies:  TS1N16ADFPCLLLVTA128X64M4SWSHOD (TSMC 16nm FFC SP SRAM)
// Revision:
//   2026/03/28 - Initial version (behavioral arrays)
//   2026/03/29 - Replaced behavioral arrays with TSMC SRAM hard macro instances
// Additional Comments:
//   SRAM macro: 128 words × 64-bit, 7-bit address, per-bit BWEB[63:0].
//   Power pins (SLP/DSLP/SD) tied inactive; test pins (RTSEL/WTSEL) set default.
//-----------------------------------------------------------------------------
module DataMemory #(
    parameter int unsigned DMEMORY_ADDRESS_WIDTH = 9,
    parameter int unsigned DMEMORY_DEFAULT_SIZE_BYTES = (1 << DMEMORY_ADDRESS_WIDTH)
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        bank_sel,
    input  logic        dm_write_en,
    input  logic [DMEMORY_ADDRESS_WIDTH-1:0] dm_write_addr,
    input  logic [63:0] dm_write_data,
    input  logic [7:0]  dm_write_mask,
    input  logic [DMEMORY_ADDRESS_WIDTH-1:0] dm_read_addr,
    output logic [63:0] dm_read_data
);
    // ----------------------------------------------------------------
    // SRAM macro parameters
    //   TS1N16ADFPCLLLVTA128X64M4SWSHOD: 128 words, 64-bit, 7-bit addr
    // ----------------------------------------------------------------
    localparam int unsigned SRAM_DEPTH    = 128;
    localparam int unsigned SRAM_AW       = 7;       // $clog2(128) = 7
    localparam logic [DMEMORY_ADDRESS_WIDTH-1:0] BYTE_ADDR_MASK = DMEMORY_DEFAULT_SIZE_BYTES - 1;
    localparam int unsigned SIM_BANK_DEPTH = (1 << (DMEMORY_ADDRESS_WIDTH + 1));

    // ----------------------------------------------------------------
    // Internal signals
    // ----------------------------------------------------------------
    // Bank 0 SRAM ports
    logic                   sram0_ceb;
    logic                   sram0_web;
    logic [63:0]            sram0_bweb;
    logic [SRAM_AW-1:0]    sram0_addr;
    logic [63:0]            sram0_d;
    logic [63:0]            sram0_q;

    // Bank 1 SRAM ports
    logic                   sram1_ceb;
    logic                   sram1_web;
    logic [63:0]            sram1_bweb;
    logic [SRAM_AW-1:0]    sram1_addr;
    logic [63:0]            sram1_d;
    logic [63:0]            sram1_q;
    logic                   sram0_slp;
    logic                   sram1_slp;
    logic                   sram0_dslp;
    logic                   sram1_dslp;
    logic                   sram0_sd;
    logic                   sram1_sd;
    logic [1:0]             sram0_rtsel;
    logic [1:0]             sram1_rtsel;
    logic [1:0]             sram0_wtsel;
    logic [1:0]             sram1_wtsel;

    // Masked & word-aligned addresses
    logic [DMEMORY_ADDRESS_WIDTH-1:0] w_byte_addr;
    logic [DMEMORY_ADDRESS_WIDTH-1:0] r_byte_addr;
    logic [SRAM_AW-1:0]               w_word_addr;
    logic [SRAM_AW-1:0]               r_word_addr;

    assign w_byte_addr = dm_write_addr & BYTE_ADDR_MASK;
    assign r_byte_addr = dm_read_addr & BYTE_ADDR_MASK;
    assign w_word_addr = {1'b0, w_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};
    assign r_word_addr = {1'b0, r_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};

    // ----------------------------------------------------------------
    // Expand 8-bit byte-write mask → 64-bit bit-write mask (active-low)
    //   dm_write_mask[i]=1 means write byte i → BWEB[i*8 +: 8] = 8'h00
    //   dm_write_mask[i]=0 means mask byte i  → BWEB[i*8 +: 8] = 8'hFF
    // ----------------------------------------------------------------
    logic [63:0] bweb_expanded;
    assign bweb_expanded = {
        {8{~dm_write_mask[7]}},
        {8{~dm_write_mask[6]}},
        {8{~dm_write_mask[5]}},
        {8{~dm_write_mask[4]}},
        {8{~dm_write_mask[3]}},
        {8{~dm_write_mask[2]}},
        {8{~dm_write_mask[1]}},
        {8{~dm_write_mask[0]}}
    };

    // ----------------------------------------------------------------
    // SRAM control logic
    // ----------------------------------------------------------------
    always_comb begin
        sram0_slp  = 1'b0;
        sram1_slp  = 1'b0;
        sram0_dslp = 1'b0;
        sram1_dslp = 1'b0;
        sram0_sd   = 1'b0;
        sram1_sd   = 1'b0;
        sram0_rtsel = 2'b01;
        sram1_rtsel = 2'b01;
        sram0_wtsel = 2'b01;
        sram1_wtsel = 2'b01;
        sram0_ceb  = 1'b0;
        sram1_ceb  = 1'b0;
        sram0_web  = 1'b1;
        sram1_web  = 1'b1;
        sram0_bweb = {64{1'b1}};
        sram1_bweb = {64{1'b1}};
        sram0_addr = r_word_addr;
        sram1_addr = r_word_addr;
        sram0_d    = '0;
        sram1_d    = '0;

`ifdef TSMC_SRAM_BEHAV_MODEL
        sram0_slp   = 1'b1;
        sram1_slp   = 1'b1;
        sram0_sd    = 1'b1;
        sram1_sd    = 1'b1;
        sram0_rtsel = r_byte_addr[1:0];
        sram1_rtsel = r_byte_addr[1:0];
        sram0_wtsel = {1'b0, r_byte_addr[2]};
        sram1_wtsel = {1'b0, r_byte_addr[2]};
`endif

        if (dm_write_en) begin
            if (bank_sel) begin
                sram1_web  = 1'b0;
                sram1_bweb = bweb_expanded;
                sram1_addr = w_word_addr;
                sram1_d    = dm_write_data;
`ifdef TSMC_SRAM_BEHAV_MODEL
                sram1_rtsel = w_byte_addr[1:0];
                sram1_wtsel = {1'b0, w_byte_addr[2]};
`endif
            end else begin
                sram0_web  = 1'b0;
                sram0_bweb = bweb_expanded;
                sram0_addr = w_word_addr;
                sram0_d    = dm_write_data;
`ifdef TSMC_SRAM_BEHAV_MODEL
                sram0_rtsel = w_byte_addr[1:0];
                sram0_wtsel = {1'b0, w_byte_addr[2]};
`endif
            end
        end
    end

    // ----------------------------------------------------------------
    // Read data path
    //   Read data comes directly from the shared SRAM behavioral model.
    // ----------------------------------------------------------------
    assign dm_read_data = bank_sel ? sram0_q : sram1_q;

    // ----------------------------------------------------------------
    // TSMC SRAM hard macro instances
    //   TS1N16ADFPCLLLVTA128X64M4SWSHOD — 128×64 Single-port SRAM
    //   Ports: CLK, CEB, WEB, A[6:0], D[63:0], BWEB[63:0], Q[63:0]
    //          SLP, DSLP, SD (power), RTSEL[1:0], WTSEL[1:0] (test)
    // ----------------------------------------------------------------
    TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram_bank0 (
        .PUDELAY(),
        .CLK    (clk),
        .CEB    (sram0_ceb),
        .WEB    (sram0_web),
        .A      (sram0_addr),
        .D      (sram0_d),
        .BWEB   (sram0_bweb),
        .Q      (sram0_q),
        // Power management — all inactive (normal operation)
        .SLP    (sram0_slp),
        .DSLP   (sram0_dslp),
        .SD     (sram0_sd),
        // Test mode — default settings per TSMC datasheet
        .RTSEL  (sram0_rtsel),
        .WTSEL  (sram0_wtsel)
    );

    TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram_bank1 (
        .PUDELAY(),
        .CLK    (clk),
        .CEB    (sram1_ceb),
        .WEB    (sram1_web),
        .A      (sram1_addr),
        .D      (sram1_d),
        .BWEB   (sram1_bweb),
        .Q      (sram1_q),
        // Power management — all inactive (normal operation)
        .SLP    (sram1_slp),
        .DSLP   (sram1_dslp),
        .SD     (sram1_sd),
        // Test mode — default settings per TSMC datasheet
        .RTSEL  (sram1_rtsel),
        .WTSEL  (sram1_wtsel)
    );

endmodule
