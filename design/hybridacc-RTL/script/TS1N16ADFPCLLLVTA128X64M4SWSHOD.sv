`ifndef TSMC_SRAM_MODEL_LOADED
`define TSMC_SRAM_MODEL_LOADED

(* black_box, syn_black_box *)
module TS1N16ADFPCLLLVTA128X64M4SWSHOD (
    input  wire        SLP,
    input  wire        DSLP,
    input  wire        SD,
    output wire        PUDELAY,
    input  wire        CLK,
    input  wire        CEB,
    input  wire        WEB,
    input  wire [6:0]  A,
    input  wire [63:0] D,
    input  wire [63:0] BWEB,
    input  wire [1:0]  RTSEL,
    input  wire [1:0]  WTSEL,
    output wire [63:0] Q
);
endmodule

`endif