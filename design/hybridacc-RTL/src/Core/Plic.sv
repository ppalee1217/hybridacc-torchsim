//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   Plic
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Single-hart PLIC baseline for CoreController.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module Plic import core_pkg::*; #(
    parameter int unsigned NUM_CLUSTERS = 1,
    parameter int unsigned NUM_NLU = 0
) (
    input  logic clk,
    input  logic reset_n,
    input  logic cluster_irq_i[NUM_CLUSTERS],
    input  logic nlu_irq_i[NUM_NLU > 0 ? NUM_NLU : 1],
    input  logic dma_irq_i,
    input  logic loader_fault_i,
    input  logic fabric_fault_i,
    output logic meip_o,
    input  logic        mmio_req_valid_i,
    input  logic        mmio_req_write_i,
    input  logic [31:0] mmio_req_addr_i,
    input  logic [31:0] mmio_req_wdata_i,
    output logic        mmio_resp_valid_o,
    output logic [31:0] mmio_resp_rdata_o,
    output logic [31:0] pending_lo_o,
    output logic [31:0] pending_hi_o
);
    localparam int unsigned NUM_SOURCES = NUM_CLUSTERS + NUM_NLU + 3;
    localparam int unsigned MAX_SOURCES = 64;
    localparam logic [31:0] PRIORITY_RESET = 32'd1;

    logic [31:0] priority_reg [0:MAX_SOURCES];
    logic [31:0] enable_lo_reg;
    logic [31:0] enable_hi_reg;
    logic [31:0] threshold_reg;
    logic [31:0] pending_lo_reg;
    logic [31:0] pending_hi_reg;
    logic [31:0] claimed_lo_reg;
    logic [31:0] claimed_hi_reg;
    logic        priority_init_done_reg;
    logic [31:0] claim_id_w;

    function automatic logic sample_nlu_source(
        input int unsigned id,
        input logic nlu_irq[NUM_NLU > 0 ? NUM_NLU : 1]
    );
        logic level;
        level = 1'b0;
        if (NUM_NLU == 0) begin
            level = nlu_irq[0] && (id == 0);
        end else begin
            for (int unsigned nlu = 0; nlu < NUM_NLU; nlu++) begin
                if (id == (NUM_CLUSTERS + 2 + nlu)) begin
                    level = nlu_irq[nlu];
                end
            end
        end
        return level;
    endfunction

    function automatic logic sample_source(
        input int unsigned id,
        input logic cluster_irq[NUM_CLUSTERS],
        input logic nlu_irq[NUM_NLU > 0 ? NUM_NLU : 1],
        input logic dma_irq,
        input logic loader_fault,
        input logic fabric_fault
    );
        logic level;
        level = 1'b0;
        if ((id >= 1) && (id <= NUM_CLUSTERS)) begin
            level = cluster_irq[id - 1];
        end else if (id == (NUM_CLUSTERS + 1)) begin
            level = dma_irq;
        end else if (id == (NUM_CLUSTERS + NUM_NLU + 2)) begin
            level = loader_fault;
        end else if (id == (NUM_CLUSTERS + NUM_NLU + 3)) begin
            level = fabric_fault;
        end else begin
            level = sample_nlu_source(id, nlu_irq);
        end
        return level;
    endfunction

    function automatic logic is_pending(
        input int unsigned id,
        input logic [31:0] pending_lo,
        input logic [31:0] pending_hi
    );
        logic pending;

        pending = 1'b0;
        if (id < 32) begin
            pending = pending_lo[id];
        end else if (id < 64) begin
            pending = pending_hi[id - 32];
        end
        return pending;
    endfunction

    function automatic logic is_enabled(
        input int unsigned id,
        input logic [31:0] enable_lo,
        input logic [31:0] enable_hi
    );
        logic enabled;

        enabled = 1'b0;
        if (id < 32) begin
            enabled = enable_lo[id];
        end else if (id < 64) begin
            enabled = enable_hi[id - 32];
        end
        return enabled;
    endfunction

    always_comb begin
        logic [31:0] best_id;

        meip_o = 1'b0;
        best_id = 32'h0;
        for (int unsigned source = 1; source <= NUM_SOURCES; source++) begin
            if (is_pending(source, pending_lo_reg, pending_hi_reg)
                && is_enabled(source, enable_lo_reg, enable_hi_reg)
                && (priority_reg[source] > threshold_reg)) begin
                meip_o = 1'b1;
                if (best_id == 0) begin
                    best_id = source;
                end else if ((priority_reg[source] > priority_reg[best_id])
                    || ((priority_reg[source] == priority_reg[best_id]) && (source < best_id))) begin
                    best_id = source;
                end
            end
        end
        claim_id_w = best_id;
    end

    assign pending_lo_o = pending_lo_reg;
    assign pending_hi_o = pending_hi_reg;

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            enable_lo_reg      <= 32'h0;
            enable_hi_reg      <= 32'h0;
            threshold_reg      <= 32'h0;
            pending_lo_reg     <= 32'h0;
            pending_hi_reg     <= 32'h0;
            claimed_lo_reg     <= 32'h0;
            claimed_hi_reg     <= 32'h0;
            mmio_resp_valid_o  <= 1'b0;
            mmio_resp_rdata_o  <= 32'h0;
            for (int unsigned idx = 0; idx <= MAX_SOURCES; idx++) begin
                priority_reg[idx] <= 32'h0;
            end
            priority_init_done_reg <= 1'b0;
        end else begin
            logic [31:0] claim_val;
            claim_val = 32'h0;

            mmio_resp_valid_o <= 1'b0;
            for (int unsigned source = 1; source <= NUM_SOURCES; source++) begin
                if (sample_source(source, cluster_irq_i, nlu_irq_i, dma_irq_i, loader_fault_i, fabric_fault_i)) begin
                    if (source < 32) begin
                        pending_lo_reg[source] <= 1'b1;
                    end else begin
                        pending_hi_reg[source - 32] <= 1'b1;
                    end
                end
            end

            if (!priority_init_done_reg) begin
                for (int unsigned idx = 0; idx <= MAX_SOURCES; idx++) begin
                    priority_reg[idx] <= PRIORITY_RESET;
                end
                priority_init_done_reg <= 1'b1;
            end else if (mmio_req_valid_i) begin
                mmio_resp_valid_o <= 1'b1;
                if (mmio_req_addr_i < PLIC_PENDING_LO) begin
                    int unsigned source;
                    source = mmio_req_addr_i[31:2];
                    if ((source >= 1) && (source <= NUM_SOURCES)) begin
                        if (mmio_req_write_i) begin
                            priority_reg[source] <= mmio_req_wdata_i;
                        end else begin
                            mmio_resp_rdata_o <= priority_reg[source];
                        end
                    end else begin
                        mmio_resp_rdata_o <= 32'h0;
                    end
                end else if (mmio_req_addr_i == PLIC_PENDING_LO) begin
                    mmio_resp_rdata_o <= pending_lo_reg;
                end else if (mmio_req_addr_i == PLIC_PENDING_HI) begin
                    mmio_resp_rdata_o <= pending_hi_reg;
                end else if (mmio_req_addr_i == PLIC_ENABLE_LO) begin
                    if (mmio_req_write_i) begin
                        enable_lo_reg <= mmio_req_wdata_i;
                    end else begin
                        mmio_resp_rdata_o <= enable_lo_reg;
                    end
                end else if (mmio_req_addr_i == PLIC_ENABLE_HI) begin
                    if (mmio_req_write_i) begin
                        enable_hi_reg <= mmio_req_wdata_i;
                    end else begin
                        mmio_resp_rdata_o <= enable_hi_reg;
                    end
                end else if (mmio_req_addr_i == PLIC_THRESHOLD) begin
                    if (mmio_req_write_i) begin
                        threshold_reg <= mmio_req_wdata_i;
                    end else begin
                        mmio_resp_rdata_o <= threshold_reg;
                    end
                end else if (mmio_req_addr_i == PLIC_CLAIM_COMPLETE) begin
                    if (!mmio_req_write_i) begin
                        claim_val = claim_id_w;
                        mmio_resp_rdata_o <= claim_val;
                        if (claim_val != 0) begin
                            if (claim_val < 32) begin
                                pending_lo_reg[claim_val] <= 1'b0;
                                claimed_lo_reg[claim_val] <= 1'b1;
                            end else begin
                                pending_hi_reg[claim_val - 32] <= 1'b0;
                                claimed_hi_reg[claim_val - 32] <= 1'b1;
                            end
                        end
                    end else if ((mmio_req_wdata_i >= 1) && (mmio_req_wdata_i <= NUM_SOURCES)) begin
                        if (mmio_req_wdata_i < 32) begin
                            claimed_lo_reg[mmio_req_wdata_i] <= 1'b0;
                            if (sample_source(mmio_req_wdata_i, cluster_irq_i, nlu_irq_i, dma_irq_i, loader_fault_i, fabric_fault_i)) begin
                                pending_lo_reg[mmio_req_wdata_i] <= 1'b1;
                            end
                        end else begin
                            claimed_hi_reg[mmio_req_wdata_i - 32] <= 1'b0;
                            if (sample_source(mmio_req_wdata_i, cluster_irq_i, nlu_irq_i, dma_irq_i, loader_fault_i, fabric_fault_i)) begin
                                pending_hi_reg[mmio_req_wdata_i - 32] <= 1'b1;
                            end
                        end
                        mmio_resp_rdata_o <= 32'h0;
                    end
                end else if (mmio_req_addr_i == PLIC_MAX_SOURCE_ID) begin
                    mmio_resp_rdata_o <= NUM_SOURCES;
                end else begin
                    mmio_resp_rdata_o <= 32'h0;
                end
            end
        end
    end

endmodule