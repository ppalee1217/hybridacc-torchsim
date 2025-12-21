#pragma once

#include "utils.hpp"
#include <systemc>
#include <array>

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

// -----------------------------------------------------------------------------
enum class DMARequestType {
    LOAD_BYTE, // 8-bit load
    LOAD_HALF, // 16-bit load
    LOAD_WORD, // 32-bit load
    LOAD_DWORD, // 64-bit load
    STORE_DWORD, // 64-bit store
};

// Add operator<< support for DMARequestType
inline std::ostream& operator<<(std::ostream& os, DMARequestType type) {
    switch (type) {
        case DMARequestType::LOAD_BYTE: return os << "LOAD_BYTE";
        case DMARequestType::LOAD_HALF: return os << "LOAD_HALF";
        case DMARequestType::LOAD_WORD: return os << "LOAD_WORD";
        case DMARequestType::LOAD_DWORD: return os << "LOAD_DWORD";
        case DMARequestType::STORE_DWORD: return os << "STORE_DWORD";
        default: return os << "UNKNOWN";
    }
}

// sc_trace for DMARequestType
inline void sc_trace(sc_core::sc_trace_file* tf, const DMARequestType& type, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(type), name);
}

// -----------------------------------------------------------------------------
// State machine
enum class State {
    IDLE,
    STORE_PRE,
    STORE,
    LOAD_PRE,
    LOAD_WAIT,
    LOAD_PIPELINE,  // 新增: 流水線讀取狀態
    DONE
};

// Add operator<< support for State
inline std::ostream& operator<<(std::ostream& os, State state) {
    switch (state) {
        case State::IDLE: return os << "IDLE";
        case State::STORE_PRE: return os << "STORE_PRE";
        case State::STORE: return os << "STORE";
        case State::LOAD_PRE: return os << "LOAD_PRE";
        case State::LOAD_WAIT: return os << "LOAD_WAIT";
        case State::LOAD_PIPELINE: return os << "LOAD_PIPELINE";  // 新增
        case State::DONE: return os << "DONE";
        default: return os << "UNKNOWN";
    }
}

// sc_trace for State
inline void sc_trace(sc_core::sc_trace_file* tf, const State& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}

