`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/FIFO.sv"
`include "../../src/asyncFIFO.sv"
`include "../../src/PE/PErouter.sv"

module tb_perouter;
    import hybridacc_utils_pkg::*;

    logic clk, reset_n, enable;
    PERouterMode route_mode;
    noc_request_t noc_ps_req_data, noc_pd_req_data, noc_pli_req_data;
    noc_addr_req_t noc_plo_req_data;
    logic noc_ps_req_valid, noc_ps_req_ready, noc_pd_req_valid, noc_pd_req_ready, noc_pli_req_valid, noc_pli_req_ready, noc_plo_req_valid, noc_plo_req_ready;
    noc_response_t noc_plo_resp_data; logic noc_plo_resp_valid, noc_plo_resp_ready;
    logic [63:0] ln_pli_data, ln_plo_data, pe_ps_data, pe_pd_set_data, pe_pli_data, pe_plo_data;
    logic ln_pli_valid, ln_pli_ready, ln_plo_valid, ln_plo_ready;
    logic pe_reset, pe_start, pe_program, im_write_en;
    logic [15:0] im_write_addr, pe_pd_data; pe_inst_t im_write_data;
    logic pe_ps_valid, pe_ps_ready, pe_pd_valid, pe_pd_ready, pe_pd_set_valid, pe_pd_set_ready, pe_pli_valid, pe_pli_ready, pe_plo_valid, pe_plo_ready;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    PErouter dut(
        .clk(clk), .reset_n(reset_n), .enable(enable), .route_mode(route_mode),
        .noc_ps_req_data(noc_ps_req_data), .noc_ps_req_valid(noc_ps_req_valid), .noc_ps_req_ready(noc_ps_req_ready),
        .noc_pd_req_data(noc_pd_req_data), .noc_pd_req_valid(noc_pd_req_valid), .noc_pd_req_ready(noc_pd_req_ready),
        .noc_pli_req_data(noc_pli_req_data), .noc_pli_req_valid(noc_pli_req_valid), .noc_pli_req_ready(noc_pli_req_ready),
        .noc_plo_req_data(noc_plo_req_data), .noc_plo_req_valid(noc_plo_req_valid), .noc_plo_req_ready(noc_plo_req_ready),
        .noc_plo_resp_data(noc_plo_resp_data), .noc_plo_resp_valid(noc_plo_resp_valid), .noc_plo_resp_ready(noc_plo_resp_ready),
        .ln_pli_data(ln_pli_data), .ln_pli_valid(ln_pli_valid), .ln_pli_ready(ln_pli_ready), .ln_plo_data(ln_plo_data), .ln_plo_valid(ln_plo_valid), .ln_plo_ready(ln_plo_ready),
        .pe_reset(pe_reset), .pe_start(pe_start), .pe_program(pe_program), .im_write_en(im_write_en), .im_write_addr(im_write_addr), .im_write_data(im_write_data),
        .pe_ps_data(pe_ps_data), .pe_ps_valid(pe_ps_valid), .pe_ps_ready(pe_ps_ready), .pe_pd_data(pe_pd_data), .pe_pd_valid(pe_pd_valid), .pe_pd_ready(pe_pd_ready),
        .pe_pd_set_data(pe_pd_set_data), .pe_pd_set_valid(pe_pd_set_valid), .pe_pd_set_ready(pe_pd_set_ready), .pe_pli_data(pe_pli_data), .pe_pli_valid(pe_pli_valid), .pe_pli_ready(pe_pli_ready),
        .pe_plo_data(pe_plo_data), .pe_plo_valid(pe_plo_valid), .pe_plo_ready(pe_plo_ready)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    initial begin
        enable=1; route_mode=PLI_FROM_BUS_PLO_TO_BUS;
        noc_ps_req_data='0; noc_pd_req_data='0; noc_pli_req_data='0; noc_plo_req_data='0;
        noc_ps_req_valid=0; noc_pd_req_valid=0; noc_pli_req_valid=0; noc_plo_req_valid=0; noc_plo_resp_ready=1;
        ln_pli_data=0; ln_pli_valid=0; ln_plo_ready=1;
        pe_ps_ready=1; pe_pd_ready=1; pe_pd_set_ready=1; pe_pli_ready=1;
        pe_plo_data='0; pe_plo_valid=0;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: CMD_RESET
        noc_ps_req_data.addr = PE_CMD_ADDRESS;
        noc_ps_req_data.data = {60'h0, CMD_RESET};
        noc_ps_req_valid = 1; #1;
        check("CMD_RESET: pe_reset=1", pe_reset === 1'b1);
        check("CMD_RESET: pe_start=0", pe_start === 1'b0);
        noc_ps_req_valid = 0;

        // Test 2: CMD_START_PE
        @(posedge clk);
        noc_ps_req_data.addr = PE_CMD_ADDRESS;
        noc_ps_req_data.data = {60'h0, CMD_START_PE};
        noc_ps_req_valid = 1; #1;
        check("CMD_START: pe_start=1", pe_start === 1'b1);
        check("CMD_START: pe_reset=0", pe_reset === 1'b0);
        noc_ps_req_valid = 0;

        // Test 3: CMD_LOAD_PROGRAM
        @(posedge clk);
        noc_ps_req_data.addr = PE_CMD_ADDRESS;
        // im_write_addr = bits[15:4], im_write_data = bits[31:16]
        noc_ps_req_data.data = {32'h0, 16'hABCD, 12'h123, CMD_LOAD_PROGRAM};
        noc_ps_req_valid = 1; #1;
        check("LOAD_PROG: pe_program=1", pe_program === 1'b1);
        check("LOAD_PROG: im_write_en=1", im_write_en === 1'b1);
        check("LOAD_PROG: im_write_addr", im_write_addr === 16'h0123);
        check("LOAD_PROG: im_write_data", im_write_data === 16'hABCD);
        noc_ps_req_valid = 0;

        // Test 4: Non-command PS data goes to PS FIFO
        pe_ps_ready = 0; // Prevent immediate pop
        @(posedge clk);
        noc_ps_req_data.addr = 16'h0001; // Not PE_CMD_ADDRESS
        noc_ps_req_data.data = 64'hDEAD_BEEF_1234_5678;
        noc_ps_req_valid = 1; @(posedge clk); noc_ps_req_valid = 0;
        @(posedge clk); #1;
        check("PS FIFO: pe_ps_valid=1", pe_ps_valid === 1'b1);
        check("PS FIFO: data correct", pe_ps_data === 64'hDEAD_BEEF_1234_5678);
        // Pop it
        pe_ps_ready = 1; @(posedge clk); #1;

        // Test 5: PLO output in BUS mode
        route_mode = PLI_FROM_BUS_PLO_TO_BUS;
        noc_plo_resp_ready = 0; // Prevent immediate pop
        pe_plo_data = 64'hCAFE_BABE_0000_0001;
        pe_plo_valid = 1; @(posedge clk); pe_plo_valid = 0;
        @(posedge clk); #1;
        check("PLO BUS: noc_plo_resp_valid=1", noc_plo_resp_valid === 1'b1);
        check("PLO BUS: data correct", noc_plo_resp_data.data === 64'hCAFE_BABE_0000_0001);
        // Pop via resp_ready
        noc_plo_resp_ready = 1; @(posedge clk); #1;

        // Test 6: PLO output in LN mode
        route_mode = PLI_FROM_BUS_PLO_TO_LN;
        ln_plo_ready = 0; // Prevent immediate pop
        pe_plo_data = 64'h1111_2222_3333_4444;
        pe_plo_valid = 1; @(posedge clk); pe_plo_valid = 0;
        @(posedge clk); #1;
        check("PLO LN: ln_plo_valid=1", ln_plo_valid === 1'b1);
        check("PLO LN: data correct", ln_plo_data === 64'h1111_2222_3333_4444);
        ln_plo_ready = 1; @(posedge clk); #1;

        // Test 7: PLI from local network
        route_mode = PLI_FROM_LN_PLO_TO_LN;
        pe_pli_ready = 0; // Prevent immediate pop
        ln_pli_data = 64'hAAAA_BBBB_CCCC_DDDD;
        ln_pli_valid = 1; @(posedge clk); ln_pli_valid = 0;
        @(posedge clk); #1;
        check("PLI LN: pe_pli_valid=1", pe_pli_valid === 1'b1);
        check("PLI LN: data correct", pe_pli_data === 64'hAAAA_BBBB_CCCC_DDDD);
        pe_pli_ready = 1; @(posedge clk); #1;

        // Test 8: Enable=0 blocks all channels
        enable = 0; #1;
        check("Disable: noc_ps_req_ready=0", noc_ps_req_ready === 1'b0);
        check("Disable: noc_pd_req_ready=0", noc_pd_req_ready === 1'b0);
        enable = 1;

        $display("\n=== tb_perouter Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_perouter FAIL");
        else $display("tb_perouter PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_perouter"); $finish; end
endmodule
