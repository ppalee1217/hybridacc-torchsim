#pragma once

#include "utils.hpp"
#include <systemc>
#include "TransformRegFile.hpp"
#include "VMULU.hpp"
#include "DataLoader.hpp"
#include "DataMemory.hpp"

namespace hybridacc {
namespace pe {

SC_MODULE(EXE_M_Stage) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // PE control signals
    sc_in<bool> stage_reset;        // Reset stage state
    sc_in<bool> pe_running;         // PE is running (false = all stages stop)

    // pipelined inputs
    sc_in<pe_decode_signals_t> ID_decode_signals_in;
    sc_in<bool> signal_valid_in;

    // pipelined outputs
    sc_out<v_fp16_t> vmul_out_out;
    sc_out<pe_decode_signals_t> EXE_A_decode_signals_out;
    sc_out<bool> signal_valid_out;

    // Status outputs
    sc_out<bool> halted_out;

    // stall control
    sc_in<bool> stall_from_downstream;
    sc_out<bool> stall_DL;
    sc_out<bool> stall_PS;
    sc_out<bool> stall_PD;

    // PS port (Port Static)
    sc_in<uint64_t> ps_data_in;
    sc_in<bool> ps_data_in_valid;
    sc_out<bool> ps_data_in_ready;

    // PD port (Port Dynamic)
    sc_in<uint16_t> pd_data_in;
    sc_in<bool> pd_data_in_valid;
    sc_out<bool> pd_data_in_ready;

    SC_CTOR(EXE_M_Stage)
        : clk("clk"),
          reset_n("reset_n"),
          stage_reset("stage_reset"),
          pe_running("pe_running"),
          ID_decode_signals_in("ID_decode_signals_in"),
          signal_valid_in("signal_valid_in"),
          vmul_out_out("vmul_out_out"),
          EXE_A_decode_signals_out("EXE_A_decode_signals_out"),
          signal_valid_out("signal_valid_out"),
          halted_out("halted_out"),
          stall_from_downstream("stall_from_downstream"),
          stall_DL("stall_DL"),
          stall_PS("stall_PS"),
          stall_PD("stall_PD"),
          ps_data_in("ps_data_in"),
          ps_data_in_valid("ps_data_in_valid"),
          ps_data_in_ready("ps_data_in_ready"),
          pd_data_in("pd_data_in"),
          pd_data_in_valid("pd_data_in_valid"),
          pd_data_in_ready("pd_data_in_ready"),
          TR("TR"),
          vmul("vmul"),
          DL("DL"),
          DM("DM")
    {
        DEBUG_MSG("[Create] EXE_M_Stage");
        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);

        // === Combinational logic split by function ===

        // Stage 1: Stall calculation (only reads, no writes to control signals)
        SC_METHOD(stall_calculation_process);
        sensitive << decode_signals_reg << valid_reg
                  << signal_valid_in << ID_decode_signals_in
                  << dl_stall_sig  // DataLoader stall output
                  << ps_data_in_valid << pd_data_in_valid
                  << stall_from_downstream;

        // Stage 2: Pipeline control (calculates next state based on stalls)
        SC_METHOD(pipeline_control_process);
        sensitive << decode_signals_reg << valid_reg << halted_reg
                  << pe_running << stage_reset
                  << ID_decode_signals_in << signal_valid_in
                  << dl_stall_internal << ps_stall_internal << pd_stall_internal << stage_stall_internal
                  << vmul_result_sig;

        // Stage 3: Submodule control (drives control signals based on CURRENT register state)
        SC_METHOD(submodule_control_process);
        sensitive << decode_signals_reg << valid_reg  // 使用暫存器而非 _next
                  << stall_from_downstream  // 只需要外部 stall
                  << tr_vtid_out_sig << dl_dmrv_out_sig
                  << pd_data_in_valid << pd_data_in;

        // PS data type conversion process
        SC_METHOD(ps_data_conversion_process);
        sensitive << ps_data_in;

        // Bind submodules
        bind();
    }

    // Sub-modules (public)
    TransformRegFile TR;
    VMULU vmul;
    DataLoader DL;
    DataMemory DM;

    // === Sequential Elements (Registers) ===
    sc_signal<pe_decode_signals_t> decode_signals_reg;
    sc_signal<pe_decode_signals_t> decode_signals_next;

    sc_signal<bool> valid_reg;
    sc_signal<bool> valid_next;

    sc_signal<bool> halted_reg;
    sc_signal<bool> halted_next;

    // === Internal signals for submodules ===
    // PS port type conversion (uint64_t -> v_fp16_t)
    sc_signal<v_fp16_t> ps_data_in_converted_sig;

