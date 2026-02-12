#pragma once

#include "utils.hpp"
#include <systemc>
#include <array>

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

// -----------------------------------------------------------------------------
enum class LDMARequestType {
    LOAD_BYTE, // 8-bit load
    LOAD_HALF, // 16-bit load
    LOAD_WORD, // 32-bit load
    LOAD_DWORD, // 64-bit load
};

// Add operator<< support for LDMARequestType
inline std::ostream& operator<<(std::ostream& os, LDMARequestType type) {
    switch (type) {
        case LDMARequestType::LOAD_BYTE: return os << "LOAD_BYTE";
        case LDMARequestType::LOAD_HALF: return os << "LOAD_HALF";
        case LDMARequestType::LOAD_WORD: return os << "LOAD_WORD";
        case LDMARequestType::LOAD_DWORD: return os << "LOAD_DWORD";
        default: return os << "UNKNOWN";
    }
}

// sc_trace for LDMARequestType
inline void sc_trace(sc_core::sc_trace_file* tf, const LDMARequestType& type, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(type), name);
}

// -----------------------------------------------------------------------------
// State machine
enum class LDMAState {
    IDLE,
    LOAD_PRE,
    LOAD_WAIT,
    LOAD_PIPELINE,  // 新增: 流水線讀取狀態
    DONE
};

// Add operator<< support for LDMAState
inline std::ostream& operator<<(std::ostream& os, LDMAState state) {
    switch (state) {
        case LDMAState::IDLE: return os << "IDLE";
        case LDMAState::LOAD_PRE: return os << "LOAD_PRE";
        case LDMAState::LOAD_WAIT: return os << "LOAD_WAIT";
        case LDMAState::LOAD_PIPELINE: return os << "LOAD_PIPELINE";
        case LDMAState::DONE: return os << "DONE";
        default: return os << "UNKNOWN";
    }
}

// sc_trace for LDMAState
inline void sc_trace(sc_core::sc_trace_file* tf, const LDMAState& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}

