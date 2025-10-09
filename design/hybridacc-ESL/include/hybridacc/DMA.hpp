#ifndef HYBRIDACC_DMA_HPP
#define HYBRIDACC_DMA_HPP
// ============================================================================
//  File        : DMA.hpp
//  Project     : HybridAcc ESL
//  Module      : DMA (Direct Memory Access Engine)
//  Description : 抽象資料搬運引擎，提供非阻塞搬移請求/完成查詢 API 骨架。
//
//  Responsibilities:
//    - 提供記憶體/位址空間之間的傳輸請求 (host↔local / dram↔sram)
//    - 追蹤多筆 outstanding request (以 tag 識別)
//    - 模擬延遲 (未來: latency model / bandwidth throttle)
//
//  Basic API:
//    - submit_copy(src, dst, bytes) -> tag
//    - poll(tag)  : 查詢是否完成
//    - tick()     : 週期推進 (若無 SystemC 行為暫以函式觸發)
//
//  Future Extensions:
//    - Stride / 2D pitch copy
//    - Scatter / Gather
//    - QoS / Priority / Channel 分離
//    - 與 NoC 整合成封包傳輸 (目前僅抽象)
// ============================================================================
#include <systemc.h>
#include <cstdint>
#include <unordered_map>
#include <string>

namespace hybridacc {

struct DMACopyDesc {
    uint64_t src_addr{0};
    uint64_t dst_addr{0};
    uint32_t bytes{0};
    uint32_t remain_cycles{0}; // 簡單延遲模型 (bytes / bandwidth)
};

class DMA : public sc_core::sc_module {
public:
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};

    SC_HAS_PROCESS(DMA);
    DMA(sc_module_name name, uint32_t bandwidth_bytes_per_cycle = 16)
        : sc_module(name), m_bw(bandwidth_bytes_per_cycle) {
        SC_METHOD(step);
        sensitive << clk.pos();
        dont_initialize();
    }

    // 提交 copy 請求，回傳 tag
    uint64_t submit_copy(uint64_t src, uint64_t dst, uint32_t bytes) {
        uint64_t tag = ++m_last_tag;
        DMACopyDesc d{src, dst, bytes, calc_latency(bytes)};
        m_reqs.emplace(tag, d);
        return tag;
    }

    // 查詢是否完成
    bool poll(uint64_t tag) const {
        return m_reqs.find(tag) == m_reqs.end();
    }

    // (可選) 取得尚未完成筆數
    size_t pending() const { return m_reqs.size(); }

private:
    uint32_t m_bw; // bytes / cycle (簡化)
    uint64_t m_last_tag{0};
    std::unordered_map<uint64_t, DMACopyDesc> m_reqs; // 未完成

    uint32_t calc_latency(uint32_t bytes) const {
        return (bytes + m_bw - 1) / m_bw; // ceiling
    }

    void step() {
        if (!rst_n.read()) {
            m_reqs.clear();
            return;
        }
        // 遍歷遞減 latency
        for (auto it = m_reqs.begin(); it != m_reqs.end(); ) {
            if (it->second.remain_cycles > 0) {
                --(it->second.remain_cycles);
            }
            if (it->second.remain_cycles == 0) {
                it = m_reqs.erase(it); // 完成
            } else {
                ++it;
            }
        }
    }
};

} // namespace hybridacc

#endif // HYBRIDACC_DMA_HPP
