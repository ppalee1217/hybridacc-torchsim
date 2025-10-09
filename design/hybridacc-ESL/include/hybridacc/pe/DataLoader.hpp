#ifndef HYBRIDACC_PE_DATALOADER_HPP
#define HYBRIDACC_PE_DATALOADER_HPP
// ============================================================================
//  File        : DataLoader.hpp (refactored)
//  Change      : 拆分為 seq_proc (註冊更新) 與 comb_proc (next-state + 輸出)
// ============================================================================
#include <systemc.h>
#include <cstdint>
#include <hybridacc/utils.hpp>

namespace hybridacc { namespace pe {

SC_MODULE(DataLoader) {
public:
    // ---- Clock / Reset ----
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    // ---- Control Inputs ----
    sc_in<sc_uint<10>> addr_len_i{"addr_len_i"};
    sc_in<bool> set_addr_i{"set_addr_i"};
    sc_in<bool> set_len_i{"set_len_i"};

    sc_in<sc_uint<3>> mode_i{"mode_i"};
    sc_in<sc_uint<3>> stride_i{"stride_i"};
    sc_in<bool> wen_i{"wen_i"};          // 1 => store (DMA.SD) 模式
    sc_in<bool> activate_i{"activate_i"};
    sc_in<bool> next_i{"next_i"};         // 取出預取 + 啟動下一筆 (load 模式)

    // ---- Status Outputs ----
    sc_out<bool> done_o{"done_o"};        // 單拍完成 (或 comb 生成的一拍脈衝)
    sc_out<bool> busy_o{"busy_o"};        // store 模式期間為 1

    // ---- Data Memory Interface ----
    sc_out<bool>        dm_en_o{"dm_en_o"};
    sc_out<bool>        dm_wr_o{"dm_wr_o"};
    sc_out<sc_uint<10>> dm_addr_o{"dm_addr_o"};
    sc_out<sc_uint<8>>  dm_mask_o{"dm_mask_o"};
    sc_out<sc_uint<64>> dm_wdata_o{"dm_wdata_o"};
    sc_in<sc_uint<64>>  dm_rdata_i{"dm_rdata_i"};

    // ---- Load Data Output ----
    sc_out<sc_uint<64>> data_o{"data_o"};
    sc_out<bool> data_valid_o{"data_valid_o"};

    // ---- PortIO (store source) ----
    sc_in<bool>  ps_valid_i{"ps_valid_i"};
    sc_out<bool> ps_ready_o{"ps_ready_o"};
    sc_in<sc_uint<PORT_STATIC_WIDTH>> ps_data_i{"ps_data_i"};

    // ---- Constructor ----
    SC_CTOR(DataLoader) {
        SC_METHOD(seq_proc);  sensitive << clk.pos();  dont_initialize();
        SC_METHOD(comb_proc); sensitive
            << rst_n
            << base_addr_r << remaining_r << stride_r << mask_r
            << is_store_r << state_r
            << store_transfers_cnt_r << load_outputs_cnt_r
            << addr_len_i << set_addr_i << set_len_i
            << mode_i << stride_i << wen_i
            << activate_i << next_i
            << ps_valid_i << ps_data_i << dm_rdata_i;
        dont_initialize();
    }

private:
    // ---- State Registers ( *_r ) ----
    sc_signal<sc_uint<16>> base_addr_r;          // 當前位址
    sc_signal<sc_uint<16>> remaining_r;          // 尚未輸出(或寫入)的元素數
    sc_signal<sc_uint<16>> stride_r;             // bytes stride / element 單位已轉成 bytes
    sc_signal<sc_uint<8>>  mask_r;               // byte mask
    sc_signal<bool>        is_store_r;           // 當前工作是否 store

    // State encoding: 0=IDLE 1=BUSY 2=DONE
    sc_signal<sc_uint<2>>  state_r;

    sc_signal<sc_uint<32>> store_transfers_cnt_r; // coverage
    sc_signal<sc_uint<32>> load_outputs_cnt_r;    // coverage

    // ---- Combinational (next-state) shadow vars ----
    sc_uint<16> base_addr_n;
    sc_uint<16> remaining_n;
    sc_uint<16> stride_n;
    sc_uint<8>  mask_n;
    bool        is_store_n;
    bool clr_counters_flag;
    sc_uint<2>  state_n;
    sc_uint<32> store_transfers_cnt_n;
    sc_uint<32> load_outputs_cnt_n;

    // ---- Helper decode ----
    inline bool is_broadcast_mode(uint8_t m) const { return (m & 0x7) >= 4 && (m & 0x7) <= 6; }
    inline unsigned element_bytes(uint8_t m) const {
        switch(m & 0x7){ case 0: case 4: return 1; case 1: case 5: return 2; case 2: case 6: return 4; case 3: return 8; default: return 8; }
    }
    uint8_t    mode_to_mask(uint8_t mode) const;
    sc_uint<64> expand_broadcast(uint8_t mode, sc_uint<64> raw) const;

    // ---- Processes ----
    void seq_proc(); // 註冊更新
    void comb_proc(); // next-state + outputs

public:
    // ---- Coverage API ----
    unsigned get_store_transfers() const { return store_transfers_cnt_r.read(); }
    unsigned get_load_outputs()   const { return load_outputs_cnt_r.read(); }
    void clr_counters(){ clr_counters_flag = true;}

    void add_traces(sc_trace_file* tf) {
        if(!tf) return;
        sc_trace(tf, clk, name()+std::string(".clk"));

        sc_trace(tf, base_addr_r, name()+std::string(".base_addr"));
        sc_trace(tf, remaining_r, name()+std::string(".remaining"));
        sc_trace(tf, stride_r, name()+std::string(".stride"));
        sc_trace(tf, state_r, name()+std::string(".state"));
        sc_trace(tf, is_store_r, name()+std::string(".is_store"));
        sc_trace(tf, busy_o, name()+std::string(".busy_o"));
        sc_trace(tf, done_o, name()+std::string(".done_o"));
        sc_trace(tf, store_transfers_cnt_r, name()+std::string(".store_cnt"));
        sc_trace(tf, load_outputs_cnt_r, name()+std::string(".load_cnt"));

        sc_trace(tf, dm_en_o, name() + std::string(".dm_en_o"));
        sc_trace(tf, dm_wr_o, name() + std::string(".dm_wr_o"));
        sc_trace(tf, dm_addr_o, name() + std::string(".dm_addr_o"));
        sc_trace(tf, dm_mask_o, name() + std::string(".dm_mask_o"));
        sc_trace(tf, dm_wdata_o, name() + std::string(".dm_wdata_o"));
        sc_trace(tf, dm_rdata_i, name() + std::string(".dm_rdata_i"));

        sc_trace(tf, data_o, name()+std::string(".data_o"));
        sc_trace(tf, data_valid_o, name()+std::string(".data_valid_o"));

        sc_trace(tf, next_i, name()+std::string(".next_i"));
        sc_trace(tf, activate_i, name()+std::string(".activate_i"));

        sc_trace(tf, ps_valid_i, name() + std::string(".ps_valid_i"));
        sc_trace(tf, ps_ready_o, name() + std::string(".ps_ready_o"));
        sc_trace(tf, ps_data_i, name() + std::string(".ps_data_i"));
    }
};

}} // namespace hybridacc::pe
#endif // HYBRIDACC_PE_DATALOADER_HPP