//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_psumregfile
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for psumregfile module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/PsumRegFile.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/hybridacc_utils_pkg.sv"
`endif
`ifndef GATE_SIM
`include "../../src/PE/PsumRegFile.sv"
`endif

module tb_psumregfile;
    import hybridacc_utils_pkg::*;

    logic clk, reset_n;
    logic enable, vpid_write_en, clear_regs, use_pcounter, clear_pcounter, incr_pcounter;
    logic [31:0] pid, mode;
    fp16_t p_in, p_out;
    v_fp16_t vp_in, vp_out;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    PsumRegFile dut(
        .clk(clk), .reset_n(reset_n), .enable(enable), .pid(pid), .p_in(p_in), .vp_in(vp_in),
        .vpid_write_en(vpid_write_en), .mode(mode), .p_out(p_out), .vp_out(vp_out),
        .clear_regs(clear_regs), .use_pcounter(use_pcounter), .clear_pcounter(clear_pcounter), .incr_pcounter(incr_pcounter)
    );

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/PsumRegFile/PsumRegFile.sdf", dut);
end
`endif

    // Sample after the falling edge so synchronous writes are visible in gate-level runs.
    fp16_t p_out_sampled;
    v_fp16_t vp_out_sampled;
    task automatic sample_outputs();
        begin
            #(`TB_SETTLE);
            p_out_sampled = p_out;
            vp_out_sampled = vp_out;
        end
    endtask

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;


    initial begin
        enable=1; vpid_write_en=0; clear_regs=0; use_pcounter=0; clear_pcounter=0; incr_pcounter=0;
        pid=0; mode=0; p_in=0; vp_in='0;
        @(posedge reset_n);
        @(posedge clk); @(negedge clk); sample_outputs();

        // Test 1: Reset state - all zeros
        pid = 0; mode = 0; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("Reset: p_out=0", p_out_sampled, 16'h0000)

        // Test 2: Scalar write and read (mode=0)
        pid = 3; mode = 0; p_in = 16'h2222;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("ScalarWrite: p_out=0x2222", p_out_sampled, 16'h2222)

        // Test 3: Different register doesn't corrupt
        pid = 5; mode = 0; p_in = 16'hBBBB;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("ScalarWrite2: pid5=0xBBBB", p_out_sampled, 16'hBBBB)
        pid = 3; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("NoCorrupt: pid3 still 0x2222", p_out_sampled, 16'h2222)

        // Test 4: Vector write (mode=1, pid<8 maps to p_regs[pid*4..pid*4+3])
        pid = 0; mode = 1;
        vp_in.lanes[0] = 16'hAA00;
        vp_in.lanes[1] = 16'hAA11;
        vp_in.lanes[2] = 16'hAA22;
        vp_in.lanes[3] = 16'hAA33;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0;
        repeat (4) begin
            @(posedge clk); @(negedge clk); sample_outputs();
            if ((vp_out_sampled.lanes[0] === 16'hAA00) &&
                (vp_out_sampled.lanes[1] === 16'hAA11) &&
                (vp_out_sampled.lanes[2] === 16'hAA22) &&
                (vp_out_sampled.lanes[3] === 16'hAA33)) begin
                break;
            end
        end
        `CHECK_VAL("VecWrite: vp_out[0]=0xAA00", vp_out_sampled.lanes[0], 16'hAA00)
        `CHECK_VAL("VecWrite: vp_out[1]=0xAA11", vp_out_sampled.lanes[1], 16'hAA11)
        `CHECK_VAL("VecWrite: vp_out[2]=0xAA22", vp_out_sampled.lanes[2], 16'hAA22)
        `CHECK_VAL("VecWrite: vp_out[3]=0xAA33", vp_out_sampled.lanes[3], 16'hAA33)

        // Test 5: Vector write to vp64 region (pid>=8)
        pid = 10; mode = 1;
        vp_in.lanes[0] = 16'h1111;
        vp_in.lanes[1] = 16'h2222;
        vp_in.lanes[2] = 16'h3333;
        vp_in.lanes[3] = 16'h4444;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0;
        repeat (4) begin
            @(posedge clk); @(negedge clk); sample_outputs();
            if ((vp_out_sampled.lanes[0] === 16'h1111) &&
                (vp_out_sampled.lanes[3] === 16'h4444)) begin
                break;
            end
        end
        `CHECK_VAL("VecVP64: vp_out[0]=0x1111", vp_out_sampled.lanes[0], 16'h1111)
        `CHECK_VAL("VecVP64: vp_out[3]=0x4444", vp_out_sampled.lanes[3], 16'h4444)

        // Test 6: Clear all registers
        clear_regs = 1; @(posedge clk); clear_regs = 0;
        pid = 3; mode = 0; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("Clear: pid3=0", p_out_sampled, 16'h0000)
        pid = 10; mode = 1; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("Clear: vp64=0", vp_out_sampled, '0)

        // Test 7: Pcounter auto-increment
        clear_pcounter = 1; @(posedge clk); clear_pcounter = 0;
        // Write to pid=0 via pcounter
        use_pcounter = 1; pid = 1; mode = 0; p_in = 16'hCC00;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0;
        // Increment pcounter by pid (=1)
        incr_pcounter = 1; @(posedge clk); incr_pcounter = 0;
        // Write to next pid via pcounter (now pcounter=1)
        p_in = 16'hCC01;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0;
        use_pcounter = 0;
        pid = 0; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("Pcounter: pid0=0xCC00", p_out_sampled, 16'hCC00)
        pid = 1; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("Pcounter: pid1=0xCC01", p_out_sampled, 16'hCC01)

        // Test 8: Boundary pid=31 (max scalar)
        use_pcounter = 0;
        pid = 31; mode = 0; p_in = 16'hFFFF;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("Boundary: pid31=0xFFFF", p_out_sampled, 16'hFFFF)

        // Test 9: Enable gating
        enable = 0; pid = 31; mode = 0; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("EnableOff: p_out=0", p_out_sampled, 16'h0000)
        enable = 1; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("EnableOn: p_out=0xFFFF", p_out_sampled, 16'hFFFF)

        // Test 10: Hybrid mode hazard - vector write aliases scalar regs
        // Write vector to pid=1 (mode=1): writes p_regs[4..7]
        clear_regs = 1; @(posedge clk); clear_regs = 0;
        pid = 1; mode = 1;
        vp_in.lanes[0] = 16'hDD00;
        vp_in.lanes[1] = 16'hDD11;
        vp_in.lanes[2] = 16'hDD22;
        vp_in.lanes[3] = 16'hDD33;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0;
        // Read back scalar at pid=4,5,6,7 (mode=0) to verify aliasing
        mode = 0;
        pid = 4; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("HybridAlias: p_regs[4]=0xDD00", p_out_sampled, 16'hDD00)
        pid = 5; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("HybridAlias: p_regs[5]=0xDD11", p_out_sampled, 16'hDD11)
        pid = 6; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("HybridAlias: p_regs[6]=0xDD22", p_out_sampled, 16'hDD22)
        pid = 7; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("HybridAlias: p_regs[7]=0xDD33", p_out_sampled, 16'hDD33)

        // Test 11: Pcounter overflow/wrap behavior
        clear_pcounter = 1; @(posedge clk); clear_pcounter = 0;
        use_pcounter = 1; pid = 31; // large increment
        incr_pcounter = 1; @(posedge clk); incr_pcounter = 0;
        // pcounter should now be 31
        mode = 0; p_in = 16'hEE00;
        vpid_write_en = 1; @(posedge clk); vpid_write_en = 0;
        // Write goes to pcounter index (31)
        use_pcounter = 0; pid = 31; @(posedge clk); @(negedge clk); sample_outputs();
        `CHECK_VAL("PcounterLarge: pid31=0xEE00", p_out_sampled, 16'hEE00)

        `TB_SUMMARY("tb_psumregfile")
        $finish;
    end

    initial begin #100000; $error("[TIMEOUT] tb_psumregfile"); $finish; end
endmodule
