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
    logic noc_plo_to_bus_req_ready_w;
    logic bus_to_noc_plo_resp_valid_w;
    logic scan_chain_fire_w;

    always_comb begin
        scan_chain_fire_w = (scan_chain_enable === 1'b1) && (scan_chain_in === scan_chain_in);
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int unsigned i = 0; i < NUM_PES; i++) begin
                pe_cfg_reg[i] <= '0;
            end
            rx_mask_reg <= '0;
        end else begin
            if (scan_chain_fire_w) begin
                pe_cfg_reg[0] <= scan_chain_in;
                for (int unsigned i = 1; i < NUM_PES; i++) begin
                    pe_cfg_reg[i] <= pe_cfg_reg[i-1];
                end
            end
            rx_mask_reg <= rx_mask_next;
        end
    end

    // synopsys translate_off
    always_ff @(posedge clk) begin
        if (reset_n && scan_chain_fire_w
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
        logic command_ps, command_pd, command_pli, command_plo;
        logic [5:0] tag_ps, tag_pd, tag_pli, tag_plo;
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

        command_ps  = noc_ps_to_bus_req_data.addr[6];
        command_pd  = noc_pd_to_bus_req_data.addr[6];
        command_pli = noc_pli_to_bus_req_data.addr[6];
        command_plo = noc_plo_to_bus_req_data.addr[6];
        tag_ps      = noc_ps_to_bus_req_data.addr[5:0];
        tag_pd      = noc_pd_to_bus_req_data.addr[5:0];
        tag_pli     = noc_pli_to_bus_req_data.addr[5:0];
        tag_plo     = noc_plo_to_bus_req_data.addr[5:0];

        for (int unsigned i = 0; i < NUM_PES; i++) begin
            mask_ps[i]  = pe_cfg_reg[i].enable && (command_ps  || (pe_cfg_reg[i].ps_id  == tag_ps));
            mask_pd[i]  = pe_cfg_reg[i].enable && (command_pd  || (pe_cfg_reg[i].pd_id  == tag_pd));
            mask_pli[i] = pe_cfg_reg[i].enable && (command_pli || (pe_cfg_reg[i].pli_id == tag_pli));
            mask_plo[i] = pe_cfg_reg[i].enable && (command_plo || (pe_cfg_reg[i].plo_id == tag_plo));
        end

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            if (mask_ps[i] && !bus_to_pe_ps_req_ready[i]) begin
                all_ready = 1'b0;
            end
        end
        noc_ps_to_bus_req_ready = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            bus_to_pe_ps_req_valid[i] = noc_ps_to_bus_req_valid && all_ready && mask_ps[i];
        end

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            if (mask_pd[i] && !bus_to_pe_pd_req_ready[i]) begin
                all_ready = 1'b0;
            end
        end
        noc_pd_to_bus_req_ready = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            bus_to_pe_pd_req_valid[i] = noc_pd_to_bus_req_valid && all_ready && mask_pd[i];
        end

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            if (mask_pli[i] && !bus_to_pe_pli_req_ready[i]) begin
                all_ready = 1'b0;
            end
        end
        noc_pli_to_bus_req_ready = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            bus_to_pe_pli_req_valid[i] = noc_pli_to_bus_req_valid && all_ready && mask_pli[i];
        end

        all_ready = 1'b1;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            if (mask_plo[i] && !bus_to_pe_plo_req_ready[i]) begin
                all_ready = 1'b0;
            end
        end
        noc_plo_to_bus_req_ready_w = all_ready;
        for (int unsigned i = 0; i < NUM_PES; i++) begin
            bus_to_pe_plo_req_valid[i] = noc_plo_to_bus_req_valid && all_ready && mask_plo[i];
        end

        rx_mask_next = rx_mask_reg;
        if (noc_plo_to_bus_req_valid && noc_plo_to_bus_req_ready_w) begin
            rx_mask_next = mask_plo;
        end

        bus_to_noc_plo_resp_data = '0;
        bus_to_noc_plo_resp_data.status = NOC_NOP;
        bus_to_noc_plo_resp_valid_w = 1'b0;

        for (int unsigned i = 0; i < NUM_PES; i++) begin
            if (rx_mask_reg[i] && pe_to_bus_plo_resp_valid[i] && !bus_to_noc_plo_resp_valid_w) begin
                bus_to_noc_plo_resp_data = pe_to_bus_plo_resp_data[i];
                bus_to_noc_plo_resp_valid_w = 1'b1;
                pe_to_bus_plo_resp_ready[i] = bus_to_noc_plo_resp_ready;
            end
        end

        noc_plo_to_bus_req_ready = noc_plo_to_bus_req_ready_w;
        bus_to_noc_plo_resp_valid = bus_to_noc_plo_resp_valid_w;
    end
endmodule
