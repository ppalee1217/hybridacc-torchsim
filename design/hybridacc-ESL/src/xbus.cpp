// filepath: src/xbus.cpp
// ============================================================================
//  File        : xbus.cpp
//  Module      : NoC X-Direction Bus
//  Description : 模擬 X 向 (row-wise / horizontal) 資料/控制傳輸通道骨架。
//
//  Responsibilities (預期):
//    - 封裝橫向連線 (PE <-> PE / PE <-> Router) 的 ready/valid 或 credit 介面
//    - 提供資料注入 (inject) 與 接收 (receive) 方法
//    - 之後可統計每條 link 利用率 / 擁塞 / 延遲
//
//  Future Extensions:
//    - pipeline stage 數量參數化
//    - 錯誤注入 (bit flip) 模型
//    - flow control (Backpressure / Credits)
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/noc/xbus.hpp"

namespace hybridacc {
// TODO: 在 xbus.hpp 宣告 XBus 類別並於此實作其方法。
} // namespace hybridacc
