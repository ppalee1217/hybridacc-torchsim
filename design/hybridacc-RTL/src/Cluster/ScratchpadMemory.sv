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
    parameter int unsigned ADDR_WIDTH                = 32,
    parameter int unsigned AXI_QUEUE_DEPTH           = 16
) (
    input  logic                        clk,
    input  logic                        reset_n,
    input  logic                        pmu_rst_i,
    input  logic                        soft_reset_i,
    input  logic [7:0]                  config_map_i,
    input  logic                        config_update_i,

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
    localparam int unsigned BYTES_PER_BANK    = BANK_DATA_WIDTH / 8;
    localparam int unsigned GROUP_LINEAR_WORDS= BANKS_PER_GROUP * BANK_DEPTH;
    localparam int unsigned GROUP_SPAN_WORDS  = (BANKS_PER_GROUP + 1) * BANK_DEPTH;
    localparam int unsigned GROUP_IDX_W       = (NUM_GROUPS <= 1) ? 1 : $clog2(NUM_GROUPS);
    localparam int unsigned BANK_IDX_W        = (TOTAL_BANKS <= 1) ? 1 : $clog2(TOTAL_BANKS);
    localparam int unsigned BANK_ROW_W        = (BANK_DEPTH <= 1) ? 1 : $clog2(BANK_DEPTH);
    localparam int unsigned MACRO_DEPTH       = 128;
    localparam int          TOTAL_BANKS_GEN   = int'(TOTAL_BANKS);
    localparam int unsigned AXI_QUEUE_PTR_W   =
        (AXI_QUEUE_DEPTH <= 1) ? 1 : $clog2(AXI_QUEUE_DEPTH);
    localparam int unsigned AXI_QUEUE_CNT_W   =
        (AXI_QUEUE_DEPTH <= 1) ? 1 : $clog2(AXI_QUEUE_DEPTH + 1);

    typedef logic [BANK_DATA_WIDTH-1:0] bank_word_t;
    typedef logic [BANK_ROW_W-1:0]      bank_row_t;
    typedef logic [AXI_QUEUE_PTR_W-1:0] axi_queue_ptr_t;
    typedef logic [AXI_QUEUE_CNT_W-1:0] axi_queue_count_t;

    logic [GROUP_IDX_W-1:0] active_map_reg [NUM_NOC_PORTS];
    logic                   active_map_init_done_reg;

    logic                   resp_valid_reg [NUM_NOC_PORTS];
    spm_resp_192_t          resp_data_reg  [NUM_NOC_PORTS];
    logic                   noc_read_pending_reg  [NUM_NOC_PORTS];
    logic                   noc_read_parallel_reg [NUM_NOC_PORTS];
    logic [NUM_NOC_PORTS*GROUP_IDX_W-1:0] noc_read_group_reg;
    logic [BANK_IDX_W-1:0]  noc_read_bank_reg     [NUM_NOC_PORTS];

    logic [ADDR_WIDTH-1:0]  axi_aw_fifo_reg[0:AXI_QUEUE_DEPTH-1];
    logic [BANK_DATA_WIDTH-1:0]
                            axi_w_data_fifo_reg[0:AXI_QUEUE_DEPTH-1];
    logic [BANK_DATA_WIDTH/8-1:0]
                            axi_w_strb_fifo_reg[0:AXI_QUEUE_DEPTH-1];
    logic [ADDR_WIDTH-1:0]  axi_ar_fifo_reg[0:AXI_QUEUE_DEPTH-1];
    logic [1:0]             axi_b_resp_fifo_reg[0:AXI_QUEUE_DEPTH-1];
    logic [BANK_DATA_WIDTH-1:0]
                            axi_r_data_fifo_reg[0:AXI_QUEUE_DEPTH-1];
    logic [1:0]             axi_r_resp_fifo_reg[0:AXI_QUEUE_DEPTH-1];
    axi_queue_ptr_t         axi_aw_wr_ptr_reg;
    axi_queue_ptr_t         axi_aw_rd_ptr_reg;
    axi_queue_ptr_t         axi_w_wr_ptr_reg;
    axi_queue_ptr_t         axi_w_rd_ptr_reg;
    axi_queue_ptr_t         axi_ar_wr_ptr_reg;
    axi_queue_ptr_t         axi_ar_rd_ptr_reg;
    axi_queue_ptr_t         axi_b_wr_ptr_reg;
    axi_queue_ptr_t         axi_b_rd_ptr_reg;
    axi_queue_ptr_t         axi_r_wr_ptr_reg;
    axi_queue_ptr_t         axi_r_rd_ptr_reg;
    axi_queue_count_t       axi_aw_count_reg;
    axi_queue_count_t       axi_w_count_reg;
    axi_queue_count_t       axi_ar_count_reg;
    axi_queue_count_t       axi_b_count_reg;
    axi_queue_count_t       axi_r_count_reg;
    logic                   dma_read_pending_reg;
    logic [BANK_IDX_W-1:0]  dma_read_bank_reg;

    logic [63:0]            pmu_cycle_cnt_reg;
    logic [63:0]            pmu_port_txn_cnt_reg [NUM_NOC_PORTS];
    logic [63:0]            pmu_arb_stall_cnt_reg;
    logic [63:0]            pmu_credit_stall_cnt_reg;

    logic                   noc_accept_w   [NUM_NOC_PORTS];
    logic [NUM_NOC_PORTS-1:0] spm_req_ready_w;
    logic                   noc_parallel_w [NUM_NOC_PORTS];
    logic [GROUP_IDX_W-1:0] noc_group_w    [NUM_NOC_PORTS];
    logic [BANK_IDX_W-1:0]  noc_bank_idx_w [NUM_NOC_PORTS];
    bank_row_t              noc_row_w      [NUM_NOC_PORTS];
    logic                   s_axi_awready_w;
    logic                   s_axi_wready_w;
    logic                   s_axi_arready_w;
    logic                   s_axi_aw_push_w;
    logic                   s_axi_w_push_w;
    logic                   s_axi_ar_push_w;
    logic                   s_axi_b_pop_w;
    logic                   s_axi_r_pop_w;
    logic                   dma_write_head_valid_w;
    logic                   dma_write_decode_error_w;
    logic                   dma_write_issue_w;
    logic                   dma_write_complete_w;
    logic [BANK_IDX_W-1:0]  dma_write_bank_idx_w;
    bank_row_t              dma_write_row_w;
    logic                   dma_read_head_valid_w;
    logic                   dma_read_decode_error_w;
    logic                   dma_read_issue_w;
    logic                   dma_read_complete_error_w;
    logic                   dma_read_head_pop_w;
    logic                   dma_r_push_w;
    logic [BANK_IDX_W-1:0]  dma_read_bank_idx_w;
    bank_row_t              dma_read_row_w;

    logic                   bank_ceb_w            [TOTAL_BANKS];
    logic                   bank_web_w            [TOTAL_BANKS];
    bank_row_t              bank_addr_w           [TOTAL_BANKS];
    bank_word_t             bank_d_w              [TOTAL_BANKS];
    bank_word_t             bank_bweb_w           [TOTAL_BANKS];
    bank_word_t             bank_q_w              [TOTAL_BANKS];
    logic                   bank_pudelay_unused_w [TOTAL_BANKS];
    logic                   bank_pudelay_unused_reduce_w;
    logic                   bank_claimed_w        [TOTAL_BANKS];

    logic [31:0]            dma_write_gwaddr_w;
    int unsigned            dma_write_grp_w;
    int unsigned            dma_write_lidx_w;
    int unsigned            dma_write_bank_sel_w;
    int unsigned            dma_write_bank_idx_int_w;
    int unsigned            dma_write_row_sel_w;
    logic [31:0]            dma_read_gwaddr_w;
    int unsigned            dma_read_grp_w;
    int unsigned            dma_read_lidx_w;
    int unsigned            dma_read_bank_sel_w;
    int unsigned            dma_read_bank_idx_int_w;
    int unsigned            dma_read_row_sel_w;

    logic [GROUP_IDX_W-1:0] arb_group_idx_w;
    logic [31:0]            arb_laddr_w;
    logic                   arb_is_parallel_w;
    logic                   arb_can_accept_w;
    int unsigned            arb_bank_base_w;
    logic [31:0]            arb_row_w;
    logic [BANK_IDX_W-1:0]  arb_bank_sel_w;
    bank_row_t              arb_row_sel_w;
    logic [BANK_IDX_W-1:0]  arb_bank_idx_w;
    logic                   spm_decode_known_w;

    function automatic bank_word_t expand_strb_to_bweb(
        input logic [BANK_DATA_WIDTH/8-1:0] strb
    );
        bank_word_t mask;
        for (int unsigned byte_idx = 0; byte_idx < BANK_DATA_WIDTH/8; byte_idx++) begin
            mask[byte_idx*8 +: 8] = strb[byte_idx] ? 8'h00 : 8'hFF;
        end
        return mask;
    endfunction

    function automatic axi_queue_ptr_t axi_queue_ptr_inc(
        input axi_queue_ptr_t ptr
    );
        if (ptr == axi_queue_ptr_t'(AXI_QUEUE_DEPTH - 1)) begin
            return '0;
        end
        return ptr + 1'b1;
    endfunction

    function automatic logic [GROUP_IDX_W-1:0] noc_read_group_get(
        input int unsigned port_idx,
        input logic [NUM_NOC_PORTS*GROUP_IDX_W-1:0] group_reg
    );
        return group_reg[port_idx*GROUP_IDX_W +: GROUP_IDX_W];
    endfunction

    function automatic logic decode_linear_bank(
        input  logic [ADDR_WIDTH-1:0] laddr,
        output logic [BANK_IDX_W-1:0] bank_idx,
        output bank_row_t row_idx
    );
        logic [31:0] bank_sel;
        bank_sel = $bits(bank_sel)'(laddr / BANK_DEPTH);
        row_idx  = bank_row_t'(laddr % BANK_DEPTH);
        bank_idx = $bits(bank_idx)'(bank_sel);
        decode_linear_bank = (bank_sel < BANKS_PER_GROUP);
    endfunction

    always_comb begin
        // Keep the ready/accept guard input-facing only. Feeding internal
        // decode temporaries back into this check creates a self-referential
        // combinational loop in Jasper/Superlint.
        spm_decode_known_w = 1'b1;
        spm_decode_known_w &= (active_map_init_done_reg === active_map_init_done_reg);

        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            spm_decode_known_w &= (active_map_reg[p] === active_map_reg[p]);
            spm_decode_known_w &= (spm_req_valid_i[p] === spm_req_valid_i[p]);
            if (spm_req_valid_i[p]) begin
                spm_decode_known_w &= (spm_req_i[p].addr === spm_req_i[p].addr);
                spm_decode_known_w &= (spm_req_i[p].wen === spm_req_i[p].wen);
                if (spm_req_i[p].wen) begin
                    spm_decode_known_w &= (spm_req_i[p].wdata === spm_req_i[p].wdata);
                end
            end
        end

        spm_decode_known_w &= (s_axi_awvalid_i === s_axi_awvalid_i);
        if (s_axi_awvalid_i) begin
            spm_decode_known_w &= (s_axi_awaddr_i === s_axi_awaddr_i);
        end
        if (axi_aw_count_reg != 0) begin
            spm_decode_known_w &=
                (axi_aw_fifo_reg[axi_aw_rd_ptr_reg] ===
                 axi_aw_fifo_reg[axi_aw_rd_ptr_reg]);
        end

        spm_decode_known_w &= (s_axi_wvalid_i === s_axi_wvalid_i);
        if (s_axi_wvalid_i) begin
            spm_decode_known_w &= (s_axi_wdata_i === s_axi_wdata_i);
            spm_decode_known_w &= (s_axi_wstrb_i === s_axi_wstrb_i);
        end
        if (axi_w_count_reg != 0) begin
            spm_decode_known_w &=
                (axi_w_data_fifo_reg[axi_w_rd_ptr_reg] ===
                 axi_w_data_fifo_reg[axi_w_rd_ptr_reg]);
            spm_decode_known_w &=
                (axi_w_strb_fifo_reg[axi_w_rd_ptr_reg] ===
                 axi_w_strb_fifo_reg[axi_w_rd_ptr_reg]);
        end

        spm_decode_known_w &= (s_axi_arvalid_i === s_axi_arvalid_i);
        if (s_axi_arvalid_i) begin
            spm_decode_known_w &= (s_axi_araddr_i === s_axi_araddr_i);
        end
        if (axi_ar_count_reg != 0) begin
            spm_decode_known_w &=
                (axi_ar_fifo_reg[axi_ar_rd_ptr_reg] ===
                 axi_ar_fifo_reg[axi_ar_rd_ptr_reg]);
        end
    end

    // ---------------------------------------------------------------------
    // Combinational readiness for NoC and DMA ingress.
    // Static priority: lower port index has higher priority.
    // ---------------------------------------------------------------------
    always_comb begin
        for (int unsigned b = 0; b < TOTAL_BANKS; b++) begin
            bank_claimed_w[b] = 1'b0;
            bank_ceb_w[b]       = 1'b1;
            bank_web_w[b]       = 1'b1;
            bank_addr_w[b]      = '0;
            bank_d_w[b]         = '0;
            bank_bweb_w[b]      = {BANK_DATA_WIDTH{1'b1}};
        end
        bank_pudelay_unused_reduce_w = 1'b0;
        for (int unsigned b = 0; b < TOTAL_BANKS; b++) begin
            bank_pudelay_unused_reduce_w ^= bank_pudelay_unused_w[b];
        end

        dma_write_head_valid_w = 1'b0;
        dma_write_decode_error_w = 1'b0;
        dma_write_issue_w    = 1'b0;
        dma_write_complete_w = 1'b0;
        dma_write_bank_idx_w = '0;
        dma_write_row_w      = '0;
        dma_read_head_valid_w = 1'b0;
        dma_read_decode_error_w = 1'b0;
        dma_read_issue_w     = 1'b0;
        dma_read_complete_error_w = 1'b0;
        dma_read_head_pop_w  = 1'b0;
        dma_r_push_w         = 1'b0;
        dma_read_bank_idx_w  = '0;
        dma_read_row_w       = '0;
        dma_write_gwaddr_w   = '0;
        dma_write_grp_w      = 0;
        dma_write_lidx_w     = 0;
        dma_write_bank_sel_w = 0;
        dma_write_bank_idx_int_w = 0;
        dma_write_row_sel_w  = 0;
        dma_read_gwaddr_w    = '0;
        dma_read_grp_w       = 0;
        dma_read_lidx_w      = 0;
        dma_read_bank_sel_w  = 0;
        dma_read_bank_idx_int_w = 0;
        dma_read_row_sel_w   = 0;
        arb_group_idx_w      = '0;
        arb_laddr_w          = '0;
        arb_is_parallel_w    = 1'b0;
        arb_can_accept_w     = 1'b1;
        arb_bank_base_w      = 0;
        arb_row_w            = '0;
        arb_bank_sel_w       = '0;
        arb_row_sel_w        = '0;
        arb_bank_idx_w       = '0;

        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            arb_group_idx_w   = '0;
            arb_laddr_w       = '0;
            arb_is_parallel_w = 1'b0;
            arb_can_accept_w  = 1'b1;
            arb_bank_base_w   = 0;
            arb_row_w         = '0;
            arb_bank_sel_w    = '0;
            arb_row_sel_w     = '0;
            noc_accept_w[p]   = 1'b0;
            noc_parallel_w[p] = 1'b0;
            noc_group_w[p]    = '0;
            noc_bank_idx_w[p] = '0;
            noc_row_w[p]      = '0;
            spm_req_ready_w[p]= 1'b0;

            if (resp_valid_reg[p] || noc_read_pending_reg[p]) begin
                continue;
            end

            arb_group_idx_w = active_map_init_done_reg ? active_map_reg[p] : GROUP_IDX_W'(p);
            arb_bank_base_w = $unsigned(arb_group_idx_w) * BANKS_PER_GROUP;

            if (spm_req_valid_i[p]) begin
                arb_laddr_w = spm_req_i[p].addr;
                arb_is_parallel_w = (arb_laddr_w >= GROUP_LINEAR_WORDS);
                noc_group_w[p] = arb_group_idx_w;
                if (arb_group_idx_w >= NUM_GROUPS) begin
                    arb_can_accept_w = 1'b0;
                end else if (arb_is_parallel_w) begin
                    arb_row_w = arb_laddr_w - GROUP_LINEAR_WORDS;
                    noc_parallel_w[p] = 1'b1;
                    noc_row_w[p]      = bank_row_t'(arb_row_w);
                    if (arb_row_w >= BANK_DEPTH) begin
                        arb_can_accept_w = 1'b0;
                    end else begin
                        for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                            if (bank_claimed_w[arb_bank_base_w + k]) begin
                                arb_can_accept_w = 1'b0;
                            end
                        end
                        if (arb_can_accept_w) begin
                            for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                                bank_claimed_w[arb_bank_base_w + k] = 1'b1;
                            end
                        end
                    end
                end else begin
                    if (!decode_linear_bank(arb_laddr_w, arb_bank_sel_w, arb_row_sel_w)) begin
                        arb_can_accept_w = 1'b0;
                    end else if (bank_claimed_w[arb_bank_base_w + arb_bank_sel_w]) begin
                        arb_can_accept_w = 1'b0;
                    end else begin
                        bank_claimed_w[arb_bank_base_w + arb_bank_sel_w] = 1'b1;
                        noc_bank_idx_w[p] = $bits(noc_bank_idx_w[p])'(arb_bank_base_w + arb_bank_sel_w);
                        noc_row_w[p]      = arb_row_sel_w;
                    end
                end
            end

                spm_req_ready_w[p] = arb_can_accept_w && spm_decode_known_w
                    && (bank_pudelay_unused_reduce_w === bank_pudelay_unused_reduce_w);
                noc_accept_w[p]    = arb_can_accept_w && spm_decode_known_w && spm_req_valid_i[p];
        end

        s_axi_awready_w = (axi_aw_count_reg < AXI_QUEUE_DEPTH) &&
            spm_decode_known_w &&
            (bank_pudelay_unused_reduce_w === bank_pudelay_unused_reduce_w);
        s_axi_wready_w = (axi_w_count_reg < AXI_QUEUE_DEPTH) &&
            spm_decode_known_w &&
            (bank_pudelay_unused_reduce_w === bank_pudelay_unused_reduce_w);
        s_axi_arready_w = (axi_ar_count_reg < AXI_QUEUE_DEPTH) &&
            spm_decode_known_w &&
            (bank_pudelay_unused_reduce_w === bank_pudelay_unused_reduce_w);
        s_axi_aw_push_w = s_axi_awvalid_i && s_axi_awready_w;
        s_axi_w_push_w  = s_axi_wvalid_i && s_axi_wready_w;
        s_axi_ar_push_w = s_axi_arvalid_i && s_axi_arready_w;
        s_axi_b_pop_w =
            (axi_b_count_reg != 0) && s_axi_bready_i;
        s_axi_r_pop_w =
            (axi_r_count_reg != 0) && s_axi_rready_i;

        dma_write_head_valid_w =
            (axi_aw_count_reg != 0) &&
            (axi_w_count_reg != 0) &&
            ((axi_b_count_reg < AXI_QUEUE_DEPTH) || s_axi_b_pop_w);
        if (dma_write_head_valid_w) begin
            dma_write_gwaddr_w       =
                axi_aw_fifo_reg[axi_aw_rd_ptr_reg] / BYTES_PER_BANK;
            dma_write_grp_w          = dma_write_gwaddr_w / GROUP_SPAN_WORDS;
            dma_write_lidx_w         = dma_write_gwaddr_w % GROUP_SPAN_WORDS;
            dma_write_bank_sel_w     = dma_write_lidx_w / BANK_DEPTH;
            dma_write_row_sel_w      = dma_write_lidx_w % BANK_DEPTH;
            dma_write_bank_idx_int_w = $unsigned(dma_write_grp_w * BANKS_PER_GROUP + dma_write_bank_sel_w);

            dma_write_decode_error_w =
                (dma_write_grp_w >= NUM_GROUPS) ||
                (dma_write_lidx_w >= GROUP_LINEAR_WORDS) ||
                (dma_write_bank_idx_int_w >= TOTAL_BANKS);
            if (dma_write_decode_error_w) begin
                dma_write_complete_w = 1'b1;
            end else if (!bank_claimed_w[dma_write_bank_idx_int_w]) begin
                dma_write_issue_w    = 1'b1;
                dma_write_complete_w = 1'b1;
                dma_write_bank_idx_w = $bits(dma_write_bank_idx_w)'(dma_write_bank_idx_int_w);
                dma_write_row_w      = bank_row_t'(dma_write_row_sel_w);
                bank_claimed_w[dma_write_bank_idx_int_w] = 1'b1;
            end
        end

        dma_read_head_valid_w =
            (axi_ar_count_reg != 0) &&
            (({1'b0, axi_r_count_reg} + dma_read_pending_reg) <
             AXI_QUEUE_DEPTH || s_axi_r_pop_w);
        if (dma_read_head_valid_w) begin
            dma_read_gwaddr_w       =
                axi_ar_fifo_reg[axi_ar_rd_ptr_reg] / BYTES_PER_BANK;
            dma_read_grp_w          = dma_read_gwaddr_w / GROUP_SPAN_WORDS;
            dma_read_lidx_w         = dma_read_gwaddr_w % GROUP_SPAN_WORDS;
            dma_read_bank_sel_w     = dma_read_lidx_w / BANK_DEPTH;
            dma_read_row_sel_w      = dma_read_lidx_w % BANK_DEPTH;
            dma_read_bank_idx_int_w = $unsigned(dma_read_grp_w * BANKS_PER_GROUP + dma_read_bank_sel_w);

            dma_read_decode_error_w =
                (dma_read_grp_w >= NUM_GROUPS) ||
                (dma_read_lidx_w >= GROUP_LINEAR_WORDS) ||
                (dma_read_bank_idx_int_w >= TOTAL_BANKS);
            if (dma_read_decode_error_w && !dma_read_pending_reg) begin
                dma_read_complete_error_w = 1'b1;
                dma_read_head_pop_w = 1'b1;
            end else if (!dma_read_decode_error_w &&
                         !bank_claimed_w[dma_read_bank_idx_int_w]) begin
                dma_read_issue_w    = 1'b1;
                dma_read_head_pop_w = 1'b1;
                dma_read_bank_idx_w = $bits(dma_read_bank_idx_w)'(dma_read_bank_idx_int_w);
                dma_read_row_w      = bank_row_t'(dma_read_row_sel_w);
                bank_claimed_w[dma_read_bank_idx_int_w] = 1'b1;
            end
        end
        dma_r_push_w =
            dma_read_pending_reg || dma_read_complete_error_w;

        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            arb_bank_idx_w = '0;
            if (noc_accept_w[p]) begin
                if (noc_parallel_w[p]) begin
                    for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                        arb_bank_idx_w = $bits(arb_bank_idx_w)'(noc_group_w[p] * BANKS_PER_GROUP + k);
                        bank_ceb_w[arb_bank_idx_w]  = 1'b0;
                        bank_web_w[arb_bank_idx_w]  = !spm_req_i[p].wen;
                        bank_addr_w[arb_bank_idx_w] = noc_row_w[p];
                        if (spm_req_i[p].wen) begin
                            bank_bweb_w[arb_bank_idx_w] = '0;
                            bank_d_w[arb_bank_idx_w]    = spm_req_i[p].wdata[k*BANK_DATA_WIDTH +: BANK_DATA_WIDTH];
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
            bank_bweb_w[dma_write_bank_idx_w]  =
                expand_strb_to_bweb(axi_w_strb_fifo_reg[axi_w_rd_ptr_reg]);
            bank_d_w[dma_write_bank_idx_w]     =
                axi_w_data_fifo_reg[axi_w_rd_ptr_reg];
        end

        if (dma_read_issue_w) begin
            bank_ceb_w[dma_read_bank_idx_w]  = 1'b0;
            bank_web_w[dma_read_bank_idx_w]  = 1'b1;
            bank_addr_w[dma_read_bank_idx_w] = dma_read_row_w;
        end

        for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
            spm_req_ready_o[p] = spm_req_ready_w[p];
        end

        if (!spm_decode_known_w) begin
            dma_write_issue_w          = 1'b0;
            dma_write_complete_w       = 1'b0;
            dma_read_issue_w           = 1'b0;
            dma_read_complete_error_w  = 1'b0;
            dma_read_head_pop_w        = 1'b0;
            dma_r_push_w               = dma_read_pending_reg;
        end

        s_axi_awready_o = s_axi_awready_w;
        s_axi_wready_o  = s_axi_wready_w;
        s_axi_arready_o = s_axi_arready_w;

        s_axi_bvalid_o  = (axi_b_count_reg != 0);
        s_axi_bresp_o   =
            (axi_b_count_reg != 0) ?
            axi_b_resp_fifo_reg[axi_b_rd_ptr_reg] : 2'b00;
        s_axi_rvalid_o  = (axi_r_count_reg != 0);
        s_axi_rdata_o   =
            (axi_r_count_reg != 0) ?
            axi_r_data_fifo_reg[axi_r_rd_ptr_reg] : '0;
        s_axi_rresp_o   =
            (axi_r_count_reg != 0) ?
            axi_r_resp_fifo_reg[axi_r_rd_ptr_reg] : 2'b00;

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
                pmu_port_txn_cnt_reg[p]<= 64'd0;
            end
            axi_aw_wr_ptr_reg         <= '0;
            axi_aw_rd_ptr_reg         <= '0;
            axi_w_wr_ptr_reg          <= '0;
            axi_w_rd_ptr_reg          <= '0;
            axi_ar_wr_ptr_reg         <= '0;
            axi_ar_rd_ptr_reg         <= '0;
            axi_b_wr_ptr_reg          <= '0;
            axi_b_rd_ptr_reg          <= '0;
            axi_r_wr_ptr_reg          <= '0;
            axi_r_rd_ptr_reg          <= '0;
            axi_aw_count_reg          <= '0;
            axi_w_count_reg           <= '0;
            axi_ar_count_reg          <= '0;
            axi_b_count_reg           <= '0;
            axi_r_count_reg           <= '0;
            dma_read_pending_reg      <= 1'b0;
            dma_read_bank_reg         <= '0;
            pmu_cycle_cnt_reg         <= 64'd0;
            pmu_arb_stall_cnt_reg     <= 64'd0;
            pmu_credit_stall_cnt_reg  <= 64'd0;
            active_map_init_done_reg  <= 1'b0;
        end else begin
            pmu_cycle_cnt_reg <= $bits(pmu_cycle_cnt_reg)'(pmu_cycle_cnt_reg + 64'd1);

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
                    active_map_reg[p] <= GROUP_IDX_W'(p);
                end
                active_map_init_done_reg <= 1'b1;
            end else if (config_update_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO")) begin
                    $display("[%0t] [TRACE][SPM] config_update map=0x%02x",
                             $time,
                             config_map_i);
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
                end
                axi_aw_wr_ptr_reg    <= '0;
                axi_aw_rd_ptr_reg    <= '0;
                axi_w_wr_ptr_reg     <= '0;
                axi_w_rd_ptr_reg     <= '0;
                axi_ar_wr_ptr_reg    <= '0;
                axi_ar_rd_ptr_reg    <= '0;
                axi_b_wr_ptr_reg     <= '0;
                axi_b_rd_ptr_reg     <= '0;
                axi_r_wr_ptr_reg     <= '0;
                axi_r_rd_ptr_reg     <= '0;
                axi_aw_count_reg     <= '0;
                axi_w_count_reg      <= '0;
                axi_ar_count_reg     <= '0;
                axi_b_count_reg      <= '0;
                axi_r_count_reg      <= '0;
                dma_read_pending_reg <= 1'b0;
            end else begin
                // synopsys translate_off
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && s_axi_awvalid_i && s_axi_awready_w) begin
                    $display("[%0t] [TRACE][SPM][AXI] aw addr=0x%08x",
                             $time,
                             s_axi_awaddr_i);
                end
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && s_axi_wvalid_i && s_axi_wready_w) begin
                    $display("[%0t] [TRACE][SPM][AXI] w data=0x%016x strb=0x%02x",
                             $time,
                             s_axi_wdata_i,
                             s_axi_wstrb_i);
                end
                if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                    && s_axi_arvalid_i && s_axi_arready_w) begin
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
                             axi_w_data_fifo_reg[axi_w_rd_ptr_reg],
                             axi_w_strb_fifo_reg[axi_w_rd_ptr_reg]);
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
                        pmu_credit_stall_cnt_reg <= $bits(pmu_credit_stall_cnt_reg)'(pmu_credit_stall_cnt_reg + 64'd1);
                    end
                end
                if (s_axi_b_pop_w) begin
                    axi_b_rd_ptr_reg <= axi_queue_ptr_inc(axi_b_rd_ptr_reg);
                end
                if (s_axi_r_pop_w) begin
                    axi_r_rd_ptr_reg <= axi_queue_ptr_inc(axi_r_rd_ptr_reg);
                end

                // Complete outstanding SRAM reads.
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    if (noc_read_pending_reg[p]) begin
                        // synopsys translate_off
                        if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                            $display("[%0t] [TRACE][SPM][P%0d] read_complete parallel=%0b group=%0d bank=%0d data_lo=0x%016x",
                                     $time,
                                     p,
                                     noc_read_parallel_reg[p],
                                     noc_read_group_get(p, noc_read_group_reg),
                                     noc_read_bank_reg[p],
                                     bank_q_w[noc_read_parallel_reg[p] ? (noc_read_group_get(p, noc_read_group_reg) * BANKS_PER_GROUP) : noc_read_bank_reg[p]]);
                        end
                        // synopsys translate_on

                        resp_valid_reg[p]     <= 1'b1;
                        resp_data_reg[p].rdata<= '0;
                        resp_data_reg[p].code <= SPM_OK;
                        if (noc_read_parallel_reg[p]) begin
                            for (int unsigned k = 0; k < BANKS_PER_GROUP; k++) begin
                                logic [BANK_IDX_W-1:0] bank_idx;

                                bank_idx = $bits(bank_idx)'(noc_read_group_get(p, noc_read_group_reg) * BANKS_PER_GROUP + k);
                                resp_data_reg[p].rdata[k*BANK_DATA_WIDTH +: BANK_DATA_WIDTH] <= bank_q_w[bank_idx];
                            end
                        end else begin
                            resp_data_reg[p].rdata[BANK_DATA_WIDTH-1:0] <= bank_q_w[noc_read_bank_reg[p]];
                        end
                        noc_read_pending_reg[p]  <= 1'b0;
                        noc_read_parallel_reg[p] <= 1'b0;
                        noc_read_group_reg[p*GROUP_IDX_W +: GROUP_IDX_W] <= '0;
                        noc_read_bank_reg[p]     <= '0;
                    end
                end

                if (dma_r_push_w) begin
                    axi_r_data_fifo_reg[axi_r_wr_ptr_reg] <=
                        dma_read_pending_reg ?
                        bank_q_w[dma_read_bank_reg] : '0;
                    axi_r_resp_fifo_reg[axi_r_wr_ptr_reg] <=
                        dma_read_pending_reg ? 2'b00 : 2'b10;
                    axi_r_wr_ptr_reg <= axi_queue_ptr_inc(axi_r_wr_ptr_reg);
                end
                if (dma_read_pending_reg) begin
                    dma_read_pending_reg <= 1'b0;
                end

                // DMA ingress capture
                if (s_axi_aw_push_w) begin
                    axi_aw_fifo_reg[axi_aw_wr_ptr_reg] <= s_axi_awaddr_i;
                    axi_aw_wr_ptr_reg <= axi_queue_ptr_inc(axi_aw_wr_ptr_reg);
                end
                if (s_axi_w_push_w) begin
                    axi_w_data_fifo_reg[axi_w_wr_ptr_reg] <= s_axi_wdata_i;
                    axi_w_strb_fifo_reg[axi_w_wr_ptr_reg] <= s_axi_wstrb_i;
                    axi_w_wr_ptr_reg <= axi_queue_ptr_inc(axi_w_wr_ptr_reg);
                end
                if (s_axi_ar_push_w) begin
                    axi_ar_fifo_reg[axi_ar_wr_ptr_reg] <= s_axi_araddr_i;
                    axi_ar_wr_ptr_reg <= axi_queue_ptr_inc(axi_ar_wr_ptr_reg);
                end

                // NoC request execution (higher priority than DMA)
                for (int unsigned p = 0; p < NUM_NOC_PORTS; p++) begin
                    if (noc_accept_w[p]) begin
                        logic [31:0] laddr;
                        laddr     = spm_req_i[p].addr;

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
                            end
                        end
                        pmu_port_txn_cnt_reg[p] <= $bits(pmu_port_txn_cnt_reg[p])'(pmu_port_txn_cnt_reg[p] + 64'd1);
                    end else if (spm_req_valid_i[p] && !spm_req_ready_w[p]) begin
                        pmu_arb_stall_cnt_reg <= $bits(pmu_arb_stall_cnt_reg)'(pmu_arb_stall_cnt_reg + 64'd1);
                    end
                end

                // DMA write issue (after NoC)
                if (dma_write_complete_w) begin
                    axi_aw_rd_ptr_reg <= axi_queue_ptr_inc(axi_aw_rd_ptr_reg);
                    axi_w_rd_ptr_reg <= axi_queue_ptr_inc(axi_w_rd_ptr_reg);
                    axi_b_resp_fifo_reg[axi_b_wr_ptr_reg] <=
                        dma_write_decode_error_w ? 2'b10 : 2'b00;
                    axi_b_wr_ptr_reg <= axi_queue_ptr_inc(axi_b_wr_ptr_reg);
                end

                // DMA read issue (after NoC and DMA write)
                if (dma_read_head_pop_w) begin
                    axi_ar_rd_ptr_reg <= axi_queue_ptr_inc(axi_ar_rd_ptr_reg);
                end
                if (dma_read_issue_w) begin
                    dma_read_pending_reg <= 1'b1;
                    dma_read_bank_reg <= dma_read_bank_idx_w;
                end

                unique case ({s_axi_aw_push_w, dma_write_complete_w})
                    2'b10: axi_aw_count_reg <= axi_aw_count_reg + 1'b1;
                    2'b01: axi_aw_count_reg <= axi_aw_count_reg - 1'b1;
                    default: ;
                endcase
                unique case ({s_axi_w_push_w, dma_write_complete_w})
                    2'b10: axi_w_count_reg <= axi_w_count_reg + 1'b1;
                    2'b01: axi_w_count_reg <= axi_w_count_reg - 1'b1;
                    default: ;
                endcase
                unique case ({s_axi_ar_push_w, dma_read_head_pop_w})
                    2'b10: axi_ar_count_reg <= axi_ar_count_reg + 1'b1;
                    2'b01: axi_ar_count_reg <= axi_ar_count_reg - 1'b1;
                    default: ;
                endcase
                unique case ({dma_write_complete_w, s_axi_b_pop_w})
                    2'b10: axi_b_count_reg <= axi_b_count_reg + 1'b1;
                    2'b01: axi_b_count_reg <= axi_b_count_reg - 1'b1;
                    default: ;
                endcase
                unique case ({dma_r_push_w, s_axi_r_pop_w})
                    2'b10: axi_r_count_reg <= axi_r_count_reg + 1'b1;
                    2'b01: axi_r_count_reg <= axi_r_count_reg - 1'b1;
                    default: ;
                endcase
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
            ((BANKS_PER_GROUP * BANK_DATA_WIDTH) != CLUSTER_DATA_WIDTH)) begin
            $error("ScratchpadMemory baseline currently supports only NUM_NOC_PORTS=4, BANKS_PER_GROUP=3, BANK_DATA_WIDTH=64, ADDR_WIDTH=32, NOC_DATA_WIDTH=192");
        end
        if ((BANK_DEPTH % MACRO_DEPTH) != 0) begin
            $error("ScratchpadMemory requires BANK_DEPTH to be a multiple of %0d", MACRO_DEPTH);
        end
        if (AXI_QUEUE_DEPTH < 2) begin
            $error("ScratchpadMemory requires AXI_QUEUE_DEPTH >= 2");
        end
    end
    // synopsys translate_on

endmodule
