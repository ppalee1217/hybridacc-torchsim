#ifndef HYBRIDACC_PE_PE_HPP
#define HYBRIDACC_PE_PE_HPP

/*
 * File        : pe.hpp
 * Project     : hybridacc-ESL
 * Module      : Processing Element (PE)
 * Brief       : 定義加速器中的處理元素介面與基本行為骨架。
 *
 * Responsibilities:
 *   - 接收/配置運算任務
 *   - 週期性執行運算 (可延伸 pipeline / latency 模型)
 *   - 與 Interconnect 或其他 PE 溝通
 *
 * Interfaces (新增三組 Ready/Valid 介面):
 *   1) PLI (Producer -> PE Input)  : 外部來源送進指令/資料 (pli_data/pli_valid/pli_ready)
 *   2) PLO (PE Output -> Consumer) : PE 將結果輸出 (plo_data/plo_valid/plo_ready)
 *   3) PSD (Peer Shared Data)      : 與共享/記憶體或其他單元雙向資料流
 *        - 輸入方向:  psd_in_data / psd_in_valid / psd_in_ready
 *        - 輸出方向:  psd_out_data / psd_out_valid / psd_out_ready
 *
 * Configurability:
 *   透過巨集 HYBRIDACC_PLI_WIDTH / HYBRIDACC_PLO_WIDTH / HYBRIDACC_PSD_WIDTH 覆寫位寬 (預設 32/32/64)
 *   在包含此檔前定義即可，例如:
 *     #define HYBRIDACC_PLI_WIDTH 64
 *     #include "hybridacc/pe.hpp"
 *
 * Public APIs:
 *   - configure(unsigned id) : 指派 PE 識別碼
 *   - load_task(int task_id) : 載入任務 (僅示意，可擴充參數)
 *
 * Helper Methods (本檔內提供):
 *   - bool pli_handshake() const
 *   - bool plo_handshake() const
 *   - bool psd_in_handshake() const
 *   - bool psd_out_handshake() const
 *
 * Future Extension Ideas:
 *   - 加入功耗/延遲統計與背壓/隨機延遲模型
 *   - 加入 QoS / Tag / Last / Burst 資訊
 *   - 與記憶體層 (scratchpad / cache 模型) 整合
 *
 * Change Log:
 *   2025-10-04  init skeleton
 *   2025-10-04  add PLI/PLO/PSD interfaces & width config
 */

#include <systemc.h>
#include <iostream>
#include "hybridacc/pe/DataLodaer.hpp"
#include "hybridacc/pe/DataMemory.hpp"
#include "hybridacc/pe/Decoder.hpp"
#include "hybridacc/pe/InstLoader.hpp"
#include "hybridacc/pe/InstMemory.hpp"
#include "hybridacc/pe/Loopstack.hpp"
#include "hybridacc/pe/PortIO.hpp"
#include "hybridacc/pe/Pregfile.hpp"
#include "hybridacc/pe/Tregfile.hpp"
#include "hybridacc/pe/VADD.hpp"
#include "hybridacc/pe/VMUL.hpp"

namespace hybridacc {
namespace pe {

class PE_ID {
public:
    PE_ID(unsigned int r = 0, unsigned int c = 0, unsigned int a = 0)
        : row(r), col(c), array(a) {}
    unsigned int get_row() const { return row; }
    unsigned int get_col() const { return col; }
    unsigned int get_array() const { return array; }
private:
    unsigned int row;
    unsigned int col;
    unsigned int array;
};

class PE : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};
    sc_in<bool> load_inst{"load_inst"};

    SC_HAS_PROCESS(PE);
    PE(sc_core::sc_module_name name) : sc_module(name) {
        // Create component instances
        create_components();

        // === Connections ===
        bind_components();

        // Sequential process
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();

        // Combinational process
        SC_METHOD(comb_proc);
        sensitive << load_inst << IF_decoded_fields << EXE_1_decoded_fields_r << EXE_2_decoded_fields_r << pc_r;
        dont_initialize();
    }

    ~PE();

private:

    void create_components();
    void bind_components();
    void comb_proc();
    void decode_fields_proc();
    void seq_proc();

    // ========================
    //   Components Instance
    // ========================
    // Independent I/O ports
    PortIO*        port_io;
    InstLoader*    inst_loader;

    // IF/ID stage
    InstMemory*    inst_memory;
    Loopstack*     loopstack;
    Decoder*       decoder;


    // EX1 stage
    DataLoader*    data_loader;
    DataMemory*    data_mem;
    Tregfile*      tregfile;
    VMUL*          vmul_unit;

    // EX2 stage
    Pregfile*      pregfile;
    VADD*          vadd_unit;


    // ========================
    //   FSM / Registers
    // ========================
    // Program Counter
    sc_signal<sc_uint<8>> pc_r; // 程式計數器
    sc_uint<8> pc_n; // 下一個程式計數器

    // T counter
    sc_signal<sc_uint<TCOUNTER_BITS>> tcounter_r; // 迴圈計數器
    sc_uint<TCOUNTER_BITS> tcounter_n; // 下一個迴圈計數器

    // P counter
    sc_signal<sc_uint<PCOUNTER_BITS>> pcounter_r; // 預取計數器
    sc_uint<PCOUNTER_BITS> pcounter_n; // 下一個預取計數器

