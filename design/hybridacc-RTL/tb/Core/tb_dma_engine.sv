//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_dma_engine
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   DMA engine smoke test for single-beat DRAM->cluster copy.
// Dependencies:  ../tb_common.svh and Core DMA source files.
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/Core/core_pkg.sv"
`include "../../src/Core/DmaEngine.sv"
`endif

module tb_dma_engine;
    import core_pkg::*;

    logic clk, reset_n;
    logic mmio_req_valid_i;
    logic mmio_req_write_i;
    logic [31:0] mmio_req_addr_i;
    logic [31:0] mmio_req_wdata_i;
    logic mmio_resp_valid_o;
    logic [31:0] mmio_resp_rdata_o;
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
    logic [1:0] m_mem_axi_b_resp_i;
    logic m_mem_axi_b_ready_o;
    logic m_mem_axi_ar_valid_o;
    logic m_mem_axi_ar_ready_i;
    logic [31:0] m_mem_axi_ar_addr_o;
    logic [7:0] m_mem_axi_ar_len_o;
    logic m_mem_axi_r_valid_i;
    logic m_mem_axi_r_ready_o;
    logic [63:0] m_mem_axi_r_data_i;
    logic [1:0] m_mem_axi_r_resp_i;
    logic m_mem_axi_r_last_i;
    logic m_cl_axi_aw_valid_o;
    logic m_cl_axi_aw_ready_i;
    logic [31:0] m_cl_axi_aw_addr_o;
    logic m_cl_axi_w_valid_o;
    logic m_cl_axi_w_ready_i;
    logic [63:0] m_cl_axi_w_data_o;
    logic [7:0] m_cl_axi_w_strb_o;
    logic m_cl_axi_b_valid_i;
    logic [1:0] m_cl_axi_b_resp_i;
    logic m_cl_axi_b_ready_o;
    logic m_cl_axi_ar_valid_o;
    logic m_cl_axi_ar_ready_i;
    logic [31:0] m_cl_axi_ar_addr_o;
    logic m_cl_axi_r_valid_i;
    logic m_cl_axi_r_ready_o;
    logic [63:0] m_cl_axi_r_data_i;
    logic [1:0] m_cl_axi_r_resp_i;
    logic dma_irq_o;

    logic [63:0] dram_mem [0:255];
    logic [63:0] cluster_mem [0:255];
    logic dram_r_pending_reg;
    logic [31:0] dram_r_addr_reg;
    logic cl_b_pending_reg;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    DmaEngine dut (
        .clk(clk),
        .reset_n(reset_n),
        .mmio_req_valid_i(mmio_req_valid_i),
        .mmio_req_write_i(mmio_req_write_i),
        .mmio_req_addr_i(mmio_req_addr_i),
        .mmio_req_wdata_i(mmio_req_wdata_i),
        .mmio_resp_valid_o(mmio_resp_valid_o),
        .mmio_resp_rdata_o(mmio_resp_rdata_o),
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
        .m_cl_axi_aw_valid_o(m_cl_axi_aw_valid_o),
        .m_cl_axi_aw_ready_i(m_cl_axi_aw_ready_i),
        .m_cl_axi_aw_addr_o(m_cl_axi_aw_addr_o),
        .m_cl_axi_w_valid_o(m_cl_axi_w_valid_o),
        .m_cl_axi_w_ready_i(m_cl_axi_w_ready_i),
        .m_cl_axi_w_data_o(m_cl_axi_w_data_o),
        .m_cl_axi_w_strb_o(m_cl_axi_w_strb_o),
        .m_cl_axi_b_valid_i(m_cl_axi_b_valid_i),
        .m_cl_axi_b_ready_o(m_cl_axi_b_ready_o),
        .m_cl_axi_b_resp_i(m_cl_axi_b_resp_i),
        .m_cl_axi_ar_valid_o(m_cl_axi_ar_valid_o),
        .m_cl_axi_ar_ready_i(m_cl_axi_ar_ready_i),
        .m_cl_axi_ar_addr_o(m_cl_axi_ar_addr_o),
        .m_cl_axi_r_valid_i(m_cl_axi_r_valid_i),
        .m_cl_axi_r_ready_o(m_cl_axi_r_ready_o),
        .m_cl_axi_r_data_i(m_cl_axi_r_data_i),
        .m_cl_axi_r_resp_i(m_cl_axi_r_resp_i),
        .dma_irq_o(dma_irq_o)
    );

    task automatic mmio_write(input logic [31:0] addr, input logic [31:0] data);
        @(negedge clk);
        mmio_req_addr_i = addr;
        mmio_req_wdata_i = data;
        mmio_req_write_i = 1'b1;
        mmio_req_valid_i = 1'b1;
        @(posedge clk);
        @(negedge clk);
        mmio_req_valid_i = 1'b0;
        mmio_req_write_i = 1'b0;
    endtask

    task automatic mmio_read(input logic [31:0] addr, output logic [31:0] data);
        @(negedge clk);
        mmio_req_addr_i = addr;
        mmio_req_write_i = 1'b0;
        mmio_req_valid_i = 1'b1;
        #(`TB_SETTLE);
        data = mmio_resp_rdata_o;
        @(posedge clk);
        @(negedge clk);
        mmio_req_valid_i = 1'b0;
    endtask

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int i = 0; i < 256; i++) begin
                dram_mem[i] <= 64'h0;
                cluster_mem[i] <= 64'h0;
            end
            dram_mem[32'h20 >> 3] <= 64'h1122_3344_5566_7788;
            m_mem_axi_aw_ready_i <= 1'b1;
            m_mem_axi_w_ready_i <= 1'b1;
            m_mem_axi_b_valid_i <= 1'b0;
            m_mem_axi_b_resp_i <= 2'b00;
            m_mem_axi_ar_ready_i <= 1'b1;
            m_mem_axi_r_valid_i <= 1'b0;
            m_mem_axi_r_data_i <= 64'h0;
            m_mem_axi_r_resp_i <= 2'b00;
            m_mem_axi_r_last_i <= 1'b0;
            m_cl_axi_aw_ready_i <= 1'b1;
            m_cl_axi_w_ready_i <= 1'b1;
            m_cl_axi_b_valid_i <= 1'b0;
            m_cl_axi_b_resp_i <= 2'b00;
            m_cl_axi_ar_ready_i <= 1'b1;
            m_cl_axi_r_valid_i <= 1'b0;
            m_cl_axi_r_data_i <= 64'h0;
            m_cl_axi_r_resp_i <= 2'b00;
            dram_r_pending_reg <= 1'b0;
            dram_r_addr_reg <= 32'h0;
            cl_b_pending_reg <= 1'b0;
        end else begin
            m_mem_axi_b_valid_i <= 1'b0;
            m_mem_axi_r_valid_i <= 1'b0;
            m_mem_axi_r_last_i <= 1'b0;
            m_cl_axi_b_valid_i <= 1'b0;
            m_cl_axi_r_valid_i <= 1'b0;

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

            if (m_mem_axi_aw_valid_o && m_mem_axi_w_valid_o) begin
                dram_mem[m_mem_axi_aw_addr_o[9:3]] <= m_mem_axi_w_data_o;
                m_mem_axi_b_valid_i <= 1'b1;
            end

            if (m_cl_axi_ar_valid_o && m_cl_axi_ar_ready_i) begin
                m_cl_axi_r_valid_i <= 1'b1;
                m_cl_axi_r_data_i <= cluster_mem[m_cl_axi_ar_addr_o[9:3]];
                m_cl_axi_r_resp_i <= 2'b00;
            end
            if (m_cl_axi_aw_valid_o && m_cl_axi_w_valid_o) begin
                cluster_mem[m_cl_axi_aw_addr_o[9:3]] <= m_cl_axi_w_data_o;
                cl_b_pending_reg <= 1'b1;
            end
            if (cl_b_pending_reg) begin
                cl_b_pending_reg <= 1'b0;
                m_cl_axi_b_valid_i <= 1'b1;
                m_cl_axi_b_resp_i <= 2'b00;
            end
        end
    end

    initial begin
        mmio_req_valid_i = 1'b0;
        mmio_req_write_i = 1'b0;
        mmio_req_addr_i = 32'h0;
        mmio_req_wdata_i = 32'h0;

        @(posedge reset_n);
        @(posedge clk);

        begin
            logic [31:0] rd;
            mmio_write(DMA_SRC_KIND, DMA_EP_DRAM);
            mmio_write(DMA_DST_KIND, DMA_EP_CLUSTER_SPM);
            mmio_write(DMA_SRC_ADDR_LO, 32'h0000_0020);
            mmio_write(DMA_DST_ADDR_LO, 32'h0000_0040);
            mmio_write(DMA_DST_CLUSTER_ID, 32'h0);
            mmio_write(DMA_COUNT_D0, 32'd1);
            mmio_write(DMA_CMD_TAG, 32'h55);
            mmio_write(DMA_CTRL, 32'h1);

            wait (dma_irq_o === 1'b1);
            mmio_read(DMA_DONE_TAG, rd);
            `CHECK_VAL("DMA done tag", rd, 32'h55)
            `CHECK_VAL("DMA copied data", cluster_mem[32'h40 >> 3], 64'h1122_3344_5566_7788)
        end

        `TB_SUMMARY("tb_dma_engine")
        $finish;
    end

    initial begin
        #1000000;
        $error("[TB_TIMEOUT] tb_dma_engine did not finish in time");
        `TB_SUMMARY("tb_dma_engine")
        $finish;
    end

endmodule