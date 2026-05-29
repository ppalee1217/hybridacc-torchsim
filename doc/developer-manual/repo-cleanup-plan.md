# HybridAcc Repo 整理計劃書

狀態：Phase 0~4 已完成，後續維護改依 repo-cleanup-guide.md

最後更新：2026-05-21

## 1. 目的

本計劃書用來先界定後續 repo 整理的範圍、優先順序與候選清單，避免直接在工作樹中邊看邊刪，造成重要 artefact、暫時仍需比對的結果或可追溯資訊被過早移除。

這份文件只做規劃，不授權任何實際刪除、搬移、重新命名或腳本收斂動作。

## 1.1 目前已確認的整理邊界

依 2026-05-20 與 2026-05-21 的最新指示，當前整理範圍先固定如下：

1. design/hybridacc-RTL/sim/ 先保留，因仍需實驗數據。
2. design/hybridacc-RTL/report/ 先保留，因仍需實驗數據。
3. output/ 不納入本輪整理，由使用者自行處理。
4. jgproject/ 可移除；但本輪確認工作區時，已未看到 jgproject/ 目錄，因此目前無需再執行刪除。
5. libs/ 與 packages/ 保留，作為模擬所需的本地 tools，維持 git ignore，不納入 git repo。
6. 若其他 worktree 後續再出現 jgproject/，保留 Tcl 與 report 類檔案，其餘 work/log/tmp/backup/lock 類內容一律刪除。

後續若要真的開始整理，應以這個邊界為優先，而不是重新打開 sim/report/output 的範圍。

## 1.2 本輪執行進度

截至 2026-05-21，本輪已完成下列項目：

1. Phase 0：新增 doc/repo-classification.md，完成 source/generated/scratch/archive/local-tool 分類，並確認 SystemC canonical path 為 libs/systemc-2.3.3。
2. Phase 1：清除 root 下高信心 scratch 檔案 diff.log、report.txt、temp_report.txt。
3. Phase 2：同步 README、python/README、ESL guide、RTL guide 與 profiling note 的 canonical 入口與新腳本路徑。
4. Phase 3：建立 scripts/check/ 與 scripts/analysis/，並完成腳本歸位與 setup.sh fast 入口同步。
5. 補清除 root scratch 檔案 command.log、noc_ps_in_data_report.txt、script.awk、signals.list。
6. 確認 libs/ 與 packages/ 保留為本地模擬 tools，並維持 git ignore。
7. 檢查可達的 git worktree；目前只看到本工作區，且未找到 jgproject/。
8. 刪除 design/hybridacc-RTL/report/ 下的舊版 timestamped top-level 報告。
9. 補定義並採用 design/hybridacc-RTL/report/ 的保留層級與 baseline 命名規則。
10. 新增 design/hybridacc-RTL/report/README.md，標明 baseline、working、disposable 與命名規則。
11. 新增 doc/repo-cleanup-guide.md，將本輪決策收斂為後續開發者必須遵守的維護規則。

## 2. 整理目標

後續 repo 整理建議鎖定五個目標：

1. 降低 repo root 的雜訊，讓 source、docs、generated artefacts、scratch 檔案分層清楚。
2. 把 generated / cached / transient artefacts 從 source 觀點切乾淨。
3. 收斂 scripts 的 canonical entrypoint，避免多個近似入口同時存在。
4. 把文件與程式碼責任邊界拉清楚，避免 README、guide、臨時報告交錯。
5. 建立產物保留政策，避免 output/、report/、build/、jgproject/ 長期堆積。

## 3. 整理原則

後續執行時建議遵守以下原則：

1. 先分類，再刪除；不直接做大規模 rm。
2. 先清 generated artefacts，再碰 source tree。
3. 先收斂 canonical path，再移除舊入口。
4. 凡是仍可能拿來比對或追問題的合成、模擬、lint 結果，先移到 archive 區，不直接刪除。
5. 每一類刪除都要有可重建方式。
6. 所有 root-level ad hoc 檔案都要先判斷是否已被正式文件吸收。
7. 沒有明確 owner 或用途的檔案，不直接砍，先列入待確認清單。

