//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   SectionLoader
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Minimal section loader baseline.
//                Streams aligned 32-bit words from DRAM AXI read channel into
//                local SRAM, using ISRAM first as functional bring-up path.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module SectionLoader import core_pkg::*; #(
    parameter int unsigned ISRAM_BYTES_P = ISRAM_BYTES
) (
    input  logic clk,
    input  logic reset_n,
    input  logic        kick_i,
    input  logic [31:0] manifest_addr_lo_i,
    input  logic [31:0] manifest_addr_hi_i,
    input  logic [31:0] manifest_size_i,
    output logic        m_mem_axi_ar_valid_o,
    input  logic        m_mem_axi_ar_ready_i,
    output logic [31:0] m_mem_axi_ar_addr_o,
    output logic [7:0]  m_mem_axi_ar_len_o,
    input  logic        m_mem_axi_r_valid_i,
    output logic        m_mem_axi_r_ready_o,
    input  logic [MEM_AXI_DATA_WIDTH-1:0] m_mem_axi_r_data_i,
    input  logic [1:0]  m_mem_axi_r_resp_i,
    input  logic        m_mem_axi_r_last_i,
    output logic        isram_wr_en_o,
    output logic [31:0] isram_wr_addr_o,
    output logic [31:0] isram_wr_data_o,
    output logic [3:0]  isram_wr_strb_o,
    output logic        dsram_wr_en_o,
    output logic [31:0] dsram_wr_addr_o,
    output logic [31:0] dsram_wr_data_o,
    output logic [3:0]  dsram_wr_strb_o,
    output logic        load_phase_o,
    output logic        busy_o,
    output logic        done_o,
    output logic [31:0] status_o,
    output logic [31:0] err_code_o,
    output logic [31:0] err_info_o
);
    typedef enum logic [2:0] {
        LD_IDLE,
        LD_FETCH,
        LD_WAIT_R,
        LD_WRITE_LOCAL,
        LD_DONE,
        LD_ERR
    } loader_state_e;

    loader_state_e state_reg;
    logic [31:0] remaining_words_reg;
    logic [31:0] dram_addr_reg;
    logic [31:0] local_addr_reg;
    logic [31:0] data_word_reg;
    logic        load_phase_w;

    function automatic logic [31:0] make_status(input loader_state_e st, input logic [15:0] remaining);
        logic [31:0] status;
        status = 32'h0;
        if ((st != LD_IDLE) && (st != LD_DONE) && (st != LD_ERR)) begin
            status[0] = 1'b1;
        end
        if (st == LD_DONE) begin
            status[1] = 1'b1;
        end
        if (st == LD_ERR) begin
            status[2] = 1'b1;
        end
        status[7:4] = st;
        status[31:16] = remaining;
        return status;
    endfunction

    assign m_mem_axi_ar_len_o = 8'h00;
    assign m_mem_axi_ar_addr_o = dram_addr_reg;
    assign m_mem_axi_ar_valid_o = (state_reg == LD_FETCH) && (manifest_addr_hi_i === manifest_addr_hi_i);
    assign m_mem_axi_r_ready_o = (state_reg == LD_WAIT_R)
                              && (m_mem_axi_r_data_i[MEM_AXI_DATA_WIDTH-1:32] === m_mem_axi_r_data_i[MEM_AXI_DATA_WIDTH-1:32])
                              && (m_mem_axi_r_last_i === m_mem_axi_r_last_i);
    assign load_phase_w = (state_reg != LD_IDLE) && (state_reg != LD_DONE) && (state_reg != LD_ERR);
    assign load_phase_o = load_phase_w;
    assign busy_o = load_phase_w;
    assign done_o = (state_reg == LD_DONE);
    assign status_o = make_status(state_reg, remaining_words_reg[15:0]);

    always_comb begin
        isram_wr_en_o = 1'b0;
        isram_wr_addr_o = 32'h0;
        isram_wr_data_o = 32'h0;
        isram_wr_strb_o = 4'h0;
        dsram_wr_en_o = 1'b0;
        dsram_wr_addr_o = 32'h0;
        dsram_wr_data_o = 32'h0;
        dsram_wr_strb_o = 4'h0;

        if (state_reg == LD_WRITE_LOCAL) begin
            if (local_addr_reg < ISRAM_BYTES_P) begin
                isram_wr_en_o = 1'b1;
                isram_wr_addr_o = local_addr_reg;
                isram_wr_data_o = data_word_reg;
                isram_wr_strb_o = 4'hF;
            end else begin
                dsram_wr_en_o = 1'b1;
                dsram_wr_addr_o = local_addr_reg - ISRAM_BYTES_P;
                dsram_wr_data_o = data_word_reg;
                dsram_wr_strb_o = 4'hF;
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state_reg <= LD_IDLE;
            remaining_words_reg <= 32'h0;
            dram_addr_reg <= 32'h0;
            local_addr_reg <= 32'h0;
            data_word_reg <= 32'h0;
            err_code_o <= LOADER_OK;
            err_info_o <= 32'h0;
        end else begin
            unique0 case (state_reg)
                LD_IDLE: begin
                    if (kick_i) begin
                        if ((manifest_size_i == 32'h0) || manifest_size_i[1:0] != 2'b00) begin
                            err_code_o <= LOADER_ERR_MANIFEST_SIZE;
                            err_info_o <= manifest_size_i;
                            state_reg <= LD_ERR;
                        end else begin
                            err_code_o <= LOADER_OK;
                            err_info_o <= 32'h0;
                            remaining_words_reg <= manifest_size_i >> 2;
                            dram_addr_reg <= manifest_addr_lo_i;
                            local_addr_reg <= 32'h0;
                            state_reg <= LD_FETCH;
                        end
                    end
                end
                LD_FETCH: begin
                    if (m_mem_axi_ar_ready_i) begin
                        state_reg <= LD_WAIT_R;
                    end
                end
                LD_WAIT_R: begin
                    if (m_mem_axi_r_valid_i) begin
                        if (m_mem_axi_r_resp_i != 2'b00) begin
                            err_code_o <= LOADER_ERR_AXI;
                            err_info_o <= dram_addr_reg;
                            state_reg <= LD_ERR;
                        end else begin
                            data_word_reg <= m_mem_axi_r_data_i[31:0];
                            state_reg <= LD_WRITE_LOCAL;
                        end
                    end
                end
                LD_WRITE_LOCAL: begin
                    if (remaining_words_reg <= 32'd1) begin
                        remaining_words_reg <= 32'h0;
                        state_reg <= LD_DONE;
                    end else begin
                        remaining_words_reg <= $bits(remaining_words_reg)'(remaining_words_reg - 32'd1);
                        dram_addr_reg <= $bits(dram_addr_reg)'(dram_addr_reg + 32'd8);
                        local_addr_reg <= $bits(local_addr_reg)'(local_addr_reg + 32'd4);
                        state_reg <= LD_FETCH;
                    end
                end
                LD_DONE: begin
                    if (kick_i) begin
                        state_reg <= LD_IDLE;
                    end
                end
                LD_ERR: begin
                    if (kick_i) begin
                        err_code_o <= LOADER_OK;
                        err_info_o <= 32'h0;
                        state_reg <= LD_IDLE;
                    end
                end
                default: state_reg <= LD_IDLE;
            endcase
        end
    end

endmodule