    // DataLoader signals
    sc_signal<uint16_t> dl_addr_len_sig;
    sc_signal<bool> dl_set_addr_sig;
    sc_signal<bool> dl_set_len_sig;
    sc_signal<uint16_t> dl_mode_sig;
    sc_signal<uint16_t> dl_stride_sig;
    sc_signal<bool> dl_write_en_sig;
    sc_signal<bool> dl_active_sig;
    sc_signal<bool> dl_next_sig;
    sc_signal<v_fp16_t> dl_dmrv_out_sig;
    sc_signal<bool> dl_busy_sig;
    sc_signal<bool> dl_done_sig;
    sc_signal<bool> dl_stall_sig;

    // TransformRegFile signals
    sc_signal<int> tr_enable_sig;
    sc_signal<bool> tr_shift_en_sig;
    sc_signal<int> tr_shift_mode_sig;
    sc_signal<int> tr_tid_sig;
    sc_signal<fp16_t> tr_tid_in_sig;
    sc_signal<bool> tr_tid_write_en_sig;
    sc_signal<v_fp16_t> tr_vtid_out_sig;
    sc_signal<bool> tr_clear_regs_sig;
    sc_signal<bool> tr_use_vcounter_sig;
    sc_signal<bool> tr_set_vcounter_sig;
    sc_signal<bool> tr_clear_vcounter_sig;
    sc_signal<bool> tr_incr_vcounter_sig;

    // VMULU signals
    sc_signal<v_fp16_t> vmul_op1_sig;
    sc_signal<v_fp16_t> vmul_op2_sig;
    sc_signal<v_fp16_t> vmul_result_sig;

    // DataMemory <-> DataLoader signals
    sc_signal<bool> dm_write_en_sig;
    sc_signal<uint16_t> dm_write_addr_sig;
    sc_signal<uint64_t> dm_write_data_sig;
    sc_signal<uint8_t> dm_write_mask_sig;
    sc_signal<uint16_t> dm_read_addr_sig;
    sc_signal<uint64_t> dm_read_data_sig;

    // Combinational outputs
    sc_signal<bool> dl_stall_next;
    sc_signal<bool> ps_stall_next;
    sc_signal<bool> pd_stall_next;
    sc_signal<v_fp16_t> vmul_out_next;
    sc_signal<pe_decode_signals_t> exe_a_decode_next;
    sc_signal<bool> signal_valid_out_next;
    sc_signal<bool> pd_ready_next;

    // === Internal combinational signals ===
    sc_signal<bool> dl_stall_internal;
    sc_signal<bool> ps_stall_internal;
    sc_signal<bool> pd_stall_internal;
    sc_signal<bool> stage_stall_internal;

    void bind() {
        // Clock and reset
        TR.clk(clk);
        TR.reset_n(reset_n);
        DL.clk(clk);
        DL.reset_n(reset_n);
        DM.clk(clk);
        DM.reset_n(reset_n);

        // DataMemory <-> DataLoader
        DL.dm_write_en(dm_write_en_sig);
        DL.dm_write_addr(dm_write_addr_sig);
        DL.dm_write_data(dm_write_data_sig);
        DL.dm_write_mask(dm_write_mask_sig);
        DL.dm_read_addr(dm_read_addr_sig);
        DL.dm_read_data(dm_read_data_sig);

        DM.dm_write_en(dm_write_en_sig);
        DM.dm_write_addr(dm_write_addr_sig);
        DM.dm_write_data(dm_write_data_sig);
        DM.dm_write_mask(dm_write_mask_sig);
        DM.dm_read_addr(dm_read_addr_sig);
        DM.dm_read_data(dm_read_data_sig);

        // DataLoader
        DL.addr_len(dl_addr_len_sig);
        DL.set_addr(dl_set_addr_sig);
        DL.set_len(dl_set_len_sig);
        DL.mode(dl_mode_sig);
        DL.stride(dl_stride_sig);
        DL.write_en(dl_write_en_sig);
        DL.active(dl_active_sig);
        DL.next(dl_next_sig);
        DL.ps_data_in(ps_data_in_converted_sig);
        DL.ps_data_in_valid(ps_data_in_valid);
        DL.ps_data_in_ready(ps_data_in_ready);
        DL.dmrv_out(dl_dmrv_out_sig);
        DL.busy(dl_busy_sig);
        DL.done(dl_done_sig);
        DL.dl_stall_out(dl_stall_sig);

        // TransformRegFile
        TR.enable(tr_enable_sig);
        TR.shift_en(tr_shift_en_sig);
        TR.shift_mode(tr_shift_mode_sig);
        TR.tid(tr_tid_sig);
        TR.tid_in(tr_tid_in_sig);
        TR.tid_write_en(tr_tid_write_en_sig);
        TR.vtid_out(tr_vtid_out_sig);
        TR.clear_regs(tr_clear_regs_sig);
        TR.use_vcounter(tr_use_vcounter_sig);
        TR.set_vcounter(tr_set_vcounter_sig);
        TR.clear_vcounter(tr_clear_vcounter_sig);
        TR.incr_vcounter(tr_incr_vcounter_sig);

        // VMULU
        vmul.op1(vmul_op1_sig);
        vmul.op2(vmul_op2_sig);
        vmul.result(vmul_result_sig);
    }

