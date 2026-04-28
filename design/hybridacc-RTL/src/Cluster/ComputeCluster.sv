//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   ComputeCluster
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Cluster top-level functional RTL baseline.
//                Integrates SPM, HDDU, existing NoC, direct/native command,
//                AHB-lite MMIO path, and 64-bit DMA data slave.
// Dependencies:  src/hybridacc_utils_pkg.sv, src/Cluster/cluster_pkg.sv,
//                src/Cluster/ScratchpadMemory.sv,
//                src/Cluster/HybridDataDeliverUnit.sv,
//                src/Cluster/ClusterControlUnit.sv,
//                src/NetworkOnChip.sv
// Revision:
//   2026/04/27 - Initial version (M2 cluster integration baseline)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;
import cluster_pkg::*;

module ComputeCluster #(
    parameter int unsigned SPM_NUM_NOC_CHANNEL         = 4,
    parameter int unsigned SPM_NUM_BANKS_PER_GROUP     = 3,
    parameter int unsigned SPM_SRAM_BANK_WIDTH_BITS    = 64,
    parameter int unsigned SPM_SRAM_BANK_DEPTH_WORDS   = 8192,
    parameter int unsigned SPM_SRAM_BANK_LATENCY       = 1,
    parameter int unsigned SPM_SRAM_BANK_PIPELINE_DEPTH= 1,
    parameter int unsigned SPM_ADDR_WIDTH              = 32,
    parameter int unsigned NOC_NUM_PORTS               = 3,
    parameter int unsigned NOC_PORT_WIDTH_BITS         = 64,
    parameter int unsigned NOC_NUM_PES_PER_PORT        = 16,
    parameter int unsigned PE_FIFO_DEPTH               = 4,
    parameter int unsigned NOC_FIFO_DEPTH              = 4
) (
    input  logic             clk,
    input  logic             reset_n,
    input  logic             power_enable_i,
    output logic             interrupt_o,

    input  logic             cmd_req_valid_i,
    input  logic             cmd_req_write_i,
    input  logic [31:0]      cmd_req_addr_i,
    input  logic [31:0]      cmd_req_wdata_i,
    input  logic [3:0]       cmd_req_wstrb_i,
    output logic             cmd_req_ready_o,
    output logic             cmd_resp_valid_o,
    output logic [31:0]      cmd_resp_rdata_o,
    output logic             cmd_resp_err_o,

    input  logic             s_axi_awvalid_i,
    output logic             s_axi_awready_o,
    input  logic [SPM_ADDR_WIDTH-1:0] s_axi_awaddr_i,
    input  logic             s_axi_wvalid_i,
    output logic             s_axi_wready_o,
    input  logic [SPM_SRAM_BANK_WIDTH_BITS-1:0] s_axi_wdata_i,
    input  logic [SPM_SRAM_BANK_WIDTH_BITS/8-1:0] s_axi_wstrb_i,
    output logic             s_axi_bvalid_o,
    input  logic             s_axi_bready_i,
    output logic [1:0]       s_axi_bresp_o,
    input  logic             s_axi_arvalid_i,
    output logic             s_axi_arready_o,
    input  logic [SPM_ADDR_WIDTH-1:0] s_axi_araddr_i,
    output logic             s_axi_rvalid_o,
    input  logic             s_axi_rready_i,
    output logic [SPM_SRAM_BANK_WIDTH_BITS-1:0] s_axi_rdata_o,
    output logic [1:0]       s_axi_rresp_o,

    input  logic             hsel_i,
    input  logic [31:0]      haddr_i,
    input  logic             hwrite_i,
    input  logic [1:0]       htrans_i,
    input  logic [2:0]       hsize_i,
    input  logic [2:0]       hburst_i,
    input  logic [3:0]       hprot_i,
    input  logic             hready_i,
    input  logic [31:0]      hwdata_i,
    output logic             hready_o,
    output logic             hresp_o,
    output logic [31:0]      hrdata_o
);
    localparam int unsigned HDDU_DATA_BITS = NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS;

    localparam logic [31:0] K_CMD_SPM_BASE      = 32'h0000_0000;
    localparam logic [31:0] K_CMD_SPM_SIZE      = 32'h0000_0100;
    localparam logic [31:0] K_CMD_HDDU_BASE     = 32'h0000_1000;
    localparam logic [31:0] K_CMD_HDDU_SIZE     = 32'h0000_1000;
    localparam logic [31:0] K_CMD_NOC_BASE      = 32'h0000_2000;
    localparam logic [31:0] K_CMD_NOC_SIZE      = 32'h0000_0100;
    localparam logic [31:0] K_CMD_CLUSTER_BASE  = CLUSTER_MMIO_BASE;
    localparam logic [31:0] K_CMD_CLUSTER_SIZE  = CLUSTER_MMIO_SIZE;

    localparam logic [31:0] K_SPM_CFG_MAP       = 32'h0000_0000;
    localparam logic [31:0] K_SPM_CFG_UPDATE    = 32'h0000_0004;
    localparam logic [31:0] K_SPM_ARB_POLICY    = 32'h0000_0008;
    localparam logic [31:0] K_SPM_PMU_CTRL      = 32'h0000_000C;
    localparam logic [31:0] K_SPM_PMU_CYCLE_LO  = 32'h0000_0010;
    localparam logic [31:0] K_SPM_PMU_CYCLE_HI  = 32'h0000_0014;
    localparam logic [31:0] K_SPM_PMU_ARB_LO    = 32'h0000_0018;
    localparam logic [31:0] K_SPM_PMU_ARB_HI    = 32'h0000_001C;
    localparam logic [31:0] K_SPM_PMU_CREDIT_LO = 32'h0000_0020;
    localparam logic [31:0] K_SPM_PMU_CREDIT_HI = 32'h0000_0024;
    localparam logic [31:0] K_SPM_PMU_PORT_BASE = 32'h0000_0040;

    localparam logic [31:0] K_NOC_CMD_DATA      = 32'h0000_0000;
    localparam logic [31:0] K_NOC_STATUS        = 32'h0000_0004;

    logic local_reset_n;

    logic [7:0]  spm_cfg_map_reg;
    logic        spm_arb_policy_reg;
    logic        spm_cfg_update_pulse;
    logic        spm_pmu_rst_pulse;
    logic        spm_drop_noc_resp_sig;
    logic        spm_soft_reset_sig;
    logic [63:0] spm_pmu_cycle_cnt_sig;
    logic [63:0] spm_pmu_port_txn_cnt_sig[SPM_NUM_NOC_CHANNEL];
    logic [63:0] spm_pmu_arb_stall_cnt_sig;
    logic [63:0] spm_pmu_credit_stall_cnt_sig;

    logic            hddu_spm_req_valid_sig [SPM_NUM_NOC_CHANNEL];
    logic            hddu_spm_req_ready_sig [SPM_NUM_NOC_CHANNEL];
    spm_req_32_192_t hddu_spm_req_payload_sig[SPM_NUM_NOC_CHANNEL];
    logic            hddu_spm_resp_valid_sig[SPM_NUM_NOC_CHANNEL];
    logic            hddu_spm_resp_ready_sig[SPM_NUM_NOC_CHANNEL];
    spm_resp_192_t   hddu_spm_resp_payload_sig[SPM_NUM_NOC_CHANNEL];

    logic [31:0] hddu_mmio_addr_sig;
    logic        hddu_mmio_write_sig;
    logic [31:0] hddu_mmio_wdata_sig;
    logic [31:0] hddu_mmio_rdata_sig;
    logic        hddu_interrupt_sig;

    logic                             s_axi_awready_sig;
    logic                             s_axi_wready_sig;
    logic                             s_axi_bvalid_sig;
    logic [1:0]                       s_axi_bresp_sig;
    logic                             s_axi_arready_sig;
    logic                             s_axi_rvalid_sig;
    logic [SPM_SRAM_BANK_WIDTH_BITS-1:0] s_axi_rdata_sig;
    logic [1:0]                       s_axi_rresp_sig;

    logic [HDDU_DATA_BITS-1:0] hddu_noc_ps_data;
    logic [15:0]               hddu_noc_ps_addr;
    logic [63:0]               hddu_noc_ps_mask;
    logic                      hddu_noc_ps_valid;
    logic                      hddu_noc_ps_ready;
    logic [HDDU_DATA_BITS-1:0] hddu_noc_pd_data;
    logic [15:0]               hddu_noc_pd_addr;
    logic [63:0]               hddu_noc_pd_mask;
    logic                      hddu_noc_pd_valid;
    logic                      hddu_noc_pd_ready;
    logic [HDDU_DATA_BITS-1:0] hddu_noc_pli_data;
    logic [15:0]               hddu_noc_pli_addr;
    logic [63:0]               hddu_noc_pli_mask;
    logic                      hddu_noc_pli_valid;
    logic                      hddu_noc_pli_ready;
    logic [15:0]               hddu_noc_plo_addr;
    logic                      hddu_noc_plo_valid;
    logic                      hddu_noc_plo_ready;
    logic [HDDU_DATA_BITS-1:0] hddu_noc_plo_resp_data;
    NOC_RESPONSE_STATUS        hddu_noc_plo_resp_status;
    logic                      hddu_noc_plo_resp_valid;
    logic                      hddu_noc_plo_resp_ready;
    logic                      noc_hw_quiesced_sig;

    logic noc_command_mode_sig;
    logic [31:0] noc_command_data_sig;
    logic [31:0] noc_last_cmd_reg;

    logic ccu_write_mode_sig;
    logic [31:0] ccu_mode_wdata_sig;
    logic ccu_write_ctrl_sig;
    logic [31:0] ccu_ctrl_wdata_sig;
    logic ccu_write_error_sig;
    logic [31:0] ccu_error_wdata_sig;
    logic ccu_notify_direct_start_sig;
    logic ccu_notify_direct_stop_sig;
    logic ccu_notify_direct_reset_sig;
    logic [1:0] ccu_noc_action_sig;
    logic       ccu_spm_soft_reset_sig;
    cluster_mode_e ccu_mode_sig;
    cluster_substate_e ccu_substate_sig;
    logic [31:0] ccu_error_code_sig;
    logic       ccu_layer_active_sig;
    logic       ccu_stop_pending_sig;
    logic       ccu_soft_reset_pending_sig;
    logic       ccu_done_sticky_sig;
    logic [31:0] ccu_status_word_sig;

    logic [63:0] cluster_run_cycles_reg;
    logic [31:0] cmd_stub_reg0;
    logic [31:0] cmd_stub_reg1;

    wire noc_quiesced_w;
    wire spm_quiesced_w;

    function automatic logic in_range(input logic [31:0] addr, input logic [31:0] base, input logic [31:0] size);
        return (addr >= base) && (addr < (base + size));
    endfunction

    function automatic logic [31:0] apply_wstrb(input logic [31:0] current, input logic [31:0] wdata, input logic [3:0] wstrb);
        logic [31:0] merged;
        merged = current;
        for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
            if (wstrb[byte_idx]) begin
                merged[byte_idx*8 +: 8] = wdata[byte_idx*8 +: 8];
            end
        end
        return merged;
    endfunction

    function automatic logic [31:0] compose_noc_cmd(input logic [1:0] action);
        logic [31:0] cmd;
        cmd = 32'h0;
        unique case (action)
            CLUSTER_ACTION_NOC_START: cmd[3:0] = CMD_START_PE;
            CLUSTER_ACTION_NOC_STOP:  cmd[3:0] = CMD_STOP_PE;
            CLUSTER_ACTION_NOC_RESET: cmd[3:0] = CMD_RESET;
            default:                  cmd[3:0] = CMD_INIT;
        endcase
        return cmd;
    endfunction

    function automatic logic [31:0] spm_readback(input logic [31:0] off);
        logic [31:0] r;
        r = 32'h0;
        unique case (off)
            K_SPM_CFG_MAP:       r = {24'h0, spm_cfg_map_reg};
            K_SPM_ARB_POLICY:    r = {31'h0, spm_arb_policy_reg};
            K_SPM_PMU_CYCLE_LO:  r = spm_pmu_cycle_cnt_sig[31:0];
            K_SPM_PMU_CYCLE_HI:  r = spm_pmu_cycle_cnt_sig[63:32];
            K_SPM_PMU_ARB_LO:    r = spm_pmu_arb_stall_cnt_sig[31:0];
            K_SPM_PMU_ARB_HI:    r = spm_pmu_arb_stall_cnt_sig[63:32];
            K_SPM_PMU_CREDIT_LO: r = spm_pmu_credit_stall_cnt_sig[31:0];
            K_SPM_PMU_CREDIT_HI: r = spm_pmu_credit_stall_cnt_sig[63:32];
            default: begin
                for (int p = 0; p < SPM_NUM_NOC_CHANNEL; p++) begin
                    if (off == (K_SPM_PMU_PORT_BASE + p*8))       r = spm_pmu_port_txn_cnt_sig[p][31:0];
                    if (off == (K_SPM_PMU_PORT_BASE + p*8 + 4))   r = spm_pmu_port_txn_cnt_sig[p][63:32];
                end
            end
        endcase
        return r;
    endfunction

    function automatic logic [31:0] cluster_readback(input logic [31:0] off);
        logic [31:0] r;
        r = 32'h0;
        unique case (off)
            CLUSTER_REG_MODE:       r = ccu_mode_sig;
            CLUSTER_REG_CTRL:       r = 32'h0;
            CLUSTER_REG_STATUS:     r = ccu_status_word_sig;
            CLUSTER_REG_ERROR_CODE: r = ccu_error_code_sig;
            CLUSTER_REG_SUBSTATE:   r = ccu_substate_sig;
            default:                r = 32'h0;
        endcase
        return r;
    endfunction

    function automatic logic [31:0] noc_status_word(input logic quiesced);
        logic [31:0] r;
        r = 32'h0;
        r[0] = !quiesced;
        r[1] = quiesced;
        return r;
    endfunction

    wire cmd_wr_fire = power_enable_i && cmd_req_valid_i && cmd_req_write_i;
    wire cmd_rd_fire = power_enable_i && cmd_req_valid_i && !cmd_req_write_i;
    wire ahb_wr_fire = power_enable_i && hsel_i && hready_i && htrans_i[1] && hwrite_i;
    wire ahb_rd_fire = power_enable_i && hsel_i && hready_i && htrans_i[1] && !hwrite_i;

    wire [31:0] wr_addr_w  = cmd_wr_fire ? cmd_req_addr_i  : haddr_i;
    wire [31:0] wr_data_w  = cmd_wr_fire ? apply_wstrb(32'h0, cmd_req_wdata_i, cmd_req_wstrb_i) : hwdata_i;
    wire        wr_valid_w = cmd_wr_fire || ahb_wr_fire;
    wire [31:0] rd_addr_w  = cmd_rd_fire ? cmd_req_addr_i  : haddr_i;
    wire        rd_valid_w = cmd_rd_fire || ahb_rd_fire;

    assign local_reset_n = reset_n && power_enable_i;

    assign cmd_req_ready_o = power_enable_i;
    assign hready_o        = power_enable_i;
    assign hresp_o         = 1'b0;

    assign s_axi_awready_o = power_enable_i ? s_axi_awready_sig : 1'b0;
    assign s_axi_wready_o  = power_enable_i ? s_axi_wready_sig  : 1'b0;
    assign s_axi_bvalid_o  = power_enable_i ? s_axi_bvalid_sig  : 1'b0;
    assign s_axi_bresp_o   = power_enable_i ? s_axi_bresp_sig   : 2'b00;
    assign s_axi_arready_o = power_enable_i ? s_axi_arready_sig : 1'b0;
    assign s_axi_rvalid_o  = power_enable_i ? s_axi_rvalid_sig  : 1'b0;
    assign s_axi_rdata_o   = power_enable_i ? s_axi_rdata_sig   : '0;
    assign s_axi_rresp_o   = power_enable_i ? s_axi_rresp_sig   : 2'b00;

    assign interrupt_o     = power_enable_i && (hddu_interrupt_sig || ccu_done_sticky_sig);

    assign spm_cfg_update_pulse = wr_valid_w && in_range(wr_addr_w, K_CMD_SPM_BASE, K_CMD_SPM_SIZE) && ((wr_addr_w - K_CMD_SPM_BASE) == K_SPM_CFG_UPDATE) && wr_data_w[0];
    assign spm_pmu_rst_pulse    = wr_valid_w && in_range(wr_addr_w, K_CMD_SPM_BASE, K_CMD_SPM_SIZE) && ((wr_addr_w - K_CMD_SPM_BASE) == K_SPM_PMU_CTRL) && wr_data_w[0];

    assign hddu_mmio_write_sig  = wr_valid_w && in_range(wr_addr_w, K_CMD_HDDU_BASE, K_CMD_HDDU_SIZE);
    assign hddu_mmio_addr_sig   = (wr_valid_w && in_range(wr_addr_w, K_CMD_HDDU_BASE, K_CMD_HDDU_SIZE)) ? (wr_addr_w - K_CMD_HDDU_BASE)
                              : (rd_valid_w && in_range(rd_addr_w, K_CMD_HDDU_BASE, K_CMD_HDDU_SIZE)) ? (rd_addr_w - K_CMD_HDDU_BASE)
                              : 32'h0;
    assign hddu_mmio_wdata_sig  = wr_data_w;

    assign ccu_write_mode_sig   = wr_valid_w && in_range(wr_addr_w, K_CMD_CLUSTER_BASE, K_CMD_CLUSTER_SIZE) && ((wr_addr_w - K_CMD_CLUSTER_BASE) == CLUSTER_REG_MODE);
    assign ccu_mode_wdata_sig   = wr_data_w;
    assign ccu_write_ctrl_sig   = wr_valid_w && in_range(wr_addr_w, K_CMD_CLUSTER_BASE, K_CMD_CLUSTER_SIZE) && ((wr_addr_w - K_CMD_CLUSTER_BASE) == CLUSTER_REG_CTRL);
    assign ccu_ctrl_wdata_sig   = wr_data_w;
    assign ccu_write_error_sig  = wr_valid_w && in_range(wr_addr_w, K_CMD_CLUSTER_BASE, K_CMD_CLUSTER_SIZE) && ((wr_addr_w - K_CMD_CLUSTER_BASE) == CLUSTER_REG_ERROR_CODE);
    assign ccu_error_wdata_sig  = wr_data_w;

    assign ccu_notify_direct_start_sig = wr_valid_w && in_range(wr_addr_w, K_CMD_NOC_BASE, K_CMD_NOC_SIZE) && ((wr_addr_w - K_CMD_NOC_BASE) == K_NOC_CMD_DATA) && (wr_data_w[3:0] == CMD_START_PE);
    assign ccu_notify_direct_stop_sig  = wr_valid_w && in_range(wr_addr_w, K_CMD_NOC_BASE, K_CMD_NOC_SIZE) && ((wr_addr_w - K_CMD_NOC_BASE) == K_NOC_CMD_DATA) && (wr_data_w[3:0] == CMD_STOP_PE);
    assign ccu_notify_direct_reset_sig = wr_valid_w && in_range(wr_addr_w, K_CMD_NOC_BASE, K_CMD_NOC_SIZE) && ((wr_addr_w - K_CMD_NOC_BASE) == K_NOC_CMD_DATA) && (wr_data_w[3:0] == CMD_RESET);

    assign spm_soft_reset_sig = ccu_spm_soft_reset_sig;
    assign spm_drop_noc_resp_sig = 1'b0;

    assign noc_command_mode_sig = (ccu_noc_action_sig != CLUSTER_ACTION_NONE)
                               || (wr_valid_w && in_range(wr_addr_w, K_CMD_NOC_BASE, K_CMD_NOC_SIZE) && ((wr_addr_w - K_CMD_NOC_BASE) == K_NOC_CMD_DATA));
    assign noc_command_data_sig = (ccu_noc_action_sig != CLUSTER_ACTION_NONE) ? compose_noc_cmd(ccu_noc_action_sig) : wr_data_w;

    assign noc_quiesced_w = noc_hw_quiesced_sig;
    assign spm_quiesced_w = !(hddu_spm_req_valid_sig[0] || hddu_spm_req_valid_sig[1] || hddu_spm_req_valid_sig[2] || hddu_spm_req_valid_sig[3]
                           || hddu_spm_resp_valid_sig[0] || hddu_spm_resp_valid_sig[1] || hddu_spm_resp_valid_sig[2] || hddu_spm_resp_valid_sig[3]
                           || s_axi_bvalid_sig || s_axi_rvalid_sig);

    always_comb begin
        cmd_resp_valid_o = cmd_rd_fire;
        cmd_resp_rdata_o = 32'h0;
        cmd_resp_err_o   = 1'b0;
        hrdata_o         = 32'h0;

        if (rd_valid_w) begin
            logic [31:0] rdata;
            logic        err;
            rdata = 32'h0;
            err   = 1'b0;
            if (cmd_rd_fire && (rd_addr_w == 32'h0000_0000)) begin
                rdata = cmd_stub_reg0;
            end else if (cmd_rd_fire && (rd_addr_w == 32'h0000_0004)) begin
                rdata = cmd_stub_reg1;
            end else if (in_range(rd_addr_w, K_CMD_SPM_BASE, K_CMD_SPM_SIZE)) begin
                rdata = spm_readback(rd_addr_w - K_CMD_SPM_BASE);
            end else if (in_range(rd_addr_w, K_CMD_HDDU_BASE, K_CMD_HDDU_SIZE)) begin
                rdata = hddu_mmio_rdata_sig;
            end else if (in_range(rd_addr_w, K_CMD_NOC_BASE, K_CMD_NOC_SIZE)) begin
                if ((rd_addr_w - K_CMD_NOC_BASE) == K_NOC_STATUS) begin
                    rdata = noc_status_word(noc_quiesced_w);
                end else begin
                    rdata = noc_last_cmd_reg;
                end
            end else if (in_range(rd_addr_w, K_CMD_CLUSTER_BASE, K_CMD_CLUSTER_SIZE)) begin
                rdata = cluster_readback(rd_addr_w - K_CMD_CLUSTER_BASE);
            end else begin
                err = 1'b1;
            end

            if (cmd_rd_fire) begin
                cmd_resp_rdata_o = rdata;
                cmd_resp_err_o   = err;
            end
            if (ahb_rd_fire) begin
                hrdata_o         = rdata;
            end
        end
    end

    always_ff @(posedge clk or negedge local_reset_n) begin
        if (!local_reset_n) begin
            spm_cfg_map_reg      <= 8'hE4;
            spm_arb_policy_reg   <= 1'b0;
            noc_last_cmd_reg     <= 32'h0;
            cluster_run_cycles_reg <= 64'h0;
            cmd_stub_reg0        <= 32'hDEAD_BEEF;
            cmd_stub_reg1        <= 32'h0;
        end else begin
            if (cmd_wr_fire && (wr_addr_w == 32'h0000_0000)) begin
                cmd_stub_reg0 <= wr_data_w;
            end
            if (cmd_wr_fire && (wr_addr_w == 32'h0000_0004)) begin
                cmd_stub_reg1 <= wr_data_w;
            end
            if (wr_valid_w && in_range(wr_addr_w, K_CMD_SPM_BASE, K_CMD_SPM_SIZE)) begin
                unique case (wr_addr_w - K_CMD_SPM_BASE)
                    K_SPM_CFG_MAP:    spm_cfg_map_reg    <= wr_data_w[7:0];
                    K_SPM_ARB_POLICY: spm_arb_policy_reg <= wr_data_w[0];
                    default: ;
                endcase
            end
            if (wr_valid_w && in_range(wr_addr_w, K_CMD_NOC_BASE, K_CMD_NOC_SIZE) && ((wr_addr_w - K_CMD_NOC_BASE) == K_NOC_CMD_DATA)) begin
                noc_last_cmd_reg <= wr_data_w;
            end
            if (ccu_layer_active_sig) begin
                cluster_run_cycles_reg <= cluster_run_cycles_reg + 64'd1;
            end
        end
    end

    // synopsys translate_off
    always_ff @(posedge clk) begin
        if (local_reset_n) begin
            if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO")) && wr_valid_w) begin
                if (in_range(wr_addr_w, K_CMD_SPM_BASE, K_CMD_SPM_SIZE)) begin
                    $display("[%0t] [TRACE][CLUSTER][MMIO][SPM] addr=0x%08x off=0x%08x data=0x%08x",
                             $time, wr_addr_w, wr_addr_w - K_CMD_SPM_BASE, wr_data_w);
                end else if (in_range(wr_addr_w, K_CMD_HDDU_BASE, K_CMD_HDDU_SIZE)) begin
                    $display("[%0t] [TRACE][CLUSTER][MMIO][HDDU] addr=0x%08x off=0x%08x data=0x%08x",
                             $time, wr_addr_w, wr_addr_w - K_CMD_HDDU_BASE, wr_data_w);
                end else if (in_range(wr_addr_w, K_CMD_NOC_BASE, K_CMD_NOC_SIZE)) begin
                    $display("[%0t] [TRACE][CLUSTER][MMIO][NOC] addr=0x%08x off=0x%08x data=0x%08x cmd=0x%01x",
                             $time, wr_addr_w, wr_addr_w - K_CMD_NOC_BASE, wr_data_w, wr_data_w[3:0]);
                end else if (in_range(wr_addr_w, K_CMD_CLUSTER_BASE, K_CMD_CLUSTER_SIZE)) begin
                    $display("[%0t] [TRACE][CLUSTER][MMIO][CTRL] addr=0x%08x off=0x%08x data=0x%08x",
                             $time, wr_addr_w, wr_addr_w - K_CMD_CLUSTER_BASE, wr_data_w);
                end
            end

            if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                && (ccu_noc_action_sig != CLUSTER_ACTION_NONE)) begin
                $display("[%0t] [TRACE][CLUSTER][CCU] noc_action=%0d layer_active=%0b stop_pending=%0b soft_reset_pending=%0b noc_quiesced=%0b spm_quiesced=%0b",
                         $time,
                         ccu_noc_action_sig,
                         ccu_layer_active_sig,
                         ccu_stop_pending_sig,
                         ccu_soft_reset_pending_sig,
                         noc_quiesced_w,
                         spm_quiesced_w);
            end
        end
    end
    // synopsys translate_on

    ScratchpadMemory #(
        .NUM_NOC_PORTS(SPM_NUM_NOC_CHANNEL),
        .BANKS_PER_GROUP(SPM_NUM_BANKS_PER_GROUP),
        .BANK_DATA_WIDTH(SPM_SRAM_BANK_WIDTH_BITS),
        .BANK_DEPTH(SPM_SRAM_BANK_DEPTH_WORDS),
        .SRAM_BANK_LATENCY(SPM_SRAM_BANK_LATENCY),
        .SRAM_BANK_PIPELINE_DEPTH(SPM_SRAM_BANK_PIPELINE_DEPTH),
        .ADDR_WIDTH(SPM_ADDR_WIDTH)
    ) spm (
        .clk(clk),
        .reset_n(local_reset_n),
        .pmu_rst_i(spm_pmu_rst_pulse),
        .drop_noc_resp_i(spm_drop_noc_resp_sig),
        .soft_reset_i(spm_soft_reset_sig),
        .config_map_i(spm_cfg_map_reg),
        .config_update_i(spm_cfg_update_pulse),
        .arb_policy_i(spm_arb_policy_reg),
        .spm_req_valid_i(hddu_spm_req_valid_sig),
        .spm_req_ready_o(hddu_spm_req_ready_sig),
        .spm_req_i(hddu_spm_req_payload_sig),
        .spm_resp_valid_o(hddu_spm_resp_valid_sig),
        .spm_resp_ready_i(hddu_spm_resp_ready_sig),
        .spm_resp_o(hddu_spm_resp_payload_sig),
        .s_axi_awvalid_i(s_axi_awvalid_i),
        .s_axi_awready_o(s_axi_awready_sig),
        .s_axi_awaddr_i(s_axi_awaddr_i),
        .s_axi_wvalid_i(s_axi_wvalid_i),
        .s_axi_wready_o(s_axi_wready_sig),
        .s_axi_wdata_i(s_axi_wdata_i),
        .s_axi_wstrb_i(s_axi_wstrb_i),
        .s_axi_bvalid_o(s_axi_bvalid_sig),
        .s_axi_bready_i(s_axi_bready_i),
        .s_axi_bresp_o(s_axi_bresp_sig),
        .s_axi_arvalid_i(s_axi_arvalid_i),
        .s_axi_arready_o(s_axi_arready_sig),
        .s_axi_araddr_i(s_axi_araddr_i),
        .s_axi_rvalid_o(s_axi_rvalid_sig),
        .s_axi_rready_i(s_axi_rready_i),
        .s_axi_rdata_o(s_axi_rdata_sig),
        .s_axi_rresp_o(s_axi_rresp_sig),
        .pmu_cycle_cnt_o(spm_pmu_cycle_cnt_sig),
        .pmu_port_txn_cnt_o(spm_pmu_port_txn_cnt_sig),
        .pmu_arb_stall_cnt_o(spm_pmu_arb_stall_cnt_sig),
        .pmu_credit_stall_cnt_o(spm_pmu_credit_stall_cnt_sig)
    );

    HybridDataDeliverUnit #(
        .SPM_ADDR_BITS(SPM_ADDR_WIDTH),
        .NOC_TAG_BITS(6),
        .DATA_BITS(HDDU_DATA_BITS)
    ) hddu (
        .clk(clk),
        .reset_n(local_reset_n),
        .spm_req_valid(hddu_spm_req_valid_sig),
        .spm_req_ready(hddu_spm_req_ready_sig),
        .spm_req_payload(hddu_spm_req_payload_sig),
        .spm_resp_valid(hddu_spm_resp_valid_sig),
        .spm_resp_ready(hddu_spm_resp_ready_sig),
        .spm_resp_payload(hddu_spm_resp_payload_sig),
        .noc_ps_out_data(hddu_noc_ps_data),
        .noc_ps_out_addr(hddu_noc_ps_addr),
        .noc_ps_out_mask(hddu_noc_ps_mask),
        .noc_ps_out_valid(hddu_noc_ps_valid),
        .noc_ps_out_ready(hddu_noc_ps_ready),
        .noc_pd_out_data(hddu_noc_pd_data),
        .noc_pd_out_addr(hddu_noc_pd_addr),
        .noc_pd_out_mask(hddu_noc_pd_mask),
        .noc_pd_out_valid(hddu_noc_pd_valid),
        .noc_pd_out_ready(hddu_noc_pd_ready),
        .noc_pli_out_data(hddu_noc_pli_data),
        .noc_pli_out_addr(hddu_noc_pli_addr),
        .noc_pli_out_mask(hddu_noc_pli_mask),
        .noc_pli_out_valid(hddu_noc_pli_valid),
        .noc_pli_out_ready(hddu_noc_pli_ready),
        .noc_plo_out_addr(hddu_noc_plo_addr),
        .noc_plo_out_valid(hddu_noc_plo_valid),
        .noc_plo_out_ready(hddu_noc_plo_ready),
        .noc_plo_in_data(hddu_noc_plo_resp_data),
        .noc_plo_in_status(hddu_noc_plo_resp_status),
        .noc_plo_in_valid(hddu_noc_plo_resp_valid),
        .noc_plo_in_ready(hddu_noc_plo_resp_ready),
        .noc_quiesced_i(noc_hw_quiesced_sig),
        .mmio_addr(hddu_mmio_addr_sig),
        .mmio_write(hddu_mmio_write_sig),
        .mmio_wdata(hddu_mmio_wdata_sig),
        .mmio_rdata(hddu_mmio_rdata_sig),
        .interrupt(hddu_interrupt_sig)
    );

    NetworkOnChip #(
        .NUM_PORTS(NOC_NUM_PORTS),
        .PORT_WIDTH_BITS(NOC_PORT_WIDTH_BITS),
        .NUM_PES_PER_PORT(NOC_NUM_PES_PER_PORT),
        .PE_FIFO_DEPTH(PE_FIFO_DEPTH),
        .NOC_FIFO_DEPTH(NOC_FIFO_DEPTH)
    ) noc (
        .clk(clk),
        .reset_n(local_reset_n),
        .command_mode(noc_command_mode_sig),
        .command_data(noc_command_data_sig),
        .noc_ps_in_data(hddu_noc_ps_data),
        .noc_ps_in_addr(hddu_noc_ps_addr),
        .noc_ps_in_mask(hddu_noc_ps_mask),
        .noc_ps_in_valid(hddu_noc_ps_valid),
        .noc_ps_in_ready(hddu_noc_ps_ready),
        .noc_pd_in_data(hddu_noc_pd_data),
        .noc_pd_in_addr(hddu_noc_pd_addr),
        .noc_pd_in_mask(hddu_noc_pd_mask),
        .noc_pd_in_valid(hddu_noc_pd_valid),
        .noc_pd_in_ready(hddu_noc_pd_ready),
        .noc_pli_in_data(hddu_noc_pli_data),
        .noc_pli_in_addr(hddu_noc_pli_addr),
        .noc_pli_in_mask(hddu_noc_pli_mask),
        .noc_pli_in_valid(hddu_noc_pli_valid),
        .noc_pli_in_ready(hddu_noc_pli_ready),
        .noc_plo_in_data('{addr: hddu_noc_plo_addr}),
        .noc_plo_in_valid(hddu_noc_plo_valid),
        .noc_plo_in_ready(hddu_noc_plo_ready),
        .noc_plo_out_data(hddu_noc_plo_resp_data),
        .noc_plo_out_status(hddu_noc_plo_resp_status),
        .noc_plo_out_valid(hddu_noc_plo_resp_valid),
        .noc_plo_out_ready(hddu_noc_plo_resp_ready),
        .quiesced_o(noc_hw_quiesced_sig)
    );

    ClusterControlUnit ccu (
        .clk(clk),
        .reset_n(local_reset_n),
        .write_mode_i(ccu_write_mode_sig),
        .mode_wdata_i(ccu_mode_wdata_sig),
        .write_ctrl_i(ccu_write_ctrl_sig),
        .ctrl_wdata_i(ccu_ctrl_wdata_sig),
        .write_error_i(ccu_write_error_sig),
        .error_wdata_i(ccu_error_wdata_sig),
        .notify_direct_start_i(ccu_notify_direct_start_sig),
        .notify_direct_stop_i(ccu_notify_direct_stop_sig),
        .notify_direct_reset_i(ccu_notify_direct_reset_sig),
        .noc_quiesced_i(noc_quiesced_w),
        .spm_quiesced_i(spm_quiesced_w),
        .noc_action_o(ccu_noc_action_sig),
        .spm_soft_reset_o(ccu_spm_soft_reset_sig),
        .mode_o(ccu_mode_sig),
        .substate_o(ccu_substate_sig),
        .error_code_o(ccu_error_code_sig),
        .layer_active_o(ccu_layer_active_sig),
        .stop_pending_o(ccu_stop_pending_sig),
        .soft_reset_pending_o(ccu_soft_reset_pending_sig),
        .done_sticky_o(ccu_done_sticky_sig),
        .status_word_o(ccu_status_word_sig)
    );

endmodule