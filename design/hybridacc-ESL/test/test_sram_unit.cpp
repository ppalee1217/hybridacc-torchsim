#include <systemc>
#include <iostream>
#include <cassert>
#include "Cluster/SRAM.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc::cluster;

int sc_main(int argc, char* argv[]) {
    sc_clock clk("clk", 10, SC_NS);
    sc_signal<bool> reset_n;

    // Signals for SRAM
    sc_signal< sc_uint<32> > req_addr;
    sc_signal<bool> req_valid;
    sc_signal<bool> req_ready;

    sc_signal< sc_biguint<256> > resp_data;
    sc_signal<bool> resp_valid;
    sc_signal<bool> resp_ready;

    sc_signal<bool> write_en;
    sc_signal< sc_uint<32> > write_addr;
    sc_signal< sc_biguint<256> > write_data;
    sc_signal< sc_uint<32> > write_mask; // only lower bytes used for 256-bit (=32 bytes)

    SRAM<256,32> sram("SRAM", 1024, 3, 4); // 1KB, latency=3 cycles, pip_depth=4

    sram.clk(clk); sram.reset_n(reset_n);
    sram.req_addr(req_addr); sram.req_valid(req_valid); sram.req_ready(req_ready);
    sram.resp_data(resp_data); sram.resp_valid(resp_valid); sram.resp_ready(resp_ready);
    sram.write_en(write_en); sram.write_addr(write_addr); sram.write_data(write_data); sram.write_mask(write_mask);

    // Testbench
    SC_REPORT_INFO("Test", "Starting SRAM unit test");

    reset_n.write(false);
    req_valid.write(false);
    resp_ready.write(false);
    write_en.write(false);
    sc_start(20, SC_NS);
    reset_n.write(true);

    // Prepare a 256-bit pattern
    sc_biguint<256> pattern = 0;
    for (int i = 0; i < 8; ++i) {
        // set 8 32-bit words with different patterns
        pattern.range(32*i+31, 32*i) = (0xA0 + i);
    }

    // Write to address 16
    write_en.write(true);
    write_addr.write(16);
    write_data.write(pattern);
    write_mask.write( (1ULL << 32) - 1 ); // mark all bytes valid (only lower 32 bits used of mask signal)
    sc_start(10, SC_NS);
    write_en.write(false);

    // Issue a read request
    req_addr.write(16);
    req_valid.write(true);
    resp_ready.write(true);

    // Wait until req_ready is asserted and accepted
    while (!req_ready.read()) { sc_start(10, SC_NS); }
    // Let one cycle pass for acceptance
    sc_start(10, SC_NS);
    req_valid.write(false);

    // Wait for response valid
    while (!resp_valid.read()) {
        sc_start(10, SC_NS);
    }

    // Check data
    sc_biguint<256> got = resp_data.read();
    std::cout << "Got data: " << std::hex << got << std::dec << std::endl;

    assert(got == pattern && "SRAM returned incorrect data");

    SC_REPORT_INFO("Test", "SRAM unit test passed");

    return 0;
}
