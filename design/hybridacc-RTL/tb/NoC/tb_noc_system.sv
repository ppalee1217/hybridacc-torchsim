//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_noc_system
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for noc_system module.
// Dependencies:  tb_common.svh, tb_networkonchip.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../tb_networkonchip.sv"

module tb_noc_system;
    initial begin
        $display("tb_noc_system delegates to tb_networkonchip");
        #(`TB_SETTLE);
    end
endmodule
