//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/28
// Design Name:   HybridAcc
// Module Name:   SRAM
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Fake SRAM model for pre-synthesis simulation.
//                This module aligns with the ESL Utils/SRAM.hpp contract:
//                  * byte-addressed storage with wraparound accesses
//                  * synchronous masked writes
//                  * configurable read latency and pipeline depth
//                  * decoupled req/resp handshakes with response backpressure
// Dependencies:  None
// Revision:
//   2026/04/28 - Initial version restored for cluster SRAM unit coverage
// Additional Comments:
//   This is the behavioral simulation model. Synthesis should use the target
//   SRAM hardmacro path instead of this fake module.
//-----------------------------------------------------------------------------

module SRAM #(
    parameter int unsigned DATA_WIDTH_BITS = 64,
    parameter int unsigned ADDR_WIDTH      = 32,
    parameter int unsigned SIZE_BYTES      = (1 << 16),
    parameter int unsigned LATENCY         = 1,
    parameter int unsigned PIPELINE_DEPTH  = 3
) (
    input  logic                         clk,
    input  logic                         reset_n,

    input  logic [ADDR_WIDTH-1:0]       req_addr,
    input  logic                         req_valid,
    output logic                         req_ready,

    output logic [DATA_WIDTH_BITS-1:0]  resp_data,
    output logic                         resp_valid,
    input  logic                         resp_ready,

    input  logic                         write_en,
    input  logic [ADDR_WIDTH-1:0]       write_addr,
    input  logic [DATA_WIDTH_BITS-1:0]  write_data,
    input  logic [(DATA_WIDTH_BITS/8)-1:0] write_mask
);
    localparam int unsigned DATA_WIDTH_BYTES = DATA_WIDTH_BITS / 8;

    byte unsigned mem [0:SIZE_BYTES-1];

    logic                         pipe_valid_reg [0:PIPELINE_DEPTH-1];
    logic [ADDR_WIDTH-1:0]       pipe_addr_reg  [0:PIPELINE_DEPTH-1];
    int unsigned                  pipe_cycles_reg[0:PIPELINE_DEPTH-1];
    int unsigned                  head_reg;
    int unsigned                  count_reg;

    logic [DATA_WIDTH_BITS-1:0]  resp_data_reg;
    logic                         resp_valid_reg;

    function automatic logic [DATA_WIDTH_BITS-1:0] read_word(input logic [ADDR_WIDTH-1:0] byte_addr);
        logic [DATA_WIDTH_BITS-1:0] value;
        int unsigned base;
        begin
            value = '0;
            base = byte_addr % SIZE_BYTES;
            for (int byte_idx = 0; byte_idx < DATA_WIDTH_BYTES; byte_idx++) begin
                value[byte_idx*8 +: 8] = mem[(base + byte_idx) % SIZE_BYTES];
            end
            return value;
        end
    endfunction

    task automatic write_word(
        input logic [ADDR_WIDTH-1:0]      byte_addr,
        input logic [DATA_WIDTH_BITS-1:0] value,
        input logic [DATA_WIDTH_BYTES-1:0] byte_mask
    );
        int unsigned base;
        begin
            base = byte_addr % SIZE_BYTES;
            for (int byte_idx = 0; byte_idx < DATA_WIDTH_BYTES; byte_idx++) begin
                if (byte_mask[byte_idx]) begin
                    mem[(base + byte_idx) % SIZE_BYTES] = value[byte_idx*8 +: 8];
                end
            end
        end
    endtask

    always_comb begin
        req_ready = (count_reg < PIPELINE_DEPTH);
        resp_data = resp_data_reg;
        resp_valid = resp_valid_reg;
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int idx = 0; idx < PIPELINE_DEPTH; idx++) begin
                pipe_valid_reg[idx]  <= 1'b0;
                pipe_addr_reg[idx]   <= '0;
                pipe_cycles_reg[idx] <= 0;
            end
            head_reg       <= 0;
            count_reg      <= 0;
            resp_data_reg  <= '0;
            resp_valid_reg <= 1'b0;
        end else begin
            int unsigned head;
            int unsigned count;
            int unsigned next_head;
            int unsigned next_count;
            int unsigned effective_count;
            int unsigned tail;
            logic        head_done;
            logic        out_free;
            logic        do_pop;
            logic        do_push;

            head = head_reg;
            count = count_reg;
            next_head = head;
            next_count = count;

            if (write_en) begin
                write_word(write_addr, write_data, write_mask);
            end

            for (int idx = 0; idx < PIPELINE_DEPTH; idx++) begin
                if (pipe_valid_reg[idx] && (pipe_cycles_reg[idx] > 1)) begin
                    pipe_cycles_reg[idx] <= pipe_cycles_reg[idx] - 1;
                end
            end

            head_done = (count > 0) && (pipe_cycles_reg[head] <= 1);
            out_free = !resp_valid_reg || resp_ready;
            do_pop = head_done && out_free;

            if (do_pop) begin
                resp_data_reg <= read_word(pipe_addr_reg[head]);
                resp_valid_reg <= 1'b1;
                pipe_valid_reg[head] <= 1'b0;
                next_head = (head + 1) % PIPELINE_DEPTH;
                next_count = count - 1;
            end else if (resp_valid_reg && resp_ready) begin
                resp_valid_reg <= 1'b0;
            end

            effective_count = count - (do_pop ? 1 : 0);
            do_push = req_valid && (effective_count < PIPELINE_DEPTH);

            if (do_push) begin
                tail = (head + count) % PIPELINE_DEPTH;
                pipe_valid_reg[tail] <= 1'b1;
                pipe_addr_reg[tail] <= req_addr;
                pipe_cycles_reg[tail] <= LATENCY;
                next_count = effective_count + 1;
            end

            head_reg <= next_head;
            count_reg <= next_count;
        end
    end

endmodule