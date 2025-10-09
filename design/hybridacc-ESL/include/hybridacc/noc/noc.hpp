#ifndef HYBRIDACC_NOC_NOC_HPP
#define HYBRIDACC_NOC_NOC_HPP
// ============================================================================
//  File        : noc.hpp
//  Module      : NoC
//  Description : 簡化的 On-Chip Network 抽象，提供封包 enqueue/dequeue 骨架。
// ============================================================================
#include <systemc.h>
#include <queue>
#include <cstdint>
#include <optional>

namespace hybridacc {
struct NoCPacket { uint32_t src{0}; uint32_t dst{0}; uint32_t data{0}; };

class NoC : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    SC_HAS_PROCESS(NoC);
    NoC(sc_core::sc_module_name name) : sc_module(name) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void send(const NoCPacket& p) { m_in.push(p); }
    bool recv(NoCPacket& out) {
        if (m_out.empty()) return false; out = m_out.front(); m_out.pop(); return true; }

    size_t in_flight() const { return m_in.size() + m_mid.size() + m_out.size(); }

private:
    std::queue<NoCPacket> m_in;  // 新進封包
    std::queue<NoCPacket> m_mid; // 傳輸中 (一拍延遲示意)
    std::queue<NoCPacket> m_out; // 可被取走

    void step() {
        if (!rst_n.read()) {
            while(!m_in.empty()) m_in.pop();
            while(!m_mid.empty()) m_mid.pop();
            while(!m_out.empty()) m_out.pop();
            return;
        }
        // 推進 pipeline: out 清空 -> 釋放 mid -> in 進 mid
        if (!m_out.empty()) { /* 保留上一拍結果直到被取走 */ }
        // mid -> out
        if (!m_mid.empty()) {
            while(!m_mid.empty()) { m_out.push(m_mid.front()); m_mid.pop(); }
        }
        // in -> mid (簡化: 全部搬移)
        if (!m_in.empty()) {
            while(!m_in.empty()) { m_mid.push(m_in.front()); m_in.pop(); }
        }
    }
};

} // namespace hybridacc
#endif // HYBRIDACC_NOC_NOC_HPP
