# Synthesis And Post-Sim

## 1. 適用範圍

這份文件整理 `design/hybridacc-RTL` 的 synthesis、STA、PrimePower 與 gate/post-sim 操作流程，重點是提供一條完整的 signoff 工作路徑，而不是只列出零散 target。

## 2. 固定工作模式

1. 一律在 `tcsh` 執行。
2. 工作目錄固定在 `/home/easonyeh/hybridacc/design/hybridacc-RTL`。
3. top-level synthesis / PrimeTime / PrimePower 的 clock tag 皆使用 `clk_<period>ns`，例如 `clk_1p25ns`。

## 3. 推薦整體順序

1. 先跑 synthesis。
2. 再跑 `syn_report` 彙整。
3. 產生 gate sim 波形或 activity。
4. 跑 PrimeTime。
5. 跑 PrimePower。
6. 驗證 `report/` 內 HTML / PDF 與原始 report 都存在。

## 4. Synthesis

### 4.1 top-level synthesis

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_top CLOCK_PERIOD_NS=1.25'
```

主要輸出：

- `syn/clk_1p25ns/HybridAcc/HybridAcc_syn.v`
- `syn/clk_1p25ns/HybridAcc/HybridAcc.sdf`
- `report/clk_1p25ns/HybridAcc/`

### 4.2 unit synthesis

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_pe_ProcessElement'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_noc_NetworkOnChip'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_cluster_ComputeCluster'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_pe_all'
```

### 4.3 unit module 分類與 SDC 規則

最穩定的分類原則如下：

| 類型 | 代表模組 | 約束策略 |
| --- | --- | --- |
| 純組合 | `Decoder`、`VMULU`、`VADDU` | `DC_comb.sdc`，以 virtual clock 分析 |
| 序向 | `DataMemory`、`LoopController`、`LDMA`、`SDMA`、`PErouter`、`ProcessElement` | `DC.sdc`，以實體 `clk` 分析 |

實務判斷方式：

1. 沒有 `clk` / `reset_n`，且只有 `always_comb` / `assign`，通常是組合模組。
2. 自身或子模組含 `always_ff`、FIFO、register file，視為序向模組。

### 4.4 synthesis report parser

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_report'
```

或直接調 parser：

```bash
uv run python script/python/reporting/syn_report.py --report-dir ./report --output ./report/manual_summary.md
```

## 5. PrimeTime

### 5.1 完整流程

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primetime_full CLOCK_PERIOD_NS=1.25'
```

### 5.2 GUI

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primetime_gui CLOCK_PERIOD_NS=1.25 PRIMETIME_GUI=1'
```

主要輸出：

- build: `build/primetime/clk_1p25ns/`
- report: `report/primetime/clk_1p25ns/`
- analysis: `report/primetime/clk_1p25ns/analysis/index.html`

### 5.3 PrimeTime 建議檢查順序

PrimeTime 起來後，先照這個順序檢查：

1. `check_timing`
2. `report_analysis_coverage`
3. `report_constraint -all_violators`
4. `report_timing -delay_type max`
5. `report_timing -delay_type min`

如果一開始就有大量 unconstrained path，先修 constraint，不要急著看 WNS / TNS。

### 5.4 剩餘 `-0.00` hold / `min_capacitance` 怎麼判讀

目前 top-level 2.00ns PrimeTime 剩下的項目，重點不是 setup，而是報表精度與 block-boundary 假設：

1. `timing_min.rpt` 的 worst min path 都是 top control input 直接進 `BootHostIf` / `DmaEngine` 的邊界 hold path，arrival 與 required 都印在 `0.24ns`，slack 顯示 `-0.00` 並附註 `increase significant digits`。
2. 這類項目若本次 signoff 目的是確認 block 內部邏輯已 closure，可先視為 standalone block 邊界 artifact，因為真正的 SoC 端 `set_input_delay -min` 尚未在這裡建模。
3. 若要把這些項目從 waiver 轉成 fix，正確修法不是重設 clock，而是替 host-control 介面補明確的 `set_input_delay -min`，或在 integration wrapper 把該邊界再 register / synchronize。
4. `constraint_violators.rpt` 內的 `min_capacitance` 若同時顯示 required=`0.00`、actual=`0.00`、slack=`0.00`，而且一樣是 `increase significant digits`，可先視為報表解析度 noise；只有在提高 precision 後仍出現非零 shortage，才需要做 driver/buffer 調整。

## 6. Gate sim 與 PrimePower 前置 activity

PrimePower 要做 workload-aware analysis，先要有 activity。最常見做法是先跑 gate sim 並打出 FSDB。

### 6.1 gate sim 產生 FSDB

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make gate_sim_tb_hybridacc_smoke MOD_NAME=HybridAcc CLOCK_PERIOD_NS=1.25 GATE_NETLIST_DIR="$PWD/syn/clk_1p25ns" WAVE_DUMP=1 WAVE_DEPTH=0 SIM_PLUSARGS="+SDF_FILE=$PWD/syn/clk_1p25ns/HybridAcc/HybridAcc.sdf"'
```

