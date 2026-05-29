# PrimePower Guide

Repo-wide 操作入口請先看 [../../../doc/index.md](../../../doc/index.md) 與 [../../../doc/user-manual/synthesis-and-postsim.md](../../../doc/user-manual/synthesis-and-postsim.md)；本文件保留 PrimePower 的 subsystem 細節。

本文件說明如何在這個 repo 裡用 Synopsys PrimePower 做 HybridAcc 的 gate-level power analysis。

這份指南的核心目標是：

- 利用本 repo 已經有的 synthesis netlist 與 gate sim flow
- 產生可以被 PrimePower 消化的 activity
- 做出比 DC `report_power` 更接近 workload 的功耗估計

如果你還沒有先完成 synthesis，請先看 `syn_guide.md`。如果你還不熟 gate sim target，請一起參考 `sim_test_guide.md`。

---

## 1. 在這個 repo 中，PrimePower 拿來做什麼

在目前 HybridAcc RTL flow 裡，Design Compiler 已經會在 synthesis 結束時輸出一份 `report_power -analysis_effort low`。但那份報告的定位比較接近：

- vectorless baseline
- synthesis 當下的快速估算
- 方便做 module-to-module 或 run-to-run 的粗略比較

PrimePower 則適合拿來做：

- 以 gate-level netlist 為基礎的獨立功耗分析
- 讀入 workload activity 後的動態功耗估計
- 分層級查看哪個 block 吃掉最多 internal / switching / leakage power
- 比較不同 clock、不同 workload、不同 dump window 的功耗差異

簡單講：

- DC power report 適合「快」
- PrimePower 適合「比較像真的 workload」

---

## 2. 這份 flow 需要哪些輸入

| 類型 | 路徑 / 來源 | 說明 |
|------|-------------|------|
| Gate-level netlist | `syn/clk_<period>/HybridAcc/HybridAcc_syn.v` | `make syn_top` 產生 |
| SDF | `syn/clk_<period>/HybridAcc/HybridAcc.sdf` | 用來做 gate sim，讓 activity 更接近 timing-annotated 行為 |
| DC baseline power report | `report/clk_<period>/HybridAcc/power_rpt_HybridAcc.txt` | 用來和 PrimePower 結果比較 |
| Library | `script/synopsys_dc.setup` | stdcell / SRAM / DW link 設定來源 |
| Activity | gate sim 產生的 FSDB / 轉換後的 VCD / SAIF | PrimePower 的關鍵輸入 |
| 可選 parasitics | backend SPEF | 若未來有後端資料，可提升可信度 |

目前 repo 內還有一些 `pwr_shell` 相關 log，它們只代表早期 probe 過 standalone Power shell。這個站點的 `pwr_shell` 會把 `read_verilog`、`read_ddc` 之類的設計讀入命令關掉，所以正式 flow 改成用 `pt_shell` 啟動，再用 `set_app_var power_enable_analysis true` 進入 PrimePower mode。

---

## 3. 先準備 top-level synthesis 輸出

在 `design/hybridacc-RTL` 目錄下執行：

```bash
make syn_top CLOCK_PERIOD_NS=1.25
```

確認以下檔案存在：

```text
syn/clk_1p25ns/HybridAcc/HybridAcc_syn.v
syn/clk_1p25ns/HybridAcc/HybridAcc.sdf
report/clk_1p25ns/HybridAcc/power_rpt_HybridAcc.txt
report/clk_1p25ns/HybridAcc/timing_max_rpt_HybridAcc.txt
report/clk_1p25ns/HybridAcc/timing_min_rpt_HybridAcc.txt
```

其中 `power_rpt_HybridAcc.txt` 是 DC baseline，不是 PrimePower 結果，但它非常適合拿來做 sanity check。

---

## 4. 先產生活動檔

PrimePower 沒有 activity 時也能跑，但那只是在做 vectorless 推估，價值有限。對 HybridAcc 這種有 clock gating、SRAM、DMA、NoC、PE 陣列的設計，**建議一定要餵真實 activity**。

