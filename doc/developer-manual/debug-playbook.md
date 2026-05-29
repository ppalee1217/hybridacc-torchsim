# Debug Playbook

## 1. 適用範圍

這份文件是故障排查的第一層分流表。目標不是一次給出完整 root cause，而是用最短時間先判斷問題落在哪一層：

1. 環境 / 工具
2. compile
3. run / hang
4. compare / 功能
5. signoff / activity / parser

## 2. 先問自己哪一類故障

### 2.1 命令根本起不來

優先懷疑 shell / PATH / license。

### 2.2 compile 失敗

優先懷疑檔案、include、module、library。

### 2.3 run 會掛住或 fatal

優先懷疑 testbench 終止條件、runtime 卡住、wave 或 plusargs 問題。

### 2.4 compare fail

優先分成「整體功能錯」還是「少量 fp16 tolerance 邊界」。

## 3. 環境 / 工具問題

### 症狀

- `vcs`、`dc_shell`、`pt_shell` 找不到
- license / library path 錯誤

### 先做什麼

```bash
tcsh -ic 'source ~/.tcshrc; command -v vcs; command -v dc_shell; command -v pt_shell; command -v fsdb2vcd'
```

若只有 `uv run ...` 正常，但 `make sim_*` 不正常，問題多半不是 Python，而是 `tcsh` 與 EDA 環境。

## 4. RTL simulation / regression 問題

### 先看哪三個檔

1. `design/hybridacc-RTL/sim/log/<case>.compile.log`
2. `design/hybridacc-RTL/sim/log/<case>.run.log`
3. `design/hybridacc-RTL/sim/log/<case>.compare.log`

### compile fail 時

先查：

1. module / include path
2. library 或 DW 路徑
3. 工作目錄是不是 `design/hybridacc-RTL`

### run fail 或 hang 時

先查：

1. `fatal`
2. assertion
3. timeout
4. 測試是否真的走到 `$finish` 或 EBREAK

### compare fail 時

先分兩類：

1. 大量 mismatch：優先當 functional bug 查。
2. 少量 fp16 級誤差：先檢查 tolerance 與 artefact 是否是同一批。

## 5. single-wave firmware regression 問題

### 最常用 trace 開關

```bash
RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"
```

### 最常看路徑

- `output/rtl-fw-regress/<case>/`
- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_<case>.compile.log`
- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_<case>.run.log`
- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_<case>.compare.log`

### 已知高價值線索

GEMM 若出現不合理 mismatch，先把它當作 DMA submit / runtime sequencing 類問題，不要第一時間改 compare threshold。

若 `gate_regress_gemm_single_wave` 出現「每 48 個 output 只前 12 個正確、後面重複前段」這種 pattern，請直接回看 [gemm-gate-debug-chain.md](gemm-gate-debug-chain.md)。目前已確認的高價值結論是：一旦 gate backpressure 讓 `p2pe0` 打進 `S_WAIT_PLO`，`EXE_A_Stage` 會在 release 當拍重送 stale `vaddu_result_reg`，不要再把第一嫌疑放回 DRAM / fabric / downstream SRAM readback。

## 6. synthesis / gate sim 對不齊

### 先核對三件事

1. `CLOCK_PERIOD_NS` 是否一致。
2. `syn/clk_<period>ns/HybridAcc/` 是否有對應 netlist / SDF。
3. gate sim 是否讀到同一版 netlist、SDF 與 top module。

### top gate sim 常見錯誤

1. 忘記 `MOD_NAME=HybridAcc`
2. `GATE_NETLIST_DIR` 指到 `./syn` 而不是 `syn/clk_<period>ns`
3. `+SDF_FILE` 與 netlist clock tag 不一致

## 7. PrimePower 問題

### 先看哪些檔

1. `power_summary.rpt`
2. `power_hierarchy.rpt`
3. `unannotated_activity.rpt`
4. `analysis/index.html`

### 常見分流

1. activity 沒進去：先看 FSDB / VCD 是否正確，與 annotation summary。
2. 原始 report 有值但 HTML 怪：先懷疑 parser 與 analysis 輸出，不是 PrimePower 本身。
3. flow 啟不來：確認是 `pt_shell` 啟動 power analysis，而不是錯用 `pwr_shell`。

## 8. Jasper / Superlint 問題

### 原則

1. 主流程只認 canonical `script/tcl/superlint/jasper_superlint.tcl`。
2. query helper 是查詢工具，不是主流程本體。

### 常見錯誤思路

1. 把 helper script 當成 main flow。
2. 沒有開 DW / SRAM stub，導致 helper logic 類問題淹沒真正警告。

## 9. 五步最短 triage

1. 先確認 shell 與工作目錄。
2. 再找第一個失敗的 log，而不是最後一個 summary。
3. 先分 compile / run / compare，再決定要看哪份 manual。
4. 若是 signoff，先分原始 report 壞還是 analysis 壞。
5. 只有在定位到哪一層之後，才去改參數或腳本。

## 10. 相關文件

- [rtl-simulation.md](../user-manual/rtl-simulation.md)
- [rtl-firmware-regression.md](../user-manual/rtl-firmware-regression.md)
- [synthesis-and-postsim.md](../user-manual/synthesis-and-postsim.md)
- [lint-and-formal.md](../user-manual/lint-and-formal.md)
- [artefact-and-log-map.md](artefact-and-log-map.md)

## 11. 長期追加規則

後續每出現一個可重用的 debug 案例，都應在同一輪變更內回填本文件，而不是只留在 session note 或臨時報告。

新增條目時至少補這四件事：

1. 症狀或觀察到的失敗訊號。
2. 最便宜的第一個檢查點。
3. 能快速區分 root cause 的關鍵訊號。
4. 應該回查哪份 workflow manual。