// -----------------------------------------------------------------------------
SC_MODULE(DataLoader) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // control ports
    sc_in<uint16_t> addr_len;
    sc_in<bool> set_addr;
    sc_in<bool> set_len;
    sc_in<uint16_t> mode;
    sc_in<uint16_t> stride;
    sc_in<bool> write_en;

    sc_in<bool> active;
    sc_in<bool> next;

    sc_out<bool> busy;
    sc_out<bool> done;
    sc_out<bool> dl_stall_out;

    // data ports - using VRDIF
    VRDIF<v_fp16_t> ps_data;

    // DM-related ports
    sc_out<bool> dm_write_en;
    sc_out<uint16_t> dm_write_addr;
    sc_out<uint64_t> dm_write_data;
    sc_out<uint8_t> dm_write_mask;

    sc_out<uint16_t> dm_read_addr;
    sc_in<uint64_t> dm_read_data;

    // dmrv
    sc_out<v_fp16_t> dmrv_out;

    // Constructor
    SC_CTOR(DataLoader)
        : clk("clk"),
          reset_n("reset_n"),
          addr_len("addr_len"),
          set_addr("set_addr"),
          set_len("set_len"),
          mode("mode"),
          stride("stride"),
          write_en("write_en"),
          active("active"),
          next("next"),
          busy("busy"),
          done("done"),
          dl_stall_out("dl_stall_out"),
          ps_data("ps_data"),
          dm_write_en("dm_write_en"),
          dm_write_addr("dm_write_addr"),
          dm_write_data("dm_write_data"),
          dm_write_mask("dm_write_mask"),
          dm_read_addr("dm_read_addr"),
          dm_read_data("dm_read_data"),
          dmrv_out("dmrv_out")
    {
        DEBUG_MSG("[Create] DataLoader", DEBUG_LEVEL_PE_COMPONENTS);

        // 初始化暫存器
        state_reg.write(State::IDLE);
        dmrv_reg.write(v_fp16_t());
        dmwv_reg.write(v_fp16_t());
        dma_base_reg.write(0);
        dma_offset_reg.write(0);
        dma_len_reg.write(0);
        dma_stride_reg.write(0);
        dma_broadcast_reg.write(false);
        request_type_reg.write(DMARequestType::LOAD_DWORD);

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);

        // 拆分成多個組合邏輯方法
        SC_METHOD(next_state_logic);
        sensitive << state_reg << active << write_en << next << set_addr << set_len
                  << ps_data.valid_in << ps_data.data_in << dm_read_data
                  << addr_len << mode << stride
                  << dma_base_reg << dma_offset_reg << dma_len_reg << dma_stride_reg
                  << dma_broadcast_reg << request_type_reg << dmwv_reg << dmrv_reg;

        SC_METHOD(output_logic);
        sensitive << state_reg << dmrv_reg << dmwv_reg
                  << dma_base_reg << dma_offset_reg
                  << dma_offset_next << next;

        SC_METHOD(ready_logic);
        sensitive << state_reg;

        SC_METHOD(stall_logic);
        sensitive << state_reg << active << write_en;
    }

    // Registers (current state)
    sc_signal<State> state_reg;
    sc_signal<v_fp16_t> dmrv_reg;
    sc_signal<v_fp16_t> dmwv_reg;
    sc_signal<uint16_t> dma_base_reg;
    sc_signal<uint16_t> dma_offset_reg;
    sc_signal<uint16_t> dma_len_reg;
    sc_signal<uint16_t> dma_stride_reg;
    sc_signal<bool> dma_broadcast_reg;
    sc_signal<DMARequestType> request_type_reg;

    // Next state (combinational) - 改為 signal
    sc_signal<State> state_next;
    sc_signal<v_fp16_t> dmrv_next;
    sc_signal<v_fp16_t> dmwv_next;
    sc_signal<uint16_t> dma_base_next;
    sc_signal<uint16_t> dma_offset_next;
    sc_signal<uint16_t> dma_len_next;
    sc_signal<uint16_t> dma_stride_next;
    sc_signal<bool> dma_broadcast_next;
    sc_signal<DMARequestType> request_type_next;

    uint64_t mask_and_broadcast(uint64_t v, DMARequestType request_type, bool dma_broadcast) {
        uint64_t broadcast_v = 0;
        switch (request_type) {
            case DMARequestType::LOAD_BYTE:
                v = v & 0xFF;
                if(dma_broadcast) {
                    for(int i = 0; i < 4; ++i) {
                        broadcast_v |= v << (i * 16);
                    }
                    v = broadcast_v;
                }
                break;
            case DMARequestType::LOAD_HALF:
                v = v & 0xFFFF;
                if(dma_broadcast) {
                    for(int i = 0; i < 4; ++i) {
                        broadcast_v |= v << (i * 16);
                    }
                    v = broadcast_v;
                }
                break;
            case DMARequestType::LOAD_WORD:
                v = v & 0xFFFFFFFF;
                if(dma_broadcast) {
                    for(int i = 0; i < 2; ++i) {
                        broadcast_v |= v << (i * 32);
                    }
                    v = broadcast_v;
                }
                break;
            case DMARequestType::LOAD_DWORD:
                break;
            default:
                break;
        }
        return v;
    }

    // SystemC processes
    void sequential_process() {
        // Reset initialization
        state_reg.write(State::IDLE);
        dmrv_reg.write(v_fp16_t());
        dmwv_reg.write(v_fp16_t());
        dma_base_reg.write(0);
        dma_offset_reg.write(0);
        dma_len_reg.write(0);
        dma_stride_reg.write(0);
        dma_broadcast_reg.write(false);
        request_type_reg.write(DMARequestType::LOAD_DWORD);
        wait();

        while (true) {
            // Update all registers with next values
            state_reg.write(state_next.read());
            dmrv_reg.write(dmrv_next.read());
            dmwv_reg.write(dmwv_next.read());
            dma_base_reg.write(dma_base_next.read());
            dma_offset_reg.write(dma_offset_next.read());
            dma_len_reg.write(dma_len_next.read());
            dma_stride_reg.write(dma_stride_next.read());
            dma_broadcast_reg.write(dma_broadcast_next.read());
            request_type_reg.write(request_type_next.read());

            DEBUG_MSG("[DataLoader] State=" << state_reg.read()
                      << " ps_valid=" << ps_data.valid_in.read()
                      << " ready=" << ps_data.ready_out.read()
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
        dma_base_next.write(dma_base_reg.read());
        dma_offset_next.write(dma_offset_reg.read());
        dma_len_next.write(dma_len_reg.read());
        dma_stride_next.write(dma_stride_reg.read());
        dma_broadcast_next.write(dma_broadcast_reg.read());
        request_type_next.write(request_type_reg.read());

        uint64_t v;
        v_fp16_t v_fp16 = v_fp16_t();

        switch (state_reg.read()) {
            case State::IDLE:
                if (active.read()) {
                    // Initialize DMA parameters
                    dma_offset_next.write(0);
                    dma_stride_next.write(stride.read());
                    state_next.write(write_en.read() ? State::STORE_PRE : State::LOAD_PRE);

                    switch (mode.read() & 0x7) {
                        case 0: request_type_next.write(DMARequestType::LOAD_BYTE); dma_broadcast_next.write(false); break;
                        case 1: request_type_next.write(DMARequestType::LOAD_HALF); dma_broadcast_next.write(false); break;
                        case 2: request_type_next.write(DMARequestType::LOAD_WORD); dma_broadcast_next.write(false); break;
                        case 3: request_type_next.write(DMARequestType::LOAD_DWORD); dma_broadcast_next.write(false); break;
                        case 4: request_type_next.write(DMARequestType::LOAD_BYTE); dma_broadcast_next.write(true); break;
                        case 5: request_type_next.write(DMARequestType::LOAD_HALF); dma_broadcast_next.write(true); break;
                        case 6: request_type_next.write(DMARequestType::LOAD_WORD); dma_broadcast_next.write(true); break;
                        default: request_type_next.write(DMARequestType::LOAD_DWORD); dma_broadcast_next.write(false); break;
                    }
                } else if (set_addr.read()) {
                    dma_base_next.write(addr_len.read());
                } else if (set_len.read()) {
                    dma_len_next.write(addr_len.read());
                }
                break;

            case State::STORE_PRE:
                if (ps_data.valid_in.read()) {
                    dmwv_next.write(ps_data.data_in.read());
                    state_next.write(State::STORE);
                }
                break;

            case State::STORE:
                dma_offset_next.write(dma_offset_reg.read() + dma_stride_reg.read() * sizeof(uint16_t));
                dma_len_next.write(dma_len_reg.read() - 1);

                if (dma_len_next.read() == 0) {
                    state_next.write(State::DONE);
                } else {
                    state_next.write(State::STORE_PRE);
                }
                break;

            case State::LOAD_PRE:
                state_next.write(State::LOAD_WAIT);
                // 更新 offset ，準備讀取下一筆資料
                dma_offset_next.write(dma_offset_reg.read() + dma_stride_reg.read() * sizeof(uint16_t));
                break;

            case State::LOAD_WAIT:
                state_next.write(State::LOAD_PIPELINE);

                // 更新 len
                dma_len_next.write(dma_len_reg.read() - 1);

                v = dm_read_data.read();
                v = mask_and_broadcast(v, request_type_reg.read(), dma_broadcast_reg.read());
                v_fp16.fromUint64(v);
                dmrv_next.write(v_fp16);
                break;

            case State::LOAD_PIPELINE:
                if (next.read()) {
                    if (dma_len_reg.read() == 0) {
                        state_next.write(State::DONE);
                    } else {
                        state_next.write(State::LOAD_PIPELINE);
                        dma_offset_next.write(dma_offset_reg.read() + dma_stride_reg.read() * sizeof(uint16_t));
                        dma_len_next.write(dma_len_reg.read() - 1);
                    }

                    v = dm_read_data.read();
                    v = mask_and_broadcast(v, request_type_reg.read(), dma_broadcast_reg.read());
                    v_fp16.fromUint64(v);
                    dmrv_next.write(v_fp16);
                }
                break;

            case State::DONE:
                state_next.write(State::IDLE);
                break;

            default:
                break;
        }

        DEBUG_MSG("[DataLoader] NextStateLogic: State=" << state_reg.read()
                  << " Next=" << state_next.read(), DEBUG_LEVEL_PE_COMPONENTS);
    }

    void output_logic() {
        // Default outputs
        busy.write(false);
        done.write(false);
        dm_write_en.write(false);
        dm_write_addr.write(dma_base_reg.read() + dma_offset_reg.read());
        dm_write_data.write(0);
        dm_write_mask.write(0);
        dm_read_addr.write(dma_base_reg.read() + dma_offset_reg.read());
        dmrv_out.write(dmrv_reg.read());

        switch (state_reg.read()) {
            case State::IDLE:
                // All default values
                break;

            case State::STORE_PRE:
                busy.write(true);
                break;

            case State::STORE:
                busy.write(true);
                dm_write_en.write(true);
                dm_write_data.write(dmwv_reg.read().toUint64());
                dm_write_mask.write(0xFF);
                break;

            case State::LOAD_PRE:
                busy.write(true);
                break;

            case State::LOAD_WAIT:
                busy.write(true);
                break;

            case State::LOAD_PIPELINE:
                busy.write(true);
                if (next.read()) {
                    dm_read_addr.write(dma_base_reg.read() + dma_offset_next.read());
                }
                break;

            case State::DONE:
                done.write(true);
                break;

            default:
                break;
        }
    }

    void ready_logic() {
        // ps_data.ready_out 只在 STORE_PRE 狀態時為 true
        ps_data.ready_out.write(state_reg.read() == State::STORE_PRE);
    }

    void stall_logic() {
        // Combinational stall signal generation
        bool stall = false;
        State cur_state = state_reg.read();

        if (cur_state == State::STORE_PRE || cur_state == State::STORE) {
            stall = true;
        }

        if (cur_state == State::IDLE && active.read() && write_en.read()) {
            stall = true;
        }

        dl_stall_out.write(stall);
    }
}; // end of SC_MODULE(DataLoader)

}; // namespace pe
}; // namespace hybridacc