### 4.1 用 top-level smoke test 產生 FSDB

這是最短的 bring-up 路徑：

```bash
make gate_sim_tb_hybridacc_smoke \
  MOD_NAME=HybridAcc \
  GATE_NETLIST_DIR="$PWD/syn/clk_1p25ns" \
  WAVE_DUMP=1 \
  WAVE_DEPTH=0 \
  SIM_PLUSARGS="+SDF_FILE=$PWD/syn/clk_1p25ns/HybridAcc/HybridAcc.sdf"
```

重點有兩個：

1. `gate_sim_tb_hybridacc_smoke` 不是 unit-level netlist，因此一定要手動指定 `MOD_NAME=HybridAcc`。
2. top-level gate sim 不能吃 Makefile 預設的 `GATE_NETLIST_DIR=./syn`，要改成 `GATE_NETLIST_DIR="$PWD/syn/clk_1p25ns"` 這種 top-run 根目錄。

執行完成後：

- log 在 `sim/gate_log/tb_hybridacc_smoke.run.log`
- FSDB 檔名由 testbench 寫死為 `tb_hybridacc_smoke.fsdb`

### 4.2 用較真實的 top-level sim 產生 FSDB

如果你需要比較像實際 workload 的 activity，建議直接跑：

```bash
make gate_sim_tb_hybridacc_sim \
  MOD_NAME=HybridAcc \
  GATE_NETLIST_DIR="$PWD/syn/clk_1p25ns" \
  WAVE_DUMP=1 \
  WAVE_DEPTH=0 \
  SIM_PLUSARGS="+SDF_FILE=$PWD/syn/clk_1p25ns/HybridAcc/HybridAcc.sdf"
```

這個 testbench 內的 DUT instance 名稱是 `dut`，後面做 activity mapping 時會用到。

### 4.3 用單一 workload regression 產生活動

如果你要看特定 workload，例如 GEMM / conv1x1 / conv3x3，建議直接利用既有 gate regression target：

```bash
make gate_regress_gemm_single_wave CLOCK_PERIOD_NS=1.25 WAVE_DUMP=1 WAVE_DEPTH=0
```

這類 target 會：

- 先編譯 workload
- 自動使用 `GATE_TOP_NETLIST_DIR` 與 `HybridAcc.sdf`
- 跑 gate-level `tb_hybridacc_sim`

執行完後，case log 會在：

```text
sim/gate_log/tb_hybridacc_sim_gate_gemm_single_wave.compile.log
sim/gate_log/tb_hybridacc_sim_gate_gemm_single_wave.run.log
```

但 FSDB 檔名仍然是 testbench 內固定的 `tb_hybridacc_sim.fsdb`，所以**每跑完一個 workload 就要立刻改名或搬走**，否則下一次會覆寫。

例如：

```bash
mv tb_hybridacc_sim.fsdb primepower/gemm_clk_1p25ns.fsdb
```

---

## 5. Activity 檔格式與 hierarchy mapping

目前 repo 內 top-level testbench 預設 dump 的是 FSDB：

- `tb_hybridacc_smoke.sv` 會產生 `tb_hybridacc_smoke.fsdb`
- `tb_hybridacc_sim.sv` 會產生 `tb_hybridacc_sim.fsdb`

兩者的 DUT instance 都叫做 `dut`，所以常用的 hierarchy scope 會是：

- smoke: `tb_hybridacc_smoke/dut`
- sim: `tb_hybridacc_sim/dut`

這件事會直接影響 PrimePower 的 activity annotation：

- 若你讀 VCD，通常要用 `-strip_path tb_hybridacc_sim/dut`
- 若你讀 SAIF，通常要用 `-instance_name tb_hybridacc_sim/dut`

### 5.1 FSDB 可不可以直接餵 PrimePower？

這取決於你所在站點的 PrimePower / Verdi 整合方式。不同環境可能：

- 可以直接讀 FSDB
- 只能穩定讀 VCD / SAIF

**建議最保守的做法**是把 FSDB 轉成 VCD 或 SAIF 再餵 PrimePower。

