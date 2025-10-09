#ifndef HYBRIDACC_PE_CONTROLLER_HPP
#define HYBRIDACC_PE_CONTROLLER_HPP
// ============================================================================
//  File        : controller.hpp
//  Module      : PE::Controller
//  Description : 負責解析指令與協調 DataLoader / VALU / 資料路徑。
// ============================================================================
#include <systemc.h>
#include <cstdint>
#include <vector>
#include "hybridacc/utils.hpp"
#include "hybridacc/pe/VALU.hpp"

namespace hybridacc {
namespace pe {

class PEController : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    sc_in<bool> en_i{"en_i"};
    sc_out<bool> stall_o{"stall_o"};

    // Inst Memory interface (Read Only)
    sc_out<sc_uint<ADDR_BITS>>   im_addr_o{"im_addr_o"};
    sc_in<sc_uint<16>>         rd_data_i{"rd_data_i"};

    // Loop Stack interface
    sc_out< sc_uint<ADDR_BITS> >      pc_jump_o{"lpstk_pc_jump_o"};
    sc_out< sc_uint<LOOP_COUNT_BITS> > loop_count_o{"lpstk_loop_count_o"};

    sc_out<bool> push_o{"lpstk_push_o"};
    sc_out<bool> pop_o{"lpstk_pop_o"};
    sc_out<bool> jump_o{"lpstk_jump_o"};

    sc_in< sc_uint<ADDR_BITS> > pc_jump_i{"lpstk_pc_jump_i"};
    sc_in<bool>                 empty_i{"lpstk_empty_i"};
    sc_in< sc_uint<LOOP_COUNT_BITS> > top_count_i{"lpstk_top_count_i"}; // 新增: 迴圈頂層剩餘次數

    // DataLoader interface
    sc_out<sc_uint<10>> dm_addr_len_o{"dm_addr_len_o"};
    sc_out<bool> dm_set_addr_o{"dm_set_addr_o"};
    sc_out<bool> dm_set_len_o{"dm_set_len_o"};

    sc_out<sc_uint<3>> dm_mode_o{"dm_mode_o"};
    sc_out<sc_uint<3>> dm_stride_o{"dm_stride_o"};
    sc_out<bool> dm_wen_o{"dm_wen_o"}; // 目前未使用 (僅 load)

    sc_out<bool> dm_activate_o{"dm_activate_o"};
    sc_out<bool> dm_next_o{"dm_next_o"}; // 觸發取出預取資料並預取下一筆

    sc_in<bool> dm_done_i{"dm_done_i"};
    sc_in<bool> dm_busy_i{"dm_busy_i"};

    // VALU interface
    sc_out<VALUMODE> valumode_i; // Operation mode

    // Port IO interface
    sc_in<bool> pli_valid_i{"pli_valid_i"};
    sc_out<bool> pli_ready_o{"pli_ready_o"};
    sc_out<bool> plo_valid_o{"plo_valid_o"};
    sc_in<bool> plo_ready_i{"plo_ready_i"};
    sc_in<bool> ps_valid_i{"ps_valid_i"};
    sc_out<bool> ps_ready_o{"ps_ready_o"};
    sc_in<bool> pd_valid_i{"pd_valid_i"};
    sc_out<bool> pd_ready_o{"pd_ready_o"};

    // Partial Sum Register interface
    sc_out<bool> p_mode_o{"p_mode_o"};    // 0: scalar, 1: vector
    sc_out<sc_uint<5>> p_rs_idx_o{"p_rs_idx_o"};  // Read scalar index or vector index
    sc_out<sc_uint<5>> p_rd_idx_o{"p_rd_idx_o"};  // Write scalar index or vector index

    sc_out<bool> p_we_o{"p_we_o"};        // Write enable
    sc_out<bool> p_clear_o{"p_clear_o"};  // Clear all registers (independent of rst_n)

    // Transform Register interface
    sc_out<sc_uint<2>> t_vrs_idx_o{"t_vrs_idx_o"};  // Read vector window index (0..2) => 3 windows * 4 = 12
    sc_out<sc_uint<4>> t_rd_idx_o{"t_rd_idx_o"};    // Write scalar index (0..11)
    sc_out<sc_uint<3>> t_shift_mode_o{"t_shift_mode_o"};  // Shift mode (0:K3,1:K5,2:K7)

    sc_out<bool> t_wen_o{"t_wen_o"};          // Scalar write enable
    sc_out<bool> t_shift_en_o{"t_shift_en_o"}; // Shift enable (mutually exclusive with wen_i, shift 優先)
    sc_out<bool> t_clear_o{"t_clear_o"};      // Synchronous clear (independent of rst_n)

    SC_HAS_PROCESS(PEController);
    PEController(sc_core::sc_module_name name) : sc_module(name) {
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();
    }

    void add_traces(sc_trace_file* tf){
        if(!tf) return;
        sc_trace(tf, clk, name()+std::string(".clk"));
        sc_trace(tf, rst_n, name()+std::string(".rst_n"));
        sc_trace(tf, en_i, name()+std::string(".en"));
        sc_trace(tf, stall_o, name()+std::string(".stall"));
        sc_trace(tf, im_addr_o, name()+std::string(".pc"));
        sc_trace(tf, m_inst, name()+std::string(".inst"));
    }

private:
    // -------- Internal state --------
    uint8_t m_pc{0};
    sc_uint<16> m_inst{0}; // 當前指令 (若啟用同步取指則為前一拍 latch)
    bool m_use_sync_fetch{false}; // 未來可由建構子參數設定

    bool m_pending_dm_next{false}; // *N 指令延後觸發 dm_next

    enum class State : uint8_t {
        IDLE,
        RUNNING,
        WAIT_PLI,
        WAIT_PLO,
        WAIT_PS,
        WAIT_PD,
        WAIT_DM // DataLoader busy 等待
    } m_state = State::IDLE;

    // ---- ISA 欄位抽取工具 ----
    inline uint8_t fld_opcode(sc_uint<16> w) const { return (w >> 1) & 0x3; }
    inline uint8_t fld_funct2(sc_uint<16> w) const { return (w >> 3) & 0x3; }
    inline uint8_t fld_payload(sc_uint<16> w) const { return (w >> 5) & 0x7F; } // bits[11:5]
    inline uint8_t fld_func1(sc_uint<16> w) const { return (w >> 12) & 0x1; }
    inline uint8_t fld_func3(sc_uint<16> w) const { return (w >> 13) & 0x7; }
    inline bool    fld_loopend(sc_uint<16> w) const { return (w & 0x1) != 0; }

    inline uint16_t decode_compressed10(sc_uint<16> w) const {
        uint16_t func3 = fld_func3(w);
        uint16_t payload = fld_payload(w);
        uint16_t bit0 = (payload >> 6) & 0x1;
        uint16_t bits6_1 = payload & 0x3F;
        return (func3 << 7) | (bits6_1 << 1) | bit0; // 10-bit value
    }
    inline uint16_t decode_jump_imm(sc_uint<16> w) const {
        // imm 11 bits: (func3[2:0]=imm[9:7]), func1=imm[10], bit11(payload[6])=imm[0], payload[5:0]=imm[6:1]
        uint16_t imm = ( (fld_func3(w) & 0x7) << 7 )
                     | ( fld_func1(w) << 10 )
                     | ( ((w>>11)&1) )
                     | ( (fld_payload(w) & 0x3F) << 1 );
        return imm;
    }

    void seq_proc();
};

} // namespace pe
} // namespace hybridacc
#endif // HYBRIDACC_PE_CONTROLLER_HPP