    // === Combinational Logic (Split by function) ===

    // PS data type conversion: uint64_t -> v_fp16_t
    void ps_data_conversion_process() {
        v_fp16_t converted;
        converted.fromUint64(ps_data_in.read());
        ps_data_in_converted_sig.write(converted);
    }

    // Stage 1: Calculate all stall conditions
    void stall_calculation_process() {
        bool dl_stall = dl_stall_sig.read();  // Read from DataLoader output
        bool ps_stall = false;
        bool pd_stall = false;

        pe_decode_signals_t decode_current = decode_signals_reg.read();
        bool valid_current = valid_reg.read();

        // Calculate stalls for current instruction
        if (valid_current) {
            // PS stall: check if PS operation needs data
            bool ps_operation = decode_current.DL_active &&
                               (decode_current.func3 == 0 || decode_current.func3 == 1);
            if (ps_operation && !ps_data_in_valid.read()) {
                ps_stall = true;
            }

            // PD stall: check if PD operation needs data
            bool pd_operation = decode_current.pd_load;
            if (pd_operation && !pd_data_in_valid.read()) {
                pd_stall = true;
            }
        }

        // Calculate stalls for incoming instruction
        pe_decode_signals_t next_signals = ID_decode_signals_in.read();
        bool next_valid = signal_valid_in.read();

        if (next_valid && !valid_current) {
            // Check if new instruction will stall
            bool next_ps_operation = next_signals.DL_active &&
                                    (next_signals.func3 == 0 || next_signals.func3 == 1);
            if (next_ps_operation && !ps_data_in_valid.read()) {
                ps_stall = true;
            }

            bool next_pd_operation = next_signals.pd_load;
            if (next_pd_operation && !pd_data_in_valid.read()) {
                pd_stall = true;
            }
        }

        bool stage_stall = dl_stall || ps_stall || pd_stall || stall_from_downstream.read();

        // Write internal stall signals
        dl_stall_internal.write(dl_stall);
        ps_stall_internal.write(ps_stall);
        pd_stall_internal.write(pd_stall);
        stage_stall_internal.write(stage_stall);

        // Drive output ports
        stall_DL.write(dl_stall);
        stall_PS.write(ps_stall);
        stall_PD.write(pd_stall);
    }

    // Stage 2: Pipeline control and next state calculation
    void pipeline_control_process() {
        pe_decode_signals_t decode_current = decode_signals_reg.read();
        bool valid_current = valid_reg.read();
        bool halted_current = halted_reg.read();

        pe_decode_signals_t decode_n = decode_current;
        bool valid_n = valid_current;
        bool halted_n = halted_current;

        pe_decode_signals_t next_signals = ID_decode_signals_in.read();
        bool next_valid = signal_valid_in.read();
        bool stage_stall = stage_stall_internal.read();

        // Output defaults
        v_fp16_t vmul_out = v_fp16_t();
        pe_decode_signals_t exe_a_out = pe_decode_signals_t();
        bool valid_out = false;

        // Check for stage_reset (highest priority)
        if (stage_reset.read()) {
            decode_n = pe_decode_signals_t();
            valid_n = false;
            halted_n = false;
        }
        // If PE not running or halted, hold state
        else if (!pe_running.read() || halted_current) {
            decode_n = decode_current;
            valid_n = valid_current;
            halted_n = halted_current;
        }
        // If stalled, hold state and insert bubble (NOP) to downstream
        else if (stage_stall) {
            decode_n = decode_current;
            valid_n = valid_current;
            halted_n = halted_current;

            // 當 stall 時,向下游傳遞 NOP (valid_out=false)
            // 不要輸出有效指令,以防止下游 stage 錯誤地執行指令
            vmul_out = v_fp16_t();
            exe_a_out = pe_decode_signals_t();
            valid_out = false;  // 插入 bubble
        }
        // Normal operation: advance pipeline
        else {
            // Load new instruction from upstream
            decode_n = next_signals;
            valid_n = next_valid;

            // Check for halt instruction
            if (valid_n && decode_n.halt) {
                halted_n = true;
                DEBUG_MSG("[EXE_M_Stage] HALT instruction detected");
            }

            // Execute current instruction
            if (valid_current) {
                vmul_out = vmul_result_sig.read();
                exe_a_out = decode_current;
                valid_out = true;

                DEBUG_MSG("[EXE_M_Stage] Execute: Inst=0x" << std::hex
                          << decode_current.inst << std::dec);
            }
        }

        // Write next values
        decode_signals_next.write(decode_n);
        valid_next.write(valid_n);
        halted_next.write(halted_n);

        // Drive output ports
        vmul_out_out.write(vmul_out);
        EXE_A_decode_signals_out.write(exe_a_out);
        signal_valid_out.write(valid_out);
        halted_out.write(halted_n);
    }

