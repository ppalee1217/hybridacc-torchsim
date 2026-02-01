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
        sc_in<bool> bank_sel; // 0: Write->Bank0, Read->Bank1; 1: Write->Bank1, Read->Bank0

        // DM Write Ports
        sc_in<bool> dm_write_en;
        sc_in<uint16_t> dm_write_addr;
        sc_in<uint64_t> dm_write_data;
        sc_in<uint8_t> dm_write_mask;

        // DM Read Ports
        sc_in<uint16_t> dm_read_addr;
        sc_out<uint64_t> dm_read_data;

        SC_CTOR(DataMemory)
            : clk("clk"),
              reset_n("reset_n"),
              bank_sel("bank_sel"),
              dm_write_en("dm_write_en"),
              dm_write_addr("dm_write_addr"),
              dm_write_data("dm_write_data"),
              dm_write_mask("dm_write_mask"),
              dm_read_addr("dm_read_addr"),
              dm_read_data("dm_read_data")
        {
            DEBUG_MSG("[Create] DataMemory (Dual Bank)", DEBUG_LEVEL_PE_COMPONENTS);

            // Initialize banks
            banks[0].resize(DMEMORY_DEFAULT_SIZE_BYTES / sizeof(uint8_t), 0);
            banks[1].resize(DMEMORY_DEFAULT_SIZE_BYTES / sizeof(uint8_t), 0);

            SC_CTHREAD(sequential_process, clk.pos());
            reset_signal_is(reset_n, false);
        }

        // Helper Methods
        void reset() {
            banks[0].assign(DMEMORY_DEFAULT_SIZE_BYTES / sizeof(uint8_t), 0);
            banks[1].assign(DMEMORY_DEFAULT_SIZE_BYTES / sizeof(uint8_t), 0);
        }

        // Debug/Backdoor Access
        int size() const { return DMEMORY_DEFAULT_SIZE_BYTES; }

        // Read from specific bank (debug use)
        uint64_t readWord(int bank_idx, int idx) const {
            if (bank_idx < 0 || bank_idx > 1) return 0;
            const auto& mem = banks[bank_idx];
            if (idx < 0 || idx >= (int)mem.size()) return 0;

            uint64_t word = 0;
            for (int i = 0; i < 8; ++i) {
                if(idx + i < (int)mem.size())
                    word |= static_cast<uint64_t>(mem[idx + i]) << (i * 8);
            }
            return word;
        }

        std::array<std::vector<uint8_t>, 2> banks;

        void sequential_process() {
            // Reset initialization
            reset();
            dm_read_data.write(0);
            wait();

            while (true) {
                // Determine Read/Write banks based on bank_sel
                // bank_sel indicates the bank SDMA (Writer) is using.
                // If bank_sel=0: Write -> Bank 0, Read -> Bank 1
                // If bank_sel=1: Write -> Bank 1, Read -> Bank 0
                bool sel = bank_sel.read();
                int write_bank_idx = sel ? 1 : 0;
                int read_bank_idx = sel ? 0 : 1;

                // Read operation (from Read Bank)
                uint64_t r_data = readWord(read_bank_idx, dm_read_addr.read() & DMEMORY_ADDRESS_MASK);
                dm_read_data.write(r_data);

                // Write operation (to Write Bank)
                if (dm_write_en.read()) {
                    int w_addr = dm_write_addr.read() & DMEMORY_ADDRESS_MASK;
                    uint64_t w_data = dm_write_data.read();
                    uint8_t mask = dm_write_mask.read();

                    auto& mem = banks[write_bank_idx];

                    DEBUG_MSG("[DataMemory] Writing Bank" << write_bank_idx
                              << "[" << w_addr << "] = 0x"
                              << std::hex << w_data << " mask 0x" << (int)mask << std::dec,
                              DEBUG_LEVEL_PE_COMPONENTS);

                    for (int i = 0; i < 8; ++i) {
                        if (mask & (1 << i)) {
                            if (w_addr + i < (int)mem.size()) {
                                mem[w_addr + i] = static_cast<uint8_t>((w_data >> (i * 8)) & 0xFF);
                            }
                        }
                    }
                }
                wait();
            }
        }
};

}; // namespace pe
}; // namespace hybridacc