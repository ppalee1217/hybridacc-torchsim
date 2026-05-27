//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/29
// Design Name:   HybridAcc
// Module Name:   TS1N16ADFPCLLLVTA128X64M4SWSHOD (behavioral model)
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Shared behavioral simulation model for
//                TSMC TS1N16ADFPCLLLVTA128X64M4SWSHOD.
//                For pre-sim: compile or include this file when the foundry
//                  Verilog model (N16ADFP_SRAM_100a.v) is not available.
//                For synthesis / gate sim: use the foundry .db / .v model
//                  instead; this file is skipped by TSMC_SRAM_MODEL_LOADED.
// Dependencies:  None
// Revision:
//   2026/03/29 - Initial version
//   2026/04/29 - Moved from src/PE to src/utils for shared SRAM use
// Additional Comments:
//   Port-compatible with the TSMC foundry model. Power/test pins are accepted
//   but ignored in this behavioral model.
//-----------------------------------------------------------------------------

`ifndef TSMC_SRAM_MODEL_LOADED
`define TSMC_SRAM_MODEL_LOADED
`define TSMC_SRAM_BEHAV_MODEL
// Only define this behavioral model if the foundry model is NOT compiled in.
// When using the foundry model, compile N16ADFP_SRAM_100a.v with
// +define+TSMC_SRAM_MODEL_LOADED to skip this file.

module TS1N16ADFPCLLLVTA128X64M4SWSHOD (
    input  logic        SLP,
    input  logic        DSLP,
    input  logic        SD,
    output logic        PUDELAY,
    input  logic        CLK,
    input  logic        CEB,
    input  logic        WEB,
    input  logic [6:0]  A,
    input  logic [63:0] D,
    input  logic [63:0] BWEB,
    input  logic [1:0]  RTSEL,
    input  logic [1:0]  WTSEL,
    output logic [63:0] Q
);

    localparam int unsigned DEPTH = 128;
    localparam int unsigned WIDTH = 64;
    localparam int unsigned BYTES_PER_WORD = WIDTH / 8;
    localparam int unsigned BYTE_DEPTH = DEPTH * BYTES_PER_WORD;
    localparam int unsigned BYTE_WINDOW_LOGICAL_BYTES = BYTE_DEPTH / 2;

    logic [WIDTH-1:0] mem [0:DEPTH-1];
    logic [7:0] byte_mem [0:BYTE_DEPTH-1];
    logic       byte_window_mode;
    logic [2:0] byte_window_offset;

    assign PUDELAY = 1'b0 & (RTSEL === RTSEL) & (WTSEL === WTSEL);
    assign byte_window_mode = SLP & SD & ~DSLP;
    assign byte_window_offset = {WTSEL[0], RTSEL};

    always @(posedge CLK) begin
        if (!CEB) begin
            if (byte_window_mode) begin
                logic [WIDTH-1:0] read_word;
                int unsigned      base_byte_addr;

                base_byte_addr = $unsigned(($unsigned(A) * BYTES_PER_WORD) + $unsigned(byte_window_offset));
                if (!WEB) begin
                    for (int i = 0; i < WIDTH; i++) begin
                        int unsigned write_byte_addr;

                        write_byte_addr = $unsigned(base_byte_addr + (i / 8));
                        if (!BWEB[i]) begin
                            byte_mem[write_byte_addr][i % 8] <= D[i];
                        end
                    end
                    for (int i = 0; i < BYTES_PER_WORD - 1; i++) begin
                        byte_mem[BYTE_WINDOW_LOGICAL_BYTES + i] <= 8'h00;
                    end
                end

                read_word = '0;
                for (int i = 0; i < BYTES_PER_WORD; i++) begin
                    read_word[i*8 +: 8] = byte_mem[base_byte_addr + i];
                end
                Q <= read_word;
            end else if (!SLP && !DSLP && !SD) begin
                if (!WEB) begin
                    for (int i = 0; i < WIDTH; i++) begin
                        if (!BWEB[i]) begin
                            mem[A][i] <= D[i];
                        end
                    end
                end
                Q <= mem[A];
            end
        end
    end

    initial begin
        for (int i = 0; i < DEPTH; i++) begin
            mem[i] = {WIDTH{1'b0}};
        end
        for (int i = 0; i < BYTE_DEPTH; i++) begin
            byte_mem[i] = 8'h00;
        end
        Q = {WIDTH{1'b0}};
    end

endmodule

`endif // TSMC_SRAM_MODEL_LOADED