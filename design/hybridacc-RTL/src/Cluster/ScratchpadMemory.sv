//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   ScratchpadMemory
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Functional RTL baseline for Cluster scratchpad memory.
//                This implementation preserves the ESL-visible contract:
//                  * 4 NoC-side ports with port->group remap.
//                  * Linear / parallel addressing modes.
//                  * 64-bit AXI-Lite DMA side port.
//                  * Response backpressure retention.
//                  * PMU counters and soft-reset quiesce behavior.
//                Internal bank/FIFO micro-architecture is simplified versus
//                ESL, but address mapping and observable port behavior are
//                aligned for Cluster bring-up.
// Dependencies:  src/hybridacc_utils_pkg.sv, src/Cluster/cluster_pkg.sv,
//                src/Cluster/ScratchpadMemoryBank.sv
// Revision:
//   2026/04/27 - Initial version (M1 cluster datapath rewrite)
// Additional Comments:
//   Current implementation is intentionally fixed to the active Cluster
//   configuration: ADDR_WIDTH=32, NOC_DATA_WIDTH=192, NUM_NOC_PORTS=4,
//   BANKS_PER_GROUP=3, BANK_DATA_WIDTH=64.
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;
module ScratchpadMemory import cluster_pkg::*; #(
    parameter int unsigned NUM_NOC_PORTS             = 4,
    parameter int unsigned BANKS_PER_GROUP           = 3,
    parameter int unsigned BANK_DATA_WIDTH           = 64,
    parameter int unsigned BANK_DEPTH                = 8192,
    parameter int unsigned SRAM_BANK_LATENCY         = 1,
    parameter int unsigned SRAM_BANK_PIPELINE_DEPTH  = 1,
    parameter int unsigned ADDR_WIDTH                = 32,
    parameter int unsigned MAX_OUTSTANDING           = 8,
    parameter int unsigned DMA_MAX_OUTSTANDING       = 8
) (
    input  logic                        clk,
    input  logic                        reset_n,
    input  logic                        pmu_rst_i,
    input  logic                        soft_reset_i,
    input  logic [7:0]                  config_map_i,
    input  logic                        config_update_i,
    input  logic                        arb_policy_i,

    input  logic                        spm_req_valid_i [NUM_NOC_PORTS],
    output logic                        spm_req_ready_o [NUM_NOC_PORTS],
    input  spm_req_32_192_t             spm_req_i       [NUM_NOC_PORTS],

    output logic                        spm_resp_valid_o[NUM_NOC_PORTS],
    input  logic                        spm_resp_ready_i[NUM_NOC_PORTS],
    output spm_resp_192_t               spm_resp_o      [NUM_NOC_PORTS],

    input  logic                        s_axi_awvalid_i,
    output logic                        s_axi_awready_o,
    input  logic [ADDR_WIDTH-1:0]       s_axi_awaddr_i,

    input  logic                        s_axi_wvalid_i,
    output logic                        s_axi_wready_o,
    input  logic [BANK_DATA_WIDTH-1:0]  s_axi_wdata_i,
    input  logic [BANK_DATA_WIDTH/8-1:0]s_axi_wstrb_i,

    output logic                        s_axi_bvalid_o,
    input  logic                        s_axi_bready_i,
    output logic [1:0]                  s_axi_bresp_o,

    input  logic                        s_axi_arvalid_i,
    output logic                        s_axi_arready_o,
    input  logic [ADDR_WIDTH-1:0]       s_axi_araddr_i,

    output logic                        s_axi_rvalid_o,
    input  logic                        s_axi_rready_i,
    output logic [BANK_DATA_WIDTH-1:0]  s_axi_rdata_o,
    output logic [1:0]                  s_axi_rresp_o,

    output logic [63:0]                 pmu_cycle_cnt_o,
    output logic [63:0]                 pmu_port_txn_cnt_o [NUM_NOC_PORTS],
    output logic [63:0]                 pmu_arb_stall_cnt_o,
    output logic [63:0]                 pmu_credit_stall_cnt_o
);
    localparam int unsigned NUM_GROUPS        = NUM_NOC_PORTS;
    localparam int unsigned TOTAL_BANKS       = NUM_GROUPS * BANKS_PER_GROUP;
    localparam int unsigned NOC_DATA_WIDTH    = BANKS_PER_GROUP * BANK_DATA_WIDTH;
    localparam int unsigned BYTES_PER_BANK    = BANK_DATA_WIDTH / 8;
    localparam int unsigned GROUP_LINEAR_WORDS= BANKS_PER_GROUP * BANK_DEPTH;
    localparam int unsigned GROUP_SPAN_WORDS  = (BANKS_PER_GROUP + 1) * BANK_DEPTH;
    localparam int unsigned GROUP_IDX_W       = (NUM_GROUPS <= 1) ? 1 : $clog2(NUM_GROUPS);
    localparam int unsigned BANK_IDX_W        = (TOTAL_BANKS <= 1) ? 1 : $clog2(TOTAL_BANKS);
    localparam int unsigned BANK_ROW_W        = (BANK_DEPTH <= 1) ? 1 : $clog2(BANK_DEPTH);
    localparam int unsigned MACRO_DEPTH       = 128;
    localparam int          TOTAL_BANKS_GEN   = int'(TOTAL_BANKS);

    typedef logic [BANK_DATA_WIDTH-1:0] bank_word_t;
    typedef logic [BANK_ROW_W-1:0]      bank_row_t;

    logic [GROUP_IDX_W-1:0] active_map_reg [NUM_NOC_PORTS];
    logic                   active_map_init_done_reg;

    logic                   resp_valid_reg [NUM_NOC_PORTS];
    spm_resp_192_t          resp_data_reg  [NUM_NOC_PORTS];
    logic                   noc_read_pending_reg  [NUM_NOC_PORTS];
    logic                   noc_read_parallel_reg [NUM_NOC_PORTS];
    logic [NUM_NOC_PORTS*GROUP_IDX_W-1:0] noc_read_group_reg;
    logic [BANK_IDX_W-1:0]  noc_read_bank_reg     [NUM_NOC_PORTS];
    bank_row_t              noc_read_row_reg      [NUM_NOC_PORTS];

    logic                   aw_pending_valid_reg;
    logic [ADDR_WIDTH-1:0]  aw_pending_addr_reg;
    logic                   w_pending_valid_reg;
    logic [BANK_DATA_WIDTH-1:0]   w_pending_data_reg;
    logic [BANK_DATA_WIDTH/8-1:0] w_pending_strb_reg;
    logic                   ar_pending_valid_reg;
    logic [ADDR_WIDTH-1:0]  ar_pending_addr_reg;
    logic                   bvalid_reg;
    logic [1:0]             bresp_reg;
    logic                   rvalid_reg;
    logic [BANK_DATA_WIDTH-1:0] rdata_reg;
    logic [1:0]             rresp_reg;
    logic                   dma_read_pending_reg;
    logic [BANK_IDX_W-1:0]  dma_read_bank_reg;
    bank_row_t              dma_read_row_reg;

    logic [63:0]            pmu_cycle_cnt_reg;
    logic [63:0]            pmu_port_txn_cnt_reg [NUM_NOC_PORTS];
    logic [63:0]            pmu_arb_stall_cnt_reg;
    logic [63:0]            pmu_credit_stall_cnt_reg;

    logic                   noc_accept_w   [NUM_NOC_PORTS];
    logic                   noc_parallel_w [NUM_NOC_PORTS];
    logic [GROUP_IDX_W-1:0] noc_group_w    [NUM_NOC_PORTS];
    logic [BANK_IDX_W-1:0]  noc_bank_idx_w [NUM_NOC_PORTS];
    bank_row_t              noc_row_w      [NUM_NOC_PORTS];
    logic                   dma_write_issue_w;
    logic [BANK_IDX_W-1:0]  dma_write_bank_idx_w;
    bank_row_t              dma_write_row_w;
    logic                   dma_read_issue_w;
    logic [BANK_IDX_W-1:0]  dma_read_bank_idx_w;
    bank_row_t              dma_read_row_w;

    logic                   bank_ceb_w            [TOTAL_BANKS];
    logic                   bank_web_w            [TOTAL_BANKS];
    bank_row_t              bank_addr_w           [TOTAL_BANKS];
    bank_word_t             bank_d_w              [TOTAL_BANKS];
    bank_word_t             bank_bweb_w           [TOTAL_BANKS];
    bank_word_t             bank_q_w              [TOTAL_BANKS];
    logic                   bank_pudelay_unused_w [TOTAL_BANKS];

    function automatic bank_word_t expand_strb_to_bweb(
        input logic [BANK_DATA_WIDTH/8-1:0] strb
    );
        bank_word_t mask;
        for (int unsigned byte_idx = 0; byte_idx < BANK_DATA_WIDTH/8; byte_idx++) begin
            mask[byte_idx*8 +: 8] = strb[byte_idx] ? 8'h00 : 8'hFF;
        end
        return mask;
    endfunction

    function automatic logic [GROUP_IDX_W-1:0] noc_read_group_get(input int unsigned port_idx);
        return noc_read_group_reg[port_idx*GROUP_IDX_W +: GROUP_IDX_W];
    endfunction

    function automatic logic decode_linear_bank(
        input  logic [ADDR_WIDTH-1:0] laddr,
        output logic [BANK_IDX_W-1:0] bank_idx,
        output bank_row_t row_idx
    );
        logic [31:0] bank_sel;
        bank_sel = laddr / BANK_DEPTH;
        row_idx  = laddr % BANK_DEPTH;
        bank_idx = bank_sel[BANK_IDX_W-1:0];
        decode_linear_bank = (bank_sel < BANKS_PER_GROUP);
    endfunction

    // ---------------------------------------------------------------------
    // Combinational readiness for NoC and DMA ingress.
    // Static priority: lower port index has higher priority.
    // ---------------------------------------------------------------------
    always_comb begin
        logic bank_claimed [TOTAL_BANKS];

        for (int unsigned b = 0; b < TOTAL_BANKS; b++) begin
            bank_claimed[b] = 1'b0;
            bank_ceb_w[b]       = 1'b1;
            bank_web_w[b]       = 1'b1;
            bank_addr_w[b]      = '0;
            bank_d_w[b]         = '0;
            bank_bweb_w[b]      = {BANK_DATA_WIDTH{1'b1}};
        end

        dma_write_issue_w    = 1'b0;
        dma_write_bank_idx_w = '0;
        dma_write_row_w      = '0;
        dma_read_issue_w     = 1'b0;
        dma_read_bank_idx_w  = '0;
        dma_read_row_w       = '0;

        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            logic [GROUP_IDX_W-1:0] group_idx;
            logic [31:0] laddr;
            logic is_parallel;
            logic can_accept;
            int unsigned bank_base;

            noc_accept_w[p]   = 1'b0;
            noc_parallel_w[p] = 1'b0;
            noc_group_w[p]    = '0;
            noc_bank_idx_w[p] = '0;
            noc_row_w[p]      = '0;
            spm_req_ready_o[p]= 1'b0;

            if (resp_valid_reg[p] || noc_read_pending_reg[p]) begin
                continue;
            end

            group_idx = active_map_init_done_reg ? active_map_reg[p] : p[GROUP_IDX_W-1:0];
            bank_base = group_idx * BANKS_PER_GROUP;
            can_accept = 1'b1;

            if (spm_req_valid_i[p]) begin
                laddr = spm_req_i[p].addr;
                is_parallel = (laddr >= GROUP_LINEAR_WORDS);
                noc_group_w[p] = group_idx;
                if (group_idx >= NUM_GROUPS) begin
                    can_accept = 1'b0;
                end else if (is_parallel) begin
                    logic [31:0] row;
                    row = laddr - GROUP_LINEAR_WORDS;
                    noc_parallel_w[p] = 1'b1;
                    noc_row_w[p]      = bank_row_t'(row);
                    if (row >= BANK_DEPTH) begin
                        can_accept = 1'b0;
                    end else begin
                        for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                            if (bank_claimed[bank_base + k]) begin
                                can_accept = 1'b0;
                            end
                        end
                        if (can_accept) begin
                            for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                                bank_claimed[bank_base + k] = 1'b1;
                            end
                        end
                    end
                end else begin
                    logic [BANK_IDX_W-1:0] bank_sel;
                    bank_row_t row_sel;
                    if (!decode_linear_bank(laddr, bank_sel, row_sel)) begin
                        can_accept = 1'b0;
                    end else if (bank_claimed[bank_base + bank_sel]) begin
                        can_accept = 1'b0;
                    end else begin
                        bank_claimed[bank_base + bank_sel] = 1'b1;
                        noc_bank_idx_w[p] = $bits(noc_bank_idx_w[p])'(bank_base + bank_sel);
                        noc_row_w[p]      = row_sel;
                    end
                end
            end

            spm_req_ready_o[p] = can_accept;
            noc_accept_w[p]    = can_accept && spm_req_valid_i[p];
        end

        if (aw_pending_valid_reg && w_pending_valid_reg && !bvalid_reg) begin
            logic [31:0] gwaddr;
            int unsigned grp;
            int unsigned lidx;
            int unsigned bank_sel;
            int unsigned bank_idx;
            int unsigned row_sel;

            gwaddr   = aw_pending_addr_reg / BYTES_PER_BANK;
            grp      = gwaddr / GROUP_SPAN_WORDS;
            lidx     = gwaddr % GROUP_SPAN_WORDS;
            bank_sel = lidx / BANK_DEPTH;
            row_sel  = lidx % BANK_DEPTH;
            bank_idx = grp * BANKS_PER_GROUP + bank_sel;

            if ((grp < NUM_GROUPS) && (lidx < GROUP_LINEAR_WORDS) && (bank_idx < TOTAL_BANKS) && !bank_claimed[bank_idx]) begin
                dma_write_issue_w    = 1'b1;
                dma_write_bank_idx_w = bank_idx[BANK_IDX_W-1:0];
                dma_write_row_w      = bank_row_t'(row_sel);
                bank_claimed[bank_idx] = 1'b1;
            end
        end

        if (ar_pending_valid_reg && !rvalid_reg && !dma_read_pending_reg) begin
            logic [31:0] gwaddr;
            int unsigned grp;
            int unsigned lidx;
            int unsigned bank_sel;
            int unsigned bank_idx;
            int unsigned row_sel;

            gwaddr   = ar_pending_addr_reg / BYTES_PER_BANK;
            grp      = gwaddr / GROUP_SPAN_WORDS;
            lidx     = gwaddr % GROUP_SPAN_WORDS;
            bank_sel = lidx / BANK_DEPTH;
            row_sel  = lidx % BANK_DEPTH;
            bank_idx = grp * BANKS_PER_GROUP + bank_sel;

            if ((grp < NUM_GROUPS) && (lidx < GROUP_LINEAR_WORDS) && (bank_idx < TOTAL_BANKS) && !bank_claimed[bank_idx]) begin
                dma_read_issue_w    = 1'b1;
                dma_read_bank_idx_w = bank_idx[BANK_IDX_W-1:0];
                dma_read_row_w      = bank_row_t'(row_sel);
                bank_claimed[bank_idx] = 1'b1;
            end
        end

        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            if (noc_accept_w[p]) begin
                if (noc_parallel_w[p]) begin
                    for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                        logic [BANK_IDX_W-1:0] bank_idx;

                        bank_idx = $bits(bank_idx)'(noc_group_w[p] * BANKS_PER_GROUP + k);
                        bank_ceb_w[bank_idx]  = 1'b0;
                        bank_web_w[bank_idx]  = !spm_req_i[p].wen;
                        bank_addr_w[bank_idx] = noc_row_w[p];
                        if (spm_req_i[p].wen) begin
                            bank_bweb_w[bank_idx] = '0;
                            bank_d_w[bank_idx]    = spm_req_i[p].wdata[k*BANK_DATA_WIDTH +: BANK_DATA_WIDTH];
                        end
                    end
                end else begin
                    bank_ceb_w[noc_bank_idx_w[p]]  = 1'b0;
                    bank_web_w[noc_bank_idx_w[p]]  = !spm_req_i[p].wen;
                    bank_addr_w[noc_bank_idx_w[p]] = noc_row_w[p];
                    if (spm_req_i[p].wen) begin
                        bank_bweb_w[noc_bank_idx_w[p]] = '0;
                        bank_d_w[noc_bank_idx_w[p]]    = spm_req_i[p].wdata[BANK_DATA_WIDTH-1:0];
                    end
                end
            end
        end

        if (dma_write_issue_w) begin
            bank_ceb_w[dma_write_bank_idx_w]   = 1'b0;
            bank_web_w[dma_write_bank_idx_w]   = 1'b0;
            bank_addr_w[dma_write_bank_idx_w]  = dma_write_row_w;
            bank_bweb_w[dma_write_bank_idx_w]  = expand_strb_to_bweb(w_pending_strb_reg);
            bank_d_w[dma_write_bank_idx_w]     = w_pending_data_reg;
        end

        if (dma_read_issue_w) begin
            bank_ceb_w[dma_read_bank_idx_w]  = 1'b0;
            bank_web_w[dma_read_bank_idx_w]  = 1'b1;
            bank_addr_w[dma_read_bank_idx_w] = dma_read_row_w;
        end

        s_axi_awready_o = !aw_pending_valid_reg;
        s_axi_wready_o  = !w_pending_valid_reg;
        s_axi_arready_o = !ar_pending_valid_reg && !dma_read_pending_reg && !rvalid_reg;

        s_axi_bvalid_o  = bvalid_reg;
        s_axi_bresp_o   = bresp_reg;
        s_axi_rvalid_o  = rvalid_reg;
        s_axi_rdata_o   = rdata_reg;
        s_axi_rresp_o   = rresp_reg;

        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            spm_resp_valid_o[p] = resp_valid_reg[p];
            spm_resp_o[p]       = resp_data_reg[p];
        end

        pmu_cycle_cnt_o        = pmu_cycle_cnt_reg;
        pmu_arb_stall_cnt_o    = pmu_arb_stall_cnt_reg;
        pmu_credit_stall_cnt_o = pmu_credit_stall_cnt_reg;
        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            pmu_port_txn_cnt_o[p] = pmu_port_txn_cnt_reg[p];
        end

    end

    // ---------------------------------------------------------------------
    // Main sequential behavior.
    // ---------------------------------------------------------------------
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                active_map_reg[p]      <= '0;
                resp_valid_reg[p]      <= 1'b0;
                resp_data_reg[p]       <= '0;
                noc_read_pending_reg[p]  <= 1'b0;
                noc_read_parallel_reg[p] <= 1'b0;
                noc_read_group_reg[p*GROUP_IDX_W +: GROUP_IDX_W] <= '0;
                noc_read_bank_reg[p]     <= '0;
                noc_read_row_reg[p]      <= '0;
                pmu_port_txn_cnt_reg[p]<= 64'd0;
            end
            aw_pending_valid_reg      <= 1'b0;
            aw_pending_addr_reg       <= '0;
            w_pending_valid_reg       <= 1'b0;
            w_pending_data_reg        <= '0;
            w_pending_strb_reg        <= '0;
            ar_pending_valid_reg      <= 1'b0;
            ar_pending_addr_reg       <= '0;
            bvalid_reg                <= 1'b0;
            bresp_reg                 <= 2'b00;
            rvalid_reg                <= 1'b0;
            rdata_reg                 <= '0;
            rresp_reg                 <= 2'b00;
            dma_read_pending_reg      <= 1'b0;
            dma_read_bank_reg         <= '0;
            dma_read_row_reg          <= '0;
            pmu_cycle_cnt_reg         <= 64'd0;
            pmu_arb_stall_cnt_reg     <= 64'd0;
            pmu_credit_stall_cnt_reg  <= 64'd0;
            active_map_init_done_reg  <= 1'b0;
        end else begin
            pmu_cycle_cnt_reg <= pmu_cycle_cnt_reg + 64'd1;

            if (pmu_rst_i) begin
                pmu_cycle_cnt_reg        <= 64'd0;
                pmu_arb_stall_cnt_reg    <= 64'd0;
                pmu_credit_stall_cnt_reg <= 64'd0;
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    pmu_port_txn_cnt_reg[p] <= 64'd0;
                end
            end

            if (!active_map_init_done_reg) begin
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    active_map_reg[p] <= p[GROUP_IDX_W-1:0];
                end
                active_map_init_done_reg <= 1'b1;
            end else if (config_update_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO")) begin
                    $display("[%0t] [TRACE][SPM] config_update map=0x%02x arb_policy=%0b",
                             $time,
                             config_map_i,
                             arb_policy_i);
                end
                // synopsys translate_on
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    active_map_reg[p] <= config_map_i[p*2 +: 2];
                end
            end

            if (soft_reset_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                    $display("[%0t] [TRACE][SPM] soft_reset", $time);
                end
                // synopsys translate_on
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    resp_valid_reg[p] <= 1'b0;
                    resp_data_reg[p]  <= '0;
                    noc_read_pending_reg[p]  <= 1'b0;
                    noc_read_parallel_reg[p] <= 1'b0;
                    noc_read_group_reg[p*GROUP_IDX_W +: GROUP_IDX_W] <= '0;
                    noc_read_bank_reg[p]     <= '0;
                    noc_read_row_reg[p]      <= '0;
                end
                aw_pending_valid_reg <= 1'b0;
                w_pending_valid_reg  <= 1'b0;
                ar_pending_valid_reg <= 1'b0;
                bvalid_reg           <= 1'b0;
                rvalid_reg           <= 1'b0;
                dma_read_pending_reg <= 1'b0;
            end else begin
                // synopsys translate_off
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && s_axi_awvalid_i && s_axi_awready_o) begin
                    $display("[%0t] [TRACE][SPM][AXI] aw addr=0x%08x",
                             $time,
                             s_axi_awaddr_i);
                end
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && s_axi_wvalid_i && s_axi_wready_o) begin
                    $display("[%0t] [TRACE][SPM][AXI] w data=0x%016x strb=0x%02x",
                             $time,
                             s_axi_wdata_i,
                             s_axi_wstrb_i);
                end
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && s_axi_arvalid_i && s_axi_arready_o) begin
                    $display("[%0t] [TRACE][SPM][AXI] ar addr=0x%08x",
                             $time,
                             s_axi_araddr_i);
                end
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && dma_write_issue_w) begin
                    $display("[%0t] [TRACE][SPM][AXI] write_issue bank=%0d row=%0d data=0x%016x strb=0x%02x",
                             $time,
                             dma_write_bank_idx_w,
                             dma_write_row_w,
                             w_pending_data_reg,
                             w_pending_strb_reg);
                end
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && dma_read_issue_w) begin
                    $display("[%0t] [TRACE][SPM][AXI] read_issue bank=%0d row=%0d",
                             $time,
                             dma_read_bank_idx_w,
                             dma_read_row_w);
                end
                // synopsys translate_on

                // Response retirement
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    if (resp_valid_reg[p] && spm_resp_ready_i[p]) begin
                        resp_valid_reg[p] <= 1'b0;
                    end
                    if (resp_valid_reg[p] && !spm_resp_ready_i[p]) begin
                        pmu_credit_stall_cnt_reg <= pmu_credit_stall_cnt_reg + 64'd1;
                    end
                end
                if (bvalid_reg && s_axi_bready_i) begin
                    bvalid_reg <= 1'b0;
                end
                if (rvalid_reg && s_axi_rready_i) begin
                    rvalid_reg <= 1'b0;
                end

                // Complete outstanding SRAM reads.
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    if (noc_read_pending_reg[p]) begin
                        // synopsys translate_off
                        if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                            $display("[%0t] [TRACE][SPM][P%0d] read_complete parallel=%0b group=%0d bank=%0d row=%0d data_lo=0x%016x",
                                     $time,
                                     p,
                                     noc_read_parallel_reg[p],
                                     noc_read_group_get(p),
                                     noc_read_bank_reg[p],
                                     noc_read_row_reg[p],
                                     bank_q_w[noc_read_parallel_reg[p] ? (noc_read_group_get(p) * BANKS_PER_GROUP) : noc_read_bank_reg[p]]);
                        end
                        // synopsys translate_on

                        resp_valid_reg[p]     <= 1'b1;
                        resp_data_reg[p].rdata<= '0;
                        resp_data_reg[p].code <= SPM_OK;
                        if (noc_read_parallel_reg[p]) begin
                            for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                                logic [BANK_IDX_W-1:0] bank_idx;

                                bank_idx = $bits(bank_idx)'(noc_read_group_get(p) * BANKS_PER_GROUP + k);
                                resp_data_reg[p].rdata[k*BANK_DATA_WIDTH +: BANK_DATA_WIDTH] <= bank_q_w[bank_idx];
                            end
                        end else begin
                            resp_data_reg[p].rdata[BANK_DATA_WIDTH-1:0] <= bank_q_w[noc_read_bank_reg[p]];
                        end
                        noc_read_pending_reg[p]  <= 1'b0;
                        noc_read_parallel_reg[p] <= 1'b0;
                        noc_read_group_reg[p*GROUP_IDX_W +: GROUP_IDX_W] <= '0;
                        noc_read_bank_reg[p]     <= '0;
                        noc_read_row_reg[p]      <= '0;
                    end
                end

                if (dma_read_pending_reg) begin
                    rvalid_reg           <= 1'b1;
                    rresp_reg            <= 2'b00;
                    rdata_reg            <= bank_q_w[dma_read_bank_reg];
                    dma_read_pending_reg <= 1'b0;
                end

                // DMA ingress capture
                if (s_axi_awvalid_i && s_axi_awready_o) begin
                    aw_pending_valid_reg <= 1'b1;
                    aw_pending_addr_reg  <= s_axi_awaddr_i;
                end
                if (s_axi_wvalid_i && s_axi_wready_o) begin
                    w_pending_valid_reg  <= 1'b1;
                    w_pending_data_reg   <= s_axi_wdata_i;
                    w_pending_strb_reg   <= s_axi_wstrb_i;
                end
                if (s_axi_arvalid_i && s_axi_arready_o) begin
                    ar_pending_valid_reg <= 1'b1;
                    ar_pending_addr_reg  <= s_axi_araddr_i;
                end

                // NoC request execution (higher priority than DMA)
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    if (noc_accept_w[p]) begin
                        logic [GROUP_IDX_W-1:0] group_idx;
                        logic [31:0] laddr;
                        int unsigned bank_base;
                        group_idx = noc_group_w[p];
                        laddr     = spm_req_i[p].addr;
                        bank_base = group_idx * BANKS_PER_GROUP;

                        // synopsys translate_off
                        if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                            $display("[%0t] [TRACE][SPM][P%0d] accept wen=%0b addr=0x%08x parallel=%0b group=%0d bank=%0d row=%0d data_lo=0x%016x",
                                     $time,
                                     p,
                                     spm_req_i[p].wen,
                                     laddr,
                                     noc_parallel_w[p],
                                     noc_group_w[p],
                                     noc_bank_idx_w[p],
                                     noc_row_w[p],
                                     spm_req_i[p].wdata[63:0]);
                        end
                        // synopsys translate_on

                        if (laddr >= GROUP_LINEAR_WORDS) begin
                            if (spm_req_i[p].wen) begin
                                resp_valid_reg[p] <= 1'b1;
                                resp_data_reg[p].rdata <= '0;
                                resp_data_reg[p].code  <= SPM_OK;
                            end else begin
                                noc_read_pending_reg[p]  <= 1'b1;
                                noc_read_parallel_reg[p] <= 1'b1;
                                noc_read_group_reg[p*GROUP_IDX_W +: GROUP_IDX_W] <= noc_group_w[p];
                                noc_read_row_reg[p]      <= noc_row_w[p];
                            end
                        end else begin
                            if (spm_req_i[p].wen) begin
                                resp_valid_reg[p] <= 1'b1;
                                resp_data_reg[p].rdata <= '0;
                                resp_data_reg[p].code  <= SPM_OK;
                            end else begin
                                noc_read_pending_reg[p]  <= 1'b1;
                                noc_read_parallel_reg[p] <= 1'b0;
                                noc_read_bank_reg[p]     <= noc_bank_idx_w[p];
                                noc_read_row_reg[p]      <= noc_row_w[p];
                            end
                        end
                        pmu_port_txn_cnt_reg[p] <= pmu_port_txn_cnt_reg[p] + 64'd1;
                    end else if (spm_req_valid_i[p] && !spm_req_ready_o[p]) begin
                        pmu_arb_stall_cnt_reg <= pmu_arb_stall_cnt_reg + 64'd1;
                    end
                end

                // DMA write issue (after NoC)
                if (aw_pending_valid_reg && w_pending_valid_reg && !bvalid_reg) begin
                    logic [31:0] gwaddr;
                    int unsigned grp;
                    int unsigned lidx;
                    int unsigned bank_sel;
                    int unsigned bank_idx;
                    int unsigned row_sel;

                    gwaddr   = aw_pending_addr_reg / BYTES_PER_BANK;
                    grp      = gwaddr / GROUP_SPAN_WORDS;
                    lidx     = gwaddr % GROUP_SPAN_WORDS;
                    bank_sel = lidx / BANK_DEPTH;
                    row_sel  = lidx % BANK_DEPTH;
                    bank_idx = grp * BANKS_PER_GROUP + bank_sel;

                    if ((grp >= NUM_GROUPS) || (lidx >= GROUP_LINEAR_WORDS) || (bank_idx >= TOTAL_BANKS)) begin
                        bvalid_reg           <= 1'b1;
                        bresp_reg            <= 2'b10;
                        aw_pending_valid_reg <= 1'b0;
                        w_pending_valid_reg  <= 1'b0;
                    end else if (dma_write_issue_w) begin
                        bvalid_reg            <= 1'b1;
                        bresp_reg             <= 2'b00;
                        aw_pending_valid_reg  <= 1'b0;
                        w_pending_valid_reg   <= 1'b0;
                    end
                end

                // DMA read issue (after NoC and DMA write)
                if (ar_pending_valid_reg && !rvalid_reg) begin
                    logic [31:0] gwaddr;
                    int unsigned grp;
                    int unsigned lidx;
                    int unsigned bank_sel;
                    int unsigned bank_idx;
                    int unsigned row_sel;

                    gwaddr   = ar_pending_addr_reg / BYTES_PER_BANK;
                    grp      = gwaddr / GROUP_SPAN_WORDS;
                    lidx     = gwaddr % GROUP_SPAN_WORDS;
                    bank_sel = lidx / BANK_DEPTH;
                    row_sel  = lidx % BANK_DEPTH;
                    bank_idx = grp * BANKS_PER_GROUP + bank_sel;

                    if ((grp >= NUM_GROUPS) || (lidx >= GROUP_LINEAR_WORDS) || (bank_idx >= TOTAL_BANKS)) begin
                        rvalid_reg           <= 1'b1;
                        rresp_reg            <= 2'b10;
                        rdata_reg            <= '0;
                        ar_pending_valid_reg <= 1'b0;
                    end else if (dma_read_issue_w) begin
                        dma_read_pending_reg <= 1'b1;
                        dma_read_bank_reg    <= dma_read_bank_idx_w;
                        dma_read_row_reg     <= dma_read_row_w;
                        ar_pending_valid_reg <= 1'b0;
                    end
                end
            end
        end
    end

    generate
        for (genvar bank = 0; bank < TOTAL_BANKS_GEN; bank++) begin : gen_spm_bank
            ScratchpadMemoryBank #(
                .BANK_DATA_WIDTH(BANK_DATA_WIDTH),
                .BANK_DEPTH(BANK_DEPTH),
                .MACRO_DEPTH(MACRO_DEPTH),
                .BANK_ROW_W(BANK_ROW_W)
            ) u_bank (
                .SLP    (1'b0),
                .DSLP   (1'b0),
                .SD     (1'b0),
                .PUDELAY(bank_pudelay_unused_w[bank]),
                .CLK    (clk),
                .CEB    (bank_ceb_w[bank]),
                .WEB    (bank_web_w[bank]),
                .A      (bank_addr_w[bank]),
                .D      (bank_d_w[bank]),
                .BWEB   (bank_bweb_w[bank]),
                .RTSEL  (2'b01),
                .WTSEL  (2'b01),
                .Q      (bank_q_w[bank])
            );
        end
    endgenerate

    // synopsys translate_off
    initial begin
        if ((NUM_NOC_PORTS != 4) || (BANKS_PER_GROUP != 3) ||
            (BANK_DATA_WIDTH != 64) || (ADDR_WIDTH != 32) ||
            (NOC_DATA_WIDTH != CLUSTER_DATA_WIDTH)) begin
            $error("ScratchpadMemory baseline currently supports only NUM_NOC_PORTS=4, BANKS_PER_GROUP=3, BANK_DATA_WIDTH=64, ADDR_WIDTH=32, NOC_DATA_WIDTH=192");
        end
        if ((BANK_DEPTH % MACRO_DEPTH) != 0) begin
            $error("ScratchpadMemory requires BANK_DEPTH to be a multiple of %0d", MACRO_DEPTH);
        end
        if ((SRAM_BANK_LATENCY != 1) || (SRAM_BANK_PIPELINE_DEPTH != 1)) begin
            $error("ScratchpadMemory hardmacro path currently supports only SRAM_BANK_LATENCY=1 and SRAM_BANK_PIPELINE_DEPTH=1");
        end
    end
    // synopsys translate_on

endmodule