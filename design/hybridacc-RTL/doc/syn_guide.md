# PE Unit Synthesis Guide

本文件說明如何使用 Makefile 驅動 Synopsys Design Compiler 進行 PE 各 module 的獨立合成，以及如何產生彙整報告。

合成流程已針對**組合電路**與**序向電路**使用不同的 SDC 約束檔，確保每種電路類型都能獲得正確的時序約束。

---

## 目錄

1. [環境需求](#1-環境需求)
2. [目錄結構](#2-目錄結構)
3. [模組分類：組合電路 vs 序向電路](#3-模組分類組合電路-vs-序向電路)
4. [合成約束策略 (SDC)](#4-合成約束策略-sdc)
5. [合成流程概覽](#5-合成流程概覽)
6. [操作指令](#6-操作指令)
   - [合成單一模組](#61-合成單一模組)
   - [合成全部 PE 模組](#62-合成全部-pe-模組)
   - [產生彙整報告](#63-產生彙整報告)
7. [模組清單與相依關係](#7-模組清單與相依關係)
8. [輸出檔案說明](#8-輸出檔案說明)
9. [Report Parser 使用說明](#9-report-parser-使用說明)
10. [常見問題](#10-常見問題)
11. [建議合成順序](#11-建議合成順序)

---

## 1. 環境需求

| 項目 | 需求 |
|------|------|
| EDA Tool | Synopsys Design Compiler (dc_shell) |
| 版本 | W-2024.09-SP2 |
| License Server | `SNPSLMD_LICENSE_FILE=26585@lstn` |
| 製程 | TSMC N16ADFP (16nm FinFET) |
| Cell Library | `N16ADFP_StdCellss0p72vm40c.db` (slow) / `N16ADFP_StdCellff0p88v125c.db` (fast) |
| Python | 3.6+ (for `syn_report.py`) |

### 環境設定

```bash
# 確保 dc_shell 在 PATH 中
source /usr/cad/synopsys/synthesis/cur/setup.sh  # 或依站點設定調整

# License 已在 Makefile 中設定，也可手動 export
export SNPSLMD_LICENSE_FILE=26585@lstn
```

> **Note**: Makefile 已設定 `export SNPSLMD_LICENSE_FILE`，正常使用 `make` 指令時不需額外 export。

---

## 2. 目錄結構

合成流程涉及的目錄結構如下：

```
hybridacc-RTL/
├── Makefile                        # 合成/模擬主控
├── script/
│   ├── synopsys_dc.setup           # DC 環境設定（library path, target lib）
│   ├── DC.sdc                      # 序向電路時序約束（physical clock on port clk）
│   ├── DC_comb.sdc                 # 組合電路時序約束（virtual clock, no clk port）
│   ├── synthesis_pe_units.tcl      # PE 模組參數化合成腳本（自動選擇 SDC）
│   └── syn_report.py               # 報告解析 Python 腳本
├── src/                            # RTL 原始碼
│   ├── hybridacc_utils_pkg.sv      # 共用 package（FP16, type defs）
│   ├── FIFO.sv
│   ├── asyncFIFO.sv
│   └── PE/                         # PE 子模組
│       ├── DataMemory.sv
│       ├── Decoder.sv
│       └── ...
├── build/                          # DC 工作目錄（自動建立）
│   └── .synopsys_dc.setup          # 自動從 script/ 複製
├── syn/                            # 合成輸出（自動建立）
│   └── <ModuleName>/
│       ├── <ModuleName>_syn.v      # Gate-level netlist
│       └── <ModuleName>.sdf        # 時序標注檔
└── report/                         # 合成報告（自動建立）
    ├── pe_synthesis_report.md      # 彙整 Markdown 報告
    └── <ModuleName>/
        ├── syn_compile_<ModuleName>.log    # DC 完整編譯 log
        ├── timing_max_rpt_<ModuleName>.txt # Setup timing report
        ├── timing_min_rpt_<ModuleName>.txt # Hold timing report
        ├── area_rpt_<ModuleName>.txt       # Area report
        ├── power_rpt_<ModuleName>.txt      # Power report
        └── cell_rpt_<ModuleName>.txt       # Cell usage report
```

---

## 3. 模組分類：組合電路 vs 序向電路

每個 PE 模組的 RTL 原始碼已逐一分析，根據是否包含時脈埠（`clk`）、重置埠（`reset_n`）、以及是否使用 `always_ff`（暫存器）進行分類。

### 3.1 純組合電路模組（Combinational）

這些模組 **沒有** `clk` / `reset_n` 埠，內部只有 `always_comb` 與 `assign`，不含任何暫存器。

| Module | 原始碼 | 邏輯特徵 | 需要 pkg |
|--------|--------|----------|----------|
| **Decoder** | `src/PE/Decoder.sv` | 純 `always_comb`：指令解碼組合邏輯 | ✓ |
| **VMULU** | `src/PE/VMULU.sv` | 純 `always_comb` + `assign`：4-lane FP16 向量乘法 | ✓ |
| **VADDU** | `src/PE/VADDU.sv` | 純 `always_comb` + `assign`：4-lane FP16 向量加法累加 | ✓ |

**合成特點**：
- 無時脈埠 → 不能使用 `create_clock [get_ports clk]`
- 使用 **虛擬時脈**（virtual clock）作為時序參考
- 無 setup/hold 時序路徑（沒有暫存器到暫存器路徑）
- 約束的是 input-to-output 組合延遲

### 3.2 序向電路模組（Sequential）

這些模組包含 `clk` / `reset_n` 埠，內部使用 `always_ff @(posedge clk or negedge reset_n)` 含暫存器。

| Module | 原始碼 | 邏輯特徵 | 需要 pkg |
|--------|--------|----------|----------|
| **DataMemory** | `src/PE/DataMemory.sv` | 雙 bank 記憶體陣列，`always_ff` 讀寫 | ✗ |
| **InstructionMemory** | `src/PE/InstructionMemory.sv` | 指令記憶體，`always_ff` 寫入、組合讀取 | ✗ |
| **LoopController** | `src/PE/LoopController.sv` | 迴圈堆疊 + 計數器，`always_ff` 狀態機 | ✗ |
| **PsumRegFile** | `src/PE/PsumRegFile.sv` | 32 FP16 + 24 向量暫存器檔案 | ✓ |
| **TransformRegFile** | `src/PE/TransformRegFile.sv` | 12 FP16 暫存器檔案 + shift操作 | ✓ |
| **LDMA** | `src/PE/LDMA.sv` | FSM 狀態機，DMA 讀取控制 | ✓ |
| **SDMA** | `src/PE/SDMA.sv` | FSM 狀態機，DMA 寫入控制 | ✓ |
| **IF_ID_Stage** | `src/PE/IF_ID_Stage.sv` | 流水線第一級，含 PC 暫存器 | ✓ |
| **EXE_M_Stage** | `src/PE/EXE_M_Stage.sv` | 流水線乘法級，含解碼暫存器 | ✓ |
| **EXE_A_Stage** | `src/PE/EXE_A_Stage.sv` | 流水線加法級，含解碼暫存器 | ✓ |
| **PErouter** | `src/PE/PErouter.sv` | 路由邏輯（自身 `always_comb`），但內含 FIFO/asyncFIFO 序向子模組 | ✓ |
| **ProcessElement** | `src/PE/ProcessElement.sv` | 完整 PE 頂層，含所有子模組 | ✓ |

> **PErouter 分類說明**：PErouter 自身邏輯僅使用 `always_comb`，但它實例化了 FIFO 和 asyncFIFO（序向模組），且有 `clk`/`reset_n` 埠傳遞給子模組。獨立合成時會包含 FIFO，因此歸類為**序向電路**，使用 `DC.sdc`。

### 3.3 分類判定準則

```
Module 有 clk port?
├── NO → 純組合電路 → DC_comb.sdc (virtual clock)
│   例: Decoder, VMULU, VADDU
│
└── YES → 序向電路 → DC.sdc (physical clock)
    ├── 自身含 always_ff：LoopController, DataMemory, ...
    └── 子模組含 always_ff：PErouter (FIFO instances)
```

---

## 4. 合成約束策略 (SDC)

### 4.1 序向電路約束 — `DC.sdc`

用於所有含 `clk` 埠的模組。直接將 physical clock 綁定到模組的 `clk` port。

| 約束 | 值 | 說明 |
|------|----|------|
| Clock Source | `[get_ports clk]` | **Physical clock** 綁定實際 port |
| Clock Period | 1.0 ns | 目標 1000 MHz |
| Clock Uncertainty | 0.02 ns | Clock jitter margin |
| Clock Latency | 0.2 ns | Insertion delay |
| `set_dont_touch_network` | `[all_clocks]` | 保護時脈網路不被最佳化 |
| `set_fix_hold` | `[get_clocks clk]` | 要求 DC 修復 hold violation |
| `set_ideal_network` | `[get_clocks clk]` | 理想時脈網路（CTS 前） |
| Input Max Delay | 0.6 ns (60%) | 輸入在時脈邊緣後 0.6 ns 到達 |
| Input Min Delay | 0.3 ns (30%) | Hold check 用 |
| Output Max Delay | 0.1 ns (10%) | 輸出需在時脈前 0.1 ns 穩定 |
| Output Min Delay | 0.0 ns | |
| Clock Driving Cell | `BUFFD4BWP16P90LVT` | 時脈專用 buffer driver |
| Input Driving Cell | `DFQD1BWP16P90LVT` | 一般輸入由 DFF Q 驅動 |
| Clock Transition | 0.1 ns | 時脈 transition time |
| Input Transition | 0.2 ns | 一般輸入 transition time |
| Max Fanout | 10 | DRC constraint |
| Max Transition | 0.1 ns | DRC constraint |
| Operating Conditions | ss0p72vm40c (max) / ff0p88v125c (min) | Multi-corner |

### 4.2 組合電路約束 — `DC_comb.sdc`

用於 **Decoder、VMULU、VADDU**。這些模組沒有 `clk` port，因此無法綁定 physical clock。

| 約束 | 值 | 說明 |
|------|----|------|
| Clock Source | **Virtual** (`-name vclk`) | **不綁定任何 port**，純粹作為時序參考 |
| Clock Period | 1.0 ns | 與序向電路相同，確保結果可比較 |
| Clock Uncertainty | — | 不設定（虛擬時脈無 jitter） |
| Clock Latency | — | 不設定（虛擬時脈無 insertion delay） |
| `set_dont_touch_network` | — | 不設定（無實體時脈網路） |
| `set_fix_hold` | — | 不設定（無 register-to-register path） |
| `set_ideal_network` | — | 不設定（無實體時脈網路） |
| Input Max Delay | 0.6 ns (60%) | 與序向電路相同比例 |
| Input Min Delay | 0.3 ns (30%) | |
| Output Max Delay | 0.1 ns (10%) | |
| Output Min Delay | 0.0 ns | |
| Input Driving Cell | `DFQD1BWP16P90LVT` | **所有**輸入均由 DFF Q 驅動（無時脈 port） |
| Input Transition | 0.2 ns | |
| Max Fanout | 10 | DRC constraint |
| Max Transition | 0.1 ns | DRC constraint |
| Operating Conditions | ss0p72vm40c (max) / ff0p88v125c (min) | Multi-corner |

### 4.3 兩種 SDC 的關鍵差異

```
DC.sdc (序向)                     DC_comb.sdc (組合)
──────────────────                ──────────────────
create_clock ... [get_ports clk]  create_clock -name vclk -period 1.0
                                  (不綁定 port)

set_dont_touch_network [clocks]   (不需要 — 無實體時脈網路)
set_fix_hold [get_clocks clk]     (不需要 — 無 hold path)
set_clock_uncertainty 0.02        (不需要 — 虛擬時脈無 jitter)
set_clock_latency 0.2             (不需要 — 虛擬時脈無延遲)
set_ideal_network [clocks]        (不需要 — 無實體時脈網路)
set_clock_transition 0.1          (不需要 — 虛擬時脈無 transition)

BUFFD4BWP16P90LVT drives clk     (不需要 — 無 clk port)
DFQD1BWP16P90LVT drives others   DFQD1BWP16P90LVT drives ALL inputs

input_delay on non-clk inputs     input_delay on ALL inputs
```

> **設計原則**：組合模組的 input/output delay 比例與序向模組一致（60%/30%/10%/0%），使得獨立合成的組合模組延遲預算與其在上層流水線中的實際使用情境一致。

### 4.4 修改頻率目標

如需調整時鐘頻率，**兩個 SDC 都需要修改**第一行：

```tcl
# DC.sdc 和 DC_comb.sdc 中：
set clk_period 1.0    # 改為 2.0 代表 500 MHz，0.5 代表 2 GHz
```

其餘 I/O delay 約束會自動依比例計算。

---

## 5. 合成流程概覽

每個模組的合成流程由 `synthesis_pe_units.tcl` 定義。TCL 腳本會**自動判斷模組類型**並選擇正確的 SDC。

```
┌─────────────────────────────────────────────────────┐
│ 1. 讀取原始碼                                         │
│    read_file -format sverilog (pkg + module srcs)    │
├─────────────────────────────────────────────────────┤
│ 2. 設定設計                                           │
│    current_design → link                             │
├─────────────────────────────────────────────────────┤
│ 3. 自動選擇約束 ★                                     │
│    MOD_COMBINATIONAL = {Decoder VMULU VADDU}         │
│    if module ∈ COMBINATIONAL:                        │
│        source DC_comb.sdc  (virtual clock)           │
│    else:                                             │
│        source DC.sdc       (physical clock)          │
│    check_design / uniquify                           │
├─────────────────────────────────────────────────────┤
│ 4. 編譯（2-pass high effort）                          │
│    compile_ultra -timing_high_effort_script           │
│    compile_ultra -timing_high_effort_script -inc      │
├─────────────────────────────────────────────────────┤
│ 5. 產生報告                                           │
│    report_timing (max/min), report_area,              │
│    report_power, report_cell                          │
├─────────────────────────────────────────────────────┤
│ 6. 輸出 netlist                                       │
│    write -format verilog → *_syn.v                    │
│    write_sdf → *.sdf                                  │
└─────────────────────────────────────────────────────┘
```

合成時 DC log 會顯示：

```
# 組合電路
INFO: VADDU is COMBINATIONAL — using DC_comb.sdc (virtual clock)

# 序向電路
INFO: LoopController is SEQUENTIAL — using DC.sdc (physical clock on port clk)
```

- 使用 `compile_ultra` 進行兩輪最佳化（第二輪為增量 `-inc`）
- 最多使用 8 核心平行處理（`set_host_options -max_core 8`）
- 組合模組（Decoder, VMULU, VADDU）合成速度較快
- 完整 PE（ProcessElement）約需最長時間

---

## 6. 操作指令

所有指令都在 `hybridacc-RTL/` 目錄下執行。SDC 選擇完全自動，**使用者不需要手動指定電路類型**。

### 6.1 合成單一模組

```bash
make syn_pe_<ModuleName>
```

範例：

```bash
# 合成 VADDU（純組合電路 → 自動使用 DC_comb.sdc）
make syn_pe_VADDU

# 合成 VMULU（純組合電路 → 自動使用 DC_comb.sdc）
make syn_pe_VMULU

# 合成 Decoder（純組合電路 → 自動使用 DC_comb.sdc）
make syn_pe_Decoder

# 合成 PErouter（序向電路 → 自動使用 DC.sdc）
make syn_pe_PErouter

# 合成 ProcessElement（序向電路，完整 PE）
make syn_pe_ProcessElement
```

> **模組名稱大小寫必須完全匹配**（例如 `VADDU` 不是 `vaddu`）。

### 6.2 合成全部 PE 模組

```bash
make syn_pe_all
```

這會依序合成以下 15 個模組（由 `PE_UNITS` 變數定義）：

```
DataMemory → Decoder → InstructionMemory → LoopController → PsumRegFile →
TransformRegFile → VADDU → VMULU → LDMA → SDMA →
IF_ID_Stage → EXE_M_Stage → EXE_A_Stage → PErouter → ProcessElement
```

- 使用 `set -e`，任一模組失敗即停止後續合成
- 預估全部合成時間：**45–90 分鐘**

### 6.3 產生彙整報告

合成完成後，執行：

```bash
make syn_report
```

這會呼叫 `script/syn_report.py` 掃描 `report/` 目錄，自動偵測已完成合成的模組，產生：

```
report/pe_synthesis_report.md
```

---

## 7. 模組清單與相依關係

### 葉模組（Leaf Modules）

不依賴其他 RTL 子模組，最適合獨立合成：

| Module | 原始碼 | 電路類型 | SDC | 需要 pkg |
|--------|--------|----------|-----|----------|
| DataMemory | `src/PE/DataMemory.sv` | **序向** | `DC.sdc` | ✗ |
| Decoder | `src/PE/Decoder.sv` | **組合** | `DC_comb.sdc` | ✓ |
| InstructionMemory | `src/PE/InstructionMemory.sv` | **序向** | `DC.sdc` | ✗ |
| LoopController | `src/PE/LoopController.sv` | **序向** | `DC.sdc` | ✗ |
| PsumRegFile | `src/PE/PsumRegFile.sv` | **序向** | `DC.sdc` | ✓ |
| TransformRegFile | `src/PE/TransformRegFile.sv` | **序向** | `DC.sdc` | ✓ |
| VADDU | `src/PE/VADDU.sv` | **組合** | `DC_comb.sdc` | ✓ |
| VMULU | `src/PE/VMULU.sv` | **組合** | `DC_comb.sdc` | ✓ |
| LDMA | `src/PE/LDMA.sv` | **序向** | `DC.sdc` | ✓ |
| SDMA | `src/PE/SDMA.sv` | **序向** | `DC.sdc` | ✓ |

### 階層模組（Hierarchical Modules）

合成時會包含其所有子模組，全部歸類為**序向電路**（使用 `DC.sdc`）：

| Module | 電路類型 | 包含子模組 |
|--------|----------|-----------|
| IF_ID_Stage | **序向** | InstructionMemory, Decoder（組合）, LoopController |
| EXE_M_Stage | **序向** | TransformRegFile, VMULU（組合）, LDMA, SDMA, DataMemory |
| EXE_A_Stage | **序向** | VADDU（組合）, PsumRegFile |
| PErouter | **序向** | FIFO, asyncFIFO |
| ProcessElement | **序向** | **全部**（PErouter + IF_ID_Stage + EXE_M_Stage + EXE_A_Stage 及其所有子模組） |

> 雖然 Decoder/VMULU/VADDU 獨立合成時使用 `DC_comb.sdc`，但當作為子模組被包含在階層模組中合成時，整體仍使用 `DC.sdc`（因為上層模組有 `clk` port）。

---

## 8. 輸出檔案說明

每個合成完成的模組會產生以下檔案：

### report/\<Module\>/

| 檔案 | 內容 |
|------|------|
| `timing_max_rpt_*.txt` | **Setup timing report**：最壞路徑延遲、slack（正=MET，負=VIOLATED） |
| `timing_min_rpt_*.txt` | **Hold timing report**：最快路徑延遲、slack |
| `area_rpt_*.txt` | **面積報告**：組合邏輯面積、序向邏輯面積、Cell 數量 |
| `power_rpt_*.txt` | **功耗報告**：Internal/Switching/Leakage Power |
| `cell_rpt_*.txt` | **Cell 使用報告**：各 standard cell 的實例化清單 |
| `syn_compile_*.log` | **完整 DC log**：含 warning/error 資訊 |

### syn/\<Module\>/

| 檔案 | 內容 |
|------|------|
| `*_syn.v` | **Gate-level netlist**：合成後 Verilog，可用於 post-synthesis simulation |
| `*.sdf` | **SDF timing annotation**：反標延遲資訊 |

---

## 9. Report Parser 使用說明

### 基本用法

```bash
# 自動偵測已完成的模組，產生報告
python3 script/syn_report.py

# 或透過 Makefile
make syn_report
```

### 進階用法

```bash
# 只看特定模組
python3 script/syn_report.py --modules VADDU VMULU LoopController

# 包含全部 15 個 PE 模組（即使尚未合成，會顯示缺失警告）
python3 script/syn_report.py --all

# 自訂輸出路徑
python3 script/syn_report.py --output my_report.md

# 自訂報告目錄
python3 script/syn_report.py --report-dir ./report --output summary.md
```

### 報告內容

產生的 Markdown 報告包含：

1. **Summary Table** — 所有模組一覽：面積、slack、功耗
2. **Detailed Results** — 每個模組的完整 metrics：
   - Timing：Clock period, setup/hold slack, critical path endpoints
   - Area：Combinational/Non-combinational/Total area, cell counts
   - Power：Internal/Switching/Dynamic/Leakage power

---

## 10. 常見問題

### Q: 組合模組為什麼不能使用 DC.sdc？

`DC.sdc` 中使用 `create_clock [get_ports clk]` 將時脈綁定到 `clk` port。組合模組（Decoder, VMULU, VADDU）**沒有 `clk` port**，執行 `get_ports clk` 會產生錯誤：

```
Error: Can't find port 'clk' in design 'VADDU'.
```

因此需要 `DC_comb.sdc` 使用虛擬時脈（`create_clock -name vclk -period 1.0`，不綁定 port）。

### Q: 組合模組的 timing report 如何解讀？

組合模組的 timing report 中：
- **Startpoint** 是某個輸入 port
- **Endpoint** 是某個輸出 port
- **Path type**: `max` (setup) 代表最長組合路徑延遲
- **slack**: 正值表示組合延遲在一個 clock period 的預算內

沒有 register-to-register path，所有路徑都是 input-to-output。

### Q: 如何確認哪些模組是組合電路？

TCL 腳本中定義了 `MOD_COMBINATIONAL` 清單：

```tcl
# synthesis_pe_units.tcl
set MOD_COMBINATIONAL {Decoder VMULU VADDU}
```

合成時 DC log 會輸出發訊息，例如：
```
INFO: VADDU is COMBINATIONAL — using DC_comb.sdc (virtual clock)
```

### Q: 新增 module 時如何判斷要分類為組合還是序向？

檢查三個條件：
1. 模組是否有 `input logic clk` port？
2. 模組內是否使用 `always_ff` 或 `always @(posedge ...)`？
3. 模組是否實例化含暫存器的子模組？

任一條件為「是」→ **序向電路**，不需修改（預設使用 `DC.sdc`）。
全部為「否」→ **純組合電路**，需要將模組名稱加入 `MOD_COMBINATIONAL` 清單和 `syn_report.py` 的 `COMBINATIONAL_MODULES` 集合。

### Q: 合成失敗，找不到 dc_shell

```
make: dc_shell: No such file or directory
```

**A**: 確認已 source Synopsys DC 的環境設定：

```bash
source /usr/cad/synopsys/synthesis/cur/setup.sh
# 或指定 DC_SHELL 路徑
make syn_pe_VADDU DC_SHELL=/path/to/dc_shell
```

### Q: License 錯誤

```
Error: Cannot check out a license for "DesignCompiler".
```

**A**: 確認 license server 可達：

```bash
export SNPSLMD_LICENSE_FILE=26585@lstn
lmstat -c $SNPSLMD_LICENSE_FILE -a | grep "DC"
```

### Q: Warning: Unable to resolve reference 'clk'

這通常代表模組的 clock port 名稱不是 `clk`。SDC 中使用 `get_ports clk`，若模組 port 名稱不同需修改 SDC。目前所有 PE 模組都使用 `clk` 作為 clock port。

### Q: 某模組 timing 沒有 MET

```
slack (VIOLATED)  -0.1234
```

**A**: 可能需要：
1. 放寬時鐘頻率（增加 `clk_period`）
2. 檢查 critical path（查看 timing report 的 startpoint/endpoint）
3. 調整合成策略（修改 TCL 中的 compile 選項）

### Q: 只有部分模組完成合成，可以先產生報告嗎？

**A**: 可以。`syn_report.py` 預設只掃描有 timing report 的模組：

```bash
# 只會列出已完成合成的模組
make syn_report
```

### Q: 如何清除全部合成結果重新開始？

```bash
make clean_syn
```

這會刪除 `syn/` 和 `report/` 目錄。

---

## 11. 建議合成順序

### 快速驗證流程

先各合成一個組合與序向模組，確認兩種 SDC 都能正常運作：

```bash
# Step 1: 合成一個組合模組（驗證 DC_comb.sdc）
make syn_pe_VADDU

# Step 2: 合成一個序向模組（驗證 DC.sdc）
make syn_pe_LoopController

# Step 3: 確認報告正確產生
make syn_report

# Step 4: 確認沒問題後，合成全部
make syn_pe_all

# Step 5: 全部完成後產生彙整報告
make syn_report
```

### 分批合成

如果時間有限，建議分批：

```bash
# 批次 1：組合模組（最快，無暫存器）
make syn_pe_Decoder        # 組合 → DC_comb.sdc
make syn_pe_VMULU          # 組合 → DC_comb.sdc
make syn_pe_VADDU          # 組合 → DC_comb.sdc

# 批次 2：序向葉模組
make syn_pe_DataMemory     # 序向 → DC.sdc
make syn_pe_InstructionMemory
make syn_pe_LoopController
make syn_pe_PsumRegFile
make syn_pe_TransformRegFile
make syn_pe_LDMA
make syn_pe_SDMA

# 中途可先看已完成的報告
make syn_report

# 批次 3：序向階層模組
make syn_pe_IF_ID_Stage
make syn_pe_EXE_M_Stage
make syn_pe_EXE_A_Stage
make syn_pe_PErouter

# 批次 4：完整 PE（最慢）
make syn_pe_ProcessElement

# 最終報告
make syn_report
```

### 串接一次執行

如果想要一次全部跑完並產生報告：

```bash
make syn_pe_all && make syn_report
```

---

## 附錄：快速指令表

| 用途 | 指令 |
|------|------|
| 合成單一模組 | `make syn_pe_<Name>` |
| 合成全部 PE 模組 | `make syn_pe_all` |
| 產生彙整報告 | `make syn_report` |
| 檢視合成 log | `less report/<Name>/syn_compile_<Name>.log` |
| 檢視 timing | `less report/<Name>/timing_max_rpt_<Name>.txt` |
| 清除合成結果 | `make clean_syn` |
| 查看所有 make targets | `make help` |

## 附錄：SDC 與模組類型快速對照

| SDC 檔案 | 時脈類型 | 適用模組 |
|----------|----------|----------|
| `DC.sdc` | Physical clock (`clk` port) | DataMemory, InstructionMemory, LoopController, PsumRegFile, TransformRegFile, LDMA, SDMA, IF_ID_Stage, EXE_M_Stage, EXE_A_Stage, PErouter, ProcessElement |
| `DC_comb.sdc` | Virtual clock (no port) | **Decoder, VMULU, VADDU** |

> SDC 選擇由 `synthesis_pe_units.tcl` 中的 `MOD_COMBINATIONAL` 清單自動控制，使用者無需手動指定。
