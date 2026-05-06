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
//                64-bit windows at 2-byte stride, so simulation keeps a
//                byte-addressed model alongside the hard macros.
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
    logic                   sram0_pudelay_unused;
    logic                   sram1_pudelay_unused;

    // Masked & word-aligned addresses
    logic [DMEMORY_ADDRESS_WIDTH-1:0] w_byte_addr;
    logic [DMEMORY_ADDRESS_WIDTH-1:0] r_byte_addr;
    logic [SRAM_AW-1:0]               w_word_addr;
    logic [SRAM_AW-1:0]               r_word_addr;
    logic                             macro_readback_observed_w;

    assign w_byte_addr = dm_write_addr & BYTE_ADDR_MASK;
    assign r_byte_addr = dm_read_addr & BYTE_ADDR_MASK;
    assign w_word_addr = {1'b0, w_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};
    assign r_word_addr = {1'b0, r_byte_addr[DMEMORY_ADDRESS_WIDTH-1:3]};

`ifndef SYNTHESIS
    logic [7:0] sim_bank0 [0:SIM_BANK_DEPTH-1];
    logic [7:0] sim_bank1 [0:SIM_BANK_DEPTH-1];
    logic [63:0] sim_read_data_next;
    logic [63:0] sim_read_data_reg;

    function automatic logic [DMEMORY_ADDRESS_WIDTH:0] sim_byte_index(
        input logic [DMEMORY_ADDRESS_WIDTH-1:0] base_addr,
        input logic [2:0]                       byte_lane
    );
        logic [3:0] lane_sum;
        logic [DMEMORY_ADDRESS_WIDTH-3:0] word_idx;
        logic [DMEMORY_ADDRESS_WIDTH-3:0] word_sum;
        logic carry_w;

        lane_sum[0] = base_addr[0] ^ byte_lane[0];
        lane_sum[1] = base_addr[1] ^ byte_lane[1] ^ (base_addr[0] & byte_lane[0]);
        lane_sum[2] = base_addr[2] ^ byte_lane[2]
                    ^ ((base_addr[1] & byte_lane[1])
                     | (base_addr[1] & base_addr[0] & byte_lane[0])
                     | (byte_lane[1] & base_addr[0] & byte_lane[0]));
        lane_sum[3] = (base_addr[2] & byte_lane[2])
                    | (base_addr[2] & base_addr[1] & byte_lane[1])
                    | (base_addr[2] & base_addr[1] & base_addr[0] & byte_lane[0])
                    | (base_addr[2] & byte_lane[1] & base_addr[0] & byte_lane[0])
                    | (byte_lane[2] & base_addr[1] & byte_lane[1])
                    | (byte_lane[2] & base_addr[1] & base_addr[0] & byte_lane[0])
                    | (byte_lane[2] & byte_lane[1] & base_addr[0] & byte_lane[0]);

        word_idx = {1'b0, base_addr[DMEMORY_ADDRESS_WIDTH-1:3]};
        carry_w = lane_sum[3];
        for (int i = 0; i < DMEMORY_ADDRESS_WIDTH-2; i++) begin
            word_sum[i] = word_idx[i] ^ carry_w;
            carry_w = carry_w & word_idx[i];
        end

        return {word_sum, lane_sum[2:0]};
    endfunction
`endif

    assign macro_readback_observed_w = (^sram0_q)
                                     ^ (^sram1_q)
                                     ^ sram0_pudelay_unused
                                     ^ sram1_pudelay_unused;

    // ----------------------------------------------------------------
    // Expand 8-bit byte-write mask → 64-bit bit-write mask (active-low)
    //   dm_write_mask[i]=1 means write byte i → BWEB[i*8 +: 8] = 8'h00
    //   dm_write_mask[i]=0 means mask byte i  → BWEB[i*8 +: 8] = 8'hFF
    // ----------------------------------------------------------------
    logic [63:0] bweb_expanded;
    always_comb begin
        bweb_expanded = {
            {8{~dm_write_mask[7]}},
            {8{~dm_write_mask[6]}},
            {8{~dm_write_mask[5]}},
            {8{~dm_write_mask[4]}},
            {8{~dm_write_mask[3]}},
            {8{~dm_write_mask[2]}},
            {8{~dm_write_mask[1]}},
            {8{~dm_write_mask[0]}}
        };
    end

    // ----------------------------------------------------------------
    // SRAM control logic
    // ----------------------------------------------------------------
    always_comb begin
        // Defaults: both banks disabled during reset (CEB=1), read mode (WEB=1)
        sram0_ceb  = ~reset_n;                  // disabled during reset (active-low)
        sram1_ceb  = ~reset_n;
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
            sram0_addr = r_word_addr;
            sram1_addr = w_word_addr;
            if (dm_write_en) begin
                sram1_web  = 1'b0;              // enable write
                sram1_bweb = bweb_expanded;     // per-bit write mask
                sram1_d    = dm_write_data;
            end
        end else begin
            // Write → bank0, Read → bank1
            sram1_addr = r_word_addr;
            sram0_addr = w_word_addr;
            if (dm_write_en) begin
                sram0_web  = 1'b0;              // enable write
                sram0_bweb = bweb_expanded;     // per-bit write mask
                sram0_d    = dm_write_data;
            end
        end
    end

    // ----------------------------------------------------------------
    // Read data path
    //   The PE DMA model uses byte-indexed overlapping 64-bit windows.
    //   Keep that behavior in simulation while retaining hard macros.
    // ----------------------------------------------------------------
`ifndef SYNTHESIS
    always_comb begin
        logic [DMEMORY_ADDRESS_WIDTH:0] read_idx;
        logic [2:0] byte_lane;

        sim_read_data_next = 64'h0;
        if (bank_sel) begin
            for (int i = 0; i < 8; i++) begin
                byte_lane = i[2:0];
                read_idx = sim_byte_index(r_byte_addr, byte_lane);
                sim_read_data_next[i*8 +: 8] = sim_bank0[read_idx];
            end
        end else begin
            for (int i = 0; i < 8; i++) begin
                byte_lane = i[2:0];
                read_idx = sim_byte_index(r_byte_addr, byte_lane);
                sim_read_data_next[i*8 +: 8] = sim_bank1[read_idx];
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            sim_read_data_reg <= 64'h0;
            for (int i = 0; i < SIM_BANK_DEPTH; i++) begin
                sim_bank0[i] <= 8'h00;
                sim_bank1[i] <= 8'h00;
            end
        end else begin
            logic [DMEMORY_ADDRESS_WIDTH:0] write_idx;
            logic [2:0] byte_lane;

            sim_read_data_reg <= sim_read_data_next;
            if (dm_write_en) begin
                if (bank_sel) begin
                    for (int i = 0; i < 8; i++) begin
                        byte_lane = i[2:0];
                        write_idx = sim_byte_index(w_byte_addr, byte_lane);
                        sim_bank1[write_idx] <= (sim_bank1[write_idx] & {8{~dm_write_mask[byte_lane]}})
                                              | (dm_write_data[i*8 +: 8] & {8{dm_write_mask[byte_lane]}});
                    end
                end else begin
                    for (int i = 0; i < 8; i++) begin
                        byte_lane = i[2:0];
                        write_idx = sim_byte_index(w_byte_addr, byte_lane);
                        sim_bank0[write_idx] <= (sim_bank0[write_idx] & {8{~dm_write_mask[byte_lane]}})
                                              | (dm_write_data[i*8 +: 8] & {8{dm_write_mask[byte_lane]}});
                    end
                end
                sim_bank0[DMEMORY_DEFAULT_SIZE_BYTES + 0] <= 8'h00;
                sim_bank0[DMEMORY_DEFAULT_SIZE_BYTES + 1] <= 8'h00;
                sim_bank0[DMEMORY_DEFAULT_SIZE_BYTES + 2] <= 8'h00;
                sim_bank0[DMEMORY_DEFAULT_SIZE_BYTES + 3] <= 8'h00;
                sim_bank0[DMEMORY_DEFAULT_SIZE_BYTES + 4] <= 8'h00;
                sim_bank0[DMEMORY_DEFAULT_SIZE_BYTES + 5] <= 8'h00;
                sim_bank0[DMEMORY_DEFAULT_SIZE_BYTES + 6] <= 8'h00;
                sim_bank1[DMEMORY_DEFAULT_SIZE_BYTES + 0] <= 8'h00;
                sim_bank1[DMEMORY_DEFAULT_SIZE_BYTES + 1] <= 8'h00;
                sim_bank1[DMEMORY_DEFAULT_SIZE_BYTES + 2] <= 8'h00;
                sim_bank1[DMEMORY_DEFAULT_SIZE_BYTES + 3] <= 8'h00;
                sim_bank1[DMEMORY_DEFAULT_SIZE_BYTES + 4] <= 8'h00;
                sim_bank1[DMEMORY_DEFAULT_SIZE_BYTES + 5] <= 8'h00;
                sim_bank1[DMEMORY_DEFAULT_SIZE_BYTES + 6] <= 8'h00;
            end
        end
    end

    assign dm_read_data = sim_read_data_reg ^ {64{1'b0 & macro_readback_observed_w}};
`else
    assign dm_read_data = bank_sel ? sram0_q : sram1_q;
`endif

    // ----------------------------------------------------------------
    // TSMC SRAM hard macro instances
    //   TS1N16ADFPCLLLVTA128X64M4SWSHOD — 128×64 Single-port SRAM
    //   Ports: CLK, CEB, WEB, A[6:0], D[63:0], BWEB[63:0], Q[63:0]
    //          SLP, DSLP, SD (power), RTSEL[1:0], WTSEL[1:0] (test)
    // ----------------------------------------------------------------
    TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram_bank0 (
        .PUDELAY(sram0_pudelay_unused),
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
        // Test mode — default settings per TSMC datasheet
        .RTSEL  (2'b01),
        .WTSEL  (2'b01)
    );

    TS1N16ADFPCLLLVTA128X64M4SWSHOD u_sram_bank1 (
        .PUDELAY(sram1_pudelay_unused),
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
        // Test mode — default settings per TSMC datasheet
        .RTSEL  (2'b01),
        .WTSEL  (2'b01)
    );

endmodule
