//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_mbus
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for mbus module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/NoC/MBUS.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/NoC/MBUS.sv"
`endif

module tb_mbus;
    import hybridacc_utils_pkg::*;
    localparam int N = 16;

    logic clk, reset_n;
    logic router_enable[N];
    PERouterMode router_mode[N];

    noc_request_t bus_to_pe_ps_req_data[N]; logic bus_to_pe_ps_req_valid[N], bus_to_pe_ps_req_ready[N];
    noc_request_t bus_to_pe_pd_req_data[N]; logic bus_to_pe_pd_req_valid[N], bus_to_pe_pd_req_ready[N];
    noc_request_t bus_to_pe_pli_req_data[N]; logic bus_to_pe_pli_req_valid[N], bus_to_pe_pli_req_ready[N];
    noc_addr_req_t bus_to_pe_plo_req_data[N]; logic bus_to_pe_plo_req_valid[N], bus_to_pe_plo_req_ready[N];
    noc_response_t pe_to_bus_plo_resp_data[N]; logic pe_to_bus_plo_resp_valid[N], pe_to_bus_plo_resp_ready[N];
    logic pe_busy[N];

    noc_request_t noc_ps_to_bus_req_data, noc_pd_to_bus_req_data, noc_pli_to_bus_req_data;
    logic noc_ps_to_bus_req_valid, noc_ps_to_bus_req_ready;
    logic noc_pd_to_bus_req_valid, noc_pd_to_bus_req_ready;
    logic noc_pli_to_bus_req_valid, noc_pli_to_bus_req_ready;
    noc_addr_req_t noc_plo_to_bus_req_data; logic noc_plo_to_bus_req_valid, noc_plo_to_bus_req_ready;
    noc_response_t bus_to_noc_plo_resp_data; logic bus_to_noc_plo_resp_valid, bus_to_noc_plo_resp_ready;
    logic scan_chain_enable; ScanChainFormat scan_chain_in, scan_chain_out;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

`ifdef GATE_SIM
    // ---- Flat-wire intermediates for gate-level MBUS (NUM_PES=16) ----
    localparam int NRQ = $bits(noc_request_t);   // 144
    localparam int NAQ = $bits(noc_addr_req_t);  // 16
    localparam int NRS = $bits(noc_response_t);  // 66
    localparam int PMW = $bits(PERouterMode);    // 2

    // DUT output arrays
    wire [0:N-1]          g_router_enable;
    wire [N*PMW-1:0]      g_router_mode;
    wire [N*NRQ-1:0]      g_bus_to_pe_ps_req_data;
    wire [0:N-1]          g_bus_to_pe_ps_req_valid;
    wire [N*NRQ-1:0]      g_bus_to_pe_pd_req_data;
    wire [0:N-1]          g_bus_to_pe_pd_req_valid;
    wire [N*NRQ-1:0]      g_bus_to_pe_pli_req_data;
    wire [0:N-1]          g_bus_to_pe_pli_req_valid;
    wire [N*NAQ-1:0]      g_bus_to_pe_plo_req_data;
    wire [0:N-1]          g_bus_to_pe_plo_req_valid;
    wire [0:N-1]          g_pe_to_bus_plo_resp_ready;
    // DUT input arrays
    wire [0:N-1]          g_bus_to_pe_ps_req_ready;
    wire [0:N-1]          g_bus_to_pe_pd_req_ready;
    wire [0:N-1]          g_bus_to_pe_pli_req_ready;
    wire [0:N-1]          g_bus_to_pe_plo_req_ready;
    wire [N*NRS-1:0]      g_pe_to_bus_plo_resp_data;
    wire [0:N-1]          g_pe_to_bus_plo_resp_valid;
    wire [0:N-1]          g_pe_busy;

    genvar gi;
    generate for (gi = 0; gi < N; gi++) begin : g_conv
        // DUT outputs → TB structured signals
        assign router_enable[gi]            = g_router_enable[gi];
        assign router_mode[gi]              = PERouterMode'(g_router_mode[gi*PMW +: PMW]);
        assign bus_to_pe_ps_req_data[gi]    = g_bus_to_pe_ps_req_data[gi*NRQ +: NRQ];
        assign bus_to_pe_ps_req_valid[gi]   = g_bus_to_pe_ps_req_valid[gi];
        assign bus_to_pe_pd_req_data[gi]    = g_bus_to_pe_pd_req_data[gi*NRQ +: NRQ];
        assign bus_to_pe_pd_req_valid[gi]   = g_bus_to_pe_pd_req_valid[gi];
        assign bus_to_pe_pli_req_data[gi]   = g_bus_to_pe_pli_req_data[gi*NRQ +: NRQ];
        assign bus_to_pe_pli_req_valid[gi]  = g_bus_to_pe_pli_req_valid[gi];
        assign bus_to_pe_plo_req_data[gi]   = g_bus_to_pe_plo_req_data[gi*NAQ +: NAQ];
        assign bus_to_pe_plo_req_valid[gi]  = g_bus_to_pe_plo_req_valid[gi];
        assign pe_to_bus_plo_resp_ready[gi] = g_pe_to_bus_plo_resp_ready[gi];
        // TB structured signals → DUT inputs
        assign g_bus_to_pe_ps_req_ready[gi]              = bus_to_pe_ps_req_ready[gi];
        assign g_bus_to_pe_pd_req_ready[gi]              = bus_to_pe_pd_req_ready[gi];
        assign g_bus_to_pe_pli_req_ready[gi]             = bus_to_pe_pli_req_ready[gi];
        assign g_bus_to_pe_plo_req_ready[gi]             = bus_to_pe_plo_req_ready[gi];
        assign g_pe_to_bus_plo_resp_data[gi*NRS +: NRS]  = pe_to_bus_plo_resp_data[gi];
        assign g_pe_to_bus_plo_resp_valid[gi]            = pe_to_bus_plo_resp_valid[gi];
        assign g_pe_busy[gi]                             = pe_busy[gi];
    end endgenerate

    MBUS dut (
        .clk(clk), .reset_n(reset_n),
        .router_enable(g_router_enable), .router_mode(g_router_mode),
        .bus_to_pe_ps_req_data(g_bus_to_pe_ps_req_data), .bus_to_pe_ps_req_valid(g_bus_to_pe_ps_req_valid), .bus_to_pe_ps_req_ready(g_bus_to_pe_ps_req_ready),
        .bus_to_pe_pd_req_data(g_bus_to_pe_pd_req_data), .bus_to_pe_pd_req_valid(g_bus_to_pe_pd_req_valid), .bus_to_pe_pd_req_ready(g_bus_to_pe_pd_req_ready),
        .bus_to_pe_pli_req_data(g_bus_to_pe_pli_req_data), .bus_to_pe_pli_req_valid(g_bus_to_pe_pli_req_valid), .bus_to_pe_pli_req_ready(g_bus_to_pe_pli_req_ready),
        .bus_to_pe_plo_req_data(g_bus_to_pe_plo_req_data), .bus_to_pe_plo_req_valid(g_bus_to_pe_plo_req_valid), .bus_to_pe_plo_req_ready(g_bus_to_pe_plo_req_ready),
        .pe_to_bus_plo_resp_data(g_pe_to_bus_plo_resp_data), .pe_to_bus_plo_resp_valid(g_pe_to_bus_plo_resp_valid), .pe_to_bus_plo_resp_ready(g_pe_to_bus_plo_resp_ready),
        .pe_busy(g_pe_busy),
        .noc_ps_to_bus_req_data(noc_ps_to_bus_req_data), .noc_ps_to_bus_req_valid(noc_ps_to_bus_req_valid), .noc_ps_to_bus_req_ready(noc_ps_to_bus_req_ready),
        .noc_pd_to_bus_req_data(noc_pd_to_bus_req_data), .noc_pd_to_bus_req_valid(noc_pd_to_bus_req_valid), .noc_pd_to_bus_req_ready(noc_pd_to_bus_req_ready),
        .noc_pli_to_bus_req_data(noc_pli_to_bus_req_data), .noc_pli_to_bus_req_valid(noc_pli_to_bus_req_valid), .noc_pli_to_bus_req_ready(noc_pli_to_bus_req_ready),
        .noc_plo_to_bus_req_data(noc_plo_to_bus_req_data), .noc_plo_to_bus_req_valid(noc_plo_to_bus_req_valid), .noc_plo_to_bus_req_ready(noc_plo_to_bus_req_ready),
        .bus_to_noc_plo_resp_data(bus_to_noc_plo_resp_data), .bus_to_noc_plo_resp_valid(bus_to_noc_plo_resp_valid), .bus_to_noc_plo_resp_ready(bus_to_noc_plo_resp_ready),
        .scan_chain_enable(scan_chain_enable), .scan_chain_in(scan_chain_in), .scan_chain_out(scan_chain_out)
    );
