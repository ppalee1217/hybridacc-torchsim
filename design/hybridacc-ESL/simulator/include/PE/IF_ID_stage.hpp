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
        DEBUG_MSG("[Create] IF_ID_Stage", DEBUG_LEVEL_PE_STAGE);
        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);

        // 🔧 PC 計算邏輯 - 需要對 decoder 和 loop controller 輸出敏感
        SC_METHOD(pc_calculation_process);
        sensitive << pc_reg << halted_reg
                  << stage_reset << pe_running << stall_from_downstream
                  << decoder_decode_signals_out_sig
                  << loops_jump_sig << loops_pc_out_sig;

        // 🔧 輸出控制邏輯 - 只對控制信號和 register 敏感
        SC_METHOD(output_control_process);
        sensitive << pc_reg << halted_reg << valid_reg
                  << stage_reset << pe_running << stall_from_downstream
                  << decoder_decode_signals_out_sig;

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
    void pc_calculation_process() {
        // Read current register values
        uint16_t pc_current = pc_reg.read();
        bool halted_current = halted_reg.read();
        pe_decode_signals_t decode_from_decoder = decoder_decode_signals_out_sig.read();

        // Default: hold current values
        uint16_t pc_n = pc_current;
        bool halted_n = halted_current;

        // Update loop_end_en signal
        loops_loop_end_en_sig.write(decode_from_decoder.loop_end && !stall_from_downstream.read());

        // === PC Calculation ===
        uint16_t next_pc_calc = pc_current + sizeof(uint16_t); // Default: next instruction

        // Handle loop_in instruction
        if (decode_from_decoder.loop_in) {
            loops_pc_in_sig.write(next_pc_calc);
            loops_count_in_sig.write(decode_from_decoder.imm);
            loops_loop_in_en_sig.write(true);
        } else {
            loops_loop_in_en_sig.write(false);
        }

        // Handle loop_break instruction
        if (decode_from_decoder.loop_break) {
            loops_loop_break_en_sig.write(true);
        } else {
            loops_loop_break_en_sig.write(false);
        }

        // Handle loop_end instruction
        if (decode_from_decoder.loop_end && !stall_from_downstream.read()) {
            if (loops_jump_sig.read()) {
                next_pc_calc = loops_pc_out_sig.read();
            }
        }

        // Handle jump instruction
        if (decode_from_decoder.jump_en) {
            next_pc_calc = decode_from_decoder.imm;
        }

        // === State Machine Logic ===
        // Check for stage_reset (highest priority)
        if (stage_reset.read()) {
            pc_n = pc_init_value.read();
            halted_n = false;
        }
        // If PE not running, hold state but output invalid
        else if (!pe_running.read()) {
            pc_n = pc_current;
            halted_n = halted_current;
        }
        // If halted, hold state and output invalid
        else if (halted_current) {
            pc_n = pc_current;
            halted_n = true;
        }
        // 🔧 修復: If stalled, hold PC but keep fetching the same instruction
        else if (stall_from_downstream.read()) {
            pc_n = pc_current;  // PC 不前進
            halted_n = halted_current;
        }
        // Normal operation: fetch and decode
        else {
            // Check for halt instruction
            if (decode_from_decoder.halt) {
                halted_n = true;
                DEBUG_MSG("[IF_ID_Stage] HALT instruction detected at PC=" << pc_current, DEBUG_LEVEL_PE_STAGE);
            }

            // Update PC
            pc_n = next_pc_calc;
        }

        // Write next values to signals
        pc_next.write(pc_n);
        halted_next.write(halted_n);
    }

    void output_control_process() {
        // Read current register values
        uint16_t pc_current = pc_reg.read();
        bool halted_current = halted_reg.read();
        bool valid_current = valid_reg.read();
        pe_decode_signals_t decode_from_decoder = decoder_decode_signals_out_sig.read();

        // Calculate valid signal for next cycle
        bool valid_n = valid_current;

        // === State Machine Logic ===
        // Check for stage_reset (highest priority)
        if (stage_reset.read()) {
            valid_n = false;
        }
        // If PE not running, output invalid
        else if (!pe_running.read()) {
            valid_n = false;
        }
        // If halted, output invalid
        else if (halted_current) {
            valid_n = false;
        }
        // If stalled, keep valid
        else if (stall_from_downstream.read()) {
            valid_n = true;
        }
        // Normal operation
        else {
            valid_n = true;
        }

        // Write next valid signal
        valid_next.write(valid_n);

        // === Pipeline Outputs (Combinational) ===
        // 直接輸出當前 decoder 的結果，與 PC 同步
        pe_decode_signals_t output_decode = decode_from_decoder;
        bool output_valid = valid_current;

        // 特殊情況：reset、PE 停止或 halted 時輸出無效信號
        if (stage_reset.read() || !pe_running.read() || halted_current) {
            output_decode = pe_decode_signals_t();
            output_valid = false;
        }

        ID_decode_signals_out.write(output_decode);
        signal_valid_out.write(output_valid);
        pc_out.write(pc_current);
        halted_out.write(halted_current);
    }

    // === Sequential Logic (Register Updates) ===
    void main_thread() {
        // Reset initialization
        pc_reg.write(0);
        halted_reg.write(false);
        valid_reg.write(false);

        wait(); // Wait for first clock edge

        while (true) {
            // 🔧 修正：顯示當前 cycle 的 PC 和對應的 decoder 輸出
            pe_decode_signals_t current_decode = decoder_decode_signals_out_sig.read();
            DEBUG_MSG("[IF_ID_Stage] Clocked: PC=0x" << std::hex << pc_reg.read()
                      << " Inst=0x" << current_decode.inst << std::dec
                      << " Halted=" << halted_reg.read()
                      << " Valid=" << valid_reg.read()
                      << (stall_from_downstream.read() ? " (Stalled)" : ""), DEBUG_LEVEL_PE_STAGE);

            // On each clock edge, update registers with next values
            pc_reg.write(pc_next.read());
            halted_reg.write(halted_next.read());
            valid_reg.write(valid_next.read());

            wait(); // Wait for next clock edge
        }
    }
};

}; // namespace pe
}; // namespace hybridacc