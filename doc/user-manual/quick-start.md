# HybridAcc Quick Start

## 1. 適用範圍

這份文件提供第一次進 repo 時的最短可執行路徑。目標不是一次學完整個專案，而是先在最短時間內確認四件事：

1. Python 與 setup 入口可用。
2. ESL simulator 能建起來。
3. RTL smoke test 能跑。
4. single-wave firmware regression 能完成。

若你只想把環境與流程先跑通，照本文件執行即可。更細的背景與延伸操作，再分別跳到對應 manual。

## 2. 工作模式

先記住一個規則：

1. `uv sync`、`uv run ...`、`scripts/setup.sh ...` 可在 `bash` 跑。
2. `make sim_*`、`make syn_*`、`make primetime_*`、`make primepower_*` 必須進 `tcsh`。

本文件會同時示範這兩種入口。

## 3. 前置條件

- repo 已 checkout 完成，且後續命令皆以 repo root 為基準
- `uv` 已可用
- 若要跑 RTL / EDA flow，站點 `.tcshrc` 已包含 VCS / Synopsys tool 設定

## 4. 最短上手流程

### 4.1 進 repo root

```bash
cd "$(git rev-parse --show-toplevel)"
```

### 4.2 建立 Python 環境與 setup 入口

```bash
uv sync
scripts/setup.sh all
```

若偏好全程走 Python CLI：

```bash
uv run hacc-setup all
```

### 4.3 快速檢查 CLI 與 toolchain

```bash
scripts/env_check.sh
```

若這一步失敗，先回 [environment-and-toolchains.md](environment-and-toolchains.md)。

### 4.4 建置 ESL simulator

```bash
scripts/setup.sh fast hybridacc-sim build
```

### 4.5 跑一個最小 ESL workflow

```bash
uv run hacc-compile design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml -o output/quickstart-conv3x3 --dump-ir
uv run python -m hybridacc_verify.gen.gen_test_dram --ir output/quickstart-conv3x3/hardware_ir.json --workload design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml --output-dir output/quickstart-conv3x3
scripts/setup.sh fast hybridacc-sim run-task output/quickstart-conv3x3
uv run python -m hybridacc_verify.check.compare_golden output/quickstart-conv3x3
```

### 4.6 跑最小 RTL smoke test

先從最便宜的 smoke 開始：

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_agu'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_hybridacc_smoke'
```

### 4.7 跑完整 single-wave firmware regression

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_single_wave'
```

## 5. 跑完後應該看到什麼

| 步驟 | 主要輸出 |
| --- | --- |
| `uv sync` | `.venv/` 與可執行 `uv run ...` |
| `scripts/setup.sh all` | setup 成功，不再缺必要工具 |
| ESL build | simulator build artefact |
| ESL sample | `output/quickstart-conv3x3/` 內的 `firmware.elf`、`hardware_ir.json`、`dram_init.bin`、`golden_output.bin` |
| RTL smoke | `design/hybridacc-RTL/sim/log/` 下的 `.compile.log` 與 `.run.log` |
| RTL regression | `output/rtl-fw-regress/` 與 `tb_hybridacc_sim_<case>.{compile,run,compare}.log` |

## 6. 成功判準

1. `uv run hacc-compile --help` 正常。
2. `scripts/setup.sh fast hybridacc-sim build` 完成。
3. `compare_golden` 對 ESL sample 給出 PASS。
4. `sim_tb_agu`、`sim_tb_hybridacc_smoke` compile / run 完成。
5. `rtl_regress_single_wave` 三個 case 都完成 compare。

## 7. 失敗時先怎麼分流

1. Python / CLI 一開始就失敗：先看 [environment-and-toolchains.md](environment-and-toolchains.md) 與 [python-cli-reference.md](python-cli-reference.md)。
2. ESL build 失敗：優先檢查 SystemC 與 RISC-V toolchain。
3. `make sim_*` 一開始就找不到 `vcs`：表示你沒進 `tcsh` 或 `.tcshrc` 沒載入。
4. RTL smoke 可編譯但 regression fail：直接看 `output/rtl-fw-regress/` 與 compare log。

## 8. 下一步怎麼走

1. 想做自訂 workload：看 [esl-workflows.md](esl-workflows.md) 與 [python-cli-reference.md](python-cli-reference.md)。
2. 想做 RTL 驗證：看 [rtl-simulation.md](rtl-simulation.md)。
3. 想做 signoff：看 [synthesis-and-postsim.md](synthesis-and-postsim.md)。

## 9. 相關文件

- [environment-and-toolchains.md](environment-and-toolchains.md)
- [python-cli-reference.md](python-cli-reference.md)
- [esl-workflows.md](esl-workflows.md)
- [rtl-simulation.md](rtl-simulation.md)
- [rtl-firmware-regression.md](rtl-firmware-regression.md)
- [../../README.md](../../README.md)