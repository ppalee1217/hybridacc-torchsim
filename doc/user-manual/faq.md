# FAQ

## 1. 為什麼 `uv run ...` 可以，但 `make sim_*` 不行？

因為 Python / `uv` 與 Synopsys / VCS 依賴不同 shell 環境。`uv run ...` 可在 `bash` 正常工作，但 RTL / EDA 類 `make` target 應統一走 `tcsh -ic 'source ~/.tcshrc; ...'`。

## 2. 為什麼找不到 `riscv32-unknown-elf-gcc`？

RISC-V toolchain 尚未安裝，或 PATH 未更新。先回 [environment-and-toolchains.md](environment-and-toolchains.md) 檢查 `scripts/setup.sh install riscv` 與 `command -v`。

## 3. 為什麼 `hacc-compile` 可以跑，但後面沒有 `hardware_ir.json`？

多半是 compile 時沒加 `--dump-ir`，或 output dir 並不是你以為的那個目錄。

## 4. 為什麼換了 workload 之後，還要重生 `dram_init.bin` 與 `golden_output.bin`？

因為 DRAM 佈局與 golden 都受 `hardware_ir.json` 的 `tiling_params` 影響。workload 一變，舊 artefact 就不再可靠。

## 5. 為什麼 `sim_tb_hybridacc_sim` 跟 `rtl_regress_*` 看起來像同一件事？

不是。`sim_tb_hybridacc_sim` 是直接跑 simulator；`rtl_regress_*` 會先 compile workload、生成 golden、跑 RTL，再做 compare，所以更適合回歸測試。

## 6. 為什麼 single-wave regression compare fail？

先分兩類：

1. 大量 mismatch：先當 functional 問題查。
2. 少量 fp16 級誤差：先檢查 threshold 是否被 override，或 artefact 是否同一批。

## 7. 為什麼 trace 沒開出來？

firmware regression 的 trace 請放在 `RTL_FW_DEBUG_PLUSARGS`，不是一般 `SIM_PLUSARGS`。

## 8. 為什麼 gate sim 讀不到 netlist / SDF？

多半是 `CLOCK_PERIOD_NS` 與 synthesis 產物不一致，或指定的 `MOD_NAME` / `GATE_NETLIST_DIR` 不對。top gate sim 一般要用 `MOD_NAME=HybridAcc`，且 `GATE_NETLIST_DIR` 應指向 `syn/clk_<period>ns`。

## 9. 為什麼 `primepower_full` 需要 FSDB / VCD？

沒有 activity 也能跑，但只會退化成 vectorless analysis。要做 workload-aware power，應提供真實 FSDB / VCD / SAIF。

## 10. 為什麼 PrimePower 不是直接用 `pwr_shell`？

目前這個站點的實際可用 flow 是 `pt_shell + power_enable_analysis`。直接依賴 `pwr_shell` 會遇到設計讀入限制。

## 11. 為什麼 PrimeTime 與 synthesis 的結果看起來差很多？

若 constraints、library 與分析前提一致，兩者應該是同量級。差很多通常表示 constraint、library link 或分析前提不一致。

## 12. 為什麼 Superlint query script 跑了，但結果跟 main flow 對不起來？

因為 query helper 不是主流程本體。要先確認 canonical `jasper_superlint.tcl` 的 analyze / elaborate / waiver 狀態正確，再看 query script。

## 13. 為什麼 `doc/user-manual/`、`doc/developer-manual/` 和 subsystem guide 同時存在？

目前策略是 repo-wide manual 負責日常入口與維護規則，subsystem guide 保留 RTL/ESL 細節來源。日常先看 [../index.md](../index.md)，細節再跳到對應子系統文件。

## 14. 長期追加規則

FAQ 只收錄會重複被問到、且能用短答案快速導流的問題。

新增 FAQ 時請同時遵守：

1. 先更新對應 workflow manual 的完整說明，再把濃縮版答案放進 FAQ。
2. 每題至少要有「最短答案 + 第一個檢查點 + 導流到哪份 manual」。
3. 只保留高頻問題，避免把一次性的 session 細節直接塞進 FAQ。