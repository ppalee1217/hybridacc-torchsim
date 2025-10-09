#ifndef HYBRIDACC_PE_PREGFILE_HPP
#define HYBRIDACC_PE_PREGFILE_HPP
// ============================================================================
// File        : Pregfile.hpp
// Description : Partial sum register file for the Processing Element (PE).
//               Provides 32 scalar partial sum registers P[0..31] and a vector
//               namespace of 32 VPIDs (0..31). VPIDs < 8 directly map to groups
//               of 4 consecutive scalar registers (vpid*4 + lane). VPIDs >= 8
//               use 24 internal 64-bit vector entries (VP64[0..23]).
//               Features:
//                 * Scalar: single asynchronous read, single synchronous write
//                 * Vector: single asynchronous read, single synchronous write
//                 * Active-low reset clears all registers
//               NOTE: Write counters were removed (external monitor module
//                     should be used if statistics are required).
// Change Log  : 2025-10-06  Added English documentation & cleaned comments.
//               2025-10-06  Removed internal counters (external design request).
// ============================================================================

#include <systemc.h>
#include <array>
#include "hybridacc/utils.hpp"   // Get DATA_WIDTH and related macros

namespace hybridacc { namespace pe {

// Element width equals DATA_WIDTH (platform defines 16 bits)
using element_t = sc_uint<DATA_WIDTH>;
using vector_t = sc_uint<DATA_WIDTH*VECTOR_SIZE>; // 4 lanes of DATA_WIDTH each

// SC_MODULE implementation of partial sum + vector register file:
// Layout:
//   - 32 scalar partial sum registers: P[0..31]
//   - 32 vector partial sum IDs (vpid 0..31):
//       * vpid < 8  -> directly mapped to P (each vpid uses 4 consecutive scalars)
//       * vpid >= 8 -> stored in internal 24x64-bit vector entries VP64[0..23]
// Interface:
//   - Scalar: single async read / single sync write
//   - Vector: single async read / single sync write
//   - Reset: active-low, clears storage

SC_MODULE(PsumRegFile) {
    // -------- Ports --------
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};        // Active-low reset

    sc_in<bool> mode_i{"mode_i"};    // 0: scalar, 1: vector
    sc_in<sc_uint<5>> rs_idx_i{"rs_idx_i"};  // Read scalar index or vector index
    sc_in<sc_uint<5>> rd_idx_i{"rd_idx_i"};  // Write scalar index or vector index

    sc_in<bool> we_i{"we_i"};        // Write enable
    sc_in<bool> clear_i{"clear_i"};  // Clear all registers (independent of rst_n)

    sc_in<vector_t> vrd_data_i{"vrd_data_i"};   // Write data (vector input), scalar uses lower 16 bits
    sc_out<vector_t> vrs_data_o{"vrs_data_o"};  // Read data (vector output), scalar uses lower 16 bits

    // -------- Internal storage --------
    std::array<element_t, 32> P{};                    // Scalar registers
    std::array<sc_uint<DATA_WIDTH*4>, 24> VP64{};     // Vector registers (vpid 8..31)

    // -------- Processes --------
    SC_CTOR(PsumRegFile) {
        SC_METHOD(write_proc); // synchronous write & reset/clear
        sensitive << clk.pos();

        SC_METHOD(read_proc); // asynchronous read
        // Asynchronous read w.r.t. index & mode; also update after a write (clk edge)
        sensitive << mode_i << rs_idx_i << clk.pos();
    }

    // External API (testbench direct invocation)
    void clear() {
        for (auto &v: P) v = 0;
        for (auto &v: VP64) v = 0;
    }

    // Direct (software-like) accessors (bypass ports)
    void setP(unsigned pid, element_t v) { if (pid < 32) P[pid] = v; }
    element_t getP(unsigned pid) const { return (pid < 32)? P[pid]: 0; }

    void setVP64(unsigned vpid, sc_uint<DATA_WIDTH*4> vec) {
        if (vpid < 8) {
            for (int i=0;i<4;++i) {
                P[vpid*4 + i] = (vec >> (i*DATA_WIDTH)) & ((1u<<DATA_WIDTH)-1);
            }
        } else if (vpid < 32) {
            VP64[vpid - 8] = vec;
        }
    }
    sc_uint<DATA_WIDTH*4> getVP64(unsigned vpid) const {
        sc_uint<DATA_WIDTH*4> out = 0;
        if (vpid < 8) {
            for (int i=0;i<4;++i) {
                out |= (sc_uint<DATA_WIDTH>(P[vpid*4 + i]) << (i*DATA_WIDTH));
            }
        } else if (vpid < 32) {
            out = VP64[vpid - 8];
        }
        return out;
    }

private:
    // Synchronous write & reset/clear handling
    void write_proc() {
        if (!rst_n.read()) {
            for (auto &v: P) v = 0;
            for (auto &v: VP64) v = 0;
        } else if (clear_i.read()) { // synchronous clear
            for (auto &v: P) v = 0;
            for (auto &v: VP64) v = 0;
        } else if (we_i.read()) {
            if (mode_i.read() == false) { // scalar write
                unsigned idx = rd_idx_i.read();
                if (idx < 32) {
                    P[idx] = (vrd_data_i.read() & ((1u<<DATA_WIDTH)-1));
                }
            } else { // vector write
                unsigned vpid = rd_idx_i.read();
                if (vpid < 8) {
                    sc_uint<DATA_WIDTH*4> tmp = vrd_data_i.read();
                    for (int lane=0; lane<4; ++lane) {
                        P[vpid*4 + lane] = (tmp >> (lane*DATA_WIDTH)) & ((1u<<DATA_WIDTH)-1);
                    }
                } else if (vpid < 32) {
                    VP64[vpid - 8] = vrd_data_i.read();
                }
            }
        }
    }

    // Asynchronous read (combinational w.r.t. index + mode, updated also after write clock edge)
    void read_proc() {
        vector_t out = 0;
        if (mode_i.read() == false) { // scalar
            unsigned idx = rs_idx_i.read();
            if (idx < 32) {
                // place scalar in lowest lane, rest zero
                out.range(DATA_WIDTH-1, 0) = P[idx];
            }
        } else { // vector
            unsigned vpid = rs_idx_i.read();
            if (vpid < 8) {
                for (int lane=0; lane<4; ++lane) {
                    out.range((lane+1)*DATA_WIDTH-1, lane*DATA_WIDTH) = P[vpid*4 + lane];
                }
            } else if (vpid < 32) {
                sc_uint<DATA_WIDTH*4> vec = VP64[vpid - 8];
                for (int lane=0; lane<4;++lane) {
                    out.range((lane+1)*DATA_WIDTH-1, lane*DATA_WIDTH) = (vec >> (lane*DATA_WIDTH)) & ((1u<<DATA_WIDTH)-1);
                }
            }
        }
        vrs_data_o.write(out);
    }
};

}} // namespace hybridacc::pe

#endif // HYBRIDACC_PE_PREGFILE_HPP
