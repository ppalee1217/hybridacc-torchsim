//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   BootHostIf
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Host AXI4-Lite control plane for CoreController baseline.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module BootHostIf import core_pkg::*; #(
    parameter int unsigned NUM_CLUSTERS = 1,
    parameter int unsigned NUM_NLU = 0
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        s_ctrl_aw_valid_i,
    output logic        s_ctrl_aw_ready_o,
    input  logic [31:0] s_ctrl_aw_addr_i,
    input  logic        s_ctrl_w_valid_i,
    output logic        s_ctrl_w_ready_o,
    input  logic [31:0] s_ctrl_w_data_i,
    input  logic [3:0]  s_ctrl_w_strb_i,
    output logic        s_ctrl_b_valid_o,
    input  logic        s_ctrl_b_ready_i,
    output logic [1:0]  s_ctrl_b_resp_o,
    input  logic        s_ctrl_ar_valid_i,
    output logic        s_ctrl_ar_ready_o,
    input  logic [31:0] s_ctrl_ar_addr_i,
    output logic        s_ctrl_r_valid_o,
    input  logic        s_ctrl_r_ready_i,
    output logic [31:0] s_ctrl_r_data_o,
    output logic [1:0]  s_ctrl_r_resp_o,
    output logic        core_enable_o,
    output logic        core_haltreq_o,
    output logic [31:0] boot_addr_o,
    output logic        load_phase_o,
    output logic        loader_kick_o,
    output logic [31:0] manifest_addr_lo_o,
    output logic [31:0] manifest_addr_hi_o,
    output logic [31:0] manifest_size_o,
    output logic [31:0] cluster_mask_lo_o,
    output logic [31:0] cluster_mask_hi_o,
    input  logic        loader_busy_i,
    input  logic        loader_done_i,
    input  logic [31:0] loader_status_i,
    input  logic [31:0] loader_err_code_i,
    input  logic [31:0] loader_err_info_i,
    input  logic        core_halted_i,
    input  logic        core_running_i,
    input  logic [31:0] core_pc_snapshot_i,
    input  logic [31:0] core_cause_snapshot_i,
    input  logic        plic_pending_any_i,
    input  logic [31:0] fabric_last_target_i,
    input  logic [31:0] fabric_last_addr_i,
    output logic        controller_irq_o
);
    logic [31:0] ctrl_reg;
    logic [31:0] boot_addr_reg;
    logic [31:0] manifest_lo_reg;
    logic [31:0] manifest_hi_reg;
    logic [31:0] manifest_size_reg;
    logic [31:0] cluster_mask_lo_reg;
    logic [31:0] cluster_mask_hi_reg;
    logic [31:0] trace_base_reg;
    logic [31:0] trace_size_reg;
    logic [31:0] trace_ctrl_reg;
    logic [31:0] irq_summary_sticky_reg;

    logic        wr_addr_valid_reg;
    logic [31:0] wr_addr_reg;

    function automatic logic [31:0] cap0_value();
        return 32'h0000_000F;
    endfunction

    function automatic logic [31:0] cap1_value();
        return ((NUM_NLU & 32'hFF) << 8) | (NUM_CLUSTERS & 32'hFF);
    endfunction

    function automatic logic [31:0] hacc_status_word(
        input logic loader_busy,
        input logic loader_done,
        input logic core_halted,
        input logic core_running,
        input logic [31:0] loader_err_code
    );
        logic [31:0] status;
        status = 32'h0;
        if (loader_busy)                    status[0] = 1'b1;
        if (loader_done)                    status[1] = 1'b1;
        if (core_halted)                    status[2] = 1'b1;
        if (core_running)                   status[3] = 1'b1;
        if (loader_err_code != 32'h0)       status[4] = 1'b1;
        if (loader_busy)                    status[5] = 1'b1;
        return status;
    endfunction

    function automatic logic [31:0] irq_summary_word(
        input logic [31:0] sticky,
        input logic        loader_done,
        input logic [31:0] loader_err_code,
        input logic [31:0] core_cause,
        input logic        core_halted,
        input logic        plic_pending_any
    );
        logic [31:0] summary;
        summary = sticky;
        if (loader_done)                summary[0] = 1'b1;
        if (loader_err_code != 32'h0)   summary[1] = 1'b1;
        if (core_cause != 32'h0)        summary[2] = 1'b1;
        if (core_halted)                summary[3] = 1'b1;
        if (plic_pending_any)           summary[4] = 1'b1;
        return summary;
    endfunction

    function automatic logic [31:0] read_csr(input logic [31:0] offset);
        logic [31:0] r;
        r = 32'h0;
        unique0 case (offset)
            HACC_CAP0:           r = cap0_value();
            HACC_CAP1:           r = cap1_value();
            HACC_CTRL:           r = ctrl_reg;
            HACC_STATUS:         r = hacc_status_word(loader_busy_i, loader_done_i, core_halted_i, core_running_i, loader_err_code_i);
            CORE_BOOT_ADDR:      r = boot_addr_reg;
            CORE_PC_SNAPSHOT:    r = core_pc_snapshot_i;
            CORE_CAUSE_SNAPSHOT: r = core_cause_snapshot_i;
            MANIFEST_ADDR_LO:    r = manifest_lo_reg;
            MANIFEST_ADDR_HI:    r = manifest_hi_reg;
            MANIFEST_SIZE:       r = manifest_size_reg;
            LOADER_STATUS:       r = loader_status_i;
            LOADER_ERR_CODE:     r = loader_err_code_i;
            LOADER_ERR_INFO:     r = loader_err_info_i;
            IRQ_SUMMARY:         r = irq_summary_word(irq_summary_sticky_reg, loader_done_i, loader_err_code_i, core_cause_snapshot_i, core_halted_i, plic_pending_any_i);
            CLUSTER_MASK_LO:     r = cluster_mask_lo_reg;
            CLUSTER_MASK_HI:     r = cluster_mask_hi_reg;
            LAST_MMIO_TARGET:    r = fabric_last_target_i;
            LAST_MMIO_ADDR:      r = fabric_last_addr_i;
            TRACE_BASE:          r = trace_base_reg;
            TRACE_SIZE:          r = trace_size_reg;
            TRACE_CTRL:          r = trace_ctrl_reg;
            TRACE_STATUS:        r = 32'h0;
            default:             r = 32'h0;
        endcase
        return r;
    endfunction

    assign core_enable_o     = ctrl_reg[0];
    assign core_haltreq_o    = ctrl_reg[1];
    assign boot_addr_o       = boot_addr_reg;
    assign load_phase_o      = loader_busy_i;
    assign manifest_addr_lo_o= manifest_lo_reg;
    assign manifest_addr_hi_o= manifest_hi_reg;
    assign manifest_size_o   = manifest_size_reg;
    assign cluster_mask_lo_o = cluster_mask_lo_reg;
    assign cluster_mask_hi_o = cluster_mask_hi_reg;
    assign controller_irq_o  = |irq_summary_word(irq_summary_sticky_reg, loader_done_i, loader_err_code_i, core_cause_snapshot_i, core_halted_i, plic_pending_any_i);

    assign s_ctrl_aw_ready_o = !wr_addr_valid_reg && !s_ctrl_b_valid_o;
    assign s_ctrl_w_ready_o  = (wr_addr_valid_reg || s_ctrl_aw_valid_i) && !s_ctrl_b_valid_o;
    assign s_ctrl_ar_ready_o = !s_ctrl_r_valid_o && (s_ctrl_ar_addr_i[31:12] === s_ctrl_ar_addr_i[31:12]);
    assign s_ctrl_b_resp_o   = 2'b00;
    assign s_ctrl_r_resp_o   = 2'b00;

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            ctrl_reg             <= 32'h0;
            boot_addr_reg        <= 32'h0;
            manifest_lo_reg      <= 32'h0;
            manifest_hi_reg      <= 32'h0;
            manifest_size_reg    <= 32'h0;
            cluster_mask_lo_reg  <= (NUM_CLUSTERS >= 32) ? 32'hFFFF_FFFF : ((32'h1 << NUM_CLUSTERS) - 1);
            cluster_mask_hi_reg  <= (NUM_CLUSTERS > 32) ? ((32'h1 << (NUM_CLUSTERS - 32)) - 1) : 32'h0;
            trace_base_reg       <= 32'h0;
            trace_size_reg       <= 32'h0;
            trace_ctrl_reg       <= 32'h0;
            irq_summary_sticky_reg <= 32'h0;
            wr_addr_valid_reg    <= 1'b0;
            wr_addr_reg          <= 32'h0;
            s_ctrl_b_valid_o     <= 1'b0;
            s_ctrl_r_valid_o     <= 1'b0;
            s_ctrl_r_data_o      <= 32'h0;
            loader_kick_o        <= 1'b0;
        end else begin
            loader_kick_o <= 1'b0;

            irq_summary_sticky_reg <= irq_summary_word(irq_summary_sticky_reg, loader_done_i, loader_err_code_i, core_cause_snapshot_i, core_halted_i, plic_pending_any_i);

            if (s_ctrl_aw_valid_i && s_ctrl_aw_ready_o) begin
                wr_addr_valid_reg <= 1'b1;
                wr_addr_reg <= s_ctrl_aw_addr_i;
            end

            if ((wr_addr_valid_reg || s_ctrl_aw_valid_i) && s_ctrl_w_valid_i && s_ctrl_w_ready_o) begin
                logic [31:0] target_addr;
                target_addr = wr_addr_valid_reg ? wr_addr_reg : s_ctrl_aw_addr_i;
                unique0 case (target_addr[11:0])
                    HACC_CTRL: begin
                        ctrl_reg <= apply_wstrb32(ctrl_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CORE_BOOT_ADDR: begin
                        boot_addr_reg <= apply_wstrb32(boot_addr_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    MANIFEST_ADDR_LO: begin
                        manifest_lo_reg <= apply_wstrb32(manifest_lo_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    MANIFEST_ADDR_HI: begin
                        manifest_hi_reg <= apply_wstrb32(manifest_hi_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    MANIFEST_SIZE: begin
                        manifest_size_reg <= apply_wstrb32(manifest_size_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    MANIFEST_KICK: begin
                        if (s_ctrl_w_data_i[0]) loader_kick_o <= 1'b1;
                    end
                    IRQ_FORCE_ACK: begin
                        irq_summary_sticky_reg <= irq_summary_sticky_reg & ~s_ctrl_w_data_i;
                    end
                    CLUSTER_MASK_LO: begin
                        cluster_mask_lo_reg <= apply_wstrb32(cluster_mask_lo_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CLUSTER_MASK_HI: begin
                        cluster_mask_hi_reg <= apply_wstrb32(cluster_mask_hi_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    TRACE_BASE: begin
                        trace_base_reg <= apply_wstrb32(trace_base_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    TRACE_SIZE: begin
                        trace_size_reg <= apply_wstrb32(trace_size_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    TRACE_CTRL: begin
                        trace_ctrl_reg <= apply_wstrb32(trace_ctrl_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    default: ;
                endcase
                wr_addr_valid_reg <= 1'b0;
                s_ctrl_b_valid_o <= 1'b1;
            end

            if (s_ctrl_b_valid_o && s_ctrl_b_ready_i) begin
                s_ctrl_b_valid_o <= 1'b0;
            end

            if (s_ctrl_ar_valid_i && s_ctrl_ar_ready_o) begin
                s_ctrl_r_valid_o <= 1'b1;
                s_ctrl_r_data_o  <= read_csr({20'h0, s_ctrl_ar_addr_i[11:0]});
            end else if (s_ctrl_r_valid_o && s_ctrl_r_ready_i) begin
                s_ctrl_r_valid_o <= 1'b0;
            end
        end
    end

endmodule