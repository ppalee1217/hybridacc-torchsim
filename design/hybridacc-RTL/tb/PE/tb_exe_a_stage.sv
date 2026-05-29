//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_exe_a_stage
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for exe_a_stage module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/VADDU.sv, src/PE/PsumRegFile.sv, src/PE/EXE_A_Stage.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/PE/VADDU.sv"
`include "../../src/PE/PsumRegFile.sv"
`include "../../src/PE/EXE_A_Stage.sv"
`endif

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

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/EXE_A_Stage/EXE_A_Stage.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    initial begin
        stage_reset=0; pe_running=0; vmul_out_in='0; EXE_M_decode_signals_in='0; valid_in=0;
        pli_data='0; pli_valid=0; plo_ready=1;
        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // Test 1: Reset state
        `CHECK_BIT("Reset: halted_out=0", halted_out, 1'b0)
        `CHECK_BIT("Reset: stall_pli=0", stall_port_pli, 1'b0)
        `CHECK_BIT("Reset: stall_plo=0", stall_port_plo, 1'b0)

        // Test 2: Pipeline pass-through (NOP) — state: S_IDLE → S_IDLE
        pe_running = 1;
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        valid_in = 1;
        @(posedge clk); @(negedge clk);
        `CHECK_BIT("NOP: ready_out=1", ready_out, 1'b1)
        `CHECK_BIT("NOP: stall_pli=0", stall_port_pli, 1'b0)
        `CHECK_BIT("NOP: stall_plo=0", stall_port_plo, 1'b0)

        // Test 3: VADDU + PR write (vector mode, vaddu_mode=1)
        // State: S_IDLE → S_NORMAL_MODE
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.vaddu_en = 1;
        EXE_M_decode_signals_in.vaddu_mode = 32'd1; // vector mode (not VMAC)
        EXE_M_decode_signals_in.pr_write = 1;
        EXE_M_decode_signals_in.pr_mode = 1;
        EXE_M_decode_signals_in.rid5 = 0;
        vmul_out_in.lanes[0] = 16'h3C00;
        vmul_out_in.lanes[1] = 16'h4000;
        vmul_out_in.lanes[2] = 16'h4200;
        vmul_out_in.lanes[3] = 16'h4400;
        valid_in = 1;
        @(posedge clk); @(negedge clk);
        `CHECK_BIT("VADDU_PR: ready_out=1", ready_out, 1'b1)

        // Reset FSM cleanly before Test 4
        stage_reset = 1; @(posedge clk); stage_reset = 0; @(posedge clk); @(negedge clk);

        // Test 4: PLI stall — pli_plo_operation with pli_valid=0
        // State: S_IDLE → S_WAIT_PLI (stall_port_pli=1)
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.pli_plo_operation = 1;
        valid_in = 1;
        pli_valid = 0; plo_ready = 1;
        @(posedge clk); @(negedge clk);
        `CHECK_BIT("PLI_stall: stall_port_pli=1", stall_port_pli, 1'b1)
        `CHECK_BIT("PLI_stall: ready_out=0", ready_out, 1'b0)

        // Unstall: provide pli_valid, wait for FSM to leave S_WAIT_PLI
        pli_valid = 1; valid_in = 0;
        @(posedge clk); @(negedge clk);  // S_WAIT_PLI → S_EXEC_PLI_VADDU
        `CHECK_BIT("PLI_unstall: stall=0", stall_port_pli, 1'b0)

        // Reset FSM and drain PLO buffer before Test 5
        pli_valid = 0;
        stage_reset = 1; @(posedge clk); stage_reset = 0; @(posedge clk); @(negedge clk);

        // Test 5: PLO backpressure — need to fill PLO buffer then test stall
        // Step 1: Start PLI/PLO op to fill PLO buffer
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.pli_plo_operation = 1;
        valid_in = 1; pli_valid = 1; plo_ready = 1;
        @(posedge clk); @(negedge clk);  // S_IDLE → S_EXEC_PLI_VADDU (cycle A)
        // Step 2: Keep another PLI/PLO op flowing to stay in S_EXEC_PLI_VADDU
        @(posedge clk); @(negedge clk);  // S_EXEC_PLI_VADDU: plo_buf fills; accepts next → S_EXEC_PLI_VADDU (cycle B)
        // Step 3: Now plo_buf_valid_reg=1; set plo_ready=0 to block drain
        plo_ready = 0;
        @(posedge clk); @(negedge clk);  // S_EXEC_PLI_VADDU: buffer full + !plo_ready → S_WAIT_PLO
        `CHECK_BIT("PLO_stall: stall_port_plo=1", stall_port_plo, 1'b1)

        // Unstall: allow PLO drain
        plo_ready = 1; valid_in = 0; pli_valid = 0;
        @(posedge clk); @(negedge clk);  // S_WAIT_PLO drain phase
        `CHECK_BIT("PLO_unstall_drain: stall=1", stall_port_plo, 1'b1)
        @(posedge clk); @(negedge clk);  // S_WAIT_PLO refill phase → S_IDLE
        `CHECK_BIT("PLO_unstall_refill: stall=0", stall_port_plo, 1'b0)

        // Reset FSM before Test 6
        stage_reset = 1; @(posedge clk); stage_reset = 0; @(posedge clk); @(negedge clk);

        // Test 6: PLO output data — verify plo_valid after PLI/PLO operation
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.pli_plo_operation = 1;
        vmul_out_in = '0;
        pli_data = 64'h0;
        pli_valid = 1; plo_ready = 1;
        valid_in = 1;
        @(posedge clk); @(negedge clk);  // S_IDLE → S_EXEC_PLI_VADDU (PLO buf valid_next=1)
        // Need one more cycle for PLO buffer register update
        valid_in = 0;
        @(posedge clk); @(negedge clk);  // plo_buf_valid_reg now 1
        `CHECK_BIT("PLO_out: plo_valid=1", plo_valid, 1'b1)
        pli_valid = 0;

        // Reset before Test 7
        stage_reset = 1; @(posedge clk); stage_reset = 0; @(posedge clk); @(negedge clk);

        // Test 7: Halt propagation
        EXE_M_decode_signals_in = pe_decode_signals_zero();
        EXE_M_decode_signals_in.halt = 1;
        valid_in = 1;
        pli_valid = 0; plo_ready = 1;
        @(posedge clk); @(negedge clk);  // S_IDLE: halt → halted_reg=1
        `CHECK_BIT("Halt: halted_out=1", halted_out, 1'b1)

        // Test 8: Stage reset clears halt
        stage_reset = 1; @(posedge clk); stage_reset = 0; @(negedge clk);
        `CHECK_BIT("StageReset: halted=0", halted_out, 1'b0)

        `TB_SUMMARY("tb_exe_a_stage")
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_exe_a_stage"); $finish; end
endmodule
