// filepath: src/array.cpp
// ============================================================================
//  File        : array.cpp
//  Module      : PE Array (Compute Fabric)
//  Description : 負責建立/管理 多個 PE 與其互聯 (透過 NoC / multicast)。
//
//  Responsibilities (預期):
//    - 產生/持有 PE 實例 (2D Grid / Cluster)
//    - 與 SubController / TopController 協同下達啟動/同步
//    - 提供廣播 (broadcast) 與 多播 (multicast) 介面封裝
//    - 匯總統計 (每個 PE 的 cycle / op count)
//
//  Future Extensions:
//    - 動態電源管理 (關閉/開啟某些 PE)
//    - Fault Injection / Resilience 模型
//    - 自適應資料路由策略
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/array/array.hpp"
#include "hybridacc/pe/pe.hpp"

namespace hybridacc {

PEArray::PEArray(sc_core::sc_module_name name, const PEArrayDesc& d)
    : sc_module(name), m_desc(d) {
    m_pes.reserve(m_desc.rows * m_desc.cols);
    for (unsigned r = 0; r < m_desc.rows; ++r) {
        for (unsigned c = 0; c < m_desc.cols; ++c) {
            std::string n = std::string(name) + "_pe_" + std::to_string(r) + "_" + std::to_string(c);
            m_pes.emplace_back(std::make_unique<PE>(n.c_str()));
            m_pes.back()->configure(r * m_desc.cols + c);
        }
    }
}

PE* PEArray::at(unsigned r, unsigned c) const {
    return m_pes[r * m_desc.cols + c].get();
}

} // namespace hybridacc
