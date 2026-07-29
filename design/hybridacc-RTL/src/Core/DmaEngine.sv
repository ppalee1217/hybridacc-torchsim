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
module DmaEngine import core_pkg::*; (
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
    localparam logic [31:0] DMA_DST_KIND_RESET   = DMA_EP_CLUSTER_SPM;
    localparam logic [31:0] DMA_COUNT_D_RESET    = 32'h1;
    localparam logic [31:0] DMA_STRIDE_D0_RESET  = 32'd8;
    localparam int unsigned MAX_OUTSTANDING      = 16;
    localparam int unsigned FIFO_DEPTH           = 16;
    localparam int unsigned FIFO_PTR_WIDTH       = $clog2(FIFO_DEPTH);
    localparam int unsigned FIFO_COUNT_WIDTH     = $clog2(FIFO_DEPTH + 1);
    localparam int unsigned OUTSTANDING_WIDTH    = $clog2(MAX_OUTSTANDING + 1);

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
    logic [31:0] current_src_row_base_reg;
    logic [31:0] current_dst_row_base_reg;
    logic [31:0] current_src_plane_base_reg;
    logic [31:0] current_dst_plane_base_reg;
    logic [31:0] current_src_volume_base_reg;
    logic [31:0] current_dst_volume_base_reg;
    logic [31:0] src_index_d0_reg;
    logic [31:0] src_index_d1_reg;
    logic [31:0] src_index_d2_reg;
    logic [31:0] src_index_d3_reg;
    logic [31:0] dst_index_d0_reg;
    logic [31:0] dst_index_d1_reg;
    logic [31:0] dst_index_d2_reg;
    logic [31:0] dst_index_d3_reg;
    logic [31:0] total_beats_reg;
    logic [31:0] remaining_beats_reg;
    logic [31:0] read_issued_reg;
    logic [31:0] write_issued_reg;
    logic [31:0] write_retired_reg;

    logic [MEM_AXI_DATA_WIDTH-1:0] read_data_fifo_reg [0:FIFO_DEPTH-1];
    logic [FIFO_PTR_WIDTH-1:0] read_fifo_wr_ptr_reg;
    logic [FIFO_PTR_WIDTH-1:0] read_fifo_rd_ptr_reg;
    logic [FIFO_COUNT_WIDTH-1:0] read_fifo_count_reg;

    logic [31:0] read_addr_fifo_reg [0:MAX_OUTSTANDING-1];
    logic [FIFO_PTR_WIDTH-1:0] read_addr_wr_ptr_reg;
    logic [FIFO_PTR_WIDTH-1:0] read_addr_rd_ptr_reg;
    logic [OUTSTANDING_WIDTH-1:0] read_outstanding_reg;

    logic [MEM_AXI_DATA_WIDTH-1:0] write_data_reg;
    logic [31:0] write_addr_reg;
    logic [31:0] write_addr_fifo_reg [0:MAX_OUTSTANDING-1];
    logic [FIFO_PTR_WIDTH-1:0] write_addr_wr_ptr_reg;
    logic [FIFO_PTR_WIDTH-1:0] write_addr_rd_ptr_reg;
    logic write_aw_valid_reg;
    logic write_w_valid_reg;
    logic [OUTSTANDING_WIDTH-1:0] write_outstanding_reg;
    logic error_irq_reg;
    logic irq_pulse_reg;
    logic reset_init_done_reg;
    logic [31:0] read_mmio_rdata_w;

    function automatic logic [31:0] encode_cluster_fabric_addr(input logic [7:0] cluster_id, input logic [23:0] local_addr);
        return {cluster_id, local_addr};
    endfunction

    function automatic logic [31:0] effective_count(input logic [31:0] count);
        return (count == 32'h0) ? 32'h1 : count;
    endfunction

    wire dma_busy_w = (state_reg != DMA_ST_IDLE) && (state_reg != DMA_ST_DONE) && (state_reg != DMA_ST_ERROR);
    wire dma_start_pulse_w = mmio_req_valid_i && mmio_req_write_i && (mmio_req_addr_i == DMA_CTRL) && mmio_req_wdata_i[0] && !dma_busy_w;
    wire dma_clear_done_w = mmio_req_valid_i && mmio_req_write_i && (mmio_req_addr_i == DMA_CTRL) && mmio_req_wdata_i[1];
    wire dma_active_w = (state_reg == DMA_ST_READ_REQ);
    wire dma_error_drain_w = (state_reg == DMA_ST_READ_WAIT);
    wire [31:0] dma_total_beats_w =
        count_d0_reg *
        effective_count(count_d1_reg) *
        effective_count(count_d2_reg) *
        effective_count(count_d3_reg);

    wire [FIFO_COUNT_WIDTH:0] read_reserved_slots_w =
        {1'b0, read_fifo_count_reg} +
        {1'b0, read_outstanding_reg};
    wire read_issue_allowed_w =
        dma_active_w &&
        (read_issued_reg < total_beats_reg) &&
        (read_outstanding_reg < MAX_OUTSTANDING) &&
        (read_reserved_slots_w < FIFO_DEPTH);
    wire read_issue_fire_w =
        read_issue_allowed_w &&
        (((src_kind_reg == DMA_EP_DRAM) && m_mem_axi_ar_ready_i) ||
         ((src_kind_reg == DMA_EP_CLUSTER_SPM) && m_cl_axi_ar_ready_i));

    wire read_response_ready_w =
        (read_outstanding_reg != 0) &&
        (dma_error_drain_w || (dma_active_w && (read_fifo_count_reg < FIFO_DEPTH)));
    wire read_response_fire_w =
        read_response_ready_w &&
        (((src_kind_reg == DMA_EP_DRAM) && m_mem_axi_r_valid_i) ||
         ((src_kind_reg == DMA_EP_CLUSTER_SPM) && m_cl_axi_r_valid_i));
    wire read_response_error_w =
        dma_active_w &&
        read_response_fire_w &&
        (((src_kind_reg == DMA_EP_DRAM) &&
          ((m_mem_axi_r_resp_i != 2'b00) || !m_mem_axi_r_last_i)) ||
         ((src_kind_reg == DMA_EP_CLUSTER_SPM) &&
          (m_cl_axi_r_resp_i != 2'b00)));
    wire [MEM_AXI_DATA_WIDTH-1:0] read_response_data_w =
        (src_kind_reg == DMA_EP_DRAM) ? m_mem_axi_r_data_i : m_cl_axi_r_data_i;

    wire write_path_enabled_w = dma_active_w || dma_error_drain_w;
    wire write_aw_ready_w =
        (dst_kind_reg == DMA_EP_DRAM) ? m_mem_axi_aw_ready_i : m_cl_axi_aw_ready_i;
    wire write_w_ready_w =
        (dst_kind_reg == DMA_EP_DRAM) ? m_mem_axi_w_ready_i : m_cl_axi_w_ready_i;
    wire write_response_valid_w =
        (dst_kind_reg == DMA_EP_DRAM) ? m_mem_axi_b_valid_i : m_cl_axi_b_valid_i;
    wire [1:0] write_response_resp_w =
        (dst_kind_reg == DMA_EP_DRAM) ? m_mem_axi_b_resp_i : m_cl_axi_b_resp_i;
    wire write_aw_fire_w =
        write_path_enabled_w && write_aw_valid_reg && write_aw_ready_w;
    wire write_w_fire_w =
        write_path_enabled_w && write_w_valid_reg && write_w_ready_w;
    wire write_response_ready_w =
        write_path_enabled_w && (write_outstanding_reg != 0);
    wire write_response_fire_w =
        write_response_ready_w && write_response_valid_w;
    wire write_response_error_w =
        dma_active_w && write_response_fire_w && (write_response_resp_w != 2'b00);
    wire pipeline_error_w = read_response_error_w || write_response_error_w;
    wire write_slot_free_next_w =
        (!write_aw_valid_reg || write_aw_fire_w) &&
        (!write_w_valid_reg || write_w_fire_w);
    wire write_load_w =
        dma_active_w &&
        !pipeline_error_w &&
        write_slot_free_next_w &&
        (read_fifo_count_reg != 0) &&
        (write_issued_reg < total_beats_reg) &&
        ((write_outstanding_reg < MAX_OUTSTANDING) || write_response_fire_w);
    wire read_fifo_push_w =
        dma_active_w && read_response_fire_w && !read_response_error_w;
    wire read_fifo_pop_w = write_load_w;

    assign mmio_resp_valid_o = mmio_req_valid_i;
    assign mmio_resp_rdata_o = mmio_req_write_i ? 32'h0 : read_mmio_rdata_w;
    assign dma_irq_o = ctrl_reg[3] && irq_pulse_reg;

    always_comb begin
        read_mmio_rdata_w = 32'h0;
        unique0 case (mmio_req_addr_i)
            DMA_CAP0:           read_mmio_rdata_w = 32'h0000_0001;
            DMA_STATUS: begin
                read_mmio_rdata_w[0] = (state_reg == DMA_ST_IDLE) || (state_reg == DMA_ST_DONE) || (state_reg == DMA_ST_ERROR);
                read_mmio_rdata_w[1] = (state_reg == DMA_ST_DONE);
                read_mmio_rdata_w[2] = (state_reg == DMA_ST_ERROR);
            end
            DMA_CTRL:           read_mmio_rdata_w = ctrl_reg;
            DMA_SRC_KIND:       read_mmio_rdata_w = src_kind_reg;
            DMA_DST_KIND:       read_mmio_rdata_w = dst_kind_reg;
            DMA_SRC_ADDR_LO:    read_mmio_rdata_w = src_addr_lo_reg;
            DMA_SRC_ADDR_HI:    read_mmio_rdata_w = src_addr_hi_reg;
            DMA_DST_ADDR_LO:    read_mmio_rdata_w = dst_addr_lo_reg;
            DMA_DST_ADDR_HI:    read_mmio_rdata_w = dst_addr_hi_reg;
            DMA_SRC_CLUSTER_ID: read_mmio_rdata_w = src_cluster_id_reg;
            DMA_DST_CLUSTER_ID: read_mmio_rdata_w = dst_cluster_id_reg;
            DMA_COUNT_D0:       read_mmio_rdata_w = count_d0_reg;
            DMA_COUNT_D1:       read_mmio_rdata_w = count_d1_reg;
            DMA_COUNT_D2:       read_mmio_rdata_w = count_d2_reg;
            DMA_COUNT_D3:       read_mmio_rdata_w = count_d3_reg;
            DMA_SRC_STRIDE_D0:  read_mmio_rdata_w = src_stride_d0_reg;
            DMA_SRC_STRIDE_D1:  read_mmio_rdata_w = src_stride_d1_reg;
            DMA_SRC_STRIDE_D2:  read_mmio_rdata_w = src_stride_d2_reg;
            DMA_SRC_STRIDE_D3:  read_mmio_rdata_w = src_stride_d3_reg;
            DMA_DST_STRIDE_D0:  read_mmio_rdata_w = dst_stride_d0_reg;
            DMA_DST_STRIDE_D1:  read_mmio_rdata_w = dst_stride_d1_reg;
            DMA_DST_STRIDE_D2:  read_mmio_rdata_w = dst_stride_d2_reg;
            DMA_DST_STRIDE_D3:  read_mmio_rdata_w = dst_stride_d3_reg;
            DMA_CMD_TAG:        read_mmio_rdata_w = cmd_tag_reg;
            DMA_DONE_TAG:       read_mmio_rdata_w = done_tag_reg;
            DMA_ERR_CODE:       read_mmio_rdata_w = err_code_reg;
            DMA_ERR_INFO:       read_mmio_rdata_w = err_info_reg;
            DMA_DEBUG_STATE:    read_mmio_rdata_w = state_reg;
            default:            read_mmio_rdata_w = 32'h0;
        endcase
    end

    assign m_mem_axi_aw_len_o = 8'h00;
    assign m_mem_axi_ar_len_o = 8'h00;
    assign m_mem_axi_w_last_o = 1'b1;
    assign m_mem_axi_b_ready_o =
        (dst_kind_reg == DMA_EP_DRAM) && write_response_ready_w;
    assign m_mem_axi_r_ready_o =
        (src_kind_reg == DMA_EP_DRAM) && read_response_ready_w;
    assign m_cl_axi_b_ready_o =
        (dst_kind_reg == DMA_EP_CLUSTER_SPM) && write_response_ready_w;
    assign m_cl_axi_r_ready_o =
        (src_kind_reg == DMA_EP_CLUSTER_SPM) && read_response_ready_w;

    always_comb begin
        m_mem_axi_aw_valid_o = 1'b0;
        m_mem_axi_aw_addr_o  = write_addr_reg;
        m_mem_axi_w_valid_o  = 1'b0;
        m_mem_axi_w_data_o   = write_data_reg;
        m_mem_axi_w_strb_o   = '1;
        m_mem_axi_ar_valid_o = 1'b0;
        m_mem_axi_ar_addr_o  = current_src_addr_reg;
        m_cl_axi_aw_valid_o  = 1'b0;
        m_cl_axi_aw_addr_o   = encode_cluster_fabric_addr(dst_cluster_id_reg[7:0], write_addr_reg[23:0]);
        m_cl_axi_w_valid_o   = 1'b0;
        m_cl_axi_w_data_o    = write_data_reg;
        m_cl_axi_w_strb_o    = '1;
        m_cl_axi_ar_valid_o  = 1'b0;
        m_cl_axi_ar_addr_o   = encode_cluster_fabric_addr(src_cluster_id_reg[7:0], current_src_addr_reg[23:0]);

        if (read_issue_allowed_w) begin
            if (src_kind_reg == DMA_EP_DRAM) begin
                m_mem_axi_ar_valid_o = 1'b1;
            end else begin
                m_cl_axi_ar_valid_o = 1'b1;
            end
        end

        if (write_path_enabled_w) begin
            if (dst_kind_reg == DMA_EP_DRAM) begin
                m_mem_axi_aw_valid_o = write_aw_valid_reg;
                m_mem_axi_w_valid_o  = write_w_valid_reg;
            end else begin
                m_cl_axi_aw_valid_o = write_aw_valid_reg;
                m_cl_axi_w_valid_o  = write_w_valid_reg;
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            src_kind_reg <= DMA_EP_DRAM;
            dst_kind_reg <= 32'h0;
            src_addr_lo_reg <= 32'h0;
            src_addr_hi_reg <= 32'h0;
            dst_addr_lo_reg <= 32'h0;
            dst_addr_hi_reg <= 32'h0;
            src_cluster_id_reg <= 32'h0;
            dst_cluster_id_reg <= 32'h0;
            count_d0_reg <= 32'h0;
            count_d1_reg <= 32'h0;
            count_d2_reg <= 32'h0;
            count_d3_reg <= 32'h0;
            src_stride_d0_reg <= 32'h0;
            src_stride_d1_reg <= 32'h0;
            src_stride_d2_reg <= 32'h0;
            src_stride_d3_reg <= 32'h0;
            dst_stride_d0_reg <= 32'h0;
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
            current_src_row_base_reg <= 32'h0;
            current_dst_row_base_reg <= 32'h0;
            current_src_plane_base_reg <= 32'h0;
            current_dst_plane_base_reg <= 32'h0;
            current_src_volume_base_reg <= 32'h0;
            current_dst_volume_base_reg <= 32'h0;
            src_index_d0_reg <= 32'h0;
            src_index_d1_reg <= 32'h0;
            src_index_d2_reg <= 32'h0;
            src_index_d3_reg <= 32'h0;
            dst_index_d0_reg <= 32'h0;
            dst_index_d1_reg <= 32'h0;
            dst_index_d2_reg <= 32'h0;
            dst_index_d3_reg <= 32'h0;
            total_beats_reg <= 32'h0;
            remaining_beats_reg <= 32'h0;
            read_issued_reg <= 32'h0;
            write_issued_reg <= 32'h0;
            write_retired_reg <= 32'h0;
            read_fifo_wr_ptr_reg <= '0;
            read_fifo_rd_ptr_reg <= '0;
            read_fifo_count_reg <= '0;
            read_addr_wr_ptr_reg <= '0;
            read_addr_rd_ptr_reg <= '0;
            read_outstanding_reg <= '0;
            write_data_reg <= '0;
            write_addr_reg <= 32'h0;
            write_addr_wr_ptr_reg <= '0;
            write_addr_rd_ptr_reg <= '0;
            write_aw_valid_reg <= 1'b0;
            write_w_valid_reg <= 1'b0;
            write_outstanding_reg <= '0;
            error_irq_reg <= 1'b0;
            irq_pulse_reg <= 1'b0;
            state_reg <= DMA_ST_IDLE;
            reset_init_done_reg <= 1'b0;
        end else if (!reset_init_done_reg) begin
            dst_kind_reg <= DMA_DST_KIND_RESET;
            count_d0_reg <= DMA_COUNT_D_RESET;
            count_d1_reg <= DMA_COUNT_D_RESET;
            count_d2_reg <= DMA_COUNT_D_RESET;
            count_d3_reg <= DMA_COUNT_D_RESET;
            src_stride_d0_reg <= DMA_STRIDE_D0_RESET;
            dst_stride_d0_reg <= DMA_STRIDE_D0_RESET;
            state_reg <= DMA_ST_IDLE;
            read_fifo_count_reg <= '0;
            read_outstanding_reg <= '0;
            write_aw_valid_reg <= 1'b0;
            write_w_valid_reg <= 1'b0;
            write_outstanding_reg <= '0;
            reset_init_done_reg <= 1'b1;
        end else begin
            irq_pulse_reg <= 1'b0;

            // synopsys translate_off
            if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO"))
                && mmio_req_valid_i && mmio_req_write_i) begin
                $display("[%0t] [TRACE][DMA][MMIO] off=0x%03x data=0x%08x state=%0d",
                         $time,
                         mmio_req_addr_i[11:0],
                         mmio_req_wdata_i,
                         state_reg);
            end
            if (($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME"))
                && dma_start_pulse_w) begin
                $display("[%0t] [TRACE][DMA] submit src_kind=%0d dst_kind=%0d src=0x%08x dst=0x%08x src_cluster=%0d dst_cluster=%0d beats=%0d rows=%0d src_stride={%0d,%0d} dst_stride={%0d,%0d} tag=0x%08x",
                         $time,
                         src_kind_reg,
                         dst_kind_reg,
                         src_addr_lo_reg,
                         dst_addr_lo_reg,
                         src_cluster_id_reg,
                         dst_cluster_id_reg,
                         count_d0_reg,
                         count_d1_reg,
                         src_stride_d0_reg,
                         src_stride_d1_reg,
                         dst_stride_d0_reg,
                         dst_stride_d1_reg,
                         cmd_tag_reg);
            end
            // synopsys translate_on

            if (mmio_req_valid_i && mmio_req_write_i) begin
                unique0 case (mmio_req_addr_i)
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
                    DMA_ERR_CODE: begin
                        if (mmio_req_wdata_i == 32'h0) begin
                            err_code_reg <= DMA_ERR_NONE;
                        end
                    end
                    DMA_ERR_INFO: begin
                        if (mmio_req_wdata_i == 32'h0) begin
                            err_info_reg <= 32'h0;
                        end
                    end
                    default: ;
                endcase
            end

            if (dma_clear_done_w &&
                ((state_reg == DMA_ST_DONE) || (state_reg == DMA_ST_ERROR))) begin
                state_reg <= DMA_ST_IDLE;
                err_code_reg <= DMA_ERR_NONE;
                err_info_reg <= 32'h0;
                read_fifo_count_reg <= '0;
                read_outstanding_reg <= '0;
                write_aw_valid_reg <= 1'b0;
                write_w_valid_reg <= 1'b0;
                write_outstanding_reg <= '0;
            end else if (dma_start_pulse_w) begin
                current_src_addr_reg <= src_addr_lo_reg;
                current_dst_addr_reg <= dst_addr_lo_reg;
                current_src_row_base_reg <= src_addr_lo_reg;
                current_dst_row_base_reg <= dst_addr_lo_reg;
                current_src_plane_base_reg <= src_addr_lo_reg;
                current_dst_plane_base_reg <= dst_addr_lo_reg;
                current_src_volume_base_reg <= src_addr_lo_reg;
                current_dst_volume_base_reg <= dst_addr_lo_reg;
                src_index_d0_reg <= 32'h0;
                src_index_d1_reg <= 32'h0;
                src_index_d2_reg <= 32'h0;
                src_index_d3_reg <= 32'h0;
                dst_index_d0_reg <= 32'h0;
                dst_index_d1_reg <= 32'h0;
                dst_index_d2_reg <= 32'h0;
                dst_index_d3_reg <= 32'h0;
                total_beats_reg <= dma_total_beats_w;
                remaining_beats_reg <= dma_total_beats_w;
                read_issued_reg <= 32'h0;
                write_issued_reg <= 32'h0;
                write_retired_reg <= 32'h0;
                read_fifo_wr_ptr_reg <= '0;
                read_fifo_rd_ptr_reg <= '0;
                read_fifo_count_reg <= '0;
                read_addr_wr_ptr_reg <= '0;
                read_addr_rd_ptr_reg <= '0;
                read_outstanding_reg <= '0;
                write_data_reg <= '0;
                write_addr_reg <= dst_addr_lo_reg;
                write_addr_wr_ptr_reg <= '0;
                write_addr_rd_ptr_reg <= '0;
                write_aw_valid_reg <= 1'b0;
                write_w_valid_reg <= 1'b0;
                write_outstanding_reg <= '0;
                error_irq_reg <= 1'b0;
                done_tag_reg <= 32'h0;
                err_code_reg <= DMA_ERR_NONE;
                err_info_reg <= 32'h0;

                if ((count_d0_reg == 32'h0) ||
                    !((src_kind_reg == DMA_EP_DRAM) ||
                      (src_kind_reg == DMA_EP_CLUSTER_SPM)) ||
                    !((dst_kind_reg == DMA_EP_DRAM) ||
                      (dst_kind_reg == DMA_EP_CLUSTER_SPM))) begin
                    err_code_reg <= (count_d0_reg == 32'h0) ?
                        DMA_ERR_ZERO_LENGTH : DMA_ERR_BAD_ENDPOINT;
                    irq_pulse_reg <= mmio_req_wdata_i[3];
                    state_reg <= DMA_ST_ERROR;
                end else begin
                    state_reg <= DMA_ST_READ_REQ;
                end
            end else begin
                unique0 case (state_reg)
                    DMA_ST_IDLE: begin
                        write_aw_valid_reg <= 1'b0;
                        write_w_valid_reg <= 1'b0;
                    end
                    DMA_ST_READ_REQ: begin
                        // The source issue cursor advances only on AR acceptance.
                        if (read_issue_fire_w) begin
                            read_issued_reg <= read_issued_reg + 32'd1;
                            read_addr_fifo_reg[read_addr_wr_ptr_reg] <= current_src_addr_reg;
                            read_addr_wr_ptr_reg <= read_addr_wr_ptr_reg + 1'b1;

                            if ((src_index_d0_reg + 32'd1) < count_d0_reg) begin
                                src_index_d0_reg <= src_index_d0_reg + 32'd1;
                                current_src_addr_reg <=
                                    $bits(current_src_addr_reg)'(
                                        current_src_addr_reg + src_stride_d0_reg);
                            end else begin
                                src_index_d0_reg <= 32'h0;
                                if ((src_index_d1_reg + 32'd1) <
                                    effective_count(count_d1_reg)) begin
                                    src_index_d1_reg <= src_index_d1_reg + 32'd1;
                                    current_src_row_base_reg <=
                                        $bits(current_src_row_base_reg)'(
                                            current_src_row_base_reg +
                                            src_stride_d1_reg);
                                    current_src_addr_reg <=
                                        $bits(current_src_addr_reg)'(
                                            current_src_row_base_reg +
                                            src_stride_d1_reg);
                                end else begin
                                    src_index_d1_reg <= 32'h0;
                                    if ((src_index_d2_reg + 32'd1) <
                                        effective_count(count_d2_reg)) begin
                                        src_index_d2_reg <= src_index_d2_reg + 32'd1;
                                        current_src_plane_base_reg <=
                                            $bits(current_src_plane_base_reg)'(
                                                current_src_plane_base_reg +
                                                src_stride_d2_reg);
                                        current_src_row_base_reg <=
                                            $bits(current_src_row_base_reg)'(
                                                current_src_plane_base_reg +
                                                src_stride_d2_reg);
                                        current_src_addr_reg <=
                                            $bits(current_src_addr_reg)'(
                                                current_src_plane_base_reg +
                                                src_stride_d2_reg);
                                    end else begin
                                        src_index_d2_reg <= 32'h0;
                                        if ((src_index_d3_reg + 32'd1) <
                                            effective_count(count_d3_reg)) begin
                                            src_index_d3_reg <= src_index_d3_reg + 32'd1;
                                            current_src_volume_base_reg <=
                                                $bits(current_src_volume_base_reg)'(
                                                    current_src_volume_base_reg +
                                                    src_stride_d3_reg);
                                            current_src_plane_base_reg <=
                                                $bits(current_src_plane_base_reg)'(
                                                    current_src_volume_base_reg +
                                                    src_stride_d3_reg);
                                            current_src_row_base_reg <=
                                                $bits(current_src_row_base_reg)'(
                                                    current_src_volume_base_reg +
                                                    src_stride_d3_reg);
                                            current_src_addr_reg <=
                                                $bits(current_src_addr_reg)'(
                                                    current_src_volume_base_reg +
                                                    src_stride_d3_reg);
                                        end else begin
                                            src_index_d3_reg <= 32'h0;
                                        end
                                    end
                                end
                            end
                        end

                        if (read_response_fire_w) begin
                            read_addr_rd_ptr_reg <= read_addr_rd_ptr_reg + 1'b1;
                        end
                        if (read_fifo_push_w) begin
                            read_data_fifo_reg[read_fifo_wr_ptr_reg] <=
                                read_response_data_w;
                            read_fifo_wr_ptr_reg <= read_fifo_wr_ptr_reg + 1'b1;
                        end
                        if (read_fifo_pop_w) begin
                            read_fifo_rd_ptr_reg <= read_fifo_rd_ptr_reg + 1'b1;
                        end
                        unique0 case ({read_fifo_push_w, read_fifo_pop_w})
                            2'b10: read_fifo_count_reg <= read_fifo_count_reg + 1'b1;
                            2'b01: read_fifo_count_reg <= read_fifo_count_reg - 1'b1;
                            default: ;
                        endcase
                        unique0 case ({read_issue_fire_w, read_response_fire_w})
                            2'b10: read_outstanding_reg <= read_outstanding_reg + 1'b1;
                            2'b01: read_outstanding_reg <= read_outstanding_reg - 1'b1;
                            default: ;
                        endcase

                        if (write_aw_fire_w) begin
                            write_aw_valid_reg <= 1'b0;
                        end
                        if (write_w_fire_w) begin
                            write_w_valid_reg <= 1'b0;
                        end
                        if (write_load_w) begin
                            write_data_reg <= read_data_fifo_reg[read_fifo_rd_ptr_reg];
                            write_addr_reg <= current_dst_addr_reg;
                            write_addr_fifo_reg[write_addr_wr_ptr_reg] <=
                                current_dst_addr_reg;
                            write_addr_wr_ptr_reg <= write_addr_wr_ptr_reg + 1'b1;
                            write_aw_valid_reg <= 1'b1;
                            write_w_valid_reg <= 1'b1;
                            write_issued_reg <= write_issued_reg + 32'd1;

                            if ((dst_index_d0_reg + 32'd1) < count_d0_reg) begin
                                dst_index_d0_reg <= dst_index_d0_reg + 32'd1;
                                current_dst_addr_reg <=
                                    $bits(current_dst_addr_reg)'(
                                        current_dst_addr_reg + dst_stride_d0_reg);
                            end else begin
                                dst_index_d0_reg <= 32'h0;
                                if ((dst_index_d1_reg + 32'd1) <
                                    effective_count(count_d1_reg)) begin
                                    dst_index_d1_reg <= dst_index_d1_reg + 32'd1;
                                    current_dst_row_base_reg <=
                                        $bits(current_dst_row_base_reg)'(
                                            current_dst_row_base_reg +
                                            dst_stride_d1_reg);
                                    current_dst_addr_reg <=
                                        $bits(current_dst_addr_reg)'(
                                            current_dst_row_base_reg +
                                            dst_stride_d1_reg);
                                end else begin
                                    dst_index_d1_reg <= 32'h0;
                                    if ((dst_index_d2_reg + 32'd1) <
                                        effective_count(count_d2_reg)) begin
                                        dst_index_d2_reg <= dst_index_d2_reg + 32'd1;
                                        current_dst_plane_base_reg <=
                                            $bits(current_dst_plane_base_reg)'(
                                                current_dst_plane_base_reg +
                                                dst_stride_d2_reg);
                                        current_dst_row_base_reg <=
                                            $bits(current_dst_row_base_reg)'(
                                                current_dst_plane_base_reg +
                                                dst_stride_d2_reg);
                                        current_dst_addr_reg <=
                                            $bits(current_dst_addr_reg)'(
                                                current_dst_plane_base_reg +
                                                dst_stride_d2_reg);
                                    end else begin
                                        dst_index_d2_reg <= 32'h0;
                                        if ((dst_index_d3_reg + 32'd1) <
                                            effective_count(count_d3_reg)) begin
                                            dst_index_d3_reg <= dst_index_d3_reg + 32'd1;
                                            current_dst_volume_base_reg <=
                                                $bits(current_dst_volume_base_reg)'(
                                                    current_dst_volume_base_reg +
                                                    dst_stride_d3_reg);
                                            current_dst_plane_base_reg <=
                                                $bits(current_dst_plane_base_reg)'(
                                                    current_dst_volume_base_reg +
                                                    dst_stride_d3_reg);
                                            current_dst_row_base_reg <=
                                                $bits(current_dst_row_base_reg)'(
                                                    current_dst_volume_base_reg +
                                                    dst_stride_d3_reg);
                                            current_dst_addr_reg <=
                                                $bits(current_dst_addr_reg)'(
                                                    current_dst_volume_base_reg +
                                                    dst_stride_d3_reg);
                                        end else begin
                                            dst_index_d3_reg <= 32'h0;
                                        end
                                    end
                                end
                            end
                        end

                        if (write_response_fire_w) begin
                            write_addr_rd_ptr_reg <= write_addr_rd_ptr_reg + 1'b1;
                            write_retired_reg <= write_retired_reg + 32'd1;
                            if (remaining_beats_reg != 32'h0) begin
                                remaining_beats_reg <= remaining_beats_reg - 32'd1;
                            end
                        end
                        unique0 case ({write_load_w, write_response_fire_w})
                            2'b10: write_outstanding_reg <= write_outstanding_reg + 1'b1;
                            2'b01: write_outstanding_reg <= write_outstanding_reg - 1'b1;
                            default: ;
                        endcase

                        // synopsys translate_off
                        if (($test$plusargs("TRACE_CLUSTER_DEBUG") ||
                             $test$plusargs("TRACE_CLUSTER_RUNTIME")) &&
                            read_issue_fire_w) begin
                            $display("[%0t] [TRACE][DMA] read_req kind=%0d addr=0x%08x outstanding=%0d",
                                     $time,
                                     src_kind_reg,
                                     current_src_addr_reg,
                                     read_outstanding_reg);
                        end
                        if (($test$plusargs("TRACE_CLUSTER_DEBUG") ||
                             $test$plusargs("TRACE_CLUSTER_RUNTIME")) &&
                            read_response_fire_w) begin
                            $display("[%0t] [TRACE][DMA] read_resp kind=%0d addr=0x%08x resp=%0d data=0x%016x",
                                     $time,
                                     src_kind_reg,
                                     read_addr_fifo_reg[read_addr_rd_ptr_reg],
                                     (src_kind_reg == DMA_EP_DRAM) ?
                                         m_mem_axi_r_resp_i : m_cl_axi_r_resp_i,
                                     read_response_data_w);
                        end
                        if (($test$plusargs("TRACE_CLUSTER_DEBUG") ||
                             $test$plusargs("TRACE_CLUSTER_RUNTIME")) &&
                            write_load_w) begin
                            $display("[%0t] [TRACE][DMA] write_req kind=%0d addr=0x%08x data=0x%016x outstanding=%0d",
                                     $time,
                                     dst_kind_reg,
                                     current_dst_addr_reg,
                                     read_data_fifo_reg[read_fifo_rd_ptr_reg],
                                     write_outstanding_reg);
                        end
                        if (($test$plusargs("TRACE_CLUSTER_DEBUG") ||
                             $test$plusargs("TRACE_CLUSTER_RUNTIME")) &&
                            write_response_fire_w) begin
                            $display("[%0t] [TRACE][DMA] write_resp kind=%0d addr=0x%08x resp=%0d remain=%0d",
                                     $time,
                                     dst_kind_reg,
                                     write_addr_fifo_reg[write_addr_rd_ptr_reg],
                                     write_response_resp_w,
                                     remaining_beats_reg);
                        end
                        // synopsys translate_on

                        if (read_response_error_w) begin
                            err_code_reg <= (src_kind_reg == DMA_EP_DRAM) ?
                                DMA_ERR_DRAM_AXI : DMA_ERR_CLUSTER_RESP;
                            err_info_reg <= 32'h0;
                            error_irq_reg <= 1'b0;
                            read_fifo_wr_ptr_reg <= '0;
                            read_fifo_rd_ptr_reg <= '0;
                            read_fifo_count_reg <= '0;
                            state_reg <= DMA_ST_READ_WAIT;
                        end else if (write_response_error_w) begin
                            err_code_reg <= (dst_kind_reg == DMA_EP_DRAM) ?
                                DMA_ERR_DRAM_AXI : DMA_ERR_CLUSTER_RESP;
                            err_info_reg <= 32'h0;
                            error_irq_reg <= ctrl_reg[3];
                            read_fifo_wr_ptr_reg <= '0;
                            read_fifo_rd_ptr_reg <= '0;
                            read_fifo_count_reg <= '0;
                            state_reg <= DMA_ST_READ_WAIT;
                        end else if (write_response_fire_w &&
                                     (remaining_beats_reg <= 32'd1)) begin
                            done_tag_reg <= cmd_tag_reg;
                            irq_pulse_reg <= ctrl_reg[3];
                            state_reg <= DMA_ST_DONE;
                        end
                    end
                    DMA_ST_READ_WAIT: begin
                        // Error drain: finish already accepted AXI transactions, but
                        // discard returned read data and issue no new work.
                        if (read_response_fire_w) begin
                            read_addr_rd_ptr_reg <= read_addr_rd_ptr_reg + 1'b1;
                            read_outstanding_reg <= read_outstanding_reg - 1'b1;
                        end
                        if (write_aw_fire_w) begin
                            write_aw_valid_reg <= 1'b0;
                        end
                        if (write_w_fire_w) begin
                            write_w_valid_reg <= 1'b0;
                        end
                        if (write_response_fire_w) begin
                            write_addr_rd_ptr_reg <= write_addr_rd_ptr_reg + 1'b1;
                            write_outstanding_reg <= write_outstanding_reg - 1'b1;
                        end

                        if (((read_outstanding_reg == 0) ||
                             ((read_outstanding_reg == 1) &&
                              read_response_fire_w)) &&
                            ((write_outstanding_reg == 0) ||
                             ((write_outstanding_reg == 1) &&
                              write_response_fire_w)) &&
                            (!write_aw_valid_reg || write_aw_fire_w) &&
                            (!write_w_valid_reg || write_w_fire_w)) begin
                            irq_pulse_reg <= error_irq_reg;
                            state_reg <= DMA_ST_ERROR;
                        end
                    end
                    DMA_ST_DONE: begin
                        // synopsys translate_off
                        if ($test$plusargs("TRACE_CLUSTER_DEBUG") ||
                            $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                            $display("[%0t] [TRACE][DMA] done tag=0x%08x err=0x%08x",
                                     $time,
                                     done_tag_reg,
                                     err_code_reg);
                        end
                        // synopsys translate_on
                    end
                    DMA_ST_ERROR: begin
                        // synopsys translate_off
                        if ($test$plusargs("TRACE_CLUSTER_DEBUG") ||
                            $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                            $display("[%0t] [TRACE][DMA] error code=0x%08x info=0x%08x src=0x%08x dst=0x%08x",
                                     $time,
                                     err_code_reg,
                                     err_info_reg,
                                     current_src_addr_reg,
                                     current_dst_addr_reg);
                        end
                        // synopsys translate_on
                    end
                    default: begin
                        state_reg <= DMA_ST_IDLE;
                        write_aw_valid_reg <= 1'b0;
                        write_w_valid_reg <= 1'b0;
                    end
                endcase
            end
        end
    end

endmodule
