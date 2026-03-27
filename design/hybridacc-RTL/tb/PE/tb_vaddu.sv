`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/PE/VADDU.sv"

module tb_vaddu;
    import hybridacc_utils_pkg::*;

    v_fp16_t op1, op2, result;
    VADDU dut(.op1(op1), .op2(op2), .result(result));

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
        // Test 1: Zero + Zero = Zero (all lanes)
        op1 = '0; op2 = '0; #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("Zero+Zero lane[%0d]=0", i), result.lanes[i] === 16'h0000);

        // Test 2: 1.0 + 1.0 = 2.0 (0x3C00 + 0x3C00 = 0x4000)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'h3C00;
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("1.0+1.0 lane[%0d]=2.0(0x4000)", i), result.lanes[i] === 16'h4000);

        // Test 3: 1.0 + (-1.0) = 0.0 (0x3C00 + 0xBC00 = 0x0000)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h3C00;
            op2.lanes[i] = 16'hBC00;
        end
        #1;
        for (int i = 0; i < 4; i++)
            check($sformatf("1.0+(-1.0) lane[%0d]=0", i), result.lanes[i] === 16'h0000);

        // Test 4: Mixed lanes - independent computation
        op1.lanes[0] = 16'h3C00; op2.lanes[0] = 16'h3C00; // 1+1=2
        op1.lanes[1] = 16'h4000; op2.lanes[1] = 16'h4000; // 2+2=4
        op1.lanes[2] = 16'h0000; op2.lanes[2] = 16'h3C00; // 0+1=1
        op1.lanes[3] = 16'hBC00; op2.lanes[3] = 16'h0000; // -1+0=-1
        #1;
        check("Mixed lane[0]=2.0", result.lanes[0] === 16'h4000);
        check("Mixed lane[1]=4.0", result.lanes[1] === 16'h4400);
        check("Mixed lane[2]=1.0", result.lanes[2] === 16'h3C00);
        check("Mixed lane[3]=-1.0", result.lanes[3] === 16'hBC00);

        // Test 5: Large values (max normal fp16 = 0x7BFF = 65504)
        for (int i = 0; i < 4; i++) begin
            op1.lanes[i] = 16'h7BFF;
            op2.lanes[i] = 16'h7BFF;
        end
        #1;
        // Result should be +inf (0x7C00) in fp16
        for (int i = 0; i < 4; i++)
            check($sformatf("Overflow lane[%0d]=inf", i), result.lanes[i] === 16'h7C00);

        // Test 6: Subnormal (very small) numbers
        // RTL fp16_add flushes subnormals: if exp_a==0 return b, if exp_b==0 return a
        op1.lanes[0] = 16'h0001; // smallest subnormal
        op2.lanes[0] = 16'h0001;
        op1.lanes[1] = 16'h0000;
        op2.lanes[1] = 16'h0001;
        #1;
        // subnormal+subnormal: exp_a==0 → returns b (0x0001), not arithmetic sum
        check("Subnormal+subnormal (flush)", result.lanes[0] === 16'h0001);
        // zero+subnormal: a_is_zero → returns b
        check("Zero+subnormal", result.lanes[1] === 16'h0001);

        $display("\n=== tb_vaddu Summary: %0d PASSED, %0d FAILED ===", pass_count, fail_count);
        if (fail_count > 0) $display("tb_vaddu FAIL");
        else $display("tb_vaddu PASS");
        $finish;
    end
endmodule
