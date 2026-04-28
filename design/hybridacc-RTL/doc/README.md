# HybridAcc RTL Doc Index

本目錄只保留仍有實際用途的 RTL 文件，優先分成三類：

- 現況與里程碑：看目前 RTL 實作與後續收斂方向。
- 模擬與 regression：看怎麼執行、debug、判讀結果。
- 合成與時序：看 synthesis flow 與特定時序議題。

## 1. 現況與里程碑

- [implement_plan.md](implement_plan.md): 目前 RTL 實作盤點、驗收目標與後續執行順序。

## 2. 模擬與 Regression

- [sim_test_guide.md](sim_test_guide.md): 完整 RTL 模擬指南，涵蓋 unit test、NoC/Cluster workload、top-level firmware bring-up、gate-level flow。
- [rtl_fw_regression_README.md](rtl_fw_regression_README.md): single-wave RTL firmware regression 的精簡版操作手冊，包含 root cause、threshold 依據與 trace 開關。

## 3. 合成與時序

- [syn_guide.md](syn_guide.md): PE/NoC synthesis flow 與報告使用方式。
- [noc_timing_improvement_plan.md](noc_timing_improvement_plan.md): NoC 相關時序瓶頸分析與改善方向。

## 4. 歷史文件整理原則

以下類型的文件不再保留在本目錄：

- 已被現況文件取代的階段性轉換計畫。
- 僅記錄歷史批次進度、但不再作為現行操作依據的工作日誌。
- 與目前目錄狀態明顯不符的早期盤點文件。

這些內容若仍需追溯，請直接查 git history。