`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/NoC/NoCRouter.sv"

module tb_nocrouter;
    import hybridacc_utils_pkg::*;
    localparam int NP=2;
    localparam int PW=64;

    logic clk, reset_n, command_mode;
    logic [31:0] command_data;
    logic [NP*PW-1:0] noc_ps_in_data, noc_pd_in_data, noc_pli_in_data;
    logic noc_ps_in_valid, noc_ps_in_ready, noc_pd_in_valid, noc_pd_in_ready, noc_pli_in_valid, noc_pli_in_ready;
    noc_addr_req_t noc_plo_in_data; logic noc_plo_in_valid, noc_plo_in_ready;
    logic [NP*PW-1:0] noc_plo_out_data; NOC_RESPONSE_STATUS noc_plo_out_status; logic noc_plo_out_valid, noc_plo_out_ready;

    noc_request_t noc_ps_to_bus_req_data[NP]; logic noc_ps_to_bus_req_valid[NP], noc_ps_to_bus_req_ready[NP];
    noc_request_t noc_pd_to_bus_req_data[NP]; logic noc_pd_to_bus_req_valid[NP], noc_pd_to_bus_req_ready[NP];
    noc_request_t noc_pli_to_bus_req_data[NP]; logic noc_pli_to_bus_req_valid[NP], noc_pli_to_bus_req_ready[NP];
    noc_addr_req_t noc_plo_to_bus_req_data[NP]; logic noc_plo_to_bus_req_valid[NP], noc_plo_to_bus_req_ready[NP];
    noc_response_t bus_to_noc_plo_resp_data[NP]; logic bus_to_noc_plo_resp_valid[NP], bus_to_noc_plo_resp_ready[NP];
    logic scan_chain_enable; ScanChainFormat scan_chain_in[NP], scan_chain_out[NP];

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    NoCRouter #(.NUM_PORTS(NP), .PORT_WIDTH_BITS(PW)) dut(
        .clk(clk), .reset_n(reset_n), .command_mode(command_mode), .command_data(command_data),
        .noc_ps_in_data(noc_ps_in_data), .noc_ps_in_valid(noc_ps_in_valid), .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data), .noc_pd_in_valid(noc_pd_in_valid), .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data), .noc_pli_in_valid(noc_pli_in_valid), .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data), .noc_plo_in_valid(noc_plo_in_valid), .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data), .noc_plo_out_status(noc_plo_out_status), .noc_plo_out_valid(noc_plo_out_valid), .noc_plo_out_ready(noc_plo_out_ready),
        .noc_ps_to_bus_req_data(noc_ps_to_bus_req_data), .noc_ps_to_bus_req_valid(noc_ps_to_bus_req_valid), .noc_ps_to_bus_req_ready(noc_ps_to_bus_req_ready),
        .noc_pd_to_bus_req_data(noc_pd_to_bus_req_data), .noc_pd_to_bus_req_valid(noc_pd_to_bus_req_valid), .noc_pd_to_bus_req_ready(noc_pd_to_bus_req_ready),
        .noc_pli_to_bus_req_data(noc_pli_to_bus_req_data), .noc_pli_to_bus_req_valid(noc_pli_to_bus_req_valid), .noc_pli_to_bus_req_ready(noc_pli_to_bus_req_ready),
        .noc_plo_to_bus_req_data(noc_plo_to_bus_req_data), .noc_plo_to_bus_req_valid(noc_plo_to_bus_req_valid), .noc_plo_to_bus_req_ready(noc_plo_to_bus_req_ready),
        .bus_to_noc_plo_resp_data(bus_to_noc_plo_resp_data), .bus_to_noc_plo_resp_valid(bus_to_noc_plo_resp_valid), .bus_to_noc_plo_resp_ready(bus_to_noc_plo_resp_ready),
        .scan_chain_enable(scan_chain_enable), .scan_chain_in(scan_chain_in), .scan_chain_out(scan_chain_out)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    initial begin
        command_mode=0; command_data=0;
        noc_ps_in_data=0; noc_ps_in_valid=0;
        noc_pd_in_data=0; noc_pd_in_valid=0;
        noc_pli_in_data=0; noc_pli_in_valid=0;
        noc_plo_in_data='0; noc_plo_in_valid=0; noc_plo_out_ready=1;
        for (int i=0;i<NP;i++) begin
            noc_ps_to_bus_req_ready[i]=1; noc_pd_to_bus_req_ready[i]=1; noc_pli_to_bus_req_ready[i]=1; noc_plo_to_bus_req_ready[i]=1;
            bus_to_noc_plo_resp_data[i]='0; bus_to_noc_plo_resp_valid[i]=0; scan_chain_in[i]='0;
        end
        @(posedge reset_n);
        #1;

        // Test 1: All ingress channels ready after reset
        check("Ready: PS", noc_ps_in_ready === 1'b1);
        check("Ready: PD", noc_pd_in_ready === 1'b1);
        check("Ready: PLI", noc_pli_in_ready === 1'b1);
        check("Ready: PLO", noc_plo_in_ready === 1'b1);

        // Test 2: PS fanout to all ports
        noc_ps_in_data = {64'h1111, 64'h2222};
        noc_ps_in_valid = 1; #1;
        check("PS fanout: port0 valid", noc_ps_to_bus_req_valid[0] === 1'b1);
        check("PS fanout: port1 valid", noc_ps_to_bus_req_valid[1] === 1'b1);
        noc_ps_in_valid = 0;

        // Test 3: PD fanout
        noc_pd_in_data = {64'hAAAA, 64'hBBBB};
        noc_pd_in_valid = 1; #1;
        check("PD fanout: port0 valid", noc_pd_to_bus_req_valid[0] === 1'b1);
        check("PD fanout: port1 valid", noc_pd_to_bus_req_valid[1] === 1'b1);
        noc_pd_in_valid = 0;

        // Test 4: PLO fanout
        noc_plo_in_data.addr = 16'h1234;
        noc_plo_in_valid = 1; #1;
        check("PLO fanout: port0 valid", noc_plo_to_bus_req_valid[0] === 1'b1);
        check("PLO fanout: port0 addr", noc_plo_to_bus_req_data[0].addr === 16'h1234);
        noc_plo_in_valid = 0;

        // Test 5: PLO response aggregation
        bus_to_noc_plo_resp_data[0].data = 64'hDEAD_0000;
        bus_to_noc_plo_resp_data[0].status = NOC_OK;
        bus_to_noc_plo_resp_valid[0] = 1;
        bus_to_noc_plo_resp_data[1].data = 64'hBEEF_0000;
        bus_to_noc_plo_resp_data[1].status = NOC_OK;
        bus_to_noc_plo_resp_valid[1] = 1;
        #1;
        check("PLO resp: valid", noc_plo_out_valid === 1'b1);
        check("PLO resp: status OK", noc_plo_out_status === NOC_OK);
        check("PLO resp: port0 data", noc_plo_out_data[0*PW +: PW] === 64'hDEAD_0000);
        check("PLO resp: port1 data", noc_plo_out_data[1*PW +: PW] === 64'hBEEF_0000);
        bus_to_noc_plo_resp_valid[0] = 0; bus_to_noc_plo_resp_valid[1] = 0;

        // Test 6: PLO response with error status
        bus_to_noc_plo_resp_data[1].status = NOC_ERROR;
        bus_to_noc_plo_resp_valid[1] = 1;
        #1;
        check("PLO err: status=ERROR", noc_plo_out_status === NOC_ERROR);
        bus_to_noc_plo_resp_valid[1] = 0;

        // Test 7: Scan chain enable via command
        command_mode = 1;
        command_data = {28'h0, CMD_NOC_SCAN_CHAIN};
        #1;
        check("ScanChain: enable=1", scan_chain_enable === 1'b1);
        command_mode = 0; #1;
        check("ScanChain: enable=0", scan_chain_enable === 1'b0);

        // Test 8: No response => plo_out_valid=0
        for (int i=0;i<NP;i++) bus_to_noc_plo_resp_valid[i] = 0;
        #1;
        check("NoResp: plo_out_valid=0", noc_plo_out_valid === 1'b0);

        $display("\n=== tb_nocrouter Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_nocrouter FAIL");
        else $display("tb_nocrouter PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_nocrouter"); $finish; end
endmodule
