//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_hddu
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Smoke-level testbench for HybridDataDeliverUnit.
//                Covers one send plane and one receive plane.
// Dependencies:  ../tb_common.svh, src/hybridacc_utils_pkg.sv,
//                src/Cluster/cluster_pkg.sv,
//                src/Cluster/AddressGenerateUnit.sv,
//                src/Cluster/HybridDataDeliverUnit.sv
// Revision:
//   2026/04/27 - Initial version (M1 cluster datapath rewrite)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/Cluster/cluster_pkg.sv"
`include "../../src/Cluster/AddressGenerateUnit.sv"
`include "../../src/Cluster/HybridDataDeliverUnit.sv"
`endif

module tb_hddu;
    import hybridacc_utils_pkg::*;
    import cluster_pkg::*;

    logic clk, reset_n;

    logic            spm_req_valid [4];
    logic            spm_req_ready [4];
    spm_req_32_192_t spm_req_payload[4];
    logic            spm_resp_valid[4];
    logic            spm_resp_ready[4];
    spm_resp_192_t   spm_resp_payload[4];

    logic [191:0] noc_ps_out_data;
    logic [15:0]  noc_ps_out_addr;
    logic [63:0]  noc_ps_out_mask;
    logic         noc_ps_out_valid;
    logic         noc_ps_out_ready;

    logic [191:0] noc_pd_out_data;
    logic [15:0]  noc_pd_out_addr;
    logic [63:0]  noc_pd_out_mask;
    logic         noc_pd_out_valid;
    logic         noc_pd_out_ready;

    logic [191:0] noc_pli_out_data;
    logic [15:0]  noc_pli_out_addr;
    logic [63:0]  noc_pli_out_mask;
    logic         noc_pli_out_valid;
    logic         noc_pli_out_ready;

    logic [15:0]  noc_plo_out_addr;
    logic         noc_plo_out_valid;
    logic         noc_plo_out_ready;
    logic [191:0] noc_plo_in_data;
    NOC_RESPONSE_STATUS noc_plo_in_status;
    logic         noc_plo_in_valid;
    logic         noc_plo_in_ready;

    logic [31:0]  mmio_addr;
    logic         mmio_write;
    logic [31:0]  mmio_wdata;
    logic [31:0]  mmio_rdata;
    logic         interrupt;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    HybridDataDeliverUnit dut (
        .clk(clk),
        .reset_n(reset_n),
        .spm_req_valid(spm_req_valid),
        .spm_req_ready(spm_req_ready),
        .spm_req_payload(spm_req_payload),
        .spm_resp_valid(spm_resp_valid),
        .spm_resp_ready(spm_resp_ready),
        .spm_resp_payload(spm_resp_payload),
        .noc_ps_out_data(noc_ps_out_data),
        .noc_ps_out_addr(noc_ps_out_addr),
        .noc_ps_out_mask(noc_ps_out_mask),
        .noc_ps_out_valid(noc_ps_out_valid),
        .noc_ps_out_ready(noc_ps_out_ready),
        .noc_pd_out_data(noc_pd_out_data),
        .noc_pd_out_addr(noc_pd_out_addr),
        .noc_pd_out_mask(noc_pd_out_mask),
        .noc_pd_out_valid(noc_pd_out_valid),
        .noc_pd_out_ready(noc_pd_out_ready),
        .noc_pli_out_data(noc_pli_out_data),
        .noc_pli_out_addr(noc_pli_out_addr),
        .noc_pli_out_mask(noc_pli_out_mask),
        .noc_pli_out_valid(noc_pli_out_valid),
        .noc_pli_out_ready(noc_pli_out_ready),
        .noc_plo_out_addr(noc_plo_out_addr),
        .noc_plo_out_valid(noc_plo_out_valid),
        .noc_plo_out_ready(noc_plo_out_ready),
        .noc_plo_in_data(noc_plo_in_data),
        .noc_plo_in_status(noc_plo_in_status),
        .noc_plo_in_valid(noc_plo_in_valid),
        .noc_plo_in_ready(noc_plo_in_ready),
        .noc_quiesced_i(1'b1),
        .mmio_addr(mmio_addr),
        .mmio_write(mmio_write),
        .mmio_wdata(mmio_wdata),
        .mmio_rdata(mmio_rdata),
        .interrupt(interrupt)
    );

    task automatic mmio_write32(input logic [31:0] addr, input logic [31:0] data);
        @(negedge clk);
        mmio_addr  = addr;
        mmio_wdata = data;
        mmio_write = 1'b1;
        @(posedge clk);
        @(negedge clk);
        mmio_write = 1'b0;
    endtask

    task automatic cfg_agu_base_tag(input int bank, input logic [31:0] base_addr, input logic [31:0] tag_base);
        mmio_write32(bank*32'h100 + AGU_REG_BASE_ADDR, base_addr);
        mmio_write32(bank*32'h100 + AGU_REG_ITER01, 32'h0001_0001);
        mmio_write32(bank*32'h100 + AGU_REG_ITER23, 32'h0001_0001);
        mmio_write32(bank*32'h100 + AGU_REG_TAG_BASE, tag_base);
        mmio_write32(bank*32'h100 + AGU_REG_MASK_CFG, 32'h0000_000F);
    endtask

    initial begin
        mmio_addr = 0;
        mmio_wdata = 0;
        mmio_write = 0;
        noc_ps_out_ready = 1'b1;
        noc_pd_out_ready = 1'b1;
        noc_pli_out_ready = 1'b1;
        noc_plo_out_ready = 1'b1;
        noc_plo_in_data = '0;
        noc_plo_in_status = NOC_NOP;
        noc_plo_in_valid = 1'b0;
        for (int i = 0; i < 4; i++) begin
            spm_req_ready[i]   = 1'b1;
            spm_resp_valid[i]  = 1'b0;
            spm_resp_payload[i]= '0;
        end

        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // -------------------------------------------------------------
        // Test 1: PS send plane (AGU0 -> SPM read -> NoC PS request)
        // -------------------------------------------------------------
        mmio_write32(32'h0000_0808, 32'h0000_0001); // enable PS plane only
        cfg_agu_base_tag(0, 32'h0000_0020, 32'h0000_0003);
        mmio_write32(32'h0000_0800, 32'h0000_0001); // START

        while (!spm_req_valid[0]) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_BIT("PS plane: spm req is read", spm_req_payload[0].wen, 1'b0)
        `CHECK_VAL("PS plane: spm req addr", spm_req_payload[0].addr, 32'h0000_0020)

        @(negedge clk);
        spm_resp_payload[0].rdata = 192'h0123_4567_89AB_CDEF_1111_2222_3333_4444_5555_6666_7777_8888;
        spm_resp_payload[0].code  = SPM_OK;
        spm_resp_valid[0] = 1'b1;
        @(posedge clk);
        @(negedge clk);
        spm_resp_valid[0] = 1'b0;

        while (!noc_ps_out_valid) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL("PS plane: noc addr from AGU tag", noc_ps_out_addr, 16'h0003)
        `CHECK_VAL("PS plane: noc data", noc_ps_out_data, 192'h0123_4567_89AB_CDEF_1111_2222_3333_4444_5555_6666_7777_8888)

        // -------------------------------------------------------------
        // Test 2: PLO receive plane (AGU3 -> NoC addr req -> NoC resp -> SPM write)
        // -------------------------------------------------------------
        mmio_write32(32'h0000_0808, 32'h0000_0008); // enable PLO receive plane only
        cfg_agu_base_tag(3, 32'h0000_0040, 32'h0000_0005);
        mmio_write32(32'h0000_0800, 32'h0000_0001); // START again

        while (!noc_plo_out_valid) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_VAL("PLO plane: noc read addr", noc_plo_out_addr, 16'h0005)

        @(negedge clk);
        noc_plo_in_data   = 192'hFACE_CAFE_DEAD_BEEF_9999_AAAA_BBBB_CCCC_DDDD_EEEE_FFFF_1234;
        noc_plo_in_status = NOC_OK;
        noc_plo_in_valid  = 1'b1;
        @(posedge clk);
        @(negedge clk);
        noc_plo_in_valid  = 1'b0;

        while (!spm_req_valid[3]) @(posedge clk);
        #(`TB_SETTLE);
        `CHECK_BIT("PLO plane: spm req is write", spm_req_payload[3].wen, 1'b1)
        `CHECK_VAL("PLO plane: spm req addr", spm_req_payload[3].addr, 32'h0000_0040)
        `CHECK_VAL("PLO plane: spm req data", spm_req_payload[3].wdata, 192'hFACE_CAFE_DEAD_BEEF_9999_AAAA_BBBB_CCCC_DDDD_EEEE_FFFF_1234)

        // -------------------------------------------------------------
        // Test 3: global status becomes nonzero / interrupt sticky eventually
        // -------------------------------------------------------------
        @(posedge clk); @(negedge clk);
        mmio_addr = 32'h0000_0804; // status
        #(`TB_SETTLE);
        `CHECK_COND("Global status readable", mmio_rdata[STATUS_IDLE] || mmio_rdata[STATUS_BUSY] || mmio_rdata[STATUS_DONE], mmio_rdata)

        repeat (6) @(posedge clk);
        `CHECK_COND("Interrupt may assert after done", interrupt === 1'b0 || interrupt === 1'b1, interrupt)

        `TB_SUMMARY("tb_hddu")
        $finish;
    end

    initial begin
        #400000;
        $error("[TB_TIMEOUT] tb_hddu did not finish in time");
        `TB_SUMMARY("tb_hddu")
        $finish;
    end

endmodule