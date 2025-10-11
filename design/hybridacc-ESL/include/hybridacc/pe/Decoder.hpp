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

SC_MODULE(Decoder) {
    // Inputs
    sc_in<sc_uint<INST_WIDTH>> inst_i{"inst_i"};

    // Generic decoded fields
    sc_out<bool>               loopend_o{"loopend_o"};      // bit0
    sc_out<sc_uint<2>>         opcode_o{"opcode_o"};        // [2:1]
    sc_out<sc_uint<2>>         funct2_o{"funct2_o"};        // [4:3]
    sc_out<bool>               func1_o{"func1_o"};          // [12]
    sc_out<sc_uint<3>>         func3_o{"func3_o"};          // [15:13]
    sc_out<bool>               b11_o{"b11_o"};              // [11]
    sc_out<sc_uint<6>>         payload6_o{"payload6_o"};    // [10:5]

    // Reconstructed immediates
    sc_out<sc_uint<10>>        imm10_o{"imm10_o"};          // 對 DMA.ADDR / DMA.LEN / LOOPIN 等
    sc_out<sc_uint<11>>        j_imm11_o{"j_imm11_o"};      // 對 J 指令

    // Frequently used operand slices
    sc_out<sc_uint<5>>         prd_o{"prd_o"};              // [9:5] (psum/scalar reg id or stride)
    sc_out<sc_uint<2>>         vtrs_o{"vtrs_o"};            // [11:10] (vector sel / vt stride)
    sc_out<sc_uint<3>>         trd_o{"trd_o"};              // [7:5]  (TSTORE 的 trd)
    sc_out<sc_uint<3>>         stride3_o{"stride3_o"};      // [12:10] (DMA L*/SD stride or TSHIFT kcode)

    // One-hot decode (major instructions)
    sc_out<bool> is_dma_addr_o{"is_dma_addr_o"};
    sc_out<bool> is_dma_len_o{"is_dma_len_o"};
    sc_out<bool> is_dma_ld_o{"is_dma_ld_o"};      // DMA.L* (以 funct3 區分種別，這裡僅提供總類)
    sc_out<bool> is_dma_sd_o{"is_dma_sd_o"};      // DMA.SD

    sc_out<bool> is_tstore_o{"is_tstore_o"};
    sc_out<bool> is_tshift_o{"is_tshift_o"};

    sc_out<bool> is_vmac_o{"is_vmac_o"};
    sc_out<bool> is_vmacn_o{"is_vmacn_o"};
    sc_out<bool> is_vmacr_o{"is_vmacr_o"};
    sc_out<bool> is_vmacrn_o{"is_vmacrn_o"};
    sc_out<bool> is_vmul_o{"is_vmul_o"};
    sc_out<bool> is_vmuln_o{"is_vmuln_o"};
    sc_out<bool> is_vmulr_o{"is_vmulr_o"};
    sc_out<bool> is_vmulrn_o{"is_vmulrn_o"};
    sc_out<bool> is_vpsum_o{"is_vpsum_o"};
    sc_out<bool> is_vpsumr_o{"is_vpsumr_o"};

    sc_out<bool> is_j_o{"is_j_o"};
    sc_out<bool> is_loopin_o{"is_loopin_o"};
    sc_out<bool> is_loopbreak_o{"is_loopbreak_o"};

    sc_out<bool> is_nop_o{"is_nop_o"};
    sc_out<bool> is_setrid_p_o{"is_setrid_p_o"};
    sc_out<bool> is_setrid_t_o{"is_setrid_t_o"};
    sc_out<bool> is_setrid_pt_o{"is_setrid_pt_o"};
    sc_out<bool> is_clear_t_o{"is_clear_t_o"};
    sc_out<bool> is_clear_p_o{"is_clear_p_o"};
    sc_out<bool> is_halt_o{"is_halt_o"};

    SC_CTOR(Decoder) {
        SC_METHOD(comb_circuit);
        sensitive << inst_i;
        dont_initialize();
    }

private:
    void comb_circuit() {
        sc_uint<16> w = inst_i.read();

        // Base fields
        bool le    = (w & 0x1);
        sc_uint<2> opcode = (w >> 1) & 0x3;    // [2:1]
        sc_uint<2> f2     = (w >> 3) & 0x3;    // [4:3]
        sc_uint<6> pay6   = (w >> 5) & 0x3F;   // [10:5]
        bool      b11     = ((w >> 11) & 0x1);
        bool      f1      = ((w >> 12) & 0x1);
        sc_uint<3> f3     = (w >> 13) & 0x7;   // [15:13]

        // Write basic fields
        loopend_o.write(le);
        opcode_o.write(opcode);
        funct2_o.write(f2);
        func1_o.write(f1);
        func3_o.write(f3);
        b11_o.write(b11);
        payload6_o.write(pay6);

        // Common operand slices
        sc_uint<5> prd = (w >> 5) & 0x1F;     // [9:5]
        sc_uint<2> vtrs = (w >> 10) & 0x3;    // [11:10]
        sc_uint<3> trd  = (w >> 5) & 0x7;     // [7:5]
        sc_uint<3> stride3 = ((w >> 10) & 0x7); // [12:10] = {f1,b11,bit10}
        prd_o.write(prd);
        vtrs_o.write(vtrs);
        trd_o.write(trd);
        stride3_o.write(stride3);

        // Reconstruct immediates
        // 10-bit compressed: value[9:7]->f3, value[0]->b11, value[6:1]->pay6
        sc_uint<10> imm10 = ( (sc_uint<10>)(f3) << 7 ) | ( (sc_uint<10>)(pay6) << 1 ) | (sc_uint<10>)(b11 ? 1 : 0);
        imm10_o.write(imm10);
        // 11-bit J immediate: (9:7|10|0|6:1) = (f3|f1|b11|pay6)
        sc_uint<11> jimm = ( (sc_uint<11>)(f3) << 8 ) | ( (sc_uint<11>)(f1 ? 1:0) << 7 )
                          | ( (sc_uint<11>)(b11 ? 1:0) << 6 ) | (sc_uint<11>)(pay6);
        j_imm11_o.write(jimm);

        // Default clear all one-hot outputs
        is_dma_addr_o = false; is_dma_len_o = false; is_dma_ld_o = false; is_dma_sd_o = false;
        is_tstore_o = false; is_tshift_o = false;
        is_vmac_o = false; is_vmacn_o = false; is_vmacr_o = false; is_vmacrn_o = false;
        is_vmul_o = false; is_vmuln_o = false; is_vmulr_o = false; is_vmulrn_o = false;
        is_vpsum_o = false; is_vpsumr_o = false;
        is_j_o = false; is_loopin_o = false; is_loopbreak_o = false;
        is_nop_o = false; is_setrid_p_o = false; is_setrid_t_o = false; is_setrid_pt_o = false;
        is_clear_t_o = false; is_clear_p_o = false; is_halt_o = false;

        // Start decoding by classes
        // Data Movement
        // DMA.ADDR/LEN: opcode=00 f2=01 f1=0/1 (value => imm10)
        if (opcode == 0x0 && f2 == 0x1) {
            if (!f1) is_dma_addr_o = true; else is_dma_len_o = true;
        }
        // DMA.L*/SD: opcode=00 f2=11, func3 區分，stride -> [12:10]
        if (opcode == 0x0 && f2 == 0x3) {
            // func3=011 => SD；其他 (000..010,100..111) 為 L* 類 (依專案自定)
            if (f3 == 0x3) is_dma_sd_o = true; else is_dma_ld_o = true;
        }

        // TSTORE/TSHIFT: opcode=01 f2=00 func3=000/001
        if (opcode == 0x1 && f2 == 0x0) {
            if (f3 == 0x0) is_tstore_o = true;
            else if (f3 == 0x1) is_tshift_o = true;
        }

        // Arithmetic: opcode=10 f2=01
        if (opcode == 0x2 && f2 == 0x1) {
            switch (f3.to_uint()) {
                case 0x0: // VMAC / VMACN by func1
                    if (f1) is_vmacn_o = true; else is_vmac_o = true;
                    break;
                case 0x1: // VMACR / VMACRN
                    if (f1) is_vmacrn_o = true; else is_vmacr_o = true;
                    break;
                case 0x2: // VMUL / VMULN
                    if (f1) is_vmuln_o = true; else is_vmul_o = true;
                    break;
                case 0x3: // VMULR / VMULRN
                    if (f1) is_vmulrn_o = true; else is_vmulr_o = true;
                    break;
                case 0x4: // VPSUM (func1=0)
                    if (!f1) is_vpsum_o = true; break;
                case 0x5: // VPSUMR (func1=0)
                    if (!f1) is_vpsumr_o = true; break;
                default:
                    break;
            }
        }

        // Control
        // J: opcode=01 f2=10
        if (opcode == 0x1 && f2 == 0x2) {
            is_j_o = true;
        }
        // LOOPIN/LOOPBREAK: opcode=01 f2=11 f1=0/1
        if (opcode == 0x1 && f2 == 0x3) {
            if (!f1) is_loopin_o = true; else is_loopbreak_o = true;
        }

        // System
        // NOP: opcode=10 f2=00
        if (opcode == 0x2 && f2 == 0x0) {
            is_nop_o = true;
        }
        // SETRID / CLEAR / HALT (根據文件簡化判斷)
        // SETRID.*: opcode=10 f2=10 func3=001/010/011 -> P/T/PT
        if (opcode == 0x2 && f2 == 0x2) {
            if (f3 == 0x1) is_setrid_p_o = true;
            else if (f3 == 0x2) is_setrid_t_o = true;
            else if (f3 == 0x3) is_setrid_pt_o = true;
        }
        // CLEAR.*: opcode=10 f2=11 func3=000/001 -> T/P
        if (opcode == 0x2 && f2 == 0x3) {
            if (f3 == 0x0) is_clear_t_o = true;
            else if (f3 == 0x1) is_clear_p_o = true;
        }
        // HALT: opcode=11 f2=11
        if (opcode == 0x3 && f2 == 0x3) {
            is_halt_o = true;
        }
    }
};

}} // namespace hybridacc::pe

#endif // HYBRIDACC_PE_DECODER_HPP
