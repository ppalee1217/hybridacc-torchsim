// filepath: src/DLB.cpp
// ============================================================================
//  File        : DLB.cpp
//  Module      : DLB (Dynamic Load Balancer / 或 Dispatch Logic Block)
//  Description : 提供工作/資料在多個 PE / Array 之間的動態分派策略骨架。
//
//  Responsibilities (預期):
//    - 追蹤各 PE 負載 (queue 深度 / utilization)
//    - 動態選擇目標 PE 以降低 hotspot
//    - 與 SubController / TopController 溝通取得任務描述
//
//  Future Extensions:
//    - 支援多種策略 (RoundRobin / LeastLoaded / PriorityClass)
//    - 控制訊息壅塞 (backpressure) 管理
//    - 統計輸出 (load distribution histogram)
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/array/DLB.hpp"

namespace hybridacc {
// TODO: 在 DLB.hpp 定義對應類別並於此實作方法。
} // namespace hybridacc
