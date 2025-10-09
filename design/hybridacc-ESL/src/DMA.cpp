// filepath: src/DMA.cpp
// ============================================================================
//  File        : DMA.cpp
//  Project     : HybridAcc ESL
//  Module      : DMA (Direct Memory Access Engine)
//  Description : 負責主記憶體 / 外部匯流排 與 內部緩衝 (如 Inst/Data Memory) 之間的搬移。
//
//  Responsibilities (預期職責):
//    - 指令/資料批次搬運 (burst transfer)
//    - 支援對齊/分塊/Stride 模式
//    - 可能的雙緩衝 (double buffering) 策略
//    - 與 Top / Sub Controllers 的請求佇列互動
//
//  Future Extensions:
//    - QoS / 優先權排程
//    - 帶寬/通道模型、延遲/擁塞統計
//    - AXI / Custom Bus 交易層抽象
//
//  Change Log:
//    - 2025-10-05  Initial skeleton file created.
// ============================================================================
#include "hybridacc/DMA.hpp"

namespace hybridacc {
// TODO: 定義 DMA 類別於 DMA.hpp 並在此實作行為 (run thread / API)
}
