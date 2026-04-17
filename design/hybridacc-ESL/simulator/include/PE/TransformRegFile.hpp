#pragma once

#include "Utils/utils.hpp"
#include <systemc>

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

// Transform RegFile
SC_MODULE(TransformRegFile) {
    public:
        // Ports
        sc_in<bool> clk;
        sc_in<bool> reset_n;

        sc_in<int> enable;
        sc_in<bool> shift_en;
        sc_in<int> shift_mode;

        sc_in<int> tid;
        sc_in<fp16_t> tid_in;
        sc_in<bool> tid_write_en;
        sc_in<v_fp16_t> vtid_in;
        sc_in<bool> vtid_write_en;
        sc_out<v_fp16_t> vtid_out;

        sc_in<bool> clear_regs;
        sc_in<bool> use_vcounter;
        sc_in<bool> clear_vcounter;
        sc_in<bool> incr_vcounter;

        // Constructor
        SC_CTOR(TransformRegFile)
            : clk("clk"),
              reset_n("reset_n"),
              enable("enable"),
              shift_en("shift_en"),
              shift_mode("shift_mode"),
              tid("tid"),
              tid_in("tid_in"),
              tid_write_en("tid_write_en"),
              vtid_in("vtid_in"),
              vtid_write_en("vtid_write_en"),
              vtid_out("vtid_out"),
              clear_regs("clear_regs"),
              use_vcounter("use_vcounter"),
              clear_vcounter("clear_vcounter"),
              incr_vcounter("incr_vcounter"),
              vcounter(0)
        {
            DEBUG_MSG("[Create] TransformRegFile", DEBUG_LEVEL_PE_COMPONENTS);
            SC_CTHREAD(sequential_process, clk.pos());
            reset_signal_is(reset_n, false);
            SC_METHOD(combinational_process);
            sensitive << tid << tid_write_en << tid_in << shift_en << shift_mode << reset_n << vcounter << vtid_in << vtid_write_en;
        }



        sc_signal<int> vcounter;
        void clear() { reg.fill(0); }
        void reset() { clear(); vcounter.write(0); }  // 修正：使用 vcounter 而不是未定義的變數
        void setT(int tid, fp16_t v) {
            DEBUG_MSG("[TransformRegFile] Setting T for TID " << tid << " with value " << v, DEBUG_LEVEL_PE_COMPONENTS);
            reg[tid] = v;
        }
        void setVT(int tid, v_fp16_t v) {
            DEBUG_MSG("[TransformRegFile] Setting VT for TID " << tid << " with value " << v, DEBUG_LEVEL_PE_COMPONENTS);
            for (int i = 0; i < 4; ++i) {
                reg[tid + i * 3] = v.lanes[i];
            }
        }
        v_fp16_t getVT(int tid) const {
            v_fp16_t vt;
            for (int i = 0; i < 4; ++i) {
                vt.lanes[i] = reg[tid + i * 3];
            }
            return vt;
        }
        void shift(int shift_mode){  // 修正：移除多餘的類名限定符
            DEBUG_MSG("[TransformRegFile] Shift operation with mode " << shift_mode, DEBUG_LEVEL_PE_COMPONENTS);
            int maskBits = 0;
            switch(shift_mode){
                case 0: // K3 -> 011011011011b (11 ~ 0)
                    maskBits = 0b011011011011; break;
                case 1: // K5 -> 001111001111b (11 ~ 0)
                    maskBits = 0b001111001111; break;
                case 2: // K7 -> 000000111111b (11 ~ 0)
                    maskBits = 0b000000111111; break;
                default: maskBits = 0; break; // 未定義 code 清空
            }

            for(int i=0;i<11;++i){
                bool shiftEn = (maskBits >> i) & 1;
                if(shiftEn){
                    reg[i] = reg[i+1]; // 往左移，T[i] <- T[i+1]
                } else {
                    reg[i] = 0; // 清零
                }
            }
            reg[11] = 0; // 最後一個寄存器清零
        }
        std::array<fp16_t, 12> reg{}; // 4x3 = 12 registers (3 for each vector lane)

        void sequential_process() {
            // Reset initialization
            clear();
            vcounter.write(0);
            wait();

            while (true) {
                if (clear_regs.read()) {
                    clear();
                } else if (enable.read()) {
                    if (shift_en.read()) {
                        shift(shift_mode.read());
                    } else if (tid_write_en.read()) {
                        setT(tid.read(), tid_in.read());
                    } else if (vtid_write_en.read()) {
                        setVT(tid.read(), vtid_in.read());
                    }
                }

                if (clear_vcounter.read()) {
                    vcounter.write(0);
                } else if (enable.read() && incr_vcounter.read()) {
                    vcounter.write(vcounter.read() + tid.read());
                }
                wait();
            }
        }
        void combinational_process() {
            if (!reset_n.read()) {
                vtid_out.write(v_fp16_t());
                return;
            }
            if (!enable.read()) {
                vtid_out.write(v_fp16_t());
                return;
            }

            if (use_vcounter.read()) {
                vtid_out.write(getVT(vcounter.read()));
            } else {
                vtid_out.write(getVT(tid.read()));
            }
        }
    };


} // namespace pe
} // namespace hybridacc