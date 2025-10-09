// filepath: src/subcontroller.cpp
// ============================================================================
//  File        : subcontroller.cpp
//  Module      : SubController (Array-Level Controller)
//  Description : 管理單一 PE Array 內部的任務下發、同步與資源協調。
//
//  Responsibilities:
//    - 接收 TopController 任務描述並切分給各 PE / DLB
//    - 管理本地 DMA Request 佇列 (若分層)
//    - 提供 barrier / sync 機制
//
//  Future Extensions:
//    - 動態調整 Tiling / Mapping (自適應)
//    - 與能耗模型連結 (功耗估計 / DVFS hint)
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/array/subcontroller.hpp"
#include "hybridacc/array/DLB.hpp"
#include "hybridacc/array/array.hpp"

namespace hybridacc {

void SubController::submit_task(const TaskDesc& t) {
    if (m_dlb) m_dlb->push(t);
}

bool SubController::idle() const {
    return m_dlb ? (m_dlb->pending() == 0) : true;
}

void SubController::step() {
    if (!rst_n.read()) {
        m_dispatched = 0;
        return;
    }
    if (!m_dlb) return;
    TaskDesc t;
    if (m_dlb->pop(t)) {
        // 簡化：目前只計數，不實際指派至特定 PE
        ++m_dispatched;
    }
}

} // namespace hybridacc