常見站點會提供類似 `fsdb2vcd` / `fsdb2saif` 的轉檔工具，但實際指令名稱和打包方式依機房而異，請先用 `which` 或站點文件確認。

---

## 6. 建議的 PrimePower 約束觀念

PrimePower 本質上與 PrimeTime 同一個 shell 脈絡，因此 timing environment 最好也先建好。建議做法和 `primetime_guide.md` 一樣：

- 不要整份直接 `read_sdc script/sdc/seq_top.sdc`
- 只保留 clock / I/O delay / operating condition / driving cell / load 這些子集
- 若有 SPEF，再加 `read_parasitics`

也就是說，你可以直接重用 PrimeTime 指南中那份 `pt_constraints_hybridacc.tcl`。

### 6.1 repo 內建 PrimePower flow

目前這個 repo 已經把 PrimePower flow 整合成 Make target，建議優先用：

```bash
make primepower_full \
    CLOCK_PERIOD_NS=1.25 \
    PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb
```

如果你要自行復現整個 PrimePower 分析，最直接的是拆成兩步：

```bash
make primepower_run \
    CLOCK_PERIOD_NS=1.25 \
    PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb

make primepower_analyze \
    CLOCK_PERIOD_NS=1.25
```

若你只想先把 FSDB 轉成 VCD：

```bash
make primepower_fsdb_to_vcd \
    CLOCK_PERIOD_NS=1.25 \
    PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb
```

若你要在分析結束後直接留在 GUI：

```bash
make primepower_gui \
    CLOCK_PERIOD_NS=1.25 \
    PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb \
    PRIMEPOWER_GUI=1
```

這一套 flow 目前的路徑規則是：

- work dir：`build/primepower/clk_<period>/<activity_tag>/`
- staged activity：`build/primepower/clk_<period>/<activity_tag>/activity/`
- report dir：`report/primepower/clk_<period>/<activity_tag>/`
- HTML/PDF 分析：`report/primepower/clk_<period>/<activity_tag>/analysis/`

目前 `power_hierarchy.rpt` 的預設深度是 4；若要覆寫，直接在 make 命令上加 `PRIMEPOWER_HIERARCHY_LEVELS=<N>`。

其中 `primepower_analyze` 會自動把 PrimePower 結果和 `report/clk_<period>/HybridAcc/` 底下的 synthesis power baseline 做比對，並輸出階層式功耗圖表。

實際工具啟動器是 `pt_shell`，不是 standalone `pwr_shell`。原因是這個站點的 `pwr_shell` 會禁用設計匯入命令，而 `pt_shell` 可以正常 `read_verilog`、`read_vcd`，再透過 `set_app_var power_enable_analysis true` 取得 PrimePower license 與 power command。

---

## 7. 建議的 PrimePower run script

以下是一份對應 repo 內建 flow 的 TCL 邏輯；實際檔案位置目前是 `script/tcl/analysis/primepower/run_pwr_hybridacc.tcl`。它假設你從 PrimePower 自己的 work dir 啟動 `pt_shell`，並透過環境變數指定 report/activity 路徑。

