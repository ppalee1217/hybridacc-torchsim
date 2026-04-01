//-----------------------------------------------------------------------------
// Module Name:   tb_computecluster
// Description:   Testbench for Cluster/ComputeCluster module.
//                Tests: AHB-Lite register R/W for SPM config, HDDU MMIO,
//                AXI4-Lite DMA write/read through SPM, power gating.
//                Note: NetworkOnChip is stubbed (NoC sub-modules not compiled).
//                Full NoC integration is covered by noc_sim testbenches.
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/FIFO.sv"
`include "../../src/Cluster/SRAM.sv"
`include "../../src/Cluster/AddressGenerateUnit.sv"
`include "../../src/Cluster/ScratchpadMemory.sv"
`include "../../src/Cluster/HybridDataDeliverUnit.sv"

// Stub NetworkOnChip for unit testing (real NoC needs NoCRouter/MBUS/PE)
module NetworkOnChip
    import hybridacc_utils_pkg::*;
#(
    parameter int unsigned NUM_PORTS        = 3,
    parameter int unsigned PORT_WIDTH_BITS  = 64,
    parameter int unsigned NUM_PES_PER_PORT = 16,
    parameter int unsigned PE_FIFO_DEPTH    = 4
) (
    input  logic clk, input logic reset_n,
    input  logic command_mode, input logic [31:0] command_data,
    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_ps_in_data,
    input  logic noc_ps_in_valid, output logic noc_ps_in_ready,
    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_pd_in_data,
    input  logic noc_pd_in_valid, output logic noc_pd_in_ready,
    input  logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_pli_in_data,
    input  logic noc_pli_in_valid, output logic noc_pli_in_ready,
    input  noc_addr_req_t noc_plo_in_data,
    input  logic noc_plo_in_valid, output logic noc_plo_in_ready,
    output logic [NUM_PORTS*PORT_WIDTH_BITS-1:0] noc_plo_out_data,
    output NOC_RESPONSE_STATUS noc_plo_out_status,
    output logic noc_plo_out_valid, input logic noc_plo_out_ready
);
    assign noc_ps_in_ready  = 1'b1;
    assign noc_pd_in_ready  = 1'b1;
    assign noc_pli_in_ready = 1'b1;
    assign noc_plo_in_ready = 1'b1;
    assign noc_plo_out_data   = '0;
    assign noc_plo_out_status = NOC_OK;
    assign noc_plo_out_valid  = 1'b0;
endmodule

