#ifndef HYBRIDACC_NOC_YBUS_HPP
#define HYBRIDACC_NOC_YBUS_HPP
// ============================================================================
//  File        : ybus.hpp
//  Module      : YBus (Vertical Link Abstraction)
//  Description : 提供 Y 向 (垂直) 簡化資料傳輸通道骨架，類似 XBus。
// ============================================================================
#include <systemc.h>
#include <queue>
#include <cstdint>

namespace hybridacc {
struct YBusFlit { uint32_t data{0}; bool last{true}; };

class YBus : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    SC_HAS_PROCESS(YBus);
    YBus(sc_core::sc_module_name name, unsigned depth = 4)
        : sc_module(name), m_depth(depth) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    bool send(const YBusFlit& f) { if (m_q.size() >= m_depth) return false; m_q.push(f); return true; }
    bool recv(YBusFlit& f) { if (m_out.empty()) return false; f = m_out.front(); m_out.pop(); return true; }
    unsigned pending() const { return (unsigned)(m_q.size() + m_pipe.size() + m_out.size()); }

private:
    unsigned m_depth;
    std::queue<YBusFlit> m_q;
    std::queue<YBusFlit> m_pipe;
    std::queue<YBusFlit> m_out;

    void step() {
        if (!rst_n.read()) {
            while(!m_q.empty()) m_q.pop();
            while(!m_pipe.empty()) m_pipe.pop();
            while(!m_out.empty()) m_out.pop();
            return;
        }
        if (!m_pipe.empty()) {
            while(!m_pipe.empty()) { m_out.push(m_pipe.front()); m_pipe.pop(); }
        }
        if (!m_q.empty()) {
            while(!m_q.empty()) { m_pipe.push(m_q.front()); m_q.pop(); }
        }
    }
};

} // namespace hybridacc
#endif // HYBRIDACC_NOC_YBUS_HPP
