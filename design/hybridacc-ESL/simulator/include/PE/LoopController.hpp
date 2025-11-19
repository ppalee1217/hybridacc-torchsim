#pragma once

#include "utils.hpp"
#include <systemc>

namespace hybridacc {
namespace pe {

struct LoopFrame { uint16_t start_pc; uint16_t remaining; };

// Loop Controller
SC_MODULE(LoopController) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    sc_in<uint16_t> pc_in;
    sc_in<uint16_t> count_in;
    sc_in<bool> loop_in_en;
    sc_in<bool> loop_break_en;
    sc_in<bool> loop_end_en;

    sc_out<uint16_t> pc_out;
    sc_out<bool> jump;


    // methods
    SC_CTOR(LoopController)
        : clk("clk"),
          reset_n("reset_n"),
          pc_in("pc_in"),
          count_in("count_in"),
          loop_in_en("loop_in_en"),
          loop_break_en("loop_break_en"),
          loop_end_en("loop_end_en"),
          pc_out("pc_out")
    {
        DEBUG_MSG("[Create] LoopController");
        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);
        SC_METHOD(combinational_process);
        sensitive << loop_end_en;
    }

    std::vector<LoopFrame> loopstack; // 使用 vector 來儲存 loop frames

    void reset() { loopstack.clear(); }
    void loopIn(uint16_t start_pc, uint16_t count){
        if(count <= 1) return; // trivial loop 不建立
        LoopFrame fr{start_pc, count};
        loopstack.push_back(fr);
    }
    void loopBreak(){
        if(!loopstack.empty()) loopstack.pop_back();
    }
    bool empty() const { return loopstack.empty(); }

    // 組合邏輯:直接函數調用,立即返回結果
    bool shouldJump() const {
        if (loopstack.empty()) return false;
        const auto &top = loopstack.back();
        return (top.remaining > 1);  // 還有剩餘次數(減1後還>0)
    }

    uint16_t getJumpPC() const {
        if (loopstack.empty()) return 0;
        return loopstack.back().start_pc;
    }

    void combinational_process() {
        jump.write(shouldJump());
    }

    void sequential_process() {
        // Reset initialization
        reset();
        wait();

        while (true) {
            // 時序邏輯:在時鐘邊沿更新 loop stack
            if (loop_in_en.read()) {
                loopIn(pc_in.read(), count_in.read());
            }
            if (loop_break_en.read()) {
                loopBreak();
            }
            if (loop_end_en.read()) {
                if(!loopstack.empty()) {
                    auto &top = loopstack.back();
                    top.remaining--;
                    if (top.remaining == 0) {
                        loopstack.pop_back();  // 迴圈結束,移除 frame
                    }
                }
            }

            // 更新輸出 pc_out
            pc_out.write(getJumpPC());

            wait();
        }
    }
};

}; // namespace pe
}; // namespace hybridacc