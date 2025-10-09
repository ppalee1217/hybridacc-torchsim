#ifndef HYBRIDACC_PE_INSTLOADER_HPP
#define HYBRIDACC_PE_INSTLOADER_HPP

// ============================================================================
//  File        : InstLoader.hpp
//  Description : Instruction loader module responsible for splitting static
//                data into instructions and loading them into instruction memory.
//                Supports activation, reset, busy state indication, and completion notification.
//  Change Log  : 2025-10-05  Initial implementation of InstLoader module.
// ============================================================================

#include <systemc.h>
#include "hybridacc/utils.hpp"

using namespace sc_core;

namespace hybridacc {
namespace pe {

SC_MODULE(InstLoader) {
    // Ports
    sc_in<bool> clk("clk");
    sc_in<bool> rst_n("rst_n");

    sc_in<sc_uint<ADDR_BITS>> len_i; // Length of instructions to load
    sc_in<bool> en_i; // Enable signal
    sc_out<bool> done_o; // Done signal (1-cycle pulse at completion)
    sc_out<bool> busy_o; // Busy signal

    // Ports static
    sc_in<sc_uint<PORT_STATIC_WIDTH>> ps_i; // Static port input
    sc_in<bool> ps_valid_i;
    sc_out<bool> ps_ready_o;

    // Ports for InstMemory
    sc_out<bool>                 im_en_o{"im_en_o"};
    sc_out<bool>                 im_w_en_o{"im_w_en_o"};
    sc_out<sc_uint<ADDR_BITS>>   im_addr_o{"im_addr_o"};
    sc_out<sc_uint<INST_WIDTH>>  im_wr_data_o{"im_wr_data_o"};

    // Constructor
    SC_CTOR(InstLoader) {
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        SC_METHOD(comb_proc);
        sensitive << rst_n << en_i << len_i << ps_valid_i << ps_i
                   << state_r << count_r << slice_rem_r << ps_data_r << done_r;
    }

private:
    static constexpr unsigned INST_PER_STATIC = PORT_STATIC_WIDTH / INST_WIDTH;

    enum LoaderState { S_IDLE, S_LOAD, S_DONE}; // FSM states

    // Registers
    sc_signal<LoaderState> state_r, state_n;
    sc_signal<sc_uint<ADDR_BITS>> count_r, count_n;          // 已寫入的指令數 (也做為下一個寫入位址)
    sc_signal<sc_uint<8>> slice_rem_r, slice_rem_n;          // 當前 ps_data 尚未送出的指令數 (0 代表需要新資料)
    sc_signal<sc_uint<PORT_STATIC_WIDTH>> ps_data_r, ps_data_n; // Latched static word

    // Sequential process
    void seq_proc() {
        if (!rst_n.read()) {
            state_r.write(S_IDLE);
            count_r.write(0);
            slice_rem_r.write(0);
            ps_data_r.write(0);
        } else {
            state_r.write(state_n.read());
            count_r.write(count_n.read());
            slice_rem_r.write(slice_rem_n.read());
            ps_data_r.write(ps_data_n.read());
        }
    }

    // Combinational process
    void comb_proc() {
        // Default next = current
        state_n      = state_r.read();
        count_n      = count_r.read();
        slice_rem_n  = slice_rem_r.read();
        ps_data_n    = ps_data_r.read();
        done_n       = false;              // 產生單循環脈衝

        // Default outputs
        done_o.write(state_r.read() == S_DONE);
        busy_o.write(state_r.read() == S_LOAD);
        im_en_o.write(state_r.read() == S_LOAD);
        im_w_en_o.write(false);
        im_addr_o.write(count_r.read());  // 當 cycle 有寫入時代表該地址被寫
        im_wr_data_o.write(0);
        ps_ready_o.write(false);

        if (!rst_n.read()) {
            // Reset outputs (部分已在 seq reset，這裡確保組合輸出同步)
            done_o.write(false);
            busy_o.write(false);
            im_en_o.write(false);
            im_w_en_o.write(false);
            im_addr_o.write(0);
            im_wr_data_o.write(0);
            ps_ready_o.write(false);
            return;
        }

        switch (state_r.read()) {
            case S_IDLE: {
                if (en_i.read()) {
                    // 啟動載入
                    state_n.write(S_LOAD);
                    count_n.write(0);
                    slice_rem_n.write(0);          // 需要新資料
                }
                break;
            }
            case S_LOAD: {
                // 還有剩餘指令要載入 ?
                bool all_done = (count_r.read() >= len_i.read());
                if (all_done) {
                    // 結束 (理論上不應進來, 因為完成當下會轉 IDLE)
                    state_n.write(S_DONE);
                    busy_o.write(false);
                    im_en_o.write(false);
                    ps_ready_o.write(false);
                    break;
                }

                // 需要新取一個 static word
                bool need_new_word = (slice_rem_r.read() == 0);
                ps_ready_o.write(need_new_word && (count_r.read() < len_i.read())); // ready when need new word from PS port

                // 取新資料
                if (need_new_word && ps_valid_i.read() && ps_ready_o.read()) { // Handshake
                    // 擷取新資料
                    ps_data_n.write(ps_i.read());
                    // 設定剩餘可送出數
                    unsigned total_rem = INST_PER_STATIC;
                    // 如果剩餘指令不足整個 static word，只需要那麼多
                    unsigned remain_instr = (unsigned)(len_i.read().to_uint() - count_r.read().to_uint());
                    if (remain_instr < total_rem) total_rem = remain_instr;
                    slice_rem_n.write(total_rem); // 將於後面輸出流程中遞減
                }

                // 輸出一筆指令 (若 slice_rem_r > 0 表示有 latched data 可用)
                if (slice_rem_r.read() > 0) {
                    // 計算 index：用 (original_count_in_word - slice_rem) 方式
                    unsigned total_for_word = INST_PER_STATIC;
                    unsigned used_in_word = total_for_word - slice_rem_r.read(); // 0-based
                    unsigned hi = (used_in_word + 1) * INST_WIDTH - 1;
                    unsigned lo = used_in_word * INST_WIDTH;
                    sc_uint<INST_WIDTH> inst = ps_data_r.read().range(hi, lo);

                    im_w_en_o.write(true);
                    im_wr_data_o.write(inst);
                    im_addr_o.write(count_r.read());

                    // 更新計數
                    count_n.write(count_r.read() + 1);
                    slice_rem_n.write(slice_rem_r.read() - 1);

                    // 是否完成全部指令
                    if (count_n.read() >= len_i.read()) {
                        state_n.write(S_DONE);
                    }
                }
                break;
            }
            case S_DONE: {
                // 完成後回到閒置
                state_n.write(S_IDLE);
                break;
            }
            default:
                state_n.write(S_IDLE);
                break;
        }
    }
};

} // namespace pe
} // namespace hybridacc

#endif // HYBRIDACC_PE_INSTLOADER_HPP