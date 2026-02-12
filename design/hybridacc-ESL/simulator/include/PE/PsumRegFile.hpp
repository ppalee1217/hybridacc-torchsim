#pragma once

#include "utils.hpp"
#include <systemc>

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

// Psum Register File
SC_MODULE(PsumRegFile) {
    public:
        // Ports
        sc_in<bool> clk;
        sc_in<bool> reset_n;

        sc_in<bool> enable;
        sc_in<int> pid;
        sc_in<fp16_t> p_in;
        sc_in<v_fp16_t> vp_in;
        sc_in<bool> vpid_write_en;
        sc_in<int> mode; // 0: scalar, 1: vector 64-bit

        sc_out<fp16_t> p_out;
        sc_out<v_fp16_t> vp_out;

        sc_in<bool> clear_regs;
        sc_in<bool> use_pcounter;
        sc_in<bool> clear_pcounter;
        sc_in<bool> incr_pcounter;

        // Methods
        SC_CTOR(PsumRegFile)
            : clk("clk"),
              reset_n("reset_n"),
              enable("enable"),
              pid("pid"),
              p_in("p_in"),
              vp_in("vp_in"),
              vpid_write_en("vpid_write_en"),
              mode("mode"),
              p_out("p_out"),
              vp_out("vp_out"),
              clear_regs("clear_regs"),
              use_pcounter("use_pcounter"),
              clear_pcounter("clear_pcounter"),
              incr_pcounter("incr_pcounter")
        {
            DEBUG_MSG("[Create] PsumRegFile", DEBUG_LEVEL_PE_COMPONENTS);
            SC_CTHREAD(sequential_process, clk.pos());
            reset_signal_is(reset_n, false);
            SC_METHOD(combinational_process);
            sensitive << pid << write << mode << reset_n << pcounter << enable << use_pcounter;
        }


        sc_signal<int> pcounter;
        sc_signal<bool> write;

        std::array<fp16_t,32> P{};    // Partial sum scalar
        std::array<v_fp16_t, 24> VP64{}; // v_fp16_t partial sum (64-bit)

        void clear() {
            P.fill(0);
            for(auto &v: VP64){ v = v_fp16_t(); }
        }
        void reset() {
            clear();
            pcounter.write(0);
        }
        void setP(int pid, fp16_t v) { P[pid] = v; }
        fp16_t getP(int pid) const { return P[pid]; }
        void setVP64(int vpid, const v_fp16_t& v) {
            if (vpid < 8){
                for (int i = 0; i < 4; ++i) {
                    P[vpid * 4 + i] = v.lanes[i]; // 4 scalar registers for vector lanes
                }
            } else {
                VP64[vpid - 8] = v; // 24 vector registers
            }
        }
        v_fp16_t getVP64(int vpid) const {
            if (vpid < 8) {
                v_fp16_t v;
                for (int i = 0; i < 4; ++i) {
                    v.lanes[i] = P[vpid * 4 + i];
                }
                return v;
            } else {
                return VP64[vpid - 8];
            }
        }

        void sequential_process() {
            // Reset initialization
            clear();
            pcounter.write(0);
            write.write(false);
            wait();

            while (true) {
                // register update logic
                if (clear_regs.read()) {
                    clear();
                } else {
                    if (vpid_write_en.read()) {
                        write.write(!write.read());
                        int write_pid = (use_pcounter.read()) ? pcounter.read() : pid.read();
                        if (mode.read() == 0) { // scalar mode
                            DEBUG_MSG("[PsumRegFile] Write P[" << write_pid << "] = " << std::hex << p_in.read() << std::dec, DEBUG_LEVEL_PE_COMPONENTS);
                            setP(write_pid, p_in.read());
                        } else if (mode.read() == 1) { // vector 64-bit mode
                            DEBUG_MSG("[PsumRegFile] Write VP64[" << write_pid << "] = " << std::hex << vp_in.read() << std::dec, DEBUG_LEVEL_PE_COMPONENTS);
                            setVP64(write_pid, vp_in.read());
                        }
                    }
                }

                // pcounter update logic
                if (clear_pcounter.read()) {
                    DEBUG_MSG("[PsumRegFile] CLEAR pcounter: " << pcounter.read() << " -> 0", DEBUG_LEVEL_PE_COMPONENTS);
                    pcounter.write(0);
                } else if (incr_pcounter.read()) {
                    DEBUG_MSG("[PsumRegFile] INCR pcounter: " << pcounter.read() << " -> " << (pcounter.read() + pid.read()), DEBUG_LEVEL_PE_COMPONENTS);
                    pcounter.write(pcounter.read() + pid.read());
                }
                wait();
            }
        }

        void combinational_process() {
            if (!reset_n.read()) {
                p_out.write(0);
                vp_out.write(v_fp16_t());
                return;
            }

            if(!enable.read()) {
                p_out.write(0);
                vp_out.write(v_fp16_t());
                return;
            }

            int read_pid = use_pcounter.read() ? pcounter.read() : pid.read();

            if (mode.read() == 0){
                DEBUG_MSG("[PsumRegFile] Read P[" << read_pid << "] = " << std::hex << getP(read_pid) << std::dec, DEBUG_LEVEL_PE_COMPONENTS);
                p_out.write(getP(read_pid));  // 修正：添加缺少的參數
                vp_out.write(v_fp16_t()); // clear vector output in scalar mode
            } else if (mode.read() == 1){
                DEBUG_MSG("[PsumRegFile] Read VP64[" << read_pid << "] = " << std::hex << getVP64(read_pid) << std::dec, DEBUG_LEVEL_PE_COMPONENTS);
                p_out.write(0); // clear scalar output in vector mode
                vp_out.write(getVP64(read_pid));
            }
        }
    };

} // namespace pe
} // namespace hybridacc