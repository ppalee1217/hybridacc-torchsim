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
// Dependencies:  TS1N16ADFPCLLLVTA128X64M4SWSHOD (TSMC 16nm FFC SP SRAM)
// Revision:
//   2026/03/28 - Initial version (behavioral arrays)
//   2026/03/29 - Replaced behavioral arrays with TSMC SRAM hard macro instances
// Additional Comments:
//   SRAM macro: 128 words × 64-bit, 7-bit address, per-bit BWEB[63:0].
//   Addresses must be 8-byte aligned (addr[2:0] == 0).
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
    input  logic [15:0] dm_write_addr,
    input  logic [63:0] dm_write_data,
    input  logic [7:0]  dm_write_mask,
    input  logic [15:0] dm_read_addr,
    output logic [63:0] dm_read_data
);
    // ----------------------------------------------------------------
    // SRAM macro parameters
    //   TS1N16ADFPCLLLVTA128X64M4SWSHOD: 128 words, 64-bit, 7-bit addr
    // ----------------------------------------------------------------
    localparam int unsigned SRAM_DEPTH    = 128;
    localparam int unsigned SRAM_AW       = 7;       // $clog2(128) = 7
    localparam int unsigned ADDR_MASK     = (1 << DMEMORY_ADDRESS_WIDTH) - 1;

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

    // Masked & word-aligned addresses
    logic [15:0] w_byte_addr;
    logic [15:0] r_byte_addr;

    assign w_byte_addr = dm_write_addr & ADDR_MASK;
    assign r_byte_addr = dm_read_addr  & ADDR_MASK;

    // ----------------------------------------------------------------
    // Expand 8-bit byte-write mask → 64-bit bit-write mask (active-low)
    //   dm_write_mask[i]=1 means write byte i → BWEB[i*8 +: 8] = 8'h00
    //   dm_write_mask[i]=0 means mask byte i  → BWEB[i*8 +: 8] = 8'hFF
    // ----------------------------------------------------------------
    logic [63:0] bweb_expanded;
    always_comb begin
        for (int i = 0; i < 8; i++) begin
            bweb_expanded[i*8 +: 8] = dm_write_mask[i] ? 8'h00 : 8'hFF;
        end
    end

    // ----------------------------------------------------------------
    // SRAM control logic
    // ----------------------------------------------------------------
    always_comb begin
        // Defaults: both banks enabled (CEB=0), read mode (WEB=1)
        sram0_ceb  = 1'b0;                     // chip enabled (active-low)
        sram1_ceb  = 1'b0;
        sram0_web  = 1'b1;                     // read mode
        sram1_web  = 1'b1;
        sram0_bweb = {64{1'b1}};               // all bits masked (no write)
        sram1_bweb = {64{1'b1}};
        sram0_d    = 64'h0;
        sram1_d    = 64'h0;
        sram0_addr = {SRAM_AW{1'b0}};
        sram1_addr = {SRAM_AW{1'b0}};

        // bank_sel=0 → write bank0, read bank1
        // bank_sel=1 → write bank1, read bank0
        if (bank_sel) begin
            // Write → bank1, Read → bank0
            sram0_addr = {{(SRAM_AW-DMEMORY_ADDRESS_WIDTH+3){1'b0}}, r_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};
            sram1_addr = {{(SRAM_AW-DMEMORY_ADDRESS_WIDTH+3){1'b0}}, w_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};
            if (dm_write_en) begin
                sram1_web  = 1'b0;              // enable write
                sram1_bweb = bweb_expanded;     // per-bit write mask
                sram1_d    = dm_write_data;
            end
        end else begin
            // Write → bank0, Read → bank1
            sram1_addr = {{(SRAM_AW-DMEMORY_ADDRESS_WIDTH+3){1'b0}}, r_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};
            sram0_addr = {{(SRAM_AW-DMEMORY_ADDRESS_WIDTH+3){1'b0}}, w_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};
            if (dm_write_en) begin
                sram0_web  = 1'b0;              // enable write
                sram0_bweb = bweb_expanded;     // per-bit write mask
                sram0_d    = dm_write_data;
            end
        end
    end

    // ----------------------------------------------------------------
    // Read data mux (SRAM Q is registered, 1-cycle latency)
    // ----------------------------------------------------------------
    assign dm_read_data = bank_sel ? sram0_q : sram1_q;

    // ----------------------------------------------------------------
    // TSMC SRAM hard macro instances
    //   TS1N16ADFPCLLLVTA128X64M4SWSHOD — 128×64 Single-port SRAM
    //   Ports: CLK, CEB, WEB, A[6:0], D[63:0], BWEB[63:0], Q[63:0]
    //          SLP, DSLP, SD (power), RTSEL[1:0], WTSEL[1:0] (test)
    // ----------------------------------------------------------------
    TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram_bank0 (
        .CLK    (clk),
        .CEB    (sram0_ceb),
        .WEB    (sram0_web),
        .A      (sram0_addr),
        .D      (sram0_d),
        .BWEB   (sram0_bweb),
        .Q      (sram0_q),
        // Power management — all inactive (normal operation)
        .SLP    (1'b0),
        .DSLP   (1'b0),
        .SD     (1'b0),
        // Test mode — default settings
        .RTSEL  (2'b01),
        .WTSEL  (2'b00)
    );

    TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram_bank1 (
        .CLK    (clk),
        .CEB    (sram1_ceb),
        .WEB    (sram1_web),
        .A      (sram1_addr),
        .D      (sram1_d),
        .BWEB   (sram1_bweb),
        .Q      (sram1_q),
        // Power management — all inactive (normal operation)
        .SLP    (1'b0),
        .DSLP   (1'b0),
        .SD     (1'b0),
        // Test mode — default settings
        .RTSEL  (2'b01),
        .WTSEL  (2'b00)
    );

    // ----------------------------------------------------------------
    // Alignment assertion (simulation only)
    // ----------------------------------------------------------------
    // synopsys translate_off
    always @(posedge clk) begin
        if (reset_n && dm_write_en) begin
            assert (w_byte_addr[2:0] == 3'b000)
                else $warning("DataMemory: write address 0x%04h is not 8-byte aligned", dm_write_addr);
        end
        if (reset_n) begin
            assert (r_byte_addr[2:0] == 3'b000)
                else $warning("DataMemory: read address 0x%04h is not 8-byte aligned", dm_read_addr);
        end
    end
    // synopsys translate_on

endmodule
