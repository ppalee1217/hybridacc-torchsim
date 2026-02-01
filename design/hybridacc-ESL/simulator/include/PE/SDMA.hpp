#pragma once

#include "utils.hpp"
#include <systemc>
#include <array>

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

// -----------------------------------------------------------------------------
// Removed DMARequestType as SDMA only supports STORE_DWORD
// -----------------------------------------------------------------------------

// State machine
enum class SDMAState {
    IDLE,
    RUN,
    WAIT_SWAP,
    FINISH
};

// Add operator<< support for SDMAState
inline std::ostream& operator<<(std::ostream& os, SDMAState state) {
    switch (state) {
        case SDMAState::IDLE: return os << "IDLE";
        case SDMAState::RUN: return os << "RUN";
        case SDMAState::WAIT_SWAP: return os << "WAIT_SWAP";
        case SDMAState::FINISH: return os << "FINISH";
        default: return os << "UNKNOWN";
    }
}

// sc_trace for SDMAState
inline void sc_trace(sc_core::sc_trace_file* tf, const SDMAState& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}

// -----------------------------------------------------------------------------
SC_MODULE(SDMA) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // control ports
    sc_in<uint16_t> imm;
    sc_in<bool> set_addr;
    sc_in<bool> set_len;
    sc_in<bool> set_loop;
    sc_in<uint16_t> mode;
    sc_in<uint16_t> stride;
    sc_in<bool> swap_in;

    sc_in<bool> active;
    sc_in<bool> next;

    sc_out<bool> busy;
    sc_out<bool> done;
    sc_out<bool> dl_stall_out;
    sc_out<bool> bank_sel;

    // data ports - using VRDIF
    VRDIF<v_fp16_t> ps_data;

    // DM-related ports
    sc_out<bool> dm_write_en;
    sc_out<uint16_t> dm_write_addr;
    sc_out<uint64_t> dm_write_data;
    sc_out<uint8_t> dm_write_mask;

    // SDMA is STORE only: it only drives DM write signals.

    // Constructor
    SC_CTOR(SDMA)
        : clk("clk"),
          reset_n("reset_n"),
          imm("imm"),
          set_addr("set_addr"),
          set_len("set_len"),
          set_loop("set_loop"),
          mode("mode"),
          stride("stride"),
          swap_in("swap_in"),
          active("active"),
          next("next"),
          busy("busy"),
          done("done"),
          dl_stall_out("dl_stall_out"),
          bank_sel("bank_sel"),
          ps_data("ps_data"),
          dm_write_en("dm_write_en"),
          dm_write_addr("dm_write_addr"),
          dm_write_data("dm_write_data"),
          dm_write_mask("dm_write_mask")
    {
        DEBUG_MSG("[Create] SDMA", DEBUG_LEVEL_PE_COMPONENTS);

        // 初始化暫存器
        state_reg.write(SDMAState::IDLE);
        dma_base_reg.write(0);
        dma_len_cfg_reg.write(0);
        dma_stride_reg.write(0);
        dma_loop_cfg_reg.write(0);
        dma_offset_reg.write(0);
        dma_len_rem_reg.write(0);
        dma_loops_rem_reg.write(0);
        bank_sel_reg.write(0); // Default Bank 0
        bank_valid_reg.write(0);

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);

        // 拆分成多個組合邏輯方法
        SC_METHOD(next_state_logic);
        sensitive << state_reg
              << active
              << set_addr << set_len << set_loop
              << swap_in
              << ps_data.valid_in
              << imm << stride
              << dma_base_reg << dma_len_cfg_reg << dma_stride_reg << dma_loop_cfg_reg
              << dma_offset_reg << dma_len_rem_reg << dma_loops_rem_reg
              << bank_sel_reg << bank_valid_reg;

        SC_METHOD(output_logic);
        sensitive << state_reg
              << ps_data.valid_in << ps_data.data_in
              << dma_base_reg << dma_offset_reg
              << dma_len_rem_reg
              << bank_sel_reg;

        SC_METHOD(ready_logic);
        sensitive << state_reg << dma_len_rem_reg;

        SC_METHOD(stall_logic);
        sensitive << state_reg;
    }

    // Registers (current state)
    sc_signal<SDMAState> state_reg;

    // Configuration registers (programmed only in IDLE)
    sc_signal<uint16_t> dma_base_reg;
    sc_signal<uint16_t> dma_len_cfg_reg;
    sc_signal<uint16_t> dma_stride_reg;
    sc_signal<uint16_t> dma_loop_cfg_reg;

    // Runtime registers
    sc_signal<uint16_t> dma_offset_reg;
    sc_signal<uint16_t> dma_len_rem_reg;
    sc_signal<uint16_t> dma_loops_rem_reg;

    sc_signal<bool> bank_sel_reg;
    sc_signal<uint8_t> bank_valid_reg; // Bit 0: Bank0, Bit 1: Bank1

    // Next state (combinational) - 改為 signal
    sc_signal<SDMAState> state_next;
    sc_signal<uint16_t> dma_base_next;
    sc_signal<uint16_t> dma_len_cfg_next;
    sc_signal<uint16_t> dma_stride_next;
    sc_signal<uint16_t> dma_loop_cfg_next;

    sc_signal<uint16_t> dma_offset_next;
    sc_signal<uint16_t> dma_len_rem_next;
    sc_signal<uint16_t> dma_loops_rem_next;

    sc_signal<bool> bank_sel_next;
    sc_signal<uint8_t> bank_valid_next;

    static inline uint16_t normalize_loop_count(uint16_t v) {
        // Treat 0 as 1 phase to avoid "no-op task" surprises.
        return (v == 0) ? 1 : v;
    }

    // SystemC processes
    void sequential_process() {
        // Reset initialization
        state_reg.write(SDMAState::IDLE);
        dma_base_reg.write(0);
        dma_len_cfg_reg.write(0);
        dma_stride_reg.write(0);
        dma_loop_cfg_reg.write(0);
        dma_offset_reg.write(0);
        dma_len_rem_reg.write(0);
        dma_loops_rem_reg.write(0);
        bank_sel_reg.write(0);
        bank_valid_reg.write(0);
        wait();

        while (true) {
            // Update all registers with next values
            state_reg.write(state_next.read());
            dma_base_reg.write(dma_base_next.read());
            dma_len_cfg_reg.write(dma_len_cfg_next.read());
            dma_offset_reg.write(dma_offset_next.read());
            dma_stride_reg.write(dma_stride_next.read());
            dma_loop_cfg_reg.write(dma_loop_cfg_next.read());

            dma_len_rem_reg.write(dma_len_rem_next.read());
            dma_loops_rem_reg.write(dma_loops_rem_next.read());
            bank_sel_reg.write(bank_sel_next.read());
            bank_valid_reg.write(bank_valid_next.read());

            DEBUG_MSG("[SDMA] State=" << state_reg.read()
                      << " ps_valid=" << ps_data.valid_in.read()
                      << " ready=" << ps_data.ready_out.read()
                      << " len_rem=" << dma_len_rem_reg.read()
                      << " loops_rem=" << dma_loops_rem_reg.read()
                      << " next=" << next.read(), DEBUG_LEVEL_PE_COMPONENTS);

            wait();
        }
    }

    void next_state_logic() {
        // Default: hold current values
        state_next.write(state_reg.read());
        dma_base_next.write(dma_base_reg.read());
        dma_len_cfg_next.write(dma_len_cfg_reg.read());
        dma_stride_next.write(dma_stride_reg.read());
        dma_loop_cfg_next.write(dma_loop_cfg_reg.read());
        dma_offset_next.write(dma_offset_reg.read());
        dma_len_rem_next.write(dma_len_rem_reg.read());
        dma_loops_rem_next.write(dma_loops_rem_reg.read());
        bank_sel_next.write(bank_sel_reg.read());
        bank_valid_next.write(bank_valid_reg.read());

        const bool fire = (state_reg.read() == SDMAState::RUN) && ps_data.valid_in.read() && (dma_len_rem_reg.read() > 0);

        switch (state_reg.read()) {
            case SDMAState::IDLE: {
                // Configuration (only in IDLE)
                if (set_addr.read()) {
                    dma_base_next.write(imm.read());
                } else if (set_len.read()) {
                    dma_len_cfg_next.write(imm.read());
                } else if (set_loop.read()) {
                    dma_loop_cfg_next.write(imm.read());
                }

                // Allow SWAPDM even when idle (pure bank role swap)
                if (swap_in.read()) {
                    bank_sel_next.write(!bank_sel_reg.read());
                }

                // Start a background store task (SDMA.SD)
                if (active.read()) {
                    dma_stride_next.write(stride.read());
                    dma_offset_next.write(0);

                    uint16_t loops = normalize_loop_count(dma_loop_cfg_reg.read());
                    dma_loops_rem_next.write(loops);
                    dma_len_rem_next.write(dma_len_cfg_reg.read());

                    // Starting a new phase will overwrite the current writer bank, mark it invalid.
                    uint8_t mask = bank_sel_reg.read() ? 0x2 : 0x1;
                    bank_valid_next.write(bank_valid_reg.read() & static_cast<uint8_t>(~mask));

                    // If len==0, treat as "phase complete" and directly wait for SWAPDM.
                    if (dma_len_cfg_reg.read() == 0) {
                        if (loops > 0) {
                            dma_loops_rem_next.write(loops - 1);
                        }
                        state_next.write(SDMAState::WAIT_SWAP);
                        bank_valid_next.write((bank_valid_next.read()) | mask);
                    } else {
                        state_next.write(SDMAState::RUN);
                    }
                }
            } break;

            case SDMAState::RUN: {
                if (fire) {
                    dma_offset_next.write(dma_offset_reg.read() + dma_stride_reg.read() * sizeof(uint16_t));
                    uint16_t next_len = dma_len_rem_reg.read() - 1;
                    dma_len_rem_next.write(next_len);

                    if (next_len == 0) {
                        uint16_t loops_rem = dma_loops_rem_reg.read();
                        if (loops_rem > 0) {
                            dma_loops_rem_next.write(loops_rem - 1);
                        }

                        // Mark current writer bank valid.
                        uint8_t mask = bank_sel_reg.read() ? 0x2 : 0x1;
                        bank_valid_next.write(bank_valid_reg.read() | mask);

                        // One loop-phase done, wait SWAPDM.
                        state_next.write(SDMAState::WAIT_SWAP);
                    }
                }
            } break;

            case SDMAState::WAIT_SWAP: {
                if (swap_in.read()) {
                    bool new_bank_sel = !bank_sel_reg.read();
                    bank_sel_next.write(new_bank_sel);

                    if (dma_loops_rem_reg.read() > 0) {
                        // Start next phase
                        dma_offset_next.write(0);
                        dma_len_rem_next.write(dma_len_cfg_reg.read());

                        uint8_t new_mask = new_bank_sel ? 0x2 : 0x1;
                        bank_valid_next.write(bank_valid_reg.read() & static_cast<uint8_t>(~new_mask));

                        if (dma_len_cfg_reg.read() == 0) {
                            // Empty phase completes immediately
                            dma_loops_rem_next.write(dma_loops_rem_reg.read() - 1);
                            bank_valid_next.write((bank_valid_next.read()) | new_mask);
                            state_next.write(SDMAState::WAIT_SWAP);
                        } else {
                            state_next.write(SDMAState::RUN);
                        }
                    } else {
                        // Task fully completed after observing SWAPDM
                        state_next.write(SDMAState::FINISH);
                    }
                }
            } break;

            case SDMAState::FINISH:
                state_next.write(SDMAState::IDLE);
                break;

            default:
                break;
        }

        DEBUG_MSG("[SDMA] NextStateLogic: State=" << state_reg.read()
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
        bank_sel.write(bank_sel_reg.read());

        const bool fire = (state_reg.read() == SDMAState::RUN) && ps_data.valid_in.read() && (dma_len_rem_reg.read() > 0);

        switch (state_reg.read()) {
            case SDMAState::IDLE:
                // All default values
                break;

            case SDMAState::RUN:
                // Background store in progress
                busy.write(true);
                if (fire) {
                    dm_write_en.write(true);
                    dm_write_data.write(ps_data.data_in.read().toUint64());
                    dm_write_mask.write(0xFF);
                }
                break;

            case SDMAState::WAIT_SWAP:
                // Not busy (so SWAPDM can proceed), but SDMA is waiting for swap to continue/finish.
                break;

            case SDMAState::FINISH:
                done.write(true);
                break;

            default:
                break;
        }
    }

    void ready_logic() {
        // SDMA pulls stream data only while RUN and len_rem>0.
        ps_data.ready_out.write((state_reg.read() == SDMAState::RUN) && (dma_len_rem_reg.read() > 0));
    }

    void stall_logic() {
        // SDMA runs in background; pipeline sync is handled by SWAPDM (see EXE_M_stage swap_stall).
        dl_stall_out.write(false);
    }
}; // end of SC_MODULE(SDMA)

}; // namespace pe
}; // namespace hybridacc
