#pragma once

#include "utils.hpp"
#include <systemc>

namespace hybridacc {
namespace pe {
// -----------------------------------------------------------------------------
enum class VADDU_State {
    IDLE,
    ACC0,
    ACC1,
};

inline std::ostream& operator<<(std::ostream& os, VADDU_State state) {
    switch(state) {
        case VADDU_State::IDLE: return os << "IDLE";
        case VADDU_State::ACC0: return os << "ACC0";
        case VADDU_State::ACC1: return os << "ACC1";
        default: return os << "UNKNOWN";
    }
}

inline void sc_trace(sc_core::sc_trace_file* tf, const VADDU_State& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}

// -----------------------------------------------------------------------------
enum class VADDU_Mode {
    ACCUMULATE,
    ADD
};

inline std::ostream& operator<<(std::ostream& os, VADDU_Mode mode) {
    switch(mode) {
        case VADDU_Mode::ACCUMULATE: return os << "ACCUMULATE";
        case VADDU_Mode::ADD: return os << "ADD";
        default: return os << "UNKNOWN";
    }
}

inline void sc_trace(sc_core::sc_trace_file* tf, const VADDU_Mode& mode, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(mode), name);
}

// -----------------------------------------------------------------------------
// Vector Adder Unit
SC_MODULE(VADDU) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    sc_in<v_fp16_t> op1;
    sc_in<v_fp16_t> op2;
    sc_out<v_fp16_t> result;

    sc_in<fp16_t> acc_in;
    sc_out<fp16_t> acc_out;

    sc_in<bool> start;
    sc_in<VADDU_Mode> mode;
    sc_out<bool> done;

    // Constructor
    SC_CTOR(VADDU)
        : clk("clk"),
          reset_n("reset_n"),
          op1("op1"),
          op2("op2"),
          result("result"),
          acc_in("acc_in"),
          acc_out("acc_out"),
          start("start"),
          mode("mode"),
          done("done")
    {
        DEBUG_MSG("[Create] VADDU");

        // Sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational process
        SC_METHOD(comb_next_state);
        sensitive << state_reg << start << mode << op1 << op2 << acc_in
                  << acc_stage_0_0_reg << acc_stage_0_1_reg << acc_stage_1_reg;
    }

private:
    // ========================= Pipeline Registers =========================
    sc_signal<VADDU_State> state_reg;
    sc_signal<v_fp16_t> result_reg;
    sc_signal<fp16_t> acc_out_reg;
    sc_signal<bool> done_reg;
    sc_signal<fp16_t> acc_stage_0_0_reg;
    sc_signal<fp16_t> acc_stage_0_1_reg;
    sc_signal<fp16_t> acc_stage_1_reg;

    // ========================= Next-State Signals =========================
    sc_signal<VADDU_State> state_reg_next;
    sc_signal<v_fp16_t> result_reg_next;
    sc_signal<fp16_t> acc_out_reg_next;
    sc_signal<bool> done_reg_next;
    sc_signal<fp16_t> acc_stage_0_0_reg_next;
    sc_signal<fp16_t> acc_stage_0_1_reg_next;
    sc_signal<fp16_t> acc_stage_1_reg_next;

    // ========================= Sequential Process =========================
    void seq_process() {
        // Reset
        state_reg.write(VADDU_State::IDLE);
        result_reg.write(v_fp16_t());
        acc_out_reg.write(0);
        done_reg.write(false);
        acc_stage_0_0_reg.write(0);
        acc_stage_0_1_reg.write(0);
        acc_stage_1_reg.write(0);

        result.write(v_fp16_t());
        acc_out.write(0);
        done.write(false);
        wait();

        while (true) {
            // Update all registers from their _next signals
            state_reg.write(state_reg_next.read());
            result_reg.write(result_reg_next.read());
            acc_out_reg.write(acc_out_reg_next.read());
            done_reg.write(done_reg_next.read());
            acc_stage_0_0_reg.write(acc_stage_0_0_reg_next.read());
            acc_stage_0_1_reg.write(acc_stage_0_1_reg_next.read());
            acc_stage_1_reg.write(acc_stage_1_reg_next.read());

            // Update outputs
            result.write(result_reg_next.read());
            acc_out.write(acc_out_reg_next.read());
            done.write(done_reg_next.read());

            wait();
        }
    }

