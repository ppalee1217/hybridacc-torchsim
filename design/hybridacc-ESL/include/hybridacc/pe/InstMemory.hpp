#ifndef HYBRIDACC_PE_INSTMEMORY_HPP
#define HYBRIDACC_PE_INSTMEMORY_HPP

// ============================================================================
//  File        : InstMemory.hpp
//  Description : Instruction memory RTL behavioral model (64 x 16-bit) implemented with FF.
//                Provides asynchronous read and single-port write (for init / DMA).
//                Retains fetch() for early skeleton compatibility (combinational direct read, not recommended in timing paths).
//  Change Log  : 2025-10-05  Replace simple container with SystemC module.
// ============================================================================

#include <systemc.h>
#include <vector>
#include <cstdint>
#include <array>

using namespace sc_core;

namespace hybridacc {

// compile-time ceil(log2(n))
constexpr unsigned im_clog2(unsigned n){ return (n<=1)?0: 1 + im_clog2((n+1)/2); }

template<unsigned DEPTH = 64>
SC_MODULE(InstMemoryT) {
    static_assert(DEPTH > 0, "DEPTH must be > 0");
public:
    static constexpr unsigned DEPTH_WORDS = DEPTH;
    static constexpr unsigned ADDR_BITS_RAW = im_clog2(DEPTH_WORDS);
    static constexpr unsigned ADDR_BITS = (ADDR_BITS_RAW==0)?1:ADDR_BITS_RAW; // 至少 1-bit

    // Ports
    sc_in<bool>                 clk{"clk"};
    sc_in<bool>                 rst_n{"rst_n"};

    sc_in<bool>                 en_i{"en_i"};
    sc_in<bool>                 w_en_i{"w_en_i"};
    sc_in<sc_uint<ADDR_BITS>>   addr_i{"addr_i"};

    sc_out<sc_uint<16>>         rd_data_o{"rd_data_o"};
    sc_in<sc_uint<16>>          wr_data_i{"wr_data_i"};

    // Non-synthesizable methods
    void load(const std::vector<uint16_t>& bin) {
        unsigned n = (bin.size() < DEPTH_WORDS) ? bin.size() : DEPTH_WORDS;
        for(unsigned i=0;i<n;++i) mem_[i] = bin[i];
        for(unsigned i=n;i<DEPTH_WORDS;++i) mem_[i] = 0;
    }
    uint16_t fetch(uint16_t pc) const { return pc < DEPTH_WORDS ? mem_[pc] : 0; }
    size_t size() const { return DEPTH_WORDS; }


    void clear() {
        for(unsigned i=0; i<DEPTH_WORDS; ++i) {
            mem_[i] = 0;
        }
    }

    SC_CTOR(InstMemoryT) {
        SC_METHOD(comb_proc);
        sensitive << en_i << addr_i;
        dont_initialize();

        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();
    }
private:
    std::array<sc_uint<16>, DEPTH_WORDS> mem_{};
    void seq_proc() {
        if(!rst_n.read()) { // reset
            rd_data_o.write(0);
            for(unsigned i=0;i<DEPTH_WORDS;++i) mem_[i] = 0;
            return;
        }
        if(en_i.read() && w_en_i.read()) // write enable
        {
            auto wa = addr_i.read();
            assert(wa < DEPTH_WORDS); // address out of range
            mem_[wa] = wr_data_i.read();
        }
    }

    void comb_proc() {
        if(en_i.read()) { // read enable
            auto ra = addr_i.read();
            assert(ra < DEPTH_WORDS); // address out of range
            rd_data_o.write(mem_[ra]);
        } else {
            rd_data_o.write(0);
        }
    }
};

// Backward-compatible alias with default depth 64
using InstMemory = InstMemoryT<64>;

} // namespace hybridacc
#endif // HYBRIDACC_PE_INSTMEMORY_HPP