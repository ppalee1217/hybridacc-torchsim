//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_cluster_control_unit
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Unit test for ClusterControlUnit.
// Dependencies:  ../tb_common.svh, src/Cluster/cluster_pkg.sv,
//                src/Cluster/ClusterControlUnit.sv
// Revision:
//   2026/04/27 - Initial version (M2 cluster control rewrite)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/Cluster/cluster_pkg.sv"
`include "../../src/Cluster/ClusterControlUnit.sv"
`endif

module tb_cluster_control_unit;
    import cluster_pkg::*;

    logic clk, reset_n;
    logic write_mode_i;
    logic [31:0] mode_wdata_i;
    logic write_ctrl_i;
    logic [31:0] ctrl_wdata_i;
    logic write_error_i;
    logic [31:0] error_wdata_i;
    logic notify_direct_start_i;
    logic notify_direct_stop_i;
    logic notify_direct_reset_i;
    logic noc_quiesced_i;
    logic spm_quiesced_i;
    logic [1:0] noc_action_o;
    logic spm_soft_reset_o;
    cluster_mode_e mode_o;
    cluster_substate_e substate_o;
    logic [31:0] error_code_o;
    logic layer_active_o;
    logic stop_pending_o;
    logic soft_reset_pending_o;
    logic done_sticky_o;
    logic [31:0] status_word_o;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    ClusterControlUnit dut (
        .clk(clk),
        .reset_n(reset_n),
        .write_mode_i(write_mode_i),
        .mode_wdata_i(mode_wdata_i),
        .write_ctrl_i(write_ctrl_i),
        .ctrl_wdata_i(ctrl_wdata_i),
        .write_error_i(write_error_i),
        .error_wdata_i(error_wdata_i),
        .notify_direct_start_i(notify_direct_start_i),
        .notify_direct_stop_i(notify_direct_stop_i),
        .notify_direct_reset_i(notify_direct_reset_i),
        .noc_quiesced_i(noc_quiesced_i),
        .spm_quiesced_i(spm_quiesced_i),
        .noc_action_o(noc_action_o),
        .spm_soft_reset_o(spm_soft_reset_o),
        .mode_o(mode_o),
        .substate_o(substate_o),
        .error_code_o(error_code_o),
        .layer_active_o(layer_active_o),
        .stop_pending_o(stop_pending_o),
        .soft_reset_pending_o(soft_reset_pending_o),
        .done_sticky_o(done_sticky_o),
        .status_word_o(status_word_o)
    );

    task automatic pulse_mode(input logic [31:0] data);
        @(negedge clk);
        write_mode_i = 1'b1;
        mode_wdata_i = data;
        @(posedge clk);
        @(negedge clk);
        write_mode_i = 1'b0;
    endtask

    task automatic pulse_ctrl(input logic [31:0] data);
        @(negedge clk);
        write_ctrl_i = 1'b1;
        ctrl_wdata_i = data;
        @(posedge clk);
        @(negedge clk);
        write_ctrl_i = 1'b0;
    endtask

    task automatic pulse_ctrl_capture(
        input logic [31:0] data,
        output logic [1:0] action_seen,
        output logic spm_soft_reset_seen
    );
        begin
            @(negedge clk);
            write_ctrl_i = 1'b1;
            ctrl_wdata_i = data;
            @(posedge clk);
            #(`TB_SETTLE);
            action_seen = noc_action_o;
            spm_soft_reset_seen = spm_soft_reset_o;
            @(negedge clk);
            write_ctrl_i = 1'b0;
        end
    endtask

    initial begin
        logic [1:0] action_seen;
        logic       spm_soft_reset_seen;

        write_mode_i = 0;
        mode_wdata_i = 0;
        write_ctrl_i = 0;
        ctrl_wdata_i = 0;
        write_error_i = 0;
        error_wdata_i = 0;
        notify_direct_start_i = 0;
        notify_direct_stop_i = 0;
        notify_direct_reset_i = 0;
        noc_quiesced_i = 1;
        spm_quiesced_i = 1;

        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        `CHECK_VAL("Reset mode direct", mode_o, MODE_DIRECT_DEBUG)
        `CHECK_VAL("Reset substate idle", substate_o, SUBSTATE_IDLE)

        // Layer managed start
        pulse_mode(MODE_LAYER_MANAGED);
        pulse_ctrl_capture(32'h1 << CTRL_START, action_seen, spm_soft_reset_seen);
        #(`TB_SETTLE);
        `CHECK_VAL("LM start action", action_seen, CLUSTER_ACTION_NOC_START)
        `CHECK_VAL("LM start substate", substate_o, SUBSTATE_RUNNING)
        `CHECK_BIT("LM start layer_active", layer_active_o, 1'b1)

        // Layer managed soft reset while active -> STOP then quiesced RESET pulse
        noc_quiesced_i = 1'b0;
        pulse_ctrl_capture(32'h1 << CTRL_SOFT_RESET, action_seen, spm_soft_reset_seen);
        #(`TB_SETTLE);
        `CHECK_VAL("LM soft reset first action stop", action_seen, CLUSTER_ACTION_NOC_STOP)
        `CHECK_VAL("LM soft reset wait state", substate_o, SUBSTATE_WAIT_QUIESCED)
        noc_quiesced_i = 1'b1;
        #(`TB_SETTLE);
        `CHECK_VAL("Background reset action", noc_action_o, CLUSTER_ACTION_NOC_RESET)
        `CHECK_BIT("Background spm soft reset", spm_soft_reset_o, 1'b1)
        @(posedge clk); #(`TB_SETTLE);
        `CHECK_VAL("After quiesce substate idle", substate_o, SUBSTATE_IDLE)

        // Direct mode notify start/reset
        pulse_mode(MODE_DIRECT_DEBUG);
        @(negedge clk); notify_direct_start_i = 1'b1;
        @(posedge clk); @(negedge clk); notify_direct_start_i = 1'b0;
        `CHECK_VAL("Direct start substate", substate_o, SUBSTATE_RUNNING)
        @(negedge clk); notify_direct_reset_i = 1'b1;
        @(posedge clk); @(negedge clk); notify_direct_reset_i = 1'b0;
        `CHECK_VAL("Direct reset substate", substate_o, SUBSTATE_IDLE)
        `CHECK_BIT("Direct reset done sticky", done_sticky_o, 1'b1)

        `CHECK_COND("Status idle or done bits visible", status_word_o[STATUS_IDLE] || status_word_o[STATUS_DONE], status_word_o)

        `TB_SUMMARY("tb_cluster_control_unit")
        $finish;
    end

    initial begin
        #200000;
        $error("[TB_TIMEOUT] tb_cluster_control_unit did not finish in time");
        `TB_SUMMARY("tb_cluster_control_unit")
        $finish;
    end

endmodule