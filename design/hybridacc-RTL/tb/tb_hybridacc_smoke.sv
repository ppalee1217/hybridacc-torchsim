//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_hybridacc_smoke
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Top-level HybridAcc smoke test.
// Dependencies:  tb_common.svh, Core/Cluster source stack, HybridAcc.sv.
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "tb_common.svh"
`ifndef GATE_SIM
`include "../src/hybridacc_utils_pkg.sv"
`include "../src/FIFO.sv"
`include "../src/asyncFIFO.sv"
`include "../src/Cluster/cluster_pkg.sv"
`include "../src/Cluster/AddressGenerateUnit.sv"
`include "../src/Cluster/ScratchpadMemory.sv"
`include "../src/Cluster/HybridDataDeliverUnit.sv"
`include "../src/Cluster/ClusterControlUnit.sv"
`include "../src/Core/core_pkg.sv"
`include "../src/Core/Isram.sv"
`include "../src/Core/DataSram.sv"
`include "../src/Core/CoreLocalIrq.sv"
`include "../src/Core/Plic.sv"
`include "../src/Core/BootHostIf.sv"
`include "../src/Core/CmdFabric.sv"
`include "../src/Core/CmdToAhbBridge.sv"
`include "../src/Core/ClusterDataFabric.sv"
`include "../src/Core/DmaEngine.sv"
`include "../src/Core/CoreMcu.sv"
`include "../src/Core/SectionLoader.sv"
`include "../src/Core/CoreController.sv"
`include "../src/NoC/MBUS.sv"
`include "../src/NoC/NoCRouter.sv"
`include "../src/PE/DataMemory.sv"
`include "../src/PE/Decoder.sv"
`include "../src/PE/EXE_A_Stage.sv"
`include "../src/PE/EXE_M_Stage.sv"
`include "../src/PE/IF_ID_Stage.sv"
`include "../src/PE/InstructionMemory.sv"
`include "../src/PE/LDMA.sv"
`include "../src/PE/LoopController.sv"
`include "../src/PE/PErouter.sv"
`include "../src/PE/PsumRegFile.sv"
`include "../src/PE/SDMA.sv"
`include "../src/PE/TransformRegFile.sv"
`include "../src/PE/VADDU.sv"
`include "../src/PE/VMULU.sv"
`include "../src/PE/ProcessElement.sv"
`include "../src/NetworkOnChip.sv"
`include "../src/Cluster/ComputeCluster.sv"
`include "../src/HybridAcc.sv"
`endif

module tb_hybridacc_smoke;
    import core_pkg::*;

    logic clk, reset_n;
    logic s_ctrl_aw_valid_i;
    logic s_ctrl_aw_ready_o;
    logic [31:0] s_ctrl_aw_addr_i;
    logic s_ctrl_w_valid_i;
    logic s_ctrl_w_ready_o;
    logic [31:0] s_ctrl_w_data_i;
    logic [3:0] s_ctrl_w_strb_i;
    logic s_ctrl_b_valid_o;
    logic s_ctrl_b_ready_i;
    logic [1:0] s_ctrl_b_resp_o;
    logic s_ctrl_ar_valid_i;
    logic s_ctrl_ar_ready_o;
    logic [31:0] s_ctrl_ar_addr_i;
    logic s_ctrl_r_valid_o;
    logic s_ctrl_r_ready_i;
    logic [31:0] s_ctrl_r_data_o;
    logic [1:0] s_ctrl_r_resp_o;
    logic m_mem_axi_aw_valid_o;
    logic m_mem_axi_aw_ready_i;
    logic [31:0] m_mem_axi_aw_addr_o;
    logic [7:0] m_mem_axi_aw_len_o;
    logic m_mem_axi_w_valid_o;
    logic m_mem_axi_w_ready_i;
    logic [63:0] m_mem_axi_w_data_o;
    logic [7:0] m_mem_axi_w_strb_o;
    logic m_mem_axi_w_last_o;
    logic m_mem_axi_b_valid_i;
    logic m_mem_axi_b_ready_o;
    logic [1:0] m_mem_axi_b_resp_i;
    logic m_mem_axi_ar_valid_o;
    logic m_mem_axi_ar_ready_i;
    logic [31:0] m_mem_axi_ar_addr_o;
    logic [7:0] m_mem_axi_ar_len_o;
    logic m_mem_axi_r_valid_i;
    logic m_mem_axi_r_ready_o;
    logic [63:0] m_mem_axi_r_data_i;
    logic [1:0] m_mem_axi_r_resp_i;
    logic m_mem_axi_r_last_i;
    logic controller_irq_o;

    logic [63:0] dram_mem [0:255];
    logic        dram_r_pending_reg;
    logic [31:0] dram_r_addr_reg;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    HybridAcc #(
        .NUM_CLUSTERS(1)
    ) dut (
        .clk(clk),
        .reset_n(reset_n),
        .s_ctrl_aw_valid_i(s_ctrl_aw_valid_i),
        .s_ctrl_aw_ready_o(s_ctrl_aw_ready_o),
        .s_ctrl_aw_addr_i(s_ctrl_aw_addr_i),
        .s_ctrl_w_valid_i(s_ctrl_w_valid_i),
        .s_ctrl_w_ready_o(s_ctrl_w_ready_o),
        .s_ctrl_w_data_i(s_ctrl_w_data_i),
        .s_ctrl_w_strb_i(s_ctrl_w_strb_i),
        .s_ctrl_b_valid_o(s_ctrl_b_valid_o),
        .s_ctrl_b_ready_i(s_ctrl_b_ready_i),
        .s_ctrl_b_resp_o(s_ctrl_b_resp_o),
        .s_ctrl_ar_valid_i(s_ctrl_ar_valid_i),
        .s_ctrl_ar_ready_o(s_ctrl_ar_ready_o),
        .s_ctrl_ar_addr_i(s_ctrl_ar_addr_i),
        .s_ctrl_r_valid_o(s_ctrl_r_valid_o),
        .s_ctrl_r_ready_i(s_ctrl_r_ready_i),
        .s_ctrl_r_data_o(s_ctrl_r_data_o),
        .s_ctrl_r_resp_o(s_ctrl_r_resp_o),
        .m_mem_axi_aw_valid_o(m_mem_axi_aw_valid_o),
        .m_mem_axi_aw_ready_i(m_mem_axi_aw_ready_i),
        .m_mem_axi_aw_addr_o(m_mem_axi_aw_addr_o),
        .m_mem_axi_aw_len_o(m_mem_axi_aw_len_o),
        .m_mem_axi_w_valid_o(m_mem_axi_w_valid_o),
        .m_mem_axi_w_ready_i(m_mem_axi_w_ready_i),
        .m_mem_axi_w_data_o(m_mem_axi_w_data_o),
        .m_mem_axi_w_strb_o(m_mem_axi_w_strb_o),
        .m_mem_axi_w_last_o(m_mem_axi_w_last_o),
        .m_mem_axi_b_valid_i(m_mem_axi_b_valid_i),
        .m_mem_axi_b_ready_o(m_mem_axi_b_ready_o),
        .m_mem_axi_b_resp_i(m_mem_axi_b_resp_i),
        .m_mem_axi_ar_valid_o(m_mem_axi_ar_valid_o),
        .m_mem_axi_ar_ready_i(m_mem_axi_ar_ready_i),
        .m_mem_axi_ar_addr_o(m_mem_axi_ar_addr_o),
        .m_mem_axi_ar_len_o(m_mem_axi_ar_len_o),
        .m_mem_axi_r_valid_i(m_mem_axi_r_valid_i),
        .m_mem_axi_r_ready_o(m_mem_axi_r_ready_o),
        .m_mem_axi_r_data_i(m_mem_axi_r_data_i),
        .m_mem_axi_r_resp_i(m_mem_axi_r_resp_i),
        .m_mem_axi_r_last_i(m_mem_axi_r_last_i),
        .controller_irq_o(controller_irq_o)
    );

    task automatic host_write(input logic [31:0] addr, input logic [31:0] data);
        @(negedge clk);
        s_ctrl_aw_addr_i = addr;
        s_ctrl_w_data_i = data;
        s_ctrl_w_strb_i = 4'hF;
        s_ctrl_aw_valid_i = 1'b1;
        s_ctrl_w_valid_i = 1'b1;
        s_ctrl_b_ready_i = 1'b1;
        while (!(s_ctrl_aw_ready_o && s_ctrl_w_ready_o)) @(posedge clk);
        @(posedge clk);
        s_ctrl_aw_valid_i = 1'b0;
        s_ctrl_w_valid_i = 1'b0;
        while (!s_ctrl_b_valid_o) @(posedge clk);
        @(posedge clk);
        s_ctrl_b_ready_i = 1'b0;
    endtask

    task automatic host_read(input logic [31:0] addr, output logic [31:0] data);
        @(negedge clk);
        s_ctrl_ar_addr_i = addr;
        s_ctrl_ar_valid_i = 1'b1;
        s_ctrl_r_ready_i = 1'b1;
        while (!s_ctrl_ar_ready_o) @(posedge clk);
        @(posedge clk);
        s_ctrl_ar_valid_i = 1'b0;
        while (!s_ctrl_r_valid_o) @(posedge clk);
        #(`TB_SETTLE);
        data = s_ctrl_r_data_o;
        @(posedge clk);
        s_ctrl_r_ready_i = 1'b0;
    endtask

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int i = 0; i < 256; i++) begin
                dram_mem[i] <= 64'h0;
            end
            dram_mem[32'h100 >> 3] <= 64'h0000_0000_0010_0073;
            dram_r_pending_reg <= 1'b0;
            dram_r_addr_reg <= 32'h0;
            m_mem_axi_aw_ready_i <= 1'b1;
            m_mem_axi_w_ready_i <= 1'b1;
            m_mem_axi_b_valid_i <= 1'b0;
            m_mem_axi_b_resp_i <= 2'b00;
            m_mem_axi_ar_ready_i <= 1'b1;
            m_mem_axi_r_valid_i <= 1'b0;
            m_mem_axi_r_data_i <= 64'h0;
            m_mem_axi_r_resp_i <= 2'b00;
            m_mem_axi_r_last_i <= 1'b0;
        end else begin
            m_mem_axi_b_valid_i <= 1'b0;
            m_mem_axi_r_valid_i <= 1'b0;
            m_mem_axi_r_last_i <= 1'b0;
            if (m_mem_axi_aw_valid_o && m_mem_axi_w_valid_o) begin
                dram_mem[m_mem_axi_aw_addr_o[9:3]] <= m_mem_axi_w_data_o;
                m_mem_axi_b_valid_i <= 1'b1;
            end
            if (m_mem_axi_ar_valid_o && m_mem_axi_ar_ready_i) begin
                dram_r_pending_reg <= 1'b1;
                dram_r_addr_reg <= m_mem_axi_ar_addr_o;
            end
            if (dram_r_pending_reg && m_mem_axi_r_ready_o) begin
                dram_r_pending_reg <= 1'b0;
                m_mem_axi_r_valid_i <= 1'b1;
                m_mem_axi_r_data_i <= dram_mem[dram_r_addr_reg[9:3]];
                m_mem_axi_r_resp_i <= 2'b00;
                m_mem_axi_r_last_i <= 1'b1;
            end
        end
    end

    initial begin
        s_ctrl_aw_valid_i = 1'b0;
        s_ctrl_aw_addr_i = 32'h0;
        s_ctrl_w_valid_i = 1'b0;
        s_ctrl_w_data_i = 32'h0;
        s_ctrl_w_strb_i = 4'h0;
        s_ctrl_b_ready_i = 1'b0;
        s_ctrl_ar_valid_i = 1'b0;
        s_ctrl_ar_addr_i = 32'h0;
        s_ctrl_r_ready_i = 1'b0;

        @(posedge reset_n);
        @(posedge clk);

        begin
            logic [31:0] rd;
            host_write(MANIFEST_ADDR_LO, 32'h0000_0100);
            host_write(MANIFEST_SIZE, 32'd4);
            host_write(MANIFEST_KICK, 32'h1);
            repeat (6) @(posedge clk);
            host_read(HACC_STATUS, rd);
            `CHECK_BIT("HybridAcc loader done status", rd[1], 1'b1)

            host_write(CORE_BOOT_ADDR, 32'h0000_0000);
            host_write(HACC_CTRL, 32'h0000_0001);
            repeat (4) @(posedge clk);
            host_read(CORE_CAUSE_SNAPSHOT, rd);
            `CHECK_VAL("HybridAcc cause snapshot after EBREAK", rd, 32'd3)
        end

        `TB_SUMMARY("tb_hybridacc_smoke")
        $finish;
    end

    initial begin
        #1000000;
        $error("[TB_TIMEOUT] tb_hybridacc_smoke did not finish in time");
        `TB_SUMMARY("tb_hybridacc_smoke")
        $finish;
    end

endmodule