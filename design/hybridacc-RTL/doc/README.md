# HybridAcc RTL Doc Index

Repo-wide 操作入口請先看 [../../../doc/index.md](../../../doc/index.md)；本文件保留 RTL subsystem 細節索引。

本目錄只保留仍有實際用途的 RTL 文件，優先分成三類：

- 現況與里程碑：看目前 RTL 實作與後續收斂方向。
- 模擬與 regression：看怎麼執行、debug、判讀結果。
- 合成、時序與功耗：看 synthesis flow、STA 與 power 分析方式。

目前的 RTL 控制面是 entry Makefile 搭配 `mk/*.mk`；正式 Tcl/Python 腳本集中在 `script/tcl` 與 `script/python`，頂層 `script/` 不再保留 Tcl/Python wrapper。

## 1. 模擬與 Regression

- [sim_test_guide.md](sim_test_guide.md): 完整 RTL 模擬指南，涵蓋 unit test、NoC/Cluster workload、top-level firmware bring-up、gate-level flow。
- [rtl_fw_regression_README.md](rtl_fw_regression_README.md): single-wave RTL firmware regression 的精簡版操作手冊，包含 root cause、threshold 依據與 trace 開關。

## 2. 合成、時序與功耗

- [syn_guide.md](syn_guide.md): PE/NoC synthesis flow 與報告使用方式。
- [hybridacc_synthesis_guide.md](hybridacc_synthesis_guide.md): top-level 與 unit synthesis target、輸出命名、腳本位置的現行總覽。
- [primetime_guide.md](primetime_guide.md): 以 pt_shell 對 top-level HybridAcc gate netlist 做獨立 STA 的操作手冊。
- [primepower_guide.md](primepower_guide.md): 以 pt_shell 啟用 PrimePower analysis，結合 gate-level activity 做功耗分析的操作手冊。
- `script/python/reporting/build_signoff_dashboard.py`: 把 synthesis、PrimeTime、PrimePower 與 residual STA disposition 彙整成單一 static HTML signoff dashboard。
## 3. 歷史文件整理原則

以下類型的文件不再保留在本目錄：

- 已被現況文件取代的階段性轉換計畫。
- 僅記錄歷史批次進度、但不再作為現行操作依據的工作日誌。
- 與目前目錄狀態明顯不符的早期盤點文件。

這些內容若仍需追溯，請直接查 git history。