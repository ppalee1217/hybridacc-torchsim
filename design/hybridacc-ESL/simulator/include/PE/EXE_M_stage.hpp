#pragma once

#include "utils.hpp"
#include <systemc>
#include "TransformRegFile.hpp"
#include "VMULU.hpp"
#include "LDMA.hpp"
#include "SDMA.hpp"
#include "DataMemory.hpp"

using namespace sc_core;  // Add this to use SystemC types without prefix

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

    // Pipeline Handshake Inputs (From Upstream IF_ID)
    sc_in<pe_decode_signals_t> ID_decode_signals_in;
    sc_in<bool> valid_in;           // Renamed from signal_valid_in

    // Pipeline Handshake Outputs (To Upstream IF_ID)
    sc_out<bool> ready_out;         // New: Backpressure to IF_ID

    // Pipeline Handshake Outputs (To Downstream EXE_A)
    sc_out<v_fp16_t> vmul_out_out;
    sc_out<pe_decode_signals_t> EXE_A_decode_signals_out;
    sc_out<bool> valid_out;         // Renamed from signal_valid_out

    // Pipeline Handshake Inputs (From Downstream EXE_A)
    sc_in<bool> ready_in;           // New: Replaces stall_from_downstream

    // Status outputs
    sc_out<bool> halted_out;

    // stall monitoring (internal stall sources)
    sc_out<bool> stall_DL;
    sc_out<bool> stall_PS;
    sc_out<bool> stall_PD;

    // PS port (Port Static) - using VRDIF
    VRDIF<uint64_t> ps_data;

    // PD port (Port Dynamic) - using VRDIF
    VRDIF<uint16_t> pd_data;

    SC_CTOR(EXE_M_Stage)
        : clk("clk"),
          reset_n("reset_n"),
          stage_reset("stage_reset"),
          pe_running("pe_running"),
          ID_decode_signals_in("ID_decode_signals_in"),
          valid_in("signal_valid_in"),
          ready_out("ready_out"),
          vmul_out_out("vmul_out_out"),
          EXE_A_decode_signals_out("EXE_A_decode_signals_out"),
          valid_out("signal_valid_out"),
          ready_in("ready_in"), // Map to downstream stall if inverted externally, or assume ready logic
          halted_out("halted_out"),
          stall_DL("stall_DL"),
          stall_PS("stall_PS"),
          stall_PD("stall_PD"),
          ps_data("ps_data"),
          pd_data("pd_data"),
          TR("TR"),
          vmul("vmul"),
          ldma("ldma"),
          sdma("sdma"),
          DM("DM")
    {
        DEBUG_MSG("[Create] EXE_M_Stage", DEBUG_LEVEL_PE_STAGE);

        // Sequential Process
        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational Logic Processes

        // 1. Internal Stall Logic
        SC_METHOD(comb_internal_stall);
        sensitive << decode_signals_reg << valid_reg
                  << ldma_stall_sig << sdma_stall_sig << sdma_busy_sig
                  << ps_data.valid_in << pd_data.valid_in;

        // 2. Handshake Control (Ready/Valid Next)
        SC_METHOD(comb_handshake_logic);
        sensitive << valid_reg << internal_stall_sig << ready_in
                  << stage_reset << pe_running << valid_in << halted_reg;

        // 3. Pipeline Data Next State
        SC_METHOD(comb_pipeline_data);
        sensitive << decode_signals_reg << halted_reg << ID_decode_signals_in
                  << ready_out_sig; // Depends on if we accepted data

        // 4. Submodule Control Signals (Drive based on CURRENT registers)
        SC_METHOD(comb_submodule_control);
        sensitive << decode_signals_reg << valid_reg
                  << tr_vtid_out_sig << ldma_dmrv_out_sig
                  << pd_data.valid_in << pd_data.data_in
                  << internal_stall_sig; // Disable things if stalled?

        // 5. Output Signals
        SC_METHOD(comb_outputs);
        sensitive << decode_signals_reg << valid_reg << halted_reg
                  << vmul_out_out_sig << ready_out_sig
                  << dl_stall_internal << ps_stall_internal << pd_stall_internal;

        // PS data type conversion process
        SC_METHOD(ps_data_conversion_process);
        sensitive << ps_data.data_in;

        // Bind submodules
        bind();
    }

    // Sub-modules (public)
    TransformRegFile TR;
    VMULU vmul;
    LDMA ldma;
    SDMA sdma;
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

    // LDMA signals
    sc_signal<uint16_t> ldma_imm_sig;
    sc_signal<bool> ldma_set_addr_sig;
    sc_signal<bool> ldma_set_len_sig;
    sc_signal<bool> ldma_set_loop_sig;
    sc_signal<uint16_t> ldma_mode_sig;
    sc_signal<uint16_t> ldma_stride_sig;
    sc_signal<bool> ldma_write_en_sig;
    sc_signal<bool> ldma_active_sig;
    sc_signal<bool> ldma_next_sig;
    sc_signal<v_fp16_t> ldma_dmrv_out_sig;
    sc_signal<bool> ldma_busy_sig;
    sc_signal<bool> ldma_done_sig;
    sc_signal<bool> ldma_stall_sig;

    // SDMA signals
    sc_signal<uint16_t> sdma_imm_sig;
    sc_signal<bool> sdma_set_addr_sig;
    sc_signal<bool> sdma_set_len_sig;
    sc_signal<bool> sdma_set_loop_sig;
    sc_signal<uint16_t> sdma_mode_sig;
    sc_signal<uint16_t> sdma_stride_sig;
    // SDMA uses a single one-shot trigger `active` (no separate write_en).
    sc_signal<bool> sdma_swap_in_sig;
    sc_signal<bool> sdma_active_sig;
    sc_signal<bool> sdma_next_sig;
    sc_signal<bool> sdma_busy_sig;
    sc_signal<bool> sdma_done_sig;
    sc_signal<bool> sdma_stall_sig;
    sc_signal<bool> sdma_bank_sel_sig; // To DataMemory

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
    sc_signal<bool> internal_stall_sig;

    sc_signal<bool> ready_out_sig;      // To IF_ID
    sc_signal<v_fp16_t> vmul_out_out_sig;

    // Submodules ready signals
    sc_signal<bool> ldma_ps_ready_sig;
    sc_signal<bool> sdma_ps_ready_sig;

    void bind() {
        // Clock and reset
        TR.clk(clk);
        TR.reset_n(reset_n);

        ldma.clk(clk);
        ldma.reset_n(reset_n);
        sdma.clk(clk);
        sdma.reset_n(reset_n);

        DM.clk(clk);
        DM.reset_n(reset_n);
        DM.bank_sel(sdma_bank_sel_sig); // Connect Bank Select from SDMA

        // DataMemory <-> LDMA (Read Port)
        // LDMA drives read address, DM returns data
        ldma.dm_read_addr(dm_read_addr_sig);
        ldma.dm_read_data(dm_read_data_sig);

        // DataMemory <-> SDMA (Write Port)
        // SDMA drives write signals
        sdma.dm_write_en(dm_write_en_sig);
        sdma.dm_write_addr(dm_write_addr_sig);
        sdma.dm_write_data(dm_write_data_sig);
        sdma.dm_write_mask(dm_write_mask_sig);

        // Bind DM Ports to signals
        DM.dm_write_en(dm_write_en_sig);
        DM.dm_write_addr(dm_write_addr_sig);
        DM.dm_write_data(dm_write_data_sig);
        DM.dm_write_mask(dm_write_mask_sig);
        DM.dm_read_addr(dm_read_addr_sig);
        DM.dm_read_data(dm_read_data_sig);

        // LDMA Bindings
        ldma.imm(ldma_imm_sig);
        ldma.set_addr(ldma_set_addr_sig);
        ldma.set_len(ldma_set_len_sig);
        ldma.set_loop(ldma_set_loop_sig);
        ldma.mode(ldma_mode_sig);
        ldma.stride(ldma_stride_sig);
        ldma.write_en(ldma_write_en_sig);
        ldma.active(ldma_active_sig);
        ldma.next(ldma_next_sig);
        // LDMA does not use PS data input for load?
        // Wait, current LDMA implementation has ps_data port??
        // Let's check LDMA.hpp - Yes, it has ps_data ports but ready_logic says ready_out only true in STORE_PRE?
        // Wait, LDMA doesn't use PS data. It uses DM data.
        // It has ps_data ports but effectively unused for LOAD?
        // Let's bind dummy or shared.
        // Actually LDMA load doesn't need ps_data.
        // The previous LDMA implementation has ps_data ports inherited from template or previous pattern.
        // I'll bind them to avoid open port, but logic ignores?
        ldma.ps_data.data_in(ps_data_in_converted_sig);
        ldma.ps_data.valid_in(ps_data.valid_in);
        ldma.ps_data.ready_out(ldma_ps_ready_sig);

        // SDMA Bindings
        sdma.imm(sdma_imm_sig);
        sdma.set_addr(sdma_set_addr_sig);
        sdma.set_len(sdma_set_len_sig);
        sdma.set_loop(sdma_set_loop_sig);
        sdma.mode(sdma_mode_sig);
        sdma.stride(sdma_stride_sig);
        sdma.swap_in(sdma_swap_in_sig);
        sdma.active(sdma_active_sig);
        sdma.next(sdma_next_sig);
        sdma.bank_sel(sdma_bank_sel_sig);

        // SDMA handles PS Store
        sdma.ps_data.data_in(ps_data_in_converted_sig);
        sdma.ps_data.valid_in(ps_data.valid_in);
        // sdma.ps_data.ready_out -> Connect to ps_data.ready_out?
        // If multiple consumers (LDMA/SDMA), how to merge ready_out?
        // Since SDMA is the only one consuming PS data (Store), we can use its ready_out.
        // What if LDMA consumes PS data? (No, LDMA loads from DM to DMRV).
        // So ps_data.ready_out driven by SDMA.
        sdma.ps_data.ready_out(ps_data.ready_out);

        // Outputs
        ldma.dmrv_out(ldma_dmrv_out_sig); // Use this for TR
        ldma.busy(ldma_busy_sig);
        ldma.done(ldma_done_sig);
        ldma.dl_stall_out(ldma_stall_sig);

        sdma.busy(sdma_busy_sig);
        sdma.done(sdma_done_sig);
        sdma.dl_stall_out(sdma_stall_sig);

        // Alias for compatibility with rest of EXE_M code
        // dl_dmrv_out_sig used by TR input
        // Since only LDMA produces DMRV (Load result), we connect ldma_dmrv_out_sig to it?
        // No, dl_dmrv_out_sig is defined in EXE_M. I should map ldma_dmrv_out_sig to it?
        // Actually, I can just bind ldma.dmrv_out(dl_dmrv_out_sig). Reusing the signal.
        // Wait, I declared ldma_dmrv_out_sig above.
        // Let's use dl_dmrv_out_sig in bind.
        // (Re-binding ldma.dmrv_out)

        // Re-binding corrects:
        // ldma.dmrv_out(dl_dmrv_out_sig); // Removing ldma_dmrv_out_sig usage


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
        vmul.result(vmul_out_out_sig);
    }

    // === Combinational Logic ===

    // PS data type conversion: uint64_t -> v_fp16_t
    void ps_data_conversion_process() {
        v_fp16_t converted;
        converted.fromUint64(ps_data.data_in.read());
        ps_data_in_converted_sig.write(converted);
    }

    // 1. Internal Stall Logic
    // Calculates if the CURRENT valid instruction cannot proceed due to internal resources
    void comb_internal_stall() {
        bool valid_current = valid_reg.read();
        pe_decode_signals_t decode_current = decode_signals_reg.read();

        const bool dl_stall = ldma_stall_sig.read() || sdma_stall_sig.read();
        bool ps_stall = false;
        bool pd_stall = false;

        // Calculate stalls for current instruction
        if (valid_current) {
            // PS stall: check if PS operation needs data
            bool ps_operation = decode_current.DL_active &&
                               (decode_current.func3 == 0 || decode_current.func3 == 1);
            if (ps_operation && !ps_data.valid_in.read()) {
                ps_stall = true;
            }

            // PD stall: check if PD operation needs data
            bool pd_operation = decode_current.pd_load;
            if (pd_operation && !pd_data.valid_in.read()) {
                pd_stall = true;
            }
        }

        // Write internal stall debugging signals
        dl_stall_internal.write(dl_stall);
        ps_stall_internal.write(ps_stall);
        pd_stall_internal.write(pd_stall);

        // Total internal stall
        // Added stall for SWAPDM: if swap is requested but SDMA is busy (not IDLE)
        const bool swap_stall = decode_current.is_swap && sdma_busy_sig.read();

        internal_stall_sig.write(dl_stall || ps_stall || pd_stall || swap_stall);
    }

    // 2. Handshake Control (Ready/Valid Next)
    void comb_handshake_logic() {
        // Standard Pipeline Handshake Logic
        // Ready to accept new input if:
        // 1. Current valid data is being accepted by downstream (ready_in) OR
        // 2. We don't have valid data (Bubble)
        // AND
        // 3. We are not stalled internally

        bool internal_busy = internal_stall_sig.read();
        bool downstream_ready = ready_in.read();
        bool current_valid = valid_reg.read();

        // If we are stalled internally, we can't move current data, so we can't accept new data
        // If we have valid data and downstream is not ready, we can't accept new data
        bool ready_for_new_data = !internal_busy && (downstream_ready || !current_valid);

        ready_out_sig.write(ready_for_new_data);

        // Logic for valid_next
        bool valid_n = false;

        if (stage_reset.read() || !pe_running.read() || halted_reg.read()) {
            valid_n = false;
        } else {
            if (ready_for_new_data) {
                // If we are ready, next valid depends on input valid
                valid_n = valid_in.read();
            } else {
                // Not ready, keep current state (stall)
                valid_n = current_valid;
            }
        }
        valid_next.write(valid_n);
    }

    // 3. Pipeline Data Next State
    void comb_pipeline_data() {
        bool halted_n = halted_reg.read();
        pe_decode_signals_t decode_n = decode_signals_reg.read();

        if (stage_reset.read()) {
            halted_n = false;
            decode_n = pe_decode_signals_t();
        } else if (ready_out_sig.read()) {
             // We are accepting new data
             decode_n = ID_decode_signals_in.read();

             // Check for halt in NEW instruction
             if (valid_in.read() && decode_n.halt) {
                 halted_n = true;
             }
        }
        // Else hold current data

        decode_signals_next.write(decode_n);
        halted_next.write(halted_n);
    }

    // 4. Submodule Control Signals
    void comb_submodule_control() {
        pe_decode_signals_t signals = decode_signals_reg.read();
        bool valid = valid_reg.read();
        bool internal_stall = internal_stall_sig.read();
        // Even if stalled internally or by downstream, we might need to hold control signals active
        // e.g. Memory read enable must stay high until done.
        // However, some pulses (like write enable) might need care.
        // Assuming strictly combinatorial based on current instruction.

        bool pd_ready = false;

        if (!valid) {
            // Nothing to do
            ldma_active_sig.write(false);
            ldma_write_en_sig.write(false);
            ldma_set_addr_sig.write(false);
            ldma_set_len_sig.write(false);
            ldma_set_loop_sig.write(false);
            ldma_imm_sig.write(0);
            ldma_stride_sig.write(0);
            ldma_next_sig.write(false);
            ldma_mode_sig.write(0);

            sdma_active_sig.write(false);
            sdma_set_addr_sig.write(false);
            sdma_set_len_sig.write(false);
            sdma_set_loop_sig.write(false);
            sdma_imm_sig.write(0);
            sdma_stride_sig.write(0);
            sdma_mode_sig.write(0);
            sdma_swap_in_sig.write(false);
            sdma_next_sig.write(false);

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
            tr_tid_in_sig.write(0);

            vmul_op1_sig.write(v_fp16_t());
            vmul_op2_sig.write(v_fp16_t());

            pd_ready = false;
        } else {
            // DL Control -> Split to LDMA/SDMA
            if (signals.DL_active) {
                // If DL_is_sdma is true, it is SDMA (e.g. Store)
                // If DL_is_sdma is false, it is LDMA (e.g. Load)
                if (signals.DL_is_sdma) {
                    // SDMA start trigger is merged: `active` implies "this is a store task trigger".
                    sdma_active_sig.write(true);
                    ldma_active_sig.write(false);
                    ldma_write_en_sig.write(false);
                } else {
                    ldma_active_sig.write(true);
                    ldma_write_en_sig.write(false);
                    sdma_active_sig.write(false);
                }
            } else {
                ldma_active_sig.write(false); sdma_active_sig.write(false);
                ldma_write_en_sig.write(false);
            }

            // Defaults for config
            ldma_set_addr_sig.write(false); sdma_set_addr_sig.write(false);
            ldma_set_len_sig.write(false); sdma_set_len_sig.write(false);
            ldma_set_loop_sig.write(false); sdma_set_loop_sig.write(false);
            sdma_swap_in_sig.write(false);

            if (signals.DL_setaddr) {
                if (signals.DL_is_sdma) sdma_set_addr_sig.write(true);
                else ldma_set_addr_sig.write(true);
            }
            if (signals.DL_setlen) {
                if (signals.DL_is_sdma) sdma_set_len_sig.write(true);
                else ldma_set_len_sig.write(true);
            }
            if (signals.DL_setloop) {
                if (signals.DL_is_sdma) sdma_set_loop_sig.write(true);
                else ldma_set_loop_sig.write(true);
            }
            if (signals.is_swap) {
                sdma_swap_in_sig.write(true);
            }

            // Shared immediate / mode
            ldma_imm_sig.write(signals.imm);
            sdma_imm_sig.write(signals.imm);
            ldma_mode_sig.write(signals.func3);
            sdma_mode_sig.write(signals.func3);
            ldma_stride_sig.write(signals.imm);
            sdma_stride_sig.write(signals.imm);

            // Next
            ldma_next_sig.write(signals.DL_next);
            sdma_next_sig.write(false); // SDMA next signal not used in decoder? or DL_next used for both?
            // Actually, DL_next is generic. If instruct says "Next", it applies to the active unit.
            // If DL_is_sdma, then send next to SDMA.
             if (signals.DL_next) {
                if (signals.DL_is_sdma) sdma_next_sig.write(true);
                else ldma_next_sig.write(true);
             } else {
                 sdma_next_sig.write(false);
                 ldma_next_sig.write(false);
             }


            // TransformRegFile Control
            tr_enable_sig.write(signals.tr_en);
            tr_shift_en_sig.write(signals.tr_shift);
            tr_shift_mode_sig.write(signals.imm & 0x3);
            tr_tid_sig.write(signals.rid3);

            if (signals.tr_write) {
                 tr_tid_write_en_sig.write(true);
                 if (signals.pd_load) { // TR = PD
                     tr_tid_in_sig.write((fp16_t)pd_data.data_in.read());
                 } else { // TR = DMRV
                     // From DMRV (Lead lane of LDMA output)
                     tr_tid_in_sig.write(ldma_dmrv_out_sig.read().lanes[0]);
                 }
            } else {
                 tr_tid_write_en_sig.write(false);
                 tr_tid_in_sig.write(0);
            }

            tr_clear_regs_sig.write(signals.tr_clear_regs);
            tr_use_vcounter_sig.write(signals.tr_use_vcounter);
            tr_set_vcounter_sig.write(signals.tr_set_vcounter);
            tr_clear_vcounter_sig.write(signals.tr_clear_vcounter);
            // Counter Increment Guarding
            bool advancing = ready_in.read() && !internal_stall;
            tr_incr_vcounter_sig.write(signals.tr_incr_vcounter && advancing);

            // VMULU Control
            // VMAC/VMUL: op1=VT, op2=DMRV
            vmul_op1_sig.write(tr_vtid_out_sig.read());
            vmul_op2_sig.write(ldma_dmrv_out_sig.read());

            // Prepare PD Ready
            if (signals.pd_load && !internal_stall) {
                // If we are loading from PD, checking if data is valid happens inside if(valid_in) usually?
                // Wait, here we only set Ready Out.
                // We are ready to consume if we are not stalled.
                pd_ready = true;
            }
        }

        // Drive PD ready output
        pd_data.ready_out.write(pd_ready);
    }

    // 5. Output Signals
    void comb_outputs() {
        bool valid = valid_reg.read();
        bool internal_stall = internal_stall_sig.read();

        // Valid Output to Downstream:
        // Valid if current stage has valid data AND no internal stall
        // (If internal stall, data is not ready to move)
        bool out_valid = valid && !internal_stall;

        // Mask outputs if invalid
        if (out_valid) {
             EXE_A_decode_signals_out.write(decode_signals_reg.read());
             vmul_out_out.write(vmul_out_out_sig.read());
        } else {
             EXE_A_decode_signals_out.write(pe_decode_signals_t());
             vmul_out_out.write(v_fp16_t());
        }

        valid_out.write(out_valid);

        // Ready Output to Upstream
        ready_out.write(ready_out_sig.read());

        // Debug outputs
        halted_out.write(halted_reg.read());
        stall_DL.write(dl_stall_internal.read());
        stall_PS.write(ps_stall_internal.read());
        stall_PD.write(pd_stall_internal.read());
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

            if (pe_running.read()) {
                DEBUG_MSG("[EXE_M_Stage] Clocked: Valid=" << valid_reg.read()
                      << " ReadyIn=" << ready_in.read()
                      << " ReadyOut=" << ready_out_sig.read()
                      << " Inst=0x" << std::hex << decode_signals_reg.read().inst << std::dec
                      << " IntStall=" << internal_stall_sig.read(), DEBUG_LEVEL_PE_STAGE);
            }

            wait(); // Wait for next clock edge
        }
    }
};

} // namespace pe
} // namespace hybridacc