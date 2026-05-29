# PrimeTime Guide

Repo-wide 操作入口請先看 [../../../doc/index.md](../../../doc/index.md) 與 [../../../doc/user-manual/synthesis-and-postsim.md](../../../doc/user-manual/synthesis-and-postsim.md)；本文件保留 PrimeTime 的 subsystem 細節。

本文件說明如何在這個 repo 裡用 Synopsys PrimeTime 做 HybridAcc top-level netlist 的獨立時序分析。

重點不是介紹 PrimeTime 所有功能，而是把這個專案現有的 synthesis 輸出、library 設定、SDC 假設與常見坑點整理成一份可直接照做的操作手冊。

如果你還沒有先產生 netlist，請先看 `syn_guide.md` 與 `hybridacc_synthesis_guide.md`。

---

## 1. 這份指南涵蓋什麼

在本 repo 中，PrimeTime 主要用來做以下幾件事：

- 對 Design Compiler 產生的 gate-level netlist 做獨立 STA。
- 重新檢查 setup/hold、constraint coverage、違規 path 與 clock path。
- 在未來有後端 SPEF 之後，從目前的 pre-layout flow 擴充到較接近 signoff 的分析。

目前 repo 內已經有：

- top-level synthesis 腳本：`script/tcl/synthesis/syn_hybridacc.tcl`
- top-level 約束：`script/sdc/seq_top.sdc`
- library 設定：`script/synopsys_dc.setup`
- PrimeTime TCL：`script/tcl/analysis/primetime/run_pt_hybridacc.tcl`
- PrimeTime constraints TCL：`script/tcl/analysis/primetime/pt_constraints_hybridacc.tcl`
- Make targets：`make primetime_run`、`make primetime_gui`、`make primetime_analyze`、`make primetime_full`
- 既有 synthesis 輸出：`syn/clk_*/HybridAcc/`、`report/clk_*/HybridAcc/`

目前 repo 內**沒有**：

- 已簽核的 SPEF / parasitics 檔
- MCMM signoff flow

因此，這份指南的定位是：

- 先做 standalone、single-corner、single-run 的 PrimeTime 分析
- 以 repo 現有 DC 產物為輸入
- 明確區分「pre-layout sanity check」與「真正 signoff」

---

## 2. 需要哪些輸入

| 類型 | 來源 | 說明 |
|------|------|------|
| Gate-level netlist | `syn/clk_<period>/HybridAcc/HybridAcc_syn.v` | 由 `make syn_top` 產生 |
| Top-level timing reports | `report/clk_<period>/HybridAcc/` | DC baseline，用來和 PrimeTime 比對 |
| Library 設定 | `script/synopsys_dc.setup` | 包含 stdcell / SRAM / DesignWare link 設定 |
| 約束來源 | `script/sdc/seq_top.sdc` | 建議只擷取其中的時序約束子集，不要整份直接丟給 PT |
| 可選 parasitics | backend SPEF | repo 目前沒有內建，需要外部提供 |

目前 Makefile 的 top-level naming 規則是 `clk_1p25ns` 這種形式，例如：

- `syn/clk_1p25ns/HybridAcc/HybridAcc_syn.v`
- `report/clk_1p25ns/HybridAcc/timing_max_rpt_HybridAcc.txt`

repo 裡也保留了一些舊 run，例如 `hybridacc_1d25ns`。這些可以拿來參考，但不要把它們當成目前 Makefile 的標準輸出命名。

### 2.1 repo 內建 PrimeTime flow

目前這個 repo 已經把 standalone PrimeTime flow 整合成 Make target，建議優先用：

```bash
make primetime_full CLOCK_PERIOD_NS=1.25
```

若你要在分析結束後直接留在 GUI：

```bash
make primetime_gui CLOCK_PERIOD_NS=1.25 PRIMETIME_GUI=1
```

這一套 flow 目前的路徑規則是：

- work dir：`build/primetime/clk_<period>/`
- report dir：`report/primetime/clk_<period>/`
- HTML/PDF 分析：`report/primetime/clk_<period>/analysis/`

其中 `primetime_analyze` 會自動把 PrimeTime 結果和 `report/clk_<period>/HybridAcc/` 底下的 synthesis baseline 做比對。

---

## 3. 先準備 synthesis 輸出

在 `design/hybridacc-RTL` 目錄下執行：

```bash
make syn_top CLOCK_PERIOD_NS=1.25
```

完成後，至少要確認這幾個檔案存在：

```text
syn/clk_1p25ns/HybridAcc/HybridAcc_syn.v
syn/clk_1p25ns/HybridAcc/HybridAcc.sdf
report/clk_1p25ns/HybridAcc/timing_max_rpt_HybridAcc.txt
report/clk_1p25ns/HybridAcc/timing_min_rpt_HybridAcc.txt
report/clk_1p25ns/HybridAcc/power_rpt_HybridAcc.txt
build/clk_1p25ns/syn_compile_hybridacc_clk_1p25ns.log
```

