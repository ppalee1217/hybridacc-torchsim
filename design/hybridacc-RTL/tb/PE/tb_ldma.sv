//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_ldma
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Testbench for ldma module.
// Dependencies:  tb_common.svh, src/hybridacc_utils_pkg.sv, src/PE/LDMA.sv
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`ifndef GATE_SIM
`include "../../src/PE/LDMA.sv"
`endif

module tb_ldma;
    import hybridacc_utils_pkg::*;

    localparam logic [2:0] ST_IDLE          = 3'd0;
    localparam logic [2:0] ST_LOAD_PRE      = 3'd1;
    localparam logic [2:0] ST_LOAD_WAIT     = 3'd2;
    localparam logic [2:0] ST_LOAD_PIPELINE = 3'd3;
    localparam logic [2:0] ST_DONE          = 3'd4;

    logic clk, reset_n;
    logic [15:0] imm;
    logic [2:0]  mode;
    logic [8:0]  dm_read_addr;
    logic set_addr, set_len, set_loop, set_mode, active, next, reset_active;
    logic dl_stall_out;
    logic [63:0] dm_read_data;
    v_fp16_t dmrv_out;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    LDMA dut(
        .clk(clk), .reset_n(reset_n), .imm(imm), .set_addr(set_addr), .set_len(set_len), .set_loop(set_loop), .set_mode(set_mode),
        .mode(mode), .active(active), .next(next), .reset_active(reset_active), .dl_stall_out(dl_stall_out),
        .dm_read_addr(dm_read_addr), .dm_read_data(dm_read_data), .dmrv_out(dmrv_out)
    );

`ifdef GATE_SIM
initial begin
    $sdf_annotate("syn/LDMA/LDMA.sdf", dut);
