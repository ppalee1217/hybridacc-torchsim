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

    logic [15:0] mem [0:DEPTH_WORDS-1];

    initial begin
        if ((MEM_BYTES % WORD_BYTES) != 0) begin
            $error("InstructionMemory: MEM_BYTES (%0d) must be multiple of %0d", MEM_BYTES, WORD_BYTES);
        end
    end

    always_ff @(posedge clk or negedge reset_n) begin
        int i;
        if (!reset_n) begin
            for (i = 0; i < DEPTH_WORDS; i++) begin
                mem[i] <= 16'h0000;
            end
        end else begin
            if (im_write_en) begin
                mem[im_write_addr[15:1]] <= im_write_data;
            end
        end
    end

    always_comb begin
        if (!reset_n) begin
            im_read_data = 16'h0000;
        end else begin
            im_read_data = mem[im_read_addr[15:1]];
        end
    end

endmodule