```tcl
set rtl_root [file normalize "."]
set top_name HybridAcc

if {[info exists ::env(RUN_TAG)] && [string trim $::env(RUN_TAG)] ne ""} {
    set run_tag [string trim $::env(RUN_TAG)]
} else {
    set run_tag clk_1p25ns
}

if {[info exists ::env(CLOCK_PERIOD_NS)] && [string trim $::env(CLOCK_PERIOD_NS)] ne ""} {
    set clk_period [string trim $::env(CLOCK_PERIOD_NS)]
} else {
    set clk_period 1.25
}

if {[info exists ::env(ACTIVITY_SCOPE)] && [string trim $::env(ACTIVITY_SCOPE)] ne ""} {
    set activity_scope [string trim $::env(ACTIVITY_SCOPE)]
} else {
    set activity_scope tb_hybridacc_sim/dut
}

if {[info exists ::env(HIERARCHY_LEVELS)] && [string trim $::env(HIERARCHY_LEVELS)] ne ""} {
    set hierarchy_levels [string trim $::env(HIERARCHY_LEVELS)]
} else {
    set hierarchy_levels 4
}

set out_dir [file join $rtl_root primepower $run_tag]
file mkdir $out_dir

set netlist [file join $rtl_root syn $run_tag $top_name ${top_name}_syn.v]
if {![file exists $netlist]} {
    puts "ERROR: netlist not found: $netlist"
    exit 1
}

set_app_var search_path [list \
    <stdcell-NLDM-dir> \
    <sram-NLDM-dir> \
    <synopsys-library-dir>]

set_app_var link_path [list \
    * \
    N16ADFP_StdCellss0p72vm40c.db \
    N16ADFP_StdCellff0p88v125c.db \
    N16ADFP_SRAM_ss0p72v0p72vm40c_100a.db \
    N16ADFP_SRAM_ff0p88v0p88v125c_100a.db \
    dw_foundation.sldb]

read_verilog $netlist
current_design $top_name
link_design $top_name
set_app_var power_enable_analysis true

source [file join $rtl_root primetime pt_constraints_hybridacc.tcl]

if {[info exists ::env(SPEF_FILE)] && [string trim $::env(SPEF_FILE)] ne ""} {
    read_parasitics -format spef [file normalize $::env(SPEF_FILE)]
    set_propagated_clock [all_clocks]
    update_timing
}

if {[info exists ::env(VCD_FILE)] && [string trim $::env(VCD_FILE)] ne ""} {
    read_vcd -strip_path $activity_scope [file normalize $::env(VCD_FILE)]
} elseif {[info exists ::env(SAIF_FILE)] && [string trim $::env(SAIF_FILE)] ne ""} {
    read_saif -input [file normalize $::env(SAIF_FILE)] -instance_name $activity_scope
} else {
    puts "INFO: no VCD_FILE / SAIF_FILE provided; falling back to vectorless activity"
}

check_power > [file join $out_dir check_power.rpt]
update_power

report_power > [file join $out_dir power_summary.rpt]
report_power -hierarchy -levels $hierarchy_levels > [file join $out_dir power_hierarchy.rpt]
report_switching_activity -list_not_annotated > [file join $out_dir unannotated_activity.rpt]
report_clock_gate_savings > [file join $out_dir clock_gate_savings.rpt]

quit
```

執行方式範例：

```bash
mkdir -p primepower

RUN_TAG=clk_1p25ns \
CLOCK_PERIOD_NS=1.25 \
ACTIVITY_SCOPE=tb_hybridacc_sim/dut \
HIERARCHY_LEVELS=4 \
VCD_FILE=$PWD/primepower/tb_hybridacc_sim.vcd \
pt_shell -file primepower/run_pwr_hybridacc.tcl | tee primepower/clk_1p25ns/pwr_shell.log
```

若你用的是 SAIF：

```bash
RUN_TAG=clk_1p25ns \
CLOCK_PERIOD_NS=1.25 \
ACTIVITY_SCOPE=tb_hybridacc_sim/dut \
SAIF_FILE=$PWD/primepower/tb_hybridacc_sim.saif \
pt_shell -file primepower/run_pwr_hybridacc.tcl | tee primepower/clk_1p25ns/pwr_shell.log
```

如果你用的是 smoke test，記得把 `ACTIVITY_SCOPE` 改成：

```text
tb_hybridacc_smoke/dut
```

---

## 8. 如何解讀 PrimePower 輸出

建議至少看這四份：

- `power_summary.rpt`
- `power_hierarchy.rpt`
- `unannotated_activity.rpt`
- `clock_gate_savings.rpt`

### 8.1 `power_summary.rpt`

先看總功耗是否落在合理範圍，再拆成：

- internal power
- switching power
- leakage power

### 8.2 `power_hierarchy.rpt`

用來找是哪一個 block 在吃功耗。對 HybridAcc 這種階層式設計來說，這份比總表更重要。

### 8.3 `unannotated_activity.rpt`

