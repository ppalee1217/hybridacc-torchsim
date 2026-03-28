//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_ldma
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for ldma module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/LDMA.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/PE/LDMA.sv"

module tb_ldma;
    import hybridacc_utils_pkg::*;
    logic clk, reset_n;
    logic [15:0] imm, mode, dm_read_addr;
    logic set_addr, set_len, set_loop, set_mode, active, next, reset_active;
    logic busy, done, dl_stall_out;
    logic [63:0] dm_read_data;
    v_fp16_t dmrv_out;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    LDMA dut(
        .clk(clk), .reset_n(reset_n), .imm(imm), .set_addr(set_addr), .set_len(set_len), .set_loop(set_loop), .set_mode(set_mode),
        .mode(mode), .active(active), .next(next), .reset_active(reset_active), .busy(busy), .done(done), .dl_stall_out(dl_stall_out),
        .dm_read_addr(dm_read_addr), .dm_read_data(dm_read_data), .dmrv_out(dmrv_out)
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
        imm=0; mode=0; set_addr=0; set_len=0; set_loop=0; set_mode=0; active=0; next=0; reset_active=0;
        dm_read_data = 64'h0;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: Reset state
        check("Reset: busy=0", busy === 1'b0);
        check("Reset: done=0", done === 1'b0);

        // Test 2: Configure and activate LDMA (LOAD_DWORD, len=2, loop=1)
        imm = 16'h0010; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0002; set_len = 1; @(posedge clk); set_len = 0;
        imm = 16'h0001; set_loop = 1; @(posedge clk); set_loop = 0;
        mode = 16'd3; imm = 16'd1; set_mode = 1; @(posedge clk); set_mode = 0; // LOAD_DWORD, stride=1
        #1;

        // Activate
        dm_read_data = 64'hAAAA_BBBB_CCCC_DDDD;
        active = 1; @(posedge clk); active = 0; #1;
        check("Activate: busy=1", busy === 1'b1);

        // Wait for LOAD_WAIT -> LOAD_PIPELINE
        @(posedge clk); #1;
        check("Pipeline: busy=1", busy === 1'b1);

        // First next signal - advance to second element
        dm_read_data = 64'h1111_2222_3333_4444;
        next = 1; @(posedge clk); #1;
        check("Next1: dmrv_out captured first data", dmrv_out !== '0);

        // Second next - should consume second element
        dm_read_data = 64'h5555_6666_7777_8888;
        @(posedge clk); #1;
        next = 0;

        // Wait for done
        repeat(4) @(posedge clk);
        #1;
        check("Done: done=1 or back to idle", done === 1'b1 || busy === 1'b0);

        // Test 3: Reset active
        reset_active = 1; @(posedge clk); reset_active = 0; #1;
        check("ResetActive: busy=0", busy === 1'b0);

        // Test 4: Zero length (should not go busy)
        imm = 16'h0020; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0000; set_len = 1; @(posedge clk); set_len = 0;
        active = 1; @(posedge clk); active = 0; #1;
        check("ZeroLen: stays idle", busy === 1'b0);

        // Test 5: Stall indication (busy without next)
        imm = 16'h0030; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0001; set_len = 1; @(posedge clk); set_len = 0;
        dm_read_data = 64'hFFFF_FFFF_FFFF_FFFF;
        active = 1; @(posedge clk); active = 0;
        @(posedge clk); #1; // in LOAD_PIPELINE, next=0
        next = 0;
        @(posedge clk); #1;
        check("Stall: dl_stall_out=1", dl_stall_out === 1'b1);
        // Unstall
        next = 1; @(posedge clk); next = 0;
        repeat(3) @(posedge clk); #1;
        check("Unstall: eventually done", done === 1'b1 || busy === 1'b0);

        // Test 6: Broadcast HALF mode (mode=5 => LOAD_HALF + broadcast)
        // Reset first
        reset_active = 1; @(posedge clk); reset_active = 0;
        imm = 16'h0040; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0001; set_len = 1; @(posedge clk); set_len = 0;
        imm = 16'h0001; set_loop = 1; @(posedge clk); set_loop = 0;
        mode = 16'd5; imm = 16'd1; set_mode = 1; @(posedge clk); set_mode = 0; // LOAD_HALF broadcast, stride=1
        dm_read_data = 64'h0000_0000_0000_ABCD; // lower 16 bits = 0xABCD
        active = 1; @(posedge clk); active = 0;
        @(posedge clk); #1; // LOAD_WAIT -> LOAD_PIPELINE
        next = 1; @(posedge clk); next = 0; #1;
        // broadcast: 0xABCD replicated to all 4 lanes
        check("Broadcast: lane0=0xABCD", dmrv_out.lanes[0] === 16'hABCD);
        check("Broadcast: lane1=0xABCD", dmrv_out.lanes[1] === 16'hABCD);
        check("Broadcast: lane2=0xABCD", dmrv_out.lanes[2] === 16'hABCD);
        check("Broadcast: lane3=0xABCD", dmrv_out.lanes[3] === 16'hABCD);
        repeat(3) @(posedge clk); #1;

        // Test 7: Multi-loop (loop=2, len=1 each)
        reset_active = 1; @(posedge clk); reset_active = 0;
        imm = 16'h0050; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0001; set_len = 1; @(posedge clk); set_len = 0;
        imm = 16'h0002; set_loop = 1; @(posedge clk); set_loop = 0;
        mode = 16'd3; imm = 16'd2; set_mode = 1; @(posedge clk); set_mode = 0; // LOAD_DWORD, stride=2
        dm_read_data = 64'h1111_2222_3333_4444;
        active = 1; @(posedge clk); active = 0;
        @(posedge clk); // LOAD_WAIT
        next = 1;
        @(posedge clk); #1; // First element consumed
        check("MultiLoop: busy during loop", busy === 1'b1);
        // Let the DMA cycle through loop iterations
        repeat(6) @(posedge clk);
        next = 0; #1;
        check("MultiLoop: eventually completes", done === 1'b1 || busy === 1'b0);

        $display("\n=== tb_ldma Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_ldma FAIL");
        else $display("tb_ldma PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_ldma"); $finish; end
endmodule
