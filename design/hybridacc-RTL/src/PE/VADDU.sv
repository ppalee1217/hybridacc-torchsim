// Module: VADDU
// Function: Vector add unit that accumulates lane-wise half-precision sums.
import hybridacc_utils_pkg::*;

module VADDU (
    input  v_fp16_t op1,
    input  v_fp16_t op2,
    output v_fp16_t result
);
    v_fp16_t res;

    always_comb begin
        for (int i = 0; i < 4; i++) begin
            res.lanes[i] = fp16_add(op1.lanes[i], op2.lanes[i]);
        end
    end

    assign result = res;
endmodule
