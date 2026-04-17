#pragma once

#include "Utils/utils.hpp"
#include <systemc>
#include "VADDU.hpp"
#include "PsumRegFile.hpp"

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

// =============================================================================
// EXE_A Stage State Machine
// =============================================================================
enum class EXE_A_State {
    IDLE,              // 無有效指令
    NORMAL_MODE,       // Normal Mode: 1-stage (VPSUM/VMUL等)
    VMAC_S1,           // VMAC Mode Stage 1
    VMAC_S2,           // VMAC Mode Stage 2
    VMAC_S3,           // VMAC Mode Stage 3
    WAIT_PLI,          // 等待 PLI 數據
    EXEC_PLI_VADDU,    // PLI 數據就緒,執行 VADDU
    WAIT_PLO,          // 等待 PLO 握手
};

inline std::ostream& operator<<(std::ostream& os, EXE_A_State state) {
    switch (state) {
        case EXE_A_State::IDLE: return os << "IDLE";
        case EXE_A_State::NORMAL_MODE: return os << "NORMAL_MODE";
        case EXE_A_State::VMAC_S1: return os << "VMAC_S1";
        case EXE_A_State::VMAC_S2: return os << "VMAC_S2";
        case EXE_A_State::VMAC_S3: return os << "VMAC_S3";
        case EXE_A_State::WAIT_PLI: return os << "WAIT_PLI";
        case EXE_A_State::EXEC_PLI_VADDU: return os << "EXEC_PLI_VADDU";
        case EXE_A_State::WAIT_PLO: return os << "WAIT_PLO";
        default: return os << "UNKNOWN";
    }
}

inline void sc_trace(sc_core::sc_trace_file* tf, const EXE_A_State& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}

// =============================================================================
// State Transition Result Structure
// =============================================================================
struct StateTransitionResult {
    EXE_A_State next_state;
    pe_decode_signals_t next_decode;
    pe_decode_signals_t next_decode_s1;
    pe_decode_signals_t next_decode_s2;
    v_fp16_t next_vmul_data;
    v_fp16_t next_pli_data;
    v_fp16_t next_vaddu_result;
    bool next_halted;
    // VMAC pipeline registers
    fp16_t next_s1_reg0;
    fp16_t next_s1_reg1;
    fp16_t next_s2_reg;
};