    // Stage 3: Submodule control signals
    void submodule_control_process() {
        pe_decode_signals_t signals = decode_signals_reg.read();  //  改為讀取暫存器
        bool valid = valid_reg.read();  //  改為讀取暫存器
        bool stalled = stall_from_downstream.read();  //  只需要外部 stall

        bool pd_ready = false;

        if (!valid || stalled) {
            // Reset control signals when invalid or stalled
            dl_addr_len_sig.write(0);
            dl_set_addr_sig.write(false);
            dl_set_len_sig.write(false);
            dl_mode_sig.write(0);
            dl_stride_sig.write(0);
            dl_write_en_sig.write(false);
            dl_active_sig.write(false);
            dl_next_sig.write(false);

            tr_enable_sig.write(0);
            tr_shift_en_sig.write(false);
            tr_shift_mode_sig.write(0);
            tr_tid_sig.write(0);
            tr_tid_write_en_sig.write(false);
            tr_clear_regs_sig.write(false);
            tr_use_vcounter_sig.write(false);
            tr_set_vcounter_sig.write(false);
            tr_clear_vcounter_sig.write(false);
            tr_incr_vcounter_sig.write(false);

            pd_ready = false;
        } else {
            // DataLoader control
            dl_addr_len_sig.write(signals.imm);
            dl_set_addr_sig.write(signals.DL_setaddr);
            dl_set_len_sig.write(signals.DL_setlen);
            dl_mode_sig.write(signals.func3);
            dl_stride_sig.write(signals.imm);
            dl_write_en_sig.write(signals.DL_write_en);
            dl_active_sig.write(signals.DL_active);
            dl_next_sig.write(signals.DL_next);

            // TransformRegFile control
            tr_enable_sig.write(signals.tr_en);
            tr_shift_en_sig.write(signals.tr_shift);
            tr_shift_mode_sig.write(signals.func3);
            tr_tid_sig.write(signals.rid3);
            tr_tid_write_en_sig.write(signals.tr_write);
            tr_clear_regs_sig.write(signals.tr_clear_regs);
            tr_use_vcounter_sig.write(signals.tr_use_vcounter);
            tr_set_vcounter_sig.write(signals.tr_set_vcounter);
            tr_clear_vcounter_sig.write(signals.tr_clear_vcounter);
            tr_incr_vcounter_sig.write(signals.tr_incr_vcounter);

            // VMUL inputs (combinational)
            vmul_op1_sig.write(tr_vtid_out_sig.read());
            vmul_op2_sig.write(dl_dmrv_out_sig.read());

            // PD control (Port Dynamic)
            bool pd_operation = signals.pd_load;
            if (pd_operation && pd_data_in_valid.read()) {
                tr_tid_in_sig.write(pd_data_in.read());
                pd_ready = true;
            } else {
                pd_ready = false;
            }
        }

        // Drive PD ready output
        pd_data_in_ready.write(pd_ready);
    }

    // === Sequential Logic (Register Updates) ===
    void main_thread() {
        // Reset initialization
        decode_signals_reg.write(pe_decode_signals_t());
        valid_reg.write(false);
        halted_reg.write(false);

        wait(); // Wait for first clock edge

        while (true) {
            // On each clock edge, update registers with next values
            decode_signals_reg.write(decode_signals_next.read());
            valid_reg.write(valid_next.read());
            halted_reg.write(halted_next.read());

            DEBUG_MSG("[EXE_M_Stage] Clocked: Valid=" << valid_reg.read()
                      << " Halted=" << halted_reg.read()
                      << " Inst=0x" << std::hex << decode_signals_reg.read().inst << std::dec
                      << " Stall=" << stage_stall_internal.read());

            wait(); // Wait for next clock edge
        }
    }
};

} // namespace pe
} // namespace hybridacc