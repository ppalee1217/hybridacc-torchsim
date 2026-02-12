#pragma once

#include "utils.hpp"
#include <systemc>
#include <array>
#include <cstdint>

using namespace sc_core;

namespace hybridacc {
namespace pe {

static inline int getOpcode(uint16_t w){ return (w >> 1) & 0x3; }
static inline int getFunct2(uint16_t w){ return (w >> 3) & 0x3; }
static inline int getFunc1(uint16_t w){ return (w >> 5) & 0x1; }
static inline int getPayload(uint16_t w){ return (w >> 6) & 0x3FF; }
static inline int getFunc3(uint16_t w){ return getPayload(w) & 0x7; }

SC_MODULE(Decoder) {
public:
    sc_in<pe_inst_t> inst_in;
    sc_out<pe_decode_signals_t> decode_signals_out;

    // Methods
    SC_CTOR(Decoder):
        inst_in("inst_in"),
        decode_signals_out("decode_signals_out")
    {
        DEBUG_MSG("[Create] Decoder", DEBUG_LEVEL_PE_COMPONENTS);
        SC_METHOD(combinational_process);
        sensitive << inst_in;
    }

    pe_decode_signals_t decode(pe_inst_t w) {
        pe_decode_signals_t s;
        _decode(w, s);
        return s;
    }


    void combinational_process() {
        if (!inst_in.read()) {
            decode_signals_out.write(pe_decode_signals_t{});
            return;
        }
        pe_decode_signals_t signals;
        _decode(inst_in.read(), signals);
        decode_signals_out.write(signals);
    }

    void _decode(pe_inst_t w, pe_decode_signals_t &signals) {
        signals = pe_decode_signals_t{
            .inst = 0,
            .halt = false,
            .nop = false,
            .func3 = 0,
            .imm = 0,
            .loop_in = false,
            .loop_end = false,
            .is_swap = false,
            .sys_sdma_act = false,
            .sys_sdma_rst = false,
            .sys_ldma_act = false,
            .sys_ldma_rst = false,
            .sys_rst_pid = false,
            .sys_rst_tid = false,
            .DMA_setaddr = false,
            .DMA_setlen = false,
            .DMA_setloop = false,
            .DMA_setmode = false,
            .LDMA_next = false,
            .DMA_is_sdma = false,
            .rid3 = 0,
            .rid5 = 0,
            .pd_load = false,
            .pd_load_v = false,
            .tr_en = false,
            .tr_write = false,
            .tr_write_v = false,
            .tr_shift = false,
            .tr_clear_regs = false,
            .tr_use_vcounter = false,
            .tr_incr_vcounter = false,
            .pli_plo_operation = false,
            .pr_write = false,
            .pr_mode = false,
            .pr_clear_regs = false,
            .pr_use_vcounter = false,
            .pr_incr_vcounter = false,
            .vaddu_en = false,
            .vaddu_mode = 0
        };


        // parse instruction fields
        int opcode = getOpcode(w);
        int funct2 = getFunct2(w);
        int func1  = getFunc1(w);
        int payload= getPayload(w);
        int func3  = payload & 0x7; // payload[2:0]

        signals.inst = w;
        signals.func3 = func3;
        signals.loop_end = (w & 1); // loop end flag

        // HALT (opcode=10 func2=11 func1=0)
        if (opcode == 2 && funct2 == 3 && func1 == 0) {
            signals.halt = true;
            signals.loop_end = false;
            return;
        }

        // SYS.SYNC (opcode=10 func2=01 func1=1)
        if (opcode == 2 && funct2 == 1 && func1 == 1) {
            signals.is_swap = (payload & 0x1) == 0x1;
            return;
        }

        // SYS.CTRL (opcode=10 func2=01 func1=0)
        if (opcode == 2 && funct2 == 1 && func1 == 0) {
            signals.sys_sdma_act = (payload >> 7) & 0x1;
            signals.sys_sdma_rst = (payload >> 6) & 0x1;
            signals.sys_ldma_act = (payload >> 5) & 0x1;
            signals.sys_ldma_rst = (payload >> 4) & 0x1;
            signals.sys_rst_pid = (payload >> 3) & 0x1;
            signals.sys_rst_tid = (payload >> 2) & 0x1;
            signals.tr_clear_regs = (payload >> 1) & 0x1; // CLEAR.T
            signals.pr_clear_regs = (payload >> 0) & 0x1; // CLEAR.P
            return;
        }

        // NOP (opcode=10 func2=10 func1=0)
        if (opcode == 2 && funct2 == 2 && func1 == 0) {
            signals.nop = true;
            return;
        }

        // LOOPIN (opcode=10 func2=00)
        if (opcode == 2 && funct2 == 0 && func1 == 0) {
            signals.imm = payload;
            signals.loop_in = true;
            return;
        }

        // LDMA/SDMA ADDR (opcode=00 func2=00)
        if (opcode == 0 && funct2 == 0) {
            signals.imm = payload;
            signals.DMA_setaddr = true;
            signals.DMA_is_sdma = (func1 != 0);
            return;
        }

        // LDMA/SDMA LEN (opcode=00 func2=01)
        if (opcode == 0 && funct2 == 1) {
            signals.imm = payload;
            signals.DMA_setlen = true;
            signals.DMA_is_sdma = (func1 != 0);
            return;
        }

        // LDMA/SDMA LOOP (opcode=00 func2=10)
        if (opcode == 0 && funct2 == 2) {
            signals.imm = payload;
            signals.DMA_setloop = true;
            signals.DMA_is_sdma = (func1 != 0);
            return;
        }

        // LDMA/SDMA MODE (opcode=00 func2=11 func1=0)
        if (opcode == 0 && funct2 == 3 && func1 == 0) {
            signals.imm = (payload >> 3) & 0x7; // stride
            signals.DMA_setmode = true;
            signals.DMA_is_sdma = (func3 == 0x7);
            return;
        }

        // DMA Ops / TSTORE / VTSTORE / TSHIFT (opcode=00 func2=11)
        if (opcode == 0 && funct2 == 3 && func1 == 1) {
            if (func3 == 0) { // TSTORE
                int trd = (payload >> 6) & 0xF;
                signals.rid3 = trd;
                signals.tr_en = true;
                signals.tr_write = true;
                signals.pd_load = true;
            } else if (func3 == 1) { // VTSTORE
                int vtrd = (payload >> 3) & 0x3;
                signals.rid3 = vtrd;
                signals.tr_en = true;
                signals.tr_write_v = true;
                signals.pd_load_v = true;
            } else if (func3 == 2) { // TSHIFT
                signals.imm = (payload >> 3) & 0x3;
                signals.tr_en = true;
                signals.tr_shift = true;
            }
            return;
        }

        // Arithmetic Group (opcode=01)
        if (opcode == 1) {
            const int reg5 = (payload >> 5) & 0x1F;
            const int vtbits = (payload >> 3) & 0x3;
            const int pstride = reg5;
            const int vtstride = vtbits;

            if (funct2 == 0) {
                if (func3 == 0) { // VMAC / VMACN
                    signals.rid5 = reg5;
                    signals.rid3 = vtbits;
                    signals.pr_en = true;
                    signals.pr_write = true;
                    signals.pr_mode = 0;
                    signals.tr_en = true;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 0;
                    signals.LDMA_next = func1;
                } else if (func3 == 1) { // VMACR / VMACRN
                    signals.rid5 = reg5;
                    signals.rid3 = vtbits;
                    signals.pr_use_vcounter = true;
                    signals.tr_use_vcounter = true;
                    signals.tr_en = true;
                    signals.pr_en = true;
                    signals.pr_write = true;
                    signals.pr_mode = 0;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 0;
                    signals.LDMA_next = func1;

                    if (pstride == 31) signals.sys_rst_pid = true;
                    else signals.pr_incr_vcounter = true;

                    if (vtstride == 3) signals.sys_rst_tid = true;
                    else signals.tr_incr_vcounter = true;
                }
                return;
            }

            if (funct2 == 1) {
                if (func3 == 0) { // VMUL / VMULN
                    signals.rid5 = reg5;
                    signals.rid3 = vtbits;
                    signals.pr_en = true;
                    signals.pr_write = true;
                    signals.pr_mode = 1;
                    signals.tr_en = true;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1;
                    signals.LDMA_next = func1;
                } else if (func3 == 1) { // VMULR / VMULRN
                    signals.rid5 = reg5;
                    signals.rid3 = vtbits;
                    signals.pr_use_vcounter = true;
                    signals.tr_use_vcounter = true;
                    signals.tr_en = true;
                    signals.pr_en = true;
                    signals.pr_write = true;
                    signals.pr_mode = 1;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1;
                    signals.LDMA_next = func1;

                    if (pstride == 31) signals.sys_rst_pid = true;
                    else signals.pr_incr_vcounter = true;

                    if (vtstride == 3) signals.sys_rst_tid = true;
                    else signals.tr_incr_vcounter = true;
                }
                return;
            }

            if (funct2 == 2) {
                if (func3 == 0) { // VPSUM
                    signals.rid5 = reg5;
                    signals.pli_plo_operation = true;
                    signals.pr_en = true;
                    signals.pr_mode = 1;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1;
                } else if (func3 == 1) { // VPSUMR
                    signals.rid5 = reg5;
                    signals.pr_use_vcounter = true;
                    signals.pli_plo_operation = true;
                    signals.pr_en = true;
                    signals.pr_mode = 1;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1;

                    if (pstride == 31) signals.sys_rst_pid = true;
                    else signals.pr_incr_vcounter = true;
                }
                return;
            }

            if (funct2 == 3) {
                if (func3 == 0 || func3 == 2) { // VPSUM (VPSUM_VTSTORE, VPSUM_TSHIFT)
                    // VPSUM
                    signals.rid5 = reg5;
                    signals.pli_plo_operation = true;
                    signals.pr_en = true;
                    signals.pr_mode = 1;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1;
                }

                if (func3 == 1 || func3 == 3) { // VPSUMR (VPSUMR_VTSTORE, VPSUMR_TSHIFT)
                    // VPSUMR
                    signals.rid5 = reg5;
                    signals.pr_use_vcounter = true;
                    signals.pli_plo_operation = true;
                    signals.pr_en = true;
                    signals.pr_mode = 1;
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1;
                    if (pstride == 31) signals.sys_rst_pid = true;
                    else signals.pr_incr_vcounter = true;
                }

                if (func3 == 0 || func3 == 1) { // VTSTORE (VPSUM_VTSTORE, VPSUMR_VTSTORE)
                    // VTSTORE
                    int vtrd = (payload >> 3) & 0x3;
                    signals.rid3 = vtrd;
                    signals.tr_en = true;
                    signals.tr_write_v = true;
                    signals.pd_load_v = true;
                }

                if (func3 == 2 || func3 == 3) { // TSHIFT (VPSUM_TSHIFT, VPSUMR_TSHIFT)
                    // TSHIFT
                    signals.imm = (payload >> 3) & 0x3;
                    signals.tr_en = true;
                    signals.tr_shift = true;
                }
                return;
            }
        }

        // loop end is disabled for following instructions
        signals.loop_end = false;

        // no matching instruction - NOP
        signals.nop = true;
        return;
    }
};

}; // namespace pe
}; // namespace hybridacc