#pragma once

#include "Utils/utils.hpp"
#include <systemc>

using namespace sc_core;

namespace hybridacc {
namespace pe {

// Multiply Unit
SC_MODULE(VMULU) {
    public:
        // Ports
        sc_in<v_fp16_t> op1;
        sc_in<v_fp16_t> op2;
        sc_out<v_fp16_t> result;

        // Constructor
        SC_CTOR(VMULU)
            : op1("op1"),
              op2("op2"),
              result("result")
        {
            DEBUG_MSG("[Create] VMULU", DEBUG_LEVEL_PE_COMPONENTS);
            SC_METHOD(combinational_process);
            sensitive << op1 << op2;
        }


        void combinational_process() {
            v_fp16_t res;
            for (size_t i = 0; i < 4; ++i) {
                res[i] = fp16_mul(op1.read()[i], op2.read()[i]);
            }
            result.write(res);
            DEBUG_MSG("[VMULU] op1=" << op1.read()
                      << " op2=" << op2.read()
                      << " result=" << res, DEBUG_LEVEL_PE_COMPONENTS);
        }
};

} // namespace pe
} // namespace hybridacc
