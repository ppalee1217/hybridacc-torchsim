//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_hybridacc_sim
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Top-level HybridAcc firmware validation harness.
//                Flow:
//                  * build/load firmware image into external DRAM model
//                  * run SectionLoader through HybridAcc host registers
//                  * boot CoreMcu from ISRAM
//                  * validate DSRAM test result area written by firmware
// Dependencies:  tb_common.svh, full HybridAcc RTL stack,
//                and a firmware mem image passed in through plusargs.
// Revision:
//   2026/04/28 - Initial version
// Additional Comments:
//   Use +FW_MEM=<path> and +FW_BYTES=<n> to override the default firmware
//   image path and byte count when needed. Optional workload payload/golden
//   files can be passed through +DRAM_MIRROR / +GOLDEN_OUTPUT plusargs.
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
`define HACC_SIM_DEBUG_READBACK
`include "../src/Core/DataSram.sv"
`undef HACC_SIM_DEBUG_READBACK
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

module tb_hybridacc_sim;
    import core_pkg::*;

    localparam logic [31:0] FW_DRAM_BASE     = 32'h0000_0100;
    localparam int unsigned MAX_FW_BYTES     = ISRAM_BYTES + DATA_SRAM_BYTES;
    localparam int unsigned FW_DRAM_WORDS    = (FW_DRAM_BASE >> 3) + (MAX_FW_BYTES >> 2);
    localparam int unsigned DEFAULT_FW_BYTES = 1184;
    localparam int unsigned DEFAULT_MAX_LOADER_CYCLES = MAX_FW_BYTES * 2;
    localparam int unsigned DEFAULT_MAX_CORE_CYCLES   = 500000;
    localparam logic [31:0] DEFAULT_DRAM_MIRROR_BASE  = 32'h8000_0000;

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

    logic [63:0] fw_dram_mem [0:FW_DRAM_WORDS-1];
    logic [63:0] ext_dram_mem[];
    byte unsigned fw_image[0:MAX_FW_BYTES-1];
    byte unsigned golden_output_image[];
    logic        dram_r_pending_reg;
    logic [31:0] dram_r_addr_reg;
    logic [31:0] last_retire_pc;

    string fw_mem_path;
    string dram_mirror_path;
    string golden_output_path;
    string actual_output_dump_path;
    integer fw_bytes;
    integer dram_mirror_bytes;
    integer golden_output_bytes;
    integer max_loader_cycles;
    integer max_core_cycles;
    logic [31:0] dram_mirror_base;
    logic [31:0] golden_output_base;

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
        return byte_addr >> 3;
    endfunction

    function automatic bit ext_dram_active;
        return (dram_mirror_bytes > 0);
    endfunction

    function automatic int unsigned ext_dram_word_index(input logic [31:0] byte_addr);
        return (byte_addr - dram_mirror_base) >> 3;
    endfunction

    function automatic logic [63:0] merge_write_strobes(
        input logic [63:0] old_data,
        input logic [63:0] new_data,
        input logic [7:0]  strb
    );
        logic [63:0] merged;
        begin
            merged = old_data;
            for (int byte_idx = 0; byte_idx < 8; byte_idx++) begin
                if (strb[byte_idx]) begin
                    merged[byte_idx*8 +: 8] = new_data[byte_idx*8 +: 8];
                end
            end
            return merged;
        end
    endfunction

    function automatic logic [63:0] dram_load_word(input logic [31:0] byte_addr);
        int unsigned word_idx;
        begin
            if (ext_dram_active() &&
                (byte_addr >= dram_mirror_base) &&
                (byte_addr < (dram_mirror_base + dram_mirror_bytes))) begin
                return ext_dram_mem[ext_dram_word_index(byte_addr)];
            end

            word_idx = dram_word_index(byte_addr);
            if (word_idx < FW_DRAM_WORDS) begin
                return fw_dram_mem[word_idx];
            end

            return 64'h0;
        end
    endfunction

    function automatic logic [7:0] dram_load_byte(input logic [31:0] byte_addr);
        logic [63:0] word_data;
        begin
            word_data = dram_load_word({byte_addr[31:3], 3'b000});
            return word_data[{byte_addr[2:0], 3'b000} +: 8];
        end
    endfunction

    task automatic dram_store_word(input logic [31:0] byte_addr, input logic [63:0] word_data);
        int unsigned word_idx;
        int unsigned ext_word_idx;
        begin
            if (ext_dram_active() &&
                (byte_addr >= dram_mirror_base) &&
                (byte_addr < (dram_mirror_base + dram_mirror_bytes))) begin
                ext_word_idx = ext_dram_word_index(byte_addr);
                if (ext_word_idx >= ext_dram_mem.size()) begin
                    $fatal(1, "External DRAM write out of range: addr=0x%08x base=0x%08x bytes=%0d", byte_addr, dram_mirror_base, dram_mirror_bytes);
                end
                ext_dram_mem[ext_word_idx] = word_data;
                return;
            end

            word_idx = dram_word_index(byte_addr);
            if (word_idx < FW_DRAM_WORDS) begin
                fw_dram_mem[word_idx] = word_data;
                return;
            end

            $fatal(1, "DRAM write out of range: addr=0x%08x", byte_addr);
        end
    endtask

    task automatic dump_output_region;
        integer dump_fd;
        logic [7:0] dump_byte;
        begin
            actual_output_dump_path = "";
            void'($value$plusargs("ACTUAL_OUTPUT_DUMP=%s", actual_output_dump_path));
            if (actual_output_dump_path == "") begin
                return;
            end
            if (golden_output_bytes <= 0) begin
                $fatal(1, "ACTUAL_OUTPUT_DUMP requires GOLDEN_OUTPUT_BYTES/GOLDEN_OUTPUT_BASE to be set");
            end

            dump_fd = $fopen(actual_output_dump_path, "wb");
            if (dump_fd == 0) begin
                $fatal(1, "Failed to open ACTUAL_OUTPUT_DUMP path: %0s", actual_output_dump_path);
            end

            for (int byte_idx = 0; byte_idx < golden_output_bytes; byte_idx++) begin
                dump_byte = dram_load_byte(golden_output_base + byte_idx);
                $fwrite(dump_fd, "%c", dump_byte);
            end
            $fclose(dump_fd);

            $display("[TB] Dumped actual output path=%0s bytes=%0d base=0x%08x",
                     actual_output_dump_path, golden_output_bytes, golden_output_base);
        end
    endtask

    function automatic logic [31:0] dsram_word(input int unsigned byte_offset);
        begin
`ifndef GATE_SIM
            return dut.core_ctrl.dsram.debug_read_word(byte_offset);
