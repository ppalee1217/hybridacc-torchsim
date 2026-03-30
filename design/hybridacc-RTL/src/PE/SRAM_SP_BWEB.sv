//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/29
// Design Name:   HybridAcc
// Module Name:   TS1N16ADFPCLLLVTA128X64M4SWSHOD (behavioral model)
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Behavioral simulation model for TSMC TS1N16ADFPCLLLVTA128X64M4SWSHOD
//                128-word × 64-bit single-port SRAM with per-bit write enable.
//                For simulation: compile this file when foundry Verilog model
//                  (N16ADFP_SRAM_100a.v) is not available.
//                For synthesis: use the foundry .db timing library instead.
// Dependencies:  None
// Revision:
//   2026/03/29 - Initial version
// Additional Comments:
//   Port-compatible with TSMC foundry model. Power/test pins are accepted
//   but ignored in this behavioral model.
//-----------------------------------------------------------------------------

`ifndef TSMC_SRAM_MODEL_LOADED
`define TSMC_SRAM_MODEL_LOADED
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

    // ---- Behavioral memory array ----
    logic [WIDTH-1:0] mem [0:DEPTH-1];

    // PUDELAY is not modeled — tie low
    assign PUDELAY = 1'b0;

    always @(posedge CLK) begin
        if (!CEB && !SLP && !DSLP && !SD) begin
            if (!WEB) begin
                // Write with per-bit mask (BWEB active-low)
                for (int i = 0; i < WIDTH; i++) begin
                    if (!BWEB[i]) begin
                        mem[A][i] <= D[i];
                    end
                end
            end
            // Read — returns old data on write (read-before-write)
            Q <= mem[A];
        end
    end

    // ---- Initialization for simulation ----
    initial begin
        for (int i = 0; i < DEPTH; i++) begin
            mem[i] = {WIDTH{1'b0}};
        end
        Q = {WIDTH{1'b0}};
    end

endmodule

`endif // TSMC_SRAM_MODEL_LOADED
