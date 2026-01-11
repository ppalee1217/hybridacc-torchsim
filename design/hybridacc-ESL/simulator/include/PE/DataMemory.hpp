#pragma once

#include "utils.hpp"
#include <systemc>
#include <array>

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

constexpr int DMEMORY_ADDRESS_WIDTH = 9; // 512 bytes addressable space
constexpr int DMEMORY_ADDRESS_MASK = (1 << DMEMORY_ADDRESS_WIDTH) - 1;
constexpr int DMEMORY_DEFAULT_SIZE_BYTES = (1 << DMEMORY_ADDRESS_WIDTH); // 512 bytes

// Data Memory (element size 16-bit, bandwidth 64-bit)
SC_MODULE(DataMemory) {
    public:
        // Ports
        sc_in<bool> clk;
        sc_in<bool> reset_n;

        sc_in<bool> dm_write_en;
        sc_in<uint16_t> dm_write_addr;
        sc_in<uint64_t> dm_write_data;
        sc_in<uint8_t> dm_write_mask;

        sc_in<uint16_t> dm_read_addr;
        sc_out<uint64_t> dm_read_data;

        SC_CTOR(DataMemory)
            : clk("clk"),
              reset_n("reset_n"),
              dm_write_en("dm_write_en"),
              dm_write_addr("dm_write_addr"),
              dm_write_data("dm_write_data"),
              dm_write_mask("dm_write_mask"),
              dm_read_addr("dm_read_addr"),
              dm_read_data("dm_read_data"),
              mem(DMEMORY_DEFAULT_SIZE_BYTES / sizeof(uint8_t), 0)
        {
            DEBUG_MSG("[Create] DataMemory", DEBUG_LEVEL_PE_COMPONENTS);
            SC_CTHREAD(sequential_process, clk.pos());
            reset_signal_is(reset_n, false);
            SC_METHOD(combinational_process);
            sensitive << reset_n;
        } // default DMEMORY_DEFAULT_SIZE_BYTES bytes

        // Helper Methods
        void reset() { mem.clear(); mem.resize(DMEMORY_DEFAULT_SIZE_BYTES / sizeof(uint8_t), 0); }
        void resize(int bytes) { mem.resize(bytes / sizeof(uint8_t), 0); }
        int size() const { return (int)(mem.size() * sizeof(uint8_t)); }
        const std::vector<uint8_t>& raw() const { return mem; }

        uint64_t readWord(int idx) const {
            if (idx < 0 || idx >= size()) {
                throw std::out_of_range("[readWord] Index out of range: " + std::to_string(idx));
            }
            uint64_t word = 0;
            for (int i = 0; i < 8; ++i) {
                word |= static_cast<uint64_t>(mem[idx + i]) << (i * 8);
            }
            return word;
        }

        void writeWord(int idx, uint64_t v, uint8_t mask) {
            if (mask == 0) return; // 如果 mask 為 0，則
            if (idx < 0 || idx >= size()) {
                throw std::out_of_range("[writeWord] Index out of range: " + std::to_string(idx));
            }
            for (int i = 0; i < 8; ++i) {
                if (mask & (1 << i)) mem[idx + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
            }
        }


        std::vector<uint8_t> mem;

        void combinational_process() {
            // This process should not write to any ports to avoid multiple drivers
            // All outputs are handled by sequential_process
        }

        void sequential_process() {
            // Reset initialization
            reset();
            dm_read_data.write(0);
            wait();

            while (true) {
                // Read operation
                uint64_t data = readWord(dm_read_addr.read() & DMEMORY_ADDRESS_MASK);
                dm_read_data.write(data);

                // Write operation
                if (dm_write_en.read()) {
                    DEBUG_MSG("[DataMemory] Writing DM["
                              << (dm_write_addr.read() & DMEMORY_ADDRESS_MASK) << "] = 0x"
                              << std::hex << std::setw(16) << std::setfill('0') << dm_write_data.read()
                              << " with mask 0x"
                              << std::hex << std::setw(2) << std::setfill('0') << static_cast<uint16_t>(dm_write_mask.read())
                              << std::dec, DEBUG_LEVEL_PE_COMPONENTS);
                    writeWord(dm_write_addr.read() & DMEMORY_ADDRESS_MASK, dm_write_data.read(), dm_write_mask.read());
                }
                wait();
            }
        }
};

}; // namespace pe
}; // namespace hybridacc