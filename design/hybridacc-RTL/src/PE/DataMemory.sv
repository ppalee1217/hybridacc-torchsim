// Module: DataMemory
// Function: Local PE data memory with byte-mask write support for DMA and compute stages.
module DataMemory #(
    parameter int unsigned DMEMORY_ADDRESS_WIDTH = 9,
    parameter int unsigned DMEMORY_DEFAULT_SIZE_BYTES = (1 << DMEMORY_ADDRESS_WIDTH)
) (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        bank_sel,
    input  logic        dm_write_en,
    input  logic [15:0] dm_write_addr,
    input  logic [63:0] dm_write_data,
    input  logic [7:0]  dm_write_mask,
    input  logic [15:0] dm_read_addr,
    output logic [63:0] dm_read_data
);
    localparam int unsigned DMEMORY_ADDRESS_MASK = (1 << DMEMORY_ADDRESS_WIDTH) - 1;

    logic [7:0] bank0 [0:DMEMORY_DEFAULT_SIZE_BYTES-1];
    logic [7:0] bank1 [0:DMEMORY_DEFAULT_SIZE_BYTES-1];

    function automatic logic [63:0] read_word(
        input logic sel_bank,
        input logic [15:0] idx
    );
        logic [63:0] word;
        word = 64'h0;
        for (int i = 0; i < 8; i++) begin
            if ((idx + i) < DMEMORY_DEFAULT_SIZE_BYTES) begin
                word[i*8 +: 8] = sel_bank ? bank1[idx+i] : bank0[idx+i];
            end
        end
        return word;
    endfunction

    always_ff @(posedge clk or negedge reset_n) begin
        logic write_bank_idx;
        logic read_bank_idx;
        logic [15:0] w_addr;
        logic [15:0] r_addr;

        if (!reset_n) begin
            for (int i = 0; i < DMEMORY_DEFAULT_SIZE_BYTES; i++) begin
                bank0[i] <= 8'h00;
                bank1[i] <= 8'h00;
            end
            dm_read_data <= 64'h0;
        end else begin
            write_bank_idx = bank_sel ? 1'b1 : 1'b0;
            read_bank_idx  = bank_sel ? 1'b0 : 1'b1;
            w_addr = dm_write_addr & DMEMORY_ADDRESS_MASK;
            r_addr = dm_read_addr  & DMEMORY_ADDRESS_MASK;

            dm_read_data <= read_word(read_bank_idx, r_addr);

            if (dm_write_en) begin
                for (int i = 0; i < 8; i++) begin
                    if (dm_write_mask[i] && ((w_addr + i) < DMEMORY_DEFAULT_SIZE_BYTES)) begin
                        if (write_bank_idx) bank1[w_addr+i] <= dm_write_data[i*8 +: 8];
                        else bank0[w_addr+i] <= dm_write_data[i*8 +: 8];
                    end
                end
            end
        end
    end
endmodule
