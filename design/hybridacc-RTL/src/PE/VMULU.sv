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

    genvar i;
    generate
        for (i = 0; i < 4; i++) begin : fp_mul_lane
            DW_fp_mult #(
                .sig_width       (10),
                .exp_width       (5),
                .ieee_compliance (1)
            ) u_fp_mult (
                .a      (op1.lanes[i]),
                .b      (op2.lanes[i]),
                .rnd    (3'b000),       // Round to nearest even
                .z      (result.lanes[i]),
                .status ()
            );
        end
    endgenerate
endmodule
