# HybridAcc PE 測試資料生成工具 - 範例

本目錄包含各種配置文件範例，展示如何使用 JSON 或 YAML 格式配置測試資料生成。

## 📁 檔案說明

### Conv1D 範例
- `conv_k3c4.json` - Mode k3c4 (kernel=3, in_ch=4, stride=1), channels_last, bin 格式
- `conv_k5c2.json` - Mode k5c2 (kernel=5, in_ch=2, stride=1), channels_first, hex 格式
- `conv_k7c1.yaml` - Mode k7c1 (kernel=7, in_ch=1, stride=2), channels_last, 無 partial sum

### GEMM 範例
- `gemm.json` - GEMM 測試，bin 格式
- `gemm.yaml` - GEMM 測試，hex 格式

## 🚀 使用方式

### 1. 使用配置文件

```bash
# 使用 JSON 配置
python -m ESLverify.src.pe_sim_gen --config examples/conv_k3c4.json

# 使用 YAML 配置
python -m ESLverify.src.pe_sim_gen --config examples/gemm.yaml
```

### 2. 使用命令列參數

```bash
# Conv1D
python -m ESLverify.src.pe_sim_gen conv \
  --mode k3c4 \
  --out-ch 8 \
  --in-width 128 \
  --out-dir ./output/test

# GEMM
python -m ESLverify.src.pe_sim_gen gemm \
  --out-width 64 \
  --in-width 128 \
  --dim 32 \
  --out-dir ./output/gemm_test
```

## 📋 配置參數說明

### Conv1D 配置

| 參數 | 說明 | 範圍/選項 | 預設值 |
|------|------|-----------|--------|
| `task` | 任務類型 | `conv` 或 `conv1d` | - |
| `mode` | 運算模式 | `k3c4`, `k5c2`, `k7c1`, `k1c12` | - |
| `out_ch` | 輸出 channel 數 | 1~16 | - |
| `in_width` | 輸入寬度 | 3~800 | - |
| `fmt` | 輸出格式 | `bin`, `hex` | `bin` |
| `seed` | 隨機種子 | 整數 | 0 |
| `no_ps` | 是否將 partial sum 設為 0 | true/false | false |
| `layout` | 資料排列方式 | `channels_first`, `channels_last` | `channels_last` |
| `out_dir` | 輸出目錄 | 路徑字串 | `./output` |

### GEMM 配置

| 參數 | 說明 | 範圍/選項 | 預設值 |
|------|------|-----------|--------|
| `task` | 任務類型 | `gemm` | - |
| `out_width` | 輸出寬度 | 1~800 | - |
| `in_width` | 輸入寬度 | 3~800 | - |
| `dim` | 中間維度 | >= 1 | - |
| `fmt` | 輸出格式 | `bin`, `hex` | `bin` |
| `seed` | 隨機種子 | 整數 | 0 |
| `no_ps` | 是否將 partial sum 設為 0 | true/false | false |
| `out_dir` | 輸出目錄 | 路徑字串 | `./output` |

## 🎯 Conv1D 模式說明

| Mode | Kernel Size | Input Channels | Stride | 說明 |
|------|-------------|----------------|--------|------|
| k3c4 | 3 | 4 | 1 | 標準 3x3 卷積 |
| k5c2 | 5 | 2 | 1 | 5x5 卷積 (特殊權重打包) |
| k7c1 | 7 | 1 | 2 | 7x7 卷積 (特殊權重打包) |
| k1c12 | 1 | 12 | 1 | 1x1 卷積 (pointwise) |

## 📦 輸出檔案

生成的測試資料會包含以下檔案：

- `activation_input.{fmt}` - 輸入激活值
- `weight.{fmt}` - 權重資料
- `ps_input.{fmt}` - Partial sum 輸入
- `activation_output.{fmt}` - 輸出激活值（golden reference）
- `meta.txt` - 元資料資訊

## 💡 提示

1. YAML 格式需要安裝 PyYAML：`pip install pyyaml`
2. 使用 `--no-ps` 可以將 partial sum 設為全 0（適合測試純卷積/GEMM）
3. `channels_last` 格式對應 TensorFlow 常用的 NHWC layout
4. `channels_first` 格式對應 PyTorch 常用的 NCHW layout
