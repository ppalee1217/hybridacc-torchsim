//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   InstructionMemory
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Common utility package with type definitions, FP16 arithmetic, and shared constants.
// Dependencies:  None
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module InstructionMemory #(
    parameter int unsigned MEM_BYTES = 512
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        im_write_en,
    input  logic [15:0] im_write_addr,
    input  logic [15:0] im_write_data,
    input  logic [15:0] im_read_addr,
    output logic [15:0] im_read_data
);
    localparam int unsigned WORD_BYTES  = 2;
    localparam int unsigned DEPTH_WORDS = MEM_BYTES / WORD_BYTES;
    localparam int unsigned WORD_ADDR_W = 15;
    localparam int unsigned ADDR_INDEX_W = (DEPTH_WORDS <= 1) ? 1 : $clog2(DEPTH_WORDS);
    localparam logic [WORD_ADDR_W:0] DEPTH_WORDS_COUNT = DEPTH_WORDS;

    logic [15:0] mem [0:DEPTH_WORDS-1];
    logic [WORD_ADDR_W-1:0] im_write_word_addr_w;
    logic [WORD_ADDR_W-1:0] im_read_word_addr_w;
    logic [ADDR_INDEX_W-1:0] im_write_index_w;
    logic [ADDR_INDEX_W-1:0] im_read_index_w;
    logic im_write_addr_valid_w;
    logic im_read_addr_valid_w;

    assign im_write_word_addr_w = im_write_addr[15:1];
    assign im_read_word_addr_w  = im_read_addr[15:1];
    assign im_write_index_w = im_write_word_addr_w[ADDR_INDEX_W-1:0];
    assign im_read_index_w  = im_read_word_addr_w[ADDR_INDEX_W-1:0];
    assign im_write_addr_valid_w = ({1'b0, im_write_word_addr_w} < DEPTH_WORDS_COUNT);
    assign im_read_addr_valid_w  = ({1'b0, im_read_word_addr_w} < DEPTH_WORDS_COUNT);

    // synopsys translate_off
    initial begin
        if ((MEM_BYTES % WORD_BYTES) != 0) begin
            $error("InstructionMemory: MEM_BYTES (%0d) must be multiple of %0d", MEM_BYTES, WORD_BYTES);
        end
    end
    // synopsys translate_on

    always_ff @(posedge clk or negedge reset_n) begin
        int unsigned i;
        if (!reset_n) begin
            for (i = 0; i < DEPTH_WORDS; i++) begin
                mem[i] <= 16'h0000;
            end
        end else begin
            if (im_write_en && im_write_addr_valid_w) begin
                mem[im_write_index_w] <= im_write_data;
            end
        end
    end

    always_comb begin
        im_read_data = im_read_addr_valid_w ? mem[im_read_index_w] : 16'h0000;
    end

endmodule
