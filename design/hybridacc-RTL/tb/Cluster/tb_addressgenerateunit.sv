//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_addressgenerateunit
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for addressgenerateunit module.
// Dependencies:  tb_common.svh, src/Cluster/AddressGenerateUnit.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/Cluster/AddressGenerateUnit.sv"

module tb_addressgenerateunit;
    localparam logic [7:0] REG_BASE_ADDR   = 8'h00;
    localparam logic [7:0] REG_ITER01      = 8'h08;
    localparam logic [7:0] REG_ITER23      = 8'h0C;
    localparam logic [7:0] REG_STRIDE0     = 8'h10;
    localparam logic [7:0] REG_STRIDE1     = 8'h14;
    localparam logic [7:0] REG_CTRL        = 8'h20;
    localparam logic [7:0] REG_STATUS      = 8'h24;
    localparam logic [7:0] REG_TAG_BASE    = 8'h40;
    localparam logic [7:0] REG_TAG_STRIDE0 = 8'h44;
    localparam logic [7:0] REG_TAG_STRIDE1 = 8'h48;
    localparam logic [7:0] REG_TAG_CTRL    = 8'h4C;
    localparam logic [7:0] REG_MASK_CFG    = 8'h54;

    logic clk;
    logic reset_n;
    logic cfg_write;
    logic [7:0] cfg_addr;
    logic [31:0] cfg_wdata;
    logic [31:0] cfg_rdata;
    logic start;
    logic stop;
    logic gen_valid;
    logic gen_ready;
    logic [31:0] gen_addr;
    logic [15:0] gen_tag;
    logic gen_ultra;
    logic [15:0] gen_mask;
    logic busy;
    logic done;
    logic [1:0] fsm_state;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    AddressGenerateUnit dut (
        .clk(clk),
        .reset_n(reset_n),
        .cfg_write(cfg_write),
        .cfg_addr(cfg_addr),
        .cfg_wdata(cfg_wdata),
        .cfg_rdata(cfg_rdata),
        .start(start),
        .stop(stop),
        .gen_valid(gen_valid),
        .gen_ready(gen_ready),
        .gen_addr(gen_addr),
        .gen_tag(gen_tag),
        .gen_ultra(gen_ultra),
        .gen_mask(gen_mask),
        .busy(busy),
        .done(done),
        .fsm_state(fsm_state)
    );

    task automatic mmio_write(input logic [7:0] addr, input logic [31:0] data);
        @(posedge clk);
        cfg_addr <= addr;
        cfg_wdata <= data;
        cfg_write <= 1'b1;
        @(posedge clk);
        cfg_write <= 1'b0;
        cfg_addr <= '0;
        cfg_wdata <= '0;
    endtask

    task automatic expect_descriptor(
        input logic [31:0] expected_addr,
        input logic [15:0] expected_tag,
        input logic [15:0] expected_mask,
        input logic expected_ultra
    );
        // Ensure we wait for a fresh gen_valid assertion (not a stale one)
        if (gen_valid === 1'b1) @(negedge gen_valid);
        wait (gen_valid === 1'b1);
        #(`TB_SETTLE);
        check($sformatf("AGU addr=0x%08h", expected_addr), gen_addr == expected_addr);
        check($sformatf("AGU tag=0x%04h", expected_tag), gen_tag == expected_tag);
        check($sformatf("AGU mask=0x%04h", expected_mask), gen_mask == expected_mask);
        check($sformatf("AGU ultra=%0b", expected_ultra), gen_ultra == expected_ultra);
        @(posedge clk);
    endtask

    task automatic configure_defaults;
        begin
            mmio_write(REG_BASE_ADDR, 32'h0);
            mmio_write(REG_ITER01, {16'd1, 16'd1});
            mmio_write(REG_ITER23, {16'd1, 16'd1});
            mmio_write(REG_STRIDE0, 32'h0);
            mmio_write(REG_STRIDE1, 32'h0);
            mmio_write(REG_TAG_BASE, 32'h0);
            mmio_write(REG_TAG_STRIDE0, 32'h1);
            mmio_write(REG_TAG_STRIDE1, 32'h1);
            mmio_write(REG_TAG_CTRL, 32'h0);
            mmio_write(REG_MASK_CFG, 32'h0000_000F);
        end
    endtask

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    initial begin
        cfg_write = 1'b0;
        cfg_addr = '0;
        cfg_wdata = '0;
        start = 1'b0;
        stop = 1'b0;
        gen_ready = 1'b1;

        @(posedge reset_n);

        configure_defaults();

        // Test 1: single descriptor with ultra and zero-normalized iter.
        mmio_write(REG_BASE_ADDR, 32'h0000_0100);
        mmio_write(REG_ITER01, 32'h0000_0000);
        mmio_write(REG_ITER23, 32'h0000_0000);
        mmio_write(REG_TAG_BASE, 32'h0000_0005);
        mmio_write(REG_MASK_CFG, 32'h0000_00AF);
        mmio_write(REG_CTRL, 32'h0000_0009);
        expect_descriptor(32'h0000_0100, 16'h0005, 16'h00AF, 1'b1);
        @(posedge clk); #(`TB_SETTLE);
        check("Test1: single descriptor done", done === 1'b1 || busy === 1'b0);

        // Test 2: nested address and tag sequence.
        configure_defaults();
        mmio_write(REG_BASE_ADDR, 32'h0000_0020);
        mmio_write(REG_ITER01, {16'd3, 16'd2});
        mmio_write(REG_STRIDE0, 32'd4);
        mmio_write(REG_STRIDE1, 32'd16);
        mmio_write(REG_TAG_BASE, 32'd1);
        mmio_write(REG_TAG_STRIDE1, 32'd3);
        mmio_write(REG_TAG_CTRL, 32'd1);
        mmio_write(REG_CTRL, 32'h0000_0001);

        expect_descriptor(32'h0000_0020, 16'd1, 16'h000F, 1'b0);
        expect_descriptor(32'h0000_0024, 16'd1, 16'h000F, 1'b0);
        expect_descriptor(32'h0000_0030, 16'd4, 16'h000F, 1'b0);
        expect_descriptor(32'h0000_0034, 16'd4, 16'h000F, 1'b0);
        expect_descriptor(32'h0000_0040, 16'd7, 16'h000F, 1'b0);
        expect_descriptor(32'h0000_0044, 16'd7, 16'h000F, 1'b0);
        wait (busy == 1'b0);

        // Test 3: backpressure keeps descriptor stable.
        configure_defaults();
        mmio_write(REG_BASE_ADDR, 32'h0000_0200);
        mmio_write(REG_ITER01, {16'd1, 16'd2});
        mmio_write(REG_STRIDE0, 32'd8);
        mmio_write(REG_CTRL, 32'h0000_0001);
        wait (gen_valid === 1'b1);
        gen_ready = 1'b0;
        repeat (3) begin
            @(posedge clk); #(`TB_SETTLE);
            check("Backpressure: addr held", gen_addr == 32'h0000_0200);
            check("Backpressure: valid held", gen_valid == 1'b1);
        end
        gen_ready = 1'b1;
        @(posedge clk);
        // Wait for the old valid to deassert, then the new one
        if (gen_valid === 1'b1) @(negedge gen_valid);
        wait (gen_valid === 1'b1); #(`TB_SETTLE);
        check("Backpressure: advanced after release", gen_addr == 32'h0000_0208);
        @(posedge clk);
        wait (busy == 1'b0);

        // Test 4: stop aborts an active run.
        configure_defaults();
        mmio_write(REG_ITER01, {16'd1, 16'd4});
        mmio_write(REG_CTRL, 32'h0000_0001);
        wait (gen_valid === 1'b1);
        @(posedge clk);
        stop <= 1'b1;
        @(posedge clk);
        stop <= 1'b0;
        @(posedge clk); #(`TB_SETTLE);
        check("Stop: busy=0", busy == 1'b0);
        check("Stop: valid=0", gen_valid == 1'b0);

        $display("\n=== tb_addressgenerateunit Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_addressgenerateunit FAIL");
        else $display("tb_addressgenerateunit PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_addressgenerateunit"); $finish; end
endmodule