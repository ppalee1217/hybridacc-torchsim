#ifndef HYBRIDACC_NOC_XBUS_HPP
#define HYBRIDACC_NOC_XBUS_HPP
// ============================================================================
//  File        : xbus.hpp
//  Module      : XBus (Horizontal Link Abstraction)
//  Description : 提供 X 向 PE / Router 之間簡化資料傳輸通道骨架。
// ============================================================================
#include <systemc.h>
#include <queue>
#include <cstdint>

namespace hybridacc {
struct XBusFlit { uint32_t data{0}; bool last{true}; };

class XBus : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    SC_HAS_PROCESS(XBus);
    XBus(sc_core::sc_module_name name, unsigned depth = 4)
        : sc_module(name), m_depth(depth) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    bool send(const XBusFlit& f) { if (m_q.size() >= m_depth) return false; m_q.push(f); return true; }
    bool recv(XBusFlit& f) { if (m_out.empty()) return false; f = m_out.front(); m_out.pop(); return true; }
    unsigned pending() const { return (unsigned)(m_q.size() + m_pipe.size() + m_out.size()); }

private:
    unsigned m_depth; // input queue depth
    std::queue<XBusFlit> m_q;     // input
    std::queue<XBusFlit> m_pipe;  // in-flight (one cycle latency)
    std::queue<XBusFlit> m_out;   // ready to consume

    void step() {
        if (!rst_n.read()) {
            while(!m_q.empty()) m_q.pop();
            while(!m_pipe.empty()) m_pipe.pop();
            while(!m_out.empty()) m_out.pop();
            return;
        }
        // pipe -> out
        if (!m_pipe.empty()) {
            while(!m_pipe.empty()) { m_out.push(m_pipe.front()); m_pipe.pop(); }
        }
        // q -> pipe (全部搬移示意)
        if (!m_q.empty()) {
            while(!m_q.empty()) { m_pipe.push(m_q.front()); m_q.pop(); }
        }
    }
};

} // namespace hybridacc
#endif // HYBRIDACC_NOC_XBUS_HPP
