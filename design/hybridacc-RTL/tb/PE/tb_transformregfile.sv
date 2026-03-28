//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_transformregfile
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for transformregfile module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/TransformRegFile.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/PE/TransformRegFile.sv"

module tb_transformregfile;
    import hybridacc_utils_pkg::*;

    logic clk, reset_n;
    logic [31:0] enable, shift_mode, tid;
    logic shift_en, tid_write_en, vtid_write_en;
    fp16_t tid_in;
    v_fp16_t vtid_in, vtid_out;
    logic clear_regs, use_vcounter, clear_vcounter, incr_vcounter;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    TransformRegFile dut(
        .clk(clk), .reset_n(reset_n), .enable(enable), .shift_en(shift_en), .shift_mode(shift_mode),
        .tid(tid), .tid_in(tid_in), .tid_write_en(tid_write_en), .vtid_in(vtid_in), .vtid_write_en(vtid_write_en),
        .vtid_out(vtid_out), .clear_regs(clear_regs), .use_vcounter(use_vcounter),
        .clear_vcounter(clear_vcounter), .incr_vcounter(incr_vcounter)
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
        enable=1; shift_en=0; shift_mode=0; tid=0; tid_in=0; tid_write_en=0; vtid_in='0; vtid_write_en=0;
        clear_regs=0; use_vcounter=0; clear_vcounter=0; incr_vcounter=0;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: Reset state
        tid = 0; #1;
        check("Reset: vtid_out=0", vtid_out === '0);

        // Test 2: Scalar write to tid=0 (read via vtid_out which reads reg[0], reg[3], reg[6], reg[9])
        tid = 0; tid_in = 16'h1111; tid_write_en = 1;
        @(posedge clk); tid_write_en = 0; #1;
        check("ScalarWrite: vtid_out[0]=0x1111", vtid_out.lanes[0] === 16'h1111);

        // Test 3: Scalar write to tid=3 (maps to vtid_out.lanes[1] when reading from base=0)
        tid = 3; tid_in = 16'h3333; tid_write_en = 1;
        @(posedge clk); tid_write_en = 0;
        tid = 0; #1;
        check("ScalarWrite3: vtid_out[1]=0x3333", vtid_out.lanes[1] === 16'h3333);

        // Test 4: Vector write (vtid_write_en, writes to tid, tid+3, tid+6, tid+9)
        // For tid=0: writes reg[0]=lanes[0], reg[3]=lanes[1], reg[6]=lanes[2], reg[9]=lanes[3]
        // Need tid+9 < 12 => tid must be < 3, so tid=0 ok
        tid = 0;
        vtid_in.lanes[0] = 16'hAA00;
        vtid_in.lanes[1] = 16'hAA33;
        vtid_in.lanes[2] = 16'hAA66;
        vtid_in.lanes[3] = 16'hAA99;
        vtid_write_en = 1;
        @(posedge clk); vtid_write_en = 0; #1;
        check("VecWrite: vtid_out[0]=0xAA00", vtid_out.lanes[0] === 16'hAA00);
        check("VecWrite: vtid_out[1]=0xAA33", vtid_out.lanes[1] === 16'hAA33);
        check("VecWrite: vtid_out[2]=0xAA66", vtid_out.lanes[2] === 16'hAA66);
        check("VecWrite: vtid_out[3]=0xAA99", vtid_out.lanes[3] === 16'hAA99);

        // Test 5: Clear all registers
        clear_regs = 1; @(posedge clk); clear_regs = 0;
        tid = 0; #1;
        check("Clear: vtid_out=0", vtid_out === '0);

        // Test 6: Shift K3 mode
        // First populate registers: write values to reg[0..11]
        for (int i = 0; i < 12; i++) begin
            tid = i; tid_in = 16'h0100 + i; tid_write_en = 1;
            @(posedge clk);
        end
        tid_write_en = 0;
        // Shift K3: mask=0b0110_1101_1011
        // masked positions shift from [i+1], unmasked get 0
        shift_en = 1; shift_mode = 32'd0; // K3
        @(posedge clk); shift_en = 0;
        // After shift, read tid=0 to see reg[0,3,6,9]
        tid = 0; #1;
        // reg[0] mask[0]=1 => reg[0]=old reg[1]=0x0101
        check("ShiftK3: reg[0]=0x0101", vtid_out.lanes[0] === 16'h0101);

        // Test 7: Vcounter auto-increment
        clear_regs = 1; @(posedge clk); clear_regs = 0;
        clear_vcounter = 1; @(posedge clk); clear_vcounter = 0;
        // Write via scalar to positions 0,3,6,9
        for (int i = 0; i < 12; i++) begin
            tid = i; tid_in = 16'h0200 + i; tid_write_en = 1;
            @(posedge clk);
        end
        tid_write_en = 0;
        // Use vcounter for reading
        use_vcounter = 1; tid = 1;
        incr_vcounter = 1; @(posedge clk); incr_vcounter = 0;
        // vcounter should now be 1, read base=1 => reg[1,4,7,10]
        #1;
        check("Vcounter: vtid_out[0]=reg[1]=0x0201", vtid_out.lanes[0] === 16'h0201);
        use_vcounter = 0;

        // Test 8: Enable gating
        enable = 0; tid = 0; #1;
        check("EnableOff: vtid_out=0", vtid_out === '0);
        enable = 1; #1;
        check("EnableOn: vtid_out!=0", vtid_out.lanes[0] !== 16'h0000);

        // Test 9: Out-of-bounds tid (tid+9 >= 12 => vtid_out should be 0)
        tid = 5; #1; // 5+9=14 >= 12
        check("OOB: vtid_out=0", vtid_out === '0);

        // Test 10: Shift K5 mode (mask=12'b0011_1100_1111)
        // Repopulate registers
        clear_regs = 1; @(posedge clk); clear_regs = 0;
        for (int i = 0; i < 12; i++) begin
            tid = i; tid_in = 16'h0300 + i; tid_write_en = 1;
            @(posedge clk);
        end
        tid_write_en = 0;
        // K5 mask bits: [11:0] = 0011_1100_1111
        // bit0=1: reg[0] = old reg[1] (0x0301)
        // bit1=1: reg[1] = old reg[2] (0x0302)
        // bit2=1: reg[2] = old reg[3] (0x0303)
        // bit3=1: reg[3] = old reg[4] (0x0304)
        // bit4=0: reg[4] = 0
        // bit5=0: reg[5] = 0
        // bit6=1: reg[6] = old reg[7] (0x0307)
        // bit7=1: reg[7] = old reg[8] (0x0308)
        // bit8=0: reg[8] = 0
        // bit9=0: reg[9] = 0
        // bit10=1: reg[10] = old reg[11] (0x030B) ... wait bit10=1
        // Actually for K5: 12'b0011_1100_1111 = {bit[11]=0, bit[10]=0, bit[9]=1, bit[8]=1, bit[7]=1, bit[6]=1, bit[5]=0, bit[4]=0, bit[3]=1, bit[2]=1, bit[1]=1, bit[0]=1}
        shift_en = 1; shift_mode = 32'd1; // K5
        @(posedge clk); shift_en = 0;
        tid = 0; #1;
        // vtid_out reads reg[0,3,6,9]
        // reg[0]: mask[0]=1 -> old reg[1] = 0x0301
        check("ShiftK5: reg[0]=0x0301", vtid_out.lanes[0] === 16'h0301);
        // reg[3]: mask[3]=1 -> old reg[4] = 0x0304
        check("ShiftK5: reg[3]=0x0304", vtid_out.lanes[1] === 16'h0304);
        // reg[6]: mask[6]=1 -> old reg[7] = 0x0307
        check("ShiftK5: reg[6]=0x0307", vtid_out.lanes[2] === 16'h0307);
        // reg[9]: mask[9]=1 -> old reg[10] = 0x030A
        check("ShiftK5: reg[9]=0x030A", vtid_out.lanes[3] === 16'h030A);

        // Test 11: Shift K7 mode (mask=12'b0000_0011_1111)
        clear_regs = 1; @(posedge clk); clear_regs = 0;
        for (int i = 0; i < 12; i++) begin
            tid = i; tid_in = 16'h0400 + i; tid_write_en = 1;
            @(posedge clk);
        end
        tid_write_en = 0;
        // K7 mask: 12'b0000_0011_1111
        // bits[5:0]=1: reg[0..5] shift from [1..6]
        // bits[11:6]=0: reg[6..11] = 0
        shift_en = 1; shift_mode = 32'd2; // K7
        @(posedge clk); shift_en = 0;
        tid = 0; #1;
        // reg[0]: mask[0]=1 -> old reg[1] = 0x0401
        check("ShiftK7: reg[0]=0x0401", vtid_out.lanes[0] === 16'h0401);
        // reg[3]: mask[3]=1 -> old reg[4] = 0x0404
        check("ShiftK7: reg[3]=0x0404", vtid_out.lanes[1] === 16'h0404);
        // reg[6]: mask[6]=0 -> 0
        check("ShiftK7: reg[6]=0 (cleared)", vtid_out.lanes[2] === 16'h0000);
        // reg[9]: mask[9]=0 -> 0
        check("ShiftK7: reg[9]=0 (cleared)", vtid_out.lanes[3] === 16'h0000);

        $display("\n=== tb_transformregfile Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_transformregfile FAIL");
        else $display("tb_transformregfile PASS");
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_transformregfile"); $finish; end
endmodule
