//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   DmaEngine
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Minimal DMA engine baseline for CoreController.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module DmaEngine (
    input  logic clk,
    input  logic reset_n,
    input  logic        mmio_req_valid_i,
    input  logic        mmio_req_write_i,
    input  logic [31:0] mmio_req_addr_i,
    input  logic [31:0] mmio_req_wdata_i,
    output logic        mmio_resp_valid_o,
    output logic [31:0] mmio_resp_rdata_o,
    output logic        m_mem_axi_aw_valid_o,
    input  logic        m_mem_axi_aw_ready_i,
    output logic [31:0] m_mem_axi_aw_addr_o,
    output logic [7:0]  m_mem_axi_aw_len_o,
    output logic        m_mem_axi_w_valid_o,
    input  logic        m_mem_axi_w_ready_i,
    output logic [MEM_AXI_DATA_WIDTH-1:0] m_mem_axi_w_data_o,
    output logic [MEM_AXI_DATA_WIDTH/8-1:0] m_mem_axi_w_strb_o,
    output logic        m_mem_axi_w_last_o,
    input  logic        m_mem_axi_b_valid_i,
    output logic        m_mem_axi_b_ready_o,
    input  logic [1:0]  m_mem_axi_b_resp_i,
    output logic        m_mem_axi_ar_valid_o,
    input  logic        m_mem_axi_ar_ready_i,
    output logic [31:0] m_mem_axi_ar_addr_o,
    output logic [7:0]  m_mem_axi_ar_len_o,
    input  logic        m_mem_axi_r_valid_i,
    output logic        m_mem_axi_r_ready_o,
    input  logic [MEM_AXI_DATA_WIDTH-1:0] m_mem_axi_r_data_i,
    input  logic [1:0]  m_mem_axi_r_resp_i,
    input  logic        m_mem_axi_r_last_i,
    output logic        m_cl_axi_aw_valid_o,
    input  logic        m_cl_axi_aw_ready_i,
    output logic [31:0] m_cl_axi_aw_addr_o,
    output logic        m_cl_axi_w_valid_o,
    input  logic        m_cl_axi_w_ready_i,
    output logic [CL_AXI_DATA_WIDTH-1:0] m_cl_axi_w_data_o,
    output logic [CL_AXI_DATA_WIDTH/8-1:0] m_cl_axi_w_strb_o,
    input  logic        m_cl_axi_b_valid_i,
    output logic        m_cl_axi_b_ready_o,
    input  logic [1:0]  m_cl_axi_b_resp_i,
    output logic        m_cl_axi_ar_valid_o,
    input  logic        m_cl_axi_ar_ready_i,
    output logic [31:0] m_cl_axi_ar_addr_o,
    input  logic        m_cl_axi_r_valid_i,
    output logic        m_cl_axi_r_ready_o,
    input  logic [CL_AXI_DATA_WIDTH-1:0] m_cl_axi_r_data_i,
    input  logic [1:0]  m_cl_axi_r_resp_i,
    output logic        dma_irq_o
);
    typedef enum logic [2:0] {
        DMA_ST_IDLE,
        DMA_ST_READ_REQ,
        DMA_ST_READ_WAIT,
        DMA_ST_WRITE_REQ,
        DMA_ST_WRITE_WAIT,
        DMA_ST_DONE,
        DMA_ST_ERROR
    } dma_state_e;

    dma_state_e state_reg;

    logic [31:0] src_kind_reg;
    logic [31:0] dst_kind_reg;
    logic [31:0] src_addr_lo_reg;
    logic [31:0] src_addr_hi_reg;
    logic [31:0] dst_addr_lo_reg;
    logic [31:0] dst_addr_hi_reg;
    logic [31:0] src_cluster_id_reg;
    logic [31:0] dst_cluster_id_reg;
    logic [31:0] count_d0_reg;
    logic [31:0] count_d1_reg;
    logic [31:0] count_d2_reg;
    logic [31:0] count_d3_reg;
    logic [31:0] src_stride_d0_reg;
    logic [31:0] src_stride_d1_reg;
    logic [31:0] src_stride_d2_reg;
    logic [31:0] src_stride_d3_reg;
    logic [31:0] dst_stride_d0_reg;
    logic [31:0] dst_stride_d1_reg;
    logic [31:0] dst_stride_d2_reg;
    logic [31:0] dst_stride_d3_reg;
    logic [31:0] cmd_tag_reg;
    logic [31:0] done_tag_reg;
    logic [31:0] err_code_reg;
    logic [31:0] err_info_reg;
    logic [31:0] ctrl_reg;

    logic [31:0] current_src_addr_reg;
    logic [31:0] current_dst_addr_reg;
    logic [31:0] remaining_beats_reg;
    logic [63:0] read_data_reg;
    logic [1:0]  read_resp_reg;

    logic mem_aw_sent_reg;
    logic mem_w_sent_reg;
    logic cl_aw_sent_reg;
    logic cl_w_sent_reg;
    logic irq_pulse_reg;

    function automatic logic [31:0] encode_cluster_fabric_addr(input logic [31:0] cluster_id, input logic [31:0] local_addr);
        return {cluster_id[7:0], local_addr[23:0]};
    endfunction

    function automatic logic [31:0] read_mmio(input logic [31:0] off);
        logic [31:0] r;
        r = 32'h0;
        unique case (off)
            DMA_CAP0:           r = 32'h0000_0001;
            DMA_STATUS: begin
                r[0] = (state_reg == DMA_ST_IDLE) || (state_reg == DMA_ST_DONE) || (state_reg == DMA_ST_ERROR);
                r[1] = (state_reg == DMA_ST_DONE);
                r[2] = (state_reg == DMA_ST_ERROR);
            end
            DMA_CTRL:           r = ctrl_reg;
            DMA_SRC_KIND:       r = src_kind_reg;
            DMA_DST_KIND:       r = dst_kind_reg;
            DMA_SRC_ADDR_LO:    r = src_addr_lo_reg;
            DMA_SRC_ADDR_HI:    r = src_addr_hi_reg;
            DMA_DST_ADDR_LO:    r = dst_addr_lo_reg;
            DMA_DST_ADDR_HI:    r = dst_addr_hi_reg;
            DMA_SRC_CLUSTER_ID: r = src_cluster_id_reg;
            DMA_DST_CLUSTER_ID: r = dst_cluster_id_reg;
            DMA_COUNT_D0:       r = count_d0_reg;
            DMA_COUNT_D1:       r = count_d1_reg;
            DMA_COUNT_D2:       r = count_d2_reg;
            DMA_COUNT_D3:       r = count_d3_reg;
            DMA_SRC_STRIDE_D0:  r = src_stride_d0_reg;
            DMA_SRC_STRIDE_D1:  r = src_stride_d1_reg;
            DMA_SRC_STRIDE_D2:  r = src_stride_d2_reg;
            DMA_SRC_STRIDE_D3:  r = src_stride_d3_reg;
            DMA_DST_STRIDE_D0:  r = dst_stride_d0_reg;
            DMA_DST_STRIDE_D1:  r = dst_stride_d1_reg;
            DMA_DST_STRIDE_D2:  r = dst_stride_d2_reg;
            DMA_DST_STRIDE_D3:  r = dst_stride_d3_reg;
            DMA_CMD_TAG:        r = cmd_tag_reg;
            DMA_DONE_TAG:       r = done_tag_reg;
            DMA_ERR_CODE:       r = err_code_reg;
            DMA_ERR_INFO:       r = err_info_reg;
            DMA_DEBUG_STATE:    r = state_reg;
            default:            r = 32'h0;
        endcase
        return r;
    endfunction

    wire dma_busy_w = (state_reg != DMA_ST_IDLE) && (state_reg != DMA_ST_DONE) && (state_reg != DMA_ST_ERROR);
    wire dma_start_pulse_w = mmio_req_valid_i && mmio_req_write_i && (mmio_req_addr_i == DMA_CTRL) && mmio_req_wdata_i[0] && !dma_busy_w;
    wire dma_clear_done_w = mmio_req_valid_i && mmio_req_write_i && (mmio_req_addr_i == DMA_CTRL) && mmio_req_wdata_i[1];

    assign mmio_resp_valid_o = mmio_req_valid_i;
    assign mmio_resp_rdata_o = mmio_req_write_i ? 32'h0 : read_mmio(mmio_req_addr_i);
    assign dma_irq_o = ctrl_reg[3] && irq_pulse_reg;

    assign m_mem_axi_aw_len_o = 8'h00;
    assign m_mem_axi_ar_len_o = 8'h00;
    assign m_mem_axi_w_last_o = 1'b1;
    assign m_mem_axi_b_ready_o = 1'b1;
    assign m_mem_axi_r_ready_o = 1'b1;
    assign m_cl_axi_b_ready_o = 1'b1;
    assign m_cl_axi_r_ready_o = 1'b1;

    always_comb begin
        m_mem_axi_aw_valid_o = 1'b0;
        m_mem_axi_aw_addr_o  = current_dst_addr_reg;
        m_mem_axi_w_valid_o  = 1'b0;
        m_mem_axi_w_data_o   = read_data_reg;
        m_mem_axi_w_strb_o   = '1;
        m_mem_axi_ar_valid_o = 1'b0;
        m_mem_axi_ar_addr_o  = current_src_addr_reg;
        m_cl_axi_aw_valid_o  = 1'b0;
        m_cl_axi_aw_addr_o   = encode_cluster_fabric_addr(dst_cluster_id_reg, current_dst_addr_reg);
        m_cl_axi_w_valid_o   = 1'b0;
        m_cl_axi_w_data_o    = read_data_reg;
        m_cl_axi_w_strb_o    = '1;
        m_cl_axi_ar_valid_o  = 1'b0;
        m_cl_axi_ar_addr_o   = encode_cluster_fabric_addr(src_cluster_id_reg, current_src_addr_reg);

        unique case (state_reg)
            DMA_ST_READ_REQ: begin
                if (src_kind_reg == DMA_EP_DRAM) begin
                    m_mem_axi_ar_valid_o = 1'b1;
                end else begin
                    m_cl_axi_ar_valid_o = 1'b1;
                end
            end
            DMA_ST_WRITE_REQ: begin
                if (dst_kind_reg == DMA_EP_DRAM) begin
                    m_mem_axi_aw_valid_o = !mem_aw_sent_reg;
                    m_mem_axi_w_valid_o  = !mem_w_sent_reg;
                end else begin
                    m_cl_axi_aw_valid_o = !cl_aw_sent_reg;
                    m_cl_axi_w_valid_o  = !cl_w_sent_reg;
                end
            end
            default: ;
        endcase
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            src_kind_reg <= DMA_EP_DRAM;
            dst_kind_reg <= DMA_EP_CLUSTER_SPM;
            src_addr_lo_reg <= 32'h0;
            src_addr_hi_reg <= 32'h0;
            dst_addr_lo_reg <= 32'h0;
            dst_addr_hi_reg <= 32'h0;
            src_cluster_id_reg <= 32'h0;
            dst_cluster_id_reg <= 32'h0;
            count_d0_reg <= 32'h1;
            count_d1_reg <= 32'h1;
            count_d2_reg <= 32'h1;
            count_d3_reg <= 32'h1;
            src_stride_d0_reg <= 32'd8;
            src_stride_d1_reg <= 32'h0;
            src_stride_d2_reg <= 32'h0;
            src_stride_d3_reg <= 32'h0;
            dst_stride_d0_reg <= 32'd8;
            dst_stride_d1_reg <= 32'h0;
            dst_stride_d2_reg <= 32'h0;
            dst_stride_d3_reg <= 32'h0;
            cmd_tag_reg <= 32'h0;
            done_tag_reg <= 32'h0;
            err_code_reg <= DMA_ERR_NONE;
            err_info_reg <= 32'h0;
            ctrl_reg <= 32'h0;
            current_src_addr_reg <= 32'h0;
            current_dst_addr_reg <= 32'h0;
            remaining_beats_reg <= 32'h0;
            read_data_reg <= 64'h0;
            read_resp_reg <= 2'b00;
            mem_aw_sent_reg <= 1'b0;
            mem_w_sent_reg <= 1'b0;
            cl_aw_sent_reg <= 1'b0;
            cl_w_sent_reg <= 1'b0;
            irq_pulse_reg <= 1'b0;
            state_reg <= DMA_ST_IDLE;
        end else begin
            irq_pulse_reg <= 1'b0;

            if (mmio_req_valid_i && mmio_req_write_i) begin
                unique case (mmio_req_addr_i)
                    DMA_CTRL:           ctrl_reg <= mmio_req_wdata_i;
                    DMA_SRC_KIND:       src_kind_reg <= mmio_req_wdata_i;
                    DMA_DST_KIND:       dst_kind_reg <= mmio_req_wdata_i;
                    DMA_SRC_ADDR_LO:    src_addr_lo_reg <= mmio_req_wdata_i;
                    DMA_SRC_ADDR_HI:    src_addr_hi_reg <= mmio_req_wdata_i;
                    DMA_DST_ADDR_LO:    dst_addr_lo_reg <= mmio_req_wdata_i;
                    DMA_DST_ADDR_HI:    dst_addr_hi_reg <= mmio_req_wdata_i;
                    DMA_SRC_CLUSTER_ID: src_cluster_id_reg <= mmio_req_wdata_i;
                    DMA_DST_CLUSTER_ID: dst_cluster_id_reg <= mmio_req_wdata_i;
                    DMA_COUNT_D0:       count_d0_reg <= mmio_req_wdata_i;
                    DMA_COUNT_D1:       count_d1_reg <= mmio_req_wdata_i;
                    DMA_COUNT_D2:       count_d2_reg <= mmio_req_wdata_i;
                    DMA_COUNT_D3:       count_d3_reg <= mmio_req_wdata_i;
                    DMA_SRC_STRIDE_D0:  src_stride_d0_reg <= mmio_req_wdata_i;
                    DMA_SRC_STRIDE_D1:  src_stride_d1_reg <= mmio_req_wdata_i;
                    DMA_SRC_STRIDE_D2:  src_stride_d2_reg <= mmio_req_wdata_i;
                    DMA_SRC_STRIDE_D3:  src_stride_d3_reg <= mmio_req_wdata_i;
                    DMA_DST_STRIDE_D0:  dst_stride_d0_reg <= mmio_req_wdata_i;
                    DMA_DST_STRIDE_D1:  dst_stride_d1_reg <= mmio_req_wdata_i;
                    DMA_DST_STRIDE_D2:  dst_stride_d2_reg <= mmio_req_wdata_i;
                    DMA_DST_STRIDE_D3:  dst_stride_d3_reg <= mmio_req_wdata_i;
                    DMA_CMD_TAG:        cmd_tag_reg <= mmio_req_wdata_i;
                    DMA_DONE_TAG:       done_tag_reg <= mmio_req_wdata_i;
                    DMA_ERR_CODE:       if (mmio_req_wdata_i == 32'h0) err_code_reg <= DMA_ERR_NONE;
                    DMA_ERR_INFO:       if (mmio_req_wdata_i == 32'h0) err_info_reg <= 32'h0;
                    default: ;
                endcase
            end

            if (dma_clear_done_w && ((state_reg == DMA_ST_DONE) || (state_reg == DMA_ST_ERROR))) begin
                state_reg <= DMA_ST_IDLE;
                err_code_reg <= DMA_ERR_NONE;
                err_info_reg <= 32'h0;
            end

            unique case (state_reg)
                DMA_ST_IDLE: begin
                    mem_aw_sent_reg <= 1'b0;
                    mem_w_sent_reg <= 1'b0;
                    cl_aw_sent_reg <= 1'b0;
                    cl_w_sent_reg <= 1'b0;
                    if (dma_start_pulse_w) begin
                        if ((count_d0_reg == 32'h0) || !((src_kind_reg == DMA_EP_DRAM) || (src_kind_reg == DMA_EP_CLUSTER_SPM)) || !((dst_kind_reg == DMA_EP_DRAM) || (dst_kind_reg == DMA_EP_CLUSTER_SPM))) begin
                            err_code_reg <= (count_d0_reg == 32'h0) ? DMA_ERR_ZERO_LENGTH : DMA_ERR_BAD_ENDPOINT;
                            err_info_reg <= 32'h0;
                            irq_pulse_reg <= mmio_req_wdata_i[3];
                            state_reg <= DMA_ST_ERROR;
                        end else begin
                            current_src_addr_reg <= src_addr_lo_reg;
                            current_dst_addr_reg <= dst_addr_lo_reg;
                            remaining_beats_reg <= count_d0_reg;
                            done_tag_reg <= 32'h0;
                            err_code_reg <= DMA_ERR_NONE;
                            err_info_reg <= 32'h0;
                            state_reg <= DMA_ST_READ_REQ;
                        end
                    end
                end
                DMA_ST_READ_REQ: begin
                    if ((src_kind_reg == DMA_EP_DRAM) && m_mem_axi_ar_ready_i) begin
                        state_reg <= DMA_ST_READ_WAIT;
                    end else if ((src_kind_reg == DMA_EP_CLUSTER_SPM) && m_cl_axi_ar_ready_i) begin
                        state_reg <= DMA_ST_READ_WAIT;
                    end
                end
                DMA_ST_READ_WAIT: begin
                    if ((src_kind_reg == DMA_EP_DRAM) && m_mem_axi_r_valid_i) begin
                        read_data_reg <= m_mem_axi_r_data_i;
                        read_resp_reg <= m_mem_axi_r_resp_i;
                        if (m_mem_axi_r_resp_i != 2'b00) begin
                            err_code_reg <= DMA_ERR_DRAM_AXI;
                            state_reg <= DMA_ST_ERROR;
                        end else begin
                            mem_aw_sent_reg <= 1'b0;
                            mem_w_sent_reg <= 1'b0;
                            cl_aw_sent_reg <= 1'b0;
                            cl_w_sent_reg <= 1'b0;
                            state_reg <= DMA_ST_WRITE_REQ;
                        end
                    end else if ((src_kind_reg == DMA_EP_CLUSTER_SPM) && m_cl_axi_r_valid_i) begin
                        read_data_reg <= m_cl_axi_r_data_i;
                        read_resp_reg <= m_cl_axi_r_resp_i;
                        if (m_cl_axi_r_resp_i != 2'b00) begin
                            err_code_reg <= DMA_ERR_CLUSTER_RESP;
                            state_reg <= DMA_ST_ERROR;
                        end else begin
                            mem_aw_sent_reg <= 1'b0;
                            mem_w_sent_reg <= 1'b0;
                            cl_aw_sent_reg <= 1'b0;
                            cl_w_sent_reg <= 1'b0;
                            state_reg <= DMA_ST_WRITE_REQ;
                        end
                    end
                end
                DMA_ST_WRITE_REQ: begin
                    if (dst_kind_reg == DMA_EP_DRAM) begin
                        if (m_mem_axi_aw_ready_i) mem_aw_sent_reg <= 1'b1;
                        if (m_mem_axi_w_ready_i)  mem_w_sent_reg <= 1'b1;
                        if ((m_mem_axi_aw_ready_i || mem_aw_sent_reg) && (m_mem_axi_w_ready_i || mem_w_sent_reg)) begin
                            state_reg <= DMA_ST_WRITE_WAIT;
                        end
                    end else begin
                        if (m_cl_axi_aw_ready_i) cl_aw_sent_reg <= 1'b1;
                        if (m_cl_axi_w_ready_i)  cl_w_sent_reg <= 1'b1;
                        if ((m_cl_axi_aw_ready_i || cl_aw_sent_reg) && (m_cl_axi_w_ready_i || cl_w_sent_reg)) begin
                            state_reg <= DMA_ST_WRITE_WAIT;
                        end
                    end
                end
                DMA_ST_WRITE_WAIT: begin
                    if ((dst_kind_reg == DMA_EP_DRAM) && m_mem_axi_b_valid_i) begin
                        if (m_mem_axi_b_resp_i != 2'b00) begin
                            err_code_reg <= DMA_ERR_DRAM_AXI;
                            irq_pulse_reg <= ctrl_reg[3];
                            state_reg <= DMA_ST_ERROR;
                        end else if (remaining_beats_reg <= 32'd1) begin
                            done_tag_reg <= cmd_tag_reg;
                            irq_pulse_reg <= ctrl_reg[3];
                            state_reg <= DMA_ST_DONE;
                        end else begin
                            remaining_beats_reg <= remaining_beats_reg - 32'd1;
                            current_src_addr_reg <= current_src_addr_reg + src_stride_d0_reg;
                            current_dst_addr_reg <= current_dst_addr_reg + dst_stride_d0_reg;
                            mem_aw_sent_reg <= 1'b0;
                            mem_w_sent_reg <= 1'b0;
                            state_reg <= DMA_ST_READ_REQ;
                        end
                    end else if ((dst_kind_reg == DMA_EP_CLUSTER_SPM) && m_cl_axi_b_valid_i) begin
                        if (m_cl_axi_b_resp_i != 2'b00) begin
                            err_code_reg <= DMA_ERR_CLUSTER_RESP;
                            irq_pulse_reg <= ctrl_reg[3];
                            state_reg <= DMA_ST_ERROR;
                        end else if (remaining_beats_reg <= 32'd1) begin
                            done_tag_reg <= cmd_tag_reg;
                            irq_pulse_reg <= ctrl_reg[3];
                            state_reg <= DMA_ST_DONE;
                        end else begin
                            remaining_beats_reg <= remaining_beats_reg - 32'd1;
                            current_src_addr_reg <= current_src_addr_reg + src_stride_d0_reg;
                            current_dst_addr_reg <= current_dst_addr_reg + dst_stride_d0_reg;
                            cl_aw_sent_reg <= 1'b0;
                            cl_w_sent_reg <= 1'b0;
                            state_reg <= DMA_ST_READ_REQ;
                        end
                    end
                end
                DMA_ST_DONE: begin
                    if (dma_clear_done_w) begin
                        state_reg <= DMA_ST_IDLE;
                    end else if (dma_start_pulse_w) begin
                        current_src_addr_reg <= src_addr_lo_reg;
                        current_dst_addr_reg <= dst_addr_lo_reg;
                        remaining_beats_reg <= count_d0_reg;
                        done_tag_reg <= 32'h0;
                        err_code_reg <= DMA_ERR_NONE;
                        err_info_reg <= 32'h0;
                        mem_aw_sent_reg <= 1'b0;
                        mem_w_sent_reg <= 1'b0;
                        cl_aw_sent_reg <= 1'b0;
                        cl_w_sent_reg <= 1'b0;
                        if ((count_d0_reg == 32'h0) || !((src_kind_reg == DMA_EP_DRAM) || (src_kind_reg == DMA_EP_CLUSTER_SPM)) || !((dst_kind_reg == DMA_EP_DRAM) || (dst_kind_reg == DMA_EP_CLUSTER_SPM))) begin
                            err_code_reg <= (count_d0_reg == 32'h0) ? DMA_ERR_ZERO_LENGTH : DMA_ERR_BAD_ENDPOINT;
                            irq_pulse_reg <= mmio_req_wdata_i[3];
                            state_reg <= DMA_ST_ERROR;
                        end else begin
                            state_reg <= DMA_ST_READ_REQ;
                        end
                    end
                end
                DMA_ST_ERROR: begin
                    if (dma_clear_done_w) begin
                        state_reg <= DMA_ST_IDLE;
                        err_code_reg <= DMA_ERR_NONE;
                        err_info_reg <= 32'h0;
                    end else if (dma_start_pulse_w) begin
                        current_src_addr_reg <= src_addr_lo_reg;
                        current_dst_addr_reg <= dst_addr_lo_reg;
                        remaining_beats_reg <= count_d0_reg;
                        done_tag_reg <= 32'h0;
                        err_code_reg <= DMA_ERR_NONE;
                        err_info_reg <= 32'h0;
                        mem_aw_sent_reg <= 1'b0;
                        mem_w_sent_reg <= 1'b0;
                        cl_aw_sent_reg <= 1'b0;
                        cl_w_sent_reg <= 1'b0;
                        if ((count_d0_reg == 32'h0) || !((src_kind_reg == DMA_EP_DRAM) || (src_kind_reg == DMA_EP_CLUSTER_SPM)) || !((dst_kind_reg == DMA_EP_DRAM) || (dst_kind_reg == DMA_EP_CLUSTER_SPM))) begin
                            err_code_reg <= (count_d0_reg == 32'h0) ? DMA_ERR_ZERO_LENGTH : DMA_ERR_BAD_ENDPOINT;
                            irq_pulse_reg <= mmio_req_wdata_i[3];
                            state_reg <= DMA_ST_ERROR;
                        end else begin
                            state_reg <= DMA_ST_READ_REQ;
                        end
                    end
                end
                default: state_reg <= DMA_ST_IDLE;
            endcase
        end
    end

endmodule