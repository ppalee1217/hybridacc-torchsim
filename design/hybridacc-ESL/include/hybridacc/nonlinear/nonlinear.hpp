#ifndef HYBRIDACC_NONLINEAR_NONLINEAR_HPP
#define HYBRIDACC_NONLINEAR_NONLINEAR_HPP
// ============================================================================
//  File        : nonlinear.hpp
//  Module      : NonLinearUnit
//  Description : 提供常見非線性運算 (ReLU / Sigmoid / Tanh 等) 之骨架 (僅 ReLU 示意)。
// ============================================================================
#include <systemc>
#include <cstdint>

namespace hybridacc {
class NonLinearUnit : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    sc_in<int32_t>  in_data{"in_data"};
    sc_out<int32_t> out_data{"out_data"};

    SC_HAS_PROCESS(NonLinearUnit);
    NonLinearUnit(sc_core::sc_module_name name) : sc_module(name) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

private:
    void step() {
        if (!rst_n.read()) { out_data.write(0); return; }
        // ReLU 示意
        int32_t v = in_data.read();
        out_data.write(v < 0 ? 0 : v);
    }
};
}
#endif // HYBRIDACC_NONLINEAR_NONLINEAR_HPP
