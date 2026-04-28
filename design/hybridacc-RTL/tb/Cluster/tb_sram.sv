//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_sram
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Unit test for the fake simulation SRAM model.
//                Covers:
//                  * reset defaults
//                  * full-width write/read roundtrip
//                  * byte-mask write
//                  * response backpressure retention
//                  * pipeline full / request throttling
//                  * read-after-write
// Dependencies:  ../tb_common.svh, src/Cluster/SRAM.sv
// Revision:
//   2026/04/28 - Initial version restored for cluster SRAM coverage
// Additional Comments:
//   The first roundtrip matches the ESL test_sram_unit.cpp configuration.
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/Cluster/SRAM.sv"
`endif

module tb_sram;
    localparam int unsigned DATA_WIDTH_BITS = 256;
    localparam int unsigned ADDR_WIDTH      = 32;
    localparam int unsigned SIZE_BYTES      = 1024;
    localparam int unsigned LATENCY         = 3;
    localparam int unsigned PIPELINE_DEPTH  = 4;
    localparam logic [(DATA_WIDTH_BITS/8)-1:0] FULL_MASK = '1;

    logic clk, reset_n;
    logic [ADDR_WIDTH-1:0]      req_addr;
    logic                       req_valid;
    logic                       req_ready;
    logic [DATA_WIDTH_BITS-1:0] resp_data;
    logic                       resp_valid;
    logic                       resp_ready;
    logic                       write_en;
    logic [ADDR_WIDTH-1:0]      write_addr;
    logic [DATA_WIDTH_BITS-1:0] write_data;
    logic [(DATA_WIDTH_BITS/8)-1:0] write_mask;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset #(.CLK_PERIOD_NS(10)) clk_rst(
        .clk(clk),
        .reset_n(reset_n)
    );

    SRAM #(
        .DATA_WIDTH_BITS(DATA_WIDTH_BITS),
        .ADDR_WIDTH(ADDR_WIDTH),
        .SIZE_BYTES(SIZE_BYTES),
        .LATENCY(LATENCY),
        .PIPELINE_DEPTH(PIPELINE_DEPTH)
    ) dut (
        .clk(clk),
        .reset_n(reset_n),
        .req_addr(req_addr),
        .req_valid(req_valid),
        .req_ready(req_ready),
        .resp_data(resp_data),
        .resp_valid(resp_valid),
        .resp_ready(resp_ready),
        .write_en(write_en),
        .write_addr(write_addr),
        .write_data(write_data),
        .write_mask(write_mask)
    );

    function automatic logic [DATA_WIDTH_BITS-1:0] make_pattern(input logic [31:0] base_word);
        logic [DATA_WIDTH_BITS-1:0] value;
        begin
            value = '0;
            for (int idx = 0; idx < DATA_WIDTH_BITS/32; idx++) begin
                value[idx*32 +: 32] = base_word + idx;
            end
            return value;
        end
    endfunction

    task automatic issue_write(
        input logic [ADDR_WIDTH-1:0] addr,
        input logic [DATA_WIDTH_BITS-1:0] data,
        input logic [(DATA_WIDTH_BITS/8)-1:0] mask
    );
        begin
            @(negedge clk);
            write_en = 1'b1;
            write_addr = addr;
            write_data = data;
            write_mask = mask;
            @(posedge clk);
            @(negedge clk);
            write_en = 1'b0;
            write_mask = '0;
        end
    endtask

    task automatic issue_read(input logic [ADDR_WIDTH-1:0] addr);
        begin
            @(negedge clk);
            req_addr = addr;
            req_valid = 1'b1;
            while (!req_ready) @(posedge clk);
            @(posedge clk);
            @(negedge clk);
            req_valid = 1'b0;
        end
    endtask

    initial begin
        logic [DATA_WIDTH_BITS-1:0] pattern_a;
        logic [DATA_WIDTH_BITS-1:0] pattern_b;
        logic [DATA_WIDTH_BITS-1:0] expected;

        req_addr = '0;
        req_valid = 1'b0;
        resp_ready = 1'b0;
        write_en = 1'b0;
        write_addr = '0;
        write_data = '0;
        write_mask = '0;

        @(posedge reset_n);
        @(posedge clk);
        #(`TB_SETTLE);

        `CHECK_BIT("Reset req_ready high", req_ready, 1'b1)
        `CHECK_BIT("Reset resp_valid low", resp_valid, 1'b0)

        pattern_a = make_pattern(32'h0000_00A0);
        issue_write(32'd16, pattern_a, FULL_MASK);

        resp_ready = 1'b1;
        issue_read(32'd16);
        while (!resp_valid) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL("Full-width roundtrip", resp_data, pattern_a)
        @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_BIT("Response clears after handshake", resp_valid, 1'b0)

        pattern_b = pattern_a;
        pattern_b[31:0] = 32'hDEAD_BEEF;
        issue_write(32'd16, pattern_b, 32'h0000_000F);
        issue_read(32'd16);
        while (!resp_valid) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL("Byte-mask write", resp_data, pattern_b)
        @(posedge clk);

        pattern_a = make_pattern(32'h0100_0000);
        issue_write(32'd64, pattern_a, FULL_MASK);
        resp_ready = 1'b0;
        issue_read(32'd64);
        while (!resp_valid) @(posedge clk);
        #(`TB_SETTLE);
        expected = resp_data;
        `CHECK_VAL("Backpressure data stable cycle0", resp_data, pattern_a)
        repeat (2) begin
            @(posedge clk);
            #(`TB_SETTLE);
            `CHECK_BIT("Backpressure resp_valid held", resp_valid, 1'b1)
            `CHECK_VAL("Backpressure data held", resp_data, expected)
        end
        resp_ready = 1'b1;
        @(posedge clk);

        issue_write(32'd128, make_pattern(32'h1000_0000), FULL_MASK);
        issue_write(32'd160, make_pattern(32'h2000_0000), FULL_MASK);
        issue_write(32'd192, make_pattern(32'h3000_0000), FULL_MASK);
        issue_write(32'd224, make_pattern(32'h4000_0000), FULL_MASK);

        resp_ready = 1'b0;
        @(negedge clk);
        req_valid = 1'b1;
        req_addr = 32'd128;
        @(posedge clk);
        req_addr = 32'd160;
        @(posedge clk);
        req_addr = 32'd192;
        @(posedge clk);
        req_addr = 32'd224;
        @(posedge clk);
        req_addr = 32'd128;
        @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_BIT("Pipeline full deasserts req_ready", req_ready, 1'b0)
        @(negedge clk);
        req_valid = 1'b0;
        resp_ready = 1'b1;
        while (!resp_valid) @(posedge clk);
        @(posedge clk);
        while (!req_ready) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_BIT("Pipeline drains and req_ready returns", req_ready, 1'b1)

        pattern_a = make_pattern(32'h5000_0000);
        issue_write(32'd320, pattern_a, FULL_MASK);
        issue_read(32'd320);
        while (!resp_valid) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL("Read-after-write", resp_data, pattern_a)

        `TB_SUMMARY("tb_sram")
        $finish;
    end

    initial begin
        #500000;
        $error("[TB_TIMEOUT] tb_sram did not finish in time");
        `TB_SUMMARY("tb_sram")
        $finish;
    end

endmodule