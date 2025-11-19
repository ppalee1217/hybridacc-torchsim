#pragma once

#include "utils.hpp"
#include <systemc>

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
    LOAD,
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
        case State::LOAD: return os << "LOAD";
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
    sc_out<bool> dl_stall_out;  // Combinational stall signal (立即反映是否需要 stall)

    // data ports
    sc_in<v_fp16_t> ps_data_in;
    sc_in<bool> ps_data_in_valid;
    sc_out<bool> ps_data_in_ready;

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
          ps_data_in("ps_data_in"),
          ps_data_in_valid("ps_data_in_valid"),
          ps_data_in_ready("ps_data_in_ready"),
          dm_write_en("dm_write_en"),
          dm_write_addr("dm_write_addr"),
          dm_write_data("dm_write_data"),
          dm_write_mask("dm_write_mask"),
          dm_read_addr("dm_read_addr"),
          dm_read_data("dm_read_data"),
          dmrv_out("dmrv_out"),
          dmrv(),
          dmwv(),
          dma_base(0),
          dma_offset(0),
          dma_len(0),
          dma_stride(0),
          dma_broadcast(false)
    {
        DEBUG_MSG("[Create] DataLoader");

        // 在構造函數體中初始化 sc_signal
        current_state.write(State::IDLE);

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);

        SC_METHOD(combinational_process);
        sensitive << current_state << active << write_en;
    }


    // Member variables
    v_fp16_t dmrv;
    v_fp16_t dmwv;
    uint16_t dma_base;
    uint16_t dma_offset;
    uint16_t dma_len;
    uint16_t dma_stride;
    bool dma_broadcast;
    DMARequestType request_type;

    // State machine variables
    sc_signal<State> current_state;
    State next_state;

    uint64_t mask_and_broadcast(uint64_t v, DMARequestType request_type, bool dma_broadcast) {  // 修正：移除 const&
        uint64_t broadcast_v = 0;
        switch (request_type) {
            case DMARequestType::LOAD_BYTE:
                v = v & 0xFF; // 假設只讀取低位
                if(dma_broadcast) {
                    for(int i = 0; i < 4; ++i) {
                        broadcast_v |= v << (i * 16);
                    }
                    v = broadcast_v; // 將所有 lane 設為相同值
                }
                break;
            case DMARequestType::LOAD_HALF:
                v = v & 0xFFFF; // 假設只讀取低位
                if(dma_broadcast) {
                    for(int i = 0; i < 4; ++i) {
                        broadcast_v |= v << (i * 16);
                    }
                    v = broadcast_v; // 將所有 lane 設為相同值
                }
                break;
            case DMARequestType::LOAD_WORD:
                v = v & 0xFFFFFFFF; // 假設只讀取低位
                if(dma_broadcast) {
                    for(int i = 0; i < 2; ++i) {
                        broadcast_v |= v << (i * 32);
                    }
                    v = broadcast_v; // 將所有 lane 設為相同值
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
        current_state.write(State::IDLE);
        busy.write(false);
        done.write(false);
        ps_data_in_ready.write(false);
        dm_write_en.write(false);
        dm_write_addr.write(0);
        dm_write_data.write(0);
        dm_write_mask.write(0);
        dm_read_addr.write(0);
        dmrv_out.write(v_fp16_t());
        wait();

        while (true) {
            // Default next state
            next_state = current_state.read();

            // Default outputs (will be overridden in state machine)
            busy.write(false);
            done.write(false);
            ps_data_in_ready.write(false);
            dm_write_en.write(false);
            dm_write_addr.write(0);
            dm_write_data.write(0);
            dm_write_mask.write(0);
            dm_read_addr.write(0);

            switch (current_state.read()) {
                case State::IDLE:
                    if (active.read()) {
                        // Initialize DMA parameters
                        dma_offset = 0;
                        dma_stride = stride.read();
                        next_state = write_en.read() ? State::STORE_PRE : State::LOAD_PRE;
                        switch (mode.read() & 0x7) {
                            case 0: request_type = DMARequestType::LOAD_BYTE; dma_broadcast = false; break;
                            case 1: request_type = DMARequestType::LOAD_HALF; dma_broadcast = false; break;
                            case 2: request_type = DMARequestType::LOAD_WORD; dma_broadcast = false; break;
                            case 3: request_type = DMARequestType::LOAD_DWORD; dma_broadcast = false; break;
                            case 4: request_type = DMARequestType::LOAD_BYTE; dma_broadcast = true; break;
                            case 5: request_type = DMARequestType::LOAD_HALF; dma_broadcast = true; break;
                            case 6: request_type = DMARequestType::LOAD_WORD; dma_broadcast = true; break;
                            default: request_type = DMARequestType::LOAD_DWORD; dma_broadcast = false; break;
                        }
                    } else if (set_addr.read()) {
                        dma_base = addr_len.read();
                    } else if (set_len.read()) {
                        dma_len = addr_len.read();
                    }
                    break;

                case State::STORE_PRE:
                    busy.write(true);
                    ps_data_in_ready.write(true);
                    if (ps_data_in_valid.read()) {
                        dmwv = ps_data_in.read();
                        next_state = State::STORE;
                    }
                    break;

                case State::STORE:
                    busy.write(true);
                    dm_write_en.write(true);
                    dm_write_addr.write(dma_base + dma_offset);
                    dm_write_data.write(dmwv.toUint64());
                    dm_write_mask.write(0xFF);

                    dma_offset += dma_stride * sizeof(uint16_t);
                    dma_len--;
                    if (dma_len == 0) {
                        next_state = State::DONE;
                    } else {
                        next_state = State::STORE_PRE;
                    }
                    break;

                case State::LOAD_PRE:
                    busy.write(true);
                    dm_read_addr.write(dma_base + dma_offset);
                    next_state = State::LOAD_WAIT;
                    break;

                case State::LOAD_WAIT: {
                    busy.write(true);
                    next_state = State::LOAD;
                    uint64_t v = dm_read_data.read();
                    v = mask_and_broadcast(v, request_type, dma_broadcast);
                    dmrv.fromUint64(v);
                    dmrv_out.write(dmrv);
                    break;
                }

                case State::LOAD: {
                    busy.write(true);
                    if (next.read()) {
                        dma_offset += dma_stride * sizeof(uint16_t);  //  修正語法錯誤
                        dma_len--;

                        if (dma_len == 0) {
                            next_state = State::DONE;
                        } else {
                            next_state = State::LOAD_PRE;
                            dm_read_addr.write(dma_base + dma_offset);
                        }

                        uint64_t v = dm_read_data.read();
                        v = mask_and_broadcast(v, request_type, dma_broadcast);
                        dmrv.fromUint64(v);
                        dmrv_out.write(dmrv);
                    }
                    break;
                }

                case State::DONE:
                    done.write(true);
                    next_state = State::IDLE;
                    break;

                default:
                    break;
            }

            current_state.write(next_state);
            wait();
        }
    }

    void combinational_process() {
        // Combinational stall signal generation
        // dl_stall_out 只基於 current_state（暫存器），不依賴輸入控制信號

        bool stall = false;
        State cur_state = current_state.read();

        // 檢查是否處於 busy 狀態
        // STORE operations
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
