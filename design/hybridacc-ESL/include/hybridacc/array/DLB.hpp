#ifndef HYBRIDACC_ARRAY_DLB_HPP
#define HYBRIDACC_ARRAY_DLB_HPP
// ============================================================================
//  File        : DLB.hpp
//  Module      : Dynamic Load Balancer (DLB)
//  Description : 為 SubController 提供任務分派策略骨架。
// ============================================================================
#include <systemc.h>
#include <queue>
#include <cstdint>

namespace hybridacc {
struct TaskDesc { uint32_t id{0}; uint32_t cost{0}; };

class DLB : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    SC_HAS_PROCESS(DLB);
    DLB(sc_core::sc_module_name name) : sc_module(name) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void push(const TaskDesc& t) { m_q.push(t); }
    bool pop(TaskDesc& out) {
        if (m_q.empty()) return false;
        out = m_q.front();
        m_q.pop();
        return true;
    }
    size_t pending() const { return m_q.size(); }

private:
    std::queue<TaskDesc> m_q;
    void step() {
        if (!rst_n.read()) {
            std::queue<TaskDesc> empty; std::swap(empty, m_q);
        }
        // 更複雜策略未來擴充 (priority / aging etc.)
    }
};

} // namespace hybridacc
#endif // HYBRIDACC_ARRAY_DLB_HPP
