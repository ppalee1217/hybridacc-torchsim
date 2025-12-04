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
    EXEC_VADDU,        // 執行 VADDU 運算 (VMAC/VMUL)
    WAIT_PLI,          // 等待 PLI 數據
    EXEC_PLI_VADDU,    // PLI 數據就緒,執行 VADDU
    WAIT_PLO,          // 等待 PLO 握手
};

inline std::ostream& operator<<(std::ostream& os, EXE_A_State state) {
    switch (state) {
        case EXE_A_State::IDLE: return os << "IDLE";
        case EXE_A_State::EXEC_VADDU: return os << "EXEC_VADDU";
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
    v_fp16_t next_vmul_data;
    v_fp16_t next_pli_data;
    v_fp16_t next_vaddu_result;
    bool next_halted;
    bool next_vaddu_started;
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
    VADDU vadd;
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
          vadd("vadd"), PR("PR")
    {
        DEBUG_PE_MSG("[Create] EXE_A_Stage");

        // Sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational processes
        SC_METHOD(comb_next_state);
        sensitive << state_reg << decode_reg << vmul_data_reg << pli_data_reg << vaddu_started_reg
                  << signal_valid_in << EXE_M_decode_signals_in << vmul_out_in
                  << stall_from_downstream << stage_reset << pe_running << halted_reg
                  << pli.valid_in << pli.data_in << plo.ready_in << vadd_done_sig << vadd_result_sig << vadd_start_sig;

        SC_METHOD(comb_vaddu_control);
        sensitive << state_reg << decode_reg << vmul_data_reg << pli_data_reg
                  << vaddu_started_reg << pr_vp_out_sig;

        SC_METHOD(comb_pr_control);
        sensitive << state_reg  << decode_reg << vadd_done_sig;

        SC_METHOD(comb_outputs);
        sensitive << state_reg << halted_reg << vaddu_result_reg;

        SC_METHOD(comb_pli_handshake);
        sensitive << state_reg << signal_valid_in << EXE_M_decode_signals_in
                  << stall_from_downstream << pli.valid_in;

        bind_submodules();
    }

private:
    // ========================= Pipeline Registers =========================
    sc_signal<EXE_A_State> state_reg;
    sc_signal<pe_decode_signals_t> decode_reg;
    sc_signal<v_fp16_t> vmul_data_reg;
    sc_signal<v_fp16_t> pli_data_reg;
    sc_signal<v_fp16_t> vaddu_result_reg;
    sc_signal<bool> halted_reg;
    sc_signal<bool> vaddu_started_reg;  // 記錄 VADDU 是否已經觸發 start

    // ========================= Next-State Signals =========================
    sc_signal<EXE_A_State> state_reg_next;
    sc_signal<pe_decode_signals_t> decode_reg_next;
    sc_signal<v_fp16_t> vmul_data_reg_next;
    sc_signal<v_fp16_t> pli_data_reg_next;
    sc_signal<v_fp16_t> vaddu_result_reg_next;
    sc_signal<bool> halted_reg_next;
    sc_signal<bool> vaddu_started_reg_next;  // vaddu_started 的 next state

    // ========================= VADDU Interface Signals =========================
    sc_signal<v_fp16_t> vadd_op1_sig;
    sc_signal<v_fp16_t> vadd_op2_sig;
    sc_signal<v_fp16_t> vadd_result_sig;
    sc_signal<fp16_t> vadd_acc_out_sig;
    sc_signal<bool> vadd_start_sig;
    sc_signal<VADDU_Mode> vadd_mode_sig;
    sc_signal<bool> vadd_done_sig;

    // ========================= PsumRegFile Interface Signals =========================
    sc_signal<bool> pr_enable_sig;
    sc_signal<int> pr_pid_sig;
    sc_signal<bool> pr_vpid_write_en_sig;
    sc_signal<int> pr_mode_sig;
    sc_signal<fp16_t> pr_p_out_sig;
    sc_signal<v_fp16_t> pr_vp_out_sig;
    sc_signal<bool> pr_clear_regs_sig;
    sc_signal<bool> pr_use_pcounter_sig;
    sc_signal<bool> pr_set_pcounter_sig;
    sc_signal<bool> pr_clear_pcounter_sig;
    sc_signal<bool> pr_incr_pcounter_sig;

    // ========================= Submodule Binding =========================
    void bind_submodules() {
        // VADDU
        vadd.clk(clk);
        vadd.reset_n(reset_n);
        vadd.op1(vadd_op1_sig);
        vadd.op2(vadd_op2_sig);
        vadd.result(vadd_result_sig);
        vadd.acc_in(pr_p_out_sig); // vadd_acc_in_sig
        vadd.acc_out(vadd_acc_out_sig);
        vadd.start(vadd_start_sig);
        vadd.mode(vadd_mode_sig);
        vadd.done(vadd_done_sig);

        // PsumRegFile
        PR.clk(clk);
        PR.reset_n(reset_n);
        PR.enable(pr_enable_sig);
        PR.pid(pr_pid_sig);
        PR.p_in(vadd_acc_out_sig); // pr_p_in_sig
        PR.vp_in(vadd_result_sig); // pr_vp_in_sig
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
        vmul_data_reg.write(v_fp16_t());
        pli_data_reg.write(v_fp16_t());
        vaddu_result_reg.write(v_fp16_t());
        halted_reg.write(false);
        vaddu_started_reg.write(false);
        wait();

        while (true) {
            // Update all registers from their _next signals
            state_reg.write(state_reg_next.read());
            decode_reg.write(decode_reg_next.read());
            vmul_data_reg.write(vmul_data_reg_next.read());
            pli_data_reg.write(pli_data_reg_next.read());
            vaddu_result_reg.write(vaddu_result_reg_next.read());
            halted_reg.write(halted_reg_next.read());
            vaddu_started_reg.write(vaddu_started_reg_next.read());

            DEBUG_PE_MSG("[EXE_A_Stage] Clocked: State=" << state_reg.read()
                << " -> " << state_reg_next.read()
                << " VadduStarted=" << vaddu_started_reg.read()
                << " Inst=0x" << std::hex << decode_reg.read().inst << std::dec
                );

            wait();
        }
    }

    // ========================= Comb: Next State Logic (拆分版本) =========================

    // 處理 IDLE 狀態的轉換
    void handle_idle_state(StateTransitionResult& result) {
        if (signal_valid_in.read() && !stall_from_downstream.read()) {
            result.next_decode = EXE_M_decode_signals_in.read();
            result.next_vmul_data = vmul_out_in.read();
            result.next_vaddu_started = false;  // 立即清除

            DEBUG_PE_MSG("[EXE_A_stage] New instruction, vmul_data = " << vmul_out_in.read() );

            // 檢查 HALT
            if (result.next_decode.halt) {
                result.next_halted = true;
                result.next_state = EXE_A_State::IDLE;
                DEBUG_PE_MSG("[EXE_A_stage] HALT detected");
            }
            // PLI-PLO 操作
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
            // 一般 VADDU 操作 (VMAC/VMUL)
            else if (result.next_decode.vaddu_en) {
                result.next_state = EXE_A_State::EXEC_VADDU;
                DEBUG_PE_MSG("[EXE_A_stage] Start VADDU operation, mode=" << result.next_decode.vaddu_mode);
            }
            // 其他指令 (只操作 PR,不需要 VADDU)
            else {
                result.next_state = EXE_A_State::IDLE;
                DEBUG_PE_MSG("[EXE_A_stage] Execute PR-only operation");
            }
        } else {
            // 在 IDLE 且沒有新指令時,也清除 vaddu_started
            result.next_vaddu_started = false;
        }
    }

    // 處理 EXEC_VADDU 狀態的轉換
    void handle_exec_vaddu_state(StateTransitionResult& result) {
        if (vadd_start_sig.read()) {
            DEBUG_PE_MSG("[EXE_A_stage] VADDU started" );
            result.next_vaddu_started = true;
        }

        if (vadd_done_sig.read() && !stall_from_downstream.read()) {
            result.next_state = EXE_A_State::IDLE;
            DEBUG_PE_MSG("[EXE_A_stage] VADDU complete, returning to IDLE");
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
        if (vadd_start_sig.read()) {
            DEBUG_PE_MSG("[EXE_A_stage] VADDU started" );
            result.next_vaddu_started = true;
        }

        if (vadd_done_sig.read()) {
            result.next_vaddu_result = vadd_result_sig.read();

            if (!stall_from_downstream.read()) {
                result.next_state = EXE_A_State::WAIT_PLO;
                DEBUG_PE_MSG("[EXE_A_stage] PLI VADDU complete");
            }
        }
    }

    // 處理 WAIT_PLO 狀態的轉換
    void handle_wait_plo_state(StateTransitionResult& result) {
        if (plo.ready_in.read()) {
            result.next_state = EXE_A_State::IDLE;
            DEBUG_PE_MSG("[EXE_A_stage] PLO handshake done");
        }
    }

    // 主要的 next state 邏輯 (重構後的版本)
    void comb_next_state() {
        // 讀取當前狀態
        StateTransitionResult result;
        result.next_state = state_reg.read();
        result.next_decode = decode_reg.read();
        result.next_vmul_data = vmul_data_reg.read();
        result.next_pli_data = pli_data_reg.read();
        result.next_vaddu_result = vaddu_result_reg.read();
        result.next_halted = halted_reg.read();
        result.next_vaddu_started = vaddu_started_reg.read();

        // ===== Reset & Halt Handling =====
        if (stage_reset.read()) {
            result.next_state = EXE_A_State::IDLE;
            result.next_decode = pe_decode_signals_t();
            result.next_halted = false;
            result.next_vaddu_started = false;
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

                case EXE_A_State::EXEC_VADDU:
                    handle_exec_vaddu_state(result);
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
        vmul_data_reg_next.write(result.next_vmul_data);
        pli_data_reg_next.write(result.next_pli_data);
        vaddu_result_reg_next.write(result.next_vaddu_result);
        halted_reg_next.write(result.next_halted);
        vaddu_started_reg_next.write(result.next_vaddu_started);
    }

    // ========================= Comb: VADDU Control =========================
    void comb_vaddu_control() {
        EXE_A_State state = state_reg.read();
        pe_decode_signals_t decode = decode_reg.read();
        v_fp16_t vmul_data = vmul_data_reg.read();
        v_fp16_t pli_data = pli_data_reg.read();
        bool vaddu_started = vaddu_started_reg.read();

        // Default: disable VADDU
        vadd_start_sig.write(false);
        vadd_op1_sig.write(v_fp16_t());
        vadd_op2_sig.write(v_fp16_t());
        vadd_mode_sig.write(VADDU_Mode::ADD);

        // VADDU control based on state
        switch (state) {
            case EXE_A_State::EXEC_VADDU:
                vadd_op1_sig.write(vmul_data);
                vadd_op2_sig.write(pr_vp_out_sig.read());
                vadd_mode_sig.write(static_cast<VADDU_Mode>(decode.vaddu_mode));

                if (decode.vaddu_en && !vaddu_started) {
                    vadd_start_sig.write(true);
                    DEBUG_PE_MSG("[EXE_A_stage] Generate VADDU start pulse" );
                }
                break;

            case EXE_A_State::EXEC_PLI_VADDU:
                vadd_op1_sig.write(pli_data);
                vadd_op2_sig.write(pr_vp_out_sig.read());
                vadd_mode_sig.write(VADDU_Mode::ADD);

                if (!vaddu_started) {
                    vadd_start_sig.write(true);
                }
                break;

            default:
                // IDLE, WAIT_PLI, WAIT_PLO: no VADDU operation
                break;
        }
    }

    // ========================= Comb: PR Control =========================
    void comb_pr_control() {
        EXE_A_State state = state_reg.read();
        EXE_A_State next_state = state_reg_next.read();
        pe_decode_signals_t decode = decode_reg.read();

        // PR 讀取在所有狀態都可以進行
        pr_enable_sig.write(decode.pr_en);
        pr_pid_sig.write(decode.rid5);
        pr_mode_sig.write(decode.pr_mode);
        pr_use_pcounter_sig.write(decode.pr_use_vcounter);


        if (vadd_done_sig.read() && decode.pr_write) {
            pr_vpid_write_en_sig.write(decode.pr_write);
        } else {
            pr_vpid_write_en_sig.write(false);
        }

        // PR 計數器增加:只在指令完成時觸發一次
        if (vadd_done_sig.read() && decode.pr_incr_vcounter) {
            pr_incr_pcounter_sig.write(true);
        } else {
            pr_incr_pcounter_sig.write(false);
        }

        // 這些控制信號在所有狀態都可以執行
        pr_clear_regs_sig.write(decode.pr_clear_regs);
        pr_set_pcounter_sig.write(decode.pr_set_vcounter);
        pr_clear_pcounter_sig.write(decode.pr_clear_vcounter);
    }

    // ========================= Comb: Output Signals =========================
    void comb_outputs() {
        EXE_A_State state = state_reg.read();
        bool halted = halted_reg.read();

        // Stall signals
        bool stall_adder_val = (state == EXE_A_State::EXEC_VADDU) ||
                               (state == EXE_A_State::EXEC_PLI_VADDU);
        bool stall_port_io_val = (state == EXE_A_State::WAIT_PLI) ||
                                  (state == EXE_A_State::WAIT_PLO);

        stall_adder.write(stall_adder_val);
        stall_port_io.write(stall_port_io_val);

        // PLO handshake
        bool plo_valid = (state == EXE_A_State::WAIT_PLO);
        uint64_t plo_data = vaddu_result_reg.read().toUint64();
        plo.valid_out.write(plo_valid);
        plo.data_out.write(plo_data);

        // Halted status
        halted_out.write(halted);
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