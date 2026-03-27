`include "tb_common.svh"
`include "../src/hybridacc_utils_pkg.sv"
`include "../src/FIFO.sv"
`include "../src/asyncFIFO.sv"
`include "../src/PE/InstructionMemory.sv"
`include "../src/PE/LoopController.sv"
`include "../src/PE/Decoder.sv"
`include "../src/PE/VADDU.sv"
`include "../src/PE/VMULU.sv"
`include "../src/PE/TransformRegFile.sv"
`include "../src/PE/PsumRegFile.sv"
`include "../src/PE/DataMemory.sv"
`include "../src/PE/LDMA.sv"
`include "../src/PE/SDMA.sv"
`include "../src/PE/IF_ID_Stage.sv"
`include "../src/PE/EXE_M_Stage.sv"
`include "../src/PE/EXE_A_Stage.sv"
`include "../src/PE/PErouter.sv"
`include "../src/PE/ProcessElement.sv"
`include "../src/NoC/MBUS.sv"
`include "../src/NoC/NoCRouter.sv"
`include "../src/NetworkOnChip.sv"

module tb_networkonchip;
    import hybridacc_utils_pkg::*;
    localparam int NP=1;
    localparam int PW=64;
    localparam int NPP=1;

    logic clk, reset_n, command_mode;
    logic [31:0] command_data;
    logic [NP*PW-1:0] noc_ps_in_data, noc_pd_in_data, noc_pli_in_data;
    logic noc_ps_in_valid, noc_ps_in_ready, noc_pd_in_valid, noc_pd_in_ready, noc_pli_in_valid, noc_pli_in_ready;
    noc_addr_req_t noc_plo_in_data; logic noc_plo_in_valid, noc_plo_in_ready;
    logic [NP*PW-1:0] noc_plo_out_data; NOC_RESPONSE_STATUS noc_plo_out_status; logic noc_plo_out_valid, noc_plo_out_ready;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    NetworkOnChip #(.NUM_PORTS(NP), .PORT_WIDTH_BITS(PW), .NUM_PES_PER_PORT(NPP), .PE_FIFO_DEPTH(2)) dut(
        .clk(clk), .reset_n(reset_n), .command_mode(command_mode), .command_data(command_data),
        .noc_ps_in_data(noc_ps_in_data), .noc_ps_in_valid(noc_ps_in_valid), .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data), .noc_pd_in_valid(noc_pd_in_valid), .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data), .noc_pli_in_valid(noc_pli_in_valid), .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data), .noc_plo_in_valid(noc_plo_in_valid), .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data), .noc_plo_out_status(noc_plo_out_status), .noc_plo_out_valid(noc_plo_out_valid), .noc_plo_out_ready(noc_plo_out_ready)
    );

    initial begin
        command_mode=0; command_data=0;
        noc_ps_in_data=0; noc_ps_in_valid=0;
        noc_pd_in_data=0; noc_pd_in_valid=0;
        noc_pli_in_data=0; noc_pli_in_valid=0;
        noc_plo_in_data='0; noc_plo_in_valid=0; noc_plo_out_ready=1;

        @(posedge reset_n);
        noc_ps_in_valid=1; noc_ps_in_data=64'h1;
        @(posedge clk);
        noc_ps_in_valid=0;

        repeat(5) @(posedge clk);
        `TB_ASSERT(noc_ps_in_ready==1'b1, "NetworkOnChip should accept simple PS traffic");
        $display("tb_networkonchip PASS");
        $finish;
    end
endmodule
