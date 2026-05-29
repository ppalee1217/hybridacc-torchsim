# ESL Workflows

## 1. 適用範圍

這份文件整理 `design/hybridacc-ESL` 的可操作流程，涵蓋：

1. workload YAML
2. firmware / IR compile
3. DRAM 與 golden 生成
4. ESL simulator build / run
5. compare 與 debug 入口

若你是第一次把自訂 workload 串到 ESL，請直接照這份文件走。

## 2. 前置條件

- 在 repo root：`/home/easonyeh/hybridacc`
- `uv sync` 已完成
- `scripts/setup.sh all` 或 `uv run hacc-setup all` 已完成
- SystemC 與 RISC-V toolchain 可用

## 3. 先知道哪些 workload 是已驗證基線

下列三個 YAML 是最穩定的已知案例：

1. `design/hybridacc-cc/example/conv1x1/conv2d_1x1_single_wave.yaml`
2. `design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml`
3. `design/hybridacc-cc/example/gemm/gemm_single_wave.yaml`

若是新流程第一次 bring-up，建議先從這三個例子開始，再換自訂 workload。

## 4. 標準 ESL 流程

### 4.1 建置 simulator

```bash
scripts/setup.sh fast hybridacc-sim build
```

### 4.2 compile workload

```bash
uv run hacc-compile design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml -o output/esl-conv3x3 --dump-ir
```

重要輸出：

- `output/esl-conv3x3/firmware.elf`
- `output/esl-conv3x3/hardware_ir.json`
- `output/esl-conv3x3/hardware_viz.html`

### 4.3 生成 DRAM 與 golden

```bash
uv run python -m hybridacc_verify.gen.gen_test_dram --ir output/esl-conv3x3/hardware_ir.json --workload design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml --output-dir output/esl-conv3x3
```

重要輸出：

- `dram_init.bin`
- `golden_output.bin`
- `golden_meta.txt`

### 4.4 執行 ESL simulator

```bash
scripts/setup.sh fast hybridacc-sim run-task output/esl-conv3x3
```

### 4.5 比對輸出

```bash
uv run python -m hybridacc_verify.check.compare_golden output/esl-conv3x3
```

## 5. 自訂 workload 時至少要有什麼

YAML 至少要能正確描述以下內容：

1. `hardware` 設定，例如 cluster / PE / bus / SPM 參數
2. tensor 形狀、dtype、layout
3. operation 類型，例如 `conv2d_3x3`、`conv2d_1x1`、`gemm`

實務上，第一次寫新 YAML 時，建議從現有 example 複製再改，而不是從空白開始。

### 5.1 常用硬體參數約束

| 參數 | 範圍 | 說明 |
| --- | --- | --- |
| `num_clusters` | 1-16 | Cluster 數量 |
| `num_pes` | 1-256 | 每個 Cluster 的 PE 數，需與 `num_bus` 對應 |
| `num_bus` | 1-16 | 每個 Cluster 的 MBUS 通道數 |
| `spm_banks_per_group` | 1-8 | SPM bank 群組大小 |
| `spm_bank_depth` | 至少 1024 且為 2 的冪 | 每個 bank 的深度 |
| `dram_base` | 4KB 對齊 | DRAM 起始位址 |

### 5.2 compile 常用選項

```bash
uv run hacc-compile <workload.yaml> -o <output_dir> --dump-ir --opt-level 2
```

常用選項：

1. `--dump-ir`: 產出 `workload_ir.json`、`hardware_ir.json`、`hardware_viz.html`
2. `--no-compile`: 只產生 C 原始碼，不執行 GCC
3. `--dry-run`: 印出 GCC 指令但不執行
4. `--opt-level {0,1,2,s}`: 最佳化等級

### 5.3 `hardware_viz.html` 應該看什麼

`hardware_viz.html` 最適合用來檢查：

1. scan-chain 拓撲
2. SPM mapping
3. AGU iter / stride 設定

## 6. 這個 workflow 的輸入與輸出關係

| 階段 | 輸入 | 輸出 |
| --- | --- | --- |
| compile | workload YAML | `firmware.elf`、`hardware_ir.json` |
| test data gen | `hardware_ir.json`、workload YAML | `dram_init.bin`、`golden_output.bin` |
| ESL run | case output dir | simulator result |
| compare | simulator result、golden | PASS / FAIL verdict |

## 7. 最常見失敗點

1. SystemC 或 RISC-V toolchain 未就緒。
2. compile 沒有 `--dump-ir`，後面缺 `hardware_ir.json`。
3. workload YAML 改了，卻沿用舊的 `dram_init.bin` 或 `golden_output.bin`。
4. 直接跳過 DRAM generation，導致 simulator 缺必要輸入。
5. compare fail 時只看結果，不回頭比對 `golden_meta.txt` 與 IR。

## 8. 成功判準

1. build 成功。
2. `firmware.elf`、`hardware_ir.json`、`dram_init.bin`、`golden_output.bin` 都存在。
3. `compare_golden` 給出 PASS。

### 8.1 compare 常看指標

至少要能解讀：

1. cosine similarity
2. MSE
3. max absolute diff
4. exact match 比例

## 9. 出問題時怎麼分流

1. compile fail：先看 YAML 與 compiler 錯誤。
2. gen_test_dram fail：先看 `hardware_ir.json` 是否存在且與 workload 對得上。
3. simulator fail：先檢查 build 與 task 目錄是否完整。
4. compare fail：先分是輸出全壞，還是少數數值差異。

## 10. fast entry / 批次入口

若你要快速走完編譯、測資、模擬、驗證，可直接使用：

```bash
scripts/fast_entry/run_e2e.sh design/hybridacc-cc/example/conv2d_3x3_example_test.yaml
scripts/fast_entry/run_e2e.sh design/hybridacc-cc/example/*.yaml --jobs 8
```

這條路徑會自動完成：編譯、產生測資、執行模擬、驗證結果，完整 log 會落在對應 output case 目錄下的 `e2e_run.log`。

## 11. 相關文件

- [quick-start.md](quick-start.md)
- [python-cli-reference.md](python-cli-reference.md)
- [environment-and-toolchains.md](environment-and-toolchains.md)
- [../../design/hybridacc-ESL/README.md](../../design/hybridacc-ESL/README.md)
- [../../design/hybridacc-ESL/doc/index.md](../../design/hybridacc-ESL/doc/index.md)
- [../../design/hybridacc-ESL/doc/guide/README.md](../../design/hybridacc-ESL/doc/guide/README.md)