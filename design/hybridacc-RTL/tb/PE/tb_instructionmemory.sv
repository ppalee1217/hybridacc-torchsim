//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_instructionmemory
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for instructionmemory module.
// Dependencies:  tb_common.svh, src/PE/InstructionMemory.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/PE/InstructionMemory.sv"

module tb_instructionmemory;
    logic clk, reset_n;
    logic im_write_en;
    logic [15:0] im_write_addr, im_write_data;
    logic [15:0] im_read_addr, im_read_data;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    InstructionMemory dut (
        .clk(clk), .reset_n(reset_n),
        .im_write_en(im_write_en), .im_write_addr(im_write_addr), .im_write_data(im_write_data),
        .im_read_addr(im_read_addr), .im_read_data(im_read_data)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin
            $error("[FAIL] %s", test_name);
            fail_count++;
        end else begin
            $display("[PASS] %s", test_name);
            pass_count++;
        end
    endtask

    initial begin
        im_write_en=0; im_write_addr=0; im_write_data=0; im_read_addr=0;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: Read after reset -> all zeros
        im_read_addr = 16'd0; #1;
        check("Reset: addr0=0", im_read_data === 16'h0000);
        im_read_addr = 16'd10; #1;
        check("Reset: addr10=0", im_read_data === 16'h0000);

        // Test 2: Write and read back
        im_write_addr = 16'd4;
        im_write_data = 16'h1234;
        im_write_en = 1;
        @(posedge clk); im_write_en = 0;
        im_read_addr = 16'd4; #1;
        check("Write/Read: addr4=0x1234", im_read_data === 16'h1234);

        // Test 3: Write to different address doesn't corrupt
        im_write_addr = 16'd6;
        im_write_data = 16'hABCD;
        im_write_en = 1;
        @(posedge clk); im_write_en = 0;
        im_read_addr = 16'd4; #1;
        check("NoCorrupt: addr4 still 0x1234", im_read_data === 16'h1234);
        im_read_addr = 16'd6; #1;
        check("Write2: addr6=0xABCD", im_read_data === 16'hABCD);

        // Test 4: Overwrite same address
        im_write_addr = 16'd4;
        im_write_data = 16'h5678;
        im_write_en = 1;
        @(posedge clk); im_write_en = 0;
        im_read_addr = 16'd4; #1;
        check("Overwrite: addr4=0x5678", im_read_data === 16'h5678);

        // Test 5: Boundary address (last word)
        // MEM_BYTES=512, DEPTH_WORDS=256, address uses [15:1], last word index=255 => addr=510
        im_write_addr = 16'd510;
        im_write_data = 16'hBEEF;
        im_write_en = 1;
        @(posedge clk); im_write_en = 0;
        im_read_addr = 16'd510; #1;
        check("Boundary: last word=0xBEEF", im_read_data === 16'hBEEF);

        // Test 6: Write-enable gating (write_en=0 should not write)
        im_write_addr = 16'd8;
        im_write_data = 16'hDEAD;
        im_write_en = 0;
        @(posedge clk);
        im_read_addr = 16'd8; #1;
        check("WriteGate: addr8 unchanged(0)", im_read_data === 16'h0000);

        // Test 7: Combinational read (no clock needed)
        im_read_addr = 16'd6; #1;
        check("CombRead: immediate=0xABCD", im_read_data === 16'hABCD);

        // Test 8: Sequential writes to multiple addresses
        for (int i = 0; i < 8; i++) begin
            im_write_addr = i * 2;
            im_write_data = 16'hA000 + i;
            im_write_en = 1;
            @(posedge clk);
        end
        im_write_en = 0;
        for (int i = 0; i < 8; i++) begin
            im_read_addr = i * 2; #1;
            check($sformatf("SeqWrite[%0d]", i), im_read_data === (16'hA000 + i));
        end

        $display("\n=== tb_instructionmemory Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_instructionmemory FAIL");
        else $display("tb_instructionmemory PASS");
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_instructionmemory"); $finish; end
endmodule