`else
            $fatal(1, "dsram_word debug readback is unavailable in GATE_SIM");
            return 32'h0;
`endif
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

    task automatic read_binary_file(
        input string path,
        input int unsigned expected_bytes,
        output byte unsigned data[]
    );
        int fd;
        int bytes_read;
        begin
            if (expected_bytes == 0) begin
                $fatal(1, "Binary file %s has zero expected bytes", path);
            end

            data = new[expected_bytes];
            fd = $fopen(path, "rb");
            if (fd == 0) begin
                $fatal(1, "Failed to open binary file: %s", path);
            end

            bytes_read = $fread(data, fd);
            $fclose(fd);

            if (bytes_read != expected_bytes) begin
                $fatal(1, "Binary file %s size mismatch: got=%0d want=%0d", path, bytes_read, expected_bytes);
            end
        end
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
            if ((first_word + (fw_bytes / 4)) > FW_DRAM_WORDS) begin
                $fatal(1, "Firmware image does not fit DRAM model: base_word=%0d words=%0d dram_words=%0d", first_word, fw_bytes / 4, FW_DRAM_WORDS);
            end

            for (int word_idx = 0; word_idx < (fw_bytes / 4); word_idx++) begin
                fw_dram_mem[first_word + word_idx] = {
                    32'h0000_0000,
                    fw_image[word_idx*4 + 3],
                    fw_image[word_idx*4 + 2],
                    fw_image[word_idx*4 + 1],
                    fw_image[word_idx*4 + 0]
                };
            end
        end
    endtask

    task automatic load_external_dram_mirror;
        byte unsigned dram_mirror_image[];
        logic [63:0] word_data;
        int unsigned ext_word_count;
        begin
            dram_mirror_path = "";
            dram_mirror_bytes = 0;
            dram_mirror_base = DEFAULT_DRAM_MIRROR_BASE;

            void'($value$plusargs("DRAM_MIRROR=%s", dram_mirror_path));
            void'($value$plusargs("DRAM_MIRROR_BYTES=%d", dram_mirror_bytes));
            void'($value$plusargs("DRAM_MIRROR_BASE=%h", dram_mirror_base));

            if (dram_mirror_path == "") begin
                ext_dram_mem = new[0];
                return;
            end

            if (dram_mirror_bytes <= 0) begin
                $fatal(1, "DRAM_MIRROR_BYTES must be positive when DRAM_MIRROR is set");
            end

            ext_word_count = (dram_mirror_bytes + 7) >> 3;
            ext_dram_mem = new[ext_word_count];
            for (int unsigned word_idx = 0; word_idx < ext_word_count; word_idx++) begin
                ext_dram_mem[word_idx] = 64'h0;
            end

            read_binary_file(dram_mirror_path, dram_mirror_bytes, dram_mirror_image);

            for (int unsigned word_idx = 0; word_idx < ext_word_count; word_idx++) begin
                word_data = 64'h0;
                for (int byte_idx = 0; byte_idx < 8; byte_idx++) begin
                    int unsigned byte_offset;
                    byte_offset = word_idx * 8 + byte_idx;
                    if (byte_offset < dram_mirror_bytes) begin
                        word_data[byte_idx*8 +: 8] = dram_mirror_image[byte_offset];
                    end
                end
                ext_dram_mem[word_idx] = word_data;
            end

            $display("[TB] Loaded DRAM mirror path=%0s bytes=%0d base=0x%08x words=%0d",
                     dram_mirror_path, dram_mirror_bytes, dram_mirror_base, ext_word_count);
        end
    endtask

    task automatic load_golden_output;
        begin
            golden_output_path = "";
            golden_output_bytes = 0;
            golden_output_base = 32'h0;
            golden_output_image = new[0];

            void'($value$plusargs("GOLDEN_OUTPUT=%s", golden_output_path));
            if (golden_output_path == "") begin
                return;
            end

            if (!$value$plusargs("GOLDEN_OUTPUT_BYTES=%d", golden_output_bytes)) begin
                $fatal(1, "GOLDEN_OUTPUT_BYTES must be set when GOLDEN_OUTPUT is provided");
            end
            if (!$value$plusargs("GOLDEN_OUTPUT_BASE=%h", golden_output_base)) begin
                $fatal(1, "GOLDEN_OUTPUT_BASE must be set when GOLDEN_OUTPUT is provided");
            end

            read_binary_file(golden_output_path, golden_output_bytes, golden_output_image);

            $display("[TB] Loaded golden output path=%0s bytes=%0d base=0x%08x",
                     golden_output_path, golden_output_bytes, golden_output_base);
        end
    endtask

    task automatic compare_output_against_golden(output logic match);
        int mismatch_count;
        int first_mismatch_byte;
        int printed_mismatches;
        logic [7:0] actual_byte;
        logic [7:0] expected_byte;
        begin
            match = 1'b1;
            if (golden_output_bytes <= 0) begin
                return;
            end

            mismatch_count = 0;
            first_mismatch_byte = -1;
            printed_mismatches = 0;
            for (int byte_idx = 0; byte_idx < golden_output_bytes; byte_idx++) begin
                expected_byte = golden_output_image[byte_idx];
                actual_byte = dram_load_byte(golden_output_base + byte_idx);
                if (actual_byte !== expected_byte) begin
                    mismatch_count++;
                    if (first_mismatch_byte < 0) begin
                        first_mismatch_byte = byte_idx;
                    end
                    if (printed_mismatches < 8) begin
                        $display("[TB_GOLDEN][DIFF] idx=%0d addr=0x%08x expected=0x%02x actual=0x%02x",
                                 byte_idx,
                                 golden_output_base + byte_idx,
                                 expected_byte,
                                 actual_byte);
                        printed_mismatches++;
                    end
                end
            end

            if (mismatch_count != 0) begin
                match = 1'b0;
                $display("[TB_GOLDEN] mismatch_count=%0d first_mismatch_byte=%0d addr=0x%08x expected=0x%02x actual=0x%02x",
                         mismatch_count,
                         first_mismatch_byte,
                         golden_output_base + first_mismatch_byte,
                         golden_output_image[first_mismatch_byte],
                         dram_load_byte(golden_output_base + first_mismatch_byte));
            end else begin
                $display("[TB_GOLDEN] exact_match bytes=%0d base=0x%08x",
                         golden_output_bytes,
                         golden_output_base);
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
                dram_store_word(
                    m_mem_axi_aw_addr_o,
                    merge_write_strobes(
                        dram_load_word(m_mem_axi_aw_addr_o),
                        m_mem_axi_w_data_o,
                        m_mem_axi_w_strb_o
                    )
                );
                m_mem_axi_b_valid_i <= 1'b1;
            end
            if (m_mem_axi_ar_valid_o && m_mem_axi_ar_ready_i) begin
                dram_r_pending_reg <= 1'b1;
                dram_r_addr_reg <= m_mem_axi_ar_addr_o;
            end
            if (dram_r_pending_reg && m_mem_axi_r_ready_o) begin
                dram_r_pending_reg <= 1'b0;
                m_mem_axi_r_valid_i <= 1'b1;
                m_mem_axi_r_data_i <= dram_load_word(dram_r_addr_reg);
                m_mem_axi_r_resp_i <= 2'b00;
                m_mem_axi_r_last_i <= 1'b1;
            end
        end
    end

    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            last_retire_pc <= 32'h0;
        end else if (dut.core_ctrl.mcu_retire_valid_w) begin
            last_retire_pc <= dut.core_ctrl.mcu_retire_pc_w;
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
        logic golden_match;
        logic skip_fw_test_summary;
        logic skip_golden_exact_check;

        for (int i = 0; i < FW_DRAM_WORDS; i++) begin
            fw_dram_mem[i] = 64'h0;
        end
        ext_dram_mem = new[0];
        golden_output_image = new[0];

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

        max_loader_cycles = DEFAULT_MAX_LOADER_CYCLES;
        if (!$value$plusargs("MAX_LOADER_CYCLES=%d", max_loader_cycles)) begin
            if (!$value$plusargs("MAX_CYCLES=%d", max_loader_cycles)) begin
                void'($value$plusargs("MAX_POLLS=%d", max_loader_cycles));
            end
        end

        max_core_cycles = DEFAULT_MAX_CORE_CYCLES;
        if (!$value$plusargs("MAX_CORE_CYCLES=%d", max_core_cycles)) begin
            if (!$value$plusargs("MAX_CYCLES=%d", max_core_cycles)) begin
                void'($value$plusargs("MAX_POLLS=%d", max_core_cycles));
            end
        end

        load_firmware_into_dram();
        load_external_dram_mirror();
        load_golden_output();
        skip_fw_test_summary = $test$plusargs("SKIP_FW_TEST_SUMMARY");
        skip_golden_exact_check = $test$plusargs("SKIP_GOLDEN_EXACT_CHECK");

        host_write(MANIFEST_ADDR_LO, FW_DRAM_BASE);
        host_write(MANIFEST_SIZE, fw_bytes);
        host_write(MANIFEST_KICK, 32'h1);

        loader_done = 1'b0;
        for (int cycle = 0; cycle < max_loader_cycles; cycle++) begin
            @(posedge clk);
            if (dut.core_ctrl.loader_done_w) begin
                loader_done = 1'b1;
                break;
            end
        end
        `CHECK_BIT("HybridAcc firmware loader done", loader_done, 1'b1)
        if (!loader_done) begin
            `TB_SUMMARY("tb_hybridacc_sim")
            $finish;
        end

        host_write(CORE_BOOT_ADDR, 32'h0000_0000);
        host_write(HACC_CTRL, 32'h0000_0001);

        core_halted = 1'b0;
        for (int cycle = 0; cycle < max_core_cycles; cycle++) begin
            @(posedge clk);
            if (dut.core_ctrl.mcu_halted_w) begin
                core_halted = 1'b1;
                break;
            end
        end
        `CHECK_BIT("HybridAcc firmware core halted", core_halted, 1'b1)
        if (!core_halted) begin
            `TB_SUMMARY("tb_hybridacc_sim")
            $finish;
        end

        host_read(CORE_CAUSE_SNAPSHOT, rd);
        `CHECK_VAL("HybridAcc firmware cause snapshot", rd, 32'd3)

        total_tests = dsram_word(0);
        pass_tests  = dsram_word(4);
        fail_tests  = dsram_word(8);
        first_fail  = dsram_word(12);

        if (!skip_fw_test_summary) begin
            `CHECK_BIT("HybridAcc firmware total tests nonzero", total_tests != 32'd0, 1'b1)
            `CHECK_VAL("HybridAcc firmware pass count", pass_tests, total_tests)
            `CHECK_VAL("HybridAcc firmware fail count", fail_tests, 32'd0)
            `CHECK_VAL("HybridAcc firmware first fail", first_fail, 32'd0)
        end else begin
            $display("[TB] Skipping firmware summary checks: total_tests=%0d pass_tests=%0d fail_tests=%0d first_fail=%0d",
                     total_tests,
                     pass_tests,
                     fail_tests,
                     first_fail);
        end

        dump_output_region();
        if (!skip_golden_exact_check) begin
            compare_output_against_golden(golden_match);
            `CHECK_BIT("HybridAcc golden output exact match", golden_match, 1'b1)
        end else begin
            golden_match = 1'b1;
            $display("[TB] Skipping exact golden compare; use external fp16 comparator for regression verdict");
        end

        $display("[TB_RESULT] tb_hybridacc_sim instret=%0d mcycle=%0d last_retire_pc=0x%08x pass_tests=%0d total_tests=%0d fail_tests=%0d first_fail=%0d",
             dut.core_ctrl.core_mcu.instret_reg,
             dut.core_ctrl.core_mcu.cycle_reg,
             last_retire_pc,
             pass_tests,
             total_tests,
             fail_tests,
             first_fail);

        `TB_SUMMARY("tb_hybridacc_sim")
        $finish;
    end

    initial begin
        #5000000;
        $error("[TB_TIMEOUT] tb_hybridacc_sim did not finish in time");
        `TB_SUMMARY("tb_hybridacc_sim")
        $finish;
    end

endmodule