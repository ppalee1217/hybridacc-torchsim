//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   ClusterDataFabric
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Pipelined dual-ingress AXI4-Lite to per-cluster fabric.
//                Requests are retired in-order per requester and direction.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
//   2026/07/29 - Multi-active ingress, ROBs, and per-cluster inflight queues
//-----------------------------------------------------------------------------
module ClusterDataFabric import core_pkg::*; #(
    parameter int unsigned NUM_CLUSTERS = 1,
    parameter int unsigned FABRIC_DEPTH = 16
) (
    input  logic clk,
    input  logic reset_n,
    input  logic        s_dma_axi_aw_valid_i,
    output logic        s_dma_axi_aw_ready_o,
    input  logic [31:0] s_dma_axi_aw_addr_i,
    input  logic        s_dma_axi_w_valid_i,
    output logic        s_dma_axi_w_ready_o,
    input  logic [CL_AXI_DATA_WIDTH-1:0] s_dma_axi_w_data_i,
    input  logic [CL_AXI_DATA_WIDTH/8-1:0] s_dma_axi_w_strb_i,
    output logic        s_dma_axi_b_valid_o,
    input  logic        s_dma_axi_b_ready_i,
    output logic [1:0]  s_dma_axi_b_resp_o,
    input  logic        s_dma_axi_ar_valid_i,
    output logic        s_dma_axi_ar_ready_o,
    input  logic [31:0] s_dma_axi_ar_addr_i,
    output logic        s_dma_axi_r_valid_o,
    input  logic        s_dma_axi_r_ready_i,
    output logic [CL_AXI_DATA_WIDTH-1:0] s_dma_axi_r_data_o,
    output logic [1:0]  s_dma_axi_r_resp_o,
    input  logic        s_nlu_axi_aw_valid_i,
    output logic        s_nlu_axi_aw_ready_o,
    input  logic [31:0] s_nlu_axi_aw_addr_i,
    input  logic        s_nlu_axi_w_valid_i,
    output logic        s_nlu_axi_w_ready_o,
    input  logic [CL_AXI_DATA_WIDTH-1:0] s_nlu_axi_w_data_i,
    input  logic [CL_AXI_DATA_WIDTH/8-1:0] s_nlu_axi_w_strb_i,
    output logic        s_nlu_axi_b_valid_o,
    input  logic        s_nlu_axi_b_ready_i,
    output logic [1:0]  s_nlu_axi_b_resp_o,
    input  logic        s_nlu_axi_ar_valid_i,
    output logic        s_nlu_axi_ar_ready_o,
    input  logic [31:0] s_nlu_axi_ar_addr_i,
    output logic        s_nlu_axi_r_valid_o,
    input  logic        s_nlu_axi_r_ready_i,
    output logic [CL_AXI_DATA_WIDTH-1:0] s_nlu_axi_r_data_o,
    output logic [1:0]  s_nlu_axi_r_resp_o,
    output logic        m_cl_data_aw_valid_o[NUM_CLUSTERS],
    input  logic        m_cl_data_aw_ready_i[NUM_CLUSTERS],
    output logic [31:0] m_cl_data_aw_addr_o[NUM_CLUSTERS],
    output logic        m_cl_data_w_valid_o[NUM_CLUSTERS],
    input  logic        m_cl_data_w_ready_i[NUM_CLUSTERS],
    output logic [CL_AXI_DATA_WIDTH-1:0] m_cl_data_w_data_o[NUM_CLUSTERS],
    output logic [CL_AXI_DATA_WIDTH/8-1:0] m_cl_data_w_strb_o[NUM_CLUSTERS],
    input  logic        m_cl_data_b_valid_i[NUM_CLUSTERS],
    output logic        m_cl_data_b_ready_o[NUM_CLUSTERS],
    input  logic [1:0]  m_cl_data_b_resp_i[NUM_CLUSTERS],
    output logic        m_cl_data_ar_valid_o[NUM_CLUSTERS],
    input  logic        m_cl_data_ar_ready_i[NUM_CLUSTERS],
    output logic [31:0] m_cl_data_ar_addr_o[NUM_CLUSTERS],
    input  logic        m_cl_data_r_valid_i[NUM_CLUSTERS],
    output logic        m_cl_data_r_ready_o[NUM_CLUSTERS],
    input  logic [CL_AXI_DATA_WIDTH-1:0] m_cl_data_r_data_i[NUM_CLUSTERS],
    input  logic [1:0]  m_cl_data_r_resp_i[NUM_CLUSTERS]
);
    typedef enum logic {OWNER_DMA, OWNER_NLU} fabric_owner_e;

    localparam int unsigned CLUSTER_ID_WIDTH =
        $clog2(NUM_CLUSTERS > 1 ? NUM_CLUSTERS : 2);
    localparam int unsigned PTR_WIDTH =
        (FABRIC_DEPTH <= 1) ? 1 : $clog2(FABRIC_DEPTH);
    localparam int unsigned COUNT_WIDTH =
        (FABRIC_DEPTH <= 1) ? 1 : $clog2(FABRIC_DEPTH + 1);

    typedef logic [PTR_WIDTH-1:0] fifo_ptr_t;
    typedef logic [COUNT_WIDTH-1:0] fifo_count_t;

    typedef struct packed {
        logic [31:0] addr;
    } aw_entry_t;

    typedef struct packed {
        logic [CL_AXI_DATA_WIDTH-1:0] data;
        logic [CL_AXI_DATA_WIDTH/8-1:0] strb;
    } w_entry_t;

    typedef struct packed {
        logic [CLUSTER_ID_WIDTH-1:0] cluster;
        logic [31:0] local_addr;
        logic [CL_AXI_DATA_WIDTH-1:0] data;
        logic [CL_AXI_DATA_WIDTH/8-1:0] strb;
        fifo_ptr_t rob_slot;
    } wr_req_t;

    typedef struct packed {
        logic [CLUSTER_ID_WIDTH-1:0] cluster;
        logic [31:0] local_addr;
        fifo_ptr_t rob_slot;
    } rd_req_t;

    aw_entry_t dma_aw_fifo_reg[0:FABRIC_DEPTH-1];
    aw_entry_t nlu_aw_fifo_reg[0:FABRIC_DEPTH-1];
    w_entry_t  dma_w_fifo_reg [0:FABRIC_DEPTH-1];
    w_entry_t  nlu_w_fifo_reg [0:FABRIC_DEPTH-1];
    fifo_ptr_t dma_aw_wr_ptr_reg;
    fifo_ptr_t dma_aw_rd_ptr_reg;
    fifo_ptr_t nlu_aw_wr_ptr_reg;
    fifo_ptr_t nlu_aw_rd_ptr_reg;
    fifo_ptr_t dma_w_wr_ptr_reg;
    fifo_ptr_t dma_w_rd_ptr_reg;
    fifo_ptr_t nlu_w_wr_ptr_reg;
    fifo_ptr_t nlu_w_rd_ptr_reg;
    fifo_count_t dma_aw_count_reg;
    fifo_count_t nlu_aw_count_reg;
    fifo_count_t dma_w_count_reg;
    fifo_count_t nlu_w_count_reg;

    wr_req_t dma_wr_req_fifo_reg[0:FABRIC_DEPTH-1];
    wr_req_t nlu_wr_req_fifo_reg[0:FABRIC_DEPTH-1];
    rd_req_t dma_rd_req_fifo_reg[0:FABRIC_DEPTH-1];
    rd_req_t nlu_rd_req_fifo_reg[0:FABRIC_DEPTH-1];
    fifo_ptr_t dma_wr_req_wr_ptr_reg;
    fifo_ptr_t dma_wr_req_rd_ptr_reg;
    fifo_ptr_t nlu_wr_req_wr_ptr_reg;
    fifo_ptr_t nlu_wr_req_rd_ptr_reg;
    fifo_ptr_t dma_rd_req_wr_ptr_reg;
    fifo_ptr_t dma_rd_req_rd_ptr_reg;
    fifo_ptr_t nlu_rd_req_wr_ptr_reg;
    fifo_ptr_t nlu_rd_req_rd_ptr_reg;
    fifo_count_t dma_wr_req_count_reg;
    fifo_count_t nlu_wr_req_count_reg;
    fifo_count_t dma_rd_req_count_reg;
    fifo_count_t nlu_rd_req_count_reg;

    logic dma_wr_rob_complete_reg[0:FABRIC_DEPTH-1];
    logic nlu_wr_rob_complete_reg[0:FABRIC_DEPTH-1];
    logic [1:0] dma_wr_rob_resp_reg[0:FABRIC_DEPTH-1];
    logic [1:0] nlu_wr_rob_resp_reg[0:FABRIC_DEPTH-1];
    logic dma_rd_rob_complete_reg[0:FABRIC_DEPTH-1];
    logic nlu_rd_rob_complete_reg[0:FABRIC_DEPTH-1];
    logic [CL_AXI_DATA_WIDTH-1:0] dma_rd_rob_data_reg[0:FABRIC_DEPTH-1];
    logic [CL_AXI_DATA_WIDTH-1:0] nlu_rd_rob_data_reg[0:FABRIC_DEPTH-1];
    logic [1:0] dma_rd_rob_resp_reg[0:FABRIC_DEPTH-1];
    logic [1:0] nlu_rd_rob_resp_reg[0:FABRIC_DEPTH-1];
    fifo_ptr_t dma_wr_rob_wr_ptr_reg;
    fifo_ptr_t dma_wr_rob_rd_ptr_reg;
    fifo_ptr_t nlu_wr_rob_wr_ptr_reg;
    fifo_ptr_t nlu_wr_rob_rd_ptr_reg;
    fifo_ptr_t dma_rd_rob_wr_ptr_reg;
    fifo_ptr_t dma_rd_rob_rd_ptr_reg;
    fifo_ptr_t nlu_rd_rob_wr_ptr_reg;
    fifo_ptr_t nlu_rd_rob_rd_ptr_reg;
    fifo_count_t dma_wr_rob_count_reg;
    fifo_count_t nlu_wr_rob_count_reg;
    fifo_count_t dma_rd_rob_count_reg;
    fifo_count_t nlu_rd_rob_count_reg;

    logic wr_aw_valid_reg[NUM_CLUSTERS];
    logic [31:0] wr_aw_addr_reg[NUM_CLUSTERS];
    logic wr_w_valid_reg[NUM_CLUSTERS];
    logic [CL_AXI_DATA_WIDTH-1:0] wr_w_data_reg[NUM_CLUSTERS];
    logic [CL_AXI_DATA_WIDTH/8-1:0] wr_w_strb_reg[NUM_CLUSTERS];
    logic rd_ar_valid_reg[NUM_CLUSTERS];
    logic [31:0] rd_ar_addr_reg[NUM_CLUSTERS];

    fabric_owner_e wr_inflight_owner_reg[NUM_CLUSTERS][0:FABRIC_DEPTH-1];
    fifo_ptr_t wr_inflight_rob_slot_reg[NUM_CLUSTERS][0:FABRIC_DEPTH-1];
    fabric_owner_e rd_inflight_owner_reg[NUM_CLUSTERS][0:FABRIC_DEPTH-1];
    fifo_ptr_t rd_inflight_rob_slot_reg[NUM_CLUSTERS][0:FABRIC_DEPTH-1];
    fifo_ptr_t wr_inflight_wr_ptr_reg[NUM_CLUSTERS];
    fifo_ptr_t wr_inflight_rd_ptr_reg[NUM_CLUSTERS];
    fifo_ptr_t rd_inflight_wr_ptr_reg[NUM_CLUSTERS];
    fifo_ptr_t rd_inflight_rd_ptr_reg[NUM_CLUSTERS];
    fifo_count_t wr_inflight_count_reg[NUM_CLUSTERS];
    fifo_count_t rd_inflight_count_reg[NUM_CLUSTERS];

    fabric_owner_e wr_rr_last_grant_reg;
    fabric_owner_e rd_rr_last_grant_reg;

    logic dma_aw_push_w;
    logic dma_w_push_w;
    logic nlu_aw_push_w;
    logic nlu_w_push_w;
    logic dma_wr_merge_w;
    logic nlu_wr_merge_w;
    logic dma_wr_req_push_w;
    logic nlu_wr_req_push_w;
    logic dma_rd_accept_w;
    logic nlu_rd_accept_w;
    logic dma_rd_req_push_w;
    logic nlu_rd_req_push_w;
    logic dma_b_retire_w;
    logic nlu_b_retire_w;
    logic dma_r_retire_w;
    logic nlu_r_retire_w;

    logic wr_aw_fire_w[NUM_CLUSTERS];
    logic wr_w_fire_w[NUM_CLUSTERS];
    logic wr_slot_free_w[NUM_CLUSTERS];
    logic wr_b_fire_w[NUM_CLUSTERS];
    logic rd_ar_fire_w[NUM_CLUSTERS];
    logic rd_slot_free_w[NUM_CLUSTERS];
    logic rd_r_fire_w[NUM_CLUSTERS];

    logic wr_load_w;
    fabric_owner_e wr_load_owner_w;
    logic [CLUSTER_ID_WIDTH-1:0] wr_load_cluster_w;
    logic [31:0] wr_load_addr_w;
    logic [CL_AXI_DATA_WIDTH-1:0] wr_load_data_w;
    logic [CL_AXI_DATA_WIDTH/8-1:0] wr_load_strb_w;
    fifo_ptr_t wr_load_rob_slot_w;
    logic rd_load_w;
    fabric_owner_e rd_load_owner_w;
    logic [CLUSTER_ID_WIDTH-1:0] rd_load_cluster_w;
    logic [31:0] rd_load_addr_w;
    fifo_ptr_t rd_load_rob_slot_w;

    logic dma_wr_candidate_w;
    logic nlu_wr_candidate_w;
    logic dma_rd_candidate_w;
    logic nlu_rd_candidate_w;

    function automatic fifo_ptr_t ptr_inc(input fifo_ptr_t ptr);
        if (ptr == fifo_ptr_t'(FABRIC_DEPTH - 1)) begin
            return '0;
        end
        return ptr + 1'b1;
    endfunction

    function automatic logic [CLUSTER_ID_WIDTH-1:0] decode_cluster_id(
        input logic [31:0] global_addr
    );
        return global_addr[24 +: CLUSTER_ID_WIDTH];
    endfunction

    function automatic logic [31:0] decode_local_addr(
        input logic [31:0] global_addr
    );
        return {8'h0, global_addr[23:0]};
    endfunction

    function automatic logic decode_addr_ok(input logic [31:0] global_addr);
        return ((global_addr >> 24) < NUM_CLUSTERS);
    endfunction

    always_comb begin
        s_dma_axi_aw_ready_o = (dma_aw_count_reg < FABRIC_DEPTH);
        s_dma_axi_w_ready_o  = (dma_w_count_reg < FABRIC_DEPTH);
        s_nlu_axi_aw_ready_o = (nlu_aw_count_reg < FABRIC_DEPTH);
        s_nlu_axi_w_ready_o  = (nlu_w_count_reg < FABRIC_DEPTH);
        s_dma_axi_ar_ready_o =
            (dma_rd_rob_count_reg < FABRIC_DEPTH) &&
            (dma_rd_req_count_reg < FABRIC_DEPTH);
        s_nlu_axi_ar_ready_o =
            (nlu_rd_rob_count_reg < FABRIC_DEPTH) &&
            (nlu_rd_req_count_reg < FABRIC_DEPTH);

        dma_aw_push_w = s_dma_axi_aw_valid_i && s_dma_axi_aw_ready_o;
        dma_w_push_w  = s_dma_axi_w_valid_i && s_dma_axi_w_ready_o;
        nlu_aw_push_w = s_nlu_axi_aw_valid_i && s_nlu_axi_aw_ready_o;
        nlu_w_push_w  = s_nlu_axi_w_valid_i && s_nlu_axi_w_ready_o;

        dma_wr_merge_w =
            (dma_aw_count_reg != 0) &&
            (dma_w_count_reg != 0) &&
            (dma_wr_rob_count_reg < FABRIC_DEPTH) &&
            (dma_wr_req_count_reg < FABRIC_DEPTH);
        nlu_wr_merge_w =
            (nlu_aw_count_reg != 0) &&
            (nlu_w_count_reg != 0) &&
            (nlu_wr_rob_count_reg < FABRIC_DEPTH) &&
            (nlu_wr_req_count_reg < FABRIC_DEPTH);
        dma_wr_req_push_w =
            dma_wr_merge_w &&
            decode_addr_ok(dma_aw_fifo_reg[dma_aw_rd_ptr_reg].addr);
        nlu_wr_req_push_w =
            nlu_wr_merge_w &&
            decode_addr_ok(nlu_aw_fifo_reg[nlu_aw_rd_ptr_reg].addr);

        dma_rd_accept_w = s_dma_axi_ar_valid_i && s_dma_axi_ar_ready_o;
        nlu_rd_accept_w = s_nlu_axi_ar_valid_i && s_nlu_axi_ar_ready_o;
        dma_rd_req_push_w =
            dma_rd_accept_w && decode_addr_ok(s_dma_axi_ar_addr_i);
        nlu_rd_req_push_w =
            nlu_rd_accept_w && decode_addr_ok(s_nlu_axi_ar_addr_i);

        s_dma_axi_b_valid_o =
            (dma_wr_rob_count_reg != 0) &&
            dma_wr_rob_complete_reg[dma_wr_rob_rd_ptr_reg];
        s_dma_axi_b_resp_o =
            (dma_wr_rob_count_reg != 0) ?
            dma_wr_rob_resp_reg[dma_wr_rob_rd_ptr_reg] : 2'b00;
        s_nlu_axi_b_valid_o =
            (nlu_wr_rob_count_reg != 0) &&
            nlu_wr_rob_complete_reg[nlu_wr_rob_rd_ptr_reg];
        s_nlu_axi_b_resp_o =
            (nlu_wr_rob_count_reg != 0) ?
            nlu_wr_rob_resp_reg[nlu_wr_rob_rd_ptr_reg] : 2'b00;
        s_dma_axi_r_valid_o =
            (dma_rd_rob_count_reg != 0) &&
            dma_rd_rob_complete_reg[dma_rd_rob_rd_ptr_reg];
        s_dma_axi_r_data_o =
            (dma_rd_rob_count_reg != 0) ?
            dma_rd_rob_data_reg[dma_rd_rob_rd_ptr_reg] : '0;
        s_dma_axi_r_resp_o =
            (dma_rd_rob_count_reg != 0) ?
            dma_rd_rob_resp_reg[dma_rd_rob_rd_ptr_reg] : 2'b00;
        s_nlu_axi_r_valid_o =
            (nlu_rd_rob_count_reg != 0) &&
            nlu_rd_rob_complete_reg[nlu_rd_rob_rd_ptr_reg];
        s_nlu_axi_r_data_o =
            (nlu_rd_rob_count_reg != 0) ?
            nlu_rd_rob_data_reg[nlu_rd_rob_rd_ptr_reg] : '0;
        s_nlu_axi_r_resp_o =
            (nlu_rd_rob_count_reg != 0) ?
            nlu_rd_rob_resp_reg[nlu_rd_rob_rd_ptr_reg] : 2'b00;

        dma_b_retire_w = s_dma_axi_b_valid_o && s_dma_axi_b_ready_i;
        nlu_b_retire_w = s_nlu_axi_b_valid_o && s_nlu_axi_b_ready_i;
        dma_r_retire_w = s_dma_axi_r_valid_o && s_dma_axi_r_ready_i;
        nlu_r_retire_w = s_nlu_axi_r_valid_o && s_nlu_axi_r_ready_i;

        for (int unsigned c = 0; c < NUM_CLUSTERS; c++) begin
            m_cl_data_aw_valid_o[c] = wr_aw_valid_reg[c];
            m_cl_data_aw_addr_o[c]  = wr_aw_addr_reg[c];
            m_cl_data_w_valid_o[c]  = wr_w_valid_reg[c];
            m_cl_data_w_data_o[c]   = wr_w_data_reg[c];
            m_cl_data_w_strb_o[c]   = wr_w_strb_reg[c];
            m_cl_data_b_ready_o[c]  = (wr_inflight_count_reg[c] != 0);
            m_cl_data_ar_valid_o[c] = rd_ar_valid_reg[c];
            m_cl_data_ar_addr_o[c]  = rd_ar_addr_reg[c];
            m_cl_data_r_ready_o[c]  = (rd_inflight_count_reg[c] != 0);

            wr_aw_fire_w[c] =
                wr_aw_valid_reg[c] && m_cl_data_aw_ready_i[c];
            wr_w_fire_w[c] =
                wr_w_valid_reg[c] && m_cl_data_w_ready_i[c];
            wr_slot_free_w[c] =
                (!wr_aw_valid_reg[c] || wr_aw_fire_w[c]) &&
                (!wr_w_valid_reg[c] || wr_w_fire_w[c]);
            wr_b_fire_w[c] =
                m_cl_data_b_valid_i[c] && m_cl_data_b_ready_o[c];
            rd_ar_fire_w[c] =
                rd_ar_valid_reg[c] && m_cl_data_ar_ready_i[c];
            rd_slot_free_w[c] =
                !rd_ar_valid_reg[c] || rd_ar_fire_w[c];
            rd_r_fire_w[c] =
                m_cl_data_r_valid_i[c] && m_cl_data_r_ready_o[c];
        end

        dma_wr_candidate_w = 1'b0;
        if (dma_wr_req_count_reg != 0) begin
            if ((dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].cluster < NUM_CLUSTERS) &&
                wr_slot_free_w[dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].cluster] &&
                ((wr_inflight_count_reg[
                    dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].cluster
                 ] < FABRIC_DEPTH) ||
                 wr_b_fire_w[
                    dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].cluster
                 ])) begin
                dma_wr_candidate_w = 1'b1;
            end
        end
        nlu_wr_candidate_w = 1'b0;
        if (nlu_wr_req_count_reg != 0) begin
            if ((nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].cluster < NUM_CLUSTERS) &&
                wr_slot_free_w[nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].cluster] &&
                ((wr_inflight_count_reg[
                    nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].cluster
                 ] < FABRIC_DEPTH) ||
                 wr_b_fire_w[
                    nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].cluster
                 ])) begin
                nlu_wr_candidate_w = 1'b1;
            end
        end

        wr_load_w = 1'b0;
        wr_load_owner_w = OWNER_DMA;
        wr_load_cluster_w = '0;
        wr_load_addr_w = '0;
        wr_load_data_w = '0;
        wr_load_strb_w = '0;
        wr_load_rob_slot_w = '0;
        if (dma_wr_candidate_w && nlu_wr_candidate_w) begin
            wr_load_owner_w =
                (wr_rr_last_grant_reg == OWNER_DMA) ? OWNER_NLU : OWNER_DMA;
            wr_load_w = 1'b1;
        end else if (dma_wr_candidate_w) begin
            wr_load_owner_w = OWNER_DMA;
            wr_load_w = 1'b1;
        end else if (nlu_wr_candidate_w) begin
            wr_load_owner_w = OWNER_NLU;
            wr_load_w = 1'b1;
        end
        if (wr_load_w && (wr_load_owner_w == OWNER_DMA)) begin
            wr_load_cluster_w =
                dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].cluster;
            wr_load_addr_w =
                dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].local_addr;
            wr_load_data_w =
                dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].data;
            wr_load_strb_w =
                dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].strb;
            wr_load_rob_slot_w =
                dma_wr_req_fifo_reg[dma_wr_req_rd_ptr_reg].rob_slot;
        end else if (wr_load_w) begin
            wr_load_cluster_w =
                nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].cluster;
            wr_load_addr_w =
                nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].local_addr;
            wr_load_data_w =
                nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].data;
            wr_load_strb_w =
                nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].strb;
            wr_load_rob_slot_w =
                nlu_wr_req_fifo_reg[nlu_wr_req_rd_ptr_reg].rob_slot;
        end

        dma_rd_candidate_w = 1'b0;
        if (dma_rd_req_count_reg != 0) begin
            if ((dma_rd_req_fifo_reg[dma_rd_req_rd_ptr_reg].cluster < NUM_CLUSTERS) &&
                rd_slot_free_w[dma_rd_req_fifo_reg[dma_rd_req_rd_ptr_reg].cluster] &&
                ((rd_inflight_count_reg[
                    dma_rd_req_fifo_reg[dma_rd_req_rd_ptr_reg].cluster
                 ] < FABRIC_DEPTH) ||
                 rd_r_fire_w[
                    dma_rd_req_fifo_reg[dma_rd_req_rd_ptr_reg].cluster
                 ])) begin
                dma_rd_candidate_w = 1'b1;
            end
        end
        nlu_rd_candidate_w = 1'b0;
        if (nlu_rd_req_count_reg != 0) begin
            if ((nlu_rd_req_fifo_reg[nlu_rd_req_rd_ptr_reg].cluster < NUM_CLUSTERS) &&
                rd_slot_free_w[nlu_rd_req_fifo_reg[nlu_rd_req_rd_ptr_reg].cluster] &&
                ((rd_inflight_count_reg[
                    nlu_rd_req_fifo_reg[nlu_rd_req_rd_ptr_reg].cluster
                 ] < FABRIC_DEPTH) ||
                 rd_r_fire_w[
                    nlu_rd_req_fifo_reg[nlu_rd_req_rd_ptr_reg].cluster
                 ])) begin
                nlu_rd_candidate_w = 1'b1;
            end
        end

        rd_load_w = 1'b0;
        rd_load_owner_w = OWNER_DMA;
        rd_load_cluster_w = '0;
        rd_load_addr_w = '0;
        rd_load_rob_slot_w = '0;
        if (dma_rd_candidate_w && nlu_rd_candidate_w) begin
            rd_load_owner_w =
                (rd_rr_last_grant_reg == OWNER_DMA) ? OWNER_NLU : OWNER_DMA;
            rd_load_w = 1'b1;
        end else if (dma_rd_candidate_w) begin
            rd_load_owner_w = OWNER_DMA;
            rd_load_w = 1'b1;
        end else if (nlu_rd_candidate_w) begin
            rd_load_owner_w = OWNER_NLU;
            rd_load_w = 1'b1;
        end
        if (rd_load_w && (rd_load_owner_w == OWNER_DMA)) begin
            rd_load_cluster_w =
                dma_rd_req_fifo_reg[dma_rd_req_rd_ptr_reg].cluster;
            rd_load_addr_w =
                dma_rd_req_fifo_reg[dma_rd_req_rd_ptr_reg].local_addr;
            rd_load_rob_slot_w =
                dma_rd_req_fifo_reg[dma_rd_req_rd_ptr_reg].rob_slot;
        end else if (rd_load_w) begin
            rd_load_cluster_w =
                nlu_rd_req_fifo_reg[nlu_rd_req_rd_ptr_reg].cluster;
            rd_load_addr_w =
                nlu_rd_req_fifo_reg[nlu_rd_req_rd_ptr_reg].local_addr;
            rd_load_rob_slot_w =
                nlu_rd_req_fifo_reg[nlu_rd_req_rd_ptr_reg].rob_slot;
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            dma_aw_wr_ptr_reg <= '0;
            dma_aw_rd_ptr_reg <= '0;
            nlu_aw_wr_ptr_reg <= '0;
            nlu_aw_rd_ptr_reg <= '0;
            dma_w_wr_ptr_reg <= '0;
            dma_w_rd_ptr_reg <= '0;
            nlu_w_wr_ptr_reg <= '0;
            nlu_w_rd_ptr_reg <= '0;
            dma_aw_count_reg <= '0;
            nlu_aw_count_reg <= '0;
            dma_w_count_reg <= '0;
            nlu_w_count_reg <= '0;

            dma_wr_req_wr_ptr_reg <= '0;
            dma_wr_req_rd_ptr_reg <= '0;
            nlu_wr_req_wr_ptr_reg <= '0;
            nlu_wr_req_rd_ptr_reg <= '0;
            dma_rd_req_wr_ptr_reg <= '0;
            dma_rd_req_rd_ptr_reg <= '0;
            nlu_rd_req_wr_ptr_reg <= '0;
            nlu_rd_req_rd_ptr_reg <= '0;
            dma_wr_req_count_reg <= '0;
            nlu_wr_req_count_reg <= '0;
            dma_rd_req_count_reg <= '0;
            nlu_rd_req_count_reg <= '0;

            dma_wr_rob_wr_ptr_reg <= '0;
            dma_wr_rob_rd_ptr_reg <= '0;
            nlu_wr_rob_wr_ptr_reg <= '0;
            nlu_wr_rob_rd_ptr_reg <= '0;
            dma_rd_rob_wr_ptr_reg <= '0;
            dma_rd_rob_rd_ptr_reg <= '0;
            nlu_rd_rob_wr_ptr_reg <= '0;
            nlu_rd_rob_rd_ptr_reg <= '0;
            dma_wr_rob_count_reg <= '0;
            nlu_wr_rob_count_reg <= '0;
            dma_rd_rob_count_reg <= '0;
            nlu_rd_rob_count_reg <= '0;

            wr_rr_last_grant_reg <= OWNER_NLU;
            rd_rr_last_grant_reg <= OWNER_NLU;

            for (int unsigned entry = 0; entry < FABRIC_DEPTH; entry++) begin
                dma_wr_rob_complete_reg[entry] <= 1'b0;
                nlu_wr_rob_complete_reg[entry] <= 1'b0;
                dma_wr_rob_resp_reg[entry] <= 2'b00;
                nlu_wr_rob_resp_reg[entry] <= 2'b00;
                dma_rd_rob_complete_reg[entry] <= 1'b0;
                nlu_rd_rob_complete_reg[entry] <= 1'b0;
                dma_rd_rob_data_reg[entry] <= '0;
                nlu_rd_rob_data_reg[entry] <= '0;
                dma_rd_rob_resp_reg[entry] <= 2'b00;
                nlu_rd_rob_resp_reg[entry] <= 2'b00;
            end

            for (int unsigned c = 0; c < NUM_CLUSTERS; c++) begin
                wr_aw_valid_reg[c] <= 1'b0;
                wr_aw_addr_reg[c] <= '0;
                wr_w_valid_reg[c] <= 1'b0;
                wr_w_data_reg[c] <= '0;
                wr_w_strb_reg[c] <= '0;
                rd_ar_valid_reg[c] <= 1'b0;
                rd_ar_addr_reg[c] <= '0;
                wr_inflight_wr_ptr_reg[c] <= '0;
                wr_inflight_rd_ptr_reg[c] <= '0;
                rd_inflight_wr_ptr_reg[c] <= '0;
                rd_inflight_rd_ptr_reg[c] <= '0;
                wr_inflight_count_reg[c] <= '0;
                rd_inflight_count_reg[c] <= '0;
            end
        end else begin
            if (dma_aw_push_w) begin
                dma_aw_fifo_reg[dma_aw_wr_ptr_reg].addr <= s_dma_axi_aw_addr_i;
                dma_aw_wr_ptr_reg <= ptr_inc(dma_aw_wr_ptr_reg);
            end
            if (nlu_aw_push_w) begin
                nlu_aw_fifo_reg[nlu_aw_wr_ptr_reg].addr <= s_nlu_axi_aw_addr_i;
                nlu_aw_wr_ptr_reg <= ptr_inc(nlu_aw_wr_ptr_reg);
            end
            if (dma_w_push_w) begin
                dma_w_fifo_reg[dma_w_wr_ptr_reg].data <= s_dma_axi_w_data_i;
                dma_w_fifo_reg[dma_w_wr_ptr_reg].strb <= s_dma_axi_w_strb_i;
                dma_w_wr_ptr_reg <= ptr_inc(dma_w_wr_ptr_reg);
            end
            if (nlu_w_push_w) begin
                nlu_w_fifo_reg[nlu_w_wr_ptr_reg].data <= s_nlu_axi_w_data_i;
                nlu_w_fifo_reg[nlu_w_wr_ptr_reg].strb <= s_nlu_axi_w_strb_i;
                nlu_w_wr_ptr_reg <= ptr_inc(nlu_w_wr_ptr_reg);
            end
            if (dma_wr_merge_w) begin
                dma_aw_rd_ptr_reg <= ptr_inc(dma_aw_rd_ptr_reg);
                dma_w_rd_ptr_reg <= ptr_inc(dma_w_rd_ptr_reg);
            end
            if (nlu_wr_merge_w) begin
                nlu_aw_rd_ptr_reg <= ptr_inc(nlu_aw_rd_ptr_reg);
                nlu_w_rd_ptr_reg <= ptr_inc(nlu_w_rd_ptr_reg);
            end
            unique case ({dma_aw_push_w, dma_wr_merge_w})
                2'b10: dma_aw_count_reg <= dma_aw_count_reg + 1'b1;
                2'b01: dma_aw_count_reg <= dma_aw_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({dma_w_push_w, dma_wr_merge_w})
                2'b10: dma_w_count_reg <= dma_w_count_reg + 1'b1;
                2'b01: dma_w_count_reg <= dma_w_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({nlu_aw_push_w, nlu_wr_merge_w})
                2'b10: nlu_aw_count_reg <= nlu_aw_count_reg + 1'b1;
                2'b01: nlu_aw_count_reg <= nlu_aw_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({nlu_w_push_w, nlu_wr_merge_w})
                2'b10: nlu_w_count_reg <= nlu_w_count_reg + 1'b1;
                2'b01: nlu_w_count_reg <= nlu_w_count_reg - 1'b1;
                default: ;
            endcase

            if (dma_wr_req_push_w) begin
                dma_wr_req_fifo_reg[dma_wr_req_wr_ptr_reg].cluster <=
                    decode_cluster_id(dma_aw_fifo_reg[dma_aw_rd_ptr_reg].addr);
                dma_wr_req_fifo_reg[dma_wr_req_wr_ptr_reg].local_addr <=
                    decode_local_addr(dma_aw_fifo_reg[dma_aw_rd_ptr_reg].addr);
                dma_wr_req_fifo_reg[dma_wr_req_wr_ptr_reg].data <=
                    dma_w_fifo_reg[dma_w_rd_ptr_reg].data;
                dma_wr_req_fifo_reg[dma_wr_req_wr_ptr_reg].strb <=
                    dma_w_fifo_reg[dma_w_rd_ptr_reg].strb;
                dma_wr_req_fifo_reg[dma_wr_req_wr_ptr_reg].rob_slot <=
                    dma_wr_rob_wr_ptr_reg;
                dma_wr_req_wr_ptr_reg <= ptr_inc(dma_wr_req_wr_ptr_reg);
            end
            if (nlu_wr_req_push_w) begin
                nlu_wr_req_fifo_reg[nlu_wr_req_wr_ptr_reg].cluster <=
                    decode_cluster_id(nlu_aw_fifo_reg[nlu_aw_rd_ptr_reg].addr);
                nlu_wr_req_fifo_reg[nlu_wr_req_wr_ptr_reg].local_addr <=
                    decode_local_addr(nlu_aw_fifo_reg[nlu_aw_rd_ptr_reg].addr);
                nlu_wr_req_fifo_reg[nlu_wr_req_wr_ptr_reg].data <=
                    nlu_w_fifo_reg[nlu_w_rd_ptr_reg].data;
                nlu_wr_req_fifo_reg[nlu_wr_req_wr_ptr_reg].strb <=
                    nlu_w_fifo_reg[nlu_w_rd_ptr_reg].strb;
                nlu_wr_req_fifo_reg[nlu_wr_req_wr_ptr_reg].rob_slot <=
                    nlu_wr_rob_wr_ptr_reg;
                nlu_wr_req_wr_ptr_reg <= ptr_inc(nlu_wr_req_wr_ptr_reg);
            end
            if (dma_rd_req_push_w) begin
                dma_rd_req_fifo_reg[dma_rd_req_wr_ptr_reg].cluster <=
                    decode_cluster_id(s_dma_axi_ar_addr_i);
                dma_rd_req_fifo_reg[dma_rd_req_wr_ptr_reg].local_addr <=
                    decode_local_addr(s_dma_axi_ar_addr_i);
                dma_rd_req_fifo_reg[dma_rd_req_wr_ptr_reg].rob_slot <=
                    dma_rd_rob_wr_ptr_reg;
                dma_rd_req_wr_ptr_reg <= ptr_inc(dma_rd_req_wr_ptr_reg);
            end
            if (nlu_rd_req_push_w) begin
                nlu_rd_req_fifo_reg[nlu_rd_req_wr_ptr_reg].cluster <=
                    decode_cluster_id(s_nlu_axi_ar_addr_i);
                nlu_rd_req_fifo_reg[nlu_rd_req_wr_ptr_reg].local_addr <=
                    decode_local_addr(s_nlu_axi_ar_addr_i);
                nlu_rd_req_fifo_reg[nlu_rd_req_wr_ptr_reg].rob_slot <=
                    nlu_rd_rob_wr_ptr_reg;
                nlu_rd_req_wr_ptr_reg <= ptr_inc(nlu_rd_req_wr_ptr_reg);
            end

            if (wr_load_w && (wr_load_owner_w == OWNER_DMA)) begin
                dma_wr_req_rd_ptr_reg <= ptr_inc(dma_wr_req_rd_ptr_reg);
            end
            if (wr_load_w && (wr_load_owner_w == OWNER_NLU)) begin
                nlu_wr_req_rd_ptr_reg <= ptr_inc(nlu_wr_req_rd_ptr_reg);
            end
            if (rd_load_w && (rd_load_owner_w == OWNER_DMA)) begin
                dma_rd_req_rd_ptr_reg <= ptr_inc(dma_rd_req_rd_ptr_reg);
            end
            if (rd_load_w && (rd_load_owner_w == OWNER_NLU)) begin
                nlu_rd_req_rd_ptr_reg <= ptr_inc(nlu_rd_req_rd_ptr_reg);
            end

            unique case ({
                dma_wr_req_push_w,
                wr_load_w && (wr_load_owner_w == OWNER_DMA)
            })
                2'b10: dma_wr_req_count_reg <= dma_wr_req_count_reg + 1'b1;
                2'b01: dma_wr_req_count_reg <= dma_wr_req_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({
                nlu_wr_req_push_w,
                wr_load_w && (wr_load_owner_w == OWNER_NLU)
            })
                2'b10: nlu_wr_req_count_reg <= nlu_wr_req_count_reg + 1'b1;
                2'b01: nlu_wr_req_count_reg <= nlu_wr_req_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({
                dma_rd_req_push_w,
                rd_load_w && (rd_load_owner_w == OWNER_DMA)
            })
                2'b10: dma_rd_req_count_reg <= dma_rd_req_count_reg + 1'b1;
                2'b01: dma_rd_req_count_reg <= dma_rd_req_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({
                nlu_rd_req_push_w,
                rd_load_w && (rd_load_owner_w == OWNER_NLU)
            })
                2'b10: nlu_rd_req_count_reg <= nlu_rd_req_count_reg + 1'b1;
                2'b01: nlu_rd_req_count_reg <= nlu_rd_req_count_reg - 1'b1;
                default: ;
            endcase

            if (dma_wr_merge_w) begin
                dma_wr_rob_complete_reg[dma_wr_rob_wr_ptr_reg] <=
                    !decode_addr_ok(dma_aw_fifo_reg[dma_aw_rd_ptr_reg].addr);
                dma_wr_rob_resp_reg[dma_wr_rob_wr_ptr_reg] <=
                    decode_addr_ok(dma_aw_fifo_reg[dma_aw_rd_ptr_reg].addr) ?
                    2'b00 : 2'b10;
                dma_wr_rob_wr_ptr_reg <= ptr_inc(dma_wr_rob_wr_ptr_reg);
            end
            if (nlu_wr_merge_w) begin
                nlu_wr_rob_complete_reg[nlu_wr_rob_wr_ptr_reg] <=
                    !decode_addr_ok(nlu_aw_fifo_reg[nlu_aw_rd_ptr_reg].addr);
                nlu_wr_rob_resp_reg[nlu_wr_rob_wr_ptr_reg] <=
                    decode_addr_ok(nlu_aw_fifo_reg[nlu_aw_rd_ptr_reg].addr) ?
                    2'b00 : 2'b10;
                nlu_wr_rob_wr_ptr_reg <= ptr_inc(nlu_wr_rob_wr_ptr_reg);
            end
            if (dma_rd_accept_w) begin
                dma_rd_rob_complete_reg[dma_rd_rob_wr_ptr_reg] <=
                    !decode_addr_ok(s_dma_axi_ar_addr_i);
                dma_rd_rob_data_reg[dma_rd_rob_wr_ptr_reg] <= '0;
                dma_rd_rob_resp_reg[dma_rd_rob_wr_ptr_reg] <=
                    decode_addr_ok(s_dma_axi_ar_addr_i) ? 2'b00 : 2'b10;
                dma_rd_rob_wr_ptr_reg <= ptr_inc(dma_rd_rob_wr_ptr_reg);
            end
            if (nlu_rd_accept_w) begin
                nlu_rd_rob_complete_reg[nlu_rd_rob_wr_ptr_reg] <=
                    !decode_addr_ok(s_nlu_axi_ar_addr_i);
                nlu_rd_rob_data_reg[nlu_rd_rob_wr_ptr_reg] <= '0;
                nlu_rd_rob_resp_reg[nlu_rd_rob_wr_ptr_reg] <=
                    decode_addr_ok(s_nlu_axi_ar_addr_i) ? 2'b00 : 2'b10;
                nlu_rd_rob_wr_ptr_reg <= ptr_inc(nlu_rd_rob_wr_ptr_reg);
            end

            if (dma_b_retire_w) begin
                dma_wr_rob_rd_ptr_reg <= ptr_inc(dma_wr_rob_rd_ptr_reg);
            end
            if (nlu_b_retire_w) begin
                nlu_wr_rob_rd_ptr_reg <= ptr_inc(nlu_wr_rob_rd_ptr_reg);
            end
            if (dma_r_retire_w) begin
                dma_rd_rob_rd_ptr_reg <= ptr_inc(dma_rd_rob_rd_ptr_reg);
            end
            if (nlu_r_retire_w) begin
                nlu_rd_rob_rd_ptr_reg <= ptr_inc(nlu_rd_rob_rd_ptr_reg);
            end

            unique case ({dma_wr_merge_w, dma_b_retire_w})
                2'b10: dma_wr_rob_count_reg <= dma_wr_rob_count_reg + 1'b1;
                2'b01: dma_wr_rob_count_reg <= dma_wr_rob_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({nlu_wr_merge_w, nlu_b_retire_w})
                2'b10: nlu_wr_rob_count_reg <= nlu_wr_rob_count_reg + 1'b1;
                2'b01: nlu_wr_rob_count_reg <= nlu_wr_rob_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({dma_rd_accept_w, dma_r_retire_w})
                2'b10: dma_rd_rob_count_reg <= dma_rd_rob_count_reg + 1'b1;
                2'b01: dma_rd_rob_count_reg <= dma_rd_rob_count_reg - 1'b1;
                default: ;
            endcase
            unique case ({nlu_rd_accept_w, nlu_r_retire_w})
                2'b10: nlu_rd_rob_count_reg <= nlu_rd_rob_count_reg + 1'b1;
                2'b01: nlu_rd_rob_count_reg <= nlu_rd_rob_count_reg - 1'b1;
                default: ;
            endcase

            if (wr_load_w) begin
                wr_rr_last_grant_reg <= wr_load_owner_w;
            end
            if (rd_load_w) begin
                rd_rr_last_grant_reg <= rd_load_owner_w;
            end

            for (int unsigned c = 0; c < NUM_CLUSTERS; c++) begin
                if (wr_aw_fire_w[c]) begin
                    wr_aw_valid_reg[c] <= 1'b0;
                end
                if (wr_w_fire_w[c]) begin
                    wr_w_valid_reg[c] <= 1'b0;
                end
                if (rd_ar_fire_w[c]) begin
                    rd_ar_valid_reg[c] <= 1'b0;
                end

                if (wr_load_w && (wr_load_cluster_w == c)) begin
                    wr_aw_valid_reg[c] <= 1'b1;
                    wr_aw_addr_reg[c] <= wr_load_addr_w;
                    wr_w_valid_reg[c] <= 1'b1;
                    wr_w_data_reg[c] <= wr_load_data_w;
                    wr_w_strb_reg[c] <= wr_load_strb_w;
                    wr_inflight_owner_reg[c][wr_inflight_wr_ptr_reg[c]] <=
                        wr_load_owner_w;
                    wr_inflight_rob_slot_reg[c][wr_inflight_wr_ptr_reg[c]] <=
                        wr_load_rob_slot_w;
                    wr_inflight_wr_ptr_reg[c] <=
                        ptr_inc(wr_inflight_wr_ptr_reg[c]);
                end
                if (rd_load_w && (rd_load_cluster_w == c)) begin
                    rd_ar_valid_reg[c] <= 1'b1;
                    rd_ar_addr_reg[c] <= rd_load_addr_w;
                    rd_inflight_owner_reg[c][rd_inflight_wr_ptr_reg[c]] <=
                        rd_load_owner_w;
                    rd_inflight_rob_slot_reg[c][rd_inflight_wr_ptr_reg[c]] <=
                        rd_load_rob_slot_w;
                    rd_inflight_wr_ptr_reg[c] <=
                        ptr_inc(rd_inflight_wr_ptr_reg[c]);
                end

                if (wr_b_fire_w[c]) begin
                    if (wr_inflight_owner_reg[c][
                        wr_inflight_rd_ptr_reg[c]
                    ] == OWNER_DMA) begin
                        dma_wr_rob_complete_reg[
                            wr_inflight_rob_slot_reg[c][
                                wr_inflight_rd_ptr_reg[c]
                            ]
                        ] <= 1'b1;
                        dma_wr_rob_resp_reg[
                            wr_inflight_rob_slot_reg[c][
                                wr_inflight_rd_ptr_reg[c]
                            ]
                        ] <= m_cl_data_b_resp_i[c];
                    end else begin
                        nlu_wr_rob_complete_reg[
                            wr_inflight_rob_slot_reg[c][
                                wr_inflight_rd_ptr_reg[c]
                            ]
                        ] <= 1'b1;
                        nlu_wr_rob_resp_reg[
                            wr_inflight_rob_slot_reg[c][
                                wr_inflight_rd_ptr_reg[c]
                            ]
                        ] <= m_cl_data_b_resp_i[c];
                    end
                    wr_inflight_rd_ptr_reg[c] <=
                        ptr_inc(wr_inflight_rd_ptr_reg[c]);
                end

                if (rd_r_fire_w[c]) begin
                    if (rd_inflight_owner_reg[c][
                        rd_inflight_rd_ptr_reg[c]
                    ] == OWNER_DMA) begin
                        dma_rd_rob_complete_reg[
                            rd_inflight_rob_slot_reg[c][
                                rd_inflight_rd_ptr_reg[c]
                            ]
                        ] <= 1'b1;
                        dma_rd_rob_data_reg[
                            rd_inflight_rob_slot_reg[c][
                                rd_inflight_rd_ptr_reg[c]
                            ]
                        ] <= m_cl_data_r_data_i[c];
                        dma_rd_rob_resp_reg[
                            rd_inflight_rob_slot_reg[c][
                                rd_inflight_rd_ptr_reg[c]
                            ]
                        ] <= m_cl_data_r_resp_i[c];
                    end else begin
                        nlu_rd_rob_complete_reg[
                            rd_inflight_rob_slot_reg[c][
                                rd_inflight_rd_ptr_reg[c]
                            ]
                        ] <= 1'b1;
                        nlu_rd_rob_data_reg[
                            rd_inflight_rob_slot_reg[c][
                                rd_inflight_rd_ptr_reg[c]
                            ]
                        ] <= m_cl_data_r_data_i[c];
                        nlu_rd_rob_resp_reg[
                            rd_inflight_rob_slot_reg[c][
                                rd_inflight_rd_ptr_reg[c]
                            ]
                        ] <= m_cl_data_r_resp_i[c];
                    end
                    rd_inflight_rd_ptr_reg[c] <=
                        ptr_inc(rd_inflight_rd_ptr_reg[c]);
                end

                unique case ({
                    wr_load_w && (wr_load_cluster_w == c),
                    wr_b_fire_w[c]
                })
                    2'b10: wr_inflight_count_reg[c] <=
                        wr_inflight_count_reg[c] + 1'b1;
                    2'b01: wr_inflight_count_reg[c] <=
                        wr_inflight_count_reg[c] - 1'b1;
                    default: ;
                endcase
                unique case ({
                    rd_load_w && (rd_load_cluster_w == c),
                    rd_r_fire_w[c]
                })
                    2'b10: rd_inflight_count_reg[c] <=
                        rd_inflight_count_reg[c] + 1'b1;
                    2'b01: rd_inflight_count_reg[c] <=
                        rd_inflight_count_reg[c] - 1'b1;
                    default: ;
                endcase
            end
        end
    end

    // synopsys translate_off
    initial begin
        if (FABRIC_DEPTH < 2) begin
            $error("ClusterDataFabric requires FABRIC_DEPTH >= 2");
        end
        if (NUM_CLUSTERS > 256) begin
            $error("ClusterDataFabric address encoding supports at most 256 clusters");
        end
    end
    // synopsys translate_on
endmodule
