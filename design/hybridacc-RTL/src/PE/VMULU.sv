//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   VMULU
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Common utility package with type definitions, FP16 arithmetic, and shared constants.
// Dependencies:  hybridacc_utils_pkg
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;

module VMULU (
    input  v_fp16_t op1,
    input  v_fp16_t op2,
    output v_fp16_t result
);
    v_fp16_t res;

    always_comb begin
        for (int i = 0; i < 4; i++) begin
            res.lanes[i] = fp16_mul(op1.lanes[i], op2.lanes[i]);
        end
    end

    assign result = res;
endmodule
