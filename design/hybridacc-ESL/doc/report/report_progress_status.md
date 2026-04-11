# HybridAcc 全系統模擬 — 進度報告

> **日期**：2026-07
> **目標**：conv2d_3x3_sim 端到端模擬驗證
> **最終結果**：✅ **PASS** — 模擬完成 cycle 4563，cosine similarity = 0.99999982

---

## 1. 已確認正確的行為

| # | 項目 | 驗證方式 | 狀態 |
|---|------|----------|------|
| 1 | **DRAM Mirror 載入** | Sparse format 正確載入 1,062,144 bytes | ✅ |
| 2 | **Scan Chain 配置** | 48 筆 PE 配置，與 cluster_gen.py 完全一致 | ✅ |
| 3 | **AGU iter/stride/tag** | PS/PD/PLI/PLO 全部正確 (tag_ctrl=1 修正後) | ✅ |
| 4 | **DMA 搬運** | weight=144, input=256, output=784 beats 正確 | ✅ |
| 5 | **HDDU/PE 計算** | 42 PE parallel conv1d，正確產出 PLO 結果 | ✅ |
| 6 | **DMA 回寫** | SPM→DRAM 784 beats 完整寫回 | ✅ |
| 7 | **輸出正確性** | Cosine similarity = 0.99999982, MSE = 3.86e-7 | ✅ |
| 8 | **Firmware 流程** | boot → config → DMA load → compute → DMA store → ebreak | ✅ |

---

## 2. 已修復 Bug 總覽

| # | Bug | 檔案 | 修正 | 影響 |
|---|-----|------|------|------|
| 1 | PLI/PLO tag_ctrl | lowering.py | 2→1 | Tag routing 錯誤 |
| 2 | spm_map_odd | lowering.py | 0xD8→0xB4 | Group swap 編碼錯誤 |
| 3 | SPM pipeline depth | ComputeCluster.hpp | 1→3 | Timing 不匹配 |
| 4 | **PE OUTPUT_WINDOW_CNT** | lowering.py | tile_h*w-1→tile_w-1 | **ROOT CAUSE OF DEADLOCK** |

### Deadlock Root Cause (Bug #4)

PE 程式迴圈次數 = tile_h_out × tile_w_out = 196，但每 PE 只處理 tile_w_out = 14 columns。
消耗完 56 beats PLI 後 PE 永久等待 → NoCRouter PLO head-of-line blocking → 全系統 deadlock。

修正後 PE 迴圈次數 = tile_w_out = 14，正確匹配每 PE 的資料量。

---

## 3. 已完成驗證項目

| 項目 | 結果 |
|------|------|
| Conv2D 3×3 端到端 (單 wave) | ✅ PASS |
| PLO → SPM write 完整路徑 | ✅ 正常 |
| DMA store (SPM→DRAM) | ✅ 784 beats 正確回寫 |
| 計算正確性 (output vs golden) | ✅ cosine = 0.99999982 |

---

## 4. 尚未驗證項目

| 項目 | 預期驗證時機 |
|------|-------------|
| Ping/pong 雙緩衝 (多 wave) | 需 num_ic_tiles > 1 配置 |
| Conv1x1 operator | 獨立 testcase |
| GEMM operator | 獨立 testcase |
| 多 cluster | 需修改 cluster_mask |

---

## 5. 已知問題

### Simulator 假陽性 FAIL

Simulator 報告 `Total: 1 Pass: 0 Fail: 1 First-fail: 15` 為假陽性。
原因是 DSRAM test counter 區域 (offset 0x00-0x0C) 與 firmware .rodata (LayerConfig) 重疊。
實際輸出與 golden 比對結果為 PASS。
- PLI AGU (agus_2) 停在 `iter=[1,1,1,0]`
- **17/784** PLO 回應成功（PE 有非零輸出資料）
- **0 筆 SPM 寫入** (`wen=1`)— PLO 回應抵達 NoCRouter 但未流回 HDDU/SPM
- 288 筆 SPM group 2 (PLI) response FIFO full stalls
---

**詳細技術分析請參閱 `report_simulation_analysis_final.md`。**