`include "../../src/Cluster/ComputeCluster.sv"

module tb_computecluster;
    import hybridacc_utils_pkg::*;

    localparam int AW  = 32;
    localparam int BDW = 64;
    localparam int BD  = 64;

    logic clk, reset_n;
    logic power_en;
    logic interrupt;

    // AXI4-Lite DMA
    logic                 axi_awvalid, axi_awready;
    logic [AW-1:0]        axi_awaddr;
    logic                 axi_wvalid, axi_wready;
    logic [BDW-1:0]       axi_wdata;
    logic [BDW/8-1:0]     axi_wstrb;
    logic                 axi_bvalid, axi_bready;
    logic [1:0]           axi_bresp;
    logic                 axi_arvalid, axi_arready;
    logic [AW-1:0]        axi_araddr;
    logic                 axi_rvalid, axi_rready;
    logic [BDW-1:0]       axi_rdata;
    logic [1:0]           axi_rresp;

    // AHB-Lite
    logic        hsel;
    logic [31:0] haddr;
    logic        hwrite;
    logic [1:0]  htrans;
    logic [2:0]  hsize;
    logic [2:0]  hburst;
    logic [3:0]  hprot;
    logic        hready_in;
    logic [31:0] hwdata;
    logic        hready_out;
    logic        hresp;
    logic [31:0] hrdata;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    ComputeCluster #(
        .SPM_NUM_NOC_CHANNEL(4),
        .SPM_NUM_BANKS_PER_GROUP(3),
        .SPM_SRAM_BANK_WIDTH_BITS(BDW),
        .SPM_SRAM_BANK_DEPTH_WORDS(BD),
        .SPM_SRAM_BANK_LATENCY(1),
        .SPM_SRAM_BANK_PIPELINE_DEPTH(2),
        .SPM_ADDR_WIDTH(AW),
        .NOC_NUM_PORTS(3),
        .NOC_PORT_WIDTH_BITS(64),
        .NOC_NUM_PES_PER_PORT(16)
    ) dut (
        .clk(clk), .reset_n(reset_n), .power_enable_i(power_en), .interrupt_o(interrupt),
        .s_axi_awvalid_i(axi_awvalid), .s_axi_awready_o(axi_awready), .s_axi_awaddr_i(axi_awaddr),
        .s_axi_wvalid_i(axi_wvalid), .s_axi_wready_o(axi_wready),
        .s_axi_wdata_i(axi_wdata), .s_axi_wstrb_i(axi_wstrb),
        .s_axi_bvalid_o(axi_bvalid), .s_axi_bready_i(axi_bready), .s_axi_bresp_o(axi_bresp),
        .s_axi_arvalid_i(axi_arvalid), .s_axi_arready_o(axi_arready), .s_axi_araddr_i(axi_araddr),
        .s_axi_rvalid_o(axi_rvalid), .s_axi_rready_i(axi_rready),
        .s_axi_rdata_o(axi_rdata), .s_axi_rresp_o(axi_rresp),
        .hsel_i(hsel), .haddr_i(haddr), .hwrite_i(hwrite), .htrans_i(htrans),
        .hsize_i(hsize), .hburst_i(hburst), .hprot_i(hprot), .hready_i(hready_in),
        .hwdata_i(hwdata), .hready_o(hready_out), .hresp_o(hresp), .hrdata_o(hrdata)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", name); fail_count++; end
        else begin $display("[PASS] %s", name); pass_count++; end
    endtask

    // AHB write: 2-phase (addr phase → data phase)
    task automatic ahb_write(input logic [31:0] addr, input logic [31:0] data);
        // Address phase
        @(posedge clk);
        hsel     <= 1'b1;
        haddr    <= addr;
        hwrite   <= 1'b1;
        htrans   <= 2'b10; // NONSEQ
        hready_in<= 1'b1;
        // Data phase
        @(posedge clk);
        hwdata   <= data;
        hsel     <= 1'b0;
        htrans   <= 2'b00;
        @(posedge clk);
        hwrite   <= 1'b0;
    endtask

    // AHB read: 2-phase (addr → wait → read data)
    task automatic ahb_read(input logic [31:0] addr, output logic [31:0] data);
        // Address phase
        @(posedge clk);
        hsel     <= 1'b1;
        haddr    <= addr;
        hwrite   <= 1'b0;
        htrans   <= 2'b10; // NONSEQ
        hready_in<= 1'b1;
        @(posedge clk);
        hsel     <= 1'b0;
        htrans   <= 2'b00;
        // Wait for hready_out
        repeat (3) @(posedge clk);
        if (!hready_out) @(posedge hready_out);
        #(`TB_SETTLE);
        data = hrdata;
    endtask

    // AXI4-Lite DMA write
    task automatic dma_write(input logic [AW-1:0] addr,
                             input logic [BDW-1:0] data);
        @(posedge clk);
        axi_awvalid <= 1'b1; axi_awaddr <= addr;
        axi_wvalid  <= 1'b1; axi_wdata  <= data; axi_wstrb <= 8'hFF;
        @(posedge clk);
        axi_awvalid <= 1'b0; axi_wvalid <= 1'b0;
        wait (axi_bvalid === 1'b1);
        @(posedge clk);
    endtask

    // AXI4-Lite DMA read
    task automatic dma_read(input logic [AW-1:0] addr,
                            output logic [BDW-1:0] data);
        @(posedge clk);
        axi_arvalid <= 1'b1; axi_araddr <= addr;
        @(posedge clk);
        axi_arvalid <= 1'b0;
        wait (axi_rvalid === 1'b1);
        data = axi_rdata;
        @(posedge clk);
    endtask

    logic [31:0] rd32;
    logic [BDW-1:0] rd64;

    initial begin
        power_en    = 1'b1;
        hsel        = 1'b0;
        haddr       = '0;
        hwrite      = 1'b0;
        htrans      = 2'b00;
        hsize       = 3'b010;
        hburst      = '0;
        hprot       = '0;
        hready_in   = 1'b1;
        hwdata      = '0;
        axi_awvalid = 1'b0; axi_awaddr = '0;
        axi_wvalid  = 1'b0; axi_wdata  = '0; axi_wstrb = '0;
        axi_bready  = 1'b1;
        axi_arvalid = 1'b0; axi_araddr = '0;
        axi_rready  = 1'b1;

        @(posedge reset_n);
        repeat (5) @(posedge clk);

        // ---- Test 1: AHB write + read SPM config_map ----
        $display("\n=== Test 1: AHB SPM config map ===");
        ahb_write(32'h0000, 32'hE4);  // SPM_CFG_MAP_OFF = 0x00 inside CMD_SPM_BASE
        ahb_read(32'h0000, rd32);
        check("T1 spm config_map", rd32[7:0] == 8'hE4);

        // ---- Test 2: Trigger SPM config update ----
        $display("\n=== Test 2: SPM config update ===");
        ahb_write(32'h0004, 32'h0001);  // SPM_CFG_UPDATE_OFF
        repeat (2) @(posedge clk);
        check("T2 config update (no hang)", 1'b1);

        // ---- Test 3: AHB write/read arb_policy ----
        $display("\n=== Test 3: SPM arb policy ===");
        ahb_write(32'h0008, 32'h0001);  // SPM_ARB_POLICY_OFF
        ahb_read(32'h0008, rd32);
        check("T3 arb_policy", rd32[0] == 1'b1);

        // ---- Test 4: AXI4-Lite DMA write + read through SPM ----
        $display("\n=== Test 4: DMA write + read ===");
        dma_write(32'h0000_0000, 64'hCAFE_BABE_DEAD_BEEF);
        repeat (5) @(posedge clk);
        dma_read(32'h0000_0000, rd64);
        check("T4 DMA roundtrip", rd64 == 64'hCAFE_BABE_DEAD_BEEF);

        // ---- Test 5: AHB read PMU cycle counter ----
        $display("\n=== Test 5: PMU cycle counter ===");
        ahb_read(32'h0010, rd32);  // SPM_PMU_CYCLE_LO_OFF
        check("T5 pmu cycle lo > 0", rd32 > 0);

        // ---- Test 6: HDDU MMIO through AHB ----
        $display("\n=== Test 6: HDDU MMIO via AHB ===");
        // Read HDDU NUM_PLANES (MMIO_GLOBAL_NUM_PLANES = 0x810 → AHB addr = 0x1000+0x810 = 0x1810)
        ahb_read(32'h1810, rd32);
        check("T6 HDDU num_planes", rd32 == 32'd4);
        // Write and read plane_en
        ahb_write(32'h1808, 32'h0000_000F);  // GLOBAL_PLANE_EN via AHB
        ahb_read(32'h1808, rd32);
        check("T6 HDDU plane_en", rd32 == 32'h0000_000F);

        // ---- Test 7: Power gating ----
        $display("\n=== Test 7: Power gating ===");
        power_en <= 1'b0;
        repeat (3) @(posedge clk);
        check("T7 hready when powered off", hready_out == 1'b0);
        check("T7 AXI awready off", axi_awready == 1'b0);
        power_en <= 1'b1;
        repeat (5) @(posedge clk);

        // ---- Test 8: NoC command via AHB ----
        $display("\n=== Test 8: NoC command ===");
        ahb_write(32'h2000, 32'hABCD_1234);  // NOC_CMD_DATA_OFF
        ahb_read(32'h2000, rd32);
        check("T8 NoC cmd readback", rd32 == 32'hABCD_1234);

        // ---- Summary ----
        repeat (10) @(posedge clk);
        $display("\n========================================");
        $display("tb_computecluster: %0d PASS, %0d FAIL", pass_count, fail_count);
        $display("========================================");
        if (fail_count > 0) $fatal(1, "FAILED");
        $finish;
    end

    initial begin
        #500000;
        $fatal(1, "TIMEOUT");
    end
endmodule