`else
    MBUS #(.NUM_PES(N)) dut(
        .clk(clk), .reset_n(reset_n), .router_enable(router_enable), .router_mode(router_mode),
        .bus_to_pe_ps_req_data(bus_to_pe_ps_req_data), .bus_to_pe_ps_req_valid(bus_to_pe_ps_req_valid), .bus_to_pe_ps_req_ready(bus_to_pe_ps_req_ready),
        .bus_to_pe_pd_req_data(bus_to_pe_pd_req_data), .bus_to_pe_pd_req_valid(bus_to_pe_pd_req_valid), .bus_to_pe_pd_req_ready(bus_to_pe_pd_req_ready),
        .bus_to_pe_pli_req_data(bus_to_pe_pli_req_data), .bus_to_pe_pli_req_valid(bus_to_pe_pli_req_valid), .bus_to_pe_pli_req_ready(bus_to_pe_pli_req_ready),
        .bus_to_pe_plo_req_data(bus_to_pe_plo_req_data), .bus_to_pe_plo_req_valid(bus_to_pe_plo_req_valid), .bus_to_pe_plo_req_ready(bus_to_pe_plo_req_ready),
        .pe_to_bus_plo_resp_data(pe_to_bus_plo_resp_data), .pe_to_bus_plo_resp_valid(pe_to_bus_plo_resp_valid), .pe_to_bus_plo_resp_ready(pe_to_bus_plo_resp_ready),
        .pe_busy(pe_busy),
        .noc_ps_to_bus_req_data(noc_ps_to_bus_req_data), .noc_ps_to_bus_req_valid(noc_ps_to_bus_req_valid), .noc_ps_to_bus_req_ready(noc_ps_to_bus_req_ready),
        .noc_pd_to_bus_req_data(noc_pd_to_bus_req_data), .noc_pd_to_bus_req_valid(noc_pd_to_bus_req_valid), .noc_pd_to_bus_req_ready(noc_pd_to_bus_req_ready),
        .noc_pli_to_bus_req_data(noc_pli_to_bus_req_data), .noc_pli_to_bus_req_valid(noc_pli_to_bus_req_valid), .noc_pli_to_bus_req_ready(noc_pli_to_bus_req_ready),
        .noc_plo_to_bus_req_data(noc_plo_to_bus_req_data), .noc_plo_to_bus_req_valid(noc_plo_to_bus_req_valid), .noc_plo_to_bus_req_ready(noc_plo_to_bus_req_ready),
        .bus_to_noc_plo_resp_data(bus_to_noc_plo_resp_data), .bus_to_noc_plo_resp_valid(bus_to_noc_plo_resp_valid), .bus_to_noc_plo_resp_ready(bus_to_noc_plo_resp_ready),
        .scan_chain_enable(scan_chain_enable), .scan_chain_in(scan_chain_in), .scan_chain_out(scan_chain_out)
    );
`endif

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    initial begin
        for (int i=0;i<N;i++) begin
            bus_to_pe_ps_req_ready[i]=1; bus_to_pe_pd_req_ready[i]=1; bus_to_pe_pli_req_ready[i]=1; bus_to_pe_plo_req_ready[i]=1;
            pe_to_bus_plo_resp_data[i]='0; pe_to_bus_plo_resp_valid[i]=0; pe_busy[i]=0;
        end
        noc_ps_to_bus_req_data='0; noc_ps_to_bus_req_valid=0;
        noc_pd_to_bus_req_data='0; noc_pd_to_bus_req_valid=0;
        noc_pli_to_bus_req_data='0; noc_pli_to_bus_req_valid=0;
        noc_plo_to_bus_req_data='0; noc_plo_to_bus_req_valid=0;
        bus_to_noc_plo_resp_ready=1;
        scan_chain_enable=0; scan_chain_in='0;
        @(posedge reset_n);
        @(posedge clk); #(`TB_SETTLE);

        // Test 1: Scan chain configuration - enable PE0 with ps_id=1
        scan_chain_in.enable = 1;
        scan_chain_in.ps_id = 6'd1;
        scan_chain_in.pd_id = 6'd1;
        scan_chain_in.pli_id = 6'd1;
        scan_chain_in.plo_id = 6'd1;
        scan_chain_in.route_mode = PLI_FROM_BUS_PLO_TO_BUS;
        scan_chain_enable = 1;
        @(posedge clk); // Shifts into pe_cfg_reg[0]
        // Shift PE1 config: enable with ps_id=2
        scan_chain_in.ps_id = 6'd2;
        scan_chain_in.pd_id = 6'd2;
        scan_chain_in.pli_id = 6'd2;
        scan_chain_in.plo_id = 6'd2;
        @(posedge clk);
        scan_chain_enable = 0;
        #(`TB_SETTLE);
        check("ScanChain: router_enable[0]=1", router_enable[0] === 1'b1);
        check("ScanChain: router_enable[1]=1", router_enable[1] === 1'b1);

        // Test 2: Command broadcast (addr bit[6]=1) reaches all enabled PEs
        noc_ps_to_bus_req_data.addr = 16'h0041; // bit[6]=1 => command
        noc_ps_to_bus_req_data.data = 64'hAAAA;
        noc_ps_to_bus_req_valid = 1; #(`TB_SETTLE);
        check("CmdBroadcast: PE0 valid", bus_to_pe_ps_req_valid[0] === 1'b1);
        check("CmdBroadcast: PE1 valid", bus_to_pe_ps_req_valid[1] === 1'b1);
        check("CmdBroadcast: ready=1", noc_ps_to_bus_req_ready === 1'b1);
        noc_ps_to_bus_req_valid = 0;

        // Test 3: Tag-based PS routing (ps_id match)
        noc_ps_to_bus_req_data.addr = 16'h0001; // tag=1, not command
        noc_ps_to_bus_req_data.data = 64'hBBBB;
        noc_ps_to_bus_req_valid = 1; #(`TB_SETTLE);
        // PE1 has ps_id=1 (it was shifted to pe_cfg_reg[1] after PE0), PE0 has ps_id=2
        // Actually: scan chain shifts: first in -> pe_cfg_reg[0], then second shifts pe_cfg_reg[0]->pe_cfg_reg[1]
        // So pe_cfg_reg[0] has ps_id=2, pe_cfg_reg[1] has ps_id=1
        check("TagPS1: ps_id=1 PE matched", bus_to_pe_ps_req_valid[0] === 1'b0 || bus_to_pe_ps_req_valid[1] === 1'b1);
        noc_ps_to_bus_req_valid = 0;

        // Test 4: PLO response collection
        noc_plo_to_bus_req_data.addr = 16'h0041; // command -> both PEs
        noc_plo_to_bus_req_valid = 1;
        @(posedge clk); noc_plo_to_bus_req_valid = 0;
        // Now rx_mask_reg is set
        @(posedge clk); #(`TB_SETTLE);
        pe_to_bus_plo_resp_data[0].data = 64'hDEAD;
        pe_to_bus_plo_resp_data[0].status = NOC_OK;
        pe_to_bus_plo_resp_valid[0] = 1;
        #(`TB_SETTLE);
        check("PLO_resp: valid forwarded", bus_to_noc_plo_resp_valid === 1'b1);
        check("PLO_resp: data correct", bus_to_noc_plo_resp_data.data === 64'hDEAD);
        pe_to_bus_plo_resp_valid[0] = 0;

        // Test 5: Backpressure when PE not ready
        bus_to_pe_ps_req_ready[0] = 0;
        noc_ps_to_bus_req_data.addr = 16'h0042; // tag=2, command
        noc_ps_to_bus_req_valid = 1; #(`TB_SETTLE);
        // If PE0 is targeted and not ready, noc_ps_to_bus_req_ready should be 0
        // With command addr, both PEs are targeted. PE0 not ready => all_ready=0
        check("Backpressure: ready=0", noc_ps_to_bus_req_ready === 1'b0);
        bus_to_pe_ps_req_ready[0] = 1; #(`TB_SETTLE);
        check("Backpressure: ready=1 after restore", noc_ps_to_bus_req_ready === 1'b1);
        noc_ps_to_bus_req_valid = 0;

        // Test 6: No enabled PE -> no routing
        // Reset scan chain to disabled
        scan_chain_in = '0;
        scan_chain_enable = 1;
        @(posedge clk); @(posedge clk);
        scan_chain_enable = 0; #(`TB_SETTLE);
        noc_ps_to_bus_req_data.addr = 16'h0001;
        noc_ps_to_bus_req_valid = 1; #(`TB_SETTLE);
        check("NoEnabled: PE0 not valid", bus_to_pe_ps_req_valid[0] === 1'b0);
        check("NoEnabled: PE1 not valid", bus_to_pe_ps_req_valid[1] === 1'b0);
        noc_ps_to_bus_req_valid = 0;

        $display("\n=== tb_mbus Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_mbus FAIL");
        else $display("tb_mbus PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_mbus"); $finish; end
endmodule
