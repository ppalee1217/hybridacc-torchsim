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

### 1. 生成 PE 測試資料

目前支援透過配置檔案生成 Conv1D 或 GEMM 的測試向量。

```bash
# 假設您有一個配置檔案 examples/conv_config.json
python -m hybridacc_verify.main gen-pe --config examples/conv_config.json
```

### 2. 生成 NoC 測試資料

生成包含 NoC 路由配置與封包的測試資料。

```bash
# 生成預設的 Conv2D 與 GEMM 測試案例到 output/noc_test
python -m hybridacc_verify.main gen-noc --output-dir output/noc_test --num-pes 64
```

### 3. 驗證模擬結果

比對模擬器輸出的二進位檔案 (`.bin`) 與黃金模型輸出。

```bash
python -m hybridacc_verify.main check \
  --sim output/sim_output.bin \
  --expected output/golden_output.bin \
  --rtol 0.01 \
  --atol 0.001
```

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
sim_result = ...
gold_result = ...

# 執行比對
result = compare(sim_result, gold_result, rtol=1e-2, atol=1e-3)

if result['num_fail'] > 0:
    print(f"Verification Failed: {result['num_fail']} mismatches")
else:
    print("Verification Passed!")
```
