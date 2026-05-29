# HybridAcc Repo 分類基準

狀態：Phase 0 建立，並已同步反映後續 cleanup 決策

最後更新：2026-05-21

## 1. 目的

本文件用來定義目前 repo 的分類基準，作為後續整理、文件同步與腳本歸位的單一依據。

本輪分類已套用目前有效邊界：

1. design/hybridacc-RTL/sim/ 先保留。
2. design/hybridacc-RTL/report/ 先保留。
3. output/ 由使用者自行處理。
4. jgproject/ 已不在目前工作區。
5. libs/ 與 packages/ 保留，作為本地模擬 tools，且不納入 git。
6. 若其他 worktree 再出現 jgproject/，保留 Tcl 腳本檔與 report 類檔案，其餘刪除。

## 2. 五大分類定義

### 2.1 source

定義：正式原始碼、設定、手冊與長期維護入口。

目前包含：

1. README.md
2. pyproject.toml
3. doc/
4. design/
5. python/
6. scripts/
7. testbench/

### 2.2 generated

定義：可由工具、建置流程或模擬流程重建的產物。

目前包含：

1. design/hybridacc-RTL/build/
2. design/hybridacc-RTL/sim/
3. design/hybridacc-RTL/report/
4. output/
5. jgproject/（若存在）

備註：本輪雖然 design/hybridacc-RTL/sim/ 與 design/hybridacc-RTL/report/ 屬於 generated，但因仍需實驗數據，暫不清理。

### 2.3 scratch

定義：單次分析、命令紀錄、臨時報表、手動比對結果。

本輪已移除：

1. diff.log
2. report.txt
3. temp_report.txt
4. command.log
5. noc_ps_in_data_report.txt
6. script.awk
7. signals.list

### 2.4 archive

定義：不參與主線工作流、但暫時仍可能作為歷史參考的備份或保留副本。

目前可見代表項：

1. 任何未來保留的歷史報告快照
2. 若重新出現、但已不屬於現行工作流的歷史副本

### 2.5 local-tool

定義：本地安裝或複製的模擬/EDA/第三方工具依賴，工作流需要，但不納入 git。

目前可見代表項：

1. libs/systemc-2.3.3/
2. packages/systemc-2.3.3/

## 3. Top-Level 分類結果

| 路徑 | 分類 | 備註 |
|------|------|------|
| README.md | source | repo 入口文件 |
| pyproject.toml | source | Python/CLI 定義 |
| doc/ | source | repo 級規劃與手冊 |
| design/ | source | RTL/ESL/ISA/CC 設計主體 |
| python/ | source | Python 套件與 CLI |
| scripts/ | source | canonical shell 入口與輔助腳本 |
| testbench/ | source | 測試配置與案例 |
| libs/ | local-tool | 本地模擬工具依賴，保留且不納入 git |
| packages/ | local-tool | 本地模擬工具/套件副本，保留且不納入 git |
| output/ | generated | 本輪不處理，由使用者自行整理 |
| .venv/ | generated | 本地 Python 環境 |
| uv.lock | source | uv 依賴鎖定檔 |
| command.log 等 root 臨時檔 | scratch | 本輪已清理，不再保留在 root |

## 4. 第三方套件 canonical path

目前已確認 SystemC 的 canonical path 為：

1. libs/systemc-2.3.3/

判定依據：

1. scripts/install/configure_shell_env.sh 將 SYSTEMC_LIB 指到 libs/systemc-2.3.3。
2. scripts/fast_entry/cluster_test.sh 預設從 libs/systemc-2.3.3 取值。
3. design/hybridacc-ESL/README.md 也把 libs/systemc-2.3.3 視為工作區內 SystemC 位置。

目前 packages/systemc-2.3.3/ 保留為本地 tool/package 副本，不納入 git，也不列入 cleanup 候選。

## 5. 本輪整理邊界

### 5.1 本輪會處理

1. root scratch 檔案中的高信心候選。
2. README / guide 漂移修正。
3. scripts 分類與 canonical 入口同步。
4. 依使用者指示，補清除剩餘 root scratch 檔案。

### 5.2 本輪不處理

1. design/hybridacc-RTL/sim/
2. design/hybridacc-RTL/report/
3. output/
4. libs/
5. packages/

## 6. Phase 0 結論

本輪 Phase 0 已得到以下結論：

1. source / generated / scratch / archive / local-tool 的分類邊界已定義。
2. SystemC canonical path 已確定為 libs/systemc-2.3.3。
3. design/hybridacc-RTL/sim/ 與 design/hybridacc-RTL/report/ 雖屬 generated，但暫時保留。
4. output/ 由使用者自行處理，不納入本輪整理。
5. jgproject/ 在目前工作區中已不存在，無需執行刪除。
6. root scratch 檔案 diff.log、report.txt、temp_report.txt、command.log、noc_ps_in_data_report.txt、script.awk、signals.list 已完成清理。
7. libs/ 與 packages/ 已確認保留為本地模擬 tools，且不納入 git。
8. 若其他 worktree 再出現 jgproject/，保留 Tcl 腳本檔與 report 類檔案，其餘刪除。