// filepath: src/hybridacc.cpp
// ============================================================================
//  File        : hybridacc.cpp
//  Project     : HybridAcc ESL
//  Description : 系統層級 (Top-Level Accelerator) 相關整合/工廠函式的預留檔案。
//
//  Responsibilities (預期職責):
//    - 提供建立整個 Hybrid Accelerator 模型的工廠函式
//    - 彙整/包含主要組件初始化順序 (PE Array, NoC, DMA, Controllers)
//    - 之後可放置全域模擬統計 (performance counter aggregation)
//
//  Design Notes:
//    - 目前 hybridacc.hpp 為空，後續可定義主類別 HybridAccelerator
//    - 若未來需要多個 SoC / 多 instantiation，可提供參數化建構介面
//
//  Extension Ideas:
//    - JSON / YAML Platform 解析 (對應 config/platform.json)
//    - 動態建立不同拓樸 (mesh / torus / hierarchy)
//    - 模擬結束時輸出 profile / trace
//
//  Change Log:
//    - 2025-10-05  Initial skeleton file created.
// ============================================================================
#include "hybridacc/hybridacc.hpp"
#include "hybridacc/topcontroller.hpp"
#include "hybridacc/DMA.hpp"
#include "hybridacc/noc/noc.hpp"
#include "hybridacc/array/array.hpp"
#include "hybridacc/array/subcontroller.hpp"
#include "hybridacc/array/DLB.hpp"

namespace hybridacc {

HybridAccelerator::~HybridAccelerator() = default;

// 簡化: 僅建立元件指標 (尚未連接時脈/重置信號等 SystemC wiring)
bool HybridAccelerator::initialize(const PlatformDesc& desc) {
    m_desc = desc;
    // 建立 DMA / NoC / TopController (放在 unique_ptr; 真正 SystemC 模組可延後)
    if (!m_dma) m_dma = std::make_unique<DMA>("dma");
    if (!m_noc) m_noc = std::make_unique<NoC>("noc");
    if (!m_top) m_top = std::make_unique<TopController>("top_ctrl");

    // 建立 Array 集合
    m_arrays.clear();
    for (unsigned a = 0; a < desc.arrays; ++a) {
        PEArrayDesc adesc{desc.pe_rows, desc.pe_cols};
        auto arr = std::make_unique<PEArray>((std::string("pe_array_") + std::to_string(a)).c_str(), adesc);
        m_arrays.push_back(std::move(arr));
    }
    return true;
}

bool HybridAccelerator::load_binary(const std::string& path) {
    (void)path; // TODO: 讀取組譯後指令並分配至 InstMemory
    return true;
}

void HybridAccelerator::run() { m_running = true; }
void HybridAccelerator::stop() { m_running = false; }

void HybridAccelerator::report_stats(std::ostream& os) const {
    os << "HybridAccelerator Stats: arrays=" << m_arrays.size()
       << " running=" << (m_running?"yes":"no") << "\n"; }

} // namespace hybridacc