end
`endif


    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    task automatic pulse_set_addr(input logic [15:0] value);
        begin
            imm = value;
            set_addr = 1'b1;
            @(posedge clk);
            set_addr = 1'b0;
        end
    endtask

    task automatic pulse_set_len(input logic [15:0] value);
        begin
            imm = value;
            set_len = 1'b1;
            @(posedge clk);
            set_len = 1'b0;
        end
    endtask

    task automatic pulse_set_loop(input logic [15:0] value);
        begin
            imm = value;
            set_loop = 1'b1;
            @(posedge clk);
            set_loop = 1'b0;
        end
    endtask

    task automatic pulse_set_mode(
        input logic [2:0]  mode_value,
        input logic [15:0] stride_value
    );
        begin
            mode = mode_value;
            imm = stride_value;
            set_mode = 1'b1;
            @(posedge clk);
            set_mode = 1'b0;
        end
    endtask

    task automatic pulse_reset_active();
        begin
            reset_active = 1'b1;
            @(posedge clk);
            reset_active = 1'b0;
        end
    endtask

    task automatic drain_to_idle();
        begin
            for (int cycle = 0; cycle < 8; cycle++) begin
                if (dut.state_reg == ST_IDLE) begin
                    break;
                end
                next = (dut.state_reg == ST_LOAD_PIPELINE);
                @(posedge clk);
                next = 1'b0;
            end
            @(negedge clk);
        end
    endtask


    initial begin
        imm=0; mode=3'd0; set_addr=0; set_len=0; set_loop=0; set_mode=0; active=0; next=0; reset_active=0;
        dm_read_data = 64'h0;
        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // Test 1: Reset state
        `CHECK_VAL("Reset: state=IDLE", dut.state_reg, ST_IDLE)
        `CHECK_VAL("Reset: dm_read_addr=0", dm_read_addr, 9'h000)
        `CHECK_VAL("Reset: dmrv_out=0", dmrv_out, '0)
        `CHECK_BIT("Reset: dl_stall_out=0", dl_stall_out, 1'b0)

        // Test 2: LOAD_DWORD sequence (len=2, loop=1, stride=1)
        pulse_set_addr(16'h0010);
        pulse_set_len(16'h0002);
        pulse_set_loop(16'h0001);
        pulse_set_mode(3'd3, 16'd1);
        @(negedge clk);

        // Activate: first visible request is base address while still in IDLE
        dm_read_data = 64'hAAAA_BBBB_CCCC_DDDD;
        active = 1;
        #(`TB_SETTLE);
        `CHECK_VAL("Activate: first read addr=0x10", dm_read_addr, 9'h010)
        @(posedge clk);
        active = 0;
        @(negedge clk);
        `CHECK_VAL("Activate: state=LOAD_WAIT", dut.state_reg, ST_LOAD_WAIT)
        `CHECK_VAL("Activate: next read addr=0x12", dm_read_addr, 9'h012)

        // LOAD_WAIT captures first data word and enters LOAD_PIPELINE
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("LoadWait: state=LOAD_PIPELINE", dut.state_reg, ST_LOAD_PIPELINE)
        `CHECK_VAL("LoadWait: lane0=0xDDDD", dmrv_out.lanes[0], 16'hDDDD)
        `CHECK_VAL("LoadWait: lane3=0xAAAA", dmrv_out.lanes[3], 16'hAAAA)
        `CHECK_VAL("LoadWait: second read addr=0x12", dm_read_addr, 9'h012)

        // Holding without next keeps pipeline state and output stable
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Hold: state=LOAD_PIPELINE", dut.state_reg, ST_LOAD_PIPELINE)
        `CHECK_VAL("Hold: lane0 still first dword", dmrv_out.lanes[0], 16'hDDDD)
        `CHECK_BIT("Hold: dl_stall_out=0", dl_stall_out, 1'b0)

        // Next pulse captures the second data word and advances address
        dm_read_data = 64'h1111_2222_3333_4444;
        next = 1;
        @(posedge clk);
        next = 0;
        @(negedge clk);
        `CHECK_VAL("Next1: state=LOAD_PIPELINE", dut.state_reg, ST_LOAD_PIPELINE)
        `CHECK_VAL("Next1: lane0=0x4444", dmrv_out.lanes[0], 16'h4444)
        `CHECK_VAL("Next1: lane3=0x1111", dmrv_out.lanes[3], 16'h1111)
        `CHECK_VAL("Next1: post-consume addr=0x14", dm_read_addr, 9'h014)

        // Final next pulse moves the FSM to DONE then back to IDLE
        dm_read_data = 64'h5555_6666_7777_8888;
        next = 1;
        @(posedge clk);
        next = 0;
        @(negedge clk);
        `CHECK_VAL("Finish: state=DONE", dut.state_reg, ST_DONE)
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Finish: state=IDLE", dut.state_reg, ST_IDLE)

        // Test 3: reset_active aborts in-flight work and clears runtime address
        pulse_set_addr(16'h0020);
        pulse_set_len(16'h0001);
        pulse_set_loop(16'h0001);
        pulse_set_mode(3'd3, 16'd1);
        @(negedge clk);
        active = 1;
        @(posedge clk);
        active = 0;
        @(negedge clk);
        `CHECK_VAL("ResetActive: precondition state=LOAD_WAIT", dut.state_reg, ST_LOAD_WAIT)
        pulse_reset_active();
        @(negedge clk);
        `CHECK_VAL("ResetActive: dm_read_addr=0", dm_read_addr, 9'h000)
        drain_to_idle();
        `CHECK_VAL("ResetActive: cleanup state=IDLE", dut.state_reg, ST_IDLE)

        // Test 4: Zero length stays IDLE
        pulse_set_addr(16'h0030);
        pulse_set_len(16'h0000);
        pulse_set_loop(16'h0001);
        pulse_set_mode(3'd3, 16'd1);
        @(negedge clk);
        active = 1;
        @(posedge clk);
        active = 0;
        @(negedge clk);
        `CHECK_VAL("ZeroLen: state=IDLE", dut.state_reg, ST_IDLE)
        `CHECK_VAL("ZeroLen: dm_read_addr holds base", dm_read_addr, 9'h030)

        // Test 5: Broadcast HALF mode (mode=5 => LOAD_HALF + broadcast)
        pulse_set_addr(16'h0040);
        pulse_set_len(16'h0001);
        pulse_set_loop(16'h0001);
        pulse_set_mode(3'd5, 16'd1);
        @(negedge clk);
        dm_read_data = 64'h0000_0000_0000_ABCD; // lower 16 bits = 0xABCD
        active = 1;
        @(posedge clk);
        active = 0;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Broadcast: state=LOAD_PIPELINE", dut.state_reg, ST_LOAD_PIPELINE)
        `CHECK_VAL("Broadcast: lane0=0xABCD", dmrv_out.lanes[0], 16'hABCD)
        `CHECK_VAL("Broadcast: lane1=0xABCD", dmrv_out.lanes[1], 16'hABCD)
        `CHECK_VAL("Broadcast: lane2=0xABCD", dmrv_out.lanes[2], 16'hABCD)
        `CHECK_VAL("Broadcast: lane3=0xABCD", dmrv_out.lanes[3], 16'hABCD)
        next = 1;
        @(posedge clk);
        next = 0;
        @(negedge clk);
        `CHECK_VAL("Broadcast: state=DONE", dut.state_reg, ST_DONE)
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("Broadcast: state=IDLE", dut.state_reg, ST_IDLE)

        // Test 6: LOAD_PIPELINE waits for next while dl_stall_out stays low
        pulse_set_addr(16'h0050);
        pulse_set_len(16'h0001);
        pulse_set_loop(16'h0001);
        pulse_set_mode(3'd3, 16'd1);
        @(negedge clk);
        dm_read_data = 64'hFFFF_FFFF_FFFF_FFFF;
        active = 1;
        @(posedge clk);
        active = 0;
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("WaitNext: state=LOAD_PIPELINE", dut.state_reg, ST_LOAD_PIPELINE)
        `CHECK_BIT("WaitNext: dl_stall_out=0", dl_stall_out, 1'b0)
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("WaitNext: state still LOAD_PIPELINE", dut.state_reg, ST_LOAD_PIPELINE)
        next = 1;
        @(posedge clk);
        next = 0;
        @(negedge clk);
        `CHECK_VAL("WaitNext: state=DONE after final next", dut.state_reg, ST_DONE)
        @(posedge clk); @(negedge clk);
        `CHECK_VAL("WaitNext: state=IDLE", dut.state_reg, ST_IDLE)

        `TB_SUMMARY("tb_ldma")
        $finish;
    end

    initial begin #200000; $error("[TIMEOUT] tb_ldma"); $finish; end
endmodule
