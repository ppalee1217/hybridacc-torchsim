#ifndef HYBRIDACC_HYBRIDACC_HPP
#define HYBRIDACC_HYBRIDACC_HPP
// ============================================================================
//  File        : hybridacc.hpp
//  Project     : HybridAcc ESL
//  Description : 系統最上層聚合類別，封裝控制器、陣列、DMA、NoC 建構與生命週期。
//
//  Responsibilities:
//    - 保存各主要元件指標 (TopController, DMA, PE Arrays, NoC)
//    - 對外提供 initialize / load_binary / run / stop / stats 等 API 骨架
//    - 之後可掛接 trace / profile / log 管線
//
//  Future Extensions:
//    - 多配置載入 (platform.json)
//    - 熱插拔 (動態建立/銷毀部分模組)
// ============================================================================
#include <memory>
#include <vector>
#include <string>
#include <systemc.h>
#include "hybridacc/topcontroller.hpp"
#include "hybridacc/DMA.hpp"
#include "hybridacc/noc/noc.hpp"
#include "hybridacc/array/array.hpp"

namespace hybridacc {

struct PlatformDesc {
    unsigned arrays{1};
    unsigned pe_rows{1};
    unsigned pe_cols{1};
};

class HybridAccelerator {
public:
    HybridAccelerator() = default;
    ~HybridAccelerator(); // 於 cpp 定義，確保看到完整類別

    bool initialize(const PlatformDesc& desc);
    bool load_binary(const std::string& path); // placeholder
    void run();   // 之後可支援事件/條件
    void stop();  // 設置旗標
    void report_stats(std::ostream& os) const; // 輸出統計骨架

private:
    PlatformDesc m_desc{};
    std::unique_ptr<TopController> m_top{};
    std::unique_ptr<DMA>           m_dma{};
    std::unique_ptr<NoC>           m_noc{};
    std::vector<std::unique_ptr<PEArray>> m_arrays; // 每個 array 持有多顆 PE
    bool m_running{false};
};

} // namespace hybridacc
#endif // HYBRIDACC_HYBRIDACC_HPP
