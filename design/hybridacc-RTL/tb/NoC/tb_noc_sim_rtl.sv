//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_noc_sim_rtl
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for noc_sim_rtl module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/FIFO.sv, src/asyncFIFO.sv, src/PE/InstructionMemory.sv, src/PE/LoopController.sv, src/PE/Decoder.sv, src/PE/VADDU.sv, src/PE/VMULU.sv, src/PE/TransformRegFile.sv, src/PE/PsumRegFile.sv, src/PE/DataMemory.sv, src/PE/LDMA.sv, src/PE/SDMA.sv, src/PE/IF_ID_Stage.sv, src/PE/EXE_M_Stage.sv, src/PE/EXE_A_Stage.sv, src/PE/PErouter.sv, src/PE/ProcessElement.sv, src/NoC/MBUS.sv, src/NoC/NoCRouter.sv, src/NetworkOnChip.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
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
`include "../../src/NoC/MBUS.sv"
`include "../../src/NoC/NoCRouter.sv"
`include "../../src/NetworkOnChip.sv"

module tb_noc_sim_rtl;
    import hybridacc_utils_pkg::*;

    localparam int NP = 3;
    localparam int PW = 64;
    localparam int NPP = 2;

    logic clk, reset_n;
    logic command_mode;
    logic [31:0] command_data;

    logic [NP*PW-1:0] noc_ps_in_data, noc_pd_in_data, noc_pli_in_data;
    logic noc_ps_in_valid, noc_ps_in_ready;
    logic noc_pd_in_valid, noc_pd_in_ready;
    logic noc_pli_in_valid, noc_pli_in_ready;

    noc_addr_req_t noc_plo_in_data;
    logic noc_plo_in_valid, noc_plo_in_ready;

    logic [NP*PW-1:0] noc_plo_out_data;
    NOC_RESPONSE_STATUS noc_plo_out_status;
    logic noc_plo_out_valid, noc_plo_out_ready;

    int ps_sent, pd_sent, pli_sent, plo_req_sent;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    NetworkOnChip #(
        .NUM_PORTS(NP), .PORT_WIDTH_BITS(PW), .NUM_PES_PER_PORT(NPP), .PE_FIFO_DEPTH(4)
    ) dut (
        .clk(clk), .reset_n(reset_n), .command_mode(command_mode), .command_data(command_data),
        .noc_ps_in_data(noc_ps_in_data), .noc_ps_in_valid(noc_ps_in_valid), .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data), .noc_pd_in_valid(noc_pd_in_valid), .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data), .noc_pli_in_valid(noc_pli_in_valid), .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data), .noc_plo_in_valid(noc_plo_in_valid), .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data), .noc_plo_out_status(noc_plo_out_status), .noc_plo_out_valid(noc_plo_out_valid), .noc_plo_out_ready(noc_plo_out_ready)
    );

    task automatic send_scan_chain_cfg(input int idx);
        logic [31:0] payload;
        logic [5:0] id_ps, id_pd, id_pli, id_plo;
        payload = 32'h0;
        id_ps  = idx[5:0];
        id_pd  = (idx + 1);
        id_pli = (idx + 2);
        id_plo = (idx + 3);
        payload[9:4]   = id_ps;
        payload[15:10] = id_pd;
        payload[21:16] = id_pli;
        payload[27:22] = id_plo;
        payload[29:28] = PLI_FROM_BUS_PLO_TO_BUS;
        payload[30]    = 1'b1;
        payload[3:0]   = CMD_NOC_SCAN_CHAIN;

        command_mode = 1'b1;
        command_data = payload;
        @(posedge clk);
        command_mode = 1'b0;
        command_data = '0;
    endtask

    task automatic send_ps(input logic [NP*PW-1:0] payload);
        noc_ps_in_data = payload;
        noc_ps_in_valid = 1'b1;
        @(posedge clk);
        while (!noc_ps_in_ready) @(posedge clk);
        ps_sent++;
        noc_ps_in_valid = 1'b0;
    endtask

    task automatic send_pd(input logic [NP*PW-1:0] payload);
        noc_pd_in_data = payload;
        noc_pd_in_valid = 1'b1;
        @(posedge clk);
        while (!noc_pd_in_ready) @(posedge clk);
        pd_sent++;
        noc_pd_in_valid = 1'b0;
    endtask

    task automatic send_pli(input logic [NP*PW-1:0] payload);
        noc_pli_in_data = payload;
        noc_pli_in_valid = 1'b1;
        @(posedge clk);
        while (!noc_pli_in_ready) @(posedge clk);
        pli_sent++;
        noc_pli_in_valid = 1'b0;
    endtask

    task automatic send_plo_req(input logic [15:0] addr);
        noc_plo_in_data.addr = addr;
        noc_plo_in_valid = 1'b1;
        @(posedge clk);
        while (!noc_plo_in_ready) @(posedge clk);
        plo_req_sent++;
        noc_plo_in_valid = 1'b0;
    endtask

    initial begin
        command_mode = 1'b0;
        command_data = '0;
        noc_ps_in_data = '0;
        noc_pd_in_data = '0;
        noc_pli_in_data = '0;
        noc_ps_in_valid = 1'b0;
        noc_pd_in_valid = 1'b0;
        noc_pli_in_valid = 1'b0;
        noc_plo_in_data = '0;
        noc_plo_in_valid = 1'b0;
        noc_plo_out_ready = 1'b1;

        ps_sent = 0;
        pd_sent = 0;
        pli_sent = 0;
        plo_req_sent = 0;

        @(posedge reset_n);
        repeat (2) @(posedge clk);

        for (int i = 0; i < NP*NPP; i++) begin
            send_scan_chain_cfg(i);
        end

        send_ps (192'h0000_0000_0000_0004_0000_0000_0000_0003_0000_0000_0000_0002);
        send_ps (192'h0000_0000_0000_0008_0000_0000_0000_0007_0000_0000_0000_0006);

        send_pd (192'h0000_0000_0000_0104_0000_0000_0000_0103_0000_0000_0000_0102);
        send_pd (192'h0000_0000_0000_0204_0000_0000_0000_0203_0000_0000_0000_0202);

        send_pli(192'h0000_0000_0000_1004_0000_0000_0000_1003_0000_0000_0000_1002);
        send_pli(192'h0000_0000_0000_2004_0000_0000_0000_2003_0000_0000_0000_2002);

        send_plo_req(16'h0000);
        send_plo_req(16'h0001);

        repeat (20) @(posedge clk);

        `TB_ASSERT(ps_sent == 2, "PS packets sent count mismatch");
        `TB_ASSERT(pd_sent == 2, "PD packets sent count mismatch");
        `TB_ASSERT(pli_sent == 2, "PLI packets sent count mismatch");
        `TB_ASSERT(plo_req_sent == 2, "PLO request count mismatch");

        `TB_ASSERT(!$isunknown(noc_ps_in_ready), "noc_ps_in_ready should not be X/Z");
        `TB_ASSERT(!$isunknown(noc_pd_in_ready), "noc_pd_in_ready should not be X/Z");
        `TB_ASSERT(!$isunknown(noc_pli_in_ready), "noc_pli_in_ready should not be X/Z");
        `TB_ASSERT(!$isunknown(noc_plo_in_ready), "noc_plo_in_ready should not be X/Z");
        `TB_ASSERT(!$isunknown(noc_plo_out_valid), "noc_plo_out_valid should not be X/Z");

        $display("tb_noc_sim_rtl PASS");
        $finish;
    end
endmodule
