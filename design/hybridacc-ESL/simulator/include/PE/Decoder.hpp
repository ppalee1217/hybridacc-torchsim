#pragma once

#include "utils.hpp"
#include <systemc>
#include <array>
#include <cstdint>

namespace hybridacc {
namespace pe {

static inline int getOpcode(uint16_t w){ return (w>>1) & 0x3; }
static inline int getFunct2(uint16_t w){ return (w>>3) & 0x3; }
static inline int getFunc3(uint16_t w){ return (w>>13)&0x7; }
static inline int getFunc1(uint16_t w){ return (w>>12)&0x1; }
static inline int getPayload(uint16_t w){ return (w>>5)&0x7F; }

SC_MODULE(Decoder) {
public:
    sc_in<pe_inst_t> inst_in;
    sc_out<pe_decode_signals_t> decode_signals_out;

    // Methods
    SC_CTOR(Decoder):
        inst_in("inst_in"),
        decode_signals_out("decode_signals_out")
    {
        DEBUG_MSG("[Create] Decoder");
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
            .loop_break = false,
            .loop_end = false,
            .jump_en = false,
            .DL_setaddr = false,
            .DL_setlen = false,
            .DL_write_en = false,
            .DL_active = false,
            .DL_next = false,
            .rid3 = 0,
            .rid5 = 0,
            .pd_load = false,
            .tr_en = false,
            .tr_write = false,
            .tr_shift = false,
            .tr_clear_regs = false,
            .tr_use_vcounter = false,
            .tr_set_vcounter = false,
            .tr_clear_vcounter = false,
            .tr_incr_vcounter = false,
            .pli_plo_operation = false,
            .pr_write = false,
            .pr_mode = false,
            .pr_clear_regs = false,
            .pr_use_vcounter = false,
            .pr_set_vcounter = false,
            .pr_clear_vcounter = false,
            .pr_incr_vcounter = false,
            .vaddu_en = false,
            .vaddu_mode = 0
        };


        // parse instruction fields
        int opcode = getOpcode(w);
        int funct2 = getFunct2(w);
        int func3  = getFunc3(w);
        int func1  = getFunc1(w);
        int payload= getPayload(w);

        signals.inst = w;
        signals.func3 = func3;
        signals.loop_end = (w & 1); // loop end flag

        // HALT
        if(opcode==3 && funct2==3){  signals.halt = true; signals.loop_end = false; return;}

        // NOP
        if(opcode==2 && funct2==0){ signals.nop = true; return;}

        // DL.ADDR / DL.LEN
        if(opcode==0 && funct2==1){
            int bits6_1 = payload & 0x3F;
            int bit0 = (payload >> 6) & 0x1;
            int val = (func3<<7) | (bits6_1<<1) | bit0; // 10-bit
            signals.imm = val;
            if(func1==0) signals.DL_setaddr = true; else signals.DL_setlen = true;
            return;
        }

        // DL Loads / Broadcast Loads
        if(opcode==0 && funct2==2){
            int stride = (w>>10)&0x7;
            signals.imm = stride;
            signals.DL_active = true;
        }

        // DL Store (only SD simplified -> just advance base)
        if(opcode==0 && funct2==3 && func3==3){
            int stride = (w>>10)&0x7;
            signals.imm = stride;
            signals.DL_write_en = true;
            signals.DL_active = true;
            return;
        }

        // TSTORE / TSHIFT
        if(opcode==1 && funct2==0){
            if(func3==0){ // TSTORE trd
                int trd = (w>>5)&0xf; // bits 8:5
                signals.rid3 = trd;
                signals.tr_en = true;
                signals.tr_write = true;
                signals.pd_load = true;
            }
            else if(func3==1){ // TSHIFT k
                int code = (w>>10)&0x7; // kernel size code (0:K3 1:K5 2:K7)
                signals.imm = code;
                signals.tr_en = true;
                signals.tr_shift = true;
            }
            return;
        }

        // Arithmetic Group
        if(opcode==2 && funct2==1){
            signals.rid5 = (w>>5)&0x1F;
            signals.rid3 = (w>>10)&0x3;
            int pstride = (w>>5)&0x1F;
            int tstride = (w>>10)&0x3;
            switch(func3){
                case 0: // VMAC / VMACN : P[prd] += dot(VT, DMRV)
                case 2: { // VMUL / VMULN : VP64[prd] = VT * DMRV + VP64[prd]
                    signals.pr_mode = (func3==2) ? 1 : 0; // 0: scalar(VMAC), 1: vector64(VMUL)
                    signals.DL_next = func1; // N 變形: 允許下一個 DMA
                    signals.vaddu_en = true;
                    signals.vaddu_mode = (func3==2) ? 1 : 0; // 0: ACCUMUATE(VMAC), 1: ADD(VMUL)
                } break;
                case 1: // VMACR / VMACRN : P[psum_cnt] += dot(VT[vtid_cnt], DMRV)
                case 3: { // VMULR / VMULRN : VP[vpsum_cnt] += mul(VT[vtid_cnt], DMRV)
                    signals.pr_use_vcounter = true;
                    signals.tr_use_vcounter = true;
                    signals.tr_en = true;
                    signals.pr_en = true;
                    signals.pr_write = true;
                    signals.DL_next = func1; // N 變形: 允許下一個 DMA
                    signals.pr_mode = (func3==3) ? 1 : 0; // 0: scalar(VMAC), 1: vector64(VMUL)
                    signals.vaddu_en = true;
                    signals.vaddu_mode = (func3==3) ? 1 : 0; // 0: ACCUMUATE(VMAC), 1: ADD(VMUL)

                    if(pstride==31) signals.pr_clear_vcounter = true;
                    else signals.pr_incr_vcounter = true;

                    if(tstride==3) signals.tr_clear_vcounter = true;
                    else signals.tr_incr_vcounter = true;
                } break;
                case 4: { // VPSUM : PLO = PLI + P[psum_cnt]
                    signals.pli_plo_operation = true;
                    signals.pr_en = true;
                    signals.pr_mode = 1; // vector 64-bit
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1; // ADD
                } break;
                case 5: { // VPSUMR : PLO = PLI + P[psum_cnt] 並移動 psum_cnt
                    signals.pli_plo_operation = true;
                    signals.pr_use_vcounter = true;
                    signals.pr_en = true;
                    signals.pr_mode = 1; // vector 64-bit
                    signals.vaddu_en = true;
                    signals.vaddu_mode = 1; // ADD

                    if(pstride==31) signals.pr_clear_vcounter = true;
                    else signals.pr_incr_vcounter = true;
                } break;
                default: break;
            }
            return;
        }

        // SETRID
        if(opcode==2 && funct2==2){
            switch(func3){
                case 1: signals.pr_set_vcounter = true; break;          // SETRID.P
                case 2: signals.tr_set_vcounter = true; break;           // SETRID.T
                case 3: signals.pr_set_vcounter = true; signals.tr_set_vcounter = true; break; // SETRID.PT
                default: break;
            }
            return;
        }

         // CLEAR
        if(opcode==2 && funct2==3){
            switch(func3){
                case 0: signals.tr_clear_regs = true; break; // CLEAR.T
                case 1: signals.pr_clear_regs = true; break; // CLEAR.P
                default: break;
            }
            return;
        }

        // loop end is disabled for following instructions
        signals.loop_end = false;

        // JUMP (J)
        if(opcode==1 && funct2==2){
            int imm = ((func3 & 0x7)<<7) | (func1<<10) | (((w>>11)&1)) | ((payload & 0x3F)<<1);
            signals.imm = imm;
            signals.jump_en = true;
            return;
        }

        // LOOPIN / LOOPBREAK
        if(opcode==1 && funct2==3){
            if(func1==0){ // LOOPIN
                int lc = ((func3 &0x7)<<7) | (((w>>11)&1)) | ((payload & 0x3F)<<1);
                signals.imm = lc;
                signals.loop_in = true;
            } else { // LOOPBREAK
                signals.loop_break = true;
            }
            return;
        }

        // no matching instruction - NOP
        signals.nop = true;
        return;
    }
};

}; // namespace pe
}; // namespace hybridacc