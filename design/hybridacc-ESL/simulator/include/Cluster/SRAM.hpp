#pragma once

#include <systemc>
#include <vector>
#include <deque>
#include <cstdint>
#include <iomanip>
#include <sstream>

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

// Configurable SRAM module
// Template parameters:
//  - DATA_WIDTH_BITS: data bus width in bits (must be divisible by 8)
//  - ADDR_WIDTH: width of address field (in bits)
// Default target is 256-bit wide words (8 x 32-bit lanes), matching existing testbench.

template <unsigned DATA_WIDTH_BITS = 256, unsigned ADDR_WIDTH = 32>
SC_MODULE(SRAM) {
    static_assert(DATA_WIDTH_BITS % 8 == 0, "DATA_WIDTH_BITS must be a multiple of 8");

    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Read request interface
    sc_in< sc_uint<ADDR_WIDTH> > req_addr;
    sc_in<bool> req_valid;
    sc_out<bool> req_ready;

    // Read response interface
    sc_out< sc_biguint<DATA_WIDTH_BITS> > resp_data;
    sc_out<bool> resp_valid;
    sc_in<bool> resp_ready;

    // Optional write interface
    sc_in<bool> write_en;
    sc_in< sc_uint<ADDR_WIDTH> > write_addr;
    sc_in< sc_biguint<DATA_WIDTH_BITS> > write_data;
    sc_in< sc_uint<DATA_WIDTH_BITS/8> > write_mask; // 1 bit per byte

    // Configuration (constructor args)
    const size_t data_width_bits;
    const size_t data_width_bytes;
    const size_t addr_width;
    const size_t default_latency;
    const size_t pipeline_depth;

    // Internal memory as byte array
    std::vector<uint8_t> mem;

    struct Req { uint32_t addr; int cycles_left; };
    std::deque<Req> pipeline;

    SC_HAS_PROCESS(SRAM);

    // Constructor: size_bytes is total memory size in bytes
    SRAM(sc_module_name name,
         size_t size_bytes = (1<<16), // default 64 KiB
         size_t latency = 5,
         size_t pip_depth = 8)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          req_addr("req_addr"), req_valid("req_valid"), req_ready("req_ready"),
          resp_data("resp_data"), resp_valid("resp_valid"), resp_ready("resp_ready"),
          write_en("write_en"), write_addr("write_addr"), write_data("write_data"), write_mask("write_mask"),
          data_width_bits(DATA_WIDTH_BITS),
          data_width_bytes(DATA_WIDTH_BITS/8),
          addr_width(ADDR_WIDTH),
          default_latency(latency),
          pipeline_depth(pip_depth),
          mem(size_bytes, 0)
    {
        // Logging
        // DEBUG_MSG not included here to avoid dependency on debug infrastructure

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

    // Reset memory (to zero)
    void reset() {
        std::fill(mem.begin(), mem.end(), 0);
        pipeline.clear();
    }

    // Resize memory (in bytes)
    void resize(size_t bytes) { mem.resize(bytes, 0); }
    size_t size_bytes_total() const { return mem.size(); }

    // Helper: read a DATA_WIDTH word starting at byte address (wraps around mem)
    sc_biguint<DATA_WIDTH_BITS> read_word(uint32_t byte_addr) const {
        sc_biguint<DATA_WIDTH_BITS> out = 0;
        size_t base = byte_addr % mem.size();
        for (size_t i = 0; i < data_width_bytes; ++i) {
            uint8_t b = mem[(base + i) % mem.size()];
            out.range(8*i+7, 8*i) = b;
        }
        return out;
    }

    // Helper: write a DATA_WIDTH word with byte mask
    void write_word(uint32_t byte_addr, sc_biguint<DATA_WIDTH_BITS> value, uint64_t byte_mask) {
        size_t base = byte_addr % mem.size();
        for (size_t i = 0; i < data_width_bytes; ++i) {
            if (byte_mask & (1ULL << i)) {
                uint8_t b = static_cast<uint8_t>(value.range(8*i+7, 8*i).to_uint());
                mem[(base + i) % mem.size()] = b;
            }
        }
    }

    // SystemC sequential process: handle requests and simulate latency with pipeline
    void sequential_process() {
        // Init
        reset();
        req_ready.write(false);
        resp_valid.write(false);
        resp_data.write(0);
        wait();

        while (true) {
            // Write handling (synchronous write)
            if (write_en.read()) {
                write_word(write_addr.read(), write_data.read(), (uint64_t)write_mask.read());
            }

            // Can accept a request if pipeline not full
            bool can_accept = (pipeline.size() < pipeline_depth);
            req_ready.write(can_accept);

            // Accept request
            if (req_valid.read() && can_accept) {
                Req r;
                r.addr = req_addr.read();
                r.cycles_left = static_cast<int>(default_latency);
                pipeline.push_back(r);
            }

            // Process pipeline: produce at most one response per cycle
            bool output_valid = false;
            sc_biguint<DATA_WIDTH_BITS> output_data = 0;

            if (!pipeline.empty()) {
                // decrement head first
                for (auto &r : pipeline) r.cycles_left--;
                // check head
                if (pipeline.front().cycles_left <= 0) {
                    // prepare data
                    output_data = read_word(pipeline.front().addr);
                    output_valid = true;
                    pipeline.pop_front();
                }
            }

            // Drive output with backpressure handling
            if (output_valid) {
                // If downstream is not ready, hold resp_valid high until it accepts
                if (!resp_valid.read()) {
                    // First cycle we present data
                    resp_data.write(output_data);
                    resp_valid.write(true);
                } else {
                    // resp_valid already high from previous cycle (shouldn't happen often)
                    // leave data as is
                }
            } else {
                // If we had resp_valid asserted and downstream still not ready, keep asserting
                if (resp_valid.read() && !resp_ready.read()) {
                    // keep asserting
                } else {
                    resp_valid.write(false);
                }
            }

            // If downstream accepted (resp_valid && resp_ready), deassert
            if (resp_valid.read() && resp_ready.read()) {
                resp_valid.write(false);
            }

            wait();
        }
    }

    // Small utility: pretty print a range of memory (for debugging)
    std::string dump(size_t start = 0, size_t len = 64) const {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < len && (start+i) < mem.size(); ++i) {
            if ((i % 16) == 0) oss << "\n" << std::setw(8) << (start + i) << ": ";
            oss << std::setw(2) << static_cast<uint32_t>(mem[start + i]) << " ";
        }
        return oss.str();
    }
}; // SRAM

} // namespace cluster
} // namespace hybridacc
