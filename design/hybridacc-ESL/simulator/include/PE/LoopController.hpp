#pragma once

#include "Utils/utils.hpp"
#include <systemc>

using namespace sc_core;

namespace hybridacc {
namespace pe {

struct LoopFrame { uint16_t start_pc; uint16_t remaining; };

// Loop Controller
SC_MODULE(LoopController) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;
    sc_in<bool> stage_reset;

    sc_in<uint16_t> pc_in;
    sc_in<uint16_t> count_in;
    sc_in<bool> loop_in_en;
    sc_in<bool> loop_end_en;

    sc_out<uint16_t> pc_out;
    sc_out<bool> jump;

    // methods
    SC_CTOR(LoopController)
        : clk("clk"),
          reset_n("reset_n"),
                    stage_reset("stage_reset"),
          pc_in("pc_in"),
          count_in("count_in"),
          loop_in_en("loop_in_en"),
          loop_end_en("loop_end_en"),
          pc_out("pc_out"),
          jump("jump")
    {
        DEBUG_MSG("[Create] LoopController", DEBUG_LEVEL_PE_COMPONENTS);
        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);
        SC_METHOD(combinational_process);
        sensitive << stage_reset << loop_end_en << loopstack_size_sig << remaining_sig;
    }

    std::vector<LoopFrame> loopstack; // 使用 vector 來儲存 loop frames

    // 內部信號
    sc_signal<uint16_t> pc_start_reg;      // 當前頂層 loop 的起始 PC
    sc_signal<uint16_t> loopstack_size_sig; // 觀察用
    sc_signal<uint16_t> remaining_sig;      // 當前 top frame 的 remaining (供組合邏輯用)

    void reset() { loopstack.clear(); }
    void loopIn(uint16_t start_pc, uint16_t count){
        if(count <= 0) return; // trivial loop 不建立
        LoopFrame fr{start_pc, (uint16_t)(count+1)}; // N-1 encoding
        loopstack.push_back(fr);
    }
    bool empty() const { return loopstack.empty(); }

    // 組合邏輯: 決定是否跳轉
    // 必須是 Combinational，以便 IF_ID 在當前 cycle 決定 next_pc
    void combinational_process() {
        if (stage_reset.read()) {
            pc_out.write(0);
            jump.write(false);
            return;
        }

        // pc_out 維持頂層 loop 起點
        if (loopstack_size_sig.read() == 0) {
            pc_out.write(0);
        } else {
            pc_out.write(pc_start_reg.read());
        }

        // Jump 條件:
        // 1. 當前是 LOOPEND 指令 (loop_end_en == true)
        // 2. Loop stack 不為空
        // 3. 剩餘次數 > 1 (因為本次執行後會減 1，如果剩 1 則本次是最後一次，不跳轉)
        if (loop_end_en.read() && loopstack_size_sig.read() > 0) {
            if (remaining_sig.read() > 1) {
                jump.write(true);
            } else {
                jump.write(false);
            }
            DEBUG_MSG("[LoopController] COMB: LOOPEND detected. Remaining=" << remaining_sig.read()
                      << ", Jump=" << (jump.read() ? "Yes" : "No"), DEBUG_LEVEL_PE_COMPONENTS);
        } else {
            jump.write(false);
        }
    }

    void sequential_process() {
        // Reset
        reset();
        pc_start_reg.write(0);
        loopstack_size_sig.write(0);
        remaining_sig.write(0);
        wait();

        while (true) {
            if (stage_reset.read()) {
                reset();
                pc_start_reg.write(0);
                loopstack_size_sig.write(0);
                remaining_sig.write(0);
                wait();
                continue;
            }

            // 1. 更新 loop stack 控制 (Push/Break)
            if (loop_in_en.read()) {
                loopIn(pc_in.read(), count_in.read());
            }

            // 2. 處理 Loop End 的計數更新 (Sequential)
            // 注意: 這裡只負責更新狀態，跳轉決策由 combinational_process 處理
            if (loop_end_en.read()) {
                if(!loopstack.empty()) {
                    auto &top = loopstack.back();
                    if (top.remaining > 0) {
                        top.remaining--;
                    }
                    if (top.remaining == 0) {
                        loopstack.pop_back();
                    }
                }
            }

            // 3. 更新內部信號供組合邏輯使用
            if (!loopstack.empty()) {
                pc_start_reg.write(loopstack.back().start_pc);
                remaining_sig.write(loopstack.back().remaining);
            } else {
                pc_start_reg.write(0);
                remaining_sig.write(0);
            }
            loopstack_size_sig.write(static_cast<uint16_t>(loopstack.size()));

            wait();
        }
    }
};

}; // namespace pe
}; // namespace hybridacc