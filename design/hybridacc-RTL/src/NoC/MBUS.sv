//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   MBUS
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Common utility package with type definitions, FP16 arithmetic, and shared constants.
// Dependencies:  hybridacc_utils_pkg
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;

module MBUS #(
    parameter int unsigned NUM_PES = 16
) (
    input  logic clk,
    input  logic reset_n,

    output logic       router_enable[NUM_PES],
    output PERouterMode router_mode[NUM_PES],

    output noc_request_t bus_to_pe_ps_req_data [NUM_PES],
    output logic         bus_to_pe_ps_req_valid[NUM_PES],
    input  logic         bus_to_pe_ps_req_ready[NUM_PES],

    output noc_request_t bus_to_pe_pd_req_data [NUM_PES],
    output logic         bus_to_pe_pd_req_valid[NUM_PES],
    input  logic         bus_to_pe_pd_req_ready[NUM_PES],

    output noc_request_t bus_to_pe_pli_req_data [NUM_PES],
    output logic         bus_to_pe_pli_req_valid[NUM_PES],
    input  logic         bus_to_pe_pli_req_ready[NUM_PES],

    output noc_addr_req_t bus_to_pe_plo_req_data [NUM_PES],
    output logic          bus_to_pe_plo_req_valid[NUM_PES],
    input  logic          bus_to_pe_plo_req_ready[NUM_PES],

    input  noc_response_t pe_to_bus_plo_resp_data [NUM_PES],
    input  logic          pe_to_bus_plo_resp_valid[NUM_PES],
    output logic          pe_to_bus_plo_resp_ready[NUM_PES],

    input  logic pe_busy[NUM_PES],

    input  noc_request_t noc_ps_to_bus_req_data,
    input  logic         noc_ps_to_bus_req_valid,
    output logic         noc_ps_to_bus_req_ready,

    input  noc_request_t noc_pd_to_bus_req_data,
    input  logic         noc_pd_to_bus_req_valid,
    output logic         noc_pd_to_bus_req_ready,

    input  noc_request_t noc_pli_to_bus_req_data,
    input  logic         noc_pli_to_bus_req_valid,
    output logic         noc_pli_to_bus_req_ready,

    input  noc_addr_req_t noc_plo_to_bus_req_data,
    input  logic          noc_plo_to_bus_req_valid,
    output logic          noc_plo_to_bus_req_ready,

    output noc_response_t bus_to_noc_plo_resp_data,
    output logic          bus_to_noc_plo_resp_valid,
    input  logic          bus_to_noc_plo_resp_ready,

    input  logic scan_chain_enable,
    input  ScanChainFormat scan_chain_in,
    output ScanChainFormat scan_chain_out
);
    ScanChainFormat pe_cfg_reg[NUM_PES];
    logic [NUM_PES-1:0] rx_mask_reg, rx_mask_next;

    function automatic logic [NUM_PES-1:0] calc_mask(input logic [15:0] addr, input NOC_CHANNELS ch);
        logic [NUM_PES-1:0] mask;
        logic command;
        logic [5:0] tag;

        mask = '0;
        command = addr[6];
        tag = addr[5:0];
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            mask[i] = pe_cfg_reg[i].enable && (
                command ||
                ((ch === NOC_CHANNEL_PS)  && (pe_cfg_reg[i].ps_id  == tag)) ||
                ((ch === NOC_CHANNEL_PD)  && (pe_cfg_reg[i].pd_id  == tag)) ||
                ((ch === NOC_CHANNEL_PLI) && (pe_cfg_reg[i].pli_id == tag)) ||
                ((ch === NOC_CHANNEL_PLO) && (pe_cfg_reg[i].plo_id == tag))
            );
        end
        return mask;
    endfunction

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int unsigned i = 0; i < NUM_PES; i++) pe_cfg_reg[i] <= '0;
            rx_mask_reg <= '0;
        end else begin
            if (scan_chain_enable) begin
                pe_cfg_reg[0] <= scan_chain_in;
                for (int unsigned i = 1; i < NUM_PES; i++) pe_cfg_reg[i] <= pe_cfg_reg[i-1];
            end
            rx_mask_reg <= rx_mask_next;
        end
    end

    // synopsys translate_off
    always_ff @(posedge clk) begin
        if (reset_n && scan_chain_enable
            && ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))) begin
            $display("[%0t] [TRACE][MBUS] scan_shift enable=%0b ps=%0d pd=%0d pli=%0d plo=%0d route=%0d",
                     $time,
                     scan_chain_in.enable,
                     scan_chain_in.ps_id,
                     scan_chain_in.pd_id,
                     scan_chain_in.pli_id,
                     scan_chain_in.plo_id,
                     scan_chain_in.route_mode);
        end
    end
    // synopsys translate_on

    always_comb begin
        logic [NUM_PES-1:0] mask_ps, mask_pd, mask_pli, mask_plo;
        logic all_ready;

        scan_chain_out = pe_cfg_reg[NUM_PES-1];

        for (int unsigned i = 0; i < NUM_PES; i++) begin
            router_enable[i] = pe_cfg_reg[i].enable;
            router_mode[i] = pe_cfg_reg[i].route_mode;
            bus_to_pe_ps_req_data[i] = noc_ps_to_bus_req_data;
            bus_to_pe_pd_req_data[i] = noc_pd_to_bus_req_data;
            bus_to_pe_pli_req_data[i] = noc_pli_to_bus_req_data;
            bus_to_pe_plo_req_data[i] = noc_plo_to_bus_req_data;
            bus_to_pe_ps_req_valid[i] = 1'b0;
            bus_to_pe_pd_req_valid[i] = 1'b0;
            bus_to_pe_pli_req_valid[i] = 1'b0;
            bus_to_pe_plo_req_valid[i] = 1'b0;
            pe_to_bus_plo_resp_ready[i] = 1'b0;
        end

        mask_ps = calc_mask(noc_ps_to_bus_req_data.addr, NOC_CHANNEL_PS);
        mask_pd = calc_mask(noc_pd_to_bus_req_data.addr, NOC_CHANNEL_PD);
        mask_pli = calc_mask(noc_pli_to_bus_req_data.addr, NOC_CHANNEL_PLI);
        mask_plo = calc_mask(noc_plo_to_bus_req_data.addr, NOC_CHANNEL_PLO);

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) if (mask_ps[i] && !bus_to_pe_ps_req_ready[i]) all_ready = 1'b0;
        noc_ps_to_bus_req_ready = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) bus_to_pe_ps_req_valid[i] = noc_ps_to_bus_req_valid && all_ready && mask_ps[i];

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) if (mask_pd[i] && !bus_to_pe_pd_req_ready[i]) all_ready = 1'b0;
        noc_pd_to_bus_req_ready = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) bus_to_pe_pd_req_valid[i] = noc_pd_to_bus_req_valid && all_ready && mask_pd[i];

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) if (mask_pli[i] && !bus_to_pe_pli_req_ready[i]) all_ready = 1'b0;
        noc_pli_to_bus_req_ready = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) bus_to_pe_pli_req_valid[i] = noc_pli_to_bus_req_valid && all_ready && mask_pli[i];

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) if (mask_plo[i] && !bus_to_pe_plo_req_ready[i]) all_ready = 1'b0;
        noc_plo_to_bus_req_ready = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) bus_to_pe_plo_req_valid[i] = noc_plo_to_bus_req_valid && all_ready && mask_plo[i];

        rx_mask_next = rx_mask_reg;
        if (noc_plo_to_bus_req_valid && noc_plo_to_bus_req_ready) rx_mask_next = mask_plo;

        bus_to_noc_plo_resp_data = '0;
        bus_to_noc_plo_resp_data.status = NOC_NOP;
        bus_to_noc_plo_resp_valid = 1'b0;

        for (int unsigned i = 0; i < NUM_PES; i++) begin
            if (rx_mask_reg[i] && pe_to_bus_plo_resp_valid[i] && !bus_to_noc_plo_resp_valid) begin
                bus_to_noc_plo_resp_data = pe_to_bus_plo_resp_data[i];
                bus_to_noc_plo_resp_valid = 1'b1;
                pe_to_bus_plo_resp_ready[i] = bus_to_noc_plo_resp_ready;
            end
        end
    end
endmodule
