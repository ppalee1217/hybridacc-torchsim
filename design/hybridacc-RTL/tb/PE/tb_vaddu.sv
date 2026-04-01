//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_vaddu
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for vaddu module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/VADDU.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/PE/VADDU.sv"
`endif

module tb_vaddu;
    import hybridacc_utils_pkg::*;

    // Sampling clock for @(negedge clk) — 4ns period gives 2ns settle for comb paths
    logic clk;
    initial begin clk = 0; forever #2 clk = ~clk; end

    v_fp16_t op1, op2, result;
    VADDU dut(.op1(op1), .op2(op2), .result(result));

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/VADDU/VADDU.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;


    initial begin
        // Test 1: Zero + Zero = Zero (all lanes)
        op1 = '0; op2 = '0; @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("Zero+Zero lane[%0d]=0", i), result.lanes[i], 16'h0000)

        // Test 2: 1.0 + 1.0 = 2.0 (0x3C00 + 0x3C00 = 0x4000)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'h3C00;
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("1.0+1.0 lane[%0d]=2.0(0x4000)", i), result.lanes[i], 16'h4000)

        // Test 3: 1.0 + (-1.0) = 0.0 (0x3C00 + 0xBC00 = 0x0000)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'hBC00;
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("1.0+(-1.0) lane[%0d]=0", i), result.lanes[i], 16'h0000)

        // Test 4: Mixed lanes - independent computation
        op1.lanes[0] = 16'h3C00; op2.lanes[0] = 16'h3C00; // 1+1=2
        op1.lanes[1] = 16'h4000; op2.lanes[1] = 16'h4000; // 2+2=4
        op1.lanes[2] = 16'h0000; op2.lanes[2] = 16'h3C00; // 0+1=1
        op1.lanes[3] = 16'hBC00; op2.lanes[3] = 16'h0000; // -1+0=-1
        @(negedge clk);
        `CHECK_VAL("Mixed lane[0]=2.0", result.lanes[0], 16'h4000)
        `CHECK_VAL("Mixed lane[1]=4.0", result.lanes[1], 16'h4400)
        `CHECK_VAL("Mixed lane[2]=1.0", result.lanes[2], 16'h3C00)
        `CHECK_VAL("Mixed lane[3]=-1.0", result.lanes[3], 16'hBC00)

        // Test 5: Large values (max normal fp16 = 0x7BFF = 65504)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h7BFF;
            op2.lanes[i] = 16'h7BFF;
        end
        @(negedge clk);
        // Result should be +inf (0x7C00) in fp16
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("Overflow lane[%0d]=inf", i), result.lanes[i], 16'h7C00)

        // Test 6: Subnormal (very small) numbers
        // VADDU uses DW_fp_add with ieee_compliance=0: subnormals are flushed to zero.
        op1.lanes[0] = 16'h0001; // smallest subnormal
        op2.lanes[0] = 16'h0001;
        op1.lanes[1] = 16'h0000;
        op2.lanes[1] = 16'h0001; // subnormal
        @(negedge clk);
        // subnormal+subnormal: flushed to 0+0 = 0
        `CHECK_VAL("Subnormal+subnormal=0 (flush)", result.lanes[0], 16'h0000)
        // zero+subnormal: flushed to 0+0 = 0
        `CHECK_VAL("Zero+subnormal=0 (flush)", result.lanes[1], 16'h0000)

        `TB_SUMMARY("tb_vaddu")
        $finish;
    end
endmodule
