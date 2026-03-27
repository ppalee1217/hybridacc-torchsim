`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/PE/VADDU.sv"
`include "../../src/PE/PsumRegFile.sv"
`include "../../src/PE/EXE_A_Stage.sv"

module tb_exe_a_stage;
    import hybridacc_utils_pkg::*;

    logic clk, reset_n, stage_reset, pe_running;
    v_fp16_t vmul_out_in;
    pe_decode_signals_t EXE_M_decode_signals_in;
    logic valid_in, ready_out, halted_out, stall_port_pli, stall_port_plo;
    logic [63:0] pli_data, plo_data;
    logic pli_valid, pli_ready, plo_valid, plo_ready;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    EXE_A_Stage dut(
        .clk(clk), .reset_n(reset_n), .stage_reset(stage_reset), .pe_running(pe_running),
        .vmul_out_in(vmul_out_in), .EXE_M_decode_signals_in(EXE_M_decode_signals_in), .valid_in(valid_in),
        .ready_out(ready_out), .halted_out(halted_out), .stall_port_pli(stall_port_pli), .stall_port_plo(stall_port_plo),
        .pli_data(pli_data), .pli_valid(pli_valid), .pli_ready(pli_ready), .plo_data(plo_data), .plo_valid(plo_valid), .plo_ready(plo_ready)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    initial begin
        stage_reset=0; pe_running=0; vmul_out_in='0; EXE_M_decode_signals_in='0; valid_in=0;
        pli_data='0; pli_valid=0; plo_ready=1;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: Reset state
        check("Reset: halted_out=0", halted_out === 1'b0);
        check("Reset: stall_pli=0", stall_port_pli === 1'b0);
        check("Reset: stall_plo=0", stall_port_plo === 1'b0);

        // Test 2: Pipeline pass-through (NOP)
        pe_running = 1;
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        valid_in = 1;
        @(posedge clk); #1;
        check("NOP: ready_out=1", ready_out === 1'b1);
        check("NOP: no stalls", stall_port_pli === 1'b0 && stall_port_plo === 1'b0);

        // Test 3: VADDU + PR write
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.vaddu_en = 1;
        EXE_M_decode_signals_in.pr_write = 1;
        EXE_M_decode_signals_in.pr_mode = 1; // vector mode
        EXE_M_decode_signals_in.rid5 = 0;
        vmul_out_in.lanes[0] = 16'h3C00; // 1.0
        vmul_out_in.lanes[1] = 16'h4000; // 2.0
        vmul_out_in.lanes[2] = 16'h4200; // 3.0
        vmul_out_in.lanes[3] = 16'h4400; // 4.0
        valid_in = 1;
        @(posedge clk); #1;
        check("VADDU_PR: ready_out=1", ready_out === 1'b1);

        // Test 4: PLI/PLO operation stall when pli_valid=0
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.pli_plo_operation = 1;
        valid_in = 1;
        pli_valid = 0; plo_ready = 1;
        @(posedge clk); #1;
        check("PLI_stall: stall_port_pli=1", stall_port_pli === 1'b1);
        check("PLI_stall: ready_out=0", ready_out === 1'b0);
        // Unstall
        pli_valid = 1; #1;
        check("PLI_unstall: stall=0", stall_port_pli === 1'b0);

        // Test 5: PLO backpressure stall
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.pli_plo_operation = 1;
        valid_in = 1;
        pli_valid = 1; plo_ready = 0;
        @(posedge clk); #1;
        check("PLO_stall: stall_port_plo=1", stall_port_plo === 1'b1);
        plo_ready = 1; #1;
        check("PLO_unstall: stall=0", stall_port_plo === 1'b0);
        pli_valid = 0;

        // Test 6: PLO output data
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.pli_plo_operation = 1;
        vmul_out_in = '0;
        pli_data = 64'h0;
        pli_valid = 1; plo_ready = 1;
        valid_in = 1;
        @(posedge clk); #1;
        check("PLO_out: plo_valid=1", plo_valid === 1'b1);
        pli_valid = 0;

        // Test 7: Halt propagation
        // Clear any stall from previous test first
        pli_valid = 1; plo_ready = 1;
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.halt = 1;
        valid_in = 1;
        @(posedge clk); #1;
        // Need two cycles: first clears stall and latches halt, second applies halt
        @(posedge clk); #1;
        check("Halt: halted_out=1", halted_out === 1'b1);

        // Test 8: Stage reset
        stage_reset = 1; @(posedge clk); stage_reset = 0; #1;
        check("StageReset: halted=0", halted_out === 1'b0);

        $display("\n=== tb_exe_a_stage Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_exe_a_stage FAIL");
        else $display("tb_exe_a_stage PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_exe_a_stage"); $finish; end
endmodule