這裡的 DC report 是 PrimeTime 的 baseline。若 PrimeTime 沒有 parasitics、library 與約束也對齊，結果應該要和 DC report 接近到同一個量級；如果差異非常大，通常是 setup、link path 或 constraint 沒對齊。

---

## 4. PrimeTime 不要直接照搬的東西

`script/sdc/seq_top.sdc` 是目前 synthesis flow 的約束來源，但它不是乾淨的 PrimeTime 專用 SDC。裡面除了 clock 與 I/O delay 之外，還含有一些偏 DC/compile 的設定，例如：

- `set_max_area`
- `define_name_rules`
- `change_names`
- `compile_ultra_ungroup_dw true`

這些不適合直接當成 PrimeTime 分析腳本的核心。

建議做法是：

1. 以 `seq_top.sdc` 為來源，抽出時序分析真正需要的 constraint 子集。
2. 在 PrimeTime 內只保留 clock、I/O delay、operating conditions、driving cell、load 這些內容。
3. 若之後 backend 提供 SPEF，再把 parasitics 加回去。

---

## 5. 建議的最小 PrimeTime 約束檔

建議另外建立一份 PrimeTime 專用 constraint，例如 `primetime/pt_constraints_hybridacc.tcl`：

```tcl
if {![info exists clk_period]} {
    set clk_period 1.25
}

set input_max  [expr {double(round(1000.0 * $clk_period * 0.6)) / 1000.0}]
set input_min  0.0
set output_max [expr {double(round(1000.0 * $clk_period * 0.1)) / 1000.0}]
set output_min 0.0

create_clock -name clk -period $clk_period [get_ports clk]

set_clock_uncertainty 0.01 [all_clocks]
set_clock_latency 0.2 [all_clocks]
set_clock_latency -source 0 [all_clocks]

set_input_transition 0.2 [all_inputs]
set_clock_transition 0.1 [all_clocks]
set_load 0.005 [all_outputs]

set_operating_conditions \
    -min_library N16ADFP_StdCellff0p88v125c -min ff0p88v125c \
    -max_library N16ADFP_StdCellss0p72vm40c -max ss0p72vm40c

set_min_library N16ADFP_StdCellss0p72vm40c.db \
    -min_version N16ADFP_StdCellff0p88v125c.db
set_min_library N16ADFP_SRAM_ss0p72v0p72vm40c_100a.db \
    -min_version N16ADFP_SRAM_ff0p88v0p88v125c_100a.db

set_driving_cell -library N16ADFP_StdCellss0p72vm40c \
    -lib_cell BUFFD4BWP16P90LVT -pin Z [get_ports clk]
set_driving_cell -library N16ADFP_StdCellss0p72vm40c \
    -lib_cell DFQD1BWP16P90LVT -pin Q \
    [remove_from_collection [all_inputs] [get_ports clk]]

set_input_delay -clock clk -max $input_max \
    [remove_from_collection [all_inputs] [get_ports clk]]
set_input_delay -clock clk -min $input_min \
    [remove_from_collection [all_inputs] [get_ports clk]]
set_output_delay -clock clk -max $output_max [all_outputs]
set_output_delay -clock clk -min $output_min [all_outputs]
```

這份 TCL 的目標是複製 `seq_top.sdc` 裡和 STA 真正相關的假設，而不是複製 DC compile 行為。

---

## 6. 建議的 PrimeTime run script

以下示範一份可直接重用的 `primetime/run_pt_hybridacc.tcl`。它假設你從 `design/hybridacc-RTL` 目錄啟動 `pt_shell`。

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

set out_dir [file join $rtl_root primetime $run_tag]
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

source [file join $rtl_root primetime pt_constraints_hybridacc.tcl]

if {[info exists ::env(SPEF_FILE)] && [string trim $::env(SPEF_FILE)] ne ""} {
    read_parasitics -format spef [file normalize $::env(SPEF_FILE)]
    set_propagated_clock [all_clocks]
}

update_timing

check_timing > [file join $out_dir check_timing.rpt]
report_analysis_coverage > [file join $out_dir analysis_coverage.rpt]
report_constraint -all_violators > [file join $out_dir constraint_violators.rpt]

report_timing -delay_type max -path_type full_clock_expanded \
    -max_paths 10 -nworst 3 > [file join $out_dir timing_max.rpt]

report_timing -delay_type min -path_type full_clock_expanded \
    -max_paths 10 -nworst 3 > [file join $out_dir timing_min.rpt]

report_clock > [file join $out_dir clocks.rpt]