    // Pipeline registers for decoded fields
    sc_signal<DECODED_FIELDS> IF_decoded_fields;
    sc_signal<DECODED_FIELDS> EXE_1_decoded_fields_r;
    sc_signal<DECODED_FIELDS> EXE_2_decoded_fields_r;

    DECODED_FIELDS EXE_1_decoded_fields_n;
    DECODED_FIELDS EXE_2_decoded_fields_n;

    // EXE1 stage
    sc_signal<vector_t> EXE2_vmul_out_r; // from VMUL unit

    // ========================
    //   Internal Signals
    // ========================
    // Stall signal
    sc_signal<bool> stall;

    // PortIO
    sc_signal<sc_uint<PORT_LOCAL_WIDTH>> pli;
    sc_signal<bool> pli_valid;
    sc_signal<bool> pli_ready;
    sc_signal<sc_uint<PORT_LOCAL_WIDTH>> plo;
    sc_signal<bool> plo_valid;
    sc_signal<bool> plo_ready;
    sc_signal<sc_uint<PORT_STATIC_WIDTH>> ps;
    sc_signal<bool> ps_valid;
    sc_signal<bool> ps_ready;
    sc_signal<sc_uint<PORT_DYNAMIC_WIDTH>> pd;
    sc_signal<bool> pd_valid;
    sc_signal<bool> pd_ready;


    //  InstMemory
    sc_signal<bool>        im_en;
    sc_signal<bool>                 im_w_en;
    sc_signal<sc_uint<ADDR_BITS>>   im_addr;
    sc_signal<sc_uint<16>>         im_rd_data;
    sc_signal<sc_uint<16>>         im_wr_data;

    // InstLoader
    sc_signal<sc_uint<ADDR_BITS>> imloader_len; // Length of instructions to load
    sc_signal<bool> imloader_en; // Enable signal
    sc_signal<bool> imloader_done; // Done signal (1-cycle pulse at completion)
    sc_signal<bool> imloader_busy; // Busy signal
    sc_signal<sc_uint<PORT_STATIC_WIDTH>> imloader_ps; // Static port input
    sc_signal<bool> imloader_ps_valid;
    sc_signal<bool> imloader_ps_ready;
    sc_signal<bool>                 imloader_im_en;
    sc_signal<bool>                 imloader_im_w_en;
    sc_signal<sc_uint<ADDR_BITS>>   imloader_im_addr;
    sc_signal<sc_uint<INST_WIDTH>>  imloader_im_wr_data;

    // Loopstack
    sc_signal< sc_uint<ADDR_BITS> >      pc_jump_i;
    sc_signal< sc_uint<LOOP_COUNT_BITS> > loop_count;
    sc_signal<bool> loop_push;
    sc_signal<bool> loop_pop;
    sc_signal<bool> loop_jump;
    sc_signal< sc_uint<ADDR_BITS> > pc_jump_o;
    sc_signal<bool>                 loop_empty;
    sc_signal< sc_uint<LOOP_COUNT_BITS> > loop_top_count; // 新增

    // DataLoader
    sc_signal<sc_uint<10>> dl_addr_len;
    sc_signal<bool> dl_set_addr;
    sc_signal<bool> dl_set_len;
    sc_signal<sc_uint<3>> dl_mode;
    sc_signal<sc_uint<3>> dl_stride;
    sc_signal<bool> dl_wen;          // 1 => store (DMA.SD) 模式
    sc_signal<bool> dl_activate;
    sc_signal<bool> dl_next;         // 取出預取 + 啟動下一筆 (load 模式)
    sc_signal<bool> dl_done;        // 單拍完成 (或 comb 生成的一拍脈衝)
    sc_signal<bool> dl_busy;        // store 模式期間為 1
    sc_signal<sc_uint<64>> data_o{"data_o"};
    sc_signal<bool> data_valid_o{"data_valid_o"};
    sc_signal<bool>  ps_valid_i{"ps_valid_i"};
    sc_signal<bool> ps_ready_o{"ps_ready_o"};
    sc_signal<sc_uint<PORT_STATIC_WIDTH>> ps_data_i{"ps_data_i"};

    // Tregfile
    sc_signal<sc_uint<2>> treg_vrs_idx;  // Read vector window index (0..2) => 3 windows * 4 = 12
    sc_signal<sc_uint<4>> treg_rd_idx;    // Write scalar index (0..11)
    sc_signal<sc_uint<3>> treg_shift_mode;  // Shift mode (0:K3,1:K5,2:K7)
    sc_signal<bool> treg_wen;          // Scalar write enable
    sc_signal<bool> treg_shift_en; // Shift enable (mutually exclusive with wen_i, shift 優先)
    sc_signal<bool> treg_clear;      // Synchronous clear (independent of rst_n)
    sc_signal<element_t> treg_rd_data;   // Write data (scalar input) TODO: rename wdata_i 未改以避免外部破壞
    sc_signal<vector_t> treg_vrs_data;  // Read data (vector output, 4 scalars slice)

    // VMUL unit
    sc_signal<vector_t> vmul_out; // output
};

} // namespace pe
} // namespace hybridacc
#endif // HYBRIDACC_PE_PE_HPP