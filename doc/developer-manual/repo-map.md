# Repo Map

## 1. 適用範圍

這份文件回答兩件事：

1. repo 裡每個主要目錄到底負責什麼。
2. 遇到某類任務時，第一個應該進哪個目錄、跑哪個入口。

如果你看到很多 `design/`、`python/`、`scripts/`、`output/` 但不知道要從哪裡下手，先看這裡。

## 2. 一張圖先看懂

| 路徑 | 角色 | 你通常在這裡做什麼 | canonical 入口 |
| --- | --- | --- | --- |
| `README.md` | repo root 入口 | 找專案總覽與最短上手路徑 | `scripts/setup.sh`、`uv run hacc-setup` |
| `doc/user-manual/` | repo 使用手冊 | 依任務查操作流程 | `index.md` |
| `doc/developer-manual/` | repo 開發維護手冊 | 查結構、artefact、debug、cleanup 規則 | `index.md` |
| `design/hybridacc-RTL/` | RTL 與 EDA flow | 跑 simulation、synthesis、STA、PrimePower | `Makefile` |
| `design/hybridacc-RTL/doc/` | RTL 詳細 guide | 查 RTL 子系統背景與細節 | `README.md` |
| `design/hybridacc-ESL/` | ESL simulator | build simulator、跑 SystemC workload | `scripts/setup.sh fast hybridacc-sim ...` |
| `design/hybridacc-ESL/doc/` | ESL docs root | 查 component 規格、legacy guide、report、test 文件 | `index.md` |
| `design/hybridacc-cc/` | compiler / example workloads | 編譯 YAML workload | `uv run hacc-compile` |
| `design/hybridacc-cc/doc/` | compiler 設計文件 | 查 pipeline、lowering、codegen、ELF、user guide | `00_Overview.md` |
| `design/hybridacc-cc/example/` | hand-authored workloads | 找 base YAML 與 sweep 範例入口 | `README.md` |
| `design/hybridacc-pe-isa/` | PE assembler / package toolchain | 建置 `ha-asm`、`ha-objdump`、`ha-package` | `README.md` |
| `python/` | Python package 與驗證邏輯 | CLI、golden compare、parser | `uv run ...` |
| `python/trace_parser/` | trace parser package | 安裝 parser、查 CLI / API 用法 | `README.md` |
| `scripts/` | setup / fast entry / analysis | 安裝、建置、批次入口 | `scripts/setup.sh` |
| `testbench/` | 測試資料與 config | 供 gen / sim / verify 使用 | 各 Python CLI |
| `output/` | 大量生成結果 | regression、sweep、runtime-check、analysis | 各 workflow 自動輸出 |
| `libs/`、`packages/` | 第三方相依 | setup / build 依賴 | 通常不手動進入 |

## 3. 按任務看應該去哪裡

### 3.1 想 setup 環境

從 repo root 開始：

```bash
cd "$(git rev-parse --show-toplevel)"
uv sync
scripts/setup.sh all
```

### 3.2 想跑 Python / compiler / ESL

停留在 repo root，使用：

- `uv run hacc-compile`
- `uv run hacc-sweep`
- `scripts/setup.sh fast hybridacc-sim ...`

若要看 compiler 設計或 YAML 範例，分別從 `design/hybridacc-cc/doc/00_Overview.md` 與 `design/hybridacc-cc/example/README.md` 進入。

### 3.3 想跑 RTL / synthesis / signoff

切到：

```bash
design/hybridacc-RTL
```

然後在 `tcsh` 中跑 `make` target。

## 4. RTL / EDA 子樹重點

### 4.1 `design/hybridacc-RTL/mk/`

這裡是 Makefile 分層入口：

- `common.mk`: 共用變數、工具、run tag、命名規則
- `pre-sim.mk`: RTL simulation
- `post-sim.mk`: gate sim / post-sim
- `synthesis.mk`: synthesis
- `superlint.mk`: Jasper / Superlint
- `primetime.mk`: PrimeTime
- `primepower.mk`: PrimePower

### 4.2 `design/hybridacc-RTL/script/`

canonical 腳本位置如下：

- `script/tcl/synthesis/`: synthesis Tcl
- `script/tcl/analysis/primetime/`: PrimeTime Tcl
- `script/tcl/analysis/primepower/`: PrimePower Tcl

## 5. 文件樹入口

- Repo root 入口：`README.md`
- Repo 文件總索引：`doc/index.md`
- ESL docs root：`design/hybridacc-ESL/doc/index.md`
- RTL docs root：`design/hybridacc-RTL/doc/README.md`
- Compiler docs root：`design/hybridacc-cc/doc/00_Overview.md`
- PE-ISA docs root：`design/hybridacc-pe-isa/README.md`
- `script/tcl/superlint/`: Jasper / Superlint 主流程
- `script/python/reporting/`: synthesis / signoff parser

頂層 `script/` 不再保留 Tcl/Python wrapper；主邏輯請走 `script/tcl/`、`script/python/` 或 Makefile target。

## 5. 生成物去哪裡找

| 路徑 | 角色 | 常見內容 |
| --- | --- | --- |
| `design/hybridacc-RTL/sim/log/` | RTL simulation log | `.compile.log`、`.run.log`、`.compare.log` |
| `design/hybridacc-RTL/sim/gate_log/` | gate sim log | gate compile / run log |
| `design/hybridacc-RTL/build/` | EDA workdir | synthesis / PrimeTime / PrimePower / Jasper workdir |
| `design/hybridacc-RTL/syn/` | synthesis 產物 | `_syn.v`、`.sdf`、`.ddc` |
| `design/hybridacc-RTL/report/` | report 與 analysis | synthesis / primetime / primepower / HTML / PDF |
| `output/rtl-fw-regress/` | single-wave regression 產物 | `firmware.elf`、`dram_init.bin`、`golden_output.bin` |
| `output/hacc-*-sweeps/` | sweep input 與 manifest | `manifest.json`、`lists/` |
| `output/hacc-*-results/` | E2E 執行結果 | case log |
| `output/hacc-*-report/` | 報告輸出 | `summary.md` 等 |

## 6. 命名規則要先知道什麼

1. synthesis / signoff 的 clock tag 會用 `clk_<period>ns`，例如 `clk_1p25ns`。
2. top-level synthesis 產物通常在 `syn/clk_<period>ns/HybridAcc/`。
3. PrimeTime report 在 `report/primetime/clk_<period>ns/`。
4. PrimePower report 在 `report/primepower/clk_<period>ns/<activity_tag>/`。

## 7. 成功判準

讀完這份文件後，你應該能回答：

1. setup 該從 repo root 還是 RTL 子目錄開始。
2. RTL flow 固定在哪個工作目錄跑。
3. synthesis、PrimeTime、PrimePower 的 canonical 腳本在哪裡。
4. `output/`、`build/`、`syn/`、`report/` 的差別。

## 8. 相關文件

- [../index.md](../index.md)
- [quick-start.md](../user-manual/quick-start.md)
- [artefact-and-log-map.md](artefact-and-log-map.md)
- [python-cli-reference.md](../user-manual/python-cli-reference.md)
- [../../README.md](../../README.md)
- [../../design/hybridacc-RTL/doc/README.md](../../design/hybridacc-RTL/doc/README.md)