    // ========================= Combinational Process =========================
    void comb_next_state() {
        VADDU_State state = state_reg.read();
        v_fp16_t res = result_reg.read();
        fp16_t acc_result = acc_out_reg.read();
        bool done_signal = done_reg.read();
        fp16_t acc_stage_0_0 = acc_stage_0_0_reg.read();
        fp16_t acc_stage_0_1 = acc_stage_0_1_reg.read();
        fp16_t acc_stage_1 = acc_stage_1_reg.read();

        // Default: keep current values
        VADDU_State next_state = state;
        v_fp16_t next_result = res;
        fp16_t next_acc_out = acc_result;
        bool next_done = done_signal;
        fp16_t next_acc_stage_0_0 = acc_stage_0_0;
        fp16_t next_acc_stage_0_1 = acc_stage_0_1;
        fp16_t next_acc_stage_1 = acc_stage_1;

        // State machine logic
        if (start.read() && state == VADDU_State::IDLE) {
            // 新的運算開始,清除 done 信號
            next_done = false;

            if (mode.read() == VADDU_Mode::ADD) {
                // Parallel add - 單週期完成
                for (size_t i = 0; i < 4; ++i) {
                    next_result[i] = fp16_add(op1.read()[i], op2.read()[i]);
                }
                next_done = true;
                next_state = VADDU_State::IDLE;
                DEBUG_MSG("[VADDU] ADD: op1=" << op1.read()
                         << " op2=" << op2.read()
                         << " result=" << next_result );
            } else if (mode.read() == VADDU_Mode::ACCUMULATE) {
                // 開始累加的第一階段
                next_acc_stage_0_0 = fp16_add(op1.read().lanes[0], op1.read().lanes[1]);
                next_acc_stage_0_1 = fp16_add(op1.read().lanes[2], op1.read().lanes[3]);
                next_state = VADDU_State::ACC0;
                DEBUG_MSG("[VADDU] ACCUMULATE Start: op1 = " << op1.read() );
            }
        } else if (state != VADDU_State::IDLE) {
            // 繼續執行進行中的 ACCUMULATE pipeline
            switch (state) {
                case VADDU_State::ACC0:
                    next_acc_stage_1 = fp16_add(acc_stage_0_0, acc_stage_0_1);
                    next_state = VADDU_State::ACC1;
                    DEBUG_MSG("[VADDU] ACCUMULATE ACC0 -> ACC1" );
                    break;

                case VADDU_State::ACC1:
                    // 完成累加
                    next_acc_out = fp16_add(acc_stage_1, acc_in.read());
                    next_state = VADDU_State::IDLE;
                    next_done = true;
                    DEBUG_MSG("[VADDU] ACCUMULATE Done: acc_stage_1=" << std::hex << acc_stage_1
                             << " acc_in=" << acc_in.read()
                             << " acc_out=" << next_acc_out << std::dec );
                    break;

                default:
                    break;
            }
        } else if (state == VADDU_State::IDLE && !start.read()) {
            // IDLE 且沒有 start 脈衝時,清除 done
            next_done = false;
        }

        // Debug output when operation completes
        if (next_done && !done_signal) {
            DEBUG_MSG("[VADDU] Operation done. Mode: " << mode.read()
                     << ", Op1: " << op1.read()
                     << ", Op2: " << op2.read()
                     << ", Result: " << next_result
                     << ", Acc_in: " << acc_in.read()
                     << ", Acc_out: " << next_acc_out );
        }

        // Write to _next signals
        state_reg_next.write(next_state);
        result_reg_next.write(next_result);
        acc_out_reg_next.write(next_acc_out);
        done_reg_next.write(next_done);
        acc_stage_0_0_reg_next.write(next_acc_stage_0_0);
        acc_stage_0_1_reg_next.write(next_acc_stage_0_1);
        acc_stage_1_reg_next.write(next_acc_stage_1);
    }
};

} // namespace pe
} // namespace hybridacc