// =============================================================================
// EXE_A Stage Module
// =============================================================================
SC_MODULE(EXE_A_Stage) {
public:
    // ========================= Ports =========================
    sc_in<bool> clk;
    sc_in<bool> reset_n;
    sc_in<bool> stage_reset;
    sc_in<bool> pe_running;

    // Pipeline inputs
    sc_in<v_fp16_t> vmul_out_in;
    sc_in<pe_decode_signals_t> EXE_M_decode_signals_in;
    sc_in<bool> valid_in;           // Renamed from signal_valid_in

    // Flow control outputs
    sc_out<bool> ready_out;         // Backpressure to EXE_M (Replaces stall_adder logic)

    // Status outputs
    sc_out<bool> halted_out;
    sc_out<bool> stall_port_pli; // Debug output
    sc_out<bool> stall_port_plo; // Debug output

    // Local Network ports (PLI/PLO) - using VRDIF/VRDOF
    VRDIF<uint64_t> pli;
    VRDOF<uint64_t> plo;

    // ========================= Sub-modules =========================
    VADDU vaddu;
    PsumRegFile PR;

    // ========================= Constructor =========================
    SC_CTOR(EXE_A_Stage)
        : clk("clk"), reset_n("reset_n"), stage_reset("stage_reset"),
          pe_running("pe_running"), vmul_out_in("vmul_out_in"),
          EXE_M_decode_signals_in("EXE_M_decode_signals_in"),
          valid_in("signal_valid_in"),
          ready_out("stall_adder"), // Map to old stall signal pin location if needed, but logical meaning is inverted
          halted_out("halted_out"),
          stall_port_pli("stall_port_pli"),
          stall_port_plo("stall_port_plo"),
          pli("pli"), plo("plo"),
          vaddu("vaddu"), PR("PR")
    {
        DEBUG_MSG("[Create] EXE_A_Stage", DEBUG_LEVEL_PE_STAGE);

        // Sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational processes

        // 1. Ready Out Logic (Backpressure calculation)
        SC_METHOD(comb_ready_out);
        sensitive << state_reg << valid_in << EXE_M_decode_signals_in
              << plo_buf_valid_reg << plo.ready_in;

        // 2. Next State Logic
        SC_METHOD(comb_next_state);
        sensitive << state_reg << decode_reg << vmul_data_reg << pli_data_reg
                  << valid_in << EXE_M_decode_signals_in << vmul_out_in
                  << stage_reset << pe_running << halted_reg
                  << pli.valid_in << pli.data_in << plo.ready_in
                  << s1_reg0 << s1_reg1 << s2_reg << vaddu_result_reg
                  << vaddu_result_sig
                  << ready_out_sig << plo_buf_valid_reg; // State transitions depend on if we accepted data

        // VADDU Control
        SC_METHOD(comb_vaddu_op1);
        sensitive << state_reg << vmul_data_reg << pli_data_reg << s1_reg0 << pr_p_out_sig;

        SC_METHOD(comb_vaddu_op2);
        sensitive << state_reg << vmul_data_reg << s1_reg1 << s2_reg << pr_vp_out_sig;

        // PR Control
        SC_METHOD(comb_pr_static_signals);
        sensitive << decode_reg << decode_s2_reg << state_reg;

        SC_METHOD(comb_pr_mode);
        sensitive << state_reg << decode_reg;

        SC_METHOD(comb_pr_p_in);
        sensitive << state_reg << vaddu_result_sig;

        SC_METHOD(comb_pr_vp_in);
        sensitive << state_reg << vaddu_result_sig;

        SC_METHOD(comb_pr_write_en);
        sensitive << state_reg << decode_reg << decode_s2_reg;

        SC_METHOD(comb_pr_incr);
        sensitive << state_reg << decode_reg << decode_s2_reg;

        // Outputs
        SC_METHOD(comb_outputs_misc);
        sensitive << state_reg << vaddu_result_reg << halted_reg
              << plo_buf_valid_reg << plo_buf_data_reg;

        SC_METHOD(comb_pli_handshake);
        sensitive << state_reg << valid_in << EXE_M_decode_signals_in
                  << pli.valid_in << ready_out_sig;

        // PLO buffer update
        SC_METHOD(comb_plo_buffer);
        sensitive << state_reg << vaddu_result_sig
                  << plo.ready_in << stage_reset
              << plo_buf_valid_reg << plo_buf_data_reg;

        bind_submodules();
    }

private:
    // ========================= Pipeline Registers =========================
    sc_signal<EXE_A_State> state_reg;
    sc_signal<pe_decode_signals_t> decode_reg;
    sc_signal<pe_decode_signals_t> decode_s1_reg; // Pipeline reg for Stage 2
    sc_signal<pe_decode_signals_t> decode_s2_reg; // Pipeline reg for Stage 3
    sc_signal<v_fp16_t> vmul_data_reg;
    sc_signal<v_fp16_t> pli_data_reg;
    sc_signal<v_fp16_t> vaddu_result_reg;
    sc_signal<bool> halted_reg;

    // PLO push pipeline buffer (decouple VPSUM from PLO ready)
    sc_signal<bool> plo_buf_valid_reg;
    sc_signal<uint64_t> plo_buf_data_reg;

    // VMAC 3-stage pipeline registers
    sc_signal<fp16_t> s1_reg0;  // Stage 1: vmul_out[0] + vmul_out[1]
    sc_signal<fp16_t> s1_reg1;  // Stage 1: vmul_out[2] + vmul_out[3]
    sc_signal<fp16_t> s2_reg;   // Stage 2: s1_reg0 + s1_reg1

    // ========================= Next-State Signals =========================
    sc_signal<EXE_A_State> state_reg_next;
    sc_signal<pe_decode_signals_t> decode_reg_next;
    sc_signal<pe_decode_signals_t> decode_s1_reg_next;
    sc_signal<pe_decode_signals_t> decode_s2_reg_next;
    sc_signal<v_fp16_t> vmul_data_reg_next;
    sc_signal<v_fp16_t> pli_data_reg_next;
    sc_signal<v_fp16_t> vaddu_result_reg_next;
    sc_signal<bool> halted_reg_next;

    sc_signal<bool> plo_buf_valid_reg_next;
    sc_signal<uint64_t> plo_buf_data_reg_next;

    // VMAC pipeline next-state signals
    sc_signal<fp16_t> s1_reg0_next;
    sc_signal<fp16_t> s1_reg1_next;
    sc_signal<fp16_t> s2_reg_next;

    // ========================= VADDU Interface Signals =========================
    sc_signal<v_fp16_t> vaddu_op1_sig;
    sc_signal<v_fp16_t> vaddu_op2_sig;
    sc_signal<v_fp16_t> vaddu_result_sig;

    // Internal signal for Ready Out
    sc_signal<bool> ready_out_sig;

    // ========================= PsumRegFile Interface Signals =========================
    sc_signal<bool> pr_enable_sig;
    sc_signal<int> pr_pid_sig;
    sc_signal<bool> pr_vpid_write_en_sig;
    sc_signal<int> pr_mode_sig;
    sc_signal<fp16_t> pr_p_in_sig;
    sc_signal<v_fp16_t> pr_vp_in_sig;
    sc_signal<fp16_t> pr_p_out_sig;
    sc_signal<v_fp16_t> pr_vp_out_sig;
    sc_signal<bool> pr_clear_regs_sig;
    sc_signal<bool> pr_use_pcounter_sig;
    sc_signal<bool> pr_clear_pcounter_sig;
    sc_signal<bool> pr_incr_pcounter_sig;

    // ========================= Submodule Binding =========================
    void bind_submodules() {
        // VADDU - combinational module
        vaddu.op1(vaddu_op1_sig);
        vaddu.op2(vaddu_op2_sig);
        vaddu.result(vaddu_result_sig);

        // PsumRegFile
        PR.clk(clk);
        PR.reset_n(reset_n);
        PR.enable(pr_enable_sig);
        PR.pid(pr_pid_sig);
        PR.p_in(pr_p_in_sig);
        PR.vp_in(pr_vp_in_sig);
        PR.vpid_write_en(pr_vpid_write_en_sig);
        PR.mode(pr_mode_sig);
        PR.p_out(pr_p_out_sig);
        PR.vp_out(pr_vp_out_sig);
        PR.clear_regs(pr_clear_regs_sig);
        PR.use_pcounter(pr_use_pcounter_sig);
        PR.clear_pcounter(pr_clear_pcounter_sig);
        PR.incr_pcounter(pr_incr_pcounter_sig);
    }

    // ========================= Sequential Process =========================
    void seq_process() {
        // Reset
        state_reg.write(EXE_A_State::IDLE);
        decode_reg.write(pe_decode_signals_t());
        decode_s1_reg.write(pe_decode_signals_t());
        decode_s2_reg.write(pe_decode_signals_t());
        vmul_data_reg.write(v_fp16_t());
        pli_data_reg.write(v_fp16_t());
        vaddu_result_reg.write(v_fp16_t());
        halted_reg.write(false);
        plo_buf_valid_reg.write(false);
        plo_buf_data_reg.write(0);
        s1_reg0.write(0);
        s1_reg1.write(0);
        s2_reg.write(0);
        wait();

        while (true) {
            // Update all registers from their _next signals
            state_reg.write(state_reg_next.read());
            decode_reg.write(decode_reg_next.read());
            decode_s1_reg.write(decode_s1_reg_next.read());
            decode_s2_reg.write(decode_s2_reg_next.read());
            vmul_data_reg.write(vmul_data_reg_next.read());
            pli_data_reg.write(pli_data_reg_next.read());
            vaddu_result_reg.write(vaddu_result_reg_next.read());
            halted_reg.write(halted_reg_next.read());
            plo_buf_valid_reg.write(plo_buf_valid_reg_next.read());
            plo_buf_data_reg.write(plo_buf_data_reg_next.read());
            s1_reg0.write(s1_reg0_next.read());
            s1_reg1.write(s1_reg1_next.read());
            s2_reg.write(s2_reg_next.read());

            DEBUG_MSG("[EXE_A_Stage] Clocked: State=" << state_reg.read()
                << " -> " << state_reg_next.read()
                << " ReadyOut=" << ready_out_sig.read()
                << " Inst=0x" << std::hex << decode_reg.read().inst << std::dec
                , DEBUG_LEVEL_PE_STAGE);

            wait();
        }
    }

    // ========================= Comb: Ready Out Logic (Backpressure) =========================

    void comb_ready_out() {
        EXE_A_State state = state_reg.read();
        bool ready = false;

        if (state == EXE_A_State::IDLE) {
            // In IDLE, we are ready for new instruction (unless VPSUM would overflow PLO buffer)
            if (valid_in.read() && EXE_M_decode_signals_in.read().pli_plo_operation
                && plo_buf_valid_reg.read() && !plo.ready_in.read()) {
                ready = false;
            } else {
                ready = true;
            }
        } else if (state == EXE_A_State::NORMAL_MODE) {
            // Normal mode is 1-cycle execution, so we are ready for next instruction
            if (valid_in.read() && EXE_M_decode_signals_in.read().pli_plo_operation
                && plo_buf_valid_reg.read() && !plo.ready_in.read()) {
                ready = false;
            } else {
                ready = true;
            }
        } else if (state == EXE_A_State::VMAC_S1) {
            // In VMAC_S1 (Pipeline Active), we check if we can accept the next instruction
            if (valid_in.read()) {
                 pe_decode_signals_t next_decode = EXE_M_decode_signals_in.read();
                 // If next is VMAC, we don't stall (Pipeline it)
                 if (next_decode.vaddu_en && next_decode.vaddu_mode == 0) {
                     ready = true;
                 } else {
                     // Next is not VMAC, we must stall upstream to drain pipeline
                     ready = false;
                 }
            } else {
                 // No valid input, we are ready (to accept a bubble or new valid)
                 ready = true;
            }
        } else if (state == EXE_A_State::EXEC_PLI_VADDU) {
            // VPSUM execution can accept next instruction if PLO buffer can take output
            bool can_push = !plo_buf_valid_reg.read() || plo.ready_in.read();
            ready = can_push;
        } else if (state == EXE_A_State::WAIT_PLO) {
            // Can accept next instruction only when PLO handshake will complete
            ready = plo.ready_in.read();
        } else {
            // Other states (VMAC_S2, VMAC_S3, NORMAL, EXEC_PLI, WAIT_*, etc.) are busy
            ready = false;
        }

        ready_out_sig.write(ready);

        // Drive Output Port (inverted for deprecated stall pin if needed, but here we use ready)
        // Note: The port name in CTOR is "stall_adder" mapped to `ready_out` signal in my variable list?
        // Ah, in CTOR I mapped `ready_out("stall_adder")`.
        // If the external world expects "stall", I should invert it.
        // Assuming user wants `ready` semantic across the board as requested.
        // I will write `ready` logic relative to standard valid/ready.
        // If the pin is physically named "stall_adder" in `systemc` traces, it will show ready behavior now.
        ready_out.write(ready);
    }

    // ========================= Comb: Next State Logic =========================

    // 處理 IDLE 狀態的轉換
    void handle_idle_state(StateTransitionResult& result) {
        if (valid_in.read() && ready_out_sig.read()) { // Only process if we accepted input
             // Check downstream backpressure?
             // EXE_A is last stage usually. But WAIT_PLO needs handshake.
             // Assume no downstream stall for general logic unless explicit IO.

            result.next_decode = EXE_M_decode_signals_in.read();
            result.next_vmul_data = vmul_out_in.read();

            // Clear pipeline registers
            result.next_decode_s1 = pe_decode_signals_t();
            result.next_decode_s2 = pe_decode_signals_t();

            DEBUG_MSG("[EXE_A_Stage] New instruction, vmul_data = " << vmul_out_in.read(), DEBUG_LEVEL_PE_STAGE);

            // 檢查 HALT
            if (result.next_decode.halt) {
                result.next_halted = true;
                result.next_state = EXE_A_State::IDLE;
                DEBUG_MSG("[EXE_A_Stage] HALT detected", DEBUG_LEVEL_PE_STAGE);
            }
            // PLI-PLO 操作 (VPSUM/VPSUMR)
            else if (result.next_decode.pli_plo_operation) {
                if(pli.valid_in.read()) {
                    v_fp16_t captured_pli_data;
                    captured_pli_data.fromUint64(pli.data_in.read());
                    result.next_pli_data = captured_pli_data;
                    result.next_state = EXE_A_State::EXEC_PLI_VADDU;
                }else{
                    result.next_state = EXE_A_State::WAIT_PLI;
                }
                DEBUG_MSG("[EXE_A_Stage] Start PLI-PLO operation", DEBUG_LEVEL_PE_STAGE);
            }
            // VMAC 模式 (vaddu_mode==0 表示 ACCUMULATE)
            else if (result.next_decode.vaddu_en && result.next_decode.vaddu_mode == 0) {
                result.next_state = EXE_A_State::VMAC_S1;
                DEBUG_MSG("[EXE_A_Stage] Start VMAC mode (Pipeline Active)", DEBUG_LEVEL_PE_STAGE);
            }
            // Normal 模式 (VMUL/VMULR 等, vaddu_mode==1 表示 ADD)
            else if (result.next_decode.vaddu_en) {
                result.next_state = EXE_A_State::NORMAL_MODE;
                DEBUG_MSG("[EXE_A_Stage] Start Normal mode (1-stage)", DEBUG_LEVEL_PE_STAGE);
            }
            // 其他指令 (只操作 PR,不需要運算)
            else {
                result.next_state = EXE_A_State::IDLE;
                DEBUG_MSG("[EXE_A_Stage] Execute PR-only operation", DEBUG_LEVEL_PE_STAGE);
            }
        }
    }

    // 處理 VMAC Stage 1 (Pipeline Active)
    void handle_vmac_s1_state(StateTransitionResult& result) {
        // Pipeline Shift Logic (Always happens if not blocked by non-existent downstream stall)
        v_fp16_t vaddu_out = vaddu_result_sig.read();
        result.next_s1_reg0 = vaddu_out[0];
        result.next_s1_reg1 = vaddu_out[1];
        result.next_s2_reg = vaddu_out[2]; // Shift from S1 to S2

        // Shift decode signals
        result.next_decode_s1 = decode_reg.read();
        result.next_decode_s2 = decode_s1_reg.read();

        // Check for new instruction to fill Stage 1
        if (valid_in.read() && ready_out_sig.read()) {
            pe_decode_signals_t next_decode = EXE_M_decode_signals_in.read();
            // If next is VMAC, accept it and stay in VMAC_S1
            if (next_decode.vaddu_en && next_decode.vaddu_mode == 0) {
                result.next_decode = next_decode;
                result.next_vmul_data = vmul_out_in.read();
                result.next_state = EXE_A_State::VMAC_S1;
                DEBUG_MSG("[EXE_A_Stage] Pipeline: Accept new VMAC", DEBUG_LEVEL_PE_STAGE);
            } else {
                // Next is NOT VMAC, start draining (we blocked upstream in ready_out logic)
                result.next_decode = pe_decode_signals_t(); // Bubble
                result.next_state = EXE_A_State::VMAC_S2;
                DEBUG_MSG("[EXE_A_Stage] Pipeline: Next not VMAC, drain (Go to S2)", DEBUG_LEVEL_PE_STAGE);
            }
        } else {
            // No valid input, start draining
            result.next_decode = pe_decode_signals_t(); // Bubble
            result.next_state = EXE_A_State::VMAC_S2;
            DEBUG_MSG("[EXE_A_Stage] Pipeline: No input, drain (Go to S2)", DEBUG_LEVEL_PE_STAGE);
        }
    }

    // 處理 VMAC Stage 2 (Draining Stage 1)
    void handle_vmac_s2_state(StateTransitionResult& result) {
        // Pipeline Shift Logic
        v_fp16_t vaddu_out = vaddu_result_sig.read();
        // Stage 1 is bubble, so s1_reg doesn't matter much, but keep shifting logic
        result.next_s1_reg0 = vaddu_out[0];
        result.next_s1_reg1 = vaddu_out[1];
        result.next_s2_reg = vaddu_out[2]; // Shift from S1 to S2

        // Shift decode signals
        result.next_decode_s1 = decode_reg.read(); // Should be bubble from S1
        result.next_decode_s2 = decode_s1_reg.read(); // Valid inst from S1 moves to S2 (Stage 3)

        // Insert bubble at Stage 1
        result.next_decode = pe_decode_signals_t();

        result.next_state = EXE_A_State::VMAC_S3;
        DEBUG_MSG("[EXE_A_Stage] VMAC S2: Draining...", DEBUG_LEVEL_PE_STAGE);
    }

    // 處理 VMAC Stage 3 (Draining Stage 2)
    void handle_vmac_s3_state(StateTransitionResult& result) {
        // Pipeline Shift Logic
        // Stage 2 is bubble, so s2_reg doesn't matter
        // Stage 3 (Writeback) happens in this cycle (combinational)

        // Shift decode signals
        result.next_decode_s2 = decode_s1_reg.read(); // Should be bubble

        result.next_state = EXE_A_State::IDLE;
        DEBUG_MSG("[EXE_A_Stage] VMAC S3: Draining complete, go IDLE", DEBUG_LEVEL_PE_STAGE);
    }

    // 處理 Normal Mode (1-stage)
    void handle_normal_mode_state(StateTransitionResult& result) {
        // Normal mode execution happens combinationally in current cycle

        // Check for next instruction immediately
        if (valid_in.read() && ready_out_sig.read()) {
            result.next_decode = EXE_M_decode_signals_in.read();
            result.next_vmul_data = vmul_out_in.read();

            // Clear pipeline registers
            result.next_decode_s1 = pe_decode_signals_t();
            result.next_decode_s2 = pe_decode_signals_t();

            DEBUG_MSG("[EXE_A_Stage] Normal Mode: Accept new instruction", DEBUG_LEVEL_PE_STAGE);

            if (result.next_decode.halt) {
                result.next_halted = true;
                result.next_state = EXE_A_State::IDLE;
            }
            else if (result.next_decode.pli_plo_operation) {
                 if(pli.valid_in.read()) {
                    v_fp16_t captured_pli_data;
                    captured_pli_data.fromUint64(pli.data_in.read());
                    result.next_pli_data = captured_pli_data;
                    result.next_state = EXE_A_State::EXEC_PLI_VADDU;
                 } else {
                    result.next_state = EXE_A_State::WAIT_PLI;
                 }
            }
            else if (result.next_decode.vaddu_en && result.next_decode.vaddu_mode == 0) {
                result.next_state = EXE_A_State::VMAC_S1;
            }
            else if (result.next_decode.vaddu_en) {
                result.next_state = EXE_A_State::NORMAL_MODE;
            }
            else {
                result.next_state = EXE_A_State::IDLE;
            }
        } else {
            result.next_state = EXE_A_State::IDLE;
            DEBUG_MSG("[EXE_A_Stage] Normal mode complete, no new input", DEBUG_LEVEL_PE_STAGE);
        }
    }

    // 處理 WAIT_PLI 狀態的轉換
    void handle_wait_pli_state(StateTransitionResult& result) {
        if (pli.valid_in.read()) {
            v_fp16_t captured_pli_data;
            captured_pli_data.fromUint64(pli.data_in.read());
            result.next_pli_data = captured_pli_data;
            result.next_state = EXE_A_State::EXEC_PLI_VADDU;
        }
    }

    // 處理 EXEC_PLI_VADDU 狀態的轉換
    void handle_exec_pli_vaddu_state(StateTransitionResult& result) {
        // PLI + PR (使用 VADDU)
        v_fp16_t vaddu_out = vaddu_result_sig.read();
        result.next_vaddu_result = vaddu_out;

        const bool can_push = !plo_buf_valid_reg.read() || plo.ready_in.read();
        if (!can_push) {
            result.next_state = EXE_A_State::WAIT_PLO;
            return;
        }
        DEBUG_MSG("[EXE_A_Stage] PLI VADDU complete: " << vaddu_out, DEBUG_LEVEL_PE_STAGE);

        const bool accepting_new = ready_out_sig.read() && valid_in.read();
        if (accepting_new) {
            pe_decode_signals_t next_decode = EXE_M_decode_signals_in.read();
            result.next_decode = next_decode;
            result.next_vmul_data = vmul_out_in.read();

            // Clear pipeline registers
            result.next_decode_s1 = pe_decode_signals_t();
            result.next_decode_s2 = pe_decode_signals_t();

            if (next_decode.halt) {
                result.next_halted = true;
                result.next_state = EXE_A_State::IDLE;
            } else if (next_decode.pli_plo_operation) {
                if (pli.valid_in.read()) {
                    v_fp16_t captured_pli_data;
                    captured_pli_data.fromUint64(pli.data_in.read());
                    result.next_pli_data = captured_pli_data;
                    result.next_state = EXE_A_State::EXEC_PLI_VADDU;
                } else {
                    result.next_state = EXE_A_State::WAIT_PLI;
                }
            } else if (next_decode.vaddu_en && next_decode.vaddu_mode == 0) {
                result.next_state = EXE_A_State::VMAC_S1;
            } else if (next_decode.vaddu_en) {
                result.next_state = EXE_A_State::NORMAL_MODE;
            } else {
                result.next_state = EXE_A_State::IDLE;
            }
        } else {
            result.next_state = EXE_A_State::IDLE;
        }
    }

    // 處理 WAIT_PLO 狀態的轉換
    void handle_wait_plo_state(StateTransitionResult& result) {
        if (!plo.ready_in.read()) {
            result.next_state = EXE_A_State::WAIT_PLO;
            return;
        }

        const bool accepting_new = ready_out_sig.read() && valid_in.read();
        if (accepting_new) {
            pe_decode_signals_t next_decode = EXE_M_decode_signals_in.read();
            result.next_decode = next_decode;
            result.next_vmul_data = vmul_out_in.read();

            // Clear pipeline registers
            result.next_decode_s1 = pe_decode_signals_t();
            result.next_decode_s2 = pe_decode_signals_t();

            if (next_decode.halt) {
                result.next_halted = true;
                result.next_state = EXE_A_State::IDLE;
            } else if (next_decode.pli_plo_operation) {
                if (pli.valid_in.read()) {
                    v_fp16_t captured_pli_data;
                    captured_pli_data.fromUint64(pli.data_in.read());
                    result.next_pli_data = captured_pli_data;
                    result.next_state = EXE_A_State::EXEC_PLI_VADDU;
                } else {
                    result.next_state = EXE_A_State::WAIT_PLI;
                }
            } else if (next_decode.vaddu_en && next_decode.vaddu_mode == 0) {
                result.next_state = EXE_A_State::VMAC_S1;
            } else if (next_decode.vaddu_en) {
                result.next_state = EXE_A_State::NORMAL_MODE;
            } else {
                result.next_state = EXE_A_State::IDLE;
            }
        } else {
            result.next_state = EXE_A_State::IDLE;
        }

        DEBUG_MSG("[EXE_A_Stage] PLO handshake done", DEBUG_LEVEL_PE_STAGE);
    }

    // 主要的 next state 邏輯
    void comb_next_state() {
        // 讀取當前狀態
        StateTransitionResult result;
        result.next_state = state_reg.read();
        result.next_decode = decode_reg.read();
        result.next_decode_s1 = decode_s1_reg.read();
        result.next_decode_s2 = decode_s2_reg.read();
        result.next_vmul_data = vmul_data_reg.read();
        result.next_pli_data = pli_data_reg.read();
        result.next_vaddu_result = vaddu_result_reg.read();
        result.next_halted = halted_reg.read();
        result.next_s1_reg0 = s1_reg0.read();
        result.next_s1_reg1 = s1_reg1.read();
        result.next_s2_reg = s2_reg.read();

        // ===== Reset & Halt Handling =====
        if (stage_reset.read()) {
            result.next_state = EXE_A_State::IDLE;
            result.next_decode = pe_decode_signals_t();
            result.next_decode_s1 = pe_decode_signals_t();
            result.next_decode_s2 = pe_decode_signals_t();
            result.next_halted = false;
            result.next_s1_reg0 = 0;
            result.next_s1_reg1 = 0;
            result.next_s2_reg = 0;
        }
        else if (!pe_running.read() || result.next_halted) {
            // Keep current state when not running or halted
        }
        // ===== FSM State Transitions =====
        else {
            switch (result.next_state) {
                case EXE_A_State::IDLE:
                    handle_idle_state(result);
                    break;

                case EXE_A_State::VMAC_S1:
                    handle_vmac_s1_state(result);
                    break;

                case EXE_A_State::VMAC_S2:
                    handle_vmac_s2_state(result);
                    break;

                case EXE_A_State::VMAC_S3:
                    handle_vmac_s3_state(result);
                    break;

                case EXE_A_State::NORMAL_MODE:
                    handle_normal_mode_state(result);
                    break;

                case EXE_A_State::WAIT_PLI:
                    handle_wait_pli_state(result);
                    break;

                case EXE_A_State::EXEC_PLI_VADDU:
                    handle_exec_pli_vaddu_state(result);
                    break;

                case EXE_A_State::WAIT_PLO:
                    handle_wait_plo_state(result);
                    break;

                default:
                    result.next_state = EXE_A_State::IDLE;
                    break;
            }
        }

        // Write to _next signals
        state_reg_next.write(result.next_state);
        decode_reg_next.write(result.next_decode);
        decode_s1_reg_next.write(result.next_decode_s1);
        decode_s2_reg_next.write(result.next_decode_s2);
        vmul_data_reg_next.write(result.next_vmul_data);
        pli_data_reg_next.write(result.next_pli_data);
        vaddu_result_reg_next.write(result.next_vaddu_result);
        halted_reg_next.write(result.next_halted);
        s1_reg0_next.write(result.next_s1_reg0);
        s1_reg1_next.write(result.next_s1_reg1);
        s2_reg_next.write(result.next_s2_reg);
    }

    // ========================= Comb: VADDU Control =========================
    void comb_vaddu_op1() {
        EXE_A_State state = state_reg.read();
        v_fp16_t vmul = vmul_data_reg.read();
        v_fp16_t pli_data = pli_data_reg.read();
        v_fp16_t op1;
        op1 = v_fp16_t(); // Clear

        switch (state) {
            case EXE_A_State::VMAC_S1:
            case EXE_A_State::VMAC_S2:
            case EXE_A_State::VMAC_S3:
                op1[0] = vmul[0]; op1[1] = vmul[2]; op1[2] = s1_reg0.read(); op1[3] = pr_p_out_sig.read();
                break;
            case EXE_A_State::NORMAL_MODE:
                op1 = vmul;
                break;
            case EXE_A_State::EXEC_PLI_VADDU:
            case EXE_A_State::WAIT_PLI:
            case EXE_A_State::WAIT_PLO:
                op1 = pli_data;
                break;
            default:
                break;
        }
        vaddu_op1_sig.write(op1);
    }

    void comb_vaddu_op2() {
        EXE_A_State state = state_reg.read();
        v_fp16_t vmul = vmul_data_reg.read();
        v_fp16_t op2;
        op2 = v_fp16_t(); // Clear

        switch (state) {
            case EXE_A_State::VMAC_S1:
            case EXE_A_State::VMAC_S2:
            case EXE_A_State::VMAC_S3:
                op2[0] = vmul[1]; op2[1] = vmul[3]; op2[2] = s1_reg1.read(); op2[3] = s2_reg.read();
                break;
            case EXE_A_State::NORMAL_MODE:
            case EXE_A_State::EXEC_PLI_VADDU:
            case EXE_A_State::WAIT_PLI:
            case EXE_A_State::WAIT_PLO:
                op2 = pr_vp_out_sig.read();
                break;
            default:
                break;
        }
        vaddu_op2_sig.write(op2);
    }

    // ========================= Comb: PR Control =========================
    void comb_pr_static_signals() {
        if (stage_reset.read()) {
            pr_enable_sig.write(false);
            pr_pid_sig.write(0);
            pr_use_pcounter_sig.write(false);
            pr_clear_regs_sig.write(true);
            pr_clear_pcounter_sig.write(true);
            return;
        }

        EXE_A_State state = state_reg.read();
        pe_decode_signals_t decode;

        // Select appropriate decode register based on state
        if (state == EXE_A_State::VMAC_S1 || state == EXE_A_State::VMAC_S2 || state == EXE_A_State::VMAC_S3) {
             // For VMAC, PR operations (Read/Write) happen at Stage 3
             decode = decode_s2_reg.read();
        } else {
             // For Normal/Idle, use decode_reg
             decode = decode_reg.read();
        }

        pr_enable_sig.write(decode.pr_en);
        pr_pid_sig.write(decode.rid5);
        pr_use_pcounter_sig.write(decode.pr_use_vcounter);
        pr_clear_regs_sig.write(decode.pr_clear_regs);
        pr_clear_pcounter_sig.write(decode.sys_rst_pid);
    }

    void comb_pr_mode() {
        EXE_A_State state = state_reg.read();
        pe_decode_signals_t decode = decode_reg.read();
        int mode = decode.pr_mode;
        if (state == EXE_A_State::VMAC_S1 || state == EXE_A_State::VMAC_S2 || state == EXE_A_State::VMAC_S3) {
            mode = 0; // scalar
        } else if (state == EXE_A_State::NORMAL_MODE) {
            mode = 1; // vector
        }
        pr_mode_sig.write(mode);
    }

    void comb_pr_p_in() {
        EXE_A_State state = state_reg.read();
        fp16_t val = 0;
        if (state == EXE_A_State::VMAC_S1 || state == EXE_A_State::VMAC_S2 || state == EXE_A_State::VMAC_S3) {
            val = vaddu_result_sig.read()[3];
        }
        pr_p_in_sig.write(val);
    }

    void comb_pr_vp_in() {
        EXE_A_State state = state_reg.read();
        v_fp16_t val;
        val = v_fp16_t();
        if (state == EXE_A_State::NORMAL_MODE) {
             val = vaddu_result_sig.read();
        }
        pr_vp_in_sig.write(val);
    }

    void comb_pr_write_en() {
        if (stage_reset.read()) {
            pr_vpid_write_en_sig.write(false);
            return;
        }

        EXE_A_State state = state_reg.read();
        bool en = false;

        if (state == EXE_A_State::NORMAL_MODE) {
            if (decode_reg.read().pr_write) en = true;
        } else if (state == EXE_A_State::VMAC_S1 || state == EXE_A_State::VMAC_S2 || state == EXE_A_State::VMAC_S3) {
            if (decode_s2_reg.read().pr_write) en = true;
        }
        pr_vpid_write_en_sig.write(en);
    }

    void comb_pr_incr() {
        if (stage_reset.read()) {
            pr_incr_pcounter_sig.write(false);
            return;
        }

        EXE_A_State state = state_reg.read();
        bool incr = false;

        if (state == EXE_A_State::NORMAL_MODE || state == EXE_A_State::EXEC_PLI_VADDU) {
            if (decode_reg.read().pr_incr_vcounter) incr = true;
        } else if (state == EXE_A_State::VMAC_S1 || state == EXE_A_State::VMAC_S2 || state == EXE_A_State::VMAC_S3) {
            if (decode_s2_reg.read().pr_incr_vcounter) incr = true;
        }
        pr_incr_pcounter_sig.write(incr);
    }

    // ========================= Comb: PLO Buffer =========================
    void comb_plo_buffer() {
        // Default hold
        plo_buf_valid_reg_next.write(plo_buf_valid_reg.read());
        plo_buf_data_reg_next.write(plo_buf_data_reg.read());

        if (stage_reset.read()) {
            plo_buf_valid_reg_next.write(false);
            plo_buf_data_reg_next.write(0);
            return;
        }

        const bool pop = plo_buf_valid_reg.read() && plo.ready_in.read();
        const bool can_push = !plo_buf_valid_reg.read() || plo.ready_in.read();
        const bool produce = (state_reg.read() == EXE_A_State::EXEC_PLI_VADDU || state_reg.read() == EXE_A_State::WAIT_PLO) && can_push;

        const uint64_t vpsum_data = (state_reg.read() == EXE_A_State::WAIT_PLO)
            ? vaddu_result_reg.read().toUint64()
            : vaddu_result_sig.read().toUint64();

        if (pop && !produce) {
            plo_buf_valid_reg_next.write(false);
        } else if (!pop && produce) {
            plo_buf_valid_reg_next.write(true);
            plo_buf_data_reg_next.write(vpsum_data);
        } else if (pop && produce) {
            plo_buf_valid_reg_next.write(true);
            plo_buf_data_reg_next.write(vpsum_data);
        }
    }

    // ========================= Comb: Output Signals =========================
    // Replaces comb_stall_adder, comb_halted, etc.

    void comb_outputs_misc() {
        // Halted output
        halted_out.write(halted_reg.read());

        // Port IO Stall (Debug/Profiling)
        EXE_A_State state = state_reg.read();
        stall_port_pli.write(state == EXE_A_State::WAIT_PLI);
        stall_port_plo.write(state == EXE_A_State::WAIT_PLO);

        // PLO Valid Output
        plo.valid_out.write(plo_buf_valid_reg.read());

        // PLO Data Output
        plo.data_out.write(plo_buf_data_reg.read());
    }

    // ========================= Comb: PLI Handshake =========================
    void comb_pli_handshake() {
        EXE_A_State state = state_reg.read();

        // PLI handshake logic
        // We are ready for PLI if:
        // 1. We are in WAIT_PLI state (explicitly waiting)
        // 2. OR we are in IDLE, and a new PLI operation is arriving (Optimistic capture)
        //    AND we are not backpressuring the input (ready_out_sig is true)

        // Only trigger early capture if we are actually accepting the instruction
        bool accepting_new = ready_out_sig.read() && valid_in.read();
        bool next_is_vpsum = EXE_M_decode_signals_in.read().pli_plo_operation;

        bool accept_vpsum = accepting_new && next_is_vpsum;

        bool pli_ready = false;
        if (state == EXE_A_State::WAIT_PLI) {
            pli_ready = true;
        } else if (state == EXE_A_State::IDLE ||
                   state == EXE_A_State::EXEC_PLI_VADDU ||
                   state == EXE_A_State::WAIT_PLO) {
            pli_ready = accept_vpsum;
        } else {
            pli_ready = false;
        }

        pli.ready_out.write(pli_ready);
    }
};

} // namespace pe
} // namespace hybridacc