quit
```

執行方式：

```bash
mkdir -p primetime
pt_shell -file primetime/run_pt_hybridacc.tcl | tee primetime/clk_1p25ns/pt_shell.log
```

若要換成 2 ns：

```bash
RUN_TAG=clk_2p00ns CLOCK_PERIOD_NS=2.0 \
pt_shell -file primetime/run_pt_hybridacc.tcl | tee primetime/clk_2p00ns/pt_shell.log
```

---

## 7. 建議檢查順序

PrimeTime 起來之後，建議按照下面順序檢查：

### 7.1 先看 link 與 coverage

- `check_timing`
- `report_analysis_coverage`
- `report_constraint -all_violators`

如果這一層就有大量 unconstrained path，先不要急著看 WNS/TNS；代表 constraint 還沒對好。

### 7.2 再看 max/min timing

- `report_timing -delay_type max`
- `report_timing -delay_type min`

其中：

- `max` 對應 setup
- `min` 對應 hold

目前 repo 既有的 1.25 ns run 在 DC report 中已經可以看到 hold violation，因此 PrimeTime 若沿用同一組 netlist 與約束，理論上應該也會看到類似量級的 min-path 問題。

### 7.3 最後做 path 深挖

常用指令：

```tcl
report_timing -from [get_pins <start_pin>] -to [get_pins <end_pin>]
all_fanin -to [get_pins <end_pin>] -flat
all_fanout -from [get_pins <start_pin>] -flat
report_disable_timing
```

如果你看到 timing loop 或 arc 被 disable，這些指令比只看 summary 更快定位問題。

---

## 8. 和 repo 既有報告怎麼比

建議把 PrimeTime 輸出與目前 DC 產物對照：

- `report/clk_1p25ns/HybridAcc/timing_max_rpt_HybridAcc.txt`
- `report/clk_1p25ns/HybridAcc/timing_min_rpt_HybridAcc.txt`
- `build/clk_1p25ns/syn_compile_hybridacc_clk_1p25ns.log`

比對時優先看：

1. 是否同一個 run tag
2. 是否同一組 library
3. clock period 與 I/O delay 是否一致
4. 是否有 parasitics

如果 DC 與 PrimeTime 差很多，最常見原因不是設計被改壞，而是：

- PrimeTime 沒有用到 SRAM library
- constraint 沒有完整帶入
- 讀到舊的 `hybridacc_1d25ns` netlist
- PrimeTime 已加 SPEF，但你拿去比的是無 parasitics 的 DC report

---

## 9. 何時要加 parasitics

目前 repo 內的 synthesis report 屬於 pre-layout、ideal-clock/zero-wireload 脈絡。這對 bring-up 與相對比較有用，但不是最後 signoff。

當你手上有後端 SPEF 之後，PrimeTime 才會真的比 DC report 更有價值。最基本的差異會是：

- cell delay 不再是唯一主因，wire RC 會開始主導部分 path
- `set_propagated_clock` 才有意義
- setup/hold 與 clock tree 影響會更接近真實結果

有 SPEF 時，建議流程是：

```tcl
read_parasitics -format spef <top.spef>
set_propagated_clock [all_clocks]
update_timing
report_timing -delay_type max
report_timing -delay_type min
```

---

## 10. 常見問題

### Q1. 可以直接 `read_sdc script/sdc/seq_top.sdc` 嗎？

不建議。這份檔案混了 DC compile 脈絡下的命令，PrimeTime 最好只保留純 timing constraint 子集。

### Q2. `link_design` 失敗怎麼辦？

先檢查 `search_path` / `link_path` 是否真的包含：

- `N16ADFP_StdCellss0p72vm40c.db`
- `N16ADFP_StdCellff0p88v125c.db`
- `N16ADFP_SRAM_ss0p72v0p72vm40c_100a.db`
- `N16ADFP_SRAM_ff0p88v0p88v125c_100a.db`
- `dw_foundation.sldb`

### Q3. PrimeTime 跑出來和 DC 差很多，是不是 netlist 壞了？

先不要先怪 netlist。先檢查 run tag、library、constraint、parasitics 是否一致。

### Q4. 目前這份 flow 算 signoff 嗎？

不是。沒有 backend parasitics 與完整 corner/mode 管理時，這仍然是 pre-layout sanity check。

---

## 11. 建議的實務用法

如果你只是要快速確認目前 top synthesis 結果是否合理：

1. `make syn_top CLOCK_PERIOD_NS=<target>`
2. 用本文件的最小 PrimeTime script 跑一次 standalone STA
3. 先比對 `report/clk_<period>/HybridAcc/` 的 max/min report
4. 只有在需要更真實數字時，才導入 SPEF 與 propagated clock

這樣做可以把「合成問題」和「後端 RC 問題」分開，debug 會乾淨很多。