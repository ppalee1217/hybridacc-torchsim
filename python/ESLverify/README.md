# HybridAcc ESL Verification Tool

HybridAcc PE (Processing Element) 測試資料生成與驗證工具。

## ✨ 功能特色

- **物件導向設計**：使用 class 架構，易於擴展和維護
- **靈活的輸入方式**：支援命令列參數或配置文件（JSON/YAML）
- **多種運算模式**：
  - Conv1D (4 種模式: k3c4/k5c2/k7c1/k1c12)
  - GEMM (通用矩陣乘法)
- **可配置的資料格式**：
  - 輸出格式：bin/hex
  - Layout：channels_first/channels_last
- **高精度驗證**：使用 fp16 浮點數格式

## 📦 安裝

```bash
cd /home/yoyo/work/MasterResearch/HybridAcc/python/ESLverify

# 安裝必要套件
pip install numpy torch

# (可選) 安裝 YAML 支援
pip install pyyaml
```

## 🚀 快速開始

### 使用配置文件

```bash
# 使用 JSON 配置
python -m src.pe_sim_gen --config examples/conv_k3c4.json

# 使用 YAML 配置
python -m src.pe_sim_gen --config examples/gemm.yaml
```

### 使用命令列參數

```bash
# 生成 Conv1D 測試資料
python -m src.pe_sim_gen conv \
  --mode k3c4 \
  --out-ch 8 \
  --in-width 128 \
  --out-dir ./output/conv_test

# 生成 GEMM 測試資料
python -m src.pe_sim_gen gemm \
  --out-width 64 \
  --in-width 128 \
  --dim 32 \
  --out-dir ./output/gemm_test
```

## 📖 詳細說明

### Conv1D 模式

| Mode | Kernel Size | Input Channels | Stride | 描述 |
|------|-------------|----------------|--------|------|
| k3c4 | 3 | 4 | 1 | 標準 3x4 卷積 |
| k5c2 | 5 | 2 | 1 | 5x2 卷積（特殊權重打包） |
| k7c1 | 7 | 1 | 2 | 7x1 卷積，stride=2（特殊權重打包） |
| k1c12 | 1 | 12 | 1 | 1x1 卷積（Pointwise） |

### 命令列選項

#### Conv 子命令

```
python -m src.pe_sim_gen conv [選項]

必要參數:
  --mode {k3c4,k5c2,k7c1,k1c12}  運算模式
  --out-ch N            輸出 channel 數量 (1~16)
  --in-width N          輸入寬度 (3~800)
  --out-dir PATH        輸出目錄

可選參數:
  --fmt {bin,hex}       輸出格式 (預設: bin)
  --seed N              隨機種子 (預設: 0)
  --no-ps               將 partial sum 設為 0
  --layout {channels_first,channels_last}
                        資料 layout (預設: channels_last)
```

#### GEMM 子命令

```
python -m src.pe_sim_gen gemm [選項]

必要參數:
  --out-width N         輸出寬度 (1~800)
  --in-width N          輸入寬度 (3~800)
  --dim N               中間維度 (>= 1)
  --out-dir PATH        輸出目錄

可選參數:
  --fmt {bin,hex}       輸出格式 (預設: bin)
  --seed N              隨機種子 (預設: 0)
  --no-ps               將 partial sum 設為 0
```

## 📁 專案結構

```
ESLverify/
├── README.md                # 本文件
├── src/
│   └── pe_sim_gen.py       # 主程式
├── examples/                # 配置文件範例
│   ├── README.md
│   ├── conv_k3c4.json
│   ├── conv_k5c2.json
│   ├── conv_k7c1.yaml
│   ├── gemm.json
│   └── gemm.yaml
└── output/                  # 生成的測試資料（自動創建）
```

## 📦 輸出檔案

每次執行會在指定的輸出目錄生成：

- `activation_input.{bin|hex}` - 輸入激活值
- `weight.{bin|hex}` - 權重資料
- `ps_input.{bin|hex}` - Partial sum 輸入
- `activation_output.{bin|hex}` - 輸出激活值（golden reference）
- `meta.txt` - 元資料資訊（包含所有配置參數和維度資訊）

## 🎯 使用範例

### 範例 1: 生成標準 Conv1D 測試資料

```bash
python -m src.pe_sim_gen conv \
  --mode k3c4 \
  --out-ch 4 \
  --in-width 64 \
  --fmt bin \
  --layout channels_last \
  --out-dir ./output/example1
```

### 範例 2: 使用配置文件批量生成

創建配置文件 `my_config.json`:
```json
{
  "task": "conv",
  "mode": "k5c2",
  "out_ch": 16,
  "in_width": 256,
  "fmt": "hex",
  "seed": 42,
  "layout": "channels_first",
  "out_dir": "./output/my_test"
}
```

執行:
```bash
python -m src.pe_sim_gen --config my_config.json
```

### 範例 3: GEMM 測試

```bash
python -m src.pe_sim_gen gemm \
  --out-width 128 \
  --in-width 256 \
  --dim 64 \
  --fmt bin \
  --out-dir ./output/gemm_large
```

## 🔧 進階功能

### Data Layout

- **channels_first** (NCHW): PyTorch 預設格式
  - activation_input: (C_in, W_in)
  - activation_output: (C_out, W_out)
  - weight: (C_out, C_in, K)

- **channels_last** (NHWC): TensorFlow 預設格式
  - activation_input: (W_in, C_in)
  - activation_output: (W_out, C_out)
  - weight: (K, C_in, C_out) 或特殊打包格式

### 特殊權重打包

k5c2 和 k7c1 模式使用特殊的權重打包格式以優化硬體存取模式。詳見程式碼中的 `pack_weight_mode_b` 和 `pack_weight_mode_c` 函數。

## 🛠️ 開發

### 架構設計

- `DataGenerator`: 基類，包含共用功能
- `ConvGenerator`: Conv1D 資料生成器
- `GemmGenerator`: GEMM 資料生成器
- `ConfigLoader`: 配置文件載入器
- `ConvConfig` / `GemmConfig`: 資料類別（dataclass）

### 擴展新模式

1. 在 `DataGenerator.MODES` 中新增模式定義
2. 如需特殊權重打包，實作對應的 `pack_weight_mode_*` 方法
3. 更新 `ConvConfig.validate()` 以支援新模式

## 📝 注意事項

1. 輸入寬度必須大於等於 kernel size
2. k5c2 和 k7c1 模式有特殊的權重打包格式
3. 使用 YAML 配置需要安裝 `pyyaml` 套件
4. 所有浮點運算使用 fp32，但儲存使用 fp16

## 📄 授權

本專案為 MasterResearch 的一部分。

## 🤝 貢獻

如需新增功能或回報問題，請聯絡專案維護者。
