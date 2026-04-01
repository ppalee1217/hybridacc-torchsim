//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_datamemory
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for datamemory module.
// Dependencies:  tb_common.svh, src/PE/DataMemory.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/PE/DataMemory.sv"
`endif

module tb_datamemory;
    logic clk, reset_n;
    logic bank_sel, dm_write_en;
    logic [15:0] dm_write_addr, dm_read_addr;
    logic [63:0] dm_write_data, dm_read_data;
    logic [7:0] dm_write_mask;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    DataMemory dut(
        .clk(clk), .reset_n(reset_n), .bank_sel(bank_sel),
        .dm_write_en(dm_write_en), .dm_write_addr(dm_write_addr), .dm_write_data(dm_write_data), .dm_write_mask(dm_write_mask),
        .dm_read_addr(dm_read_addr), .dm_read_data(dm_read_data)
    );

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/DataMemory/DataMemory.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;


    initial begin
        bank_sel=0; dm_write_en=0; dm_write_addr=0; dm_write_data=0; dm_write_mask=0; dm_read_addr=0;
        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // Test 1: Read after reset (all zero)
        dm_read_addr = 16'h0000;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Reset: read=0", dm_read_data, 64'h0)

        // Test 2: Write to bank0 (bank_sel=0 => write bank0, read bank1)
        // bank_sel=0: write_bank_idx=0(bank0), read_bank_idx=1(bank1)
        dm_write_en=1; dm_write_addr=16'h0010; dm_write_data=64'h1122_3344_5566_7788; dm_write_mask=8'hFF;
        @(posedge clk); dm_write_en=0; @(negedge clk);

        // Read from addr 0x10 with bank_sel=0 reads bank1 (which is still 0)
        dm_read_addr = 16'h0010;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("DualBank: read bank1 after write bank0 = 0", dm_read_data, 64'h0)

        // Test 3: Swap banks - now read from bank0 which has our data
        bank_sel = 1; // write_bank_idx=1, read_bank_idx=0
        dm_read_addr = 16'h0010;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("BankSwap: read bank0 = written data", dm_read_data, 64'h1122_3344_5566_7788)

        // Test 4: Byte mask write (partial write)
        bank_sel = 0; // write to bank0
        dm_write_en = 1;
        dm_write_addr = 16'h0020;
        dm_write_data = 64'hFF_FF_FF_FF_FF_FF_FF_FF;
        dm_write_mask = 8'b0000_1111; // only lower 4 bytes
        @(posedge clk); dm_write_en = 0;
        bank_sel = 1; // read from bank0
        dm_read_addr = 16'h0020;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("ByteMask: lower 4 bytes=FF, upper 4=0", dm_read_data, 64'h00000000_FFFFFFFF)

        // Test 5: Overwrite with different mask
        bank_sel = 1; // write to bank1
        dm_write_en = 1;
        dm_write_addr = 16'h0030;
        dm_write_data = 64'hAABBCCDD_11223344;
        dm_write_mask = 8'hFF;
        @(posedge clk); dm_write_en = 0;
        // Overwrite upper bytes only
        dm_write_data = 64'hEEFF0000_00000000;
        dm_write_mask = 8'b1100_0000; // byte[7:6] only
        dm_write_en = 1;
        dm_write_addr = 16'h0030;
        @(posedge clk); dm_write_en = 0;
        bank_sel = 0; // read from bank1
        dm_read_addr = 16'h0030;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("MaskOverwrite: selective bytes updated", dm_read_data, 64'hEEFF_CCDD_1122_3344)

        // Test 6: Address boundary (max address)
        bank_sel = 0;
        dm_write_en = 1;
        dm_write_addr = 16'h01F8; // near end of 512-byte space
        dm_write_data = 64'hDEADBEEF_CAFEBABE;
        dm_write_mask = 8'hFF;
        @(posedge clk); dm_write_en = 0;
        bank_sel = 1;
        dm_read_addr = 16'h01F8;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("AddrBoundary: near-end write/read", dm_read_data, 64'hDEADBEEF_CAFEBABE)

        // Test 7: Simultaneous read and write to different banks
        bank_sel = 0; // write bank0, read bank1
        dm_write_en = 1;
        dm_write_addr = 16'h0040;
        dm_write_data = 64'h1111_2222_3333_4444;
        dm_write_mask = 8'hFF;
        dm_read_addr = 16'h0040; // reads bank1 which should be 0
        @(posedge clk); dm_write_en = 0; @(negedge clk);
        `CHECK_VAL("SimRW: read other bank=0", dm_read_data, 64'h0)

        `TB_SUMMARY("tb_datamemory")
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_datamemory"); $finish; end
endmodule
