#pragma once

#include "utils.hpp"
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
    sc_in<bool> signal_valid_in;

    // Stall control
    sc_in<bool> stall_from_downstream;
    sc_out<bool> stall_adder;
    sc_out<bool> stall_port_io;

    // Status outputs
    sc_out<bool> halted_out;

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
          signal_valid_in("signal_valid_in"),
          stall_from_downstream("stall_from_downstream"),
          stall_adder("stall_adder"), stall_port_io("stall_port_io"),
          pli("pli"), plo("plo"),
          vaddu("vaddu"), PR("PR")
    {
        DEBUG_PE_MSG("[Create] EXE_A_Stage");

        // Sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational processes

        // Next State Logic
        SC_METHOD(comb_next_state);
        sensitive << state_reg << decode_reg << vmul_data_reg << pli_data_reg
                  << signal_valid_in << EXE_M_decode_signals_in << vmul_out_in
                  << stall_from_downstream << stage_reset << pe_running << halted_reg
                  << pli.valid_in << pli.data_in << plo.ready_in
                  << s1_reg0 << s1_reg1 << s2_reg << vaddu_result_reg
                  << vaddu_result_sig;

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
        SC_METHOD(comb_stall_adder);
        sensitive << state_reg << signal_valid_in << EXE_M_decode_signals_in;

        SC_METHOD(comb_stall_port_io);
        sensitive << state_reg;

        SC_METHOD(comb_plo_valid);
        sensitive << state_reg;

        SC_METHOD(comb_plo_data);
        sensitive << vaddu_result_reg;

        SC_METHOD(comb_halted);
        sensitive << halted_reg;

        SC_METHOD(comb_pli_handshake);
        sensitive << state_reg << signal_valid_in << EXE_M_decode_signals_in
                  << stall_from_downstream << pli.valid_in;

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

    // VMAC pipeline next-state signals
    sc_signal<fp16_t> s1_reg0_next;
    sc_signal<fp16_t> s1_reg1_next;
    sc_signal<fp16_t> s2_reg_next;

    // ========================= VADDU Interface Signals =========================
    sc_signal<v_fp16_t> vaddu_op1_sig;
    sc_signal<v_fp16_t> vaddu_op2_sig;
    sc_signal<v_fp16_t> vaddu_result_sig;

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
    sc_signal<bool> pr_set_pcounter_sig;
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
        PR.set_pcounter(pr_set_pcounter_sig);
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
            s1_reg0.write(s1_reg0_next.read());
            s1_reg1.write(s1_reg1_next.read());
            s2_reg.write(s2_reg_next.read());

            DEBUG_PE_MSG("[EXE_A_Stage] Clocked: State=" << state_reg.read()
                << " -> " << state_reg_next.read()
                << " Inst=0x" << std::hex << decode_reg.read().inst << std::dec
                );

            wait();
        }
    }

    // ========================= Comb: Next State Logic =========================

    // 處理 IDLE 狀態的轉換
    void handle_idle_state(StateTransitionResult& result) {
        if (signal_valid_in.read() && !stall_from_downstream.read()) {
            result.next_decode = EXE_M_decode_signals_in.read();
            result.next_vmul_data = vmul_out_in.read();

            // Clear pipeline registers
            result.next_decode_s1 = pe_decode_signals_t();
            result.next_decode_s2 = pe_decode_signals_t();

            DEBUG_PE_MSG("[EXE_A_stage] New instruction, vmul_data = " << vmul_out_in.read() );

            // 檢查 HALT
            if (result.next_decode.halt) {
                result.next_halted = true;
                result.next_state = EXE_A_State::IDLE;
                DEBUG_PE_MSG("[EXE_A_stage] HALT detected");
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
                DEBUG_PE_MSG("[EXE_A_stage] Start PLI-PLO operation");
            }
            // VMAC 模式 (vaddu_mode==0 表示 ACCUMULATE)
            else if (result.next_decode.vaddu_en && result.next_decode.vaddu_mode == 0) {
                result.next_state = EXE_A_State::VMAC_S1;
                DEBUG_PE_MSG("[EXE_A_stage] Start VMAC mode (Pipeline Active)");
            }
            // Normal 模式 (VMUL/VMULR 等, vaddu_mode==1 表示 ADD)
            else if (result.next_decode.vaddu_en) {
                result.next_state = EXE_A_State::NORMAL_MODE;
                DEBUG_PE_MSG("[EXE_A_stage] Start Normal mode (1-stage)");
            }
            // 其他指令 (只操作 PR,不需要運算)
            else {
                result.next_state = EXE_A_State::IDLE;
                DEBUG_PE_MSG("[EXE_A_stage] Execute PR-only operation");
            }
        }
    }

    // 處理 VMAC Stage 1 (Pipeline Active)
    void handle_vmac_s1_state(StateTransitionResult& result) {
        if (!stall_from_downstream.read()) {
            // Pipeline Shift Logic
            v_fp16_t vaddu_out = vaddu_result_sig.read();
            result.next_s1_reg0 = vaddu_out[0];
            result.next_s1_reg1 = vaddu_out[1];
            result.next_s2_reg = vaddu_out[2]; // Shift from S1 to S2

            // Shift decode signals
            result.next_decode_s1 = decode_reg.read();
            result.next_decode_s2 = decode_s1_reg.read();

            // Check for new instruction to fill Stage 1
            if (signal_valid_in.read()) {
                pe_decode_signals_t next_decode = EXE_M_decode_signals_in.read();
                // If next is VMAC, accept it and stay in VMAC_S1
                if (next_decode.vaddu_en && next_decode.vaddu_mode == 0) {
                    result.next_decode = next_decode;
                    result.next_vmul_data = vmul_out_in.read();
                    result.next_state = EXE_A_State::VMAC_S1;
                    DEBUG_PE_MSG("[EXE_A_stage] Pipeline: Accept new VMAC");
                } else {
                    // Next is NOT VMAC, start draining
                    result.next_decode = pe_decode_signals_t(); // Bubble
                    result.next_state = EXE_A_State::VMAC_S2;
                    DEBUG_PE_MSG("[EXE_A_stage] Pipeline: Next not VMAC, drain (Go to S2)");
                }
            } else {
                // No valid input, start draining
                result.next_decode = pe_decode_signals_t(); // Bubble
                result.next_state = EXE_A_State::VMAC_S2;
                DEBUG_PE_MSG("[EXE_A_stage] Pipeline: No input, drain (Go to S2)");
            }
        }
    }

    // 處理 VMAC Stage 2 (Draining Stage 1)
    void handle_vmac_s2_state(StateTransitionResult& result) {
        if (!stall_from_downstream.read()) {
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
            DEBUG_PE_MSG("[EXE_A_stage] VMAC S2: Draining...");
        }
    }

    // 處理 VMAC Stage 3 (Draining Stage 2)
    void handle_vmac_s3_state(StateTransitionResult& result) {
        if (!stall_from_downstream.read()) {
            // Pipeline Shift Logic
            // Stage 2 is bubble, so s2_reg doesn't matter
            // Stage 3 (Writeback) happens in this cycle (combinational)

            // Shift decode signals
            result.next_decode_s2 = decode_s1_reg.read(); // Should be bubble

            result.next_state = EXE_A_State::IDLE;
            DEBUG_PE_MSG("[EXE_A_stage] VMAC S3: Draining complete, go IDLE");
        }
    }

    // 處理 Normal Mode (1-stage)
    void handle_normal_mode_state(StateTransitionResult& result) {
        if (!stall_from_downstream.read()) {
            // Normal mode 只需要一個週期 (vector add 是 combinational)
            // 結果會在 PR control 中處理
            result.next_state = EXE_A_State::IDLE;
            DEBUG_PE_MSG("[EXE_A_stage] Normal mode complete");
        }
    }

    // 處理 WAIT_PLI 狀態的轉換
    void handle_wait_pli_state(StateTransitionResult& result) {
        if (pli.valid_in.read()) {
            v_fp16_t captured_pli_data;
            captured_pli_data.fromUint64(pli.data_in.read());
            result.next_pli_data = captured_pli_data;
            result.next_state = EXE_A_State::EXEC_PLI_VADDU;
            DEBUG_PE_MSG("[EXE_A_stage] PLI data captured: 0x" << std::hex << pli.data_in.read());
        }
    }

    // 處理 EXEC_PLI_VADDU 狀態的轉換
    void handle_exec_pli_vaddu_state(StateTransitionResult& result) {
        if (!stall_from_downstream.read()) {
            // PLI + PR (使用 VADDU)
            v_fp16_t vaddu_out = vaddu_result_sig.read();
            result.next_vaddu_result = vaddu_out;
            result.next_state = EXE_A_State::WAIT_PLO;
            DEBUG_PE_MSG("[EXE_A_stage] PLI VADDU complete: " << vaddu_out);
        }
    }

    // 處理 WAIT_PLO 狀態的轉換
    void handle_wait_plo_state(StateTransitionResult& result) {
        if (plo.ready_in.read()) {
            result.next_state = EXE_A_State::IDLE;
            DEBUG_PE_MSG("[EXE_A_stage] PLO handshake done");
        }
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
                op2 = pr_vp_out_sig.read();
                break;
            default:
                break;
        }
        vaddu_op2_sig.write(op2);
    }

    // ========================= Comb: PR Control =========================
    void comb_pr_static_signals() {
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
        pr_set_pcounter_sig.write(decode.pr_set_vcounter);
        pr_clear_pcounter_sig.write(decode.pr_clear_vcounter);
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
        EXE_A_State state = state_reg.read();
        bool incr = false;

        if (state == EXE_A_State::NORMAL_MODE) {
            if (decode_reg.read().pr_incr_vcounter) incr = true;
        } else if (state == EXE_A_State::VMAC_S1 || state == EXE_A_State::VMAC_S2 || state == EXE_A_State::VMAC_S3) {
            if (decode_s2_reg.read().pr_incr_vcounter) incr = true;
        }
        pr_incr_pcounter_sig.write(incr);
    }

    // ========================= Comb: Output Signals =========================
    void comb_stall_adder() {
        EXE_A_State state = state_reg.read();
        bool stall = true;

        if (state == EXE_A_State::IDLE) {
            stall = false;
        } else if (state == EXE_A_State::VMAC_S1) {
            // In VMAC_S1 (Pipeline Active), we check if we can accept the next instruction
            if (signal_valid_in.read()) {
                 pe_decode_signals_t next_decode = EXE_M_decode_signals_in.read();
                 // If next is VMAC, we don't stall (Pipeline it)
                 if (next_decode.vaddu_en && next_decode.vaddu_mode == 0) {
                     stall = false;
                 } else {
                     // Next is not VMAC, we must stall to drain pipeline
                     stall = true;
                 }
            } else {
                 // No valid input, we are ready
                 stall = false;
            }
        } else {
            // Other states (VMAC_S2, VMAC_S3, NORMAL, etc.) are busy
            stall = true;
        }
        stall_adder.write(stall);
    }

    void comb_stall_port_io() {
        EXE_A_State state = state_reg.read();
        bool stall = (state == EXE_A_State::WAIT_PLI) ||
                     (state == EXE_A_State::WAIT_PLO);
        stall_port_io.write(stall);
    }

    void comb_plo_valid() {
        EXE_A_State state = state_reg.read();
        plo.valid_out.write(state == EXE_A_State::WAIT_PLO);
    }

    void comb_plo_data() {
        plo.data_out.write(vaddu_result_reg.read().toUint64());
    }

    void comb_halted() {
        halted_out.write(halted_reg.read());
    }

    // ========================= Comb: PLI Handshake =========================
    void comb_pli_handshake() {
        EXE_A_State state = state_reg.read();
        bool stalled = stall_from_downstream.read();

        // PLI handshake
        bool need_pli_early = (state == EXE_A_State::IDLE && signal_valid_in.read() &&
                               EXE_M_decode_signals_in.read().pli_plo_operation &&
                               !stalled);
        bool pli_ready = (state == EXE_A_State::WAIT_PLI) || need_pli_early;
        pli.ready_out.write(pli_ready);
    }
};

} // namespace pe
} // namespace hybridacc