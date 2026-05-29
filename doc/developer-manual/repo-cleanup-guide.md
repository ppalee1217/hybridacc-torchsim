# HybridAcc Repo Cleanup Guide

狀態：現行規範

最後更新：2026-05-21

## 1. 目的

本文件定義未來開發者在 HybridAcc repo 中進行整理、搬移、重新命名或刪除檔案時必須遵守的規則。

除非有更高優先的明確任務授權，cleanup 相關操作一律以本文件與 doc/repo-classification.md 為準。

## 2. 先做分類，再做動作

任何 cleanup 動作開始前，必須先把目標路徑歸到下列其中一類：

1. source：正式原始碼、設定、手冊與長期維護入口。
2. generated：可由 build、sim、lint、synthesis 或其他工具重建的產物。
3. scratch：單次分析、命令輸出、人工比對與暫時輔助檔。
4. archive：暫時保留的歷史參考副本，不參與主線工作流。
5. local-tool：本地安裝或複製的工具依賴，工作流需要，但不納入 git。

分類的正式定義與目前 repo 對應，依 doc/repo-classification.md。

## 3. 不可逾越的整理邊界

下列規則是目前的硬邊界；若沒有明確授權，不得突破：

1. design/hybridacc-RTL/sim/ 先保留，不做 cleanup。
2. design/hybridacc-RTL/report/ 先保留，只能依其內 README 做分級維護，不能任意清空。
3. output/ 由使用者自行處理；未獲授權不得重組、搬移或刪除。
4. libs/ 與 packages/ 保留為本地模擬 tools，不納入 git，也不列入 cleanup 候選。
5. 若其他 worktree 再出現 jgproject/，只保留 Tcl 腳本檔與 report 類檔案，其餘 work/log/tmp/backup/lock 類內容直接清理。

## 4. Root 與 scratch 檔案規則

1. repo root 不得長期放一次性分析檔、diff、命令紀錄、awk helper 或手工比對輸出。
2. 若 scratch 內容已被正式文件吸收，原 scratch 檔必須刪除。
3. 若 scratch 內容仍需短暫保留，應放到使用者指定位置或外部暫存區，而不是留在 repo root。
4. 沒有明確 owner 或用途的檔案，不直接刪除；先標記待確認。

## 5. Report 目錄規則

design/hybridacc-RTL/report/ 的具體規則以 design/hybridacc-RTL/report/README.md 為準；所有開發者至少要遵守以下原則：

1. 只保留 baseline、working、disposable 三種角色。
2. baseline 目錄必須使用固定語意命名，不得用時間戳當主名稱。
3. top-level timestamped markdown 快照不得重新引入。
4. 若 working 結果要升格為 baseline，必須同步更新命名與 README/manifest。
5. 若摘要內容已被 doc/ 或 baseline README 吸收，原始單次報告應刪除。

## 6. Scripts 與入口規則

1. scripts/setup.sh 是唯一對外 shell 入口；不要再新增平行的 root-level wrapper。
2. 新腳本必須直接放入對應子類別：scripts/install/、scripts/fast_entry/、scripts/gen/、scripts/check/、scripts/analysis/、scripts/admin/。
3. 若調整 canonical entrypoint，必須同步更新 README、python/README、ESL guide、RTL guide 與任何直接引用該入口的文件。
4. 不再用舊路徑或歷史 wrapper 名稱當文件示例。

## 7. 文件同步規則

1. repo-wide 規劃或操作規範放在 doc/。
2. subsystem-specific guide 留在對應子目錄，但要能從 doc/ 或該子系統 README 找到入口。
3. plan、guide、單次分析報告不能混放成同一類文件。
4. 路徑、命令、腳本入口一旦變更，相關 README/guide 要在同一輪修改中同步更新。

## 8. Local Tool 規則

1. libs/systemc-2.3.3/ 是目前 canonical runtime path。
2. packages/systemc-2.3.3/ 保留為本地 tool/package 副本。
3. local-tool 路徑不應被當成 source tree 整理，也不應納入 git。

## 9. Cleanup 前檢查清單

實際刪除、搬移或重命名前，至少完成下列檢查：

1. 確認目標屬於哪個分類。
2. 確認它是否可重建，或已有正式文件吸收其內容。
3. 確認它不是目前 active experiment、baseline、或使用者保留區的一部分。
4. 確認相依文件是否需要同步更新。
5. 若操作會影響 report 命名、scripts 入口或 jgproject 規則，先更新對應 README/guide。

## 10. 必須停手並升級確認的情況

遇到下列情況時，不要自行清理：

1. 無法判斷檔案是 source 還是 generated。
2. 無法確定某結果是否仍在當前實驗使用。
3. 目標位於 design/hybridacc-RTL/sim/、output/、libs/、packages/ 等保留區。
4. 需要跨 worktree 批次處理，且無法先核對保留規則。
5. 需要變更本文件已定義的硬邊界。

## 11. 維護責任

1. 任何改變 cleanup 規則的提交，必須同步更新本文件。
2. 任何改變分類邊界的提交，必須同步更新 doc/repo-classification.md。
3. 任何改變 report 保留層級或命名的提交，必須同步更新 design/hybridacc-RTL/report/README.md。

這份 guide 是後續 cleanup 的常設規範；若新任務與本文件衝突，以明確使用者指示為優先，並在任務完成後回頭同步修訂本文件。