## 4. 目前觀察到的整理問題

### 4.1 repo root 曾有明顯的臨時分析檔與單次產物

本輪開始前，root 下曾有多個不像正式 source 或正式文件的檔案：

1. command.log
2. diff.log
3. report.txt
4. temp_report.txt
5. noc_ps_in_data_report.txt
6. script.awk
7. signals.list

這些檔案的共同問題是：

1. 命名無法從 repo 結構直接判斷責任歸屬。
2. 很像一次性分析、比較、命令歷史或輔助資料。
3. 不適合長期留在 root。

截至目前，上述檔案已全數清除；後續若再出現同類臨時檔，應比照本輪處理，不再長期留在 repo root。

### 4.2 generated artefacts 雖大多被 ignore，但仍長期佔用工作樹視野

目前 repo 中有多個明確屬於 generated / transient 的重區域：

1. design/hybridacc-RTL/build/
2. design/hybridacc-RTL/sim/
3. design/hybridacc-RTL/report/
4. design/hybridacc-RTL/wave/
5. jgproject/
6. output/

雖然多數路徑已在 .gitignore 中，但對工作樹可讀性與日常定位 source 仍造成明顯干擾。

不過依目前已確認邊界，本輪不處理 design/hybridacc-RTL/sim/、design/hybridacc-RTL/report/ 與 output/；jgproject/ 則已被明確允許移除，但目前工作區中已不存在。

### 4.3 多份 timestamped 報告長期累積，缺少 retention policy

例如 design/hybridacc-RTL/report/ 下可見：

1. pre_sim_report_2026_04_05_22_03_45.md
2. pre_sim_report_2026_04_06_02_31_28.md
3. pre_sim_report_2026_05_18_05_14_16.md
4. post_sim_report_2026_04_06_01_45_44.md
5. post_sim_report_2026_04_06_02_30_06.md
6. post_sim_report_2026_04_06_02_41_25.md
7. pe_synthesis_report_2026_04_07_02_04_42.md
8. pe_synthesis_report_2026_04_07_19_51_26.md
9. pe_synthesis_report_2026_04_09_00_32_37.md

這些檔案若不做分級，之後會越來越難知道哪一份仍是有效 baseline。

依目前決策，這批 top-level 舊版 timestamped 報告已直接移除，不再作為長期保留形式。

### 4.4 jgproject 清理規則已確認

目前可達的 worktree 中未找到 jgproject/。

若其他 worktree 後續再出現 jgproject/，清理規則如下：

1. 保留 Tcl 腳本檔與 report 類檔案。
2. 非 Tcl、非 report 的 work/log/tmp/backup/lock 類內容一律刪除。
3. 不再保留 jg_console.log.bak、sessionLogs.bak/、work.bak/ 這類副本。

### 4.5 output/ 兼具正式結果與 scratch 實驗輸出，邊界不清楚

output/ 內混有多種不同層級內容：

1. 正式報告類：例如 fsdb-x-propagation-analysis.md、gate-high-z-static-analysis-report.md、syn_check_report.md。
2. 長期 sweep/result 類：例如 hacc-conv1x1-results、hacc-conv3x3-report、hacc-gemm-sweeps。
3. 單次驗證或暫時實驗類：例如 conv1x1-och-validate.BQgT0b、conv1x1-och-validate.i1q4gF、spm-bank-arb-check、analysis-wave-gap、debug-cluster-mismatch.log。
4. 臨時 session 類：例如 session.log、out-cluster-*.log。

如果後續不分類，output/ 會繼續成為所有中間結果的垃圾桶。

### 4.6 第三方套件與本地副本策略已確認

目前可見：

1. libs/systemc-2.3.3/
2. packages/systemc-2.3.3/

目前策略如下：

1. libs/ 與 packages/ 都保留，視為模擬所需的本地 tools。
2. 兩者都不納入 git repo，維持 git ignore。
3. libs/systemc-2.3.3/ 仍視為目前的 canonical runtime path。
4. packages/systemc-2.3.3/ 暫不清理，保留作本地 package/tool 副本。

