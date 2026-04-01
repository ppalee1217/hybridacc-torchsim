//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_decoder
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for decoder module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/Decoder.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/PE/Decoder.sv"
`endif

module tb_decoder;
    import hybridacc_utils_pkg::*;

    // Sampling clock for @(negedge clk) — 4ns period gives 2ns settle for comb paths
    logic clk;
    initial begin clk = 0; forever #2 clk = ~clk; end

    pe_inst_t inst_in;
    pe_decode_signals_t decode_signals_out;

    Decoder dut(.inst_in(inst_in), .decode_signals_out(decode_signals_out));

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/Decoder/Decoder.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;


    // Helper to encode instruction word
    // Format: [15:6]=payload, [5]=func1, [4:3]=funct2, [2:1]=opcode, [0]=loop_end
    function automatic logic [15:0] encode(
        input logic [9:0] payload,
        input logic func1,
        input logic [1:0] funct2,
        input logic [1:0] opcode,
        input logic loop_end
    );
        return {payload, func1, funct2, opcode, loop_end};
    endfunction

    initial begin
        // Test 1: All-zero instruction (0x0000) → special case in Decoder: pe_decode_signals_zero()
        inst_in = 16'h0000; @(negedge clk);
        `CHECK_BIT("AllZero: halt=0", decode_signals_out.halt, 1'b0)
        `CHECK_BIT("AllZero: nop=0", decode_signals_out.nop, 1'b0)
        `CHECK_BIT("AllZero: DMA_setaddr=0 (zeroed out)", decode_signals_out.DMA_setaddr, 1'b0)

        // Test 2: HALT (opcode=2, funct2=3, func1=0)
        inst_in = encode(10'd0, 1'b0, 2'd3, 2'd2, 1'b0); @(negedge clk);
        `CHECK_BIT("HALT: halt=1", decode_signals_out.halt, 1'b1)
        `CHECK_BIT("HALT: loop_end=0 (forced)", decode_signals_out.loop_end, 1'b0)

        // Test 3: NOP instruction (opcode=2, funct2=2, func1=0)
        inst_in = encode(10'd0, 1'b0, 2'd2, 2'd2, 1'b0); @(negedge clk);
        `CHECK_BIT("NOP_inst: nop=1", decode_signals_out.nop, 1'b1)
        `CHECK_BIT("NOP_inst: halt=0", decode_signals_out.halt, 1'b0)

        // Test 4: LOOPIN (opcode=2, funct2=0, func1=0) with count in payload
        inst_in = encode(10'd5, 1'b0, 2'd0, 2'd2, 1'b0); @(negedge clk);
        `CHECK_BIT("LOOPIN: loop_in=1", decode_signals_out.loop_in, 1'b1)
        `CHECK_VAL("LOOPIN: imm=5", decode_signals_out.imm, 16'd5)

        // Test 5: Loop end bit propagation
        inst_in = encode(10'd0, 1'b0, 2'd2, 2'd2, 1'b1); @(negedge clk);
        `CHECK_BIT("LoopEnd: loop_end=1", decode_signals_out.loop_end, 1'b1)

        // Test 6: DMA.ADDR for LDMA (opcode=0, funct2=0, func1=0)
        inst_in = encode(10'd100, 1'b0, 2'd0, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("DMA_ADDR_LDMA: setaddr=1", decode_signals_out.DMA_setaddr, 1'b1)
        `CHECK_BIT("DMA_ADDR_LDMA: is_sdma=0", decode_signals_out.DMA_is_sdma, 1'b0)

        // Test 7: DMA.ADDR for SDMA (opcode=0, funct2=0, func1=1)
        inst_in = encode(10'd50, 1'b1, 2'd0, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("DMA_ADDR_SDMA: setaddr=1", decode_signals_out.DMA_setaddr, 1'b1)
        `CHECK_BIT("DMA_ADDR_SDMA: is_sdma=1", decode_signals_out.DMA_is_sdma, 1'b1)

        // Test 8: DMA.LEN (opcode=0, funct2=1)
        inst_in = encode(10'd10, 1'b0, 2'd1, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("DMA_LEN: setlen=1", decode_signals_out.DMA_setlen, 1'b1)

        // Test 9: DMA.LOOP (opcode=0, funct2=2)
        inst_in = encode(10'd3, 1'b0, 2'd2, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("DMA_LOOP: setloop=1", decode_signals_out.DMA_setloop, 1'b1)

        // Test 10: DMA.MODE (opcode=0, funct2=3, func1=0)
        inst_in = encode(10'b0000_000_011, 1'b0, 2'd3, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("DMA_MODE: setmode=1", decode_signals_out.DMA_setmode, 1'b1)

        // Test 11: SYS.CTRL (opcode=2, funct2=1, func1=0) - sdma_act+sdma_rst
        // payload[7]=sdma_act, payload[6]=sdma_rst
        inst_in = encode(10'b0011_00_0000, 1'b0, 2'd1, 2'd2, 1'b0); @(negedge clk);
        `CHECK_BIT("SYS_CTRL: sdma_act=1", decode_signals_out.sys_sdma_act, 1'b1)
        `CHECK_BIT("SYS_CTRL: sdma_rst=1", decode_signals_out.sys_sdma_rst, 1'b1)

        // Test 11b: SYS.CTRL ldma_act+ldma_rst
        // payload[5]=ldma_act, payload[4]=ldma_rst
        inst_in = encode(10'b0000_11_0000, 1'b0, 2'd1, 2'd2, 1'b0); @(negedge clk);
        `CHECK_BIT("SYS_CTRL: ldma_act=1", decode_signals_out.sys_ldma_act, 1'b1)
        `CHECK_BIT("SYS_CTRL: ldma_rst=1", decode_signals_out.sys_ldma_rst, 1'b1)
        `CHECK_BIT("SYS_CTRL: sdma_act=0 (not set)", decode_signals_out.sys_sdma_act, 1'b0)

        // Test 11c: SYS.CTRL clear regs
        // payload[1]=tr_clear_regs, payload[0]=pr_clear_regs
        inst_in = encode(10'b0000_00_0011, 1'b0, 2'd1, 2'd2, 1'b0); @(negedge clk);
        `CHECK_BIT("SYS_CTRL: tr_clear_regs=1", decode_signals_out.tr_clear_regs, 1'b1)
        `CHECK_BIT("SYS_CTRL: pr_clear_regs=1", decode_signals_out.pr_clear_regs, 1'b1)

        // Test 12: VMULU/VADDU arithmetic (opcode=1, funct2=0, f3=0 → VMULU mode)
        inst_in = encode({5'd8, 2'd0, 3'd0}, 1'b0, 2'd0, 2'd1, 1'b0); @(negedge clk);
        `CHECK_BIT("VMULU: vaddu_en=1", decode_signals_out.vaddu_en, 1'b1)
        `CHECK_BIT("VMULU: tr_en=1", decode_signals_out.tr_en, 1'b1)
        `CHECK_BIT("VMULU: pr_write=1", decode_signals_out.pr_write, 1'b1)
        `CHECK_BIT("VMULU: pr_mode=0", decode_signals_out.pr_mode, 1'b0)

        // Test 13: VADDU mode (opcode=1, funct2=1)
        inst_in = encode({5'd8, 2'd0, 3'd0}, 1'b0, 2'd1, 2'd1, 1'b0); @(negedge clk);
        `CHECK_BIT("VADDU: vaddu_en=1", decode_signals_out.vaddu_en, 1'b1)
        `CHECK_BIT("VADDU: pr_mode=1", decode_signals_out.pr_mode, 1'b1)

        // Test 14: VPSUM (opcode=1, funct2=2)
        inst_in = encode({5'd2, 2'd0, 3'd0}, 1'b0, 2'd2, 2'd1, 1'b0); @(negedge clk);
        `CHECK_BIT("VPSUM: pli_plo_operation=1", decode_signals_out.pli_plo_operation, 1'b1)
        `CHECK_BIT("VPSUM: vaddu_en=1", decode_signals_out.vaddu_en, 1'b1)

        // Test 15: SWAP instruction (opcode=2, funct2=1, func1=1)
        inst_in = encode({9'd0, 1'b1}, 1'b1, 2'd1, 2'd2, 1'b0); @(negedge clk);
        `CHECK_BIT("SWAP: is_swap=1", decode_signals_out.is_swap, 1'b1)

        // Test 16: DMA.MODE for SDMA (opcode=0, funct2=3, func1=0, func3=111)
        inst_in = encode({4'd0, 3'd0, 3'b111}, 1'b0, 2'd3, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("DMA_MODE_SDMA: setmode=1", decode_signals_out.DMA_setmode, 1'b1)
        `CHECK_BIT("DMA_MODE_SDMA: is_sdma=1", decode_signals_out.DMA_is_sdma, 1'b1)

        // Test 17: TSTORE instruction (opcode=0, funct2=3, func1=1, func3=0)
        inst_in = encode({4'd5, 3'd0, 3'd0}, 1'b1, 2'd3, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("TSTORE: tr_write=1", decode_signals_out.tr_write, 1'b1)
        `CHECK_BIT("TSTORE: pd_load=1", decode_signals_out.pd_load, 1'b1)
        `CHECK_BIT("TSTORE: tr_en=1", decode_signals_out.tr_en, 1'b1)

        // Test 18: VTSTORE instruction (opcode=0, funct2=3, func1=1, func3=1)
        inst_in = encode({4'd0, 3'd1, 3'd1}, 1'b1, 2'd3, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("VTSTORE: tr_write_v=1", decode_signals_out.tr_write_v, 1'b1)
        `CHECK_BIT("VTSTORE: pd_load_v=1", decode_signals_out.pd_load_v, 1'b1)

        // Test 19: TSHIFT instruction (opcode=0, funct2=3, func1=1, func3=2)
        inst_in = encode({4'd0, 3'd1, 3'd2}, 1'b1, 2'd3, 2'd0, 1'b0); @(negedge clk);
        `CHECK_BIT("TSHIFT: tr_shift=1", decode_signals_out.tr_shift, 1'b1)

        // Test 20: VPSUM (opcode=1, funct2=2, func3=0)
        inst_in = encode({5'd4, 2'd0, 3'd0}, 1'b0, 2'd2, 2'd1, 1'b0); @(negedge clk);
        `CHECK_BIT("VPSUM: pli_plo_operation=1", decode_signals_out.pli_plo_operation, 1'b1)
        `CHECK_BIT("VPSUM: vaddu_en=1", decode_signals_out.vaddu_en, 1'b1)
        `CHECK_BIT("VPSUM: pr_mode=1", decode_signals_out.pr_mode, 1'b1)

        // Test 21: VMAC with vcounter mode (opcode=1, funct2=0, func3=1)
        inst_in = encode({5'd8, 2'd1, 3'd1}, 1'b0, 2'd0, 2'd1, 1'b0); @(negedge clk);
        `CHECK_BIT("VMAC_vc: pr_use_vcounter=1", decode_signals_out.pr_use_vcounter, 1'b1)
        `CHECK_BIT("VMAC_vc: tr_use_vcounter=1", decode_signals_out.tr_use_vcounter, 1'b1)
        `CHECK_BIT("VMAC_vc: pr_incr_vcounter=1", decode_signals_out.pr_incr_vcounter, 1'b1)
        `CHECK_BIT("VMAC_vc: tr_incr_vcounter=1", decode_signals_out.tr_incr_vcounter, 1'b1)

        // Test 22: VMAC with vcounter reset (reg5=31 for pid reset, vtbits=3 for tid reset)
        inst_in = encode({5'd31, 2'd3, 3'd1}, 1'b0, 2'd0, 2'd1, 1'b0); @(negedge clk);
        `CHECK_BIT("VMAC_rst: sys_rst_pid=1", decode_signals_out.sys_rst_pid, 1'b1)
        `CHECK_BIT("VMAC_rst: sys_rst_tid=1", decode_signals_out.sys_rst_tid, 1'b1)

        // Test 23: Loop end flag with arithmetic instruction
        inst_in = encode({5'd0, 2'd0, 3'd0}, 1'b0, 2'd0, 2'd1, 1'b1); @(negedge clk);
        `CHECK_BIT("LoopEnd+Arith: loop_end=1", decode_signals_out.loop_end, 1'b1)
        `CHECK_BIT("LoopEnd+Arith: vaddu_en=1", decode_signals_out.vaddu_en, 1'b1)

        `TB_SUMMARY("tb_decoder")
        $finish;
    end
endmodule
