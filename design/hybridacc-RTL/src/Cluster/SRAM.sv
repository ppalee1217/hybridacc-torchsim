//-----------------------------------------------------------------------------
// Module Name:   SRAM
// Description:   Configurable SRAM with pipelined read and synchronous write.
//                Read pipeline is a circular FIFO of depth PIPELINE_DEPTH.
//                An output holding register back-pressures the pipeline.
//                Faithfully matches Cluster/SRAM.hpp ESL behaviour.
// Dependencies:  hybridacc_utils_pkg
//-----------------------------------------------------------------------------
import hybridacc_utils_pkg::*;

module SRAM #(
    parameter int unsigned DATA_WIDTH_BITS  = 64,
    parameter int unsigned ADDR_WIDTH       = 32,
    parameter int unsigned SIZE_BYTES       = 65536,  // 64 KiB default
    parameter int unsigned LATENCY          = 1,      // read latency in cycles (>=1)
    parameter int unsigned PIPELINE_DEPTH   = 3       // max outstanding reads (1..255)
)(
    input  logic                          clk,
    input  logic                          reset_n,
    // Read request interface
    input  logic [ADDR_WIDTH-1:0]         req_addr,
    input  logic                          req_valid,
    output logic                          req_ready,
    // Read response interface
    output logic [DATA_WIDTH_BITS-1:0]    resp_data,
    output logic                          resp_valid,
    input  logic                          resp_ready,
    // Synchronous write interface
    input  logic                          write_en,
    input  logic [ADDR_WIDTH-1:0]         write_addr,
    input  logic [DATA_WIDTH_BITS-1:0]    write_data,
    input  logic [DATA_WIDTH_BITS/8-1:0]  write_mask   // byte-enable
);

    localparam int unsigned DATA_WIDTH_BYTES = DATA_WIDTH_BITS / 8;
    localparam int unsigned NUM_WORDS = SIZE_BYTES / DATA_WIDTH_BYTES;
    localparam int unsigned WORD_ADDR_BITS = $clog2(NUM_WORDS);

    // ========================================================================
    // Memory array
    // ========================================================================
    logic [DATA_WIDTH_BITS-1:0] mem [0:NUM_WORDS-1];

    // ========================================================================
    // Read pipeline FIFO (circular buffer)
    // ========================================================================
    logic                    pipe_valid [0:PIPELINE_DEPTH-1];
    logic [ADDR_WIDTH-1:0]  pipe_addr  [0:PIPELINE_DEPTH-1];
    logic [7:0]             pipe_cycles[0:PIPELINE_DEPTH-1];

    logic [7:0] head_reg;
    logic [7:0] count_reg;

    // Output holding register
    logic [DATA_WIDTH_BITS-1:0] resp_data_reg;
    logic                       resp_valid_reg;

    // ========================================================================
    // Combinational outputs
    // ========================================================================
    assign req_ready  = (count_reg < PIPELINE_DEPTH[7:0]);
    assign resp_valid = resp_valid_reg;
    assign resp_data  = resp_data_reg;

    // ========================================================================
    // Helper: read a word from memory
    // ========================================================================
    function automatic logic [DATA_WIDTH_BITS-1:0] read_word(input logic [ADDR_WIDTH-1:0] byte_addr);
        logic [WORD_ADDR_BITS-1:0] idx;
        idx = byte_addr[WORD_ADDR_BITS+$clog2(DATA_WIDTH_BYTES)-1:$clog2(DATA_WIDTH_BYTES)];
        return mem[idx % NUM_WORDS];
    endfunction

    // ========================================================================
    // Sequential process
    // ========================================================================
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int i = 0; i < PIPELINE_DEPTH; i++) begin
                pipe_valid[i]  <= 1'b0;
                pipe_addr[i]   <= '0;
                pipe_cycles[i] <= 8'd0;
            end
            head_reg       <= 8'd0;
            count_reg      <= 8'd0;
            resp_data_reg  <= '0;
            resp_valid_reg <= 1'b0;
        end else begin
            // ── Synchronous write ──
            if (write_en) begin
                logic [WORD_ADDR_BITS-1:0] w_idx;
                w_idx = write_addr[WORD_ADDR_BITS+$clog2(DATA_WIDTH_BYTES)-1:$clog2(DATA_WIDTH_BYTES)];
                for (int b = 0; b < DATA_WIDTH_BYTES; b++) begin
                    if (write_mask[b])
                        mem[w_idx % NUM_WORDS][b*8 +: 8] <= write_data[b*8 +: 8];
                end
            end

            // ── Countdown for occupied pipeline slots ──
            for (int i = 0; i < PIPELINE_DEPTH; i++) begin
                if (pipe_valid[i] && pipe_cycles[i] > 8'd1)
                    pipe_cycles[i] <= pipe_cycles[i] - 8'd1;
            end

            // ── Pop decision ──
            begin
                logic head_done, out_free, do_pop;
                logic [7:0] h, c;
                h = head_reg;
                c = count_reg;

                head_done = (c > 8'd0) && (pipe_cycles[h] <= 8'd1);
                out_free  = !resp_valid_reg || resp_ready;
                do_pop    = head_done && out_free;

                if (do_pop) begin
                    resp_data_reg  <= read_word(pipe_addr[h]);
                    resp_valid_reg <= 1'b1;
                    pipe_valid[h]  <= 1'b0;
                    head_reg       <= (h + 8'd1) % PIPELINE_DEPTH[7:0];
                    count_reg      <= c - 8'd1;

                    // ── Push decision (after pop) ──
                    if (req_valid && ((c - 8'd1) < PIPELINE_DEPTH[7:0])) begin
                        logic [7:0] tail;
                        tail = (h + c) % PIPELINE_DEPTH[7:0];
                        pipe_valid[tail]  <= 1'b1;
                        pipe_addr[tail]   <= req_addr;
                        pipe_cycles[tail] <= LATENCY[7:0];
                        count_reg         <= c; // c-1+1
                    end else begin
                        count_reg <= c - 8'd1;
                    end
                end else begin
                    if (resp_valid_reg && resp_ready)
                        resp_valid_reg <= 1'b0;

                    // ── Push decision (no pop) ──
                    if (req_valid && (c < PIPELINE_DEPTH[7:0])) begin
                        logic [7:0] tail;
                        tail = (h + c) % PIPELINE_DEPTH[7:0];
                        pipe_valid[tail]  <= 1'b1;
                        pipe_addr[tail]   <= req_addr;
                        pipe_cycles[tail] <= LATENCY[7:0];
                        count_reg         <= c + 8'd1;
                    end
                end
            end
        end
    end

    // ========================================================================
    // Testbench helper tasks (backdoor access)
    // ========================================================================
    // synthesis translate_off
    task automatic mem_reset();
        for (int i = 0; i < NUM_WORDS; i++)
            mem[i] = '0;
    endtask

    task automatic backdoor_write(input logic [ADDR_WIDTH-1:0] byte_addr,
                                   input logic [DATA_WIDTH_BITS-1:0] data);
        logic [WORD_ADDR_BITS-1:0] idx;
        idx = byte_addr[WORD_ADDR_BITS+$clog2(DATA_WIDTH_BYTES)-1:$clog2(DATA_WIDTH_BYTES)];
        mem[idx % NUM_WORDS] = data;
    endtask

    function automatic logic [DATA_WIDTH_BITS-1:0] backdoor_read(
        input logic [ADDR_WIDTH-1:0] byte_addr);
        logic [WORD_ADDR_BITS-1:0] idx;
        idx = byte_addr[WORD_ADDR_BITS+$clog2(DATA_WIDTH_BYTES)-1:$clog2(DATA_WIDTH_BYTES)];
        return mem[idx % NUM_WORDS];
    endfunction
    // synthesis translate_on

endmodule
