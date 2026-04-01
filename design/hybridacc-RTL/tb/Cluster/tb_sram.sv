//-----------------------------------------------------------------------------
// Module Name:   tb_sram
// Description:   Testbench for Cluster/SRAM module.
//                Tests: basic read/write, pipeline fill, backpressure,
//                byte-masked write, latency accounting.
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/Cluster/SRAM.sv"

module tb_sram;
    localparam int DATA_W  = 64;
    localparam int ADDR_W  = 32;
    localparam int SIZE_B  = 1024;   // small for test
    localparam int LATENCY = 2;
    localparam int PIPE_D  = 2;

    logic                  clk, reset_n;
    logic [ADDR_W-1:0]    req_addr;
    logic                  req_valid;
    logic                  req_ready;
    logic [DATA_W-1:0]    resp_data;
    logic                  resp_valid;
    logic                  resp_ready;
    logic                  write_en;
    logic [ADDR_W-1:0]    write_addr;
    logic [DATA_W-1:0]    write_data;
    logic [DATA_W/8-1:0]  write_mask;

    tb_clock_reset clk_rst(.clk(clk), .reset_n(reset_n));

    SRAM #(
        .DATA_WIDTH_BITS(DATA_W),
        .ADDR_WIDTH(ADDR_W),
        .SIZE_BYTES(SIZE_B),
        .LATENCY(LATENCY),
        .PIPELINE_DEPTH(PIPE_D)
    ) dut (
        .clk(clk), .reset_n(reset_n),
        .req_addr(req_addr), .req_valid(req_valid), .req_ready(req_ready),
        .resp_data(resp_data), .resp_valid(resp_valid), .resp_ready(resp_ready),
        .write_en(write_en), .write_addr(write_addr),
        .write_data(write_data), .write_mask(write_mask)
    );

    int pass_count = 0;
    int fail_count = 0;

    task automatic check(input string name, input logic cond);
        if (!cond) begin $error("[FAIL] %s", name); fail_count++; end
        else begin $display("[PASS] %s", name); pass_count++; end
    endtask

    task automatic do_write(input logic [ADDR_W-1:0] addr,
                            input logic [DATA_W-1:0] data,
                            input logic [DATA_W/8-1:0] mask);
        @(posedge clk);
        write_en   <= 1'b1;
        write_addr <= addr;
        write_data <= data;
        write_mask <= mask;
        @(posedge clk);
        write_en   <= 1'b0;
    endtask

    task automatic start_read(input logic [ADDR_W-1:0] addr);
        @(posedge clk);
        req_addr  <= addr;
        req_valid <= 1'b1;
        @(posedge clk);
        req_valid <= 1'b0;
    endtask

    task automatic wait_resp(output logic [DATA_W-1:0] data);
        wait (resp_valid === 1'b1);
        data = resp_data;
        @(posedge clk);
        resp_ready <= 1'b1;
        @(posedge clk);
        resp_ready <= 1'b0;
    endtask

    logic [DATA_W-1:0] rd;

    initial begin
        req_addr   = '0;
        req_valid  = 1'b0;
        resp_ready = 1'b0;
        write_en   = 1'b0;
        write_addr = '0;
        write_data = '0;
        write_mask = '0;

        @(posedge reset_n);
        repeat (2) @(posedge clk);

        // ---- Test 1: basic write + read ----
        $display("\n=== Test 1: Basic write + read ===");
        do_write(32'h0000, 64'hDEAD_BEEF_CAFE_BABE, 8'hFF);
        start_read(32'h0000);
        wait_resp(rd);
        check("T1 read data", rd == 64'hDEAD_BEEF_CAFE_BABE);

        // ---- Test 2: byte-masked write ----
        $display("\n=== Test 2: Byte-masked write ===");
        // Write full word first
        do_write(32'h0008, 64'hFFFF_FFFF_FFFF_FFFF, 8'hFF);
        // Overwrite only lower 4 bytes
        do_write(32'h0008, 64'h0000_0000_1234_5678, 8'h0F);
        start_read(32'h0008);
        wait_resp(rd);
        check("T2 masked write", rd == 64'hFFFF_FFFF_1234_5678);

        // ---- Test 3: pipeline fill (2 reads back to back, PIPE_D=2) ----
        $display("\n=== Test 3: Pipeline fill ===");
        do_write(32'h0010, 64'hAAAA, 8'hFF);
        do_write(32'h0018, 64'hBBBB, 8'hFF);

        @(posedge clk);
        req_addr  <= 32'h0010; req_valid <= 1'b1; @(posedge clk);
        req_addr  <= 32'h0018; @(posedge clk);
        req_valid <= 1'b0;

        resp_ready <= 1'b1;
        wait (resp_valid === 1'b1); #(`TB_SETTLE);
        check("T3 first", resp_data == 64'hAAAA);
        @(posedge clk);
        if (!resp_valid) wait (resp_valid === 1'b1); #(`TB_SETTLE);
        check("T3 second", resp_data == 64'hBBBB);
        @(posedge clk);
        resp_ready <= 1'b0;

        // ---- Test 4: backpressure ----
        $display("\n=== Test 4: Backpressure ===");
        do_write(32'h0028, 64'h9999, 8'hFF);
        start_read(32'h0028);
        resp_ready <= 1'b0;
        wait (resp_valid === 1'b1); #(`TB_SETTLE);
        check("T4 data held", resp_data == 64'h9999);
        repeat (3) begin
            @(posedge clk); #(`TB_SETTLE);
            check("T4 still valid", resp_valid == 1'b1);
        end
        @(posedge clk);
        resp_ready <= 1'b1;
        @(posedge clk);
        resp_ready <= 1'b0;

        // ---- Test 5: pipeline full → req_ready deasserts ----
        $display("\n=== Test 5: Pipeline full ===");
        resp_ready <= 1'b0;
        // With PIPE_D=2 and LATENCY=2, fill both pipeline slots and let the
        // first one pop into the output register (resp_ready=0 holds it).
        // Then push 2 more to refill → count=2 → req_ready=0.
        @(posedge clk);
        req_addr <= 32'h0010; req_valid <= 1'b1;
        @(posedge clk);
        req_addr <= 32'h0018;
        @(posedge clk);
        req_valid <= 1'b0;
        // Wait for first entry to pop into output register, then push 2 more
        repeat (4) @(posedge clk);
        // Now output register is occupied, refill pipeline
        req_addr <= 32'h0010; req_valid <= 1'b1;
        @(posedge clk);
        req_addr <= 32'h0018;
        @(posedge clk);
        req_valid <= 1'b0;
        @(posedge clk); #(`TB_SETTLE);
        check("T5 req_ready=0 when full", req_ready == 1'b0);
        // Drain
        resp_ready <= 1'b1;
        repeat (20) @(posedge clk);
        resp_ready <= 1'b0;

        // ---- Test 6: read-after-write same address ----
        $display("\n=== Test 6: Read-after-write same address ===");
        do_write(32'h0030, 64'h1111_2222_3333_4444, 8'hFF);
        do_write(32'h0030, 64'h5555_6666_7777_8888, 8'hFF);
        start_read(32'h0030);
        wait_resp(rd);
        check("T6 overwrite", rd == 64'h5555_6666_7777_8888);

        // ---- Summary ----
        repeat (5) @(posedge clk);
        $display("\n========================================");
        $display("tb_sram: %0d PASS, %0d FAIL", pass_count, fail_count);
        $display("========================================");
        if (fail_count > 0) $fatal(1, "FAILED");
        $finish;
    end

    initial begin
        #50000;
        $fatal(1, "TIMEOUT");
    end
endmodule
