# HybridAcc ESL Simulator — User Guide

## 目錄

1. [概述](#1-概述)
2. [環境準備](#2-環境準備)
3. [完整工作流程](#3-完整工作流程)
4. [Step 1: 定義 Workload YAML](#step-1-定義-workload-yaml)
5. [Step 2: 編譯韌體 (hacc-compile)](#step-2-編譯韌體-hacc-compile)
6. [Step 3: 產生 DRAM 測試資料](#step-3-產生-dram-測試資料)
7. [Step 4: 執行 ESL 模擬](#step-4-執行-esl-模擬)
8. [Step 5: 驗證輸出結果](#step-5-驗證輸出結果)
9. [一鍵腳本 (fast_entry)](#6-一鍵腳本-fast_entry)
10. [常見問題](#7-常見問題)

---

## 1. 概述

HybridAcc ESL Simulator 是一個 SystemC-based 的 cycle-approximate 硬體模擬器，用於驗證 HybridAcc 神經網路加速器的功能正確性。完整的驗證流程為：

```
Workload YAML  →  hacc-compile  →  gen-test-dram  →  hybridacc-sim  →  compare-golden
   (定義)          (編譯韌體)        (產生測資)         (ESL 模擬)        (輸出驗證)
```

支援的運算類型：
- **conv2d_3x3** — 3×3 二維卷積
- **conv2d_1x1** — 1×1 二維卷積 (pointwise convolution)
- **gemm** — 通用矩陣乘法 (General Matrix Multiplication)
- **multi-layer** — 多層串接 (如 conv3x3 → conv1x1)

---

## 2. 環境準備

### 2.1 安裝所有相依套件

```bash
# 一鍵安裝 (SystemC, RISC-V toolchain, Python packages)
scripts/setup.sh all

# 或分別安裝
scripts/setup.sh install systemc
scripts/setup.sh install riscv
scripts/setup.sh install python
```

### 2.2 啟動開發環境

```bash
# 使用 uv (推薦)
source .venv/bin/activate

# 驗證 CLI 可用
hacc-compile --help
```

### 2.3 建置 ESL 模擬器

```bash
# Release build
scripts/fast_entry/hybridacc_sim.sh build

# Debug build (含除錯訊息)
scripts/fast_entry/hybridacc_sim.sh build_debug
```

---

## 3. 完整工作流程

以 conv2d_3x3 為例的完整端到端流程：

```bash
# 1. 編譯韌體
hacc-compile design/hybridacc-cc/example/conv2d_3x3_example_test.yaml \
    -o output/test_conv3x3 --dump-ir

# 2. 產生 DRAM 測試資料 + golden output
uv run python -m hybridacc_verify.gen.gen_test_dram \
    --ir output/test_conv3x3/hardware_ir.json \
    --workload design/hybridacc-cc/example/conv2d_3x3_example_test.yaml \
    --output-dir output/test_conv3x3

# 3. ESL 模擬
scripts/fast_entry/hybridacc_sim.sh run-task output/test_conv3x3

# 4. 驗證結果
uv run python -m hybridacc_verify.check.compare_golden output/test_conv3x3
```

---

## Step 1: 定義 Workload YAML

Workload YAML 定義了硬體配置、張量形狀、和要執行的運算。

### Conv2D 3×3 範例

```yaml
name: conv2d_3x3_test
hardware:
  num_clusters: 1
  num_pes: 48
  num_bus: 3
  spm_banks_per_group: 3
  spm_bank_depth: 8192
  dram_base: 0x80000000

tensors:
  input:
    shape: [1, 16, 16, 4]    # NHWC
    dtype: fp16
    layout: NHWC
  weight:
    shape: [16, 3, 3, 4]     # OC, KH, KW, IC
    dtype: fp16
    layout: OIHW
  output:
    shape: [1, 14, 14, 16]   # NHWC
    dtype: fp16
    layout: NHWC

ops:
  - name: conv1
    type: conv2d_3x3
    inputs: [input, weight]
    outputs: [output]
    attrs:
      stride: 1
      padding: 0
```

### Conv2D 1×1 範例

```yaml
name: conv2d_1x1_test
hardware:
  num_clusters: 4
  num_pes: 64
  num_bus: 4
  spm_banks_per_group: 3
  spm_bank_depth: 4096
  dram_base: 0x80000000

tensors:
  input:
    shape: [1, 14, 14, 48]
    dtype: fp16
    layout: NHWC
  weight:
    shape: [16, 1, 1, 48]
    dtype: fp16
    layout: OIHW
  output:
    shape: [1, 14, 14, 16]
    dtype: fp16
    layout: NHWC

ops:
  - name: conv1x1
    type: conv2d_1x1
    inputs: [input, weight]
    outputs: [output]
    attrs:
      stride: 1
      padding: 0
```

### GEMM 範例

```yaml
name: gemm_test
hardware:
  num_clusters: 4
  num_pes: 64
  num_bus: 4
  spm_banks_per_group: 3
  spm_bank_depth: 4096
  dram_base: 0x80000000

tensors:
  A:
    shape: [32, 64]
    dtype: fp16
  B:
    shape: [64, 48]
    dtype: fp16
  C:
    shape: [32, 48]
    dtype: fp16

ops:
  - name: gemm1
    type: gemm
    inputs: [A, B]
    outputs: [C]
```

### Multi-layer 範例

```yaml
name: multi_layer_test
hardware:
  num_clusters: 4
  num_pes: 64
  num_bus: 4
  spm_banks_per_group: 3
  spm_bank_depth: 4096
  dram_base: 0x80000000

tensors:
  input:
    shape: [1, 14, 14, 48]
    dtype: fp16
    layout: NHWC
  weight_3x3:
    shape: [48, 3, 3, 48]
    dtype: fp16
    layout: OIHW
  mid:
    shape: [1, 12, 12, 48]
    dtype: fp16
    layout: NHWC
  weight_1x1:
    shape: [48, 1, 1, 48]
    dtype: fp16
    layout: OIHW
  output:
    shape: [1, 12, 12, 48]
    dtype: fp16
    layout: NHWC

ops:
  - name: conv3x3
    type: conv2d_3x3
    inputs: [input, weight_3x3]
    outputs: [mid]
    attrs:
      stride: 1
      padding: 0
  - name: conv1x1
    type: conv2d_1x1
    inputs: [mid, weight_1x1]
    outputs: [output]
    attrs:
      stride: 1
      padding: 0
```

### 硬體參數約束

| 參數 | 範圍 | 說明 |
|------|------|------|
| `num_clusters` | 1–16 | Cluster 數量 |
| `num_pes` | 1–256 | 每個 Cluster 的 PE 數，需整除 `num_bus` |
| `num_bus` | 1–16 | 每個 Cluster 的 MBUS 通道數 |
| `spm_banks_per_group` | 1–8 | SPM bank 群組大小 |
| `spm_bank_depth` | ≥1024 (2 的冪) | 每個 bank 的深度 |
| `dram_base` | 4KB 對齊 | DRAM 起始位址 |

---

## Step 2: 編譯韌體 (hacc-compile)

```bash
hacc-compile <workload.yaml> -o <output_dir> [options]
```

### 常用選項

| 選項 | 說明 |
|------|------|
| `-o DIR` | 輸出目錄 (預設: `build/`) |
| `--dump-ir` | 輸出 WorkloadIR / HardwareIR JSON，並額外產生 `hardware_viz.html` 視覺化頁面 |
| `--no-compile` | 僅產生 C 原始碼，不執行 GCC |
| `--dry-run` | 印出 GCC 指令但不執行 |
| `--opt-level {0,1,2,s}` | 最佳化等級 (預設: 2) |

### 輸出檔案

```
output_dir/
├── firmware.elf          # RISC-V ELF (最終產物)
├── firmware_main.c       # 進入點
├── firmware_ops.c        # 運算邏輯
├── firmware_data.c       # Layer 配置
├── firmware_hw.h         # MMIO/DMA 定義
├── firmware_payload.h    # PE 二進位 payload
├── linker.ld             # 連結腳本
├── workload_ir.json      # (--dump-ir) 前端 IR
├── hardware_ir.json      # (--dump-ir) 後端 IR，包含 tiling_params
└── hardware_viz.html     # (--dump-ir) 視覺化頁面：scan-chain / SPM mapping / 4D AGU iter,stride
```

`hardware_ir.json` 中的 `tiling_params` 包含 DRAM layout 資訊，是產生測試資料的關鍵輸入。

`hardware_viz.html` 可直接在瀏覽器或 VS Code 內建預覽中開啟，用來檢查：
- scan-chain 拓撲與 route mode
- SPM ping/pong 視窗與 mapping register
- 四組 AGU 的 4D iter/stride 配置

---

## Step 3: 產生 DRAM 測試資料

```bash
uv run python -m hybridacc_verify.gen.gen_test_dram \
    --ir <hardware_ir.json> \
    --workload <workload.yaml> \
    --output-dir <output_dir> \
    [--seed 42]
```

此工具自動偵測運算類型 (conv2d_3x3, conv2d_1x1, gemm)，為每一層產生：
- **隨機 fp16 輸入張量** (activation, weight)
- **Golden 輸出** (golden conv2d / gemm 計算)
- **DRAM mirror image** (`dram_init.bin`)

### 輸出檔案

```
output_dir/
├── dram_init.bin         # DRAM 映像 (傳入模擬器)
├── golden_output.bin     # 預期輸出 (最末層)
└── golden_meta.txt       # 元資料 (DRAM 偏移量, 形狀, seed)
```

### 支援的運算類型

| 類型 | 張量 | Golden 計算 |
|------|------|-------------|
| `conv2d_3x3` | input[NHWC], weight[OKHKWC] | $O_{n,h,w,c} = \sum_{kh,kw,ic} I_{n,h+kh,w+kw,ic} \cdot W_{c,kh,kw,ic}$ |
| `conv2d_1x1` | input[NHWC], weight[OC,1,1,IC] | Pointwise: $O_{n,h,w,c} = \sum_{ic} I_{n,h,w,ic} \cdot W_{c,0,0,ic}$ |
| `gemm` | A[M,K], B[K,N] | $C_{m,n} = \sum_k A_{m,k} \cdot B_{k,n}$ |

### 多層支援

對於 multi-layer workload，gen_test_dram 會逐層計算 golden output，前一層的輸出作為後一層的輸入，最終只保留最後一層的輸出作為驗證目標。

---

## Step 4: 執行 ESL 模擬

```bash
# 方式 A: 使用 fast_entry 腳本 (推薦)
scripts/fast_entry/hybridacc_sim.sh run-task <output_dir> [options]

# 方式 B: 直接呼叫模擬器
design/hybridacc-ESL/simulator/build/bin/hybridacc-sim \
    --mirror <dram_init.bin> \
    [--max-cycles 1000000] \
    <firmware.elf>
```

### 模擬器選項

| 選項 | 說明 |
|------|------|
| `--mirror FILE` | 載入/傾印 DRAM 映像（自動偵測 `dram_init.bin`） |
| `--max-cycles N` | 最大模擬 cycle 數 (預設 500000) |
| `--core-debug` | 印出 core pipeline trace |
| `--dma-check` | 啟用 DMA loopback 驗證 |
| `--fw-check` | 解析 DSRAM 韌體測試結果 |
| `-M SIZE` | DRAM 大小 (如 `1M`, `256M`) |

### 自動偵測

當 `output_dir` 中存在 `dram_init.bin` 時，`run-task` 會自動加上 `--mirror` 參數。模擬結束後，DRAM 內容會寫回 `dram_init.bin.out`。

---

## Step 5: 驗證輸出結果

```bash
# 使用整合驗證工具
uv run python -m hybridacc_verify.check.compare_golden <output_dir> [--tolerance 0.99]
```

### 驗證機制

1. 讀取 `golden_meta.txt` 取得 DRAM output 的起始位址和大小
2. 從 `dram_init.bin.out` (sparse format) 擷取實際輸出區段
3. 將 golden 與 actual 逐元素比較 (fp16)
4. 計算以下指標：

| 指標 | 說明 |
|------|------|
| **Cosine Similarity** | 主要判定標準 (預設 ≥ 0.99 為 PASS) |
| **MSE** | 均方誤差 |
| **Max Absolute Diff** | 最大絕對差 |
| **Exact Matches** | bit-exact 相符比例 |

### 進階驗證 (fp16 逐元素比對)

```bash
# 使用底層 comparator，支援 rtol/atol 與 CSV 輸出
uv run python -m hybridacc_verify.check.comparator \
    --sim <output_dir>/actual_output.bin \
    --expected <output_dir>/golden_output.bin \
    --rtol 1e-2 --atol 1e-3 \
    --dump-csv result.csv
```

---

## 6. 一鍵腳本 (fast_entry)

### 單一 Workload

```bash
scripts/fast_entry/run_e2e.sh <workload.yaml> [--output-dir <dir>] [--seed 42]
```

自動完成：編譯 → 產生測資 → 模擬 → 驗證。

### 批次處理

```bash
# 多個 YAML 檔案
scripts/fast_entry/run_e2e.sh \
    design/hybridacc-cc/example/conv2d_3x3_example_test.yaml \
    design/hybridacc-cc/example/conv2d_1x1_example.yaml \
    design/hybridacc-cc/example/gemm_example.yaml

# 用 glob
scripts/fast_entry/run_e2e.sh design/hybridacc-cc/example/*.yaml

# 限制平行 worker 數量
scripts/fast_entry/run_e2e.sh design/hybridacc-cc/example/*.yaml --jobs 8
```

- 批次模式若未指定 `--jobs`，預設會使用 `nproc` 個 worker 平行執行。
- terminal 會顯示 live progress dashboard；每個 workload 的完整輸出會寫到對應 build directory 下的 `e2e_run.log`。

### Sweep 報表中的 utilization 指標

若使用 `uv run hacc-sweep report` 彙整 batch 結果，目前報表會區分兩個 MAC utilization 指標：

- `core_level_macs_utilization_pct`：以 `core_probe_cycles_total` 的 lifecycle 視窗計算；若 simulator 有 `drain_out_end_cycle`，則對齊到 drain-out 結束，否則才 fallback 到 `EBREAK`。
- `cluster_level_macs_utilization_pct`：只以 cluster control state 為 `RUN` 的 cycle 計算。

模擬器會直接在 `sim.log` 輸出 `[SIM] Cluster RUN cycles: <n>`，報表會直接讀取這個數值，因此不需要另外開 trace 來回推 cluster 的執行區間。

---

## 7. 常見問題

### Q: 模擬掛住 / 超時

- 增加 `--max-cycles`：`run-task <dir> --max-cycles 2000000`
- 檢查韌體是否正確執行 EBREAK (halt)
- 開啟 core-debug 觀察 pipeline：`run-task <dir> --core-debug`

### Q: Cosine similarity 低

- 檢查 DRAM layout 是否對齊 (`golden_meta.txt` 中的位址)
- 確認 weight/input 的 layout 順序 (NHWC vs NCHW)
- 使用 `--dump-csv` 檢查逐元素差異

### Q: ELF not found

- 確認已執行 `hacc-compile` 且 RISC-V GCC 可用
- 檢查 `--gcc` 路徑：`which riscv32-unknown-elf-gcc`

### Q: 模擬器未建置

```bash
scripts/fast_entry/hybridacc_sim.sh build
```

### Q: 產生測資時 hardware_ir.json 不存在

編譯時需加 `--dump-ir` 選項：
```bash
hacc-compile workload.yaml -o build/ --dump-ir
```
