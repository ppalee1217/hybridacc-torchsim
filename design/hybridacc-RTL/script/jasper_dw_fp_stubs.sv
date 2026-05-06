`ifdef HACC_JASPER_DW_STUBS

module DW_fp_mult #(
    parameter int sig_width = 23,
    parameter int exp_width = 8,
    parameter int ieee_compliance = 0
) (
    input  logic [sig_width + exp_width : 0] a,
    input  logic [sig_width + exp_width : 0] b,
    input  logic [2:0]                       rnd,
    output logic [sig_width + exp_width : 0] z,
    output logic [7:0]                       status
);
    logic input_observed;

    assign input_observed = (^a) ^ (^b) ^ (^rnd);
    assign z = {(sig_width + exp_width + 1){input_observed}};
    assign status = {7'b0, input_observed};
endmodule

module DW_fp_add #(
    parameter int sig_width = 23,
    parameter int exp_width = 8,
    parameter int ieee_compliance = 0
) (
    input  logic [sig_width + exp_width : 0] a,
    input  logic [sig_width + exp_width : 0] b,
    input  logic [2:0]                       rnd,
    output logic [sig_width + exp_width : 0] z,
    output logic [7:0]                       status
);
    logic input_observed;

    assign input_observed = (^a) ^ (^b) ^ (^rnd);
    assign z = {(sig_width + exp_width + 1){input_observed}};
    assign status = {7'b0, input_observed};
endmodule

`endif