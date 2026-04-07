#pragma once

#include <systemc>
#include <cstdint>
#include "Core/rv32i_mcu/PipelineTypes.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

SC_MODULE(GPR) {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;

    sc_core::sc_in<bool> write_en;
    sc_core::sc_in<sc_uint<5>> write_idx;
    sc_core::sc_in<sc_uint<32>> write_data;

    sc_core::sc_in<sc_uint<5>> read_idx1;
    sc_core::sc_out<sc_uint<32>> read_data1;

    sc_core::sc_in<sc_uint<5>> read_idx2;
    sc_core::sc_out<sc_uint<32>> read_data2;

    sc_core::sc_signal<sc_uint<32>> regs[32];

    SC_CTOR(GPR) {
        SC_METHOD(read);
        sensitive << read_idx1 << read_idx2;
        for (int i = 0; i < 32; ++i) {
            sensitive << regs[i];
        }

        SC_METHOD(write);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

    void read() {
        const uint32_t idx1 = read_idx1.read().to_uint();
        const uint32_t idx2 = read_idx2.read().to_uint();
        read_data1.write(idx1 == 0 ? sc_uint<32>(0u) : regs[idx1].read());
        read_data2.write(idx2 == 0 ? sc_uint<32>(0u) : regs[idx2].read());
    }

    void write() {
        if (!rst_n.read()) {
            for (int i = 0; i < 32; ++i) regs[i].write(0u);
        } else if (write_en.read() && write_idx.read() != 0) {
            regs[write_idx.read().to_uint()].write(write_data.read());
        }
        regs[0].write(0u);
    }

};


} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc