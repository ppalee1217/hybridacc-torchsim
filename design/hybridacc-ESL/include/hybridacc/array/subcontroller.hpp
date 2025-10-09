#ifndef HYBRIDACC_ARRAY_SUBCONTROLLER_HPP
#define HYBRIDACC_ARRAY_SUBCONTROLLER_HPP
// ============================================================================
//  File        : subcontroller.hpp
//  Module      : SubController (Array-Level Controller)
//  Description : 管理單一 PE Array 的任務派發與統計。
// ============================================================================
#include <systemc.h>
#include <cstdint>
#include <vector>

namespace hybridacc {
struct TaskDesc; class DLB; class PEArray;

class SubController : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    SC_HAS_PROCESS(SubController);
    SubController(sc_core::sc_module_name name) : sc_module(name) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    void bind_runtime(DLB* d, PEArray* arr) { m_dlb = d; m_array = arr; }

    void submit_task(const TaskDesc& t);
    bool idle() const;
    uint64_t dispatched() const { return m_dispatched; }

private:
    DLB* m_dlb{nullptr};
    PEArray* m_array{nullptr};
    uint64_t m_dispatched{0};

    void step();
};

} // namespace hybridacc
#endif // HYBRIDACC_ARRAY_SUBCONTROLLER_HPP
