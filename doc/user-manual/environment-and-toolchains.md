# Environment And Toolchains

## 1. 適用範圍

這份文件是 HybridAcc 所有 workflow 的環境說明總表。當你遇到「同一個命令在別人機器能跑、我這裡不能跑」時，先查這裡。

本文件涵蓋：

1. Python / `uv`
2. RISC-V firmware toolchain
3. SystemC
4. VCS / Synopsys / FSDB conversion
5. `bash` 與 `tcsh` 的角色分工

## 2. 兩個最重要的規則

### 2.1 `uv` 是 Python 的唯一標準入口

本 repo 的 Python workflow 預設使用 `uv`。除非是被 `uv run python -m ...` 包住，否則不要假設系統 Python 或 `pip` 版本正確。

### 2.2 RTL / EDA workflow 一律預設跑在 `tcsh`

以下命令不要在純 `bash` 環境直接跑：

- `make sim_*`
- `make gate_sim_*`
- `make syn_*`
- `make primetime_*`
- `make primepower_*`
- `make superlint*`

canonical 入口：

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make <target>'
```

## 3. 工具清單

| 類別 | 工具 | 主要用途 | 常用工作目錄 |
| --- | --- | --- | --- |
| Python workflow | `uv` | 環境、CLI、套件同步 | repo root |
| Firmware toolchain | `riscv32-unknown-elf-*` | bare-metal firmware build | repo root / RTL |
| ESL | SystemC | `hybridacc-ESL` simulator | repo root |
| RTL simulation | `vcs` | RTL / gate sim | `design/hybridacc-RTL` |
| Synthesis | `dc_shell` | unit / top synthesis | `design/hybridacc-RTL` |
| STA / power | `pt_shell` | PrimeTime STA / PrimePower | `design/hybridacc-RTL` |
| FSDB conversion | `fsdb2vcd` | PrimePower activity conversion | `design/hybridacc-RTL` |

## 4. 基本安裝與 setup 入口

### 4.1 repo root setup

```bash
cd /home/easonyeh/hybridacc
uv sync
scripts/setup.sh all
```

### 4.2 按功能安裝

```bash
scripts/setup.sh install all
scripts/setup.sh install riscv --prefix $HOME/.local/riscv
scripts/setup.sh install systemc
scripts/setup.sh env --riscv-prefix $HOME/.local/riscv
```

### 4.3 Python CLI 對應入口

```bash
uv run hacc-setup all
uv run hacc-setup install all
uv run hacc-setup env --riscv-prefix $HOME/.local/riscv
```

## 5. sanity check

先跑整合檢查：

```bash
scripts/env_check.sh
```

若目前只想檢查 Python / SystemC / RISC-V，不檢查 EDA tool：

```bash
scripts/env_check.sh --no-eda
```

### 5.1 Python / CLI

```bash
uv --version
uv run hacc-compile --help
uv run hacc-sweep --help
uv run python -m hybridacc_verify.main --help
```

### 5.2 RISC-V toolchain

```bash
command -v riscv32-unknown-elf-gcc
command -v riscv32-unknown-elf-objcopy
command -v riscv32-unknown-elf-size
```

### 5.3 EDA / wave toolchain

```bash
tcsh -ic 'source ~/.tcshrc; command -v vcs; command -v dc_shell; command -v pt_shell; command -v fsdb2vcd'
```

## 6. shell 與工作目錄規則

### 6.1 在 `bash` 可以做的事

- `uv sync`
- `uv run ...`
- `scripts/setup.sh ...`
- ESL compile / generate / compare workflow
- 純 Python parser / report workflow

### 6.2 在 `tcsh` 應該做的事

- 所有 RTL simulation
- gate sim
- synthesis
- PrimeTime
- PrimePower
- Jasper / Superlint

### 6.3 canonical 工作目錄

| workflow | 工作目錄 |
| --- | --- |
| setup / Python / ESL | `/home/easonyeh/hybridacc` |
| RTL / synthesis / signoff | `/home/easonyeh/hybridacc/design/hybridacc-RTL` |

## 7. 常見依賴問題

### 7.1 `uv run ...` 正常，但 `make sim_*` 失敗

通常不是 Python 問題，而是 `tcsh` / license / PATH 問題。

### 7.2 `scripts/setup.sh all` 跑完後，仍找不到 `vcs`

`scripts/setup.sh` 主要處理 repo 依賴；站點 EDA tool 的 PATH 與 license 通常仍由 `.tcshrc` 或 site module 決定。

### 7.3 `pt_shell` 可用，但 PrimePower 還是起不來

請確認實際 flow 是在 `pt_shell` 中啟用 power analysis，而不是單獨依賴 `pwr_shell`。

## 8. 成功判準

1. `uv run ...` 類命令可正常啟動。
2. `riscv32-unknown-elf-*` 可在 PATH 中找到。
3. `tcsh -ic 'source ~/.tcshrc; command -v ...'` 可找到 VCS / DC / PT / `fsdb2vcd`。
4. 你能分辨哪些工作在 `bash`，哪些工作在 `tcsh`。

## 9. 相關文件

- [quick-start.md](quick-start.md)
- [python-cli-reference.md](python-cli-reference.md)
- [esl-workflows.md](esl-workflows.md)
- [synthesis-and-postsim.md](synthesis-and-postsim.md)
- [../../README.md](../../README.md)
- [../../python/legacy_README.md](../../python/legacy_README.md)