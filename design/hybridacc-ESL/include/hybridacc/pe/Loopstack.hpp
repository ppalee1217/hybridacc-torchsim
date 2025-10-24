#ifndef HYBRIDACC_PE_LOOPSTACK_HPP
#define HYBRIDACC_PE_LOOPSTACK_HPP

// ============================================================================
// File        : Loopstack.hpp
// Description : Parameterized loop stack for managing nested hardware loops.
//               Supports push (initialize new loop), jump/iterate (decrement
//               remaining count and repeat PC while count > 0), and pop.
//               All state updates occur on clk rising edge. Outputs reflect
//               state after the active clock edge (1-cycle latency).
//
// Ports:
//   clk           : Clock
//   rst_n         : Active-low synchronous reset (clears stack)
//   pc_jump_i     : PC captured when pushing a new loop frame
//   loop_count_i  : Iteration count to load when pushing (N iterations total)
//   push_i        : Assert to push new frame (captures pc_jump_i & loop_count_i)
//   pop_i         : Assert to unconditionally pop top frame
//   jump_i        : Assert to perform loop iteration on top frame:
//                     * If remaining_count > 1 -> remaining_count--
//                     * If remaining_count == 1 -> frame popped (loop done)
//   pc_jump_o     : Top frame PC (0 if empty)
//   empty_o       : High when stack empty
//
// Behavioral notes:
//   * push, pop, jump can be asserted simultaneously. Priority inside one
//     cycle is: push first, then pop, then jump acts on the (potentially new)
//     top. Adjust if different semantics are desired.
//   * Current implementation uses std::vector (non-synthesizable); suitable
//     for ESL / simulation. Replace with fixed-size arrays for synthesis.
//
// Change Log  : 2023-10-05  Initial implementation
//               2025-10-06  Fixed type typo, jump logic, output update method,
//                           documentation clarified, removed unsafe access.
// ============================================================================
#include <systemc.h>
#include <vector>
#include <hybridacc/utils.hpp>

namespace hybridacc { namespace pe {

template<unsigned LOOP_COUNT_BITS = 10>
SC_MODULE(LoopStack) {
public:
    // -------- Ports --------
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    sc_in< sc_uint<ADDR_BITS> >      pc_jump_i{"pc_jump_i"};
    sc_in< sc_uint<LOOP_COUNT_BITS> > loop_count_i{"loop_count_i"};

    sc_in<bool> push_i{"push_i"};
    sc_in<bool> pop_i{"pop_i"};
    sc_in<bool> jump_i{"jump_i"};

    sc_out< sc_uint<ADDR_BITS> > pc_jump_o{"pc_jump_o"};
    sc_out<bool>                 empty_o{"empty_o"};
    sc_out< sc_uint<LOOP_COUNT_BITS> > top_count_o{"top_count_o"}; // 新增: 輸出頂層剩餘次數 (0 若 empty)

    // -------- Constructor --------
    SC_CTOR(LoopStack) {
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();
    }

    // -------- Non-synthesizable helper API --------
    void push(uint16_t count, uint16_t pc) {
        pc_stack_.push_back(pc);
        count_stack_.push_back(count);
    }

    void pop() {
        if (!pc_stack_.empty()) {
            pc_stack_.pop_back();
            count_stack_.pop_back();
        }
    }

    bool isEmpty() const { return pc_stack_.empty(); }

private:
    std::vector<uint16_t> pc_stack_;
    std::vector<uint16_t> count_stack_; // Remaining iteration count for each frame

    // Sequential process: updates stack & drives outputs
    void seq_proc() {
        // Update outputs (registered)
        empty_o.write(pc_stack_.empty());
        pc_jump_o.write(pc_stack_.empty() ? 0 : pc_stack_.back());
        top_count_o.write(pc_stack_.empty() ? 0 : count_stack_.back());

        if (!rst_n.read()) {
            pc_stack_.clear();
            count_stack_.clear();
        } else {
            // 1) Push
            if (push_i.read()) {
                count_stack_.push_back(loop_count_i.read().to_uint());
                pc_stack_.push_back(pc_jump_i.read().to_uint());
            }
            // 2) Pop (after push so a same-cycle pop cancels the new frame if both asserted)
            if (pop_i.read() && !pc_stack_.empty()) {
                pc_stack_.pop_back();
                count_stack_.pop_back();
            }
            // 3) Jump / iterate
            if (jump_i.read() && !pc_stack_.empty()) {
                auto &cnt = count_stack_.back();
                if (cnt > 1) {
                    cnt -= 1; // Decrement remaining iterations
                } else {      // cnt == 1 -> loop completes, pop frame
                    pc_stack_.pop_back();
                    count_stack_.pop_back();
                }
            }
        }

    }
};

}} // namespace hybridacc::pe

#endif // HYBRIDACC_PE_LOOPSTACK_HPP