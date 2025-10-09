#ifndef HYBRIDACC_TOPCONTROLLER_HPP
#define HYBRIDACC_TOPCONTROLLER_HPP
// ============================================================================
//  File        : topcontroller.hpp
//  Module      : TopController
//  Description : 系統最高層控制，協調多個 SubController / DMA / NoC。
// ============================================================================
#include <systemc.h>
#include <vector>
#include <memory>
#include <cstdint>
#include <ostream>

namespace hybridacc {
class SubController; class DMA; class NoC; struct TaskDesc; struct PlatformDesc;

class TopController : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    SC_HAS_PROCESS(TopController);
    TopController(sc_core::sc_module_name name) : sc_module(name) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void bind_runtime(DMA* d, NoC* n, const std::vector<SubController*>& subs) {
        m_dma = d; m_noc = n; m_subs = subs;
    }

    // 提交高層任務 (簡化: 以 id 表示)
    void submit_kernel(uint32_t kid) { m_kernel_queue.push_back(kid); }

    bool idle() const { return m_kernel_queue.empty(); }

    void report(std::ostream& os) const {
        os << "TopController dispatched kernels: " << m_dispatched << "\n"; }

private:
    DMA* m_dma{nullptr};
    NoC* m_noc{nullptr};
    std::vector<SubController*> m_subs;
    std::vector<uint32_t> m_kernel_queue; // 待分派 kernel id
    uint64_t m_dispatched{0};

    void step(); // 將 kernel 轉為子任務分派 (簡化: round-robin 通知)
};

} // namespace hybridacc
#endif // HYBRIDACC_TOPCONTROLLER_HPP
