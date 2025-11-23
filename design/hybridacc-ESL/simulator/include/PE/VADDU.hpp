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
// Multiply Unit
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

        sc_in<bool> enable;
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
              enable("enable"),
              mode("mode"),
              done("done")
        {
            DEBUG_MSG("[Create] VADDU");
            SC_CTHREAD(sequential_process, clk.pos());
            reset_signal_is(reset_n, false);
        }


        sc_signal<VADDU_State> state;
        VADDU_State state_next;
        sc_signal<fp16_t> acc_stage_0_0;
        sc_signal<fp16_t> acc_stage_0_1;
        sc_signal<fp16_t> acc_stage_1;
        sc_signal<bool> done_reg;  // 新增: 保持 done 狀態的暫存器

        void sequential_process() {
            // Reset state
            state.write(VADDU_State::IDLE);
            result.write(v_fp16_t());
            acc_out.write(0);
            done.write(false);
            done_reg.write(false);
            wait();

            while (true) {
                v_fp16_t res = result.read();
                fp16_t acc_result = acc_out.read();
                state_next = state.read();
                bool done_signal = done_reg.read();

                if (enable.read()) {
                    // 新的運算開始,清除 done 信號
                    done_signal = false;

                    if(mode.read() == VADDU_Mode::ADD) {
                        // parallel add
                        for (size_t i = 0; i < 4; ++i) {
                            res[i] = fp16_add(op1.read()[i], op2.read()[i]);
                        }
                        done_signal = true;
                    } else if(mode.read() == VADDU_Mode::ACCUMULATE) {
                        switch(state.read()) {
                            case VADDU_State::IDLE:
                                acc_stage_0_0 = fp16_add(op1.read().lanes[0], op1.read().lanes[1]);
                                acc_stage_0_1 = fp16_add(op1.read().lanes[2], op1.read().lanes[3]);
                                state_next = VADDU_State::ACC0;
                                break;
                            case VADDU_State::ACC0:
                                acc_stage_1 = fp16_add(acc_stage_0_0, acc_stage_0_1);
                                state_next = VADDU_State::ACC1;
                                break;
                            case VADDU_State::ACC1:
                                // 完成累加
                                acc_result = fp16_add(acc_stage_1, acc_in.read());
                                state_next = VADDU_State::IDLE;
                                done_signal = true;
                                break;
                        }
                    }
                }
                // 如果沒有 enable,保持當前的 done 狀態

                state.write(state_next);
                acc_out.write(acc_result);
                result.write(res);
                done.write(done_signal);
                done_reg.write(done_signal);

                // debug output
                if (done_signal) {
                    DEBUG_MSG("[VADDU] Operation done. Mode: " << mode.read()
                              << ", Result: " << res
                              << ", Acc_out: " << acc_result);
                }
                wait();
            }
        }

};


} // namespace pe
} // namespace hybridacc
