//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_hybridacc_firmware_alu
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Top-level HybridAcc firmware validation for test_alu.
//                Flow:
//                  * build/load firmware image into external DRAM model
//                  * run SectionLoader through HybridAcc host registers
//                  * boot CoreMcu from ISRAM
//                  * validate DSRAM test result area written by firmware
// Dependencies:  tb_common.svh, full HybridAcc RTL stack,
//                and firmware mem image generated from
//                design/hybridacc-ESL/test/firmware/test_alu/test_alu.elf
// Revision:
//   2026/04/28 - Initial version
// Additional Comments:
//   Use +FW_MEM=<path> and +FW_BYTES=<n> to override the default firmware
//   image path and byte count when needed.
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
`include "../src/PE/SRAM_SP_BWEB.sv"
`include "../src/PE/TransformRegFile.sv"
`include "../src/PE/VADDU.sv"
`include "../src/PE/VMULU.sv"
`include "../src/PE/ProcessElement.sv"
`include "../src/NetworkOnChip.sv"
`include "../src/Cluster/ComputeCluster.sv"
`include "../src/HybridAcc.sv"
`endif

module tb_hybridacc_firmware_alu;
    import core_pkg::*;

    localparam int unsigned DRAM_WORDS       = 1024;
    localparam int unsigned MAX_FW_BYTES     = 4096;
    localparam int unsigned DEFAULT_FW_BYTES = 1184;
    localparam logic [31:0] FW_DRAM_BASE     = 32'h0000_0100;
    localparam int unsigned MAX_POLLS        = 4096;

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

    logic [63:0] dram_mem [0:DRAM_WORDS-1];
    byte unsigned fw_image[0:MAX_FW_BYTES-1];
    logic        dram_r_pending_reg;
    logic [31:0] dram_r_addr_reg;

    string fw_mem_path;
    integer fw_bytes;

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

    function automatic int unsigned dram_word_index(input logic [31:0] byte_addr);
        return (byte_addr >> 3) & (DRAM_WORDS - 1);
    endfunction

    function automatic logic [31:0] dsram_word(input int unsigned byte_offset);
        logic [31:0] value;
        begin
            value = 32'h0;
            for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
                value[byte_idx*8 +: 8] = dut.core_ctrl.dsram.mem[byte_offset + byte_idx];
            end
            return value;
        end
    endfunction

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

    task automatic load_firmware_into_dram;
        int unsigned first_word;
        begin
            fw_mem_path = "../hybridacc-ESL/test/firmware/test_alu/test_alu.mem";
            fw_bytes = DEFAULT_FW_BYTES;
            void'($value$plusargs("FW_MEM=%s", fw_mem_path));
            void'($value$plusargs("FW_BYTES=%d", fw_bytes));

            if ((fw_bytes <= 0) || ((fw_bytes % 4) != 0)) begin
                $fatal(1, "FW_BYTES must be a positive multiple of 4, got %0d", fw_bytes);
            end
            if (fw_bytes > MAX_FW_BYTES) begin
                $fatal(1, "FW_BYTES=%0d exceeds MAX_FW_BYTES=%0d", fw_bytes, MAX_FW_BYTES);
            end

            for (int idx = 0; idx < MAX_FW_BYTES; idx++) begin
                fw_image[idx] = 8'h00;
            end

            $readmemh(fw_mem_path, fw_image);

            first_word = FW_DRAM_BASE >> 3;
            if ((first_word + (fw_bytes / 4)) > DRAM_WORDS) begin
                $fatal(1, "Firmware image does not fit DRAM model: base_word=%0d words=%0d dram_words=%0d", first_word, fw_bytes / 4, DRAM_WORDS);
            end

            for (int word_idx = 0; word_idx < (fw_bytes / 4); word_idx++) begin
                dram_mem[first_word + word_idx] = {
                    32'h0000_0000,
                    fw_image[word_idx*4 + 3],
                    fw_image[word_idx*4 + 2],
                    fw_image[word_idx*4 + 1],
                    fw_image[word_idx*4 + 0]
                };
            end
        end
    endtask

    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
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
                dram_mem[dram_word_index(m_mem_axi_aw_addr_o)] <= m_mem_axi_w_data_o;
                m_mem_axi_b_valid_i <= 1'b1;
            end
            if (m_mem_axi_ar_valid_o && m_mem_axi_ar_ready_i) begin
                dram_r_pending_reg <= 1'b1;
                dram_r_addr_reg <= m_mem_axi_ar_addr_o;
            end
            if (dram_r_pending_reg && m_mem_axi_r_ready_o) begin
                dram_r_pending_reg <= 1'b0;
                m_mem_axi_r_valid_i <= 1'b1;
                m_mem_axi_r_data_i <= dram_mem[dram_word_index(dram_r_addr_reg)];
                m_mem_axi_r_resp_i <= 2'b00;
                m_mem_axi_r_last_i <= 1'b1;
            end
        end
    end

    initial begin
        logic [31:0] rd;
        logic loader_done;
        logic core_halted;
        logic [31:0] total_tests;
        logic [31:0] pass_tests;
        logic [31:0] fail_tests;
        logic [31:0] first_fail;

        for (int i = 0; i < DRAM_WORDS; i++) begin
            dram_mem[i] = 64'h0;
        end

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

        load_firmware_into_dram();

        host_write(MANIFEST_ADDR_LO, FW_DRAM_BASE);
        host_write(MANIFEST_SIZE, fw_bytes);
        host_write(MANIFEST_KICK, 32'h1);

        loader_done = 1'b0;
        for (int poll = 0; poll < MAX_POLLS; poll++) begin
            host_read(HACC_STATUS, rd);
            if (rd[1]) begin
                loader_done = 1'b1;
                break;
            end
        end
        `CHECK_BIT("HybridAcc firmware loader done", loader_done, 1'b1)

        host_write(CORE_BOOT_ADDR, 32'h0000_0000);
        host_write(HACC_CTRL, 32'h0000_0001);

        core_halted = 1'b0;
        for (int poll = 0; poll < MAX_POLLS; poll++) begin
            host_read(HACC_STATUS, rd);
            if (rd[2]) begin
                core_halted = 1'b1;
                break;
            end
        end
        `CHECK_BIT("HybridAcc firmware core halted", core_halted, 1'b1)

        host_read(CORE_CAUSE_SNAPSHOT, rd);
        `CHECK_VAL("HybridAcc firmware cause snapshot", rd, 32'd3)

        total_tests = dsram_word(0);
        pass_tests  = dsram_word(4);
        fail_tests  = dsram_word(8);
        first_fail  = dsram_word(12);

        `CHECK_BIT("HybridAcc firmware total tests nonzero", total_tests != 32'd0, 1'b1)
        `CHECK_VAL("HybridAcc firmware pass count", pass_tests, total_tests)
        `CHECK_VAL("HybridAcc firmware fail count", fail_tests, 32'd0)
        `CHECK_VAL("HybridAcc firmware first fail", first_fail, 32'd0)

        `TB_SUMMARY("tb_hybridacc_firmware_alu")
        $finish;
    end

    initial begin
        #5000000;
        $error("[TB_TIMEOUT] tb_hybridacc_firmware_alu did not finish in time");
        `TB_SUMMARY("tb_hybridacc_firmware_alu")
        $finish;
    end

endmodule