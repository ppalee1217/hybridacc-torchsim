// Module: IF_ID_Stage
// Function: Instruction fetch and decode pipeline stage for the ProcessElement.
import hybridacc_utils_pkg::*;

module IF_ID_Stage (
    input  logic             clk,
    input  logic             reset_n,
    output pe_decode_signals_t ID_decode_signals_out,
    output logic             valid_out,
    output logic [15:0]      pc_out,
    output logic             halted_out,
    input  logic             stage_reset,
    input  logic             pe_running,
    input  logic             ready_in,
    input  logic [15:0]      pc_init_value,
    input  logic             im_write_en,
    input  logic [15:0]      im_write_addr,
    input  pe_inst_t         im_write_data
);
    logic [15:0] pc_reg, pc_next;
    logic halted_reg, halted_next;
    logic valid_reg, valid_next;

    pe_inst_t im_read_data_sig;
    pe_decode_signals_t decoder_decode_signals_out_sig;

    logic [15:0] loops_pc_in_sig;
    logic [15:0] loops_count_in_sig;
    logic loops_loop_in_en_sig;
    logic loops_loop_end_en_sig;
    logic [15:0] loops_pc_out_sig;
    logic loops_jump_sig;

    InstructionMemory IM (
        .clk(clk),
        .reset_n(reset_n),
        .im_write_en(im_write_en),
        .im_write_addr(im_write_addr),
        .im_write_data(im_write_data),
        .im_read_addr(pc_reg),
        .im_read_data(im_read_data_sig)
    );

    Decoder decoder (
        .inst_in(im_read_data_sig),
        .decode_signals_out(decoder_decode_signals_out_sig)
    );

    LoopController loops (
        .clk(clk),
        .reset_n(reset_n),
        .pc_in(loops_pc_in_sig),
        .count_in(loops_count_in_sig),
        .loop_in_en(loops_loop_in_en_sig),
        .loop_end_en(loops_loop_end_en_sig),
        .pc_out(loops_pc_out_sig),
        .jump(loops_jump_sig)
    );

    always_comb begin
        logic [15:0] incremented_pc;
        logic [15:0] next_pc_candidate;
        logic can_advance;

        pc_next = pc_reg;
        halted_next = halted_reg;

        incremented_pc = pc_reg + 16'd2;
        next_pc_candidate = incremented_pc;
        can_advance = ready_in && valid_reg;

        if (can_advance && decoder_decode_signals_out_sig.loop_end) begin
            if (loops_jump_sig) next_pc_candidate = loops_pc_out_sig;
        end

        if (stage_reset) begin
            pc_next = pc_init_value;
            halted_next = 1'b0;
        end else if (!pe_running) begin
            pc_next = pc_reg;
            halted_next = halted_reg;
        end else if (halted_reg) begin
            pc_next = pc_reg;
            halted_next = 1'b1;
        end else begin
            if (can_advance && decoder_decode_signals_out_sig.halt) begin
                halted_next = 1'b1;
            end

            if (can_advance) pc_next = next_pc_candidate;
            else pc_next = pc_reg;
        end
    end

    always_comb begin
        if (stage_reset) begin
            valid_next = 1'b0;
        end else if (!pe_running) begin
            valid_next = 1'b0;
        end else if (halted_reg) begin
            valid_next = 1'b0;
        end else begin
            if (ready_in) valid_next = 1'b1;
            else valid_next = valid_reg;
        end
    end

    always_comb begin
        pe_decode_signals_t output_decode;
        logic output_valid;

        output_decode = decoder_decode_signals_out_sig;
        output_valid = valid_reg;

        if (stage_reset || !pe_running || halted_reg) begin
            output_decode = pe_decode_signals_zero();
            output_valid = 1'b0;
        end

        ID_decode_signals_out = output_decode;
        valid_out = output_valid;
        pc_out = pc_reg;
        halted_out = halted_reg;
    end

    always_comb begin
        logic can_advance;
        can_advance = ready_in && valid_reg;

        if (decoder_decode_signals_out_sig.loop_in && can_advance) begin
            loops_pc_in_sig = pc_reg + 16'd2;
            loops_count_in_sig = decoder_decode_signals_out_sig.imm;
            loops_loop_in_en_sig = 1'b1;
        end else begin
            loops_pc_in_sig = 16'h0000;
            loops_count_in_sig = 16'h0000;
            loops_loop_in_en_sig = 1'b0;
        end

        loops_loop_end_en_sig = decoder_decode_signals_out_sig.loop_end && can_advance;
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            pc_reg <= 16'h0000;
            halted_reg <= 1'b0;
            valid_reg <= 1'b0;
        end else begin
            pc_reg <= pc_next;
            halted_reg <= halted_next;
            valid_reg <= valid_next;
        end
    end
endmodule
