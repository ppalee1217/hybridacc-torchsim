#ifndef HYBRIDACC_PE_TREGFILE_HPP
#define HYBRIDACC_PE_TREGFILE_HPP

// ============================================================================
// File        : Tregfile.hpp
// Description : Transform register file for the Processing Element (PE).
//               Provides 12 scalar transform registers T[0..11].
//               Features:
//                 * Scalar: single asynchronous read, single synchronous write
//                 * Active-low reset clears all registers
//               NOTE: Write counters were removed (external monitor module
//                     should be used if statistics are required).
// Change Log  : 2025-10-06  Added English documentation & cleaned comments.
//               2025-10-08  Fix constructor name / sensitivity / missing ; / logic cleanup.
// ============================================================================

#include <systemc.h>
#include <array>
#include "hybridacc/utils.hpp"   // Get DATA_WIDTH and related macros

namespace hybridacc { namespace pe {
// Element width equals DATA_WIDTH (platform defines 16 bits)
using element_t = sc_uint<DATA_WIDTH>;
using vector_t = sc_uint<DATA_WIDTH*VECTOR_SIZE>; // 4 lanes of DATA_WIDTH each

SC_MODULE(TransformRegFile) {
    // -------- Ports --------
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};        // Active-low reset

    sc_in<sc_uint<2>> vrs_idx_i{"vrs_idx_i"};  // Read vector window index (0..2) => 3 windows * 4 = 12
    sc_in<sc_uint<4>> rd_idx_i{"rd_idx_i"};    // Write scalar index (0..11)
    sc_in<sc_uint<3>> shift_mode_i{"shift_mode_i"};  // Shift mode (0:K3,1:K5,2:K7)

    sc_in<bool> wen_i{"wen_i"};          // Scalar write enable
    sc_in<bool> shift_en_i{"shift_en_i"}; // Shift enable (mutually exclusive with wen_i, shift 優先)
    sc_in<bool> clear_i{"clear_i"};      // Synchronous clear (independent of rst_n)

    sc_in<element_t> rd_data_i{"rd_data_i"};   // Write data (scalar input) TODO: rename wdata_i 未改以避免外部破壞
    sc_out<vector_t> vrs_data_o{"vrs_data_o"};  // Read data (vector output, 4 scalars slice)

    // -------- Internal storage --------
    std::array<element_t, 12> T{};  // Scalar registers

    // 預先定義 shift mask (bit i==1 表示 T[i] <- T[i+1]，0 表示清零)，僅使用低 12 bits
    static constexpr uint16_t SHIFT_MASKS[3] = {
        0b0110'1101'1011, // K3 pattern
        0b0001'1110'1111, // K5 pattern
        0b0000'0011'1111  // K7 pattern
    };

    // -------- Processes --------
    SC_CTOR(TransformRegFile) {
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();

        SC_METHOD(read_proc);          // asynchronous (ESL) read window
        sensitive << vrs_idx_i;        // index change
        sensitive << clk.pos();        // update after writes on clock
        dont_initialize();
    }

private:
    void seq_proc() {
        if (!rst_n.read()) {
            for (auto &reg : T) reg = 0;
        } else if (clear_i.read()) {
            for (auto &reg : T) reg = 0;
        } else if (shift_en_i.read()) { // shift 優先於 write
            uint16_t maskBits = 0;
            auto mode = shift_mode_i.read().to_uint();
            if (mode < 3) maskBits = SHIFT_MASKS[mode];
            // Shift direction: T[i] takes from T[i+1] when mask bit=1, else cleared
            for (int i = 0; i < 11; ++i) {
                bool shiftEn = (maskBits >> i) & 0x1;
                T[i] = shiftEn ? T[i+1] : (element_t)0;
            }
            T[11] = 0; // 新進資料留待上層在後續 cycle 寫入
        } else if (wen_i.read()) {
            auto idx = rd_idx_i.read().to_uint();
            if (idx < T.size()) {
                T[idx] = rd_data_i.read();
            }
        }
    }

    void read_proc() {
        vector_t result = 0;
        auto win = vrs_idx_i.read().to_uint();
        if (win < 3) { // 0,1,2 -> base offset win*4
            int base = win * VECTOR_SIZE;
            for (int lane = 0; lane < VECTOR_SIZE; ++lane) {
                int idx = base + lane;
                if (idx < (int)T.size()) {
                    result.range((lane+1)*DATA_WIDTH -1, lane*DATA_WIDTH) = T[idx];
                }
            }
        }
        vrs_data_o.write(result);
    }
};

}} // namespace hybridacc::pe

#endif // HYBRIDACC_PE_TREGFILE_HPP