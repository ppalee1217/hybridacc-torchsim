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

    logic [31:0] priority_reg [0:MAX_SOURCES];
    logic [31:0] enable_lo_reg;
    logic [31:0] enable_hi_reg;
    logic [31:0] threshold_reg;
    logic [31:0] pending_lo_reg;
    logic [31:0] pending_hi_reg;
    logic [31:0] claimed_lo_reg;
    logic [31:0] claimed_hi_reg;

    function automatic logic sample_nlu_source(input int unsigned id);
        logic level;
        level = 1'b0;
        for (int unsigned nlu = 0; nlu < NUM_NLU; nlu++) begin
            if (id == (NUM_CLUSTERS + 2 + nlu)) begin
                level = nlu_irq_i[nlu];
            end
        end
        return level;
    endfunction

    function automatic logic sample_source(input int unsigned id);
        logic level;
        level = 1'b0;
        if ((id >= 1) && (id <= NUM_CLUSTERS)) begin
            level = cluster_irq_i[id - 1];
        end else if (id == (NUM_CLUSTERS + 1)) begin
            level = dma_irq_i;
        end else if (id == (NUM_CLUSTERS + NUM_NLU + 2)) begin
            level = loader_fault_i;
        end else if (id == (NUM_CLUSTERS + NUM_NLU + 3)) begin
            level = fabric_fault_i;
        end else begin
            level = sample_nlu_source(id);
        end
        return level;
    endfunction

    function automatic logic is_pending(input int unsigned id);
        if (id < 32) return pending_lo_reg[id];
        if (id < 64) return pending_hi_reg[id - 32];
        return 1'b0;
    endfunction

    function automatic logic is_enabled(input int unsigned id);
        if (id < 32) return enable_lo_reg[id];
        if (id < 64) return enable_hi_reg[id - 32];
        return 1'b0;
    endfunction

    function automatic logic [31:0] claim_id();
        logic [31:0] best_id;
        logic [31:0] best_pri;
        best_id = 32'h0;
        best_pri = 32'h0;
        for (int unsigned source = 1; source <= NUM_SOURCES; source++) begin
            if (is_pending(source) && is_enabled(source) && (priority_reg[source] > threshold_reg)) begin
                if ((priority_reg[source] > best_pri) || ((priority_reg[source] == best_pri) && ((best_id == 0) || (source < best_id)))) begin
                    best_pri = priority_reg[source];
                    best_id = source;
                end
            end
        end
        return best_id;
    endfunction

    always_comb begin
        meip_o = 1'b0;
        for (int unsigned source = 1; source <= NUM_SOURCES; source++) begin
            if (is_pending(source) && is_enabled(source) && (priority_reg[source] > threshold_reg)) begin
                meip_o = 1'b1;
            end
        end
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
                priority_reg[idx] <= 32'd1;
            end
        end else begin
            logic [31:0] claim_val;
            claim_val = 32'h0;

            mmio_resp_valid_o <= 1'b0;
            for (int unsigned source = 1; source <= NUM_SOURCES; source++) begin
                if (sample_source(source)) begin
                    if (source < 32) pending_lo_reg[source] <= 1'b1;
                    else pending_hi_reg[source - 32] <= 1'b1;
                end
            end

            if (mmio_req_valid_i) begin
                mmio_resp_valid_o <= 1'b1;
                if (mmio_req_addr_i < PLIC_PENDING_LO) begin
                    int unsigned source;
                    source = mmio_req_addr_i[31:2];
                    if ((source >= 1) && (source <= NUM_SOURCES)) begin
                        if (mmio_req_write_i) priority_reg[source] <= mmio_req_wdata_i;
                        else mmio_resp_rdata_o <= priority_reg[source];
                    end else begin
                        mmio_resp_rdata_o <= 32'h0;
                    end
                end else if (mmio_req_addr_i == PLIC_PENDING_LO) begin
                    mmio_resp_rdata_o <= pending_lo_reg;
                end else if (mmio_req_addr_i == PLIC_PENDING_HI) begin
                    mmio_resp_rdata_o <= pending_hi_reg;
                end else if (mmio_req_addr_i == PLIC_ENABLE_LO) begin
                    if (mmio_req_write_i) enable_lo_reg <= mmio_req_wdata_i;
                    else mmio_resp_rdata_o <= enable_lo_reg;
                end else if (mmio_req_addr_i == PLIC_ENABLE_HI) begin
                    if (mmio_req_write_i) enable_hi_reg <= mmio_req_wdata_i;
                    else mmio_resp_rdata_o <= enable_hi_reg;
                end else if (mmio_req_addr_i == PLIC_THRESHOLD) begin
                    if (mmio_req_write_i) threshold_reg <= mmio_req_wdata_i;
                    else mmio_resp_rdata_o <= threshold_reg;
                end else if (mmio_req_addr_i == PLIC_CLAIM_COMPLETE) begin
                    if (!mmio_req_write_i) begin
                        claim_val = claim_id();
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
                            if (sample_source(mmio_req_wdata_i)) pending_lo_reg[mmio_req_wdata_i] <= 1'b1;
                        end else begin
                            claimed_hi_reg[mmio_req_wdata_i - 32] <= 1'b0;
                            if (sample_source(mmio_req_wdata_i)) pending_hi_reg[mmio_req_wdata_i - 32] <= 1'b1;
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