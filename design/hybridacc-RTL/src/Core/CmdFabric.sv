//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   CmdFabric
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   MMIO decoder/router for CoreController baseline.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module CmdFabric #(
    parameter int unsigned NUM_CLUSTERS = 1,
    parameter int unsigned NUM_NLU = 0
) (
    input  logic clk,
    input  logic reset_n,
    input  logic        core_mmio_req_valid_i,
    input  logic        core_mmio_req_write_i,
    input  logic [31:0] core_mmio_req_addr_i,
    input  logic [31:0] core_mmio_req_wdata_i,
    input  logic [3:0]  core_mmio_req_wstrb_i,
    output logic        core_mmio_resp_valid_o,
    output logic [31:0] core_mmio_resp_rdata_o,
    output logic        dma_mmio_req_valid_o,
    output logic        dma_mmio_req_write_o,
    output logic [31:0] dma_mmio_req_addr_o,
    output logic [31:0] dma_mmio_req_wdata_o,
    input  logic        dma_mmio_resp_valid_i,
    input  logic [31:0] dma_mmio_resp_rdata_i,
    output logic        plic_mmio_req_valid_o,
    output logic        plic_mmio_req_write_o,
    output logic [31:0] plic_mmio_req_addr_o,
    output logic [31:0] plic_mmio_req_wdata_o,
    input  logic        plic_mmio_resp_valid_i,
    input  logic [31:0] plic_mmio_resp_rdata_i,
    output logic        timer_mmio_req_valid_o,
    output logic        timer_mmio_req_write_o,
    output logic [31:0] timer_mmio_req_addr_o,
    output logic [31:0] timer_mmio_req_wdata_o,
    input  logic        timer_mmio_resp_valid_i,
    input  logic [31:0] timer_mmio_resp_rdata_i,
    output logic        cl_cmd_req_valid_o[NUM_CLUSTERS],
    output logic        cl_cmd_req_write_o[NUM_CLUSTERS],
    output logic [31:0] cl_cmd_req_addr_o[NUM_CLUSTERS],
    output logic [31:0] cl_cmd_req_wdata_o[NUM_CLUSTERS],
    output logic [3:0]  cl_cmd_req_wstrb_o[NUM_CLUSTERS],
    input  logic        cl_cmd_req_ready_i[NUM_CLUSTERS],
    input  logic        cl_cmd_resp_valid_i[NUM_CLUSTERS],
    input  logic [31:0] cl_cmd_resp_rdata_i[NUM_CLUSTERS],
    input  logic        cl_cmd_resp_err_i[NUM_CLUSTERS],
    output logic        nlu_cmd_req_valid_o[NUM_NLU > 0 ? NUM_NLU : 1],
    output logic        nlu_cmd_req_write_o[NUM_NLU > 0 ? NUM_NLU : 1],
    output logic [31:0] nlu_cmd_req_addr_o[NUM_NLU > 0 ? NUM_NLU : 1],
    output logic [31:0] nlu_cmd_req_wdata_o[NUM_NLU > 0 ? NUM_NLU : 1],
    input  logic        nlu_cmd_resp_valid_i[NUM_NLU > 0 ? NUM_NLU : 1],
    input  logic [31:0] nlu_cmd_resp_rdata_i[NUM_NLU > 0 ? NUM_NLU : 1],
    input  logic [31:0] cluster_mask_lo_i,
    input  logic [31:0] cluster_mask_hi_i,
    output logic [31:0] fabric_last_target_o,
    output logic [31:0] fabric_last_addr_o,
    output logic [31:0] fabric_mmio_err_status_o
);
    typedef enum logic [2:0] {
        TARGET_LOCAL_CTRL  = 3'd0,
        TARGET_DMA         = 3'd1,
        TARGET_PLIC        = 3'd2,
        TARGET_TIMER       = 3'd3,
        TARGET_CLUSTER     = 3'd4,
        TARGET_CLUSTER_BC  = 3'd5,
        TARGET_NLU         = 3'd6,
        TARGET_FAULT       = 3'd7
    } target_e;

    logic [31:0] mmio_err_status_reg;
    logic [31:0] last_target_id_reg;
    logic [31:0] last_fault_addr_reg;
    logic [31:0] last_fault_info_reg;
    logic [31:0] boot_reason_reg;

    logic pending_valid_reg;
    target_e pending_target_reg;
    logic [31:0] pending_addr_reg;
    logic [31:0] pending_target_id_reg;
    logic        pending_write_reg;

    function automatic target_e decode_target(input logic [31:0] addr);
        if (addr_in_range(addr, BASE_LOCAL_CTRL, END_LOCAL_CTRL)) return TARGET_LOCAL_CTRL;
        if (addr_in_range(addr, BASE_DMA_MMIO, END_DMA_MMIO)) return TARGET_DMA;
        if (addr_in_range(addr, BASE_LOCAL_TIMER, END_LOCAL_TIMER)) return TARGET_TIMER;
        if (addr_in_range(addr, BASE_PLIC, END_PLIC)) return TARGET_PLIC;
        if (addr_in_range(addr, BASE_CLUSTER_UNICAST, END_CLUSTER_UNICAST)) return TARGET_CLUSTER;
        if (addr_in_range(addr, BASE_CLUSTER_BCAST, END_CLUSTER_BCAST)) return TARGET_CLUSTER_BC;
        if (addr_in_range(addr, BASE_NLU, END_NLU)) return TARGET_NLU;
        return TARGET_FAULT;
    endfunction

    function automatic logic [31:0] decode_local_offset(input logic [31:0] addr, input target_e target);
        case (target)
            TARGET_LOCAL_CTRL: return addr - BASE_LOCAL_CTRL;
            TARGET_DMA:        return addr - BASE_DMA_MMIO;
            TARGET_TIMER:      return addr - BASE_LOCAL_TIMER;
            TARGET_PLIC:       return addr - BASE_PLIC;
            TARGET_CLUSTER:    return (addr - BASE_CLUSTER_UNICAST) % CLUSTER_STRIDE;
            TARGET_CLUSTER_BC: return addr - BASE_CLUSTER_BCAST;
            TARGET_NLU:        return (addr - BASE_NLU) % NLU_STRIDE;
            default:           return 32'h0;
        endcase
    endfunction

    function automatic logic [31:0] decode_target_id(input logic [31:0] addr, input target_e target);
        case (target)
            TARGET_CLUSTER: return (addr - BASE_CLUSTER_UNICAST) / CLUSTER_STRIDE;
            TARGET_NLU:     return (addr - BASE_NLU) / NLU_STRIDE;
            default:        return 32'h0;
        endcase
    endfunction

    function automatic logic [31:0] read_local_ctrl(input logic [31:0] off);
        logic [31:0] r;
        r = 32'h0;
        unique case (off)
            LOCAL_CLUSTER_MASK_LO: r = cluster_mask_lo_i;
            LOCAL_CLUSTER_MASK_HI: r = cluster_mask_hi_i;
            LOCAL_MMIO_ERR_STATUS: r = mmio_err_status_reg;
            LOCAL_LAST_TARGET_ID:  r = last_target_id_reg;
            LOCAL_LAST_FAULT_ADDR: r = last_fault_addr_reg;
            LOCAL_LAST_FAULT_INFO: r = last_fault_info_reg;
            LOCAL_BOOT_REASON:     r = boot_reason_reg;
            LOCAL_FABRIC_CAP0:     r = 32'h0000_0003;
            default:               r = 32'h0;
        endcase
        return r;
    endfunction

    wire request_fire_w = core_mmio_req_valid_i && !pending_valid_reg;
    wire [31:0] req_addr_w = core_mmio_req_addr_i;
    wire [31:0] req_wdata_w = core_mmio_req_wdata_i;
    wire [3:0]  req_wstrb_w = core_mmio_req_wstrb_i;
    wire        req_write_w = core_mmio_req_write_i;
    wire [2:0]  req_target_w = decode_target(req_addr_w);
    wire [31:0] req_offset_w = decode_local_offset(req_addr_w, decode_target(req_addr_w));
    wire [31:0] req_target_id_w = decode_target_id(req_addr_w, decode_target(req_addr_w));

    assign fabric_last_target_o = last_target_id_reg;
    assign fabric_last_addr_o = last_fault_addr_reg;
    assign fabric_mmio_err_status_o = mmio_err_status_reg;

    always_comb begin
        core_mmio_resp_valid_o = 1'b0;
        core_mmio_resp_rdata_o = 32'h0;

        dma_mmio_req_valid_o = 1'b0;
        dma_mmio_req_write_o = req_write_w;
        dma_mmio_req_addr_o  = req_offset_w;
        dma_mmio_req_wdata_o = req_wdata_w;

        plic_mmio_req_valid_o = 1'b0;
        plic_mmio_req_write_o = req_write_w;
        plic_mmio_req_addr_o  = req_offset_w;
        plic_mmio_req_wdata_o = req_wdata_w;

        timer_mmio_req_valid_o = 1'b0;
        timer_mmio_req_write_o = req_write_w;
        timer_mmio_req_addr_o  = req_offset_w;
        timer_mmio_req_wdata_o = req_wdata_w;

        for (int idx = 0; idx < NUM_CLUSTERS; idx++) begin
            cl_cmd_req_valid_o[idx] = 1'b0;
            cl_cmd_req_write_o[idx] = req_write_w;
            cl_cmd_req_addr_o[idx]  = req_offset_w;
            cl_cmd_req_wdata_o[idx] = req_wdata_w;
            cl_cmd_req_wstrb_o[idx] = req_wstrb_w;
        end

        for (int idx = 0; idx < (NUM_NLU > 0 ? NUM_NLU : 1); idx++) begin
            nlu_cmd_req_valid_o[idx] = 1'b0;
            nlu_cmd_req_write_o[idx] = req_write_w;
            nlu_cmd_req_addr_o[idx]  = req_offset_w;
            nlu_cmd_req_wdata_o[idx] = req_wdata_w;
        end

        if (request_fire_w) begin
            unique case (req_target_w)
                TARGET_LOCAL_CTRL: begin
                    core_mmio_resp_valid_o = 1'b1;
                    core_mmio_resp_rdata_o = read_local_ctrl(req_offset_w);
                end
                TARGET_DMA: begin
                    dma_mmio_req_valid_o = 1'b1;
                    core_mmio_resp_valid_o = dma_mmio_resp_valid_i;
                    core_mmio_resp_rdata_o = dma_mmio_resp_rdata_i;
                end
                TARGET_PLIC: begin
                    plic_mmio_req_valid_o = 1'b1;
                end
                TARGET_TIMER: begin
                    timer_mmio_req_valid_o = 1'b1;
                end
                TARGET_CLUSTER: begin
                    if (req_target_id_w < NUM_CLUSTERS) begin
                        cl_cmd_req_valid_o[req_target_id_w] = 1'b1;
                        if (req_write_w) begin
                            core_mmio_resp_valid_o = cl_cmd_req_ready_i[req_target_id_w];
                        end else if (cl_cmd_resp_valid_i[req_target_id_w]) begin
                            core_mmio_resp_valid_o = 1'b1;
                            core_mmio_resp_rdata_o = cl_cmd_resp_rdata_i[req_target_id_w];
                        end
                    end else begin
                        core_mmio_resp_valid_o = 1'b1;
                    end
                end
                TARGET_CLUSTER_BC: begin
                    core_mmio_resp_valid_o = req_write_w;
                    for (int idx = 0; idx < NUM_CLUSTERS; idx++) begin
                        if ((idx < 32 && cluster_mask_lo_i[idx]) || (idx >= 32 && cluster_mask_hi_i[idx - 32])) begin
                            cl_cmd_req_valid_o[idx] = 1'b1;
                        end
                    end
                    if (!req_write_w) begin
                        for (int idx = 0; idx < NUM_CLUSTERS; idx++) begin
                            if (((idx < 32 && cluster_mask_lo_i[idx]) || (idx >= 32 && cluster_mask_hi_i[idx - 32])) && cl_cmd_resp_valid_i[idx]) begin
                                core_mmio_resp_valid_o = 1'b1;
                                core_mmio_resp_rdata_o = cl_cmd_resp_rdata_i[idx];
                            end
                        end
                    end
                end
                TARGET_NLU: begin
                    if (req_target_id_w < (NUM_NLU > 0 ? NUM_NLU : 1)) begin
                        nlu_cmd_req_valid_o[req_target_id_w] = 1'b1;
                    end else begin
                        core_mmio_resp_valid_o = 1'b1;
                    end
                end
                default: begin
                    core_mmio_resp_valid_o = 1'b1;
                    core_mmio_resp_rdata_o = 32'h0;
                end
            endcase
        end else if (pending_valid_reg) begin
            unique case (pending_target_reg)
                TARGET_DMA: begin
                    core_mmio_resp_valid_o = dma_mmio_resp_valid_i;
                    core_mmio_resp_rdata_o = dma_mmio_resp_rdata_i;
                end
                TARGET_PLIC: begin
                    core_mmio_resp_valid_o = plic_mmio_resp_valid_i;
                    core_mmio_resp_rdata_o = plic_mmio_resp_rdata_i;
                end
                TARGET_TIMER: begin
                    core_mmio_resp_valid_o = timer_mmio_resp_valid_i;
                    core_mmio_resp_rdata_o = timer_mmio_resp_rdata_i;
                end
                TARGET_CLUSTER: begin
                    if (pending_target_id_reg < NUM_CLUSTERS) begin
                        cl_cmd_req_valid_o[pending_target_id_reg] = 1'b1;
                        cl_cmd_req_write_o[pending_target_id_reg] = pending_write_reg;
                        cl_cmd_req_addr_o[pending_target_id_reg]  = pending_addr_reg;
                        cl_cmd_req_wdata_o[pending_target_id_reg] = 32'h0;
                        cl_cmd_req_wstrb_o[pending_target_id_reg] = 4'h0;
                        core_mmio_resp_valid_o = cl_cmd_resp_valid_i[pending_target_id_reg];
                        core_mmio_resp_rdata_o = cl_cmd_resp_rdata_i[pending_target_id_reg];
                    end else begin
                        core_mmio_resp_valid_o = 1'b1;
                    end
                end
                TARGET_CLUSTER_BC: begin
                    for (int idx = 0; idx < NUM_CLUSTERS; idx++) begin
                        if ((idx < 32 && cluster_mask_lo_i[idx]) || (idx >= 32 && cluster_mask_hi_i[idx - 32])) begin
                            cl_cmd_req_valid_o[idx] = 1'b1;
                            cl_cmd_req_write_o[idx] = pending_write_reg;
                            cl_cmd_req_addr_o[idx]  = pending_addr_reg;
                            cl_cmd_req_wdata_o[idx] = 32'h0;
                            cl_cmd_req_wstrb_o[idx] = 4'h0;
                        end
                    end
                    for (int idx = 0; idx < NUM_CLUSTERS; idx++) begin
                        if (((idx < 32 && cluster_mask_lo_i[idx]) || (idx >= 32 && cluster_mask_hi_i[idx - 32])) && cl_cmd_resp_valid_i[idx]) begin
                            core_mmio_resp_valid_o = 1'b1;
                            core_mmio_resp_rdata_o = cl_cmd_resp_rdata_i[idx];
                        end
                    end
                end
                TARGET_NLU: begin
                    if (pending_target_id_reg < (NUM_NLU > 0 ? NUM_NLU : 1)) begin
                        core_mmio_resp_valid_o = nlu_cmd_resp_valid_i[pending_target_id_reg];
                        core_mmio_resp_rdata_o = nlu_cmd_resp_rdata_i[pending_target_id_reg];
                    end else begin
                        core_mmio_resp_valid_o = 1'b1;
                    end
                end
                default: begin
                    core_mmio_resp_valid_o = 1'b1;
                    core_mmio_resp_rdata_o = 32'h0;
                end
            endcase
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            mmio_err_status_reg  <= 32'h0;
            last_target_id_reg   <= 32'h0;
            last_fault_addr_reg  <= 32'h0;
            last_fault_info_reg  <= 32'h0;
            boot_reason_reg      <= 32'h0;
            pending_valid_reg    <= 1'b0;
            pending_target_reg   <= TARGET_LOCAL_CTRL;
            pending_addr_reg     <= 32'h0;
            pending_target_id_reg<= 32'h0;
            pending_write_reg    <= 1'b0;
        end else begin
            if (request_fire_w) begin
                last_fault_addr_reg <= req_addr_w;
                last_target_id_reg <= req_target_id_w;
                if (req_target_w == TARGET_FAULT) begin
                    mmio_err_status_reg[0] <= 1'b1;
                    last_fault_info_reg <= 32'h1;
                    pending_valid_reg <= 1'b0;
                end else if ((req_target_w == TARGET_LOCAL_CTRL) || (req_target_w == TARGET_DMA) || (req_target_w == TARGET_CLUSTER_BC && req_write_w) || (req_target_w == TARGET_CLUSTER && req_write_w)) begin
                    pending_valid_reg <= 1'b0;
                    if ((req_target_w == TARGET_LOCAL_CTRL) && req_write_w && (req_offset_w == LOCAL_MMIO_ERR_STATUS) && (req_wdata_w == 32'h0)) begin
                        mmio_err_status_reg <= 32'h0;
                    end
                end else begin
                    pending_valid_reg <= 1'b1;
                    pending_target_reg <= target_e'(req_target_w);
                    pending_addr_reg <= req_offset_w;
                    pending_target_id_reg <= req_target_id_w;
                    pending_write_reg <= req_write_w;
                end
            end

            if (pending_valid_reg && core_mmio_resp_valid_o) begin
                pending_valid_reg <= 1'b0;
            end
        end
    end

endmodule