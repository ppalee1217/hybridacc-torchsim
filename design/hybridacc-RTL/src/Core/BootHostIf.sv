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
    localparam logic [31:0] DEFAULT_CLUSTER_MASK_LO = (NUM_CLUSTERS >= 32) ? 32'hFFFF_FFFF : ((32'h1 << NUM_CLUSTERS) - 1);
    localparam logic [31:0] DEFAULT_CLUSTER_MASK_HI = (NUM_CLUSTERS > 32) ? ((32'h1 << (NUM_CLUSTERS - 32)) - 1) : 32'h0;
    localparam logic [11:0] CSR_HACC_CAP0           = HACC_CAP0[11:0];
    localparam logic [11:0] CSR_HACC_CAP1           = HACC_CAP1[11:0];
    localparam logic [11:0] CSR_HACC_CTRL           = HACC_CTRL[11:0];
    localparam logic [11:0] CSR_HACC_STATUS         = HACC_STATUS[11:0];
    localparam logic [11:0] CSR_CORE_BOOT_ADDR      = CORE_BOOT_ADDR[11:0];
    localparam logic [11:0] CSR_CORE_PC_SNAPSHOT    = CORE_PC_SNAPSHOT[11:0];
    localparam logic [11:0] CSR_CORE_CAUSE_SNAPSHOT = CORE_CAUSE_SNAPSHOT[11:0];
    localparam logic [11:0] CSR_MANIFEST_ADDR_LO    = MANIFEST_ADDR_LO[11:0];
    localparam logic [11:0] CSR_MANIFEST_ADDR_HI    = MANIFEST_ADDR_HI[11:0];
    localparam logic [11:0] CSR_MANIFEST_SIZE       = MANIFEST_SIZE[11:0];
    localparam logic [11:0] CSR_MANIFEST_KICK       = MANIFEST_KICK[11:0];
    localparam logic [11:0] CSR_LOADER_STATUS       = LOADER_STATUS[11:0];
    localparam logic [11:0] CSR_LOADER_ERR_CODE     = LOADER_ERR_CODE[11:0];
    localparam logic [11:0] CSR_LOADER_ERR_INFO     = LOADER_ERR_INFO[11:0];
    localparam logic [11:0] CSR_IRQ_SUMMARY         = IRQ_SUMMARY[11:0];
    localparam logic [11:0] CSR_IRQ_FORCE_ACK       = IRQ_FORCE_ACK[11:0];
    localparam logic [11:0] CSR_CLUSTER_MASK_LO     = CLUSTER_MASK_LO[11:0];
    localparam logic [11:0] CSR_CLUSTER_MASK_HI     = CLUSTER_MASK_HI[11:0];
    localparam logic [11:0] CSR_LAST_MMIO_TARGET    = LAST_MMIO_TARGET[11:0];
    localparam logic [11:0] CSR_LAST_MMIO_ADDR      = LAST_MMIO_ADDR[11:0];
    localparam logic [11:0] CSR_TRACE_BASE          = TRACE_BASE[11:0];
    localparam logic [11:0] CSR_TRACE_SIZE          = TRACE_SIZE[11:0];
    localparam logic [11:0] CSR_TRACE_CTRL          = TRACE_CTRL[11:0];
    localparam logic [11:0] CSR_TRACE_STATUS        = TRACE_STATUS[11:0];

    logic [31:0] ctrl_reg;
    logic [31:0] boot_addr_reg;
    logic [31:0] manifest_lo_reg;
    logic [31:0] manifest_hi_reg;
    logic [31:0] manifest_size_reg;
    logic [31:0] cluster_mask_lo_reg;
    logic [31:0] cluster_mask_hi_reg;
    logic [31:0] cluster_mask_lo_w;
    logic [31:0] cluster_mask_hi_w;
    logic [31:0] trace_base_reg;
    logic [31:0] trace_size_reg;
    logic [31:0] trace_ctrl_reg;
    logic [31:0] irq_summary_sticky_reg;
    logic        reset_init_done_reg;
    logic [31:0] read_csr_rdata_w;
    logic        ctrl_aw_ready_w;
    logic        ctrl_w_ready_w;
    logic        ctrl_ar_ready_w;
    logic        ctrl_b_valid_reg;
    logic        ctrl_r_valid_reg;
    logic [31:0] ctrl_r_data_reg;

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
        if (loader_busy) begin
            status[0] = 1'b1;
        end
        if (loader_done) begin
            status[1] = 1'b1;
        end
        if (core_halted) begin
            status[2] = 1'b1;
        end
        if (core_running) begin
            status[3] = 1'b1;
        end
        if (loader_err_code != 32'h0) begin
            status[4] = 1'b1;
        end
        if (loader_busy) begin
            status[5] = 1'b1;
        end
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
        if (loader_done) begin
            summary[0] = 1'b1;
        end
        if (loader_err_code != 32'h0) begin
            summary[1] = 1'b1;
        end
        if (core_cause != 32'h0) begin
            summary[2] = 1'b1;
        end
        if (core_halted) begin
            summary[3] = 1'b1;
        end
        if (plic_pending_any) begin
            summary[4] = 1'b1;
        end
        return summary;
    endfunction

    assign cluster_mask_lo_w = reset_init_done_reg ? cluster_mask_lo_reg : DEFAULT_CLUSTER_MASK_LO;
    assign cluster_mask_hi_w = reset_init_done_reg ? cluster_mask_hi_reg : DEFAULT_CLUSTER_MASK_HI;

    assign core_enable_o     = ctrl_reg[0];
    assign core_haltreq_o    = ctrl_reg[1];
    assign boot_addr_o       = boot_addr_reg;
    assign load_phase_o      = loader_busy_i;
    assign manifest_addr_lo_o= manifest_lo_reg;
    assign manifest_addr_hi_o= manifest_hi_reg;
    assign manifest_size_o   = manifest_size_reg;
    assign cluster_mask_lo_o = cluster_mask_lo_w;
    assign cluster_mask_hi_o = cluster_mask_hi_w;
    assign controller_irq_o  = |irq_summary_word(irq_summary_sticky_reg, loader_done_i, loader_err_code_i, core_cause_snapshot_i, core_halted_i, plic_pending_any_i);

    assign ctrl_aw_ready_w   = !wr_addr_valid_reg && !ctrl_b_valid_reg;
    assign ctrl_w_ready_w    = (wr_addr_valid_reg || s_ctrl_aw_valid_i) && !ctrl_b_valid_reg;
    assign ctrl_ar_ready_w   = !ctrl_r_valid_reg && (s_ctrl_ar_addr_i[31:12] === s_ctrl_ar_addr_i[31:12]);
    assign s_ctrl_aw_ready_o = ctrl_aw_ready_w;
    assign s_ctrl_w_ready_o  = ctrl_w_ready_w;
    assign s_ctrl_ar_ready_o = ctrl_ar_ready_w;
    assign s_ctrl_b_valid_o  = ctrl_b_valid_reg;
    assign s_ctrl_r_valid_o  = ctrl_r_valid_reg;
    assign s_ctrl_r_data_o   = ctrl_r_data_reg;
    assign s_ctrl_b_resp_o   = 2'b00;
    assign s_ctrl_r_resp_o   = 2'b00;
    wire [11:0] ctrl_write_addr_w = wr_addr_valid_reg ? wr_addr_reg[11:0] : s_ctrl_aw_addr_i[11:0];

    always_comb begin
        read_csr_rdata_w = 32'h0;
        unique0 case (s_ctrl_ar_addr_i[11:0])
            CSR_HACC_CAP0:           read_csr_rdata_w = cap0_value();
            CSR_HACC_CAP1:           read_csr_rdata_w = cap1_value();
            CSR_HACC_CTRL:           read_csr_rdata_w = ctrl_reg;
            CSR_HACC_STATUS:         read_csr_rdata_w = hacc_status_word(loader_busy_i, loader_done_i, core_halted_i, core_running_i, loader_err_code_i);
            CSR_CORE_BOOT_ADDR:      read_csr_rdata_w = boot_addr_reg;
            CSR_CORE_PC_SNAPSHOT:    read_csr_rdata_w = core_pc_snapshot_i;
            CSR_CORE_CAUSE_SNAPSHOT: read_csr_rdata_w = core_cause_snapshot_i;
            CSR_MANIFEST_ADDR_LO:    read_csr_rdata_w = manifest_lo_reg;
            CSR_MANIFEST_ADDR_HI:    read_csr_rdata_w = manifest_hi_reg;
            CSR_MANIFEST_SIZE:       read_csr_rdata_w = manifest_size_reg;
            CSR_LOADER_STATUS:       read_csr_rdata_w = loader_status_i;
            CSR_LOADER_ERR_CODE:     read_csr_rdata_w = loader_err_code_i;
            CSR_LOADER_ERR_INFO:     read_csr_rdata_w = loader_err_info_i;
            CSR_IRQ_SUMMARY:         read_csr_rdata_w = irq_summary_word(irq_summary_sticky_reg, loader_done_i, loader_err_code_i, core_cause_snapshot_i, core_halted_i, plic_pending_any_i);
            CSR_CLUSTER_MASK_LO:     read_csr_rdata_w = cluster_mask_lo_w;
            CSR_CLUSTER_MASK_HI:     read_csr_rdata_w = cluster_mask_hi_w;
            CSR_LAST_MMIO_TARGET:    read_csr_rdata_w = fabric_last_target_i;
            CSR_LAST_MMIO_ADDR:      read_csr_rdata_w = fabric_last_addr_i;
            CSR_TRACE_BASE:          read_csr_rdata_w = trace_base_reg;
            CSR_TRACE_SIZE:          read_csr_rdata_w = trace_size_reg;
            CSR_TRACE_CTRL:          read_csr_rdata_w = trace_ctrl_reg;
            CSR_TRACE_STATUS:        read_csr_rdata_w = 32'h0;
            default:             read_csr_rdata_w = 32'h0;
        endcase
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            ctrl_reg             <= 32'h0;
            boot_addr_reg        <= 32'h0;
            manifest_lo_reg      <= 32'h0;
            manifest_hi_reg      <= 32'h0;
            manifest_size_reg    <= 32'h0;
            cluster_mask_lo_reg  <= 32'h0;
            cluster_mask_hi_reg  <= 32'h0;
            trace_base_reg       <= 32'h0;
            trace_size_reg       <= 32'h0;
            trace_ctrl_reg       <= 32'h0;
            irq_summary_sticky_reg <= 32'h0;
            wr_addr_valid_reg    <= 1'b0;
            wr_addr_reg          <= 32'h0;
            ctrl_b_valid_reg     <= 1'b0;
            ctrl_r_valid_reg     <= 1'b0;
            ctrl_r_data_reg      <= 32'h0;
            loader_kick_o        <= 1'b0;
            reset_init_done_reg  <= 1'b0;
        end else if (!reset_init_done_reg) begin
            cluster_mask_lo_reg  <= DEFAULT_CLUSTER_MASK_LO;
            cluster_mask_hi_reg  <= DEFAULT_CLUSTER_MASK_HI;
            reset_init_done_reg  <= 1'b1;
        end else begin
            loader_kick_o <= 1'b0;

            irq_summary_sticky_reg <= irq_summary_word(irq_summary_sticky_reg, loader_done_i, loader_err_code_i, core_cause_snapshot_i, core_halted_i, plic_pending_any_i);

            if (s_ctrl_aw_valid_i && ctrl_aw_ready_w) begin
                wr_addr_valid_reg <= 1'b1;
                wr_addr_reg <= s_ctrl_aw_addr_i;
            end

            if ((wr_addr_valid_reg || s_ctrl_aw_valid_i) && s_ctrl_w_valid_i && ctrl_w_ready_w) begin
                unique0 case (ctrl_write_addr_w)
                    CSR_HACC_CTRL: begin
                        ctrl_reg <= apply_wstrb32(ctrl_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_CORE_BOOT_ADDR: begin
                        boot_addr_reg <= apply_wstrb32(boot_addr_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_MANIFEST_ADDR_LO: begin
                        manifest_lo_reg <= apply_wstrb32(manifest_lo_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_MANIFEST_ADDR_HI: begin
                        manifest_hi_reg <= apply_wstrb32(manifest_hi_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_MANIFEST_SIZE: begin
                        manifest_size_reg <= apply_wstrb32(manifest_size_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_MANIFEST_KICK: begin
                        if (s_ctrl_w_data_i[0]) begin
                            loader_kick_o <= 1'b1;
                        end
                    end
                    CSR_IRQ_FORCE_ACK: begin
                        irq_summary_sticky_reg <= irq_summary_sticky_reg & ~s_ctrl_w_data_i;
                    end
                    CSR_CLUSTER_MASK_LO: begin
                        cluster_mask_lo_reg <= apply_wstrb32(cluster_mask_lo_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_CLUSTER_MASK_HI: begin
                        cluster_mask_hi_reg <= apply_wstrb32(cluster_mask_hi_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_TRACE_BASE: begin
                        trace_base_reg <= apply_wstrb32(trace_base_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_TRACE_SIZE: begin
                        trace_size_reg <= apply_wstrb32(trace_size_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    CSR_TRACE_CTRL: begin
                        trace_ctrl_reg <= apply_wstrb32(trace_ctrl_reg, s_ctrl_w_data_i, s_ctrl_w_strb_i);
                    end
                    default: ;
                endcase
                wr_addr_valid_reg <= 1'b0;
                ctrl_b_valid_reg <= 1'b1;
            end

            if (ctrl_b_valid_reg && s_ctrl_b_ready_i) begin
                ctrl_b_valid_reg <= 1'b0;
            end

            if (s_ctrl_ar_valid_i && ctrl_ar_ready_w) begin
                ctrl_r_valid_reg <= 1'b1;
                ctrl_r_data_reg  <= read_csr_rdata_w;
            end else if (ctrl_r_valid_reg && s_ctrl_r_ready_i) begin
                ctrl_r_valid_reg <= 1'b0;
            end
        end
    end

endmodule