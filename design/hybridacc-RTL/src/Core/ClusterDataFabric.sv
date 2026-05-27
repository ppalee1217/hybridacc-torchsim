//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   ClusterDataFabric
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Dual-ingress AXI4-Lite to per-cluster AXI4-Lite fabric.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module ClusterDataFabric import core_pkg::*; #(
    parameter int unsigned NUM_CLUSTERS = 1
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
    localparam int unsigned CLUSTER_ID_WIDTH = $clog2(NUM_CLUSTERS > 1 ? NUM_CLUSTERS : 2);

    logic        wr_active_reg;
    logic        wr_issue_reg;
    fabric_owner_e wr_owner_reg;
    logic [CLUSTER_ID_WIDTH-1:0] wr_cluster_reg;
    logic [31:0] wr_local_addr_reg;
    logic [CL_AXI_DATA_WIDTH-1:0] wr_data_reg;
    logic [CL_AXI_DATA_WIDTH/8-1:0] wr_strb_reg;
    logic        wr_b_valid_reg;
    logic [1:0]  wr_b_resp_reg;

    logic        rd_active_reg;
    logic        rd_issue_reg;
    fabric_owner_e rd_owner_reg;
    logic [CLUSTER_ID_WIDTH-1:0] rd_cluster_reg;
    logic [31:0] rd_local_addr_reg;
    logic        rd_r_valid_reg;
    logic [CL_AXI_DATA_WIDTH-1:0] rd_r_data_reg;
    logic [1:0]  rd_r_resp_reg;

    function automatic logic [CLUSTER_ID_WIDTH-1:0] decode_cluster_id(input logic [CLUSTER_ID_WIDTH-1:0] cluster_id);
        return cluster_id;
    endfunction

    function automatic logic [31:0] decode_local_addr(input logic [23:0] local_addr);
        return {8'h0, local_addr};
    endfunction

    function automatic logic decode_high_addr_ok(input logic [6:0] high_addr);
        return high_addr == 7'h00;
    endfunction

    always_comb begin
        s_dma_axi_aw_ready_o = !wr_active_reg && !wr_b_valid_reg && s_dma_axi_w_valid_i;
        s_dma_axi_w_ready_o  = !wr_active_reg && !wr_b_valid_reg && s_dma_axi_aw_valid_i;
        s_nlu_axi_aw_ready_o = !wr_active_reg && !wr_b_valid_reg && !(s_dma_axi_aw_valid_i && s_dma_axi_w_valid_i) && s_nlu_axi_w_valid_i;
        s_nlu_axi_w_ready_o  = !wr_active_reg && !wr_b_valid_reg && !(s_dma_axi_aw_valid_i && s_dma_axi_w_valid_i) && s_nlu_axi_aw_valid_i;
        s_dma_axi_ar_ready_o = !rd_active_reg && !rd_r_valid_reg;
        s_nlu_axi_ar_ready_o = !rd_active_reg && !rd_r_valid_reg && !s_dma_axi_ar_valid_i;

        s_dma_axi_b_valid_o = wr_b_valid_reg && (wr_owner_reg == OWNER_DMA);
        s_nlu_axi_b_valid_o = wr_b_valid_reg && (wr_owner_reg == OWNER_NLU);
        s_dma_axi_b_resp_o  = wr_b_resp_reg;
        s_nlu_axi_b_resp_o  = wr_b_resp_reg;

        s_dma_axi_r_valid_o = rd_r_valid_reg && (rd_owner_reg == OWNER_DMA);
        s_nlu_axi_r_valid_o = rd_r_valid_reg && (rd_owner_reg == OWNER_NLU);
        s_dma_axi_r_data_o  = rd_r_data_reg;
        s_nlu_axi_r_data_o  = rd_r_data_reg;
        s_dma_axi_r_resp_o  = rd_r_resp_reg;
        s_nlu_axi_r_resp_o  = rd_r_resp_reg;

        for (int unsigned idx = 0; idx < NUM_CLUSTERS; idx++) begin
            m_cl_data_aw_valid_o[idx] = 1'b0;
            m_cl_data_aw_addr_o[idx]  = wr_local_addr_reg;
            m_cl_data_w_valid_o[idx]  = 1'b0;
            m_cl_data_w_data_o[idx]   = wr_data_reg;
            m_cl_data_w_strb_o[idx]   = wr_strb_reg;
            m_cl_data_b_ready_o[idx]  = wr_active_reg && (wr_cluster_reg == idx);
            m_cl_data_ar_valid_o[idx] = 1'b0;
            m_cl_data_ar_addr_o[idx]  = rd_local_addr_reg;
            m_cl_data_r_ready_o[idx]  = rd_active_reg && (rd_cluster_reg == idx);
        end

        if (wr_issue_reg && (wr_cluster_reg < NUM_CLUSTERS)) begin
            m_cl_data_aw_valid_o[wr_cluster_reg] = 1'b1;
            m_cl_data_w_valid_o[wr_cluster_reg]  = 1'b1;
        end
        if (rd_issue_reg && (rd_cluster_reg < NUM_CLUSTERS)) begin
            m_cl_data_ar_valid_o[rd_cluster_reg] = 1'b1;
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            wr_active_reg <= 1'b0;
            wr_issue_reg <= 1'b0;
            wr_owner_reg <= OWNER_DMA;
            wr_cluster_reg <= '0;
            wr_local_addr_reg <= 32'h0;
            wr_data_reg <= '0;
            wr_strb_reg <= '0;
            wr_b_valid_reg <= 1'b0;
            wr_b_resp_reg <= 2'b00;

            rd_active_reg <= 1'b0;
            rd_issue_reg <= 1'b0;
            rd_owner_reg <= OWNER_DMA;
            rd_cluster_reg <= '0;
            rd_local_addr_reg <= 32'h0;
            rd_r_valid_reg <= 1'b0;
            rd_r_data_reg <= '0;
            rd_r_resp_reg <= 2'b00;
        end else begin
            if (!wr_active_reg && !wr_b_valid_reg) begin
                if (s_dma_axi_aw_valid_i && s_dma_axi_w_valid_i) begin
                    wr_owner_reg <= OWNER_DMA;
                    wr_cluster_reg <= decode_cluster_id(s_dma_axi_aw_addr_i[24 +: CLUSTER_ID_WIDTH]);
                    wr_local_addr_reg <= decode_local_addr(s_dma_axi_aw_addr_i[23:0]);
                    wr_data_reg <= s_dma_axi_w_data_i;
                    wr_strb_reg <= s_dma_axi_w_strb_i;
                    if (decode_high_addr_ok(s_dma_axi_aw_addr_i[31:25])
                        && (decode_cluster_id(s_dma_axi_aw_addr_i[24 +: CLUSTER_ID_WIDTH]) < NUM_CLUSTERS)) begin
                        wr_active_reg <= 1'b1;
                        wr_issue_reg <= 1'b1;
                    end else begin
                        wr_active_reg <= 1'b0;
                        wr_issue_reg <= 1'b0;
                        wr_b_valid_reg <= 1'b1;
                        wr_b_resp_reg <= 2'b10;
                    end
                end else if (s_nlu_axi_aw_valid_i && s_nlu_axi_w_valid_i) begin
                    wr_owner_reg <= OWNER_NLU;
                    wr_cluster_reg <= decode_cluster_id(s_nlu_axi_aw_addr_i[24 +: CLUSTER_ID_WIDTH]);
                    wr_local_addr_reg <= decode_local_addr(s_nlu_axi_aw_addr_i[23:0]);
                    wr_data_reg <= s_nlu_axi_w_data_i;
                    wr_strb_reg <= s_nlu_axi_w_strb_i;
                    if (decode_high_addr_ok(s_nlu_axi_aw_addr_i[31:25])
                        && (decode_cluster_id(s_nlu_axi_aw_addr_i[24 +: CLUSTER_ID_WIDTH]) < NUM_CLUSTERS)) begin
                        wr_active_reg <= 1'b1;
                        wr_issue_reg <= 1'b1;
                    end else begin
                        wr_active_reg <= 1'b0;
                        wr_issue_reg <= 1'b0;
                        wr_b_valid_reg <= 1'b1;
                        wr_b_resp_reg <= 2'b10;
                    end
                end
            end

            if (wr_issue_reg && (wr_cluster_reg < NUM_CLUSTERS) && m_cl_data_aw_ready_i[wr_cluster_reg] && m_cl_data_w_ready_i[wr_cluster_reg]) begin
                wr_issue_reg <= 1'b0;
            end

            if (wr_active_reg && !wr_issue_reg && (wr_cluster_reg < NUM_CLUSTERS) && m_cl_data_b_valid_i[wr_cluster_reg]) begin
                wr_b_valid_reg <= 1'b1;
                wr_b_resp_reg <= m_cl_data_b_resp_i[wr_cluster_reg];
                wr_active_reg <= 1'b0;
            end

            if (wr_b_valid_reg) begin
                if (((wr_owner_reg == OWNER_DMA) && s_dma_axi_b_ready_i) || ((wr_owner_reg == OWNER_NLU) && s_nlu_axi_b_ready_i)) begin
                    wr_b_valid_reg <= 1'b0;
                end
            end

            if (!rd_active_reg && !rd_r_valid_reg) begin
                if (s_dma_axi_ar_valid_i) begin
                    rd_owner_reg <= OWNER_DMA;
                    rd_cluster_reg <= decode_cluster_id(s_dma_axi_ar_addr_i[24 +: CLUSTER_ID_WIDTH]);
                    rd_local_addr_reg <= decode_local_addr(s_dma_axi_ar_addr_i[23:0]);
                    if (decode_high_addr_ok(s_dma_axi_ar_addr_i[31:25])
                        && (decode_cluster_id(s_dma_axi_ar_addr_i[24 +: CLUSTER_ID_WIDTH]) < NUM_CLUSTERS)) begin
                        rd_active_reg <= 1'b1;
                        rd_issue_reg <= 1'b1;
                    end else begin
                        rd_active_reg <= 1'b0;
                        rd_issue_reg <= 1'b0;
                        rd_r_valid_reg <= 1'b1;
                        rd_r_data_reg <= '0;
                        rd_r_resp_reg <= 2'b10;
                    end
                end else if (s_nlu_axi_ar_valid_i) begin
                    rd_owner_reg <= OWNER_NLU;
                    rd_cluster_reg <= decode_cluster_id(s_nlu_axi_ar_addr_i[24 +: CLUSTER_ID_WIDTH]);
                    rd_local_addr_reg <= decode_local_addr(s_nlu_axi_ar_addr_i[23:0]);
                    if (decode_high_addr_ok(s_nlu_axi_ar_addr_i[31:25])
                        && (decode_cluster_id(s_nlu_axi_ar_addr_i[24 +: CLUSTER_ID_WIDTH]) < NUM_CLUSTERS)) begin
                        rd_active_reg <= 1'b1;
                        rd_issue_reg <= 1'b1;
                    end else begin
                        rd_active_reg <= 1'b0;
                        rd_issue_reg <= 1'b0;
                        rd_r_valid_reg <= 1'b1;
                        rd_r_data_reg <= '0;
                        rd_r_resp_reg <= 2'b10;
                    end
                end
            end

            if (rd_issue_reg && (rd_cluster_reg < NUM_CLUSTERS) && m_cl_data_ar_ready_i[rd_cluster_reg]) begin
                rd_issue_reg <= 1'b0;
            end

            if (rd_active_reg && !rd_issue_reg && (rd_cluster_reg < NUM_CLUSTERS) && m_cl_data_r_valid_i[rd_cluster_reg]) begin
                rd_r_valid_reg <= 1'b1;
                rd_r_data_reg <= m_cl_data_r_data_i[rd_cluster_reg];
                rd_r_resp_reg <= m_cl_data_r_resp_i[rd_cluster_reg];
                rd_active_reg <= 1'b0;
            end

            if (rd_r_valid_reg) begin
                if (((rd_owner_reg == OWNER_DMA) && s_dma_axi_r_ready_i) || ((rd_owner_reg == OWNER_NLU) && s_nlu_axi_r_ready_i)) begin
                    rd_r_valid_reg <= 1'b0;
                end
            end
        end
    end

endmodule