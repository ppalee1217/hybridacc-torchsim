# HybridAcc Synthesis Guide

這份文件是目前 HybridAcc RTL 合成的主操作手冊。

目前 flow 已整理成：

- clock period 可由 Make 參數指定，不需要改 SDC 檔。
- 每個 synthesis run 都有自己的 syn/report/work 目錄。
- 可以同時在不同 shell 合成不同 clock period，不會互相覆寫。
- 已新增 top-level HybridAcc 合成入口，不只限於 PE/NoC/Cluster unit。

## 1. 主要 target

- `make show_syn_config`: 顯示目前這次 synthesis run 會使用的 clock、run name、輸出路徑。
- `make syn_pe_<Module>`: 合成單一 PE module，例如 `make syn_pe_VADDU`。
- `make syn_noc_<Module>`: 合成單一 NoC module，例如 `make syn_noc_MBUS`。
- `make syn_cluster_<Module>`: 合成單一 Cluster module，例如 `make syn_cluster_ComputeCluster`。
- `make syn_hybridacc`: 合成 top-level `HybridAcc`。
- `make syn_pe_all`: 合成全部 PE modules。
- `make syn_noc_all`: 合成全部 NoC modules。
- `make syn_cluster_all`: 合成全部 Cluster modules。
- `make synall`: 依序跑 `syn_pe_all`、`syn_noc_all`、`syn_cluster_all`、`syn_hybridacc`。
- `make syn_report`: 針對目前 run 的 `report/<run>` 產生彙整報告。
- `make clean_syn`: 只清掉目前 `SYN_RUN_NAME` 的合成產物。
- `make clean_syn_all`: 清掉所有 synthesis run 的產物。

## 2. 重要參數

### `CLOCK_PERIOD_NS`

這是最直接的用法。若你只想改 clock period，直接傳這個參數即可。

```bash
make show_syn_config CLOCK_PERIOD_NS=2
make syn_hybridacc CLOCK_PERIOD_NS=1.25
```

Makefile 會自動把它映射成 synthesis 用的 `SYN_CLOCK_PERIOD_NS`。

### `SYN_CLOCK_PERIOD_NS`

若你想把 synthesis clock 與 simulation plusarg 分開控制，可以直接指定這個變數。

```bash
make syn_pe_VADDU SYN_CLOCK_PERIOD_NS=1.5
```

### `SYN_RUN_NAME`

每次 synthesis run 的名字。若不指定，會依 clock period 自動產生：

- `CLOCK_PERIOD_NS=2` -> `hybridacc_2ns`
- `CLOCK_PERIOD_NS=1.25` -> `hybridacc_1d25ns`

也可以手動覆寫：

```bash
make syn_hybridacc CLOCK_PERIOD_NS=1.25 SYN_RUN_NAME=hybridacc_1d25ns_trialA
```

## 3. 輸出目錄規則

假設這次 run name 是 `hybridacc_1d25ns`，則：

- netlist/SDF 會輸出到 `syn/hybridacc_1d25ns/<Module>/`
- synthesis log/report 會輸出到 `report/hybridacc_1d25ns/<Module>/`
- dc_shell work dir 會輸出到 `build_hybridacc_1d25ns_<Module>/`

以 top-level 為例：

```text
syn/hybridacc_1d25ns/HybridAcc/HybridAcc_syn.v
syn/hybridacc_1d25ns/HybridAcc/HybridAcc.sdf
report/hybridacc_1d25ns/HybridAcc/syn_compile_HybridAcc.log
report/hybridacc_1d25ns/HybridAcc/timing_max_rpt_HybridAcc.txt
build_hybridacc_1d25ns_HybridAcc/
```

這樣不同 clock period 的 run 可以共存，也能平行執行。

## 4. 常用操作

### 合成 top-level HybridAcc

```bash
make show_syn_config CLOCK_PERIOD_NS=1.25
make syn_hybridacc CLOCK_PERIOD_NS=1.25
```

### 合成單一 unit

```bash
make syn_pe_ProcessElement CLOCK_PERIOD_NS=2
make syn_noc_NetworkOnChip CLOCK_PERIOD_NS=1.5
make syn_cluster_ComputeCluster CLOCK_PERIOD_NS=1.25
```

### 跑完整一輪

```bash
make synall CLOCK_PERIOD_NS=1.25
```

### 產生目前 run 的 synthesis summary

```bash
make syn_report CLOCK_PERIOD_NS=1.25
```

輸出會落在：

```text
report/hybridacc_1d25ns/synthesis_report_<timestamp>.md
```

## 5. 平行不同 clock period 合成

若你要同時跑兩組不同時脈，直接在不同 shell 啟動即可：

```bash
make syn_hybridacc CLOCK_PERIOD_NS=2 &
make syn_hybridacc CLOCK_PERIOD_NS=1.25 &
wait
```

若你想顯式指定 run name：

```bash
make syn_hybridacc CLOCK_PERIOD_NS=2 SYN_RUN_NAME=hybridacc_2ns &
make syn_hybridacc CLOCK_PERIOD_NS=1.25 SYN_RUN_NAME=hybridacc_1d25ns &
wait
```

因為 work dir 也已拆開成 `build_<run>_<module>`，不同 run 不會共用 DC 工作目錄。

## 6. Gate-level simulation 對應方式

目前 gate sim 會從「當前 synthesis run」去找 netlist，也就是：

- `syn/<SYN_RUN_NAME>/<Module>/<Module>_syn.v`

所以若你要拿 `1.25ns` 那組 netlist 跑 gate sim，請維持同一組 `CLOCK_PERIOD_NS` 或直接指定相同的 `SYN_RUN_NAME`。

範例：

```bash
make gate_sim_tb_processelement MOD_NAME=ProcessElement CLOCK_PERIOD_NS=1.25
```

或：

```bash
make gate_sim_tb_processelement MOD_NAME=ProcessElement SYN_RUN_NAME=hybridacc_1d25ns
```

## 7. 這次整理後的腳本分工

- `script/syn_common.tcl`: 共用 run 設定，處理 clock period、run tag、輸出目錄。
- `script/synthesis_pe_units.tcl`: PE unit 合成。
- `script/synthesis_noc_units.tcl`: NoC unit 合成。
- `script/synthesis_cluster_units.tcl`: Cluster unit 合成。
- `script/syn_hybridacc.tcl`: top-level `HybridAcc` 合成。
- `script/sdc/comb.sdc`: 組合電路 SDC，會尊重外部傳入的 `clk_period`。
- `script/sdc/seq_unit.sdc`: unit-level sequential SDC，會尊重外部傳入的 `clk_period`。
- `script/sdc/seq_top.sdc`: top/integration SDC，會尊重外部傳入的 `clk_period`。

## 8. 建議操作順序

若你下一步是開始整顆 HybridAcc 收斂，建議流程如下：

1. `make show_syn_config CLOCK_PERIOD_NS=<target>`
2. `make syn_hybridacc CLOCK_PERIOD_NS=<target>`
3. `make syn_report CLOCK_PERIOD_NS=<target>`
4. 視需要再用相同 `SYN_RUN_NAME` 跑 gate sim 或比對多組時脈結果

如果你要做 sweep：

1. 先決定 clock period 清單
2. 每個 clock 用自己的 `SYN_RUN_NAME`
3. 平行啟動多個 `make syn_hybridacc ...`
4. 分別查看 `report/<run>/HybridAcc/` 與 `report/<run>/synthesis_report_<timestamp>.md`