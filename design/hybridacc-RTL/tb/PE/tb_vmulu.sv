//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_vmulu
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for vmulu module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/VMULU.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/PE/VMULU.sv"
`endif

module tb_vmulu;
    import hybridacc_utils_pkg::*;

    // Sampling clock for @(negedge clk) — 4ns period gives 2ns settle for comb paths
    logic clk;
    initial begin clk = 0; forever #2 clk = ~clk; end

    v_fp16_t op1, op2, result;
    VMULU dut(.op1(op1), .op2(op2), .result(result));

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/VMULU/VMULU.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;


    initial begin
        // Test 1: Zero * Zero = Zero
        op1 = '0; op2 = '0; @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("0*0 lane[%0d]=0", i), result.lanes[i], 16'h0000)

        // Test 2: 1.0 * 1.0 = 1.0 (0x3C00)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'h3C00;
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("1.0*1.0 lane[%0d]=1.0", i), result.lanes[i], 16'h3C00)

        // Test 3: 2.0 * 3.0 = 6.0 (0x4000 * 0x4200 = 0x4600)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h4000; // 2.0
            op2.lanes[i] = 16'h4200; // 3.0
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("2.0*3.0 lane[%0d]=6.0(0x4600)", i), result.lanes[i], 16'h4600)

        // Test 4: Anything * 0 = 0
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h5000; // 32.0
            op2.lanes[i] = 16'h0000; // 0
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("32*0 lane[%0d]=0", i), result.lanes[i], 16'h0000)

        // Test 5: Negative * Positive = Negative
        // 1.0 * (-1.0) = -1.0 (0x3C00 * 0xBC00 = 0xBC00)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'hBC00;
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("1*(-1) lane[%0d]=-1.0", i), result.lanes[i], 16'hBC00)

        // Test 6: Negative * Negative = Positive
        // (-2.0) * (-3.0) = 6.0 (0xC000 * 0xC200 = 0x4600)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'hC000;
            op2.lanes[i] = 16'hC200;
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("(-2)*(-3) lane[%0d]=6.0", i), result.lanes[i], 16'h4600)

        // Test 7: Overflow to infinity
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h7BFF; // max normal
            op2.lanes[i] = 16'h4000; // 2.0
        end
        @(negedge clk);
        for (int i = 0; i < 4; i++)
            `CHECK_VAL($sformatf("Overflow lane[%0d]=inf", i), result.lanes[i], 16'h7C00)

        // Test 8: Mixed lane independence
        op1.lanes[0] = 16'h3C00; op2.lanes[0] = 16'h4000; // 1*2=2
        op1.lanes[1] = 16'h0000; op2.lanes[1] = 16'h7BFF; // 0*max=0
        op1.lanes[2] = 16'h4200; op2.lanes[2] = 16'h4200; // 3*3=9
        op1.lanes[3] = 16'hBC00; op2.lanes[3] = 16'hBC00; // -1*-1=1
        @(negedge clk);
        `CHECK_VAL("Mixed lane[0]=2.0", result.lanes[0], 16'h4000)
        `CHECK_VAL("Mixed lane[1]=0", result.lanes[1], 16'h0000)
        `CHECK_VAL("Mixed lane[2]=9.0(0x4880)", result.lanes[2], 16'h4880)
        `CHECK_VAL("Mixed lane[3]=1.0", result.lanes[3], 16'h3C00)

        `TB_SUMMARY("tb_vmulu")
        $finish;
    end
endmodule
