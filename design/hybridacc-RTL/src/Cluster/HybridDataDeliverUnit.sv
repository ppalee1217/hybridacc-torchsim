//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   HybridDataDeliverUnit
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Functional RTL baseline for the Cluster HybridDataDeliverUnit.
//                This version preserves the ESL-visible contract and the
//                AGU/SPM/NoC integration semantics needed by ComputeCluster
//                bring-up, while simplifying the internal FIFO depth to
//                one outstanding transaction per plane.
// Dependencies:  src/hybridacc_utils_pkg.sv, src/Cluster/cluster_pkg.sv,
//                src/Cluster/AddressGenerateUnit.sv
// Revision:
//   2026/04/27 - Initial version (M1 cluster datapath rewrite)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;
import cluster_pkg::*;

module HybridDataDeliverUnit #(
    parameter int unsigned SPM_ADDR_BITS = 32,
    parameter int unsigned NOC_TAG_BITS  = 6,
    parameter int unsigned DATA_BITS     = 192
) (
    input  logic             clk,
    input  logic             reset_n,

    output logic             spm_req_valid [4],
    input  logic             spm_req_ready [4],
    output spm_req_32_192_t  spm_req_payload[4],
    input  logic             spm_resp_valid[4],
    output logic             spm_resp_ready[4],
    input  spm_resp_192_t    spm_resp_payload[4],

    output logic [DATA_BITS-1:0] noc_ps_out_data,
    output logic [15:0]          noc_ps_out_addr,
    output logic [63:0]          noc_ps_out_mask,
    output logic                 noc_ps_out_valid,
    input  logic                 noc_ps_out_ready,

    output logic [DATA_BITS-1:0] noc_pd_out_data,
    output logic [15:0]          noc_pd_out_addr,
    output logic [63:0]          noc_pd_out_mask,
    output logic                 noc_pd_out_valid,
    input  logic                 noc_pd_out_ready,

    output logic [DATA_BITS-1:0] noc_pli_out_data,
    output logic [15:0]          noc_pli_out_addr,
    output logic [63:0]          noc_pli_out_mask,
    output logic                 noc_pli_out_valid,
    input  logic                 noc_pli_out_ready,

    output logic [15:0]          noc_plo_out_addr,
    output logic                 noc_plo_out_valid,
    input  logic                 noc_plo_out_ready,

    input  logic [DATA_BITS-1:0] noc_plo_in_data,
    input  NOC_RESPONSE_STATUS   noc_plo_in_status,
    input  logic                 noc_plo_in_valid,
    output logic                 noc_plo_in_ready,
    input  logic                 noc_quiesced_i,

    input  logic [31:0]          mmio_addr,
    input  logic                 mmio_write,
    input  logic [31:0]          mmio_wdata,
    output logic [31:0]          mmio_rdata,

    output logic                 interrupt
);
    localparam int unsigned NUM_AGU         = 4;
    localparam int unsigned NUM_SEND_PLANES = 3;
    localparam int unsigned RECV_PLANE      = 3;
    localparam int unsigned DATA_BYTES      = DATA_BITS / 8;

    localparam logic [31:0] MMIO_AGU_BASE        = 32'h0000_0000;
    localparam logic [31:0] MMIO_AGU_BANK_STRIDE = 32'h0000_0100;
    localparam logic [31:0] MMIO_AGU_SIZE        = NUM_AGU * MMIO_AGU_BANK_STRIDE;
    localparam logic [31:0] MMIO_GLOBAL_BASE     = 32'h0000_0800;
    localparam logic [31:0] MMIO_GLOBAL_CTRL     = 32'h0000_0800;
    localparam logic [31:0] MMIO_GLOBAL_STATUS   = 32'h0000_0804;
    localparam logic [31:0] MMIO_GLOBAL_PLANE_EN = 32'h0000_0808;
    localparam logic [31:0] MMIO_GLOBAL_PLANE_MODE = 32'h0000_080C;
    localparam logic [31:0] MMIO_GLOBAL_NUM_PLANES = 32'h0000_0810;
    localparam logic [31:0] MMIO_GLOBAL_PORT_WIDTH = 32'h0000_0814;
    localparam logic [31:0] MMIO_GLOBAL_ARB_POLICY = 32'h0000_0818;
    localparam logic [31:0] MMIO_GLOBAL_ERR_CODE   = 32'h0000_081C;
    localparam logic [31:0] MMIO_GLOBAL_ERR_INFO0  = 32'h0000_0820;
    localparam logic [31:0] MMIO_GLOBAL_ERR_INFO1  = 32'h0000_0824;
    localparam logic [31:0] MMIO_GLOBAL_COUNTER_TX_PKT  = 32'h0000_0828;
    localparam logic [31:0] MMIO_GLOBAL_COUNTER_TX_BYTE = 32'h0000_082C;
    localparam logic [31:0] MMIO_GLOBAL_COUNTER_RX_BYTE = 32'h0000_0830;
    localparam logic [31:0] MMIO_GLOBAL_COUNTER_STALL   = 32'h0000_0834;

    localparam logic [3:0] DEFAULT_PLANE_EN = 4'hF;

    logic [31:0] plane_en_reg;
    logic [31:0] plane_mode_reg;
    logic [31:0] global_ctrl_reg;
    logic [31:0] arb_policy_reg;
    logic [31:0] err_code_reg;
    logic [31:0] err_info0_reg;
    logic [31:0] err_info1_reg;
    logic [31:0] counter_tx_pkt_reg;
    logic [31:0] counter_tx_byte_reg;
    logic [31:0] counter_rx_byte_reg;
    logic [31:0] counter_stall_reg;
    logic        done_sticky_reg;
    logic        done_pending_reg;

    logic                     agu_cfg_write_sig [NUM_AGU];
    logic [7:0]               agu_cfg_addr_sig  [NUM_AGU];
    logic [31:0]              agu_cfg_wdata_sig [NUM_AGU];
    logic [31:0]              agu_cfg_rdata_sig [NUM_AGU];
    logic                     agu_start_sig     [NUM_AGU];
    logic                     agu_stop_sig      [NUM_AGU];
    logic                     agu_gen_valid_sig [NUM_AGU];
    logic                     agu_gen_ready_sig [NUM_AGU];
    logic [31:0]              agu_gen_addr_sig  [NUM_AGU];
    logic [15:0]              agu_gen_tag_sig   [NUM_AGU];
    logic                     agu_gen_ultra_sig [NUM_AGU];
    logic [15:0]              agu_gen_mask_sig  [NUM_AGU];
    logic                     agu_busy_sig      [NUM_AGU];
    logic                     agu_done_sig      [NUM_AGU];
    logic [1:0]               agu_fsm_state_sig [NUM_AGU];

    logic                     send_wait_valid_reg [NUM_SEND_PLANES];
    logic [15:0]              send_wait_addr_reg  [NUM_SEND_PLANES];
    logic [63:0]              send_wait_mask_reg  [NUM_SEND_PLANES];
    logic                     send_hold_valid_reg [NUM_SEND_PLANES];
    logic [DATA_BITS-1:0]     send_hold_data_reg  [NUM_SEND_PLANES];
    logic [15:0]              send_hold_addr_reg  [NUM_SEND_PLANES];
    logic [63:0]              send_hold_mask_reg  [NUM_SEND_PLANES];

    logic                     recv_addr_pending_reg;
    logic [SPM_ADDR_BITS-1:0] recv_write_addr_reg;

    wire hddu_busy_w;
    assign hddu_busy_w = agu_busy_sig[0] | agu_busy_sig[1] | agu_busy_sig[2] | agu_busy_sig[3]
                       | send_wait_valid_reg[0] | send_wait_valid_reg[1] | send_wait_valid_reg[2]
                       | send_hold_valid_reg[0] | send_hold_valid_reg[1] | send_hold_valid_reg[2]
                       | recv_addr_pending_reg;

    genvar g;
    generate
        for (g = 0; g < NUM_AGU; g++) begin : gen_agu
            AddressGenerateUnit #(
                .AGU_INDEX(g)
            ) agu (
                .clk(clk),
                .reset_n(reset_n),
                .cfg_write(agu_cfg_write_sig[g]),
                .cfg_addr(agu_cfg_addr_sig[g]),
                .cfg_wdata(agu_cfg_wdata_sig[g]),
                .cfg_rdata(agu_cfg_rdata_sig[g]),
                .start(agu_start_sig[g]),
                .stop(agu_stop_sig[g]),
                .gen_valid(agu_gen_valid_sig[g]),
                .gen_ready(agu_gen_ready_sig[g]),
                .gen_addr(agu_gen_addr_sig[g]),
                .gen_tag(agu_gen_tag_sig[g]),
                .gen_ultra(agu_gen_ultra_sig[g]),
                .gen_mask(agu_gen_mask_sig[g]),
                .busy(agu_busy_sig[g]),
                .done(agu_done_sig[g]),
                .fsm_state(agu_fsm_state_sig[g])
            );
        end
    endgenerate

    // ---------------------------------------------------------------------
    // MMIO combinational wiring to AGU banks and global read mux.
    // ---------------------------------------------------------------------
    always_comb begin
        logic is_agu_bank;
        logic [1:0] agu_bank;
        logic [7:0] agu_subaddr;
        logic [31:0] status_word;

        is_agu_bank = (mmio_addr < MMIO_AGU_SIZE);
        agu_bank    = mmio_addr[9:8];
        agu_subaddr = mmio_addr[7:0];

        for (int i = 0; i < NUM_AGU; i++) begin
            agu_cfg_write_sig[i] = mmio_write && is_agu_bank && (agu_bank == i[1:0]);
            agu_cfg_addr_sig[i]  = (is_agu_bank && (agu_bank == i[1:0])) ? agu_subaddr : 8'h00;
            agu_cfg_wdata_sig[i] = mmio_wdata;
            agu_start_sig[i]     = mmio_write && (mmio_addr == MMIO_GLOBAL_CTRL) && mmio_wdata[CTRL_START] && plane_en_reg[i];
            agu_stop_sig[i]      = (mmio_write && (mmio_addr == MMIO_GLOBAL_CTRL) && mmio_wdata[CTRL_STOP] && plane_en_reg[i])
                                || (mmio_write && (mmio_addr == MMIO_GLOBAL_CTRL) && mmio_wdata[CTRL_SOFT_RESET] && plane_en_reg[i]);
        end

        status_word = 32'h0;
        status_word[STATUS_IDLE]     = !hddu_busy_w;
        status_word[STATUS_BUSY]     = hddu_busy_w;
        status_word[STATUS_DONE]     = done_sticky_reg;
        status_word[STATUS_QUIESCED] = !hddu_busy_w && noc_quiesced_i;
        status_word[STATUS_ERROR]    = (err_code_reg != 32'h0);

        mmio_rdata = 32'h0;
        if (is_agu_bank) begin
            mmio_rdata = agu_cfg_rdata_sig[agu_bank];
        end else begin
            unique case (mmio_addr)
                MMIO_GLOBAL_CTRL:           mmio_rdata = global_ctrl_reg;
                MMIO_GLOBAL_STATUS:         mmio_rdata = status_word;
                MMIO_GLOBAL_PLANE_EN:       mmio_rdata = plane_en_reg;
                MMIO_GLOBAL_PLANE_MODE:     mmio_rdata = plane_mode_reg;
                MMIO_GLOBAL_NUM_PLANES:     mmio_rdata = NUM_AGU;
                MMIO_GLOBAL_PORT_WIDTH:     mmio_rdata = DATA_BITS;
                MMIO_GLOBAL_ARB_POLICY:     mmio_rdata = arb_policy_reg;
                MMIO_GLOBAL_ERR_CODE:       mmio_rdata = err_code_reg;
                MMIO_GLOBAL_ERR_INFO0:      mmio_rdata = err_info0_reg;
                MMIO_GLOBAL_ERR_INFO1:      mmio_rdata = err_info1_reg;
                MMIO_GLOBAL_COUNTER_TX_PKT: mmio_rdata = counter_tx_pkt_reg;
                MMIO_GLOBAL_COUNTER_TX_BYTE:mmio_rdata = counter_tx_byte_reg;
                MMIO_GLOBAL_COUNTER_RX_BYTE:mmio_rdata = counter_rx_byte_reg;
                MMIO_GLOBAL_COUNTER_STALL:  mmio_rdata = counter_stall_reg;
                default:                    mmio_rdata = 32'h0;
            endcase
        end
    end

    // ---------------------------------------------------------------------
    // Combinational datapath wiring.
    // ---------------------------------------------------------------------
    always_comb begin
        for (int i = 0; i < 4; i++) begin
            spm_req_valid[i]   = 1'b0;
            spm_req_payload[i] = '0;
            spm_resp_ready[i]  = 1'b0;
        end

        for (int i = 0; i < NUM_SEND_PLANES; i++) begin
            agu_gen_ready_sig[i] = plane_en_reg[i]
                                 && !send_wait_valid_reg[i]
                                 && !send_hold_valid_reg[i]
                                 && spm_req_ready[i];

            spm_req_valid[i]              = plane_en_reg[i] && agu_gen_valid_sig[i] && agu_gen_ready_sig[i];
            spm_req_payload[i].addr       = agu_gen_addr_sig[i];
            spm_req_payload[i].wdata      = '0;
            spm_req_payload[i].wen        = 1'b0;

            spm_resp_ready[i]             = plane_en_reg[i] && send_wait_valid_reg[i] && !send_hold_valid_reg[i];
        end

        agu_gen_ready_sig[RECV_PLANE]    = plane_en_reg[RECV_PLANE] && !recv_addr_pending_reg && noc_plo_out_ready;

        noc_ps_out_data  = send_hold_data_reg[0];
        noc_ps_out_addr  = send_hold_addr_reg[0];
        noc_ps_out_mask  = send_hold_mask_reg[0];
        noc_ps_out_valid = send_hold_valid_reg[0] && plane_en_reg[0];

        noc_pd_out_data  = send_hold_data_reg[1];
        noc_pd_out_addr  = send_hold_addr_reg[1];
        noc_pd_out_mask  = send_hold_mask_reg[1];
        noc_pd_out_valid = send_hold_valid_reg[1] && plane_en_reg[1];

        noc_pli_out_data  = send_hold_data_reg[2];
        noc_pli_out_addr  = send_hold_addr_reg[2];
        noc_pli_out_mask  = send_hold_mask_reg[2];
        noc_pli_out_valid = send_hold_valid_reg[2] && plane_en_reg[2];

        noc_plo_out_addr  = {9'd0, agu_gen_ultra_sig[RECV_PLANE], agu_gen_tag_sig[RECV_PLANE][5:0]};
        noc_plo_out_valid = plane_en_reg[RECV_PLANE] && !recv_addr_pending_reg && agu_gen_valid_sig[RECV_PLANE];
        noc_plo_in_ready  = plane_en_reg[RECV_PLANE] && recv_addr_pending_reg && spm_req_ready[RECV_PLANE];
        spm_resp_ready[RECV_PLANE] = plane_en_reg[RECV_PLANE];

        spm_req_valid[RECV_PLANE]          = noc_plo_in_valid && noc_plo_in_ready && (noc_plo_in_status == NOC_OK);
        spm_req_payload[RECV_PLANE].addr   = recv_write_addr_reg;
        spm_req_payload[RECV_PLANE].wdata  = noc_plo_in_data;
        spm_req_payload[RECV_PLANE].wen    = 1'b1;

        interrupt = done_sticky_reg;
    end

    // ---------------------------------------------------------------------
    // Sequential control and counters.
    // ---------------------------------------------------------------------
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            plane_en_reg       <= DEFAULT_PLANE_EN;
            plane_mode_reg     <= 32'h0;
            global_ctrl_reg    <= 32'h0;
            arb_policy_reg     <= 32'h0;
            err_code_reg       <= 32'h0;
            err_info0_reg      <= 32'h0;
            err_info1_reg      <= 32'h0;
            counter_tx_pkt_reg <= 32'h0;
            counter_tx_byte_reg<= 32'h0;
            counter_rx_byte_reg<= 32'h0;
            counter_stall_reg  <= 32'h0;
            done_sticky_reg    <= 1'b0;
            done_pending_reg   <= 1'b0;
            recv_addr_pending_reg <= 1'b0;
            recv_write_addr_reg   <= '0;
            for (int i = 0; i < NUM_SEND_PLANES; i++) begin
                send_wait_valid_reg[i] <= 1'b0;
                send_wait_addr_reg[i]  <= 16'h0;
                send_wait_mask_reg[i]  <= 64'h0;
                send_hold_valid_reg[i] <= 1'b0;
                send_hold_data_reg[i]  <= '0;
                send_hold_addr_reg[i]  <= 16'h0;
                send_hold_mask_reg[i]  <= 64'h0;
            end
        end else begin
            // Global MMIO writes
            if (mmio_write) begin
                if (mmio_addr == MMIO_GLOBAL_CTRL) begin
                    global_ctrl_reg <= mmio_wdata;
                    if (mmio_wdata[CTRL_START]) begin
                        done_sticky_reg <= 1'b0;
                        done_pending_reg <= 1'b0;
                        err_code_reg    <= 32'h0;
                    end
                    if (mmio_wdata[CTRL_SOFT_RESET]) begin
                        for (int i = 0; i < NUM_SEND_PLANES; i++) begin
                            send_wait_valid_reg[i] <= 1'b0;
                            send_hold_valid_reg[i] <= 1'b0;
                        end
                        recv_addr_pending_reg <= 1'b0;
                        done_sticky_reg       <= 1'b0;
                        done_pending_reg      <= 1'b0;
                        err_code_reg          <= 32'h0;
                    end
                end else if (mmio_addr == MMIO_GLOBAL_PLANE_EN) begin
                    plane_en_reg <= mmio_wdata;
                end else if (mmio_addr == MMIO_GLOBAL_PLANE_MODE) begin
                    plane_mode_reg <= mmio_wdata;
                end else if (mmio_addr == MMIO_GLOBAL_ARB_POLICY) begin
                    arb_policy_reg <= mmio_wdata;
                end else if (mmio_addr == MMIO_GLOBAL_ERR_CODE) begin
                    err_code_reg <= mmio_wdata;
                end
            end

            // Send plane: AGU -> SPM request handshake
            for (int i = 0; i < NUM_SEND_PLANES; i++) begin
                if (plane_en_reg[i] && agu_gen_valid_sig[i] && agu_gen_ready_sig[i]) begin
                    send_wait_valid_reg[i] <= 1'b1;
                    send_wait_addr_reg[i]  <= {9'd0, agu_gen_ultra_sig[i], agu_gen_tag_sig[i][5:0]};
                    send_wait_mask_reg[i]  <= {48'd0, agu_gen_mask_sig[i]};
                end

                if (spm_resp_valid[i] && spm_resp_ready[i]) begin
                    if (spm_resp_payload[i].code != SPM_OK) begin
                        err_code_reg  <= 32'd3;
                        err_info0_reg <= i;
                    end else begin
                        send_hold_valid_reg[i] <= 1'b1;
                        send_hold_data_reg[i]  <= spm_resp_payload[i].rdata;
                        send_hold_addr_reg[i]  <= send_wait_addr_reg[i];
                        send_hold_mask_reg[i]  <= send_wait_mask_reg[i];
                        counter_tx_pkt_reg     <= counter_tx_pkt_reg + 32'd1;
                        counter_tx_byte_reg    <= counter_tx_byte_reg + DATA_BYTES;
                    end
                    send_wait_valid_reg[i] <= 1'b0;
                end

                if (send_hold_valid_reg[i]) begin
                    logic noc_ready;
                    noc_ready = (i == 0) ? noc_ps_out_ready
                             : (i == 1) ? noc_pd_out_ready
                             :            noc_pli_out_ready;
                    if (noc_ready && plane_en_reg[i]) begin
                        send_hold_valid_reg[i] <= 1'b0;
                    end else begin
                        counter_stall_reg <= counter_stall_reg + 32'd1;
                    end
                end
            end

            // Receive plane: AGU -> NoC read request
            if (plane_en_reg[RECV_PLANE] && !recv_addr_pending_reg && agu_gen_valid_sig[RECV_PLANE] && agu_gen_ready_sig[RECV_PLANE]) begin
                recv_addr_pending_reg <= 1'b1;
                recv_write_addr_reg   <= agu_gen_addr_sig[RECV_PLANE];
            end

            // Receive plane: NoC response -> SPM write
            if (noc_plo_in_valid && noc_plo_in_ready) begin
                if (noc_plo_in_status != NOC_OK) begin
                    err_code_reg       <= 32'd2;
                    err_info0_reg      <= noc_plo_in_status;
                    recv_addr_pending_reg <= 1'b0;
                end else begin
                    recv_addr_pending_reg <= 1'b0;
                    counter_rx_byte_reg   <= counter_rx_byte_reg + DATA_BYTES;
                end
            end else if (recv_addr_pending_reg && !noc_plo_in_valid) begin
                counter_stall_reg <= counter_stall_reg + 32'd1;
            end

            if (spm_resp_valid[RECV_PLANE] && spm_resp_ready[RECV_PLANE]
                && (spm_resp_payload[RECV_PLANE].code != SPM_OK)) begin
                err_code_reg  <= 32'd3;
                err_info0_reg <= RECV_PLANE;
                err_info1_reg <= spm_resp_payload[RECV_PLANE].code;
            end

            if (agu_done_sig[0] || agu_done_sig[1] || agu_done_sig[2] || agu_done_sig[3]) begin
                done_pending_reg <= 1'b1;
            end

            // Sticky done when quiesced after AGU work drained.
            if (!hddu_busy_w && done_pending_reg && noc_quiesced_i) begin
                done_sticky_reg <= 1'b1;
                done_pending_reg <= 1'b0;
            end
        end
    end

    // synopsys translate_off
    always_ff @(posedge clk) begin
        if (reset_n) begin
            if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO")) && mmio_write) begin
                if (mmio_addr < MMIO_AGU_SIZE) begin
                    $display("[%0t] [TRACE][HDDU][MMIO][AGU%0d] off=0x%02x data=0x%08x",
                             $time,
                             mmio_addr[9:8],
                             mmio_addr[7:0],
                             mmio_wdata);
                end else begin
                    $display("[%0t] [TRACE][HDDU][MMIO][GLOBAL] addr=0x%08x data=0x%08x start=%0b stop=%0b soft_reset=%0b plane_en=0x%01x",
                             $time,
                             mmio_addr,
                             mmio_wdata,
                             (mmio_addr == MMIO_GLOBAL_CTRL) ? mmio_wdata[CTRL_START] : 1'b0,
                             (mmio_addr == MMIO_GLOBAL_CTRL) ? mmio_wdata[CTRL_STOP] : 1'b0,
                             (mmio_addr == MMIO_GLOBAL_CTRL) ? mmio_wdata[CTRL_SOFT_RESET] : 1'b0,
                             plane_en_reg[3:0]);
                end
            end

            if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                for (int i = 0; i < NUM_SEND_PLANES; i++) begin
                    logic noc_ready;

                    noc_ready = (i == 0) ? noc_ps_out_ready
                             : (i == 1) ? noc_pd_out_ready
                             :            noc_pli_out_ready;

                    if (plane_en_reg[i] && agu_gen_valid_sig[i] && agu_gen_ready_sig[i]) begin
                        $display("[%0t] [TRACE][HDDU][SEND%0d] agu_issue addr=0x%08x tag=0x%04x ultra=%0b mask=0x%04x",
                                 $time,
                                 i,
                                 agu_gen_addr_sig[i],
                                 agu_gen_tag_sig[i],
                                 agu_gen_ultra_sig[i],
                                 agu_gen_mask_sig[i]);
                    end

                    if (spm_resp_valid[i] && spm_resp_ready[i]) begin
                        $display("[%0t] [TRACE][HDDU][SEND%0d] spm_resp code=%0d hold_addr=0x%04x hold_mask=0x%016x data_lo=0x%016x",
                                 $time,
                                 i,
                                 spm_resp_payload[i].code,
                                 send_wait_addr_reg[i],
                                 send_wait_mask_reg[i],
                                 spm_resp_payload[i].rdata[63:0]);
                    end

                    if (send_hold_valid_reg[i] && plane_en_reg[i] && noc_ready) begin
                        $display("[%0t] [TRACE][HDDU][SEND%0d] noc_tx addr=0x%04x mask=0x%016x data_lo=0x%016x",
                                 $time,
                                 i,
                                 send_hold_addr_reg[i],
                                 send_hold_mask_reg[i],
                                 send_hold_data_reg[i][63:0]);
                    end

                    if (agu_done_sig[i]) begin
                        $display("[%0t] [TRACE][HDDU][SEND%0d] agu_done fsm=%0d busy=%0b",
                                 $time,
                                 i,
                                 agu_fsm_state_sig[i],
                                 agu_busy_sig[i]);
                    end
                end

                if (plane_en_reg[RECV_PLANE] && !recv_addr_pending_reg
                    && agu_gen_valid_sig[RECV_PLANE] && agu_gen_ready_sig[RECV_PLANE]) begin
                    $display("[%0t] [TRACE][HDDU][RECV] noc_read_issue writeback_addr=0x%08x req_addr=0x%04x tag=0x%04x ultra=%0b",
                             $time,
                             agu_gen_addr_sig[RECV_PLANE],
                             {9'd0, agu_gen_ultra_sig[RECV_PLANE], agu_gen_tag_sig[RECV_PLANE][5:0]},
                             agu_gen_tag_sig[RECV_PLANE],
                             agu_gen_ultra_sig[RECV_PLANE]);
                end

                if (noc_plo_in_valid && noc_plo_in_ready) begin
                    $display("[%0t] [TRACE][HDDU][RECV] noc_read_resp status=%0d writeback_addr=0x%08x data_lo=0x%016x",
                             $time,
                             noc_plo_in_status,
                             recv_write_addr_reg,
                             noc_plo_in_data[63:0]);
                end

                if (!hddu_busy_w && done_pending_reg && noc_quiesced_i) begin
                    $display("[%0t] [TRACE][HDDU] done_sticky tx_pkt=%0d tx_byte=%0d rx_byte=%0d stall=%0d err=0x%08x",
                             $time,
                             counter_tx_pkt_reg,
                             counter_tx_byte_reg,
                             counter_rx_byte_reg,
                             counter_stall_reg,
                             err_code_reg);
                end
            end
        end
    end
    // synopsys translate_on

    initial begin
        if ((SPM_ADDR_BITS != CLUSTER_ADDR_WIDTH) || (DATA_BITS != CLUSTER_DATA_WIDTH) || (NOC_TAG_BITS != 6)) begin
            $error("HybridDataDeliverUnit baseline currently supports SPM_ADDR_BITS=32, DATA_BITS=192, NOC_TAG_BITS=6 only");
        end
    end

endmodule