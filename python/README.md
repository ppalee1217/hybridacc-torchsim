# HybridAcc Verification Framework

`hybridacc_verify` 是一個統一的驗證框架，旨在支援 HybridAcc 專案中的 ISA、ESL (SystemC) 和 RTL (Verilog/SystemVerilog) 層級的協同設計與驗證。

此套件提供了「單一真理來源 (Single Source of Truth)」的黃金模型 (Golden Models)、測試資料生成器以及結果比對工具。

## 安裝

在 `python/` 目錄下（即本檔案所在目錄），使用 pip 以編輯模式安裝：

```bash
pip install -e .
```

安裝後，您可以在任何地方（包括 RTL testbench 或其他 Python 腳本中）透過 `import hybridacc_verify` 來使用此套件。

## 專案結構

```text
python/
├── hybridacc_verify/       # 套件根目錄
│   ├── model/              # 黃金模型 (Golden Models)
│   │   ├── conv.py         # 卷積運算 (Conv1D/Conv2D)
│   │   └── gemm.py         # 矩陣乘法 (GEMM)
│   ├── gen/                # 測試資料生成器
│   │   ├── pe_gen.py       # PE 層級測試向量生成
│   │   └── noc_gen.py      # NoC 層級測試資料生成
│   ├── check/              # 驗證與比對工具
│   │   └── comparator.py   # 浮點數/二進位檔案比對邏輯
│   ├── utils/              # 共用工具
│   │   ├── config.py       # 配置類別 (Dataclasses)
│   │   ├── data.py         # 資料結構 (TestData)
│   │   └── io.py           # 檔案 I/O
│   └── main.py             # 統一 CLI 入口
├── pyproject.toml          # 專案定義檔
└── README.md               # 本文件
```

## 使用方法 (CLI)

您可以透過 `python -m hybridacc_verify.main` 來呼叫各種功能。

### 1. 生成 PE 測試資料 (`gen-pe`)

生成用於單一 PE 驗證的測試向量 (Input, Weight, Output)。

**使用配置檔案 (推薦):**

```bash
# 生成 Conv 測試資料
python -m hybridacc_verify.main gen-pe --config testbench/pe/conv_k3c4/config.json

# 生成 GEMM 測試資料
python -m hybridacc_verify.main gen-pe --config testbench/pe/gemv/config.json
```

**PE 配置檔案範例 (`conv`):**
```json
{
  "task": "conv",
  "mode": "k3c4",          // 模式: k3c4, k5c2, k7c1, k1c12
  "out_ch": 8,             // 輸出通道數 (1~16)
  "in_width": 128,         // 輸入寬度
  "fmt": "bin",            // 輸出格式: bin 或 hex
  "seed": 42,              // 隨機種子
  "no_ps": false,          // 是否將 Partial Sum 設為 0
  "layout": "channels_last", // 資料排列
  "out_dir": "./output/pe-sim/conv_k3c4" // 輸出目錄
}
```

**PE 配置檔案範例 (`gemm`):**
```json
{
  "task": "gemm",
  "out_width": 64,         // 輸出寬度 (N)
  "in_width": 128,         // 輸入寬度 (M)
  "dim": 32,               // 中間維度 (K)
  "fmt": "bin",
  "seed": 123,
  "no_ps": false,
  "out_dir": "./output/pe-sim/gemv"
}
```

### 2. 生成 NoC 測試資料 (`gen-noc`)

生成包含 NoC 路由配置、PE 程式碼 (Assembly) 與資料封包的系統級測試資料。

**使用配置檔案:**

```bash
# 生成 Conv2D 系統測試資料
python -m hybridacc_verify.main gen-noc --config testbench/noc/conv_k3c4/config.json

# 生成 GEMM 系統測試資料
python -m hybridacc_verify.main gen-noc --config testbench/noc/gemm/config.json
```

**NoC 配置檔案範例 (`conv2d`):**
```json
{
  "mode": "conv2d",
  "num_pes": 64,           // PE 總數
  "num_bus": 4,            // 匯流排數量
  "stride": 1,
  "input_h": 18,
  "input_w": 200,
  "input_c": 4,
  "out_ch": 16,
  "kernel_h": 3,
  "kernel_w": 3,
  "seed": 123,
  "padding": 0,
  "out_dir": "./output/noc-sim/conv_k3c4"
}
```

**NoC 配置檔案範例 (`gemm`):**
```json
{
    "mode": "gemm",
    "num_pes": 64,
    "M": 32,               // 矩陣 A 的列數
    "N": 32,               // 矩陣 B 的行數
    "K": 32,               // 共同維度
    "seed": 123,
    "out_dir": "./output/noc-sim/gemm"
}
```

### 3. 驗證模擬結果 (`check`)

比對模擬器輸出的二進位檔案 (`.bin`) 與黃金模型輸出。

```bash
python -m hybridacc_verify.main check \
  --sim output/sim_output.bin \
  --expected output/golden_output.bin \
  --rtol 0.01 \
  --atol 0.001 \
  --show 5 \
  --dump-csv report.csv
```

*   `--sim`: 模擬器產生的結果檔案。
*   `--expected`: Python 生成的黃金標準檔案。
*   `--rtol`: 相對誤差容忍值 (Relative Tolerance)。
*   `--atol`: 絕對誤差容忍值 (Absolute Tolerance)。
*   `--show`: 顯示前 N 個不吻合的詳細資訊。
*   `--dump-csv`: 將詳細比對結果輸出為 CSV 檔案。

## 使用方法 (Python API)

您可以在自己的測試腳本中直接引用核心邏輯。

### 引用黃金模型

```python
import numpy as np
from hybridacc_verify.model.conv import golden_conv1d

# 準備輸入數據
act = np.random.randn(4, 128).astype(np.float32)  # (InCh, Width)
weight = np.random.randn(8, 4, 3).astype(np.float32) # (OutCh, InCh, Kernel)

# 計算預期結果
output = golden_conv1d(act, weight, stride=1)
```

### 引用比對邏輯

```python
from hybridacc_verify.check.comparator import compare

# 讀取或產生數據
sim_result = ... # numpy array or list
gold_result = ... # numpy array or list

# 執行比對
result = compare(sim_result, gold_result, rtol=1e-2, atol=1e-3)

if result['num_fail'] > 0:
    print(f"Verification Failed: {result['num_fail']} mismatches")
    print(f"Max Abs Error: {result['max_abs']}")
else:
    print("Verification Passed!")
```

## Testbench 整合

本專案的 `testbench/` 目錄存放了預定義的測試案例配置。

*   `testbench/pe/`: 存放單一 PE 的測試配置 (`config.json`)。
*   `testbench/noc/`: 存放 NoC 系統級測試配置 (`config.json`) 與對應的 PE 程式碼 (`pe_program.asm`)。

您可以透過 `scripts/` 目錄下的 shell script 來自動化生成與執行測試：

```bash
# 生成所有 PE 測試資料
./scripts/gen_pe_tb.sh -a
```

```bash
# 生成所有 NoC 測試資料
./scripts/gen_noc_tb.sh -a
```