### 6.2 workload regression 產生 FSDB

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make gate_regress_gemm_single_wave CLOCK_PERIOD_NS=1.25 WAVE_DUMP=1 WAVE_DEPTH=0'
```

若使用 workload 波形，請立即改名，避免後面被覆蓋：

```bash
mv tb_hybridacc_sim.fsdb primepower/gemm_clk_1p25ns.fsdb
```

## 7. PrimePower

### 7.1 完整流程

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primepower_full CLOCK_PERIOD_NS=1.25 PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb'
```

### 7.2 拆開跑

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primepower_run CLOCK_PERIOD_NS=1.25 PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primepower_analyze CLOCK_PERIOD_NS=1.25'
```

重要規則：

1. 實際可用 flow 是 `pt_shell` 啟用 power analysis，不是單獨靠 `pwr_shell`。
2. 預設 hierarchy 深度為 4。
3. PrimePower report 路徑包含 clock tag 與 activity tag。

主要輸出：

- build: `build/primepower/clk_1p25ns/<activity_tag>/`
- report: `report/primepower/clk_1p25ns/<activity_tag>/`
- analysis: `report/primepower/clk_1p25ns/<activity_tag>/analysis/index.html`
- PDF: `report/primepower/clk_1p25ns/<activity_tag>/analysis/report.pdf`

### 7.3 PrimePower 報表解讀順序

建議至少看以下四份：

1. `power_summary.rpt`
2. `power_hierarchy.rpt`
3. `unannotated_activity.rpt`
4. `clock_gate_savings.rpt`

最重要的 sanity check 通常是 `unannotated_activity.rpt`。如果未標註節點很多，先懷疑 activity / hierarchy mapping，而不是直接解讀總功耗數字。

### 7.4 整合 static signoff dashboard

若要把 synthesis、PrimeTime、PrimePower 的表格與圖整成一個 static HTML，可直接跑：

```bash
uv run python script/python/reporting/build_signoff_dashboard.py \
	--clock-period 2.00 \
	--synthesis-report-dir ./report/clk_2p00ns/HybridAcc \
	--primetime-report-dir ./report/primetime/clk_2p00ns \
	--primepower-report-dir ./report/primepower/clk_2p00ns/tb_hybridacc_sim \
	--output-dir ./report/signoff-dashboard/clk_2p00ns__tb_hybridacc_sim
```

產物：

- `report/signoff-dashboard/clk_<period>ns__<activity_tag>/index.html`
- 內含 PrimeTime delay figure、PrimePower bar/pie charts、synthesis area / power 摘要、以及 residual STA disposition。

建議先確保個別 flow 都已落地原始 report，再跑這個整合腳本；它是彙整器，不負責重新執行 PrimeTime / PrimePower。

## 8. 常見命名與路徑規則

1. top-level synthesis 產物在 `syn/clk_<period>ns/HybridAcc/`。
2. top gate sim 不要用單元級 `GATE_NETLIST_DIR=./syn`；應使用 `GATE_NETLIST_DIR="$PWD/syn/clk_<period>ns"`。
3. top gate sim 通常需要手動指定 `MOD_NAME=HybridAcc`。
4. FSDB 容易同名覆蓋，產生後應立即改名。

## 9. 成功判準

1. synthesis 產出 netlist、SDF 與 report。
2. `syn_report` 可生成彙整結果。
3. PrimeTime 與 PrimePower 的原始 report、HTML、PDF 都存在。
4. gate sim 所用的 netlist / SDF 與 `CLOCK_PERIOD_NS` 完全一致。

## 10. 常見失敗點

1. 沒用 `tcsh`，導致 Synopsys tool PATH 或 license 失效。
2. unit synthesis 共用 `build/`，不適合平行跑多個 unit。
3. PrimePower 缺 activity 時雖然能跑，但只會退化成 vectorless analysis。
4. gate sim 的 `CLOCK_PERIOD_NS`、`GATE_NETLIST_DIR`、`+SDF_FILE` 指到不同版本。
5. top gate sim 忘記 `MOD_NAME=HybridAcc`。

## 11. 整體驗收建議

1. 三個固定 workload regression 先通過。
2. top-level synthesis 與關鍵 unit synthesis 產物完整。
3. gate sim 可用對應 netlist / SDF 起來。
4. PrimeTime 與 PrimePower report 都正常落地。

## 12. 相關文件

- [rtl-simulation.md](rtl-simulation.md)
- [artefact-and-log-map.md](../developer-manual/artefact-and-log-map.md)
- [debug-playbook.md](../developer-manual/debug-playbook.md)
- [../../design/hybridacc-RTL/doc/syn_guide.md](../../design/hybridacc-RTL/doc/syn_guide.md)
- [../../design/hybridacc-RTL/doc/hybridacc_synthesis_guide.md](../../design/hybridacc-RTL/doc/hybridacc_synthesis_guide.md)
- [../../design/hybridacc-RTL/doc/primetime_guide.md](../../design/hybridacc-RTL/doc/primetime_guide.md)
- [../../design/hybridacc-RTL/doc/primepower_guide.md](../../design/hybridacc-RTL/doc/primepower_guide.md)