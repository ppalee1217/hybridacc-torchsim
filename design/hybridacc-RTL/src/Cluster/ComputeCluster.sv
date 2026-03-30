//-----------------------------------------------------------------------------
// Module Name:   ComputeCluster
// Description:   RTL conversion of ESL ComputeCluster.hpp
//                Top-level integration: SPM + HDDU + NetworkOnChip
//                AXI4-Lite DMA slave + AHB-Lite command interface
//                Power gating logic
//-----------------------------------------------------------------------------
module ComputeCluster
    import hybridacc_utils_pkg::*;
#(
    parameter int unsigned SPM_NUM_NOC_CHANNEL         = 4,
    parameter int unsigned SPM_NUM_BANKS_PER_GROUP     = 3,
    parameter int unsigned SPM_SRAM_BANK_WIDTH_BITS    = 64,
    parameter int unsigned SPM_SRAM_BANK_DEPTH_WORDS   = 8192,
    parameter int unsigned SPM_SRAM_BANK_LATENCY       = 1,
    parameter int unsigned SPM_SRAM_BANK_PIPELINE_DEPTH= 1,
    parameter int unsigned SPM_ADDR_WIDTH              = 32,
    parameter int unsigned NOC_NUM_PORTS               = 3,
    parameter int unsigned NOC_PORT_WIDTH_BITS         = 64,
    parameter int unsigned NOC_NUM_PES_PER_PORT        = 16
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        power_enable_i,
    output logic        interrupt_o,

    // AXI4-Lite slave DATA port -> SPM DMA
    input  logic                                   s_axi_awvalid_i,
    output logic                                   s_axi_awready_o,
    input  logic [SPM_ADDR_WIDTH-1:0]              s_axi_awaddr_i,

    input  logic                                   s_axi_wvalid_i,
    output logic                                   s_axi_wready_o,
    input  logic [SPM_SRAM_BANK_WIDTH_BITS-1:0]    s_axi_wdata_i,
    input  logic [SPM_SRAM_BANK_WIDTH_BITS/8-1:0]  s_axi_wstrb_i,

    output logic                                   s_axi_bvalid_o,
    input  logic                                   s_axi_bready_i,
    output logic [1:0]                             s_axi_bresp_o,

    input  logic                                   s_axi_arvalid_i,
    output logic                                   s_axi_arready_o,
    input  logic [SPM_ADDR_WIDTH-1:0]              s_axi_araddr_i,

    output logic                                   s_axi_rvalid_o,
    input  logic                                   s_axi_rready_i,
    output logic [SPM_SRAM_BANK_WIDTH_BITS-1:0]    s_axi_rdata_o,
    output logic [1:0]                             s_axi_rresp_o,

    // AHB-Lite Slave Port
    input  logic        hsel_i,
    input  logic [31:0] haddr_i,
    input  logic        hwrite_i,
    input  logic [1:0]  htrans_i,
    input  logic [2:0]  hsize_i,
    input  logic [2:0]  hburst_i,
    input  logic [3:0]  hprot_i,
    input  logic        hready_i,
    input  logic [31:0] hwdata_i,
    output logic        hready_o,
    output logic        hresp_o,
    output logic [31:0] hrdata_o
);

    localparam int unsigned HDDU_DATA_BITS     = NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS;
    localparam int unsigned HDDU_NOC_TAG_BITS  = 6;

    localparam logic [31:0] CMD_SPM_BASE  = 32'h0000;
    localparam logic [31:0] CMD_SPM_SIZE  = 32'h0100;
    localparam logic [31:0] CMD_HDDU_BASE = 32'h1000;
    localparam logic [31:0] CMD_HDDU_SIZE = 32'h1000;
    localparam logic [31:0] CMD_NOC_BASE  = 32'h2000;
    localparam logic [31:0] CMD_NOC_SIZE  = 32'h0100;

    localparam logic [31:0] SPM_CFG_MAP_OFF        = 32'h00;
    localparam logic [31:0] SPM_CFG_UPDATE_OFF     = 32'h04;
    localparam logic [31:0] SPM_ARB_POLICY_OFF     = 32'h08;
    localparam logic [31:0] SPM_PMU_CTRL_OFF       = 32'h0C;
    localparam logic [31:0] SPM_PMU_CYCLE_LO_OFF   = 32'h10;
    localparam logic [31:0] SPM_PMU_CYCLE_HI_OFF   = 32'h14;
    localparam logic [31:0] SPM_PMU_ARB_LO_OFF     = 32'h18;
    localparam logic [31:0] SPM_PMU_ARB_HI_OFF     = 32'h1C;
    localparam logic [31:0] SPM_PMU_CREDIT_LO_OFF  = 32'h20;
    localparam logic [31:0] SPM_PMU_CREDIT_HI_OFF  = 32'h24;
    localparam logic [31:0] SPM_PMU_PORT_TXN_BASE  = 32'h40;
    localparam int unsigned SPM_PMU_PORT_TXN_STRIDE = 8;

    localparam logic [31:0] NOC_CMD_DATA_OFF = 32'h00;

    // -------------------------------------------------------------------------
    // Internal wires
    // -------------------------------------------------------------------------
    logic local_reset_n;

    logic [7:0]  spm_cfg_map;
    logic        spm_cfg_update;
    logic        spm_arb_policy;
    logic        spm_pmu_rst;
    logic [63:0] spm_pmu_cycle_cnt;
    logic [63:0] spm_pmu_port_txn_cnt [SPM_NUM_NOC_CHANNEL];
    logic [63:0] spm_pmu_arb_stall_cnt;
    logic [63:0] spm_pmu_credit_stall_cnt;

    logic [3:0]                     hddu_spm_req_valid;
    logic [3:0]                     hddu_spm_req_ready;
    logic [3:0][SPM_ADDR_WIDTH-1:0] hddu_spm_req_addr;
    logic [3:0][HDDU_DATA_BITS-1:0] hddu_spm_req_wdata;
    logic [3:0]                     hddu_spm_req_wen;
    logic [3:0]                     hddu_spm_resp_valid;
    logic [3:0]                     hddu_spm_resp_ready;
    logic [3:0][HDDU_DATA_BITS-1:0] hddu_spm_resp_rdata;
    SPM_RESPONSE_CODE               hddu_spm_resp_code [4];

    logic [31:0] hddu_mmio_addr;
    logic        hddu_mmio_write;
    logic [31:0] hddu_mmio_wdata;
    logic [31:0] hddu_mmio_rdata;
    logic        hddu_interrupt;

    logic        noc_command_mode;
    logic [31:0] noc_command_data;

    logic                      noc_ps_valid, noc_ps_ready;
    logic [HDDU_DATA_BITS-1:0] noc_ps_data;
    logic [15:0]               noc_ps_addr;
    logic [HDDU_DATA_BITS-1:0] noc_ps_mask;

    logic                      noc_pd_valid, noc_pd_ready;
    logic [HDDU_DATA_BITS-1:0] noc_pd_data;
    logic [15:0]               noc_pd_addr;
    logic [HDDU_DATA_BITS-1:0] noc_pd_mask;

    logic                      noc_pli_valid, noc_pli_ready;
    logic [HDDU_DATA_BITS-1:0] noc_pli_data;
    logic [15:0]               noc_pli_addr;
    logic [HDDU_DATA_BITS-1:0] noc_pli_mask;

    logic        noc_plo_req_valid, noc_plo_req_ready;
    logic [15:0] noc_plo_req_addr;

    logic                      noc_plo_resp_valid, noc_plo_resp_ready;
    logic [HDDU_DATA_BITS-1:0] noc_plo_resp_data;
    NOC_RESPONSE_STATUS        noc_plo_resp_status;

    logic                                   spm_axi_awvalid, spm_axi_awready;
    logic [SPM_ADDR_WIDTH-1:0]              spm_axi_awaddr;
    logic                                   spm_axi_wvalid, spm_axi_wready;
    logic [SPM_SRAM_BANK_WIDTH_BITS-1:0]    spm_axi_wdata;
    logic [SPM_SRAM_BANK_WIDTH_BITS/8-1:0]  spm_axi_wstrb;
    logic                                   spm_axi_bvalid, spm_axi_bready;
    logic [1:0]                             spm_axi_bresp;
    logic                                   spm_axi_arvalid, spm_axi_arready;
    logic [SPM_ADDR_WIDTH-1:0]              spm_axi_araddr;
    logic                                   spm_axi_rvalid, spm_axi_rready;
    logic [SPM_SRAM_BANK_WIDTH_BITS-1:0]    spm_axi_rdata;
    logic [1:0]                             spm_axi_rresp;

    logic        ahb_active_reg;
    logic        ahb_write_reg;
    logic [31:0] ahb_addr_reg;
    logic [2:0]  ahb_hsize_reg;
    logic        ahb_read_wait_reg;
    logic [31:0] ahb_rdata_reg;
    logic [31:0] noc_last_cmd_reg;
    logic        ahb_hddu_rd_pending_reg;

    // =========================================================================
    // Sub-module instantiations
    // =========================================================================

    ScratchpadMemory #(
        .NUM_NOC_PORTS          (SPM_NUM_NOC_CHANNEL),
        .BANKS_PER_GROUP        (SPM_NUM_BANKS_PER_GROUP),
        .BANK_DATA_WIDTH        (SPM_SRAM_BANK_WIDTH_BITS),
        .BANK_DEPTH             (SPM_SRAM_BANK_DEPTH_WORDS),
        .SRAM_BANK_LATENCY      (SPM_SRAM_BANK_LATENCY),
        .SRAM_BANK_PIPELINE_DEPTH(SPM_SRAM_BANK_PIPELINE_DEPTH),
        .ADDR_WIDTH             (SPM_ADDR_WIDTH)
    ) u_spm (
        .clk(clk), .reset_n(local_reset_n), .pmu_rst_i(spm_pmu_rst),
        .config_map_i(spm_cfg_map), .config_update_i(spm_cfg_update), .arb_policy_i(spm_arb_policy),
        .spm_req_valid_i(hddu_spm_req_valid), .spm_req_ready_o(hddu_spm_req_ready),
        .spm_req_addr_i(hddu_spm_req_addr), .spm_req_wdata_i(hddu_spm_req_wdata),
        .spm_req_wen_i(hddu_spm_req_wen),
        .spm_resp_valid_o(hddu_spm_resp_valid), .spm_resp_ready_i(hddu_spm_resp_ready),
        .spm_resp_rdata_o(hddu_spm_resp_rdata), .spm_resp_code_o(hddu_spm_resp_code),
        .s_axi_awvalid_i(spm_axi_awvalid), .s_axi_awready_o(spm_axi_awready), .s_axi_awaddr_i(spm_axi_awaddr),
        .s_axi_wvalid_i(spm_axi_wvalid), .s_axi_wready_o(spm_axi_wready),
        .s_axi_wdata_i(spm_axi_wdata), .s_axi_wstrb_i(spm_axi_wstrb),
        .s_axi_bvalid_o(spm_axi_bvalid), .s_axi_bready_i(spm_axi_bready), .s_axi_bresp_o(spm_axi_bresp),
        .s_axi_arvalid_i(spm_axi_arvalid), .s_axi_arready_o(spm_axi_arready), .s_axi_araddr_i(spm_axi_araddr),
        .s_axi_rvalid_o(spm_axi_rvalid), .s_axi_rready_i(spm_axi_rready),
        .s_axi_rdata_o(spm_axi_rdata), .s_axi_rresp_o(spm_axi_rresp),
        .pmu_cycle_cnt_o(spm_pmu_cycle_cnt), .pmu_port_txn_cnt_o(spm_pmu_port_txn_cnt),
        .pmu_arb_stall_cnt_o(spm_pmu_arb_stall_cnt), .pmu_credit_stall_cnt_o(spm_pmu_credit_stall_cnt)
    );

    HybridDataDeliverUnit #(
        .SPM_ADDR_BITS(SPM_ADDR_WIDTH),
        .NOC_TAG_BITS (HDDU_NOC_TAG_BITS),
        .DATA_BITS    (HDDU_DATA_BITS)
    ) u_hddu (
        .clk(clk), .reset_n(local_reset_n),
        .spm_req_valid(hddu_spm_req_valid), .spm_req_ready(hddu_spm_req_ready),
        .spm_req_addr(hddu_spm_req_addr), .spm_req_wdata(hddu_spm_req_wdata), .spm_req_wen(hddu_spm_req_wen),
        .spm_resp_valid(hddu_spm_resp_valid), .spm_resp_ready(hddu_spm_resp_ready),
        .spm_resp_rdata(hddu_spm_resp_rdata), .spm_resp_code(hddu_spm_resp_code),
        .noc_ps_valid(noc_ps_valid), .noc_ps_ready(noc_ps_ready),
        .noc_ps_data(noc_ps_data), .noc_ps_addr(noc_ps_addr), .noc_ps_mask(noc_ps_mask),
        .noc_pd_valid(noc_pd_valid), .noc_pd_ready(noc_pd_ready),
        .noc_pd_data(noc_pd_data), .noc_pd_addr(noc_pd_addr), .noc_pd_mask(noc_pd_mask),
        .noc_pli_valid(noc_pli_valid), .noc_pli_ready(noc_pli_ready),
        .noc_pli_data(noc_pli_data), .noc_pli_addr(noc_pli_addr), .noc_pli_mask(noc_pli_mask),
        .noc_plo_req_valid(noc_plo_req_valid), .noc_plo_req_ready(noc_plo_req_ready),
        .noc_plo_req_addr(noc_plo_req_addr),
        .noc_plo_resp_valid(noc_plo_resp_valid), .noc_plo_resp_ready(noc_plo_resp_ready),
        .noc_plo_resp_data(noc_plo_resp_data),
        .mmio_addr(hddu_mmio_addr), .mmio_write(hddu_mmio_write),
        .mmio_wdata(hddu_mmio_wdata), .mmio_rdata(hddu_mmio_rdata),
        .interrupt(hddu_interrupt)
    );

    NetworkOnChip #(
        .NUM_PORTS       (NOC_NUM_PORTS),
        .PORT_WIDTH_BITS (NOC_PORT_WIDTH_BITS),
        .NUM_PES_PER_PORT(NOC_NUM_PES_PER_PORT)
    ) u_noc (
        .clk(clk), .reset_n(local_reset_n),
        .command_mode(noc_command_mode), .command_data(noc_command_data),
        .noc_ps_in_data(noc_ps_data), .noc_ps_in_valid(noc_ps_valid), .noc_ps_in_ready(noc_ps_ready),
        .noc_pd_in_data(noc_pd_data), .noc_pd_in_valid(noc_pd_valid), .noc_pd_in_ready(noc_pd_ready),
        .noc_pli_in_data(noc_pli_data), .noc_pli_in_valid(noc_pli_valid), .noc_pli_in_ready(noc_pli_ready),
        .noc_plo_in_data(noc_addr_req_t'{addr: noc_plo_req_addr}),
        .noc_plo_in_valid(noc_plo_req_valid), .noc_plo_in_ready(noc_plo_req_ready),
        .noc_plo_out_data(noc_plo_resp_data), .noc_plo_out_status(noc_plo_resp_status),
        .noc_plo_out_valid(noc_plo_resp_valid), .noc_plo_out_ready(noc_plo_resp_ready)
    );

    // =========================================================================
    // comb_power_and_wiring
    // =========================================================================
    always_comb begin
        local_reset_n = reset_n && power_enable_i;

        spm_axi_awvalid = power_enable_i && s_axi_awvalid_i;
        spm_axi_awaddr  = s_axi_awaddr_i;
        s_axi_awready_o = power_enable_i && spm_axi_awready;

        spm_axi_wvalid  = power_enable_i && s_axi_wvalid_i;
        spm_axi_wdata   = s_axi_wdata_i;
        spm_axi_wstrb   = s_axi_wstrb_i;
        s_axi_wready_o  = power_enable_i && spm_axi_wready;

        spm_axi_bready  = power_enable_i && s_axi_bready_i;
        s_axi_bvalid_o  = power_enable_i && spm_axi_bvalid;
        s_axi_bresp_o   = power_enable_i ? spm_axi_bresp : 2'b11;

        spm_axi_arvalid = power_enable_i && s_axi_arvalid_i;
        spm_axi_araddr  = s_axi_araddr_i;
        s_axi_arready_o = power_enable_i && spm_axi_arready;

        spm_axi_rready  = power_enable_i && s_axi_rready_i;
        s_axi_rvalid_o  = power_enable_i && spm_axi_rvalid;
        s_axi_rdata_o   = power_enable_i ? spm_axi_rdata : '0;
        s_axi_rresp_o   = power_enable_i ? spm_axi_rresp : 2'b11;

        hready_o    = power_enable_i && !ahb_read_wait_reg;
        hresp_o     = 1'b0;
        interrupt_o = power_enable_i && hddu_interrupt;
        hrdata_o    = power_enable_i ? ahb_rdata_reg : 32'd0;
    end

    // =========================================================================
    // seq_ahb_ctrl
    // =========================================================================
    always_ff @(posedge clk or negedge local_reset_n) begin
        if (!local_reset_n) begin
            spm_cfg_map       <= 8'd0;
            spm_cfg_update    <= 1'b0;
            spm_arb_policy    <= 1'b0;
            spm_pmu_rst       <= 1'b0;
            noc_command_mode  <= 1'b0;
            noc_command_data  <= 32'd0;
            noc_last_cmd_reg  <= 32'd0;
            ahb_active_reg    <= 1'b0;
            ahb_write_reg     <= 1'b0;
            ahb_addr_reg      <= 32'd0;
            ahb_read_wait_reg <= 1'b0;
            ahb_rdata_reg     <= 32'd0;
            hddu_mmio_addr    <= 32'd0;
            hddu_mmio_wdata   <= 32'd0;
            hddu_mmio_write   <= 1'b0;
            ahb_hddu_rd_pending_reg <= 1'b0;
        end else begin : seq_ahb_main
            reg        is_trans;
            reg [31:0] addr;
            reg        is_write;
            reg [31:0] wdata;
            reg [31:0] off;
            reg [31:0] read_value;
            reg [31:0] rel;
            integer    idx;
            reg        is_high;

            // Default one-shot deassertions
            spm_cfg_update   <= 1'b0;
            noc_command_mode <= 1'b0;
            hddu_mmio_write  <= 1'b0;
            spm_pmu_rst      <= 1'b0;

            ahb_read_wait_reg <= ahb_active_reg && !ahb_write_reg;

            if (!power_enable_i) begin
                ahb_active_reg    <= 1'b0;
                ahb_read_wait_reg <= 1'b0;
            end else begin
                // Address phase capture
                is_trans = hsel_i && hready_i && !ahb_read_wait_reg && htrans_i[1];
                if (is_trans) begin
                    ahb_addr_reg   <= haddr_i;
                    ahb_write_reg  <= hwrite_i;
                    ahb_hsize_reg  <= hsize_i;
                    ahb_active_reg <= 1'b1;
                end else begin
                    ahb_active_reg <= 1'b0;
                end

                // Data phase execution
                if (ahb_active_reg) begin
                    addr  = ahb_addr_reg;
                    is_write = ahb_write_reg;
                    wdata = hwdata_i;
                    read_value = 32'd0;

                    if (is_write) begin
                        if (addr >= CMD_SPM_BASE && addr < (CMD_SPM_BASE + CMD_SPM_SIZE)) begin
                            off = addr - CMD_SPM_BASE;
                            case (off)
                                SPM_CFG_MAP_OFF:    spm_cfg_map    <= wdata[7:0];
                                SPM_CFG_UPDATE_OFF: begin
                                    if (wdata[0]) spm_cfg_update <= 1'b1;
                                end
                                SPM_ARB_POLICY_OFF: spm_arb_policy <= wdata[0];
                                SPM_PMU_CTRL_OFF: begin
                                    if (wdata[0]) spm_pmu_rst <= 1'b1;
                                end
                                default: ;
                            endcase
                        end
                        else if (addr >= CMD_NOC_BASE && addr < (CMD_NOC_BASE + CMD_NOC_SIZE)) begin
                            off = addr - CMD_NOC_BASE;
                            if (off == NOC_CMD_DATA_OFF) begin
                                noc_last_cmd_reg <= wdata;
                                noc_command_data <= wdata;
                                noc_command_mode <= 1'b1;
                            end
                        end
                        else if (addr >= CMD_HDDU_BASE && addr < (CMD_HDDU_BASE + CMD_HDDU_SIZE)) begin
                            hddu_mmio_addr  <= addr - CMD_HDDU_BASE;
                            hddu_mmio_wdata <= wdata;
                            hddu_mmio_write <= 1'b1;
                        end
                    end else begin
                        if (addr >= CMD_SPM_BASE && addr < (CMD_SPM_BASE + CMD_SPM_SIZE)) begin
                            off = addr - CMD_SPM_BASE;
                            case (off)
                                SPM_CFG_MAP_OFF:       read_value = {24'd0, spm_cfg_map};
                                SPM_CFG_UPDATE_OFF:    read_value = 32'd0;
                                SPM_ARB_POLICY_OFF:    read_value = {31'd0, spm_arb_policy};
                                SPM_PMU_CTRL_OFF:      read_value = 32'd0;
                                SPM_PMU_CYCLE_LO_OFF:  read_value = spm_pmu_cycle_cnt[31:0];
                                SPM_PMU_CYCLE_HI_OFF:  read_value = spm_pmu_cycle_cnt[63:32];
                                SPM_PMU_ARB_LO_OFF:    read_value = spm_pmu_arb_stall_cnt[31:0];
                                SPM_PMU_ARB_HI_OFF:    read_value = spm_pmu_arb_stall_cnt[63:32];
                                SPM_PMU_CREDIT_LO_OFF: read_value = spm_pmu_credit_stall_cnt[31:0];
                                SPM_PMU_CREDIT_HI_OFF: read_value = spm_pmu_credit_stall_cnt[63:32];
                                default: begin
                                    if (off >= SPM_PMU_PORT_TXN_BASE) begin
                                        rel = off - SPM_PMU_PORT_TXN_BASE;
                                        idx = rel / SPM_PMU_PORT_TXN_STRIDE;
                                        is_high = (rel % SPM_PMU_PORT_TXN_STRIDE) == 4;
                                        if (idx < SPM_NUM_NOC_CHANNEL) begin
                                            read_value = is_high ? spm_pmu_port_txn_cnt[idx][63:32]
                                                                 : spm_pmu_port_txn_cnt[idx][31:0];
                                        end
                                    end
                                end
                            endcase
                        end
                        else if (addr >= CMD_HDDU_BASE && addr < (CMD_HDDU_BASE + CMD_HDDU_SIZE)) begin
                            hddu_mmio_addr <= addr - CMD_HDDU_BASE;
                            ahb_hddu_rd_pending_reg <= 1'b1;
                        end
                        else if (addr >= CMD_NOC_BASE && addr < (CMD_NOC_BASE + CMD_NOC_SIZE)) begin
                            off = addr - CMD_NOC_BASE;
                            if (off == NOC_CMD_DATA_OFF) read_value = noc_last_cmd_reg;
                        end

                        ahb_rdata_reg <= read_value;
                    end
                end

                // Late-capture HDDU read data (hddu_mmio_addr set prev cycle)
                if (ahb_hddu_rd_pending_reg) begin
                    ahb_rdata_reg <= hddu_mmio_rdata;
                    ahb_hddu_rd_pending_reg <= 1'b0;
                end
            end
        end
    end

endmodule
