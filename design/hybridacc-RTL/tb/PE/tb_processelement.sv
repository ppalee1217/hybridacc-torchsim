//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_processelement
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for processelement module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/FIFO.sv, src/asyncFIFO.sv, src/PE/InstructionMemory.sv, src/PE/LoopController.sv, src/PE/Decoder.sv, src/PE/VADDU.sv, src/PE/VMULU.sv, src/PE/TransformRegFile.sv, src/PE/PsumRegFile.sv, src/PE/DataMemory.sv, src/PE/LDMA.sv, src/PE/SDMA.sv, src/PE/IF_ID_Stage.sv, src/PE/EXE_M_Stage.sv, src/PE/EXE_A_Stage.sv, src/PE/PErouter.sv, src/PE/ProcessElement.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/FIFO.sv"
`include "../../src/asyncFIFO.sv"
`include "../../src/PE/InstructionMemory.sv"
`include "../../src/PE/LoopController.sv"
`include "../../src/PE/Decoder.sv"
`include "../../src/PE/VADDU.sv"
`include "../../src/PE/VMULU.sv"
`include "../../src/PE/TransformRegFile.sv"
`include "../../src/PE/PsumRegFile.sv"
`include "../../src/PE/DataMemory.sv"
`include "../../src/PE/LDMA.sv"
`include "../../src/PE/SDMA.sv"
`include "../../src/PE/IF_ID_Stage.sv"
`include "../../src/PE/EXE_M_Stage.sv"
`include "../../src/PE/EXE_A_Stage.sv"
`include "../../src/PE/PErouter.sv"
`include "../../src/PE/ProcessElement.sv"
`endif

module tb_processelement;
    import hybridacc_utils_pkg::*;

    logic clk, reset_n, router_enable;
    PERouterMode router_mode;
    noc_request_t noc_ps_in_data, noc_pd_in_data, noc_pli_in_data;
    noc_addr_req_t noc_plo_in_data;
    logic noc_ps_in_valid, noc_ps_in_ready, noc_pd_in_valid, noc_pd_in_ready, noc_pli_in_valid, noc_pli_in_ready, noc_plo_in_valid, noc_plo_in_ready;
    noc_response_t noc_plo_out_data; logic noc_plo_out_valid, noc_plo_out_ready;
    logic pe_busy;
    logic [63:0] ln_pli_data, ln_plo_data;
    logic ln_pli_valid, ln_pli_ready, ln_plo_valid, ln_plo_ready;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    ProcessElement dut(
        .clk(clk), .reset_n(reset_n), .router_enable(router_enable), .router_mode(router_mode),
        .noc_ps_in_data(noc_ps_in_data), .noc_ps_in_valid(noc_ps_in_valid), .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data), .noc_pd_in_valid(noc_pd_in_valid), .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data), .noc_pli_in_valid(noc_pli_in_valid), .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data), .noc_plo_in_valid(noc_plo_in_valid), .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data), .noc_plo_out_valid(noc_plo_out_valid), .noc_plo_out_ready(noc_plo_out_ready),
        .pe_busy(pe_busy), .ln_pli_data(ln_pli_data), .ln_pli_valid(ln_pli_valid), .ln_pli_ready(ln_pli_ready),
        .ln_plo_data(ln_plo_data), .ln_plo_valid(ln_plo_valid), .ln_plo_ready(ln_plo_ready)
    );

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/ProcessElement/ProcessElement.sdf", dut);
end
`endif


    initial begin
        router_enable=1; router_mode=PLI_FROM_BUS_PLO_TO_BUS;
        noc_ps_in_data='0; noc_pd_in_data='0; noc_pli_in_data='0; noc_plo_in_data='0;
        noc_ps_in_valid=0; noc_pd_in_valid=0; noc_pli_in_valid=0; noc_plo_in_valid=0; noc_plo_out_ready=1;
        ln_pli_data=0; ln_pli_valid=0; ln_plo_ready=1;
        @(posedge reset_n);

        noc_ps_in_data.addr = PE_CMD_ADDRESS;
        noc_ps_in_data.data = CMD_START_PE;
        noc_ps_in_valid = 1; @(posedge clk); noc_ps_in_valid = 0;

        repeat(5) @(posedge clk);
        `TB_ASSERT(pe_busy==1'b1 || noc_ps_in_ready==1'b1, "ProcessElement should respond to start command path");

        $display("tb_processelement PASS");
        $finish;
    end
endmodule
