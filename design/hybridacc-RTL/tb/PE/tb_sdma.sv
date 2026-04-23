//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_sdma
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for sdma module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/SDMA.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/PE/SDMA.sv"
`endif

module tb_sdma;
    import hybridacc_utils_pkg::*;
    logic clk, reset_n;
    logic [15:0] imm;
    logic set_addr, set_len, set_loop, set_mode, swap_in, active, reset_active;
    logic busy, done, dl_stall_out, bank_sel;
    v_fp16_t ps_data;
    logic ps_valid, ps_ready;
    logic dm_write_en;
    logic [15:0] dm_write_addr;
    logic [63:0] dm_write_data;
    logic [7:0] dm_write_mask;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    SDMA dut(
        .clk(clk), .reset_n(reset_n), .imm(imm), .set_addr(set_addr), .set_len(set_len), .set_loop(set_loop), .set_mode(set_mode),
        .swap_in(swap_in), .active(active), .reset_active(reset_active), .busy(busy), .done(done), .dl_stall_out(dl_stall_out), .bank_sel(bank_sel),
        .ps_data(ps_data), .ps_valid(ps_valid), .ps_ready(ps_ready), .dm_write_en(dm_write_en), .dm_write_addr(dm_write_addr), .dm_write_data(dm_write_data), .dm_write_mask(dm_write_mask)
    );

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/SDMA/SDMA.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;


    initial begin
        imm=0; set_addr=0; set_len=0; set_loop=0; set_mode=0; swap_in=0; active=0; reset_active=0;
        ps_data='0; ps_valid=0;
        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // Test 1: Reset state
        `CHECK_BIT("Reset: busy=0", busy, 1'b0)
        `CHECK_BIT("Reset: done=0", done, 1'b0)
        `CHECK_BIT("Reset: bank_sel=0", bank_sel, 1'b0)

        // Test 2: Configure SDMA (addr=0x10, len=1 element (set_len imm=0 => len_static=1), loop=1)
        imm = 16'h0010; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0000; set_len = 1; @(posedge clk); set_len = 0; // len_static = 0+1 = 1
        imm = 16'h0000; set_loop = 1; @(posedge clk); set_loop = 0; // loop_static = 0+1 = 1
        imm = 16'h0001; set_mode = 1; @(posedge clk); set_mode = 0; // stride=1

        // Activate + Provide data
        active = 1; @(posedge clk); active = 0;
        ps_data.lanes[0] = 16'h1111;
        ps_data.lanes[1] = 16'h2222;
        ps_data.lanes[2] = 16'h3333;
        ps_data.lanes[3] = 16'h4444;
        ps_valid = 1;
        @(negedge clk);
        `CHECK_BIT("Activate: busy=1", busy, 1'b1)
        `CHECK_BIT("Activate: ps_ready=1", ps_ready, 1'b1)
        `CHECK_BIT("Activate: dm_write_en=0 before capture", dm_write_en, 1'b0)
        @(posedge clk);
        ps_valid = 0;
        @(negedge clk);
        `CHECK_BIT("Write: dm_write_en=1", dm_write_en, 1'b1)
        `CHECK_VAL("Write: dm_write_addr=0x10", dm_write_addr, 16'h0010)
        `CHECK_VAL("Write: dm_write_data packed", dm_write_data, 64'h4444_3333_2222_1111)
        `CHECK_VAL("Write: dm_write_mask=FF", dm_write_mask, 8'hFF)

        // Should move to WAIT_SWAP after the buffered write is issued
        @(posedge clk); @(negedge clk);
        `CHECK_COND("AfterWrite: waiting for swap", busy === 1'b0 || ps_ready === 1'b0, {busy, ps_ready})

        // Issue swap
        swap_in = 1; @(posedge clk); swap_in = 0; @(negedge clk);

        // Wait for FINISH -> IDLE
        repeat(2) @(posedge clk); @(negedge clk);
        `CHECK_COND("PostSwap: done=1 or idle", done === 1'b1 || busy === 1'b0, {done, busy})

        // Test 3: Bank swap tracking
        // swap_in toggles bank_sel in IDLE
        // After Test 2's WAIT_SWAP→swap, bank_sel was toggled to 1. Now toggling again → 0.
        begin
            logic prev_bs;
            prev_bs = bank_sel;
            swap_in = 1; @(posedge clk); swap_in = 0; @(negedge clk);
            `CHECK_COND("BankSwapIdle: bank_sel toggled", bank_sel !== prev_bs, bank_sel)
        end

        // Test 4: Zero length active (should go to WAIT_SWAP immediately)
        imm = 16'h0020; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'hFFFF; set_len = 1; @(posedge clk); set_len = 0; // len_static=0 (wraps)
        // Actually SDMA does set_len as imm+1, so 0xFFFF+1=0x0000 => len_static=0
        // This means zero-length, should go directly to WAIT_SWAP
        active = 1; @(posedge clk); active = 0; @(negedge clk);
        // With len_static=0, should have dm_write empty
        `CHECK_BIT("ZeroLen: no write_en", dm_write_en, 1'b0)

        // Cleanup: swap to transition out
        swap_in = 1; @(posedge clk); swap_in = 0;
        repeat(3) @(posedge clk); @(negedge clk);

        // Test 5: Reset active
        imm = 16'h0030; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0003; set_len = 1; @(posedge clk); set_len = 0;
        active = 1; @(posedge clk); active = 0;
        ps_valid = 1; @(posedge clk); ps_valid = 0;
        reset_active = 1; @(posedge clk); reset_active = 0; @(negedge clk);
        `CHECK_BIT("ResetActive: busy=0", busy, 1'b0)

        // Test 6: Data not valid (ps_valid=0) -> SDMA waits
        imm = 16'h0040; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0000; set_len = 1; @(posedge clk); set_len = 0;
        imm = 16'h0000; set_loop = 1; @(posedge clk); set_loop = 0;
        active = 1; @(posedge clk); active = 0;
        ps_valid = 0;
        @(posedge clk); @(negedge clk);
        `CHECK_BIT("NoData: dm_write_en=0", dm_write_en, 1'b0)
        `CHECK_BIT("NoData: busy=1 (waiting)", busy, 1'b1)
        // Now provide data and observe the buffered write on the next cycle.
        @(posedge clk);
        ps_valid = 1;
        @(posedge clk);
        ps_valid = 0;
        @(negedge clk);
        `CHECK_BIT("DataArrives: dm_write_en=1", dm_write_en, 1'b1)

        // Cleanup
        swap_in = 1; @(posedge clk); swap_in = 0;
        repeat(3) @(posedge clk); @(negedge clk);

        // Test 7: Multi-loop with bank ping-pong (loop=2, len=1)
        reset_active = 1; @(posedge clk); reset_active = 0;
        imm = 16'h0050; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0000; set_len = 1; @(posedge clk); set_len = 0; // len_static=1
        imm = 16'h0001; set_loop = 1; @(posedge clk); set_loop = 0; // loop_static=2
        imm = 16'h0001; set_mode = 1; @(posedge clk); set_mode = 0; // stride=1
        // Activate for first loop iteration
        active = 1; @(posedge clk); active = 0;
        ps_data.lanes[0] = 16'hAA00;
        ps_valid = 1;
        @(posedge clk);
        ps_valid = 0;
        @(negedge clk);
        `CHECK_BIT("MultiLoop1: dm_write_en=1", dm_write_en, 1'b1)
        // Should need swap to continue to next loop iteration
        @(posedge clk); @(negedge clk);
        // Issue swap for the bank → starts second loop iteration
        swap_in = 1; @(posedge clk); swap_in = 0;
        @(posedge clk);
        // Right after posedge: state=RUN for 2nd loop. Capture data on the next edge.
        ps_data.lanes[0] = 16'hBB00;
        ps_valid = 1;
        @(posedge clk);
        ps_valid = 0;
        @(negedge clk);
        `CHECK_BIT("MultiLoop2: dm_write_en=1", dm_write_en, 1'b1)
        // Second loop done → WAIT_SWAP, issue final swap → FINISH
        @(posedge clk); @(negedge clk);
        swap_in = 1; @(posedge clk); swap_in = 0;
        repeat(2) @(posedge clk); @(negedge clk);
        `CHECK_COND("MultiLoop: completes after swap", done === 1'b1 || busy === 1'b0, {done, busy})

        // Test 8: Stride pattern (len=2, stride=2 => addresses skip by 2*8=16)
        reset_active = 1; @(posedge clk); reset_active = 0;
        imm = 16'h0060; set_addr = 1; @(posedge clk); set_addr = 0;
        imm = 16'h0001; set_len = 1; @(posedge clk); set_len = 0; // len_static=2
        imm = 16'h0000; set_loop = 1; @(posedge clk); set_loop = 0; // loop_static=1
        imm = 16'h0002; set_mode = 1; @(posedge clk); set_mode = 0; // stride=2
        active = 1; @(posedge clk); active = 0;
        ps_data.lanes[0] = 16'hBB00;
        ps_valid = 1;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Stride: first addr=0x0060", dm_write_addr, 16'h0060)
        @(posedge clk); @(negedge clk);
        // Second element: addr should be 0x0060 + stride*2 = 0x0060 + 4 = 0x0064
        `CHECK_VAL("Stride: second addr=0x0064", dm_write_addr, 16'h0064)
        @(posedge clk);
        ps_valid = 0;

        `TB_SUMMARY("tb_sdma")
        $finish;
    end

    initial begin #500000; $error("[TIMEOUT] tb_sdma"); $finish; end
endmodule
