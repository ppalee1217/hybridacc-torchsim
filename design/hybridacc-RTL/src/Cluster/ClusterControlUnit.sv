//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   ClusterControlUnit
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Cluster lifecycle control FSM translated from
//                Cluster/ClusterControlUnit.hpp.
// Dependencies:  src/Cluster/cluster_pkg.sv
// Revision:
//   2026/04/27 - Initial version (M2 cluster control rewrite)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import cluster_pkg::*;

module ClusterControlUnit (
    input  logic             clk,
    input  logic             reset_n,

    input  logic             write_mode_i,
    input  logic [31:0]      mode_wdata_i,
    input  logic             write_ctrl_i,
    input  logic [31:0]      ctrl_wdata_i,
    input  logic             write_error_i,
    input  logic [31:0]      error_wdata_i,

    input  logic             notify_direct_start_i,
    input  logic             notify_direct_stop_i,
    input  logic             notify_direct_reset_i,

    input  logic             noc_quiesced_i,
    input  logic             spm_quiesced_i,

    output logic [1:0]       noc_action_o,
    output logic             spm_soft_reset_o,
    output cluster_mode_e    mode_o,
    output cluster_substate_e substate_o,
    output logic [31:0]      error_code_o,
    output logic             layer_active_o,
    output logic             stop_pending_o,
    output logic             soft_reset_pending_o,
    output logic             done_sticky_o,
    output logic [31:0]      status_word_o
);
    cluster_mode_e     mode_reg;
    cluster_substate_e substate_reg;
    logic [31:0]       error_code_reg;
    logic              layer_active_reg;
    logic              stop_pending_reg;
    logic              soft_reset_pending_reg;
    logic              done_sticky_reg;

    assign mode_o               = mode_reg;
    assign substate_o           = substate_reg;
    assign error_code_o         = error_code_reg;
    assign layer_active_o       = layer_active_reg;
    assign stop_pending_o       = stop_pending_reg;
    assign soft_reset_pending_o = soft_reset_pending_reg;
    assign done_sticky_o        = done_sticky_reg;

    always_comb begin
        logic busy_w;
        logic quiesced_w;

        busy_w     = layer_active_reg || stop_pending_reg || soft_reset_pending_reg;
        quiesced_w = !busy_w && noc_quiesced_i && spm_quiesced_i;

        status_word_o = 32'h0;
        if (!busy_w)          status_word_o[STATUS_IDLE]     = 1'b1;
        if (busy_w)           status_word_o[STATUS_BUSY]     = 1'b1;
        if (done_sticky_reg)  status_word_o[STATUS_DONE]     = 1'b1;
        if (quiesced_w)       status_word_o[STATUS_QUIESCED] = 1'b1;
        if (error_code_reg != 32'h0) status_word_o[STATUS_ERROR] = 1'b1;

        noc_action_o      = CLUSTER_ACTION_NONE;
        spm_soft_reset_o  = 1'b0;

        if (stop_pending_reg && noc_quiesced_i) begin
            if (soft_reset_pending_reg) begin
                noc_action_o     = CLUSTER_ACTION_NOC_RESET;
                spm_soft_reset_o = 1'b1;
            end
        end

        if (write_ctrl_i && (mode_reg == MODE_LAYER_MANAGED)) begin
            if (ctrl_wdata_i[CTRL_SOFT_RESET]) begin
                if (layer_active_reg || !noc_quiesced_i) begin
                    noc_action_o = CLUSTER_ACTION_NOC_STOP;
                end else begin
                    noc_action_o     = CLUSTER_ACTION_NOC_RESET;
                    spm_soft_reset_o = 1'b1;
                end
            end else if (ctrl_wdata_i[CTRL_STOP]) begin
                noc_action_o = CLUSTER_ACTION_NOC_STOP;
            end else if (ctrl_wdata_i[CTRL_START]) begin
                noc_action_o = CLUSTER_ACTION_NOC_START;
            end
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            mode_reg               <= MODE_DIRECT_DEBUG;
            substate_reg           <= SUBSTATE_IDLE;
            error_code_reg         <= 32'h0;
            layer_active_reg       <= 1'b0;
            stop_pending_reg       <= 1'b0;
            soft_reset_pending_reg <= 1'b0;
            done_sticky_reg        <= 1'b0;
        end else begin
            if (stop_pending_reg && noc_quiesced_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                    $display("[%0t] [TRACE][CCU] quiesced_complete soft_reset=%0b done_sticky->1",
                             $time,
                             soft_reset_pending_reg);
                end
                // synopsys translate_on
                if (soft_reset_pending_reg) begin
                    soft_reset_pending_reg <= 1'b0;
                end
                layer_active_reg <= 1'b0;
                stop_pending_reg <= 1'b0;
                done_sticky_reg  <= 1'b1;
                substate_reg     <= SUBSTATE_IDLE;
                error_code_reg   <= 32'h0;
            end

            if (write_mode_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO")) begin
                    $display("[%0t] [TRACE][CCU] write_mode old=%0d new=%0d",
                             $time,
                             mode_reg,
                             (mode_wdata_i == MODE_LAYER_MANAGED) ? MODE_LAYER_MANAGED : MODE_DIRECT_DEBUG);
                end
                // synopsys translate_on
                if (mode_wdata_i == MODE_LAYER_MANAGED) begin
                    mode_reg <= MODE_LAYER_MANAGED;
                end else begin
                    mode_reg <= MODE_DIRECT_DEBUG;
                end
            end

            if (write_ctrl_i && (mode_reg == MODE_LAYER_MANAGED)) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO")) begin
                    $display("[%0t] [TRACE][CCU] write_ctrl data=0x%08x start=%0b stop=%0b soft_reset=%0b",
                             $time,
                             ctrl_wdata_i,
                             ctrl_wdata_i[CTRL_START],
                             ctrl_wdata_i[CTRL_STOP],
                             ctrl_wdata_i[CTRL_SOFT_RESET]);
                end
                // synopsys translate_on
                if (ctrl_wdata_i[CTRL_SOFT_RESET]) begin
                    done_sticky_reg <= 1'b0;
                    if (layer_active_reg || !noc_quiesced_i) begin
                        stop_pending_reg       <= 1'b1;
                        soft_reset_pending_reg <= 1'b1;
                        substate_reg           <= SUBSTATE_WAIT_QUIESCED;
                    end else begin
                        layer_active_reg       <= 1'b0;
                        stop_pending_reg       <= 1'b0;
                        soft_reset_pending_reg <= 1'b0;
                        done_sticky_reg        <= 1'b1;
                        substate_reg           <= SUBSTATE_IDLE;
                        error_code_reg         <= 32'h0;
                    end
                end else if (ctrl_wdata_i[CTRL_STOP]) begin
                    stop_pending_reg       <= 1'b1;
                    soft_reset_pending_reg <= 1'b0;
                    done_sticky_reg        <= 1'b0;
                    substate_reg           <= SUBSTATE_STOPPING;
                end else if (ctrl_wdata_i[CTRL_START]) begin
                    layer_active_reg       <= 1'b1;
                    stop_pending_reg       <= 1'b0;
                    soft_reset_pending_reg <= 1'b0;
                    done_sticky_reg        <= 1'b0;
                    substate_reg           <= SUBSTATE_RUNNING;
                    error_code_reg         <= 32'h0;
                end
            end

            if (write_error_i && (error_wdata_i == 32'h0)) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_MMIO")) begin
                    $display("[%0t] [TRACE][CCU] clear_error", $time);
                end
                // synopsys translate_on
                error_code_reg <= 32'h0;
            end

            if (notify_direct_start_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                    $display("[%0t] [TRACE][CCU] direct_start", $time);
                end
                // synopsys translate_on
                layer_active_reg       <= 1'b1;
                stop_pending_reg       <= 1'b0;
                soft_reset_pending_reg <= 1'b0;
                done_sticky_reg        <= 1'b0;
                substate_reg           <= SUBSTATE_RUNNING;
                error_code_reg         <= 32'h0;
            end
            if (notify_direct_stop_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                    $display("[%0t] [TRACE][CCU] direct_stop", $time);
                end
                // synopsys translate_on
                stop_pending_reg <= 1'b1;
                done_sticky_reg  <= 1'b0;
                substate_reg     <= SUBSTATE_STOPPING;
            end
            if (notify_direct_reset_i) begin
                // synopsys translate_off
                if ($test$plusargs("TRACE_CLUSTER_DEBUG") || $test$plusargs("TRACE_CLUSTER_RUNTIME")) begin
                    $display("[%0t] [TRACE][CCU] direct_reset", $time);
                end
                // synopsys translate_on
                layer_active_reg       <= 1'b0;
                stop_pending_reg       <= 1'b0;
                soft_reset_pending_reg <= 1'b0;
                done_sticky_reg        <= 1'b1;
                substate_reg           <= SUBSTATE_IDLE;
                error_code_reg         <= 32'h0;
            end
        end
    end

endmodule