這份是最容易被忽略、但實務上最重要的 sanity check。

如果 unannotated nets 很多，表示：

- hierarchy scope 可能設錯
- dump depth 不夠
- 你餵的是錯的 VCD / SAIF
- gate sim 和 netlist 不是同一個 run

### 8.4 `clock_gate_savings.rpt`

HybridAcc 的 DC netlist 內可看到大量 `SNPS_CLOCK_GATE_*` cell，因此 clock-gating 節能報告是值得看的。這可以幫你回答兩件事：

- clock gating 有沒有真的在工作
- workload 之間的 clock activity 差異是否顯著

---

## 9. 和 DC `report_power` 怎麼比

建議把 PrimePower 結果拿來和：

```text
report/clk_1p25ns/HybridAcc/power_rpt_HybridAcc.txt
```

做對照，但要記住兩者本質不同：

- DC 的 `report_power -analysis_effort low` 比較偏 synthesis baseline
- PrimePower 讀了 activity 之後，比較偏 workload-aware power

因此兩者不一定要數字完全一致，但至少要能講得通：

- 如果 PrimePower 比 DC 高很多，通常是 activity 很熱或 SDF/SPEF 增加切換成本
- 如果 PrimePower 比 DC 低很多，常見原因是 clock gating 在真實 workload 下確實生效
- 如果 PrimePower 低得離譜，先檢查 activity 是否沒 annotate 進去

---

## 10. 常見問題

### Q1. 為什麼我明明開了 `WAVE_DUMP=1`，卻找不到 FSDB？

先看 `sim/gate_log/tb_hybridacc_sim.run.log` 或 `sim/gate_log/tb_hybridacc_smoke.run.log`。目前 testbench 的 FSDB 檔名是相對路徑，所以檔案通常會直接出現在 `design/hybridacc-RTL/` 根目錄，而不是 log 目錄。

### Q2. 為什麼 top-level gate sim 一定要手動設 `GATE_NETLIST_DIR`？

因為 Makefile 的 `GATE_NETLIST_DIR` 預設值是 `./syn`，這適合 unit-level gate sim，但 top-level `HybridAcc` netlist 實際上在 `syn/clk_<period>/HybridAcc/` 下。

### Q3. 為什麼我的 power 看起來幾乎全是 vectorless？

通常是 activity 沒有正確 annotate。先檢查：

- `VCD_FILE` / `SAIF_FILE` 是否真的存在
- `ACTIVITY_SCOPE` 是否正確指到 `tb_hybridacc_sim/dut` 或 `tb_hybridacc_smoke/dut`
- `unannotated_activity.rpt` 是否顯示大量未標註節點

### Q4. 可以直接拿 RTL activity 去 annotate gate netlist 嗎？

理論上可以做 mapping，但那會引入另一層不確定性。若你的目標是 gate-level power，最乾淨的做法仍然是直接用 gate sim 產生的 activity。

### Q5. 目前這份 flow 算 signoff power 嗎？

還不算。沒有後端 parasitics 與完整 mode/corner 管理時，這比較像 gate-level power characterization。

---

## 11. 建議的實務順序

如果你只是要快速得到一份可信度不錯的 HybridAcc 功耗估計，建議順序如下：

1. `make syn_top CLOCK_PERIOD_NS=<target>`
2. 用 `gate_sim_tb_hybridacc_smoke` 或 `gate_sim_tb_hybridacc_sim` 產生活動
3. 立刻把 FSDB 改名保存，避免被下次 run 覆寫
4. 視環境需要轉成 VCD / SAIF
5. 跑 `pt_shell`，並在 script 內 `set_app_var power_enable_analysis true`
6. 先看 `unannotated_activity.rpt`，再看 `power_hierarchy.rpt`

如果你要比較不同 workload，建議直接用 `gate_regress_conv2d_1x1_single_wave`、`gate_regress_conv2d_3x3_single_wave`、`gate_regress_gemm_single_wave` 這三個既有 target 產生活動，這樣結果會比 smoke test 更有代表性。