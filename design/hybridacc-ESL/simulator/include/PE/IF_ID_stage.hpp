#pragma once

#include "utils.hpp"
#include <systemc>
#include "InstructionMemory.hpp"
#include "Decoder.hpp"
#include "LoopController.hpp"

namespace hybridacc {
namespace pe {

SC_MODULE(IF_ID_Stage) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Pipeline outputs
    sc_out<pe_decode_signals_t> ID_decode_signals_out;
    sc_out<bool> signal_valid_out;

    // Status outputs
    sc_out<uint16_t> pc_out;
    sc_out<bool> halted_out;

    // PE control signals
    sc_in<bool> stage_reset;        // Reset stage state (PC and registers)
    sc_in<bool> pe_running;         // PE is running (false = all stages stop)

    // Stall control (保留 pipeline stall 機制)
    sc_in<bool> stall_from_downstream;

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
          signal_valid_out("signal_valid_out"),
          pc_out("pc_out"),
          halted_out("halted_out"),
          stage_reset("stage_reset"),
          pe_running("pe_running"),
          stall_from_downstream("stall_from_downstream"),
          pc_init_value("pc_init_value"),
          im_write_en("im_write_en"),
          im_write_addr("im_write_addr"),
          im_write_data("im_write_data"),
          IM("IM"),
          decoder("decoder"),
          loops("loops")
    {
        DEBUG_MSG("[Create] IF_ID_Stage");
        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);
        SC_METHOD(combinational_process);
        sensitive << pc_reg << halted_reg << pe_running << stage_reset
                  << stall_from_downstream << im_read_data_sig
                  << decoder_decode_signals_out_sig << loops_jump_sig << loops_pc_out_sig;

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

    // Pipeline registers
    sc_signal<pe_decode_signals_t> decode_signals_reg;
    sc_signal<pe_decode_signals_t> decode_signals_next;

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
    sc_signal<bool> loops_loop_break_en_sig;
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
        loops.loop_break_en(loops_loop_break_en_sig);
        loops.loop_end_en(loops_loop_end_en_sig);
        loops.pc_out(loops_pc_out_sig);
        loops.jump(loops_jump_sig);
    }

    // === Combinational Logic ===
    void combinational_process() {
        // Read current register values
        uint16_t pc_current = pc_reg.read();
        bool halted_current = halted_reg.read();
        pe_decode_signals_t decode_current = decoder_decode_signals_out_sig.read();

        // Default: hold current values
        uint16_t pc_n = pc_current;
        bool halted_n = halted_current;
        pe_decode_signals_t decode_n;
        bool valid_n = false;

        // Update loop_end_en signal
        loops_loop_end_en_sig.write(decode_current.loop_end);

        // === PC Calculation ===
        uint16_t next_pc_calc = pc_current + sizeof(uint16_t); // Default: next instruction

        // Handle loop_in instruction
        if (decode_current.loop_in) {
            loops_pc_in_sig.write(next_pc_calc);
            loops_count_in_sig.write(decode_current.imm);
            loops_loop_in_en_sig.write(true);
        } else {
            loops_loop_in_en_sig.write(false);
        }

        // Handle loop_break instruction
        if (decode_current.loop_break) {
            loops_loop_break_en_sig.write(true);
        } else {
            loops_loop_break_en_sig.write(false);
        }

        // Handle loop_end instruction
        if (decode_current.loop_end) {
            if (loops_jump_sig.read()) {
                next_pc_calc = loops_pc_out_sig.read();
            }
        }

        // Handle jump instruction
        if (decode_current.jump_en) {
            next_pc_calc = decode_current.imm;
        }

        // === State Machine Logic ===
        // Check for stage_reset (highest priority)
        if (stage_reset.read()) {
            pc_n = pc_init_value.read();
            halted_n = false;
            decode_n = pe_decode_signals_t();
            valid_n = false;
        }
        // If PE not running, hold state but output invalid
        else if (!pe_running.read()) {
            pc_n = pc_current;
            halted_n = halted_current;
            decode_n = pe_decode_signals_t();
            valid_n = false;
        }
        // If halted, hold state and output invalid
        else if (halted_current) {
            pc_n = pc_current;
            halted_n = true;
            decode_n = pe_decode_signals_t();
            valid_n = false;
        }
        // If stalled, hold state and keep current output
        else if (stall_from_downstream.read()) {
            pc_n = pc_current;
            halted_n = halted_current;
            decode_n = decode_signals_reg.read();
            valid_n = false;
        }
        // Normal operation: fetch and decode
        else {
            // Fetch instruction from IM (combinational read)
            decode_n = decode_current;
            valid_n = true;

            // Check for halt instruction
            if (decode_current.halt) {
                halted_n = true;
                DEBUG_MSG("[IF_ID_Stage] HALT instruction detected at PC=" << pc_current);
            }

            // Update PC
            pc_n = next_pc_calc;

            DEBUG_MSG("[IF_ID_Stage] Fetch PC=" << pc_current
                      << " Inst=0x" << std::hex << decode_current.inst << std::dec
                      << " Next_PC=" << next_pc_calc);
        }

        // Write next values to signals
        pc_next.write(pc_n);
        halted_next.write(halted_n);
        decode_signals_next.write(decode_n);
        valid_next.write(valid_n);

        // Output to pipeline (combinational)
        ID_decode_signals_out.write(decode_n);
        signal_valid_out.write(valid_n);
        pc_out.write(pc_n);
        halted_out.write(halted_n);
    }

    // === Sequential Logic (Register Updates) ===
    void main_thread() {
        // Reset initialization
        pc_reg.write(0);
        halted_reg.write(false);
        decode_signals_reg.write(pe_decode_signals_t());
        valid_reg.write(false);

        wait(); // Wait for first clock edge

        while (true) {
            // On each clock edge, update registers with next values
            pc_reg.write(pc_next.read());
            halted_reg.write(halted_next.read());
            decode_signals_reg.write(decode_signals_next.read());
            valid_reg.write(valid_next.read());

            DEBUG_MSG("[IF_ID_Stage] Clocked: PC=" << pc_reg.read()
                      << " Halted=" << halted_reg.read()
                      << " Valid=" << valid_reg.read());

            wait(); // Wait for next clock edge
        }
    }
};

}; // namespace pe
}; // namespace hybridacc