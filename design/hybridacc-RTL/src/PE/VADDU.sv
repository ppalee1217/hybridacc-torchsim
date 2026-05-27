//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   VADDU
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

module VADDU (
    input  v_fp16_t op1,
    input  v_fp16_t op2,
    output v_fp16_t result
);

    v_fp16_t         result_w;
    logic [3:0][7:0] fp_status_w;
    logic            fp_status_known_w;

    assign fp_status_known_w = ((^fp_status_w) === (^fp_status_w));
    assign result = fp_status_known_w ? result_w : '0;

    genvar i;
    generate
        for (i = 0; i < 4; i++) begin : fp_add_lane
            DW_fp_add #(
                .sig_width       (10),
                .exp_width       (5),
                .ieee_compliance (0)
            ) u_fp_add (
                .a      (op1.lanes[i]),
                .b      (op2.lanes[i]),
                .rnd    (3'b000),       // Round to nearest even
                .z      (result_w.lanes[i]),
                .status (fp_status_w[i])
            );
        end
    endgenerate
endmodule