### 4.7 文件與入口敘述曾出現 drift

本輪 Phase 2 已修補下列 drift：

1. root README 描述的 wrappers 目錄與實際 scripts 目錄不一致。
2. python/legacy_README.md 仍使用較舊的安裝與腳本示例。
3. ESL guide 與 setup.sh 的 fast entry surface 不一致。
4. RTL guide 已較完整，但仍有多份 guide/README 重複描述相近工作流。

後續重點不再是補舊 drift，而是避免文件再次回歸到舊入口。

### 4.8 scripts 已完成第一輪 canonical 化

目前 scripts/ 下已有：

1. setup.sh
2. fast_entry/
3. install/
4. gen/
5. check/
6. analysis/

這代表第一輪分類已落地；後續若再新增腳本，應直接依此結構歸位。

## 5. 後續建議整理範圍

### 5.1 第一類：可優先整理的 generated / transient artefacts

這一類原則上最安全，因為都應可重建。

候選範圍：

1. design/hybridacc-RTL/build/*
2. jgproject/.tmp/
3. jgproject/work/
4. jgproject/work.bak/
5. jgproject/sessionLogs/
6. jgproject/sessionLogs.bak/

建議動作：

1. 先排除仍在使用中的 build 與正在進行中的 synthesis 目錄。
2. jgproject/ 若在其他 worktree 出現，保留 Tcl 與 report 類檔案，其餘工作目錄與 backup 直接清理。
3. design/hybridacc-RTL/sim/ 與 design/hybridacc-RTL/report/ 目前不納入本批次。

### 5.1.1 design/hybridacc-RTL/report/ 的保留層級與 baseline 命名規則

目前採用的規則是把 design/hybridacc-RTL/report/ 內的內容分成三個保留層級：

1. baseline：長期要拿來對照的正式基準。
2. working：當前迭代中的活躍結果。
3. disposable：單次或已過時的快照，完成判讀後直接刪除。

採用對照如下：

1. baseline 保留 clock 維度目錄，例如 clk_1p25ns/、clk_2p00ns/，以及明確代表穩定版本的 module/report 目錄。
2. working 保留目前正在做分析的目錄，例如 probe_clk_1p25/ 與當前實驗需要的 hybridacc_* 結果。
3. disposable 不再保留 top-level timestamped markdown 快照，例如 pre_sim_report_*.md、post_sim_report_*.md、pe_synthesis_report_*.md。

baseline 命名規則採固定語意，不再用時間戳當主名稱：

1. clock baseline：clk_<period_ns>，例如 clk_1p25ns、clk_2p00ns。
2. top-level synthesis baseline：hybridacc_clk_<period_ns>_baseline。
3. post-sim baseline：hybridacc_postsim_clk_<period_ns>_baseline。
4. PE/module baseline：<module>_baseline 或 <module>_clk_<period_ns>_baseline。
5. 實驗性暫存結果：<topic>_working，不用時間戳做正式名稱。

若仍需要紀錄時間，建議把時間放在檔案內容 metadata 或附屬 manifest，而不是直接放進主要檔名。

採用的最小保留策略：

1. 每個 clock period 只保留一組 baseline 目錄。
2. 每種報告類型只保留一份目前有效 baseline。
3. 單次 markdown 摘要若已被其他正式文件吸收，直接刪除。
4. 若需要追溯歷史，優先把摘要整理進 doc/ 或 baseline 目錄中的 README，而不是散落成一批 timestamped md。

### 5.2 第二類：root-level ad hoc 檔案清理

本輪已完成刪除：

1. command.log
2. diff.log
3. report.txt
4. temp_report.txt
5. noc_ps_in_data_report.txt
6. script.awk
7. signals.list

建議動作：

1. 同類新檔若再次出現，優先判定是否已有正式文件吸收內容。
2. 若只是一次性分析，直接刪除或移到使用者指定的外部暫存位置。
3. 不再把一次性 scratch 檔長期留在 repo root。

### 5.3 第三類：output/ 的分類整理

本區塊先保留作後續參考；依目前指示，output/ 由使用者自行處理，不納入本輪整理。

後續建議把 output/ 重新切成四種子類型：

1. output/reports/
2. output/results/
3. output/checks/
4. output/archive/

整理時可按下列原則處理：

1. 正式可引用報告移到 reports/。
2. sweep/result 類保留在 results/。
3. 驗證性一次性檢查移到 checks/。
4. 帶隨機尾碼、明顯實驗性目錄移到 archive/ 或刪除。

高優先盤點候選：

1. conv1x1-och-validate.BQgT0b/
2. conv1x1-och-validate.i1q4gF/
3. analysis-wave-gap/
4. spm-bank-arb-check/
5. fsdb-smoke-z-scan/
6. fsdb-x-propagation-scan/
7. fw-validate-gemm/
8. hybridacc-sim-logs/
9. debug-cluster-mismatch.log
10. debug-trace-overlap.txt
11. out-cluster-conv_k3c4ich16och64s.log
12. out-cluster-conv_k3c4ich256och16.log
13. session.log

### 5.4 第四類：第三方套件位置收斂

目前已確認：

1. libs/ 與 packages/ 都保留。
2. 兩者都視為模擬所需的本地 tools。
3. 兩者都不納入 git repo。
4. libs/ 保持 canonical runtime path；packages/ 保留為本地 tool/package 副本。

這一類本輪不再列為 cleanup 候選。

### 5.5 第五類：文件歸位與命名收斂

建議後續規則：

1. repo-wide 操作/規劃文件進 doc/。
2. subsystem-specific guide 保留在對應子目錄，但要從 doc/index.md、doc/user-manual/ 或 doc/developer-manual/ 有明確入口。
3. plan 與 guide 分開存放，不混用。
4. 單次分析報告不應與正式手冊混放。

### 5.6 第六類：scripts 歸納與優化

本輪已把 scripts 收斂成以下層次：

1. scripts/setup.sh：唯一對外 shell 入口。
2. scripts/install/：安裝與環境設定。
3. scripts/fast_entry/：日常 workflow 快速入口。
4. scripts/gen/：資料與測資生成。
5. scripts/check/：檢查與驗證腳本。
6. scripts/analysis/：報告摘要、分析類工具。
7. scripts/admin/：維護性工具。

本輪已完成的搬遷如下：

1. 移除舊 compiler contract check script
2. uv run hacc-flat-fw-mem
3. 移除舊 layer0 check script
4. python/hybridacc_tools/wave_gap_summary.py / uv run hacc-wave-gap-summary
5. scripts/check/verify_pe_test.sh

### 5.7 第七類：入口面與說明文件同步

整理 scripts 時，建議同步做以下修補：

1. root README 與實際 scripts 結構對齊。
2. python/legacy_README.md 改為 uv-first，並更新示例入口。
3. scripts/setup.sh 的 fast 子命令與 fast_entry/ 目錄實際內容對齊。
4. ESL 與 RTL guide 裡統一標示 canonical entrypoint。

## 6. 建議執行分期

### Phase 0：盤點與標記（已完成）

內容：

1. 標出 source、generated、scratch、archive、local-tool 五類。
2. 為 output/、report/、jgproject/ 列出保留與淘汰候選。
3. 確定第三方套件 canonical path。

### Phase 1：低風險清理（已完成）

內容：

1. 若 jgproject/ 存在，清其 backup/work 類目錄。
2. 清 root-level 明顯臨時檔案。
3. 暫不處理 design/hybridacc-RTL/sim/、design/hybridacc-RTL/report/ 與 output/。

### Phase 2：文件與入口收斂（已完成）

內容：

1. 文件移到新版 doc 版圖。
2. 修補 README 與 guide drift。
3. 統一 shell / CLI 入口說明。

### Phase 3：scripts 重新歸類（已完成）

內容：

1. root scripts 搬到更明確的子分類。
2. 移除冗餘或不再建議直接使用的入口。
3. 建立 scripts 索引與命令對照表。

### Phase 4：第三方與歷史產物收尾（已完成）

內容：

1. libs/ 與 packages/ 的保留策略已確認：保留為本地模擬 tools，且不納入 git。
2. design/hybridacc-RTL/report/ 舊版 timestamped top-level 報告已直接移除。
3. output/ 中哪些結果屬於長期保留基準，由使用者自行決定。
4. design/hybridacc-RTL/report/ 的保留層級與 baseline 命名規則已定義並採用。
5. 若其他 worktree 後續再出現 jgproject/，保留 Tcl 與 report 類檔案，其餘刪除。
6. design/hybridacc-RTL/report/ 已補 README，作為 baseline/working/disposable 的現地說明入口。
7. 本輪整理決策已轉寫為 doc/repo-cleanup-guide.md，供後續維護直接遵守。

## 7. 建議刪除候選清單

本節只列候選，不代表現在就刪除。

### 7.1 目前 workspace 中暫無剩餘高優先候選

本輪已處理：

1. command.log
2. diff.log
3. report.txt
4. temp_report.txt
5. noc_ps_in_data_report.txt
6. script.awk
7. signals.list
8. pre_sim_report_2026_04_05_22_03_45.md
9. pre_sim_report_2026_04_06_02_31_28.md
10. pre_sim_report_2026_05_18_05_14_16.md
11. post_sim_report_2026_04_06_01_45_44.md
12. post_sim_report_2026_04_06_02_30_06.md
13. post_sim_report_2026_04_06_02_41_25.md
14. pe_synthesis_report_2026_04_07_02_04_42.md
15. pe_synthesis_report_2026_04_07_19_51_26.md
16. pe_synthesis_report_2026_04_09_00_32_37.md

備註：

1. output/ 內候選項目由使用者自行處理，因此不列入這裡。
2. jgproject/ 在目前可達的 worktree 中不存在；若其他 worktree 再次出現，仍可視為高優先 cleanup 候選，但保留 Tcl 與 report 類檔案。

### 7.2 目前無剩餘待確認刪除候選

目前 workspace 中，原先待確認的 root scratch 與 libs/packages 去留都已定案。

### 7.3 暫不建議動的項目

1. design/hybridacc-RTL/sim/。
2. design/hybridacc-RTL/report/ 中目前仍作為 baseline 或 working 的目錄。
3. output/。
4. 目前仍在使用或剛完成驗證的 rtl-fw-regress artefact。
5. 正在進行中的 1p25ns synthesis 相關 build/report。
6. 已被正式報告引用的分析文件。
7. 現有 source tree 與 testbench tree。
8. libs/ 與 packages/ 本地工具目錄。

## 8. 驗收標準

後續真正執行整理時，建議以以下結果作為驗收標準：

1. repo root 只保留 source、設定、正式文件與必要入口。
2. 所有 generated artefacts 都能被辨識為可重建內容。
3. scripts 對外只剩一組明確 canonical 入口。
4. README 與 guide 不再描述不存在的路徑或舊命令。
5. output/、report/、jgproject/ 都有清楚 retention policy。
6. 整理後不影響既有主要工作流：ESL、RTL sim、firmware regression、synthesis、lint/formal。

## 9. 本計劃書完成後的維護入口

本輪原先界定的整理工作已完成；後續不再以本文件作為操作清單，而是改由下列文件承接日常維護規則：

1. doc/repo-cleanup-guide.md：後續開發者必須遵守的 cleanup 規範。
2. doc/repo-classification.md：cleanup 前判定 source/generated/scratch/archive/local-tool 的分類基準。
3. design/hybridacc-RTL/report/README.md：report 目錄內 baseline、working、disposable 與命名規則的現地說明。

後續維護要求如下：

1. 若未取得明確授權，不重新打開 design/hybridacc-RTL/sim/、output/、libs/、packages/ 的整理範圍。
2. 若未來其他可達 worktree 再出現 jgproject/，保留 Tcl 與 report 類檔案，其餘直接清理。
3. 若要變更 report 的保留層級、baseline 命名或 scripts canonical entrypoint，必須同步更新對應 guide/README。
4. 在收到明確命令前，不進行任何超出既定邊界的整理、搬移或刪除動作。