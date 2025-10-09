// filepath: src/topcontroller.cpp
// ============================================================================
//  File        : topcontroller.cpp
//  Module      : TopController
//  Description : 系統最高層控制元件，協調多個 Array / DMA / NoC 與 Host 介面。
//
//  Responsibilities:
//    - 接收 Host (或 trace) 所描述的 Kernel / Graph / Task 序列
//    - 進行 Tiling / Partition (可委派至 Mapping 工具)
//    - 下發至各 SubController / DLB 並追蹤完成狀態
//    - 全域 Barrier / Sync / Event Management
//    - 與 DMA 溝通記憶體搬運需求
//
//  Future Extensions:
//    - Runtime 自適應 (根據 PE 負載調整 mapping)
//    - 多 Kernel pipeline / 重疊 (overlap) 調度
//    - 能耗 / 效能 heuristic 選擇
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/topcontroller.hpp"
#include "hybridacc/array/subcontroller.hpp"
#include "hybridacc/array/DLB.hpp"

namespace hybridacc {
// 簡單 round-robin kernel -> TaskDesc 分派
void TopController::step() {
    if (!rst_n.read()) {
        m_kernel_queue.clear();
        m_dispatched = 0;
        return;
    }
    if (m_kernel_queue.empty() || m_subs.empty()) return;
    // 取出一個 kernel id
    uint32_t kid = m_kernel_queue.front();
    m_kernel_queue.erase(m_kernel_queue.begin());
    // 以固定拆分成 N 個子任務 (目前 N=1 示意)
    TaskDesc t{kid, 1};
    static size_t rr = 0;
    auto* target = m_subs[rr % m_subs.size()];
    target->submit_task(t);
    ++rr;
    ++m_dispatched;
}
} // namespace hybridacc
