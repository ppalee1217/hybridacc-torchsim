`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/PE/VMULU.sv"

module tb_vmulu;
    import hybridacc_utils_pkg::*;

    v_fp16_t op1, op2, result;
    VMULU dut(.op1(op1), .op2(op2), .result(result));

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
        // Test 1: Zero * Zero = Zero
        op1 = '0; op2 = '0; #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("0*0 lane[%0d]=0", i), result.lanes[i] === 16'h0000);

        // Test 2: 1.0 * 1.0 = 1.0 (0x3C00)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'h3C00;
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("1.0*1.0 lane[%0d]=1.0", i), result.lanes[i] === 16'h3C00);

        // Test 3: 2.0 * 3.0 = 6.0 (0x4000 * 0x4200 = 0x4600)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h4000; // 2.0
            op2.lanes[i] = 16'h4200; // 3.0
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("2.0*3.0 lane[%0d]=6.0(0x4600)", i), result.lanes[i] === 16'h4600);

        // Test 4: Anything * 0 = 0
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h5000; // 32.0
            op2.lanes[i] = 16'h0000; // 0
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("32*0 lane[%0d]=0", i), result.lanes[i] === 16'h0000);

        // Test 5: Negative * Positive = Negative
        // 1.0 * (-1.0) = -1.0 (0x3C00 * 0xBC00 = 0xBC00)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'hBC00;
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("1*(-1) lane[%0d]=-1.0", i), result.lanes[i] === 16'hBC00);

        // Test 6: Negative * Negative = Positive
        // (-2.0) * (-3.0) = 6.0 (0xC000 * 0xC200 = 0x4600)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'hC000;
            op2.lanes[i] = 16'hC200;
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("(-2)*(-3) lane[%0d]=6.0", i), result.lanes[i] === 16'h4600);

        // Test 7: Overflow to infinity
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h7BFF; // max normal
            op2.lanes[i] = 16'h4000; // 2.0
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("Overflow lane[%0d]=inf", i), result.lanes[i] === 16'h7C00);

        // Test 8: Mixed lane independence
        op1.lanes[0] = 16'h3C00; op2.lanes[0] = 16'h4000; // 1*2=2
        op1.lanes[1] = 16'h0000; op2.lanes[1] = 16'h7BFF; // 0*max=0
        op1.lanes[2] = 16'h4200; op2.lanes[2] = 16'h4200; // 3*3=9
        op1.lanes[3] = 16'hBC00; op2.lanes[3] = 16'hBC00; // -1*-1=1
        #1;
        check("Mixed lane[0]=2.0", result.lanes[0] === 16'h4000);
        check("Mixed lane[1]=0", result.lanes[1] === 16'h0000);
        check("Mixed lane[2]=9.0(0x4880)", result.lanes[2] === 16'h4880);
        check("Mixed lane[3]=1.0", result.lanes[3] === 16'h3C00);

        $display("\n=== tb_vmulu Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_vmulu FAIL");
        else $display("tb_vmulu PASS");
        $finish;
    end
endmodule
