//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   NoCRouter
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   NoC Router — routes wide PS/PD/PLI/PLO requests to per-port
//                MBUS channels.  Provides FIFO buffering, ultra/broadcast
//                decoding, scan-chain command processing, and PLO response
//                collection.  Faithfully converted from the ESL (SystemC)
//                NoCRouter template.
// Dependencies:  hybridacc_utils_pkg, FIFO
// Revision:
//   2026/03/28 - Initial version (placeholder)
//   2026/04/02 - Full rewrite: add FIFOs, FSM, addr/mask ports, scan-chain
//                register, pending-read state machine (ESL parity).
// Additional Comments:
//   Assumptions:
//     - FIFO_DEPTH default 4 (matches ESL NetWorkOnChipConfig.noc_fifo_depth)
//     - Active-low async reset (matches ESL reset_signal_is(reset_n, false))
//     - PORT_WIDTH_BITS is always 64 (downstream noc_request_t.data is 64-bit)
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;

module NoCRouter #(
    parameter int unsigned NUM_PORTS       = 3,
    parameter int unsigned PORT_WIDTH_BITS = 64,
    parameter int unsigned FIFO_DEPTH      = 4
) (
    input  logic clk,
    input  logic reset_n,
    input  logic command_mode,
    input  logic [31:0] command_data,

    // === External input channels (from testbench / upper level) ===
    // PS (Program/Static weights)
    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_ps_in_data,
    input  logic [15:0]                          noc_ps_in_addr,
    input  logic [63:0]                          noc_ps_in_mask,
    input  logic                                 noc_ps_in_valid,
    output logic                                 noc_ps_in_ready,

    // PD (Program/Dynamic activations)
    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_pd_in_data,
    input  logic [15:0]                          noc_pd_in_addr,
    input  logic [63:0]                          noc_pd_in_mask,
    input  logic                                 noc_pd_in_valid,
    output logic                                 noc_pd_in_ready,

    // PLI (Partial-sum Local-network In / write)
    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_pli_in_data,
    input  logic [15:0]                          noc_pli_in_addr,
    input  logic [63:0]                          noc_pli_in_mask,
    input  logic                                 noc_pli_in_valid,
    output logic                                 noc_pli_in_ready,

    // PLO (Partial-sum Local-network Out / read request — addr only)
    input  noc_addr_req_t                        noc_plo_in_data,
    input  logic                                 noc_plo_in_valid,
    output logic                                 noc_plo_in_ready,

    // PLO output response (collected wide data + status)
    output logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_plo_out_data,
    output NOC_RESPONSE_STATUS                   noc_plo_out_status,
    output logic                                 noc_plo_out_valid,
    input  logic                                 noc_plo_out_ready,

    // === Per-port MBUS channels ===
    output noc_request_t  noc_ps_to_bus_req_data  [NUM_PORTS],
    output logic          noc_ps_to_bus_req_valid [NUM_PORTS],
    input  logic          noc_ps_to_bus_req_ready [NUM_PORTS],

    output noc_request_t  noc_pd_to_bus_req_data  [NUM_PORTS],
    output logic          noc_pd_to_bus_req_valid [NUM_PORTS],
    input  logic          noc_pd_to_bus_req_ready [NUM_PORTS],

    output noc_request_t  noc_pli_to_bus_req_data [NUM_PORTS],
    output logic          noc_pli_to_bus_req_valid[NUM_PORTS],
    input  logic          noc_pli_to_bus_req_ready[NUM_PORTS],

    output noc_addr_req_t noc_plo_to_bus_req_data [NUM_PORTS],
    output logic          noc_plo_to_bus_req_valid[NUM_PORTS],
    input  logic          noc_plo_to_bus_req_ready[NUM_PORTS],

    input  noc_response_t bus_to_noc_plo_resp_data [NUM_PORTS],
    input  logic          bus_to_noc_plo_resp_valid[NUM_PORTS],
    output logic          bus_to_noc_plo_resp_ready[NUM_PORTS],

    // === Scan-chain ===
    output logic          scan_chain_enable,
    input  ScanChainFormat scan_chain_in [NUM_PORTS],
    output ScanChainFormat scan_chain_out[NUM_PORTS]
);

    // =====================================================================
    // Local parameters
    // =====================================================================
    localparam int unsigned TDW    = NUM_PORTS * PORT_WIDTH_BITS;
    localparam int unsigned REQ_W  = TDW + 16 + 64;   // {mask,addr,data}
    localparam int unsigned RESP_W = TDW + 2;          // {status,data}

    // =====================================================================
    // Pack / unpack helpers
    // =====================================================================
    function automatic logic [REQ_W-1:0] pack_req(
        input logic [TDW-1:0] data, input logic [15:0] addr, input logic [63:0] mask
    );
        return {mask, addr, data};
    endfunction

    function automatic logic [RESP_W-1:0] pack_resp(
        input logic [TDW-1:0] data, input logic [1:0] status
    );
        return {status, data};
    endfunction

    // =====================================================================
    // FIFO wires
    // =====================================================================
    logic [REQ_W-1:0]  ps_fifo_in,  ps_fifo_out;
    logic               ps_fifo_push, ps_fifo_pop, ps_fifo_empty, ps_fifo_full;
    logic [REQ_W-1:0]  pd_fifo_in,  pd_fifo_out;
    logic               pd_fifo_push, pd_fifo_pop, pd_fifo_empty, pd_fifo_full;
    logic [REQ_W-1:0]  pli_fifo_in,  pli_fifo_out;
    logic               pli_fifo_push, pli_fifo_pop, pli_fifo_empty, pli_fifo_full;
    logic [15:0]        plo_fifo_in,  plo_fifo_out;
    logic               plo_fifo_push, plo_fifo_pop, plo_fifo_empty, plo_fifo_full;
    logic [RESP_W-1:0] resp_fifo_in,  resp_fifo_out;
    logic               resp_fifo_push, resp_fifo_pop, resp_fifo_empty, resp_fifo_full;

    logic zero_clear;
    assign zero_clear = 1'b0;

    // =====================================================================
    // FIFO instances
    // =====================================================================
    FIFO #(.T(logic [REQ_W-1:0]),  .DEPTH(FIFO_DEPTH)) u_ps_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(ps_fifo_in), .push(ps_fifo_push),
        .data_out(ps_fifo_out), .pop(ps_fifo_pop),
        .empty(ps_fifo_empty), .full(ps_fifo_full), .clear(zero_clear));
    FIFO #(.T(logic [REQ_W-1:0]),  .DEPTH(FIFO_DEPTH)) u_pd_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(pd_fifo_in), .push(pd_fifo_push),
        .data_out(pd_fifo_out), .pop(pd_fifo_pop),
        .empty(pd_fifo_empty), .full(pd_fifo_full), .clear(zero_clear));
    FIFO #(.T(logic [REQ_W-1:0]),  .DEPTH(FIFO_DEPTH)) u_pli_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(pli_fifo_in), .push(pli_fifo_push),
        .data_out(pli_fifo_out), .pop(pli_fifo_pop),
        .empty(pli_fifo_empty), .full(pli_fifo_full), .clear(zero_clear));
    FIFO #(.T(logic [15:0]),       .DEPTH(FIFO_DEPTH)) u_plo_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(plo_fifo_in), .push(plo_fifo_push),
        .data_out(plo_fifo_out), .pop(plo_fifo_pop),
        .empty(plo_fifo_empty), .full(plo_fifo_full), .clear(zero_clear));
    FIFO #(.T(logic [RESP_W-1:0]), .DEPTH(FIFO_DEPTH)) u_resp_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(resp_fifo_in), .push(resp_fifo_push),
        .data_out(resp_fifo_out), .pop(resp_fifo_pop),
        .empty(resp_fifo_empty), .full(resp_fifo_full), .clear(zero_clear));

    // =====================================================================
    // Sequential registers
    // =====================================================================
    ScanChainFormat scan_chain_data_reg;
    logic           scan_chain_enable_reg;
    logic           pending_read_reg;
    logic           pending_read_ultra_reg;

    ScanChainFormat scan_chain_data_next;
    logic           scan_chain_enable_next;
    logic           pending_read_next;
    logic           pending_read_ultra_next;

    logic           rx_stall;
    logic           command_mode_active_w;
    logic           command_is_scan_chain_w;

    logic [TDW-1:0] ps_req_data_w;
    logic [7:0]     ps_req_addr_w;
    logic [7:0]     ps_req_addr_high_w;
    logic [63:0]    ps_req_mask_w;
    logic           ps_req_is_ultra_w;
    logic [7:0]     ps_req_baddr_w;
    logic           ps_req_all_rdy_w;

    logic [TDW-1:0] pd_req_data_w;
    logic [7:0]     pd_req_addr_w;
    logic [7:0]     pd_req_addr_high_w;
    logic [63:0]    pd_req_mask_w;
    logic           pd_req_is_ultra_w;
    logic [7:0]     pd_req_baddr_w;
    logic           pd_req_all_rdy_w;

    logic [TDW-1:0] pli_req_data_w;
    logic [7:0]     pli_req_addr_w;
    logic [7:0]     pli_req_addr_high_w;
    logic [63:0]    pli_req_mask_w;
    logic           pli_req_is_ultra_w;
    logic [7:0]     pli_req_baddr_w;
    logic           pli_req_all_rdy_w;

    logic [7:0]     plo_req_addr_w;
    logic [7:0]     plo_req_addr_high_w;
    logic           plo_req_is_ultra_w;
    logic [7:0]     plo_req_baddr_w;
    logic           plo_req_all_rdy_w;

    logic [TDW-1:0]       plo_resp_collected_w;
    logic [NUM_PORTS-1:0] plo_resp_vrx_w;
    logic                 plo_resp_all_ok_w;
    logic                 plo_resp_err_w;
    logic                 plo_resp_seen_w;
    logic                 req_decode_known_w;
    logic                 plo_resp_decode_known_w;

    always_comb begin
        command_mode_active_w = (command_mode === 1'b1) && (command_data === command_data);
        command_is_scan_chain_w = command_mode_active_w
                               && (message_command_t'(command_data[3:0]) == CMD_NOC_SCAN_CHAIN);

        req_decode_known_w = (ps_req_data_w === ps_req_data_w)
                          && (ps_req_addr_w === ps_req_addr_w)
                          && (ps_req_addr_high_w === ps_req_addr_high_w)
                          && (ps_req_mask_w === ps_req_mask_w)
                          && (ps_req_is_ultra_w === ps_req_is_ultra_w)
                          && (ps_req_baddr_w === ps_req_baddr_w)
                          && (ps_req_all_rdy_w === ps_req_all_rdy_w)
                          && (pd_req_data_w === pd_req_data_w)
                          && (pd_req_addr_w === pd_req_addr_w)
                          && (pd_req_addr_high_w === pd_req_addr_high_w)
                          && (pd_req_mask_w === pd_req_mask_w)
                          && (pd_req_is_ultra_w === pd_req_is_ultra_w)
                          && (pd_req_baddr_w === pd_req_baddr_w)
                          && (pd_req_all_rdy_w === pd_req_all_rdy_w)
                          && (pli_req_data_w === pli_req_data_w)
                          && (pli_req_addr_w === pli_req_addr_w)
                          && (pli_req_addr_high_w === pli_req_addr_high_w)
                          && (pli_req_mask_w === pli_req_mask_w)
                          && (pli_req_is_ultra_w === pli_req_is_ultra_w)
                          && (pli_req_baddr_w === pli_req_baddr_w)
                          && (pli_req_all_rdy_w === pli_req_all_rdy_w)
                          && (plo_req_addr_w === plo_req_addr_w)
                          && (plo_req_addr_high_w === plo_req_addr_high_w)
                          && (plo_req_is_ultra_w === plo_req_is_ultra_w)
                          && (plo_req_baddr_w === plo_req_baddr_w)
                          && (plo_req_all_rdy_w === plo_req_all_rdy_w);

        plo_resp_decode_known_w = (plo_resp_collected_w === plo_resp_collected_w)
                               && (plo_resp_vrx_w === plo_resp_vrx_w)
                               && (plo_resp_all_ok_w === plo_resp_all_ok_w)
                               && (plo_resp_err_w === plo_resp_err_w)
                               && (plo_resp_seen_w === plo_resp_seen_w);
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            scan_chain_data_reg    <= '0;
            scan_chain_enable_reg  <= 1'b0;
            pending_read_reg       <= 1'b0;
            pending_read_ultra_reg <= 1'b0;
        end else begin
            scan_chain_data_reg    <= scan_chain_data_next;
            scan_chain_enable_reg  <= scan_chain_enable_next;
            pending_read_reg       <= pending_read_next;
            pending_read_ultra_reg <= pending_read_ultra_next;
        end
    end

    // =====================================================================
    // Block A — Input → FIFO push
    // =====================================================================
    always_comb begin
        ps_fifo_in      = pack_req(noc_ps_in_data, noc_ps_in_addr, noc_ps_in_mask);
        noc_ps_in_ready = noc_ps_in_valid && !ps_fifo_full;
        ps_fifo_push    = noc_ps_in_valid && !ps_fifo_full;

        pd_fifo_in      = pack_req(noc_pd_in_data, noc_pd_in_addr, noc_pd_in_mask);
        noc_pd_in_ready = noc_pd_in_valid && !pd_fifo_full;
        pd_fifo_push    = noc_pd_in_valid && !pd_fifo_full;

        pli_fifo_in      = pack_req(noc_pli_in_data, noc_pli_in_addr, noc_pli_in_mask);
        noc_pli_in_ready = noc_pli_in_valid && !pli_fifo_full;
        pli_fifo_push    = noc_pli_in_valid && !pli_fifo_full;

        plo_fifo_in      = noc_plo_in_data.addr;
        noc_plo_in_ready = noc_plo_in_valid && !plo_fifo_full;
        plo_fifo_push    = noc_plo_in_valid && !plo_fifo_full;
    end

    // =====================================================================
    // Block B — Resp FIFO → PLO output
    // =====================================================================
    always_comb begin
        if (!resp_fifo_empty) begin
            noc_plo_out_valid  = 1'b1;
            noc_plo_out_data   = resp_fifo_out[TDW-1:0];
            noc_plo_out_status = NOC_RESPONSE_STATUS'(resp_fifo_out[TDW +: 2]);
            resp_fifo_pop      = noc_plo_out_ready;
        end else begin
            noc_plo_out_valid  = 1'b0;
            noc_plo_out_data   = '0;
            noc_plo_out_status = NOC_OK;
            resp_fifo_pop      = 1'b0;
        end
    end

    // =====================================================================
    // Block C — PS request processing (ESL process_requests_noc_ps)
    // =====================================================================
    always_comb begin
        ps_req_data_w = '0;
        ps_req_addr_w = '0;
        ps_req_addr_high_w = '0;
        ps_req_mask_w = '0;
        ps_req_is_ultra_w = 1'b0;
        ps_req_baddr_w = '0;
        ps_req_all_rdy_w = 1'b1;
        ps_fifo_pop = 1'b0;
        for (int unsigned i = 0; i < NUM_PORTS; i++) begin
            noc_ps_to_bus_req_valid[i] = 1'b0;
            noc_ps_to_bus_req_data[i]  = '0;
        end

        if (command_mode_active_w && !command_is_scan_chain_w) begin
            // Sideband PE command bypass
            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                noc_ps_to_bus_req_valid[i]     = 1'b1;
                noc_ps_to_bus_req_data[i].addr = 16'h0040;
                noc_ps_to_bus_req_data[i].data = {32'b0, command_data};
                noc_ps_to_bus_req_data[i].mask = '0;
            end
        end else begin
            if (!ps_fifo_empty) begin
                ps_req_data_w = ps_fifo_out[TDW-1:0];
                ps_req_addr_w = ps_fifo_out[TDW +: 8];
                ps_req_addr_high_w = ps_fifo_out[TDW+8 +: 8];
                ps_req_mask_w = ps_fifo_out[(TDW+16) +: 64];
                ps_req_is_ultra_w = ps_req_addr_w[6];
                ps_req_baddr_w = (ps_req_addr_w[7] ? 8'h40 : 8'h00) | {2'b0, ps_req_addr_w[5:0]};
                ps_req_all_rdy_w = 1'b1;

                for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                    noc_ps_to_bus_req_data[i].addr = {8'b0, ps_req_baddr_w};
                    noc_ps_to_bus_req_data[i].mask = ps_req_mask_w;
                    noc_ps_to_bus_req_data[i].data = ps_req_is_ultra_w ? ps_req_data_w[64*i +: 64]
                                                                       : ps_req_data_w[63:0];
                end
                for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                    if (!noc_ps_to_bus_req_ready[i]) begin
                        ps_req_all_rdy_w = 1'b0;
                    end
                end
                if (ps_req_addr_high_w !== ps_req_addr_high_w) begin
                    ps_req_all_rdy_w = 1'b0;
                end
                if (ps_req_all_rdy_w && req_decode_known_w) begin
                    for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                        noc_ps_to_bus_req_valid[i] = 1'b1;
                    end
                    ps_fifo_pop = 1'b1;
                end
            end
        end
    end

    // =====================================================================
    // Block D — PD request processing
    // =====================================================================
    always_comb begin
        pd_req_data_w = '0;
        pd_req_addr_w = '0;
        pd_req_addr_high_w = '0;
        pd_req_mask_w = '0;
        pd_req_is_ultra_w = 1'b0;
        pd_req_baddr_w = '0;
        pd_req_all_rdy_w = 1'b1;
        pd_fifo_pop = 1'b0;
        for (int unsigned i = 0; i < NUM_PORTS; i++) begin
            noc_pd_to_bus_req_valid[i] = 1'b0;
            noc_pd_to_bus_req_data[i]  = '0;
        end
        if (!pd_fifo_empty) begin
            pd_req_data_w = pd_fifo_out[TDW-1:0];
            pd_req_addr_w = pd_fifo_out[TDW +: 8];
            pd_req_addr_high_w = pd_fifo_out[TDW+8 +: 8];
            pd_req_mask_w = pd_fifo_out[(TDW+16) +: 64];
            pd_req_is_ultra_w = pd_req_addr_w[6];
            pd_req_baddr_w = (pd_req_addr_w[7] ? 8'h40 : 8'h00) | {2'b0, pd_req_addr_w[5:0]};
            pd_req_all_rdy_w = 1'b1;

            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                noc_pd_to_bus_req_data[i].addr = {8'b0, pd_req_baddr_w};
                noc_pd_to_bus_req_data[i].mask = pd_req_mask_w;
                noc_pd_to_bus_req_data[i].data = pd_req_is_ultra_w ? pd_req_data_w[64*i +: 64]
                                                                   : pd_req_data_w[63:0];
            end
            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                if (!noc_pd_to_bus_req_ready[i]) begin
                    pd_req_all_rdy_w = 1'b0;
                end
            end
            if (pd_req_addr_high_w !== pd_req_addr_high_w) begin
                pd_req_all_rdy_w = 1'b0;
            end
            if (pd_req_all_rdy_w && req_decode_known_w) begin
                for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                    noc_pd_to_bus_req_valid[i] = 1'b1;
                end
                pd_fifo_pop = 1'b1;
            end
        end
    end

    // =====================================================================
    // Block E — PLI request processing
    // =====================================================================
    always_comb begin
        pli_req_data_w = '0;
        pli_req_addr_w = '0;
        pli_req_addr_high_w = '0;
        pli_req_mask_w = '0;
        pli_req_is_ultra_w = 1'b0;
        pli_req_baddr_w = '0;
        pli_req_all_rdy_w = 1'b1;
        pli_fifo_pop = 1'b0;
        for (int unsigned i = 0; i < NUM_PORTS; i++) begin
            noc_pli_to_bus_req_valid[i] = 1'b0;
            noc_pli_to_bus_req_data[i]  = '0;
        end
        if (!pli_fifo_empty) begin
            pli_req_data_w = pli_fifo_out[TDW-1:0];
            pli_req_addr_w = pli_fifo_out[TDW +: 8];
            pli_req_addr_high_w = pli_fifo_out[TDW+8 +: 8];
            pli_req_mask_w = pli_fifo_out[(TDW+16) +: 64];
            pli_req_is_ultra_w = pli_req_addr_w[6];
            pli_req_baddr_w = (pli_req_addr_w[7] ? 8'h40 : 8'h00) | {2'b0, pli_req_addr_w[5:0]};
            pli_req_all_rdy_w = 1'b1;

            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                noc_pli_to_bus_req_data[i].addr = {8'b0, pli_req_baddr_w};
                noc_pli_to_bus_req_data[i].mask = pli_req_mask_w;
                noc_pli_to_bus_req_data[i].data = pli_req_is_ultra_w ? pli_req_data_w[64*i +: 64]
                                                                     : pli_req_data_w[63:0];
            end
            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                if (!noc_pli_to_bus_req_ready[i]) begin
                    pli_req_all_rdy_w = 1'b0;
                end
            end
            if (pli_req_addr_high_w !== pli_req_addr_high_w) begin
                pli_req_all_rdy_w = 1'b0;
            end
            if (pli_req_all_rdy_w && req_decode_known_w) begin
                for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                    noc_pli_to_bus_req_valid[i] = 1'b1;
                end
                pli_fifo_pop = 1'b1;
            end
        end
    end

    // =====================================================================
    // Block F — PLO request processing (ESL process_requests_noc_plo)
    // =====================================================================
    always_comb begin
        plo_req_addr_w = '0;
        plo_req_addr_high_w = '0;
        plo_req_is_ultra_w = 1'b0;
        plo_req_baddr_w = '0;
        plo_req_all_rdy_w = 1'b1;
        plo_fifo_pop            = 1'b0;
        pending_read_next       = 1'b0;
        pending_read_ultra_next = 1'b0;
        for (int unsigned i = 0; i < NUM_PORTS; i++) begin
            noc_plo_to_bus_req_valid[i] = 1'b0;
            noc_plo_to_bus_req_data[i]  = '0;
        end

        if (rx_stall) begin
            pending_read_next       = pending_read_reg;
            pending_read_ultra_next = pending_read_ultra_reg;
        end else if (!plo_fifo_empty) begin
            plo_req_addr_w = plo_fifo_out[7:0];
            plo_req_addr_high_w = plo_fifo_out[15:8];
            plo_req_is_ultra_w = plo_req_addr_w[6];
            plo_req_baddr_w = (plo_req_addr_w[7] ? 8'h40 : 8'h00) | {2'b0, plo_req_addr_w[5:0]};
            plo_req_all_rdy_w = 1'b1;

            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                noc_plo_to_bus_req_data[i].addr = {8'b0, plo_req_baddr_w};
            end

            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                if (!noc_plo_to_bus_req_ready[i]) begin
                    plo_req_all_rdy_w = 1'b0;
                end
            end
            if (plo_req_addr_high_w !== plo_req_addr_high_w) begin
                plo_req_all_rdy_w = 1'b0;
            end

            if (plo_req_all_rdy_w && req_decode_known_w) begin
                for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                    noc_plo_to_bus_req_valid[i] = 1'b1;
                end
                plo_fifo_pop            = 1'b1;
                pending_read_next       = 1'b1;
                pending_read_ultra_next = plo_req_is_ultra_w;
            end
        end
    end

    // =====================================================================
    // Block G — PLO response collection (ESL process_responses_plo)
    // =====================================================================
    always_comb begin
        plo_resp_collected_w = '0;
        plo_resp_vrx_w = '0;
        plo_resp_all_ok_w = 1'b1;
        plo_resp_err_w = 1'b0;
        plo_resp_seen_w = 1'b0;
        resp_fifo_push = 1'b0;
        resp_fifo_in   = '0;
        rx_stall       = 1'b0;
        for (int unsigned i = 0; i < NUM_PORTS; i++) begin
            bus_to_noc_plo_resp_ready[i] = 1'b0;
        end

        if (pending_read_reg) begin
            for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                if (bus_to_noc_plo_resp_valid[i]) begin
                    plo_resp_vrx_w[i] = 1'b1;
                    if (bus_to_noc_plo_resp_data[i].status == NOC_ERROR) begin
                        plo_resp_err_w = 1'b1;
                    end
                end
            end

            if (pending_read_ultra_reg) begin
                for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                    if (!plo_resp_vrx_w[i]) begin
                        plo_resp_all_ok_w = 1'b0;
                    end else begin
                        plo_resp_collected_w[64*i +: 64] = bus_to_noc_plo_resp_data[i].data;
                    end
                end
            end else begin
                plo_resp_seen_w = 1'b0;
                for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                    if (plo_resp_vrx_w[i]) begin
                        if (plo_resp_seen_w) begin
                            plo_resp_err_w = 1'b1;
                        end
                        plo_resp_seen_w = 1'b1;
                        plo_resp_collected_w[63:0] = bus_to_noc_plo_resp_data[i].data;
                    end
                end
                if (!plo_resp_seen_w) begin
                    plo_resp_all_ok_w = 1'b0;
                end
            end

            if (plo_resp_all_ok_w && plo_resp_decode_known_w) begin
                if (!resp_fifo_full) begin
                    for (int unsigned i = 0; i < NUM_PORTS; i++) begin
                        if (plo_resp_vrx_w[i]) begin
                            bus_to_noc_plo_resp_ready[i] = 1'b1;
                        end
                    end
                    resp_fifo_push = 1'b1;
                    resp_fifo_in   = pack_resp(plo_resp_collected_w,
                                               plo_resp_err_w ? NOC_ERROR : NOC_OK);
                end else begin
                    rx_stall = 1'b1;
                end
            end else begin
                rx_stall = 1'b1;
            end
        end
    end

    // =====================================================================
    // Block H — Command processing (scan chain)
    // =====================================================================
    always_comb begin
        scan_chain_data_next   = scan_chain_data_reg;
        scan_chain_enable_next = 1'b0;
        if (command_is_scan_chain_w) begin
            scan_chain_data_next   = parse_scan_chain_data(command_data[30:4]);
            scan_chain_enable_next = 1'b1;
        end
    end

    // =====================================================================
    // Block I — Scan-chain output
    // =====================================================================
    always_comb begin
        logic scan_tail_known;

        scan_tail_known = (scan_chain_in[NUM_PORTS-1] === scan_chain_in[NUM_PORTS-1]);
        scan_chain_enable = scan_chain_enable_reg && scan_tail_known;
        if (scan_chain_enable_reg && scan_tail_known) begin
            scan_chain_out[0] = scan_chain_data_reg;
            for (int unsigned i = 1; i < NUM_PORTS; i++)
                scan_chain_out[i] = scan_chain_in[i-1];
        end else begin
            for (int unsigned i = 0; i < NUM_PORTS; i++)
                scan_chain_out[i] = '0;
        end
    end

    // synopsys translate_off
    always_ff @(posedge clk) begin
        if (reset_n && ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))) begin
            if (command_mode_active_w && !command_is_scan_chain_w) begin
                $display("[%0t] [TRACE][NOC] sideband_cmd data=0x%08x opcode=0x%01x",
                         $time,
                         command_data,
                         command_data[3:0]);
            end
            if (command_is_scan_chain_w) begin
                automatic ScanChainFormat trace_fmt;

                trace_fmt = parse_scan_chain_data(command_data[30:4]);
                $display("[%0t] [TRACE][NOC] scan_chain enable=%0b ps=%0d pd=%0d pli=%0d plo=%0d route=%0d",
                         $time,
                         trace_fmt.enable,
                         trace_fmt.ps_id,
                         trace_fmt.pd_id,
                         trace_fmt.pli_id,
                         trace_fmt.plo_id,
                         trace_fmt.route_mode);
            end
        end
    end
    // synopsys translate_on

endmodule
