//-----------------------------------------------------------------------------
// Description:   Basic unit test for DataSram hard-macro-backed wrapper.
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/Core/core_pkg.sv"
`include "../../src/Core/DataSram.sv"
`endif

module tb_datasram;
    import core_pkg::*;

    logic clk, reset_n;
    logic        mcu_dm_valid_i;
    logic        mcu_dm_write_i;
    logic [31:0] mcu_dm_addr_i;
    logic [31:0] mcu_dm_wdata_i;
    logic [3:0]  mcu_dm_wstrb_i;
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

    DataSram dut (
        .clk(clk),
        .reset_n(reset_n),
        .mcu_dm_valid_i(mcu_dm_valid_i),
        .mcu_dm_write_i(mcu_dm_write_i),
        .mcu_dm_addr_i(mcu_dm_addr_i),
        .mcu_dm_wdata_i(mcu_dm_wdata_i),
        .mcu_dm_wstrb_i(mcu_dm_wstrb_i),
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

    task automatic mcu_write(
        input logic [31:0] addr,
        input logic [31:0] data,
        input logic [3:0]  strb
    );
        begin
            @(negedge clk);
            mcu_dm_addr_i  = addr;
            mcu_dm_wdata_i = data;
            mcu_dm_wstrb_i = strb;
            mcu_dm_write_i = 1'b1;
            mcu_dm_valid_i = 1'b1;
            @(posedge clk);
            #(`TB_SETTLE);
            mcu_dm_valid_i = 1'b0;
            mcu_dm_write_i = 1'b0;
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
            mcu_dm_write_i = 1'b0;
            mcu_dm_valid_i = 1'b1;
            #(`TB_SETTLE);
            `CHECK_VAL(test_name, mcu_dm_rdata_o, expected)
            mcu_dm_valid_i = 1'b0;
        end
    endtask

    initial begin
        mcu_dm_valid_i = 1'b0;
        mcu_dm_write_i = 1'b0;
        mcu_dm_addr_i = '0;
        mcu_dm_wdata_i = '0;
        mcu_dm_wstrb_i = '0;
        loader_wr_valid_i = 1'b0;
        loader_wr_addr_i = '0;
        loader_wr_data_i = '0;
        loader_wr_strb_i = '0;
        load_phase_i = 1'b1;

        @(posedge reset_n);
        #(`TB_SETTLE);
        `CHECK_BIT("DataSram loader ready during load phase", loader_wr_ready_o, 1'b1)

        loader_write(32'h0000_0000, 32'h1122_3344, 4'hF);
        loader_write(32'h0000_0400, 32'h5566_7788, 4'hF);

        @(negedge clk);
        load_phase_i = 1'b0;
        #(`TB_SETTLE);
        `CHECK_BIT("DataSram loader ready outside load phase", loader_wr_ready_o, 1'b0)

        check_dm(32'h0000_0000, 32'h1122_3344, "DataSram loader word readback");
        check_dm(32'h0000_0400, 32'h5566_7788, "DataSram macro-boundary read");

        mcu_write(32'h0000_0001, 32'h0000_AA00, 4'b0010);
        check_dm(32'h0000_0000, 32'h1122_AA44, "DataSram byte write update");

        mcu_write(32'h0000_0002, 32'hCCDD_0000, 4'b1100);
        check_dm(32'h0000_0000, 32'hCCDD_AA44, "DataSram halfword write update");

        mcu_write(32'h0000_0004, 32'hA1B2_C3D4, 4'hF);
        check_dm(32'h0000_0004, 32'hA1B2_C3D4, "DataSram word write update");

        `TB_SUMMARY("tb_datasram")
        $finish;
    end

    initial begin
        #100000;
        $error("[TB_TIMEOUT] tb_datasram did not finish in time");
        `TB_SUMMARY("tb_datasram")
        $finish;
    end
endmodule