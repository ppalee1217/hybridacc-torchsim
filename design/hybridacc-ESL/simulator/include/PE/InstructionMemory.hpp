#pragma once

#include "utils.hpp"
#include <systemc>

namespace hybridacc {
namespace pe {

// Instruction Memory
SC_MODULE(InstructionMemory) {
    public:
        // Ports
        sc_in<bool> clk;
        sc_in<bool> reset_n;

        sc_in<bool> im_write_en;
        sc_in<uint16_t> im_write_addr;
        sc_in<pe_inst_t> im_write_data;

        sc_in<uint16_t> im_read_addr;
        sc_out<pe_inst_t> im_read_data;


        SC_CTOR(InstructionMemory)
            : clk("clk"),
              reset_n("reset_n"),
              im_write_en("im_write_en"),
              im_write_addr("im_write_addr"),
              im_write_data("im_write_data"),
              im_read_addr("im_read_addr"),
              im_read_data("im_read_data"),
              mem(512 / sizeof(uint16_t), 0)
        {
            DEBUG_MSG("[Create] InstructionMemory");
            SC_CTHREAD(sequential_process, clk.pos());
            reset_signal_is(reset_n, false);
            SC_METHOD(combinational_process);
            sensitive << reset_n << im_read_addr << im_write_en;
        } // default 512 bytes

        // Helper Methods
        void reset() { mem.assign(512 / sizeof(uint16_t), 0); }
        void resize(int bytes){ mem.assign(bytes / sizeof(uint16_t), 0);}
        int size() const { return (int)mem.size() * sizeof(uint16_t); }
        void set(int pc, pe_inst_t v){ mem[pc / sizeof(uint16_t)] = v; }
        pe_inst_t fetch(int pc) const { return mem[pc / sizeof(uint16_t)]; }
        void clear() { mem.clear(); }
        void dump() const {
            for (size_t i = 0; i < mem.size(); ++i) {
                DEBUG_MSG("IM[" << (i * sizeof(uint16_t)) << "] = " << std::hex << mem[i]);
            }
        }


        std::vector<pe_inst_t> mem;

        void combinational_process() {
            if (!reset_n.read()) {
                //DEBUG_MSG("[InstructionMemory] Reset active, read data set to 0");
                im_read_data.write(0);
            } else {
                //DEBUG_MSG("[InstructionMemory] Read from address " << im_read_addr.read());
                im_read_data.write(mem[im_read_addr.read() / sizeof(uint16_t)]);
            }
        }

        void sequential_process() {
            // Reset initialization
            reset();
            wait();

            while (true) {
                if (im_write_en.read()) {
                    mem[im_write_addr.read() / sizeof(uint16_t)] = im_write_data.read();
                }
                wait();
            }
        }
    };


}; // namespace pe
}; // namespace hybridacc