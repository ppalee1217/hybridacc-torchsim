// Module: NoCRouter
// Function: Router wrapper that combines multiple MBUS instances and PE clusters into one NoC-facing port group.
import hybridacc_utils_pkg::*;

module NoCRouter #(
    parameter int unsigned NUM_PORTS = 3,
    parameter int unsigned PORT_WIDTH_BITS = 64
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
    input  logic noc_plo_out_ready,

    output noc_request_t noc_ps_to_bus_req_data [NUM_PORTS],
    output logic         noc_ps_to_bus_req_valid[NUM_PORTS],
    input  logic         noc_ps_to_bus_req_ready[NUM_PORTS],

    output noc_request_t noc_pd_to_bus_req_data [NUM_PORTS],
    output logic         noc_pd_to_bus_req_valid[NUM_PORTS],
    input  logic         noc_pd_to_bus_req_ready[NUM_PORTS],

    output noc_request_t noc_pli_to_bus_req_data [NUM_PORTS],
    output logic         noc_pli_to_bus_req_valid[NUM_PORTS],
    input  logic         noc_pli_to_bus_req_ready[NUM_PORTS],

    output noc_addr_req_t noc_plo_to_bus_req_data [NUM_PORTS],
    output logic          noc_plo_to_bus_req_valid[NUM_PORTS],
    input  logic          noc_plo_to_bus_req_ready[NUM_PORTS],

    input  noc_response_t bus_to_noc_plo_resp_data [NUM_PORTS],
    input  logic          bus_to_noc_plo_resp_valid[NUM_PORTS],
    output logic          bus_to_noc_plo_resp_ready[NUM_PORTS],

    output logic scan_chain_enable,
    input  ScanChainFormat scan_chain_in [NUM_PORTS],
    output ScanChainFormat scan_chain_out[NUM_PORTS]
);
    noc_request_t req_ps, req_pd, req_pli;
    logic [63:0] packed_resp [NUM_PORTS];

    always_comb begin
        req_ps.data = noc_ps_in_data;
        req_ps.addr = 16'h0;
        req_ps.mask = '1;

        req_pd.data = noc_pd_in_data;
        req_pd.addr = 16'h0;
        req_pd.mask = '1;

        req_pli.data = noc_pli_in_data;
        req_pli.addr = 16'h0;
        req_pli.mask = '1;

        scan_chain_enable = command_mode && (message_command_t'(command_data[3:0]) == CMD_NOC_SCAN_CHAIN);
        for (int i = 0; i < NUM_PORTS; i++) begin
            scan_chain_out[i] = scan_chain_in[i];
        end

        noc_ps_in_ready = 1'b1;
        noc_pd_in_ready = 1'b1;
        noc_pli_in_ready = 1'b1;
        noc_plo_in_ready = 1'b1;

        for (int i = 0; i < NUM_PORTS; i++) begin
            noc_ps_to_bus_req_data[i] = req_ps;
            noc_pd_to_bus_req_data[i] = req_pd;
            noc_pli_to_bus_req_data[i] = req_pli;
            noc_plo_to_bus_req_data[i] = noc_plo_in_data;

            noc_ps_to_bus_req_valid[i] = noc_ps_in_valid;
            noc_pd_to_bus_req_valid[i] = noc_pd_in_valid;
            noc_pli_to_bus_req_valid[i] = noc_pli_in_valid;
            noc_plo_to_bus_req_valid[i] = noc_plo_in_valid;

            bus_to_noc_plo_resp_ready[i] = noc_plo_out_ready;
            packed_resp[i] = bus_to_noc_plo_resp_data[i].data;
        end

        noc_plo_out_data = '0;
        for (int i = 0; i < NUM_PORTS; i++) begin
            noc_plo_out_data[i*PORT_WIDTH_BITS +: PORT_WIDTH_BITS] = packed_resp[i][PORT_WIDTH_BITS-1:0];
        end

        noc_plo_out_status = NOC_OK;
        noc_plo_out_valid = 1'b0;
        for (int i = 0; i < NUM_PORTS; i++) begin
            if (bus_to_noc_plo_resp_valid[i]) begin
                noc_plo_out_valid = 1'b1;
                if (bus_to_noc_plo_resp_data[i].status != NOC_OK) noc_plo_out_status = bus_to_noc_plo_resp_data[i].status;
            end
        end
    end
endmodule
