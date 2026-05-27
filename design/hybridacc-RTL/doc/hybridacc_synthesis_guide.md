# HybridAcc Synthesis Guide

Repo-wide 操作入口請先看 [../../../doc/index.md](../../../doc/index.md) 與 [../../../doc/user-manual/synthesis-and-postsim.md](../../../doc/user-manual/synthesis-and-postsim.md)；本文件保留 top/unit synthesis 的細節與命名背景。

這份文件描述目前 `design/hybridacc-RTL` 真正存在的 synthesis target、腳本位置與輸出命名。若和較舊文件衝突，以這份與 `mk/synthesis.mk` 的實作為準。

## 1. 目前可用的 target

- `make syn_top CLOCK_PERIOD_NS=1.25`: 合成 top-level `HybridAcc`。
- `make syn_pe_<Module>`: 合成單一 PE module，例如 `make syn_pe_VADDU`。
- `make syn_noc_<Module>`: 合成單一 NoC module，例如 `make syn_noc_MBUS`。
- `make syn_cluster_<Module>`: 合成單一 Cluster module，例如 `make syn_cluster_ComputeCluster`。
- `make syn_pe_all`, `make syn_noc_all`, `make syn_cluster_all`: 批次合成各類 unit。
- `make synall`: 依序執行 `script/tcl/synthesis/` 下所有 `synthesis_*.tcl` flow；不包含 `syn_top`。
- `make syn_report`: 以 `uv run python script/python/reporting/syn_report.py` 掃描 `report/` 並產生彙整報告。
- `make clean_syn`: 目前會刪除整個 `syn/` 與 `report/`。

目前沒有 `show_syn_config`、`syn_hybridacc`、`clean_syn_all` 這幾個 target。若看到舊範例，請改用 `syn_top` 或上面的現行 target。

## 2. 腳本結構

目前 canonical synthesis 腳本集中在以下位置：

- `script/tcl/synthesis/syn_common.tcl`: 共用 run 設定與路徑 helper。
- `script/tcl/synthesis/synthesis_pe_units.tcl`: PE unit synthesis。
- `script/tcl/synthesis/synthesis_noc_units.tcl`: NoC unit synthesis。
- `script/tcl/synthesis/synthesis_cluster_units.tcl`: Cluster unit synthesis。
- `script/tcl/synthesis/syn_hybridacc.tcl`: top-level `HybridAcc` synthesis。
- `script/python/reporting/syn_report.py`: synthesis report parser。

頂層 `script/` 的 synthesis Tcl/Python wrapper 已移除；請使用上面的 canonical 路徑或 Makefile target。

新的 Makefile 入口也已拆成 include 架構：

- `Makefile`: entry point
- `mk/common.mk`: 共用變數與工具路徑
- `mk/synthesis.mk`: synthesis target 定義

## 3. 命名與輸出規則

top-level synthesis 會依 `CLOCK_PERIOD_NS` 自動產生 run tag，格式為 `clk_<period>ns`。

例如 `CLOCK_PERIOD_NS=1.25` 時：

- run tag: `clk_1p25ns`
- build dir: `build/clk_1p25ns/`
- netlist root: `syn/clk_1p25ns/HybridAcc/`
- report root: `report/clk_1p25ns/HybridAcc/`

典型輸出如下：

```text
build/clk_1p25ns/
syn/clk_1p25ns/HybridAcc/HybridAcc_syn.v
syn/clk_1p25ns/HybridAcc/HybridAcc.sdf
report/clk_1p25ns/HybridAcc/syn_compile_HybridAcc.log
report/clk_1p25ns/HybridAcc/timing_max_rpt_HybridAcc.txt
report/clk_1p25ns/HybridAcc/power_rpt_HybridAcc.txt
```

unit synthesis 目前仍沿用既有輸出規則：

- netlist/SDF: `syn/<Module>/`
- report: `report/<Module>/`
- work dir: `build/`

這代表 top-level synthesis 可以用 clock tag 並列保存，但 unit synthesis 目前共用 `build/`，不適合平行跑多個 unit job。

`make syn_report` 的輸出則是：

```text
report/pe_synthesis_report_<timestamp>.md
```

## 4. 常用操作

### 4.1 Top-level synthesis

```bash
make syn_top CLOCK_PERIOD_NS=1.25
```

### 4.2 單一 unit synthesis

```bash
make syn_pe_ProcessElement
make syn_noc_NetworkOnChip
make syn_cluster_ComputeCluster
```

### 4.3 批次 unit synthesis

```bash
make syn_pe_all
make syn_noc_all
make syn_cluster_all
```

### 4.4 產生彙整報告

```bash
make syn_report
```

若要直接呼叫 parser：

```bash
uv run python script/python/reporting/syn_report.py --report-dir ./report --output ./report/manual_summary.md
```

## 5. 和 gate sim 的對應

top-level gate sim 預設會從目前 clock tag 對應的目錄讀 netlist/SDF，例如：

- `syn/clk_1p25ns/HybridAcc/HybridAcc_syn.v`
- `syn/clk_1p25ns/HybridAcc/HybridAcc.sdf`

因此要讓 gate sim 和 top-level synthesis 對齊，最直接的作法是沿用相同 `CLOCK_PERIOD_NS`。

例如：

```bash
make syn_top CLOCK_PERIOD_NS=1.25
make gate_sim_tb_hybridacc_smoke MOD_NAME=HybridAcc CLOCK_PERIOD_NS=1.25
```

## 6. 實務建議

1. 若目標是 top-level STA 或 power，先跑 `make syn_top CLOCK_PERIOD_NS=<target>`。
2. 若只是在做 module-level QoR 比較，再跑 `syn_pe_*`、`syn_noc_*`、`syn_cluster_*`。
3. `synall` 只會跑 `synthesis_*.tcl`，不會自動補 `syn_top`。
4. unit synthesis 共用 `build/`，避免同時平行發多個 unit synthesis job。
5. 報告 parser 請使用 canonical 路徑 `script/python/reporting/syn_report.py` 或 `make syn_report`。