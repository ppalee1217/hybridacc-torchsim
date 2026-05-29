# Python CLI Reference

## 1. 適用範圍

這份文件整理 repo 內目前最常用的 Python CLI 與 module 入口，目標是讓你在不翻整份 `python/legacy_README.md` 的前提下，能快速知道：

1. 該用哪個 CLI。
2. 命令應該在哪個目錄跑。
3. 會產生什麼輸出。
4. 工作流要如何串接。

## 2. 使用原則

1. 先在 repo root 執行 `uv sync`。
2. 之後優先使用 `uv run ...`。
3. 若 CLI 未註冊成 `project.scripts`，則使用 `uv run python -m ...`。

## 3. 已註冊 CLI 總表

| CLI | 入口 | 用途 | 典型輸出 |
| --- | --- | --- | --- |
| `hacc-compile` | `hybridacc_cc.cli:main` | workload YAML -> firmware / IR | `firmware.elf`、`hardware_ir.json`、`hardware_viz.html` |
| `hacc-setup` | `hybridacc_cc.setup_cli:main` | install / env / setup | 安裝結果與環境設定 |
| `hacc-sweep` | `hybridacc_cc.sweep_tools:main` | sweep generation / report | `manifest.json`、summary report |
| `syn-report` | `syn_report_parser.cli:main` | synthesis report 解析 | markdown summary |
| `log-parser` | `log_parser.cli:main` | log parser | 摘要輸出 |
| `trace-parser` | `trace_parser.cli:main` | trace parser | trace summary |

## 4. 最常見工作流

### 4.1 setup

```bash
cd /home/easonyeh/hybridacc
uv run hacc-setup all
uv run hacc-setup install all
uv run hacc-setup env --riscv-prefix $HOME/.local/riscv
```

### 4.2 compile 一個 workload

```bash
uv run hacc-compile design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml -o output/compile-conv3x3 --dump-ir
```

跑完後最常看的輸出：

- `output/compile-conv3x3/firmware.elf`
- `output/compile-conv3x3/hardware_ir.json`
- `output/compile-conv3x3/hardware_viz.html`

### 4.3 依 `hardware_ir.json` 生成 DRAM 與 golden

```bash
uv run python -m hybridacc_verify.gen.gen_test_dram --ir output/compile-conv3x3/hardware_ir.json --workload design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml --output-dir output/compile-conv3x3
```

常見輸出：

- `dram_init.bin`
- `golden_output.bin`
- `golden_meta.txt`

### 4.4 產生 PE / NoC 測資

```bash
uv run python -m hybridacc_verify.main gen-pe --config testbench/pe/conv_k3c4/config.json
uv run python -m hybridacc_verify.main gen-noc --config testbench/noc/conv_k3c4/config.json
```

### 4.5 驗證 simulator 結果

```bash
uv run python -m hybridacc_verify.main check --sim output/compile-conv3x3/dram_init.bin.out --expected output/compile-conv3x3/golden_output.bin --rtol 0.01 --atol 0.001 --dump-csv output/compile-conv3x3/report.csv
uv run python -m hybridacc_verify.check.compare_golden output/compile-conv3x3
```

### 4.6 E2E sweep

```bash
uv run hacc-sweep gen --workload conv3x3 --output-dir ./output/hacc-conv3x3-sweeps
./scripts/fast_entry/run_e2e.sh $(cat ./output/hacc-conv3x3-sweeps/lists/conv3x3_all.list) --output-dir ./output/hacc-conv3x3-results --jobs "$(nproc)"
uv run hacc-sweep report --manifest ./output/hacc-conv3x3-sweeps/manifest.json --results-root ./output/hacc-conv3x3-results --output-dir ./output/hacc-conv3x3-report
```

### 4.7 synthesis report parser

```bash
uv run syn-report --help
uv run python script/python/reporting/syn_report.py --report-dir ./report --output ./report/manual_summary.md
```

## 5. 額外 module 入口

以下入口沒有註冊成 CLI 名稱，但仍屬常用：

```bash
uv run python -m hybridacc_verify.main --help
uv run python -m hybridacc_verify.check.compare_golden --help
uv run python -m hybridacc_verify.gen.gen_test_dram --help
```

## 6. 命令與輸出對照

| 動作 | 命令 | 主要輸出 |
| --- | --- | --- |
| setup | `uv run hacc-setup ...` | 安裝與環境設定 |
| compile | `uv run hacc-compile ...` | `firmware.elf`、`hardware_ir.json` |
| 生成 golden | `uv run python -m hybridacc_verify.gen.gen_test_dram ...` | `dram_init.bin`、`golden_output.bin` |
| compare | `uv run python -m hybridacc_verify.check.compare_golden ...` | PASS / FAIL verdict |
| sweep gen | `uv run hacc-sweep gen ...` | `manifest.json`、`lists/` |
| sweep report | `uv run hacc-sweep report ...` | 匯總報告 |

## 7. 常見失敗點

1. 忘記 `uv sync`。
2. 在非 repo root 執行，導致相對路徑找不到。
3. 只 compile 但忘了 `--dump-ir`，後面就沒有 `hardware_ir.json` 可餵給 `gen_test_dram`。
4. 工作量改了，卻沿用舊的 `dram_init.bin` 或 `golden_output.bin`。
5. 混用系統 Python 與 `uv run`，導致套件版本不一致。

## 8. 成功判準

1. `uv run <cli> --help` 可正常啟動。
2. compile / gen / compare / sweep 產物落在指定的 `output/` 或 `report/` 目錄。
3. 你能分辨「註冊 CLI」與「module 入口」的差異。

## 9. 相關文件

- [environment-and-toolchains.md](environment-and-toolchains.md)
- [esl-workflows.md](esl-workflows.md)
- [command-cheatsheet.md](command-cheatsheet.md)
- [../../python/legacy_README.md](../../python/legacy_README.md)
- [../../python/trace_parser/README.md](../../python/trace_parser/README.md)
- [../../README.md](../../README.md)