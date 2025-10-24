#ifndef HYBRIDACC_PE_DECODER_HPP
#define HYBRIDACC_PE_DECODER_HPP

#include <systemc.h>
#include <cstdint>
#include "hybridacc/utils.hpp"

// ISA 參考: design/hybridacc-isa/doc/ISA.md
// 16-bit 指令格式:
//  bit0  : LOOPEND 標記 (LE)
//  bits2:1  opcode
//  bits4:3  funct2
//  bits11:5 payload 6-bit (10:5) + b11(=bit11 單獨取出)
//  bit12    func1
//  bits15:13 func3
// 近期修訂: DMA.ADDR/LEN 為共用壓縮 10-bit 值 (value[9:7]->func3, value[0]->b11, value[6:1]->[10:5])；
// J 使用 11-bit 值 (9:7|10|0|6:1 對應 func3|func1|b11|[10:5])。

namespace hybridacc { namespace pe {

typedef struct{
    bool               loopend;      // bit0
    sc_uint<2>         opcode;        // [2:1]
    sc_uint<2>         funct2;        // [4:3]
    bool               func1;          // [12]
    sc_uint<3>         func3;          // [15:13], dma_mode
    bool               b11;              // [11]
    sc_uint<6>         payload6;    // [10:5]

    sc_uint<10>        imm10;          // 對 DMA.ADDR / DMA.LEN / LOOPIN 等
    sc_uint<11>        j_imm11;      // 對 J 指令

    sc_uint<5>         prd;              // [9:5] (psum/scalar reg id or stride)
    sc_uint<2>         vtrs;            // [11:10] (vector sel / vt stride)
    sc_uint<3>         trd;              // [7:5]  (TSTORE 的 trd)
    sc_uint<3>         stride3;      // [12:10] (DMA L*/SD stride or TSHIFT kcode)

    // One-hot DataLoader
    bool dmloader_set_addr;
    bool dmloader_set_len;

    bool dmloader_wen;          // 1 => store (DMA.SD) 模式
    bool dmloader_activate;
    bool dmloader_next;

    // One-hot Loopstack
    bool loop_push;
    bool loop_pop;
    bool jump;

    // One-hot Tregfile
    bool idx_use_tcounter;
    bool treg_clear;
    bool treg_shift_en;
    bool treg_wen;

    // One-hot Tcounter
    bool tcounter_clear;
    bool tcounter_inc; // tcounter increment
    bool tcounter_set; // tcounter set to payload6

    // One-hot Pregfile
    bool idx_use_pcounter;
    bool preg_wen;
    bool preg_clear;
    bool preg_mode;

    // One-hot Pcounter
    bool pcounter_clear;
    bool pcounter_inc; // pcounter
    bool pcounter_set; // pcounter set to payload6

    // One-hot VADD
    bool vadd_mode;

    // One-hot HALT
    bool halt;

} DECODED_FIELDS;

SC_MODULE(Decoder) {
    // Inputs
    sc_in<sc_uint<INST_WIDTH>> inst_i{"inst_i"};

    // Outputs
    sc_out<DECODED_FIELDS>  decoded_o{"decoded_o"};

    SC_CTOR(Decoder) {
        SC_METHOD(comb_circuit);
        sensitive << inst_i;
        dont_initialize();
    }

private:
    DECODED_FIELDS decoded;
    void comb_circuit() {
        sc_uint<16> w = inst_i.read(); // 16-bit instruction

        // Extract basic fields
        decoded.loopend = w[0];
        decoded.opcode = w.range(2, 1);
        decoded.funct2 = w.range(4, 3);
        decoded.payload6 = w.range(10, 5);
        decoded.b11 = w[11];
        decoded.func1 = w[12];
        decoded.func3 = w.range(15, 13);

        // Reconstruct 10-bit immediate (for DMA.ADDR/LEN/LOOPIN)
        // Format: func3[2:0] -> bits[9:7], b11 -> bit[0], payload6[5:0] -> bits[6:1]
        decoded.imm10 = (decoded.func3 << 7) | (decoded.payload6 << 1) | decoded.b11;

        // Reconstruct 11-bit immediate for J instruction
        // Format: func3[2:0] -> bits[9:7], func1 -> bit[10], b11 -> bit[0], payload6[5:0] -> bits[6:1]
        decoded.j_imm11 = (decoded.func3 << 7) | (decoded.func1 << 10) | (decoded.payload6 << 1) | decoded.b11;

        // Extract frequently used operand slices
        decoded.prd = w.range(9, 5);           // psum/scalar reg id
        decoded.vtrs = w.range(11, 10);        // vector sel / vt stride
        decoded.trd = w.range(7, 5);           // TSTORE trd
        decoded.stride3 = w.range(12, 10);     // 3-bit stride

        // Initialize all control signals to false
        decoded.dmloader_set_addr = false;
        decoded.dmloader_set_len = false;
        decoded.dmloader_wen = false;
        decoded.dmloader_activate = false;
        decoded.dmloader_next = false;

        decoded.loop_push = false;
        decoded.loop_pop = false;
        decoded.jump = false;

        decoded.treg_clear = false;
        decoded.treg_shift_en = false;
        decoded.treg_wen = false;

        decoded.idx_use_tcounter = false;
        decoded.tcounter_clear = false;
        decoded.tcounter_inc = false;
        decoded.tcounter_set = false;

        decoded.preg_wen = false;
        decoded.preg_clear = false;
        decoded.preg_mode = false;

        decoded.idx_use_pcounter = false;
        decoded.pcounter_clear = false;
        decoded.pcounter_inc = false;
        decoded.pcounter_set = false;

        decoded.vadd_mode = false;
        decoded.halt = false;

        // Decode instructions based on opcode and funct2
        switch (decoded.opcode) {
            case 0b00: // Data Movement
                if (decoded.funct2 == 0b01) {
                    // DMA.ADDR/LEN (func1=0: ADDR, func1=1: LEN)
                    if (decoded.func1 == 0) {
                        decoded.dmloader_set_addr = true;
                    } else {
                        decoded.dmloader_set_len = true;
                    }
                } else if (decoded.funct2 == 0b11) {
                    // DMA.L*/SD operations
                    decoded.dmloader_activate = true;
                    if (decoded.func3 == 0b011) {
                        // DMA.SD - store mode
                        decoded.dmloader_wen = true;
                    }
                    // For VMAC*N, VMUL*N instructions with next DMA
                    decoded.dmloader_next = true;
                }
                break;

            case 0b01: // Control/System
                if (decoded.funct2 == 0b00) {
                    if (decoded.func3 == 0b000) {
                        // TSTORE
                        decoded.treg_wen = true;
                    } else if (decoded.func3 == 0b001) {
                        // TSHIFT
                        decoded.treg_shift_en = true;
                    }
                } else if (decoded.funct2 == 0b10) {
                    // J (jump)
                    decoded.jump = true;
                } else if (decoded.funct2 == 0b11) {
                    if (decoded.func1 == 0) {
                        // LOOPIN
                        decoded.loop_push = true;
                    } else {
                        // LOOPBREAK
                        decoded.loop_pop = true;
                    }
                }
                break;

            case 0b10: // Arithmetic/System
                if (decoded.funct2 == 0b00) {
                    // NOP - no operation needed
                } else if (decoded.funct2 == 0b01) {
                    // Arithmetic operations (VMAC, VMUL, VPSUM, etc.)
                    switch (decoded.func3) {
                        case 0b000: // VMAC/VMACN
                        case 0b001: // VMACR/VMACRN
                        case 0b010: // VMUL/VMULN
                        case 0b011: // VMULR/VMULRN
                            // Enable processing element operations
                            decoded.preg_wen = true;
                            if (decoded.func1 == 1) {
                                // *N variants - next DMA
                                decoded.dmloader_next = true;
                            }
                            if (decoded.func3 == 0b001 || decoded.func3 == 0b011) {
                                // *R variants - use tcounter and pcounter indices
                                decoded.idx_use_tcounter = true;
                                decoded.idx_use_pcounter = true;
                            }
                            break;
                        case 0b100: // VPSUM
                        case 0b101: // VPSUMR
                            decoded.preg_wen = true;
                            decoded.vadd_mode = true;
                            if (decoded.func3 == 0b101) {
                                // VPSUMR - use pcounter indices (only Pregfile is used)
                                decoded.idx_use_pcounter = true;
                            }
                            break;
                    }
                } else if (decoded.funct2 == 0b10) {
                    // SETRID operations
                    switch (decoded.func3) {
                        case 0b001: // SETRID.P
                            decoded.pcounter_set = true;
                            break;
                        case 0b010: // SETRID.T
                            decoded.tcounter_set = true;
                            break;
                        case 0b011: // SETRID.PT
                            decoded.pcounter_set = true;
                            decoded.tcounter_set = true;
                            break;
                    }
                } else if (decoded.funct2 == 0b11) {
                    // CLEAR operations
                    switch (decoded.func3) {
                        case 0b000: // CLEAR.T
                            decoded.treg_clear = true;
                            decoded.tcounter_clear = true;
                            break;
                        case 0b001: // CLEAR.P
                            decoded.preg_clear = true;
                            decoded.pcounter_clear = true;
                            break;
                    }
                }
                break;

            case 0b11: // System
                if (decoded.funct2 == 0b11) {
                    // HALT - implementation dependent
                    decoded.halt = true;
                }
                break;
        }

        // Output the complete decoded structure
        decoded_o.write(decoded);
    }
};

}} // namespace hybridacc::pe

#endif // HYBRIDACC_PE_DECODER_HPP
