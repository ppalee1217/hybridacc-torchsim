// filepath: src/noc.cpp
// ============================================================================
//  File        : noc.cpp
//  Module      : NoC (Network-on-Chip) Fabric Core
//  Description : 負責不同 PE / Array / DMA / Controller 之間的資料與控制封包傳遞。
//
//  Responsibilities (預期):
//    - 管理路由拓樸 (Mesh / Crossbar / Hybrid)
//    - 封包佇列與流控 (Credit / Ready-Valid / VC 支援)
//    - 擁塞統計與延遲量測
//
//  Future Extensions:
//    - 多虛擬通道 (VC) 與 死結避免策略
//    - 優先權 / QoS 調度
//    - 功耗 / 時延 模型掛鉤
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/noc/noc.hpp"

namespace hybridacc {
// TODO: 在 noc.hpp 定義 NoC 類別與路由介面 API, 於此實作。
} // namespace hybridacc
