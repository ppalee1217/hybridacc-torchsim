// Module: NetworkOnChip
// Function: Top-level NoC fabric that distributes PS, PD, PLI, and PLO traffic across router ports.
import hybridacc_utils_pkg::*;

module NetworkOnChip #(
    parameter int unsigned NUM_PORTS = 3,
    parameter int unsigned PORT_WIDTH_BITS = 64,
    parameter int unsigned NUM_PES_PER_PORT = 4,
    parameter int unsigned PE_FIFO_DEPTH = 4
) (
    input  logic clk,
    input  logic reset_n,
    input  logic command_mode,
    input  logic [31:0] command_data,

    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_ps_in_data,
    input  logic noc_ps_in_valid,
    output logic noc_ps_in_ready,

    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_pd_in_data,
    input  logic noc_pd_in_valid,
    output logic noc_pd_in_ready,

    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_pli_in_data,
    input  logic noc_pli_in_valid,
    output logic noc_pli_in_ready,

    input  noc_addr_req_t noc_plo_in_data,
    input  logic noc_plo_in_valid,
    output logic noc_plo_in_ready,

    output logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_plo_out_data,
    output NOC_RESPONSE_STATUS noc_plo_out_status,
    output logic noc_plo_out_valid,
    input  logic noc_plo_out_ready
);
    noc_request_t noc_ps_to_bus_data [NUM_PORTS];
    logic         noc_ps_to_bus_valid[NUM_PORTS];
    logic         noc_ps_to_bus_ready[NUM_PORTS];

    noc_request_t noc_pd_to_bus_data [NUM_PORTS];
    logic         noc_pd_to_bus_valid[NUM_PORTS];
    logic         noc_pd_to_bus_ready[NUM_PORTS];

    noc_request_t noc_pli_to_bus_data [NUM_PORTS];
    logic         noc_pli_to_bus_valid[NUM_PORTS];
    logic         noc_pli_to_bus_ready[NUM_PORTS];

    noc_addr_req_t noc_plo_to_bus_data [NUM_PORTS];
    logic          noc_plo_to_bus_valid[NUM_PORTS];
    logic          noc_plo_to_bus_ready[NUM_PORTS];

    noc_response_t bus_to_noc_resp_data [NUM_PORTS];
    logic          bus_to_noc_resp_valid[NUM_PORTS];
    logic          bus_to_noc_resp_ready[NUM_PORTS];

    logic scan_chain_enable;
    ScanChainFormat router_scan_chain_in [NUM_PORTS];
    ScanChainFormat router_scan_chain_out[NUM_PORTS];
    ScanChainFormat mbus_scan_chain_out  [NUM_PORTS];

    logic router_enable_sig[NUM_PORTS][NUM_PES_PER_PORT];
    PERouterMode router_mode_sig[NUM_PORTS][NUM_PES_PER_PORT];

    noc_request_t bus_to_pe_ps_data [NUM_PORTS][NUM_PES_PER_PORT];
    logic         bus_to_pe_ps_valid[NUM_PORTS][NUM_PES_PER_PORT];
    logic         bus_to_pe_ps_ready[NUM_PORTS][NUM_PES_PER_PORT];

    noc_request_t bus_to_pe_pd_data [NUM_PORTS][NUM_PES_PER_PORT];
    logic         bus_to_pe_pd_valid[NUM_PORTS][NUM_PES_PER_PORT];
    logic         bus_to_pe_pd_ready[NUM_PORTS][NUM_PES_PER_PORT];

    noc_request_t bus_to_pe_pli_data [NUM_PORTS][NUM_PES_PER_PORT];
    logic         bus_to_pe_pli_valid[NUM_PORTS][NUM_PES_PER_PORT];
    logic         bus_to_pe_pli_ready[NUM_PORTS][NUM_PES_PER_PORT];

    noc_addr_req_t bus_to_pe_plo_data [NUM_PORTS][NUM_PES_PER_PORT];
    logic          bus_to_pe_plo_valid[NUM_PORTS][NUM_PES_PER_PORT];
    logic          bus_to_pe_plo_ready[NUM_PORTS][NUM_PES_PER_PORT];

    noc_response_t pe_to_bus_plo_data [NUM_PORTS][NUM_PES_PER_PORT];
    logic          pe_to_bus_plo_valid[NUM_PORTS][NUM_PES_PER_PORT];
    logic          pe_to_bus_plo_ready[NUM_PORTS][NUM_PES_PER_PORT];

    logic pe_busy_sig[NUM_PORTS][NUM_PES_PER_PORT];

    logic [63:0] ln_data [NUM_PORTS+1][NUM_PES_PER_PORT];
    logic        ln_valid[NUM_PORTS+1][NUM_PES_PER_PORT];
    logic        ln_ready[NUM_PORTS+1][NUM_PES_PER_PORT];

    NoCRouter #(.NUM_PORTS(NUM_PORTS), .PORT_WIDTH_BITS(PORT_WIDTH_BITS)) router (
        .clk(clk), .reset_n(reset_n), .command_mode(command_mode), .command_data(command_data),
        .noc_ps_in_data(noc_ps_in_data), .noc_ps_in_valid(noc_ps_in_valid), .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data), .noc_pd_in_valid(noc_pd_in_valid), .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data), .noc_pli_in_valid(noc_pli_in_valid), .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data), .noc_plo_in_valid(noc_plo_in_valid), .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data), .noc_plo_out_status(noc_plo_out_status), .noc_plo_out_valid(noc_plo_out_valid), .noc_plo_out_ready(noc_plo_out_ready),
        .noc_ps_to_bus_req_data(noc_ps_to_bus_data), .noc_ps_to_bus_req_valid(noc_ps_to_bus_valid), .noc_ps_to_bus_req_ready(noc_ps_to_bus_ready),
        .noc_pd_to_bus_req_data(noc_pd_to_bus_data), .noc_pd_to_bus_req_valid(noc_pd_to_bus_valid), .noc_pd_to_bus_req_ready(noc_pd_to_bus_ready),
        .noc_pli_to_bus_req_data(noc_pli_to_bus_data), .noc_pli_to_bus_req_valid(noc_pli_to_bus_valid), .noc_pli_to_bus_req_ready(noc_pli_to_bus_ready),
        .noc_plo_to_bus_req_data(noc_plo_to_bus_data), .noc_plo_to_bus_req_valid(noc_plo_to_bus_valid), .noc_plo_to_bus_req_ready(noc_plo_to_bus_ready),
        .bus_to_noc_plo_resp_data(bus_to_noc_resp_data), .bus_to_noc_plo_resp_valid(bus_to_noc_resp_valid), .bus_to_noc_plo_resp_ready(bus_to_noc_resp_ready),
        .scan_chain_enable(scan_chain_enable), .scan_chain_in(mbus_scan_chain_out), .scan_chain_out(router_scan_chain_out)
    );

    genvar i, j;
    generate
        for (i = 0; i < NUM_PORTS; i++) begin : gen_ports
            MBUS #(.NUM_PES(NUM_PES_PER_PORT)) mbus (
                .clk(clk), .reset_n(reset_n),
                .router_enable(router_enable_sig[i]), .router_mode(router_mode_sig[i]),
                .bus_to_pe_ps_req_data(bus_to_pe_ps_data[i]), .bus_to_pe_ps_req_valid(bus_to_pe_ps_valid[i]), .bus_to_pe_ps_req_ready(bus_to_pe_ps_ready[i]),
                .bus_to_pe_pd_req_data(bus_to_pe_pd_data[i]), .bus_to_pe_pd_req_valid(bus_to_pe_pd_valid[i]), .bus_to_pe_pd_req_ready(bus_to_pe_pd_ready[i]),
                .bus_to_pe_pli_req_data(bus_to_pe_pli_data[i]), .bus_to_pe_pli_req_valid(bus_to_pe_pli_valid[i]), .bus_to_pe_pli_req_ready(bus_to_pe_pli_ready[i]),
                .bus_to_pe_plo_req_data(bus_to_pe_plo_data[i]), .bus_to_pe_plo_req_valid(bus_to_pe_plo_valid[i]), .bus_to_pe_plo_req_ready(bus_to_pe_plo_ready[i]),
                .pe_to_bus_plo_resp_data(pe_to_bus_plo_data[i]), .pe_to_bus_plo_resp_valid(pe_to_bus_plo_valid[i]), .pe_to_bus_plo_resp_ready(pe_to_bus_plo_ready[i]),
                .pe_busy(pe_busy_sig[i]),
                .noc_ps_to_bus_req_data(noc_ps_to_bus_data[i]), .noc_ps_to_bus_req_valid(noc_ps_to_bus_valid[i]), .noc_ps_to_bus_req_ready(noc_ps_to_bus_ready[i]),
                .noc_pd_to_bus_req_data(noc_pd_to_bus_data[i]), .noc_pd_to_bus_req_valid(noc_pd_to_bus_valid[i]), .noc_pd_to_bus_req_ready(noc_pd_to_bus_ready[i]),
                .noc_pli_to_bus_req_data(noc_pli_to_bus_data[i]), .noc_pli_to_bus_req_valid(noc_pli_to_bus_valid[i]), .noc_pli_to_bus_req_ready(noc_pli_to_bus_ready[i]),
                .noc_plo_to_bus_req_data(noc_plo_to_bus_data[i]), .noc_plo_to_bus_req_valid(noc_plo_to_bus_valid[i]), .noc_plo_to_bus_req_ready(noc_plo_to_bus_ready[i]),
                .bus_to_noc_plo_resp_data(bus_to_noc_resp_data[i]), .bus_to_noc_plo_resp_valid(bus_to_noc_resp_valid[i]), .bus_to_noc_plo_resp_ready(bus_to_noc_resp_ready[i]),
                .scan_chain_enable(scan_chain_enable), .scan_chain_in(router_scan_chain_out[i]), .scan_chain_out(mbus_scan_chain_out[i])
            );

            for (j = 0; j < NUM_PES_PER_PORT; j++) begin : gen_pe
                ProcessElement #(.PE_FIFO_DEPTH(PE_FIFO_DEPTH)) pe (
                    .clk(clk), .reset_n(reset_n),
                    .router_enable(router_enable_sig[i][j]), .router_mode(router_mode_sig[i][j]),
                    .noc_ps_in_data(bus_to_pe_ps_data[i][j]), .noc_ps_in_valid(bus_to_pe_ps_valid[i][j]), .noc_ps_in_ready(bus_to_pe_ps_ready[i][j]),
                    .noc_pd_in_data(bus_to_pe_pd_data[i][j]), .noc_pd_in_valid(bus_to_pe_pd_valid[i][j]), .noc_pd_in_ready(bus_to_pe_pd_ready[i][j]),
                    .noc_pli_in_data(bus_to_pe_pli_data[i][j]), .noc_pli_in_valid(bus_to_pe_pli_valid[i][j]), .noc_pli_in_ready(bus_to_pe_pli_ready[i][j]),
                    .noc_plo_in_data(bus_to_pe_plo_data[i][j]), .noc_plo_in_valid(bus_to_pe_plo_valid[i][j]), .noc_plo_in_ready(bus_to_pe_plo_ready[i][j]),
                    .noc_plo_out_data(pe_to_bus_plo_data[i][j]), .noc_plo_out_valid(pe_to_bus_plo_valid[i][j]), .noc_plo_out_ready(pe_to_bus_plo_ready[i][j]),
                    .pe_busy(pe_busy_sig[i][j]),
                    .ln_pli_data(ln_data[i][j]), .ln_pli_valid(ln_valid[i][j]), .ln_pli_ready(ln_ready[i][j]),
                    .ln_plo_data(ln_data[i+1][j]), .ln_plo_valid(ln_valid[i+1][j]), .ln_plo_ready(ln_ready[i+1][j])
                );
            end
        end
    endgenerate
endmodule
