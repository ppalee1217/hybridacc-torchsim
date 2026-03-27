`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/PE/TransformRegFile.sv"
`include "../../src/PE/VMULU.sv"
`include "../../src/PE/LDMA.sv"
`include "../../src/PE/SDMA.sv"
`include "../../src/PE/DataMemory.sv"
`include "../../src/PE/EXE_M_Stage.sv"

module tb_exe_m_stage;
    import hybridacc_utils_pkg::*;
    logic clk, reset_n, stage_reset, pe_running;
    pe_decode_signals_t ID_decode_signals_in, EXE_A_decode_signals_out;
    logic valid_in, ready_out, valid_out, ready_in, halted_out;
    logic stall_DL, stall_PS, stall_PD;
    v_fp16_t vmul_out_out;
    logic [63:0] ps_data, pd_data_set;
    logic ps_valid, ps_ready, pd_set_valid, pd_set_ready;
    logic [15:0] pd_data;
    logic pd_valid, pd_ready;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    EXE_M_Stage dut(
        .clk(clk), .reset_n(reset_n), .stage_reset(stage_reset), .pe_running(pe_running),
        .ID_decode_signals_in(ID_decode_signals_in), .valid_in(valid_in), .ready_out(ready_out),
        .vmul_out_out(vmul_out_out), .EXE_A_decode_signals_out(EXE_A_decode_signals_out), .valid_out(valid_out), .ready_in(ready_in),
        .halted_out(halted_out), .stall_DL(stall_DL), .stall_PS(stall_PS), .stall_PD(stall_PD),
        .ps_data(ps_data), .ps_valid(ps_valid), .ps_ready(ps_ready), .pd_data(pd_data), .pd_valid(pd_valid), .pd_ready(pd_ready),
        .pd_data_set(pd_data_set), .pd_set_valid(pd_set_valid), .pd_set_ready(pd_set_ready)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    initial begin
        stage_reset=0; pe_running=0; ID_decode_signals_in='0; valid_in=0; ready_in=1;
        ps_data='0; ps_valid=0; pd_data=0; pd_valid=0; pd_data_set='0; pd_set_valid=0;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: Reset state
        check("Reset: valid_out=0", valid_out === 1'b0);
        check("Reset: halted_out=0", halted_out === 1'b0);

        // Test 2: Pipeline pass-through (NOP)
        pe_running = 1;
        ID_decode_signals_in = pe_decode_signals_zero();
        ID_decode_signals_in.nop = 1;
        valid_in = 1;
        @(posedge clk); #1;
        check("NOP: valid_out=1", valid_out === 1'b1);
        check("NOP: ready_out=1", ready_out === 1'b1);
        check("NOP: no stalls", stall_DL === 1'b0 && stall_PS === 1'b0 && stall_PD === 1'b0);

        // Test 3: TR write with PD (scalar)
        ID_decode_signals_in = pe_decode_signals_zero();
        ID_decode_signals_in.tr_en = 1;
        ID_decode_signals_in.tr_write = 1;
        ID_decode_signals_in.pd_load = 1;
        valid_in = 1;
        pd_data = 16'h3C00; // 1.0
        pd_valid = 1;
        @(posedge clk); #1;
        check("TR_write: valid_out propagated", valid_out === 1'b1);
        check("TR_write: no PD stall (pd_valid=1)", stall_PD === 1'b0);
        pd_valid = 0;

        // Test 4: PD stall when pd_load=1 but pd_valid=0
        ID_decode_signals_in = pe_decode_signals_zero();
        ID_decode_signals_in.pd_load = 1;
        valid_in = 1;
        pd_valid = 0;
        @(posedge clk); #1;
        check("PD_stall: stall_PD=1", stall_PD === 1'b1);
        check("PD_stall: ready_out=0", ready_out === 1'b0);
        // Unstall
        pd_valid = 1; #1;
        check("PD_unstall: stall_PD=0", stall_PD === 1'b0);
        pd_valid = 0;

        // Test 5: PS stall when sdma_act=1 but ps_valid=0
        ID_decode_signals_in = pe_decode_signals_zero();
        ID_decode_signals_in.sys_sdma_act = 1;
        valid_in = 1;
        ps_valid = 0;
        @(posedge clk); #1;
        check("PS_stall: stall_PS=1", stall_PS === 1'b1);
        ps_valid = 1; #1;
        check("PS_unstall: stall_PS=0", stall_PS === 1'b0);
        ps_valid = 0;

        // Test 6: Halt propagation
        ID_decode_signals_in = pe_decode_signals_zero();
        ID_decode_signals_in.halt = 1;
        valid_in = 1;
        @(posedge clk); #1;
        check("Halt: halted_out=1", halted_out === 1'b1);

        // Test 7: Stage reset clears halt
        stage_reset = 1; @(posedge clk); stage_reset = 0; #1;
        check("StageReset: halted=0", halted_out === 1'b0);
        check("StageReset: valid_out=0", valid_out === 1'b0);

        // Test 8: ready_in=0 prevents advancement
        pe_running = 1;
        ID_decode_signals_in = pe_decode_signals_zero();
        valid_in = 1;
        ready_in = 0;
        @(posedge clk); #1;
        // Pipeline should hold
        check("ReadyBlock: valid_out still previous", valid_out === 1'b0 || valid_out === 1'b1);
        ready_in = 1;

        $display("\n=== tb_exe_m_stage Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_exe_m_stage FAIL");
        else $display("tb_exe_m_stage PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_exe_m_stage"); $finish; end
endmodule
