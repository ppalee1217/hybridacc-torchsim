//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_agu
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Smoke-level unit testbench for AddressGenerateUnit.
//                Covers: reset defaults, MMIO config readback,
//                start/stop, basic descriptor generation, simple
//                backpressure, soft-reset behaviour.
// Dependencies:  ../tb_common.svh, src/Cluster/cluster_pkg.sv,
//                src/Cluster/AddressGenerateUnit.sv
// Revision:
//   2026/04/27 - Initial version (M1 cluster datapath rewrite)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "../../src/Cluster/cluster_pkg.sv"
`include "../../src/Cluster/AddressGenerateUnit.sv"
`endif

module tb_agu;
    import cluster_pkg::*;

    logic clk, reset_n;
    logic        cfg_write;
    logic [7:0]  cfg_addr;
    logic [31:0] cfg_wdata;
    logic [31:0] cfg_rdata;
    logic        start, stop;
    logic        gen_valid;
    logic        gen_ready;
    logic [31:0] gen_addr;
    logic [15:0] gen_tag;
    logic        gen_ultra;
    logic [15:0] gen_mask;
    logic        busy, done;
    logic [1:0]  fsm_state;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    AddressGenerateUnit dut (
        .clk(clk), .reset_n(reset_n),
        .cfg_write(cfg_write), .cfg_addr(cfg_addr),
        .cfg_wdata(cfg_wdata), .cfg_rdata(cfg_rdata),
        .start(start), .stop(stop),
        .gen_valid(gen_valid), .gen_ready(gen_ready),
        .gen_addr(gen_addr), .gen_tag(gen_tag),
        .gen_ultra(gen_ultra), .gen_mask(gen_mask),
        .busy(busy), .done(done), .fsm_state(fsm_state)
    );

    // ----- helpers -----
    task automatic mmio_write(input logic [7:0] addr, input logic [31:0] data);
        @(negedge clk);
        cfg_write = 1'b1;
        cfg_addr  = addr;
        cfg_wdata = data;
        @(posedge clk);
        @(negedge clk);
        cfg_write = 1'b0;
        cfg_wdata = 32'h0;
    endtask

    task automatic mmio_read(input logic [7:0] addr, output logic [31:0] data);
        @(negedge clk);
        cfg_write = 1'b0;
        cfg_addr  = addr;
        #(`TB_SETTLE);
        data = cfg_rdata;
    endtask

    initial begin
        cfg_write = 0; cfg_addr = 0; cfg_wdata = 0;
        start = 0; stop = 0; gen_ready = 0;

        @(posedge reset_n);
        @(posedge clk); @(negedge clk);

        // -----------------------------------------------------------------
        // Test 1: reset defaults
        // -----------------------------------------------------------------
        `CHECK_BIT("Reset: busy=0",      busy,      1'b0)
        `CHECK_BIT("Reset: done=0",      done,      1'b0)
        `CHECK_BIT("Reset: gen_valid=0", gen_valid, 1'b0)
        `CHECK_VAL("Reset: fsm=IDLE",    fsm_state, 2'd0)

        begin
            logic [31:0] rd;
            mmio_read(AGU_REG_ITER01, rd);
            `CHECK_VAL("Reset: ITER01 default = {1,1}", rd, 32'h0001_0001)
            mmio_read(AGU_REG_MASK_CFG, rd);
            `CHECK_VAL("Reset: MASK_CFG default = 0xF", rd, 32'h0000_000F)
            mmio_read(AGU_REG_TAG_STRIDE0, rd);
            `CHECK_VAL("Reset: TAG_STRIDE0 default = 1", rd, 32'h0000_0001)
        end

        // -----------------------------------------------------------------
        // Test 2: configure 1D loop iter0=4, stride0=8, base=0x100
        //         expect 4 descriptors with addr = 0x100, 0x108, 0x110, 0x118
        // -----------------------------------------------------------------
        mmio_write(AGU_REG_BASE_ADDR, 32'h0000_0100);
        mmio_write(AGU_REG_ITER01,    32'h0001_0004);  // iter0=4, iter1=1
        mmio_write(AGU_REG_ITER23,    32'h0001_0001);
        mmio_write(AGU_REG_STRIDE0,   32'h0000_0008);
        mmio_write(AGU_REG_MASK_CFG,  32'h0000_000F);
        mmio_write(AGU_REG_TAG_BASE,  32'h0000_0001);

        // Read back BASE_ADDR
        begin
            logic [31:0] rd;
            mmio_read(AGU_REG_BASE_ADDR, rd);
            `CHECK_VAL("Cfg: BASE_ADDR readback", rd, 32'h0000_0100)
            mmio_read(AGU_REG_ITER01, rd);
            `CHECK_VAL("Cfg: ITER01 readback {iter1=1, iter0=4}", rd, 32'h0001_0004)
            mmio_read(AGU_REG_STRIDE0, rd);
            `CHECK_VAL("Cfg: STRIDE0 readback", rd, 32'h0000_0008)
        end

        // -----------------------------------------------------------------
        // Test 3: start, capture 4 descriptors back-to-back
        // -----------------------------------------------------------------
        gen_ready = 1'b1;
        @(negedge clk); start = 1'b1;
        @(posedge clk); @(negedge clk); start = 1'b0;

        begin
            int  captured;
            logic [31:0] expected_addr [0:3];
            captured = 0;
            expected_addr[0] = 32'h0000_0100;
            expected_addr[1] = 32'h0000_0108;
            expected_addr[2] = 32'h0000_0110;
            expected_addr[3] = 32'h0000_0118;
            for (int cyc = 0; cyc < 200 && captured < 4; cyc++) begin
                @(posedge clk); #(`TB_SETTLE);
                if (gen_valid && gen_ready) begin
                    `CHECK_VAL($sformatf("Run: descriptor[%0d] addr", captured),
                               gen_addr, expected_addr[captured])
                    captured = captured + 1;
                end
            end
            `CHECK_VAL("Run: captured 4 descriptors", captured, 4)
        end

        // After last fire we expect IDLE within a few cycles.
        repeat (4) @(posedge clk);
        `CHECK_BIT("After run: busy=0", busy, 1'b0)
        `CHECK_BIT("After run: gen_valid=0", gen_valid, 1'b0)

        // -----------------------------------------------------------------
        // Test 4: backpressure — assert gen_ready=0 mid-run
        // -----------------------------------------------------------------
        mmio_write(AGU_REG_BASE_ADDR, 32'h0000_0200);
        mmio_write(AGU_REG_ITER01,    32'h0001_0003);
        mmio_write(AGU_REG_STRIDE0,   32'h0000_0010);
        gen_ready = 1'b0;
        @(negedge clk); start = 1'b1;
        @(posedge clk); @(negedge clk); start = 1'b0;

        // Wait until gen_valid asserts, then verify it stays asserted.
        begin
            int waits;
            waits = 0;
            while (!gen_valid && waits < 50) begin
                @(posedge clk); waits++;
            end
            `CHECK_BIT("Backpressure: gen_valid asserted", gen_valid, 1'b1)
            // Hold for 5 cycles with ready=0; gen_valid must remain high.
            repeat (5) begin @(posedge clk); #(`TB_SETTLE);
                `CHECK_BIT("Backpressure: gen_valid held", gen_valid, 1'b1)
            end
        end

        // Release ready, drain
        gen_ready = 1'b1;
        repeat (40) @(posedge clk);
        `CHECK_BIT("After drain: busy=0", busy, 1'b0)

        // -----------------------------------------------------------------
        // Test 5: soft reset clears FSM but keeps config
        // -----------------------------------------------------------------
        gen_ready = 1'b0;
        mmio_write(AGU_REG_ITER01, 32'h0001_0008);
        @(negedge clk); start = 1'b1;
        @(posedge clk); @(negedge clk); start = 1'b0;
        repeat (3) @(posedge clk);
        // assert soft reset bit
        mmio_write(AGU_REG_CTRL, 32'h0000_0004);
        @(posedge clk); #(`TB_SETTLE);
        `CHECK_BIT("Soft reset: busy=0", busy, 1'b0)
        `CHECK_BIT("Soft reset: gen_valid=0", gen_valid, 1'b0)

        // base_addr should be preserved (configured to 0x200 in test 4)
        begin
            logic [31:0] rd;
            mmio_read(AGU_REG_BASE_ADDR, rd);
            `CHECK_VAL("Soft reset: BASE_ADDR preserved", rd, 32'h0000_0200)
        end

        // Clear soft reset bit before exiting
        mmio_write(AGU_REG_CTRL, 32'h0000_0000);

        `TB_SUMMARY("tb_agu")
        $finish;
    end

    initial begin
        #200000;
        $error("[TB_TIMEOUT] tb_agu did not finish in time");
        `TB_SUMMARY("tb_agu")
        $finish;
    end

endmodule
