#include "hybridacc/pe/pe.hpp"
#include <iostream>

namespace hybridacc {

void PE::configure(unsigned id) {
    // 將線性 id 轉為 row/col (簡化: row=0 col=id)
    pe_id = PE_ID(0, id, 0);
}

void PE::load_task(int task_id) { current_task = task_id; }

void PE::run() {
    // 簡化執行緒：等待 reset 解除，然後空迴圈 (未來加入運算/通訊)
    while (true) {
        wait();
        if (!rst_n.read()) {
            current_task = -1;
            continue;
        }
        // TODO: 與 DataLoader / Controller 整合
    }
}

} // namespace hybridacc