// -----------------------------------------------------------------------------
SC_MODULE(LDMA) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // control ports
    sc_in<uint16_t> imm;
    sc_in<bool> set_addr;
    sc_in<bool> set_len;
    sc_in<bool> set_loop;
    sc_in<bool> set_mode;
    sc_in<uint16_t> mode;

    sc_in<bool> active;
    sc_in<bool> next;
    sc_in<bool> reset_active;

    sc_out<bool> busy;
    sc_out<bool> done;
    sc_out<bool> dl_stall_out;

    // Data Memory interface
    sc_out<uint16_t> dm_read_addr;
    sc_in<uint64_t> dm_read_data;

    // dmrv
    sc_out<v_fp16_t> dmrv_out;

    // Constructor
    SC_CTOR(LDMA)
        : clk("clk"),
          reset_n("reset_n"),
          imm("imm"),
          set_addr("set_addr"),
          set_len("set_len"),
          set_loop("set_loop"),
          set_mode("set_mode"),
          mode("mode"),
          active("active"),
          next("next"),
          reset_active("reset_active"),
          busy("busy"),
          done("done"),
          dl_stall_out("dl_stall_out"),
          dm_read_addr("dm_read_addr"),
          dm_read_data("dm_read_data"),
          dmrv_out("dmrv_out")
    {
        DEBUG_MSG("[Create] LDMA", DEBUG_LEVEL_PE_COMPONENTS);

        // 初始化暫存器
        state_reg.write(LDMAState::IDLE);
        dmrv_reg.write(v_fp16_t());
        dmwv_reg.write(v_fp16_t());
        dma_base_static_reg.write(0);
        dma_len_static_reg.write(0);
        dma_stride_static_reg.write(0);
        dma_loop_static_reg.write(0);
        dma_broadcast_static_reg.write(false);
        request_type_static_reg.write(LDMARequestType::LOAD_DWORD);
        dma_base_reg.write(0);
        dma_offset_reg.write(0);
        dma_len_reg.write(0);
        dma_stride_reg.write(0);
        dma_broadcast_reg.write(false);
        request_type_reg.write(LDMARequestType::LOAD_DWORD);
        dma_loop_reg.write(0);

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);

        // 拆分成多個組合邏輯方法
        SC_METHOD(next_state_logic);
        sensitive << state_reg << active << next << set_addr << set_len << set_loop << set_mode << reset_active
                << dm_read_data
                << imm << mode
              << dma_base_static_reg << dma_len_static_reg << dma_stride_static_reg << dma_loop_static_reg
              << dma_broadcast_static_reg << request_type_static_reg
              << dma_base_reg << dma_offset_reg << dma_len_reg << dma_stride_reg
              << dma_broadcast_reg << request_type_reg << dmwv_reg << dmrv_reg
              << dma_loop_reg;

        SC_METHOD(output_logic);
        sensitive << state_reg << dmrv_reg << dmwv_reg
              << dma_base_reg << dma_offset_reg
              << dma_offset_next << next
              << active << dma_base_static_reg << dma_len_static_reg;
    }

    // Registers (current state)
    sc_signal<LDMAState> state_reg;
    sc_signal<v_fp16_t> dmrv_reg;
    sc_signal<v_fp16_t> dmwv_reg;

    // Static configuration registers (programmed by LDMA.ADDR/LEN/LOOP and LDMA.L*)
    sc_signal<uint16_t> dma_base_static_reg;
    sc_signal<uint16_t> dma_len_static_reg;
    sc_signal<uint16_t> dma_stride_static_reg;
    sc_signal<uint16_t> dma_loop_static_reg;
    sc_signal<bool> dma_broadcast_static_reg;
    sc_signal<LDMARequestType> request_type_static_reg;

    // Active runtime registers
    sc_signal<uint16_t> dma_base_reg;
    sc_signal<uint16_t> dma_offset_reg;
    sc_signal<uint16_t> dma_len_reg;
    sc_signal<uint16_t> dma_stride_reg;
    sc_signal<bool> dma_broadcast_reg;
    sc_signal<LDMARequestType> request_type_reg;
    sc_signal<uint16_t> dma_loop_reg;

    // Next state (combinational) - 改為 signal
    sc_signal<LDMAState> state_next;
    sc_signal<v_fp16_t> dmrv_next;
    sc_signal<v_fp16_t> dmwv_next;
    sc_signal<uint16_t> dma_base_static_next;
    sc_signal<uint16_t> dma_len_static_next;
    sc_signal<uint16_t> dma_stride_static_next;
    sc_signal<uint16_t> dma_loop_static_next;
    sc_signal<bool> dma_broadcast_static_next;
    sc_signal<LDMARequestType> request_type_static_next;

    sc_signal<uint16_t> dma_base_next;
    sc_signal<uint16_t> dma_offset_next;
    sc_signal<uint16_t> dma_len_next;
    sc_signal<uint16_t> dma_stride_next;
    sc_signal<bool> dma_broadcast_next;
    sc_signal<LDMARequestType> request_type_next;
    sc_signal<uint16_t> dma_loop_next;

    uint64_t mask_and_broadcast(uint64_t v, LDMARequestType request_type, bool dma_broadcast) {
        uint64_t broadcast_v = 0;
        switch (request_type) {
            case LDMARequestType::LOAD_BYTE:
                v = v & 0xFF;
                if(dma_broadcast) {
                    for(int i = 0; i < 4; ++i) {
                        broadcast_v |= v << (i * 16);
                    }
                    v = broadcast_v;
                }
                break;
            case LDMARequestType::LOAD_HALF:
                v = v & 0xFFFF;
                if(dma_broadcast) {
                    for(int i = 0; i < 4; ++i) {
                        broadcast_v |= v << (i * 16);
                    }
                    v = broadcast_v;
                }
                break;
            case LDMARequestType::LOAD_WORD:
                v = v & 0xFFFFFFFF;
                if(dma_broadcast) {
                    for(int i = 0; i < 2; ++i) {
                        broadcast_v |= v << (i * 32);
                    }
                    v = broadcast_v;
                }
                break;
            case LDMARequestType::LOAD_DWORD:
                break;
            default:
                break;
        }
        return v;
    }

    // SystemC processes
    void sequential_process() {
        // Reset initialization
        state_reg.write(LDMAState::IDLE);
        dmrv_reg.write(v_fp16_t());
        dmwv_reg.write(v_fp16_t());
        dma_base_static_reg.write(0);
        dma_len_static_reg.write(0);
        dma_stride_static_reg.write(0);
        dma_loop_static_reg.write(0);
        dma_broadcast_static_reg.write(false);
        request_type_static_reg.write(LDMARequestType::LOAD_DWORD);
        dma_base_reg.write(0);
        dma_offset_reg.write(0);
        dma_len_reg.write(0);
        dma_stride_reg.write(0);
        dma_broadcast_reg.write(false);
        request_type_reg.write(LDMARequestType::LOAD_DWORD);
        dma_loop_reg.write(0);
        wait();

        while (true) {
            // Update all registers with next values
            state_reg.write(state_next.read());
            dmrv_reg.write(dmrv_next.read());
            dmwv_reg.write(dmwv_next.read());
            dma_base_static_reg.write(dma_base_static_next.read());
            dma_len_static_reg.write(dma_len_static_next.read());
            dma_stride_static_reg.write(dma_stride_static_next.read());
            dma_loop_static_reg.write(dma_loop_static_next.read());
            dma_broadcast_static_reg.write(dma_broadcast_static_next.read());
            request_type_static_reg.write(request_type_static_next.read());
            dma_base_reg.write(dma_base_next.read());
            dma_offset_reg.write(dma_offset_next.read());
            dma_len_reg.write(dma_len_next.read());
            dma_stride_reg.write(dma_stride_next.read());
            dma_broadcast_reg.write(dma_broadcast_next.read());
            request_type_reg.write(request_type_next.read());
            dma_loop_reg.write(dma_loop_next.read());

            DEBUG_MSG("[LDMA] State=" << state_reg.read()
                      << " len=" << dma_len_reg.read()
                      << " dmrv=" << dmrv_out.read()
                      << " addr=" << dm_read_addr.read()
                      << " next=" << next.read()
                      , DEBUG_LEVEL_PE_COMPONENTS);

            wait();
        }
    }

    void next_state_logic() {
        // Default: hold current values
        state_next.write(state_reg.read());
        dmrv_next.write(dmrv_reg.read());
        dmwv_next.write(dmwv_reg.read());
        dma_base_static_next.write(dma_base_static_reg.read());
        dma_len_static_next.write(dma_len_static_reg.read());
        dma_stride_static_next.write(dma_stride_static_reg.read());
        dma_loop_static_next.write(dma_loop_static_reg.read());
        dma_broadcast_static_next.write(dma_broadcast_static_reg.read());
        request_type_static_next.write(request_type_static_reg.read());
        dma_base_next.write(dma_base_reg.read());
        dma_offset_next.write(dma_offset_reg.read());
        dma_len_next.write(dma_len_reg.read());
        dma_stride_next.write(dma_stride_reg.read());
        dma_broadcast_next.write(dma_broadcast_reg.read());
        request_type_next.write(request_type_reg.read());
        dma_loop_next.write(dma_loop_reg.read());

        uint64_t v;
        v_fp16_t v_fp16 = v_fp16_t();

        // Static configuration updates (do not affect active runtime)
        if (set_addr.read()) {
            dma_base_static_next.write(imm.read());
        }
        if (set_len.read()) {
            dma_len_static_next.write(imm.read());
        }
        if (set_loop.read()) {
            dma_loop_static_next.write(imm.read());
        }
        if (set_mode.read()) {
            dma_stride_static_next.write(imm.read());
            switch (mode.read() & 0x7) {
                case 0: request_type_static_next.write(LDMARequestType::LOAD_BYTE); dma_broadcast_static_next.write(false); break;
                case 1: request_type_static_next.write(LDMARequestType::LOAD_HALF); dma_broadcast_static_next.write(false); break;
                case 2: request_type_static_next.write(LDMARequestType::LOAD_WORD); dma_broadcast_static_next.write(false); break;
                case 3: request_type_static_next.write(LDMARequestType::LOAD_DWORD); dma_broadcast_static_next.write(false); break;
                case 4: request_type_static_next.write(LDMARequestType::LOAD_BYTE); dma_broadcast_static_next.write(true); break;
                case 5: request_type_static_next.write(LDMARequestType::LOAD_HALF); dma_broadcast_static_next.write(true); break;
                case 6: request_type_static_next.write(LDMARequestType::LOAD_WORD); dma_broadcast_static_next.write(true); break;
                default: request_type_static_next.write(LDMARequestType::LOAD_DWORD); dma_broadcast_static_next.write(false); break;
            }
        }

        if (reset_active.read()) {
            state_next.write(LDMAState::IDLE);
            dma_base_next.write(0);
            dma_stride_next.write(0);
            dma_len_next.write(0);
            dma_loop_next.write(0);
            request_type_next.write(LDMARequestType::LOAD_DWORD);
            dma_broadcast_next.write(0);
            dma_offset_next.write(0);
        }

        switch (state_reg.read()) {
            case LDMAState::IDLE:
                if (active.read()) {
                    // Activate from static config
                    dma_base_next.write(dma_base_static_reg.read());
                    dma_stride_next.write(dma_stride_static_reg.read());
                    dma_len_next.write(dma_len_static_reg.read());
                    dma_loop_next.write(dma_loop_static_reg.read());
                    request_type_next.write(request_type_static_reg.read());
                    dma_broadcast_next.write(dma_broadcast_static_reg.read());
                    dma_offset_next.write(0);

                    if (dma_len_static_reg.read() == 0) {
                        state_next.write(LDMAState::IDLE);
                    } else {
                        // Directly go to LOAD_WAIT to hide one cycle
                        // Prepare next offset for the pipeline prefetch
                        dma_offset_next.write(dma_stride_static_reg.read() * sizeof(uint16_t));
                        state_next.write(LDMAState::LOAD_WAIT);
                    }
                }
                break;

            case LDMAState::LOAD_PRE:
                state_next.write(LDMAState::LOAD_WAIT);
                // 更新 offset ，準備讀取下一筆資料
                dma_offset_next.write(dma_offset_reg.read() + dma_stride_reg.read() * sizeof(uint16_t));
                break;

            case LDMAState::LOAD_WAIT:
                state_next.write(LDMAState::LOAD_PIPELINE);

                // 更新 len
                dma_len_next.write(dma_len_reg.read() - 1);

                v = dm_read_data.read();
                v = mask_and_broadcast(v, request_type_reg.read(), dma_broadcast_reg.read());
                v_fp16.fromUint64(v);
                dmrv_next.write(v_fp16);
                break;

            case LDMAState::LOAD_PIPELINE:
                if (next.read()) {
                    if (dma_len_reg.read() == 0) {
                        if (dma_loop_reg.read() > 1) {
                            dma_loop_next.write(dma_loop_reg.read() - 1);
                            dma_offset_next.write(0);
                            dma_len_next.write(dma_len_static_reg.read());
                            state_next.write(LDMAState::LOAD_PRE);
                        } else {
                            state_next.write(LDMAState::DONE);
                        }
                    } else {
                        state_next.write(LDMAState::LOAD_PIPELINE);
                        dma_offset_next.write(dma_offset_reg.read() + dma_stride_reg.read() * sizeof(uint16_t));
                        dma_len_next.write(dma_len_reg.read() - 1);
                    }

                    v = dm_read_data.read();
                    v = mask_and_broadcast(v, request_type_reg.read(), dma_broadcast_reg.read());
                    v_fp16.fromUint64(v);
                    dmrv_next.write(v_fp16);
                }
                break;

            case LDMAState::DONE:
                state_next.write(LDMAState::IDLE);
                break;

            default:
                break;
        }

        DEBUG_MSG("[LDMA] NextStateLogic: State=" << state_reg.read()
                  << " Next=" << state_next.read(), DEBUG_LEVEL_PE_COMPONENTS);
    }

    void output_logic() {
        // Default outputs
        busy.write(false);
        done.write(false);
        dm_read_addr.write(dma_base_reg.read() + dma_offset_reg.read());
        dmrv_out.write(dmrv_reg.read());

        switch (state_reg.read()) {
            case LDMAState::IDLE:
                // If activation arrives, issue first read address immediately
                if (active.read() && dma_len_static_reg.read() != 0) {
                    busy.write(true);
                    dm_read_addr.write(dma_base_static_reg.read());
                }
                break;

            case LDMAState::LOAD_PRE:
                busy.write(true);
                break;

            case LDMAState::LOAD_WAIT:
                busy.write(true);
                break;

            case LDMAState::LOAD_PIPELINE:
                busy.write(true);
                if (next.read()) {
                    dm_read_addr.write(dma_base_reg.read() + dma_offset_next.read());
                }
                break;

            case LDMAState::DONE:
                done.write(true);
                break;

            default:
                break;
        }
    }
}; // end of SC_MODULE(LDMA)

}; // namespace pe
}; // namespace hybridacc
