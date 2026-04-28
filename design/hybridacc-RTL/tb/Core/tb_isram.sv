//-----------------------------------------------------------------------------
// Description:   Basic unit test for Isram hard-macro-backed wrapper.
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/Core/core_pkg.sv"
`include "../../src/Core/Isram.sv"
`endif

module tb_isram;
    import core_pkg::*;

    logic clk, reset_n;
    logic        mcu_im_valid_i;
    logic [31:0] mcu_im_addr_i;
    logic [31:0] mcu_im_rdata_o;
    logic        mcu_dm_valid_i;
    logic [31:0] mcu_dm_addr_i;
    logic [31:0] mcu_dm_rdata_o;
    logic        loader_wr_valid_i;
    logic [31:0] loader_wr_addr_i;
    logic [31:0] loader_wr_data_i;
    logic [3:0]  loader_wr_strb_i;
    logic        loader_wr_ready_o;
    logic        load_phase_i;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    Isram dut (
        .clk(clk),
        .reset_n(reset_n),
        .mcu_im_valid_i(mcu_im_valid_i),
        .mcu_im_addr_i(mcu_im_addr_i),
        .mcu_im_rdata_o(mcu_im_rdata_o),
        .mcu_dm_valid_i(mcu_dm_valid_i),
        .mcu_dm_addr_i(mcu_dm_addr_i),
        .mcu_dm_rdata_o(mcu_dm_rdata_o),
        .loader_wr_valid_i(loader_wr_valid_i),
        .loader_wr_addr_i(loader_wr_addr_i),
        .loader_wr_data_i(loader_wr_data_i),
        .loader_wr_strb_i(loader_wr_strb_i),
        .loader_wr_ready_o(loader_wr_ready_o),
        .load_phase_i(load_phase_i)
    );

    task automatic loader_write(
        input logic [31:0] addr,
        input logic [31:0] data,
        input logic [3:0]  strb
    );
        begin
            @(negedge clk);
            loader_wr_addr_i  = addr;
            loader_wr_data_i  = data;
            loader_wr_strb_i  = strb;
            loader_wr_valid_i = 1'b1;
            @(posedge clk);
            #(`TB_SETTLE);
            loader_wr_valid_i = 1'b0;
        end
    endtask

    task automatic check_im(
        input logic [31:0] addr,
        input logic [31:0] expected,
        input string       test_name
    );
        begin
            @(negedge clk);
            mcu_im_addr_i  = addr;
            mcu_im_valid_i = 1'b1;
            #(`TB_SETTLE);
            `CHECK_VAL(test_name, mcu_im_rdata_o, expected)
            mcu_im_valid_i = 1'b0;
        end
    endtask

    task automatic check_dm(
        input logic [31:0] addr,
        input logic [31:0] expected,
        input string       test_name
    );
        begin
            @(negedge clk);
            mcu_dm_addr_i  = addr;
            mcu_dm_valid_i = 1'b1;
            #(`TB_SETTLE);
            `CHECK_VAL(test_name, mcu_dm_rdata_o, expected)
            mcu_dm_valid_i = 1'b0;
        end
    endtask

    initial begin
        mcu_im_valid_i = 1'b0;
        mcu_im_addr_i = '0;
        mcu_dm_valid_i = 1'b0;
        mcu_dm_addr_i = '0;
        loader_wr_valid_i = 1'b0;
        loader_wr_addr_i = '0;
        loader_wr_data_i = '0;
        loader_wr_strb_i = '0;
        load_phase_i = 1'b1;

        @(posedge reset_n);
        #(`TB_SETTLE);
        `CHECK_BIT("Isram loader ready during load phase", loader_wr_ready_o, 1'b1)

        loader_write(32'h0000_0000, 32'h1122_3344, 4'hF);
        loader_write(32'h0000_0004, 32'hAABB_CCDD, 4'hF);
        loader_write(32'h0000_0008, 32'h0000_EEFF, 4'b0011);
        loader_write(32'h0000_0400, 32'h5566_7788, 4'hF);

        @(negedge clk);
        load_phase_i = 1'b0;
        #(`TB_SETTLE);
        `CHECK_BIT("Isram loader ready outside load phase", loader_wr_ready_o, 1'b0)

        check_im(32'h0000_0000, 32'h1122_3344, "Isram instruction read word0");
        check_im(32'h0000_0004, 32'hAABB_CCDD, "Isram instruction read word1");
        check_dm(32'h0000_0008, 32'h0000_EEFF, "Isram data read partial word");
        check_dm(32'h0000_0400, 32'h5566_7788, "Isram macro-boundary read");

        `TB_SUMMARY("tb_isram")
        $finish;
    end

    initial begin
        #100000;
        $error("[TB_TIMEOUT] tb_isram did not finish in time");
        `TB_SUMMARY("tb_isram")
        $finish;
    end
endmodule