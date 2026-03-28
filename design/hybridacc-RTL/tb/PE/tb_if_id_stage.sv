//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_if_id_stage
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for if_id_stage module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/InstructionMemory.sv, src/PE/Decoder.sv, src/PE/LoopController.sv, src/PE/IF_ID_Stage.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/PE/InstructionMemory.sv"
`include "../../src/PE/Decoder.sv"
`include "../../src/PE/LoopController.sv"
`include "../../src/PE/IF_ID_Stage.sv"

module tb_if_id_stage;
    import hybridacc_utils_pkg::*;
    logic clk, reset_n;
    pe_decode_signals_t ID_decode_signals_out;
    logic valid_out;
    logic [15:0] pc_out;
    logic halted_out;
    logic stage_reset, pe_running, ready_in;
    logic [15:0] pc_init_value;
    logic im_write_en;
    logic [15:0] im_write_addr;
    pe_inst_t im_write_data;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    IF_ID_Stage dut(
        .clk(clk), .reset_n(reset_n), .ID_decode_signals_out(ID_decode_signals_out), .valid_out(valid_out), .pc_out(pc_out), .halted_out(halted_out),
        .stage_reset(stage_reset), .pe_running(pe_running), .ready_in(ready_in), .pc_init_value(pc_init_value),
        .im_write_en(im_write_en), .im_write_addr(im_write_addr), .im_write_data(im_write_data)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string test_name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", test_name); fail_count++; end
        else begin $display("[PASS] %s", test_name); pass_count++; end
    endtask

    initial begin
        logic [15:0] stalled_pc;
        stage_reset=0; pe_running=0; ready_in=1; pc_init_value=0; im_write_en=0; im_write_addr=0; im_write_data=0;
        @(posedge reset_n);
        @(posedge clk); #1;

        // Test 1: Reset state
        check("Reset: valid_out=0", valid_out === 1'b0);
        check("Reset: halted_out=0", halted_out === 1'b0);
        check("Reset: pc_out=0", pc_out === 16'h0000);

        // Test 2: Write program into IM
        // NOP encoding: opcode=2(10), funct2=2(10), func1=0, payload=0 => 16'b0000000000_0_10_10_0 = 0x0014
        // HALT encoding: opcode=2(10), funct2=3(11), func1=0, payload=0 => 16'b0000000000_0_11_10_0 = 0x001C
        // VMAC encoding: opcode=1(01), funct2=0(00), func1=0, func3=0, reg5=0 => 16'b0000000000_0_00_01_0 = 0x0002
        im_write_addr=16'h0000; im_write_data=16'h0014; im_write_en=1; @(posedge clk); // NOP
        im_write_addr=16'h0002; im_write_data=16'h0002; im_write_en=1; @(posedge clk); // VMAC
        im_write_addr=16'h0004; im_write_data=16'h001C; im_write_en=1; @(posedge clk); // HALT
        im_write_en=0;

        // Test 3: Start running
        pe_running = 1;
        @(posedge clk); #1;
        check("Run1: valid_out=1", valid_out === 1'b1);
        check("Run1: pc=0", pc_out === 16'h0000);
        check("Run1: NOP decoded", ID_decode_signals_out.nop === 1'b1);

        // Test 4: PC advances to next instruction
        @(posedge clk); #1;
        check("Run2: pc=2", pc_out === 16'h0002);

        // Test 5: HALT instruction halts pipeline
        @(posedge clk); #1;
        check("Run3: pc=4", pc_out === 16'h0004);
        check("Run3: halt decoded", ID_decode_signals_out.halt === 1'b1);
        @(posedge clk); #1;
        check("PostHalt: halted_out=1", halted_out === 1'b1);
        check("PostHalt: valid_out=0", valid_out === 1'b0);

        // Test 6: Stage reset clears halt
        stage_reset = 1; @(posedge clk); stage_reset = 0; #1;
        check("StageReset: halted_out=0", halted_out === 1'b0);
        check("StageReset: valid_out=0", valid_out === 1'b0);

        // Test 7: Stall when ready_in=0
        pe_running = 1;
        @(posedge clk); #1; // valid becomes 1
        ready_in = 0;
        @(posedge clk); #1;
        stalled_pc = pc_out;
        @(posedge clk); #1;
        check("Stall: PC held", pc_out === stalled_pc);
        ready_in = 1;
        @(posedge clk); #1;
        check("Unstall: PC advanced", pc_out !== stalled_pc);

        // Test 8: pe_running=0 stops output
        pe_running = 0;
        @(posedge clk); #1;
        check("NotRunning: valid_out=0", valid_out === 1'b0);

        // Test 9: pc_init_value
        pe_running = 0;
        stage_reset = 1; pc_init_value = 16'h0002; @(posedge clk); stage_reset = 0; #1;
        check("PcInit: pc set", pc_out === 16'h0002);

        $display("\n=== tb_if_id_stage Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_if_id_stage FAIL");
        else $display("tb_if_id_stage PASS");
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_if_id_stage"); $finish; end
endmodule
