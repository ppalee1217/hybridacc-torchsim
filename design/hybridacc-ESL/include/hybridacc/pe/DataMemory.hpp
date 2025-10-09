#ifndef HYBRIDACC_PE_DATAMEMORY_HPP
#define HYBRIDACC_PE_DATAMEMORY_HPP

// ============================================================================
//  File        : DataMemory.hpp
//  Description : Parameterized data memory RTL behavioral model (BYTES x 8-bit, 64-bit port).
//                - template<unsigned BYTES=512>
//                - 1-cycle synchronous read latency, write-first
//                - Supports byte mask writes
//                - Provides load16/loadBytes and combinational fetch() (64-bit aligned)
//  Change Log  : 2025-10-05  Parameterized depth (BYTES).
// ============================================================================

#include <systemc.h>
#include <vector>
#include <cstdint>
#include <array>

namespace hybridacc {

// compile-time ceil(log2(n)) for address width
constexpr unsigned dm_clog2(unsigned n){ return (n<=1)?0: 1 + dm_clog2((n+1)/2); }

template<unsigned BYTES = 512>
SC_MODULE(DataMemoryT) {
    static_assert(BYTES > 0, "BYTES must be > 0");
    static_assert(BYTES % 8 == 0, "BYTES must be multiple of 8 (64-bit alignment)");
public:
    static constexpr unsigned WORD_BYTES = 8;            // 64-bit access granularity
    static constexpr unsigned ADDR_BITS_RAW = dm_clog2(BYTES);
    static constexpr unsigned ADDR_BITS = (ADDR_BITS_RAW==0)?1:ADDR_BITS_RAW; // 至少1 bit

    // Ports
    sc_in<bool>        clk{"clk"};
    sc_in<bool>        rst_n{"rst_n"};

    // Read port (1-cycle latency)
    sc_in<bool>        en_i{"en_i"};
    sc_out<sc_uint<64>> rd_data_o{"rd_data_o"};

    // Write port
    sc_in<bool>        wr_en_i{"wr_en_i"};
    sc_in<sc_uint<ADDR_BITS>> addr_i{"addr_i"};
    sc_in<sc_uint<64>> wr_data_i{"wr_data_i"};
    sc_in<sc_uint<8>>  byte_mask_i{"byte_mask_i"};

    // Bulk preload (16-bit words)
    void load16(const std::vector<uint16_t>& words){
        unsigned need = words.size()*2u;
        unsigned n = (need < BYTES)? need : BYTES;
        unsigned byte_idx=0;
        for(size_t i=0;i<words.size() && byte_idx+1<BYTES; ++i){
            uint16_t w = words[i];
            mem_[byte_idx++] = static_cast<uint8_t>(w & 0xFF);
            mem_[byte_idx++] = static_cast<uint8_t>((w>>8) & 0xFF);
        }
        for(; byte_idx<BYTES; ++byte_idx) mem_[byte_idx]=0;
    }

    // Bulk preload (raw bytes)
    void loadBytes(const std::vector<uint8_t>& bytes){
        unsigned n = (bytes.size() < BYTES)? bytes.size() : BYTES;
        for(unsigned i=0;i<n;++i) mem_[i]=bytes[i];
        for(unsigned i=n;i<BYTES;++i) mem_[i]=0;
    }

    // Combinational fetch (64-bit aligned). Out-of-range or unaligned -> 0
    uint64_t fetch(uint32_t byte_addr) const {
        if(byte_addr >= BYTES) return 0;
        if(byte_addr & (WORD_BYTES-1)) return 0;
        uint64_t v=0;
        for(unsigned i=0; i<WORD_BYTES && (byte_addr+i)<BYTES; ++i)
            v |= (uint64_t)mem_[byte_addr+i] << (8*i);
        return v;
    }

    size_t size() const { return BYTES; }

    SC_CTOR(DataMemoryT) {
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();
    }
private:
    std::array<sc_uint<8>, BYTES> mem_{};
    sc_uint<ADDR_BITS> rd_addr_q{0};
    bool rd_valid_q = false;

    void seq_proc(){
        if(!rst_n.read()) {
            rd_data_o.write(0);
            rd_addr_q = 0; rd_valid_q = false;
            for(unsigned i=0;i<BYTES;++i) mem_[i]=0;
            return;
        }
        // Write-first
        if(wr_en_i.read()){
            auto a = wr_addr.read();
            if(a < BYTES){
                if(a & (WORD_BYTES-1)) SC_REPORT_WARNING(name(),"Unaligned write address");
                for(unsigned b=0; b<WORD_BYTES; ++b){
                    if(byte_mask_i.read() & (1u<<b)){
                        unsigned idx = a + b;
                        if(idx < BYTES) mem_[idx] = (wr_data_i.read() >> (8*b)) & 0xFF;
                    }
                }
            } else {
                SC_REPORT_WARNING(name(),"Write address out of range");
            }
        }
        // Output from previous cycle
        uint64_t dout = 0;
        if(rd_valid_q){
            auto a = rd_addr_q;
            if(a < BYTES){
                if(a & (WORD_BYTES-1)) {
                    SC_REPORT_WARNING(name(),"Unaligned read address");
                } else {
                    for(unsigned b=0; b<WORD_BYTES && (a+b)<BYTES; ++b)
                        dout |= (uint64_t)mem_[a+b] << (8*b);
                }
            } else {
                SC_REPORT_WARNING(name(),"Read address out of range");
            }
        }
        rd_data_o.write(dout);
        // Latch new read
        rd_valid_q = en_i.read();
        rd_addr_q  = addr_i.read();
    }
};

// Backward-compatible alias
using DataMemory = DataMemoryT<512>;

} // namespace hybridacc
#endif // HYBRIDACC_PE_DATAMEMORY_HPP