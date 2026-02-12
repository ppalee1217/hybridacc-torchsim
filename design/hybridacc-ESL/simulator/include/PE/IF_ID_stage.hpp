#pragma once

#include "utils.hpp"
#include <systemc>
#include "InstructionMemory.hpp"
#include "Decoder.hpp"
#include "LoopController.hpp"

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

SC_MODULE(IF_ID_Stage) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Pipeline outputs
    sc_out<pe_decode_signals_t> ID_decode_signals_out;
    sc_out<bool> valid_out; // Renamed from signal_valid_out for consistency

    // Status outputs
    sc_out<uint16_t> pc_out;
    sc_out<bool> halted_out;

    // PE control signals
    sc_in<bool> stage_reset;        // Reset stage state (PC and registers)
    sc_in<bool> pe_running;         // PE is running (false = all stages stop)

    // Flow control inputs
    sc_in<bool> ready_in;           // From downstream (EXE_M). Replaces stall_from_downstream

    // PC control from external
    sc_in<uint16_t> pc_init_value;

    // Instruction Memory programming interface
    sc_in<bool> im_write_en;
    sc_in<uint16_t> im_write_addr;
    sc_in<pe_inst_t> im_write_data;

    SC_CTOR(IF_ID_Stage)
        : clk("clk"),
          reset_n("reset_n"),
          ID_decode_signals_out("ID_decode_signals_out"),
          valid_out("signal_valid_out"),
          pc_out("pc_out"),
          halted_out("halted_out"),
          stage_reset("stage_reset"),
          pe_running("pe_running"),
          ready_in("stall_from_downstream"), // Map old signal name if needed externally, or rename in structure
          pc_init_value("pc_init_value"),
          im_write_en("im_write_en"),
          im_write_addr("im_write_addr"),
          im_write_data("im_write_data"),
          IM("IM"),
          decoder("decoder"),
          loops("loops")
    {
        DEBUG_MSG("[Create] IF_ID_Stage", DEBUG_LEVEL_PE_STAGE);

        // Single Sequential Process
        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);

        // Separate Combinational Processes
        SC_METHOD(comb_pc_next);
        sensitive << pc_reg << halted_reg << stage_reset << pe_running << ready_in
              << valid_reg
                  << decoder_decode_signals_out_sig << loops_jump_sig << loops_pc_out_sig;

        SC_METHOD(comb_valid_next);
        sensitive << valid_reg << stage_reset << pe_running << ready_in << halted_reg;

        SC_METHOD(comb_outputs);
        sensitive << pc_reg << halted_reg << valid_reg << decoder_decode_signals_out_sig;

        // Loop controls are driven by decoder output (combinational)
        SC_METHOD(comb_loop_controls);
        sensitive << decoder_decode_signals_out_sig << ready_in << loops_jump_sig << loops_pc_out_sig << pc_reg;

        // Bind submodules
        bind();
    }

    // Sub-modules (public)
    InstructionMemory IM;
    Decoder decoder;
    LoopController loops;

    // === Sequential Elements (Registers) ===
    // Program Counter
    sc_signal<uint16_t> pc_reg;
    sc_signal<uint16_t> pc_next;

    // Halted flag
    sc_signal<bool> halted_reg;
    sc_signal<bool> halted_next;

    // Valid signal register (控制 pipeline 流動)
    sc_signal<bool> valid_reg;
    sc_signal<bool> valid_next;

    // === Internal signals for connecting submodules ===
    sc_signal<pe_inst_t> im_read_data_sig;

    // Decoder internal signals
    sc_signal<pe_decode_signals_t> decoder_decode_signals_out_sig;

    // Loop controller internal signals
    sc_signal<uint16_t> loops_pc_in_sig;
    sc_signal<uint16_t> loops_count_in_sig;
    sc_signal<bool> loops_loop_in_en_sig;
    sc_signal<bool> loops_loop_end_en_sig;
    sc_signal<bool> loops_empty_sig;
    sc_signal<uint16_t> loops_pc_out_sig;
    sc_signal<bool> loops_jump_sig;

    void bind() {
        // Clock and reset for all modules
        IM.clk(clk);
        IM.reset_n(reset_n);
        loops.clk(clk);
        loops.reset_n(reset_n);

        // Instruction Memory connections
        IM.im_write_en(im_write_en);
        IM.im_write_addr(im_write_addr);
        IM.im_write_data(im_write_data);
        IM.im_read_addr(pc_reg);
        IM.im_read_data(im_read_data_sig);

        // Decoder connections
        decoder.inst_in(im_read_data_sig);
        decoder.decode_signals_out(decoder_decode_signals_out_sig);

        // Loop Controller connections
        loops.pc_in(loops_pc_in_sig);
        loops.count_in(loops_count_in_sig);
        loops.loop_in_en(loops_loop_in_en_sig);
        loops.loop_end_en(loops_loop_end_en_sig);
        loops.pc_out(loops_pc_out_sig);
        loops.jump(loops_jump_sig);
    }

    // === Combinational Logic ===

    // 1. Calculate Next PC
    void comb_pc_next() {
        // Read current register values
        uint16_t pc_current = pc_reg.read();
        bool halted_current = halted_reg.read();
        pe_decode_signals_t decode_from_decoder = decoder_decode_signals_out_sig.read();

        // Default: hold current value
        uint16_t pc_n = pc_current;
        bool halted_n = halted_current;

        // Default next PC (Instruction size = 2 bytes)
        uint16_t incremented_pc = pc_current + sizeof(uint16_t);
        uint16_t next_pc_candidate = incremented_pc;

        const bool can_advance = ready_in.read() && valid_reg.read();

        // Handle Branch/Loop Logic (only when issuing a valid instruction)
        if (can_advance && decode_from_decoder.loop_end) {
            if (loops_jump_sig.read()) {
                next_pc_candidate = loops_pc_out_sig.read();
            }
        }

        // State Machine Update Logic
        if (stage_reset.read()) {
            pc_n = pc_init_value.read();
            halted_n = false;
        } else if (!pe_running.read()) {
            // Not running, hold state
            pc_n = pc_current;
            halted_n = halted_current;
        } else if (halted_current) {
            // Already halted, stay halted
            pc_n = pc_current;
            halted_n = true;
        } else {
            // Check for new halt only when issuing a valid instruction
                if (can_advance && decode_from_decoder.halt) {
                    halted_n = true;
                    DEBUG_MSG("[IF_ID_Stage] HALT detected at PC=" << pc_current, DEBUG_LEVEL_PE_STAGE);
                }

            // Update PC only when issuing a valid instruction
            if (can_advance) {
                pc_n = next_pc_candidate;
            } else {
                // Bubble or stalled: Hold PC
                pc_n = pc_current;
            }
        }

        pc_next.write(pc_n);
        halted_next.write(halted_n);
    }

    // 2. Calculate Next Valid State
    void comb_valid_next() {
        bool valid_current = valid_reg.read();
        bool valid_n = false;

        if (stage_reset.read()) {
            valid_n = false;
        } else if (!pe_running.read()) {
            valid_n = false;
        } else if (halted_reg.read()) {
            valid_n = false;
        } else {
            // If downstream is ready, we fetch a new valid instruction
            // If downstream is NOT ready, we must keep the current valid status (stall)
            if (ready_in.read()) {
                valid_n = true; // Always fetching if running and not halted
            } else {
                valid_n = valid_current;
            }
        }

        valid_next.write(valid_n);
    }

    // 3. Drive Outputs
    void comb_outputs() {
        // Output current register state
        pe_decode_signals_t output_decode = decoder_decode_signals_out_sig.read();
        bool output_valid = valid_reg.read();

        // Mask output if reset/halted/stopped
        if (stage_reset.read() || !pe_running.read() || halted_reg.read()) {
            output_decode = pe_decode_signals_t();
            output_valid = false;
        }

        ID_decode_signals_out.write(output_decode);
        valid_out.write(output_valid);
        pc_out.write(pc_reg.read());
        halted_out.write(halted_reg.read());
    }

    // 4. Loop Controller Interface Logic
    void comb_loop_controls() {
        pe_decode_signals_t decode = decoder_decode_signals_out_sig.read();

        const bool can_advance = ready_in.read() && valid_reg.read();

        // Loop In
        if (decode.loop_in && can_advance) {
            loops_pc_in_sig.write(pc_reg.read() + sizeof(uint16_t)); // Push next instruction address
            loops_count_in_sig.write(decode.imm);
            loops_loop_in_en_sig.write(true);
        } else {
            loops_loop_in_en_sig.write(false);
        }

        // Loop End (Enable only if not stalled, as it might trigger a pop/jump)
        // If stalled, we re-execute the same loop_end instruction, checking condition again
        // But the LoopController state should only update once.
        // Actually, since loop controller is sequential, we should only enable it when we are advancing (ready_in=1)
        loops_loop_end_en_sig.write(decode.loop_end && can_advance);
    }

    // === Sequential Logic (Register Updates) ===
    void main_thread() {
        // Reset initialization
        pc_reg.write(0);
        halted_reg.write(false);
        valid_reg.write(false);

        wait(); // Wait for first clock edge

        while (true) {
            // Debug print
            pe_decode_signals_t current_decode = decoder_decode_signals_out_sig.read();
            if (pe_running.read() && !halted_reg.read()) {
                 DEBUG_MSG("[IF_ID_Stage] PC=0x" << std::hex << pc_reg.read()
                      << " Inst=0x" << current_decode.inst << std::dec
                      << " V=" << valid_reg.read()
                      << " Ready=" << ready_in.read(), DEBUG_LEVEL_PE_STAGE);
            }

            // Update registers
            pc_reg.write(pc_next.read());
            halted_reg.write(halted_next.read());
            valid_reg.write(valid_next.read());

            wait(); // Wait for next clock edge
        }
    }
};

}; // namespace pe
}; // namespace hybridacc