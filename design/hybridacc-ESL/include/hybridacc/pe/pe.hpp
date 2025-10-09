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

namespace hybridacc {

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

    SC_HAS_PROCESS(PE);
    PE(sc_core::sc_module_name name) : sc_module(name) {
        SC_THREAD(run);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

    void configure(unsigned id);
    void load_task(int task_id);

private:
    PE_ID pe_id{};
    int current_task{-1};
    void run();
};

} // namespace hybridacc
#endif // HYBRIDACC_PE_PE_HPP