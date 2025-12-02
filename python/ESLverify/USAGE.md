# 快速使用指南

## 🚀 快速開始

### 方式 1: 使用配置文件（推薦）

```bash
cd /home/yoyo/work/MasterResearch/HybridAcc/python/ESLverify

# 使用 JSON 配置
python -m src.pe_sim_gen --config examples/conv_k3c4.json

# 使用 YAML 配置
python -m src.pe_sim_gen --config examples/gemm.yaml
```

### 方式 2: 使用命令列參數

```bash
# Conv1D
python -m src.pe_sim_gen conv --mode k3c4 --out-ch 8 --in-width 128 --out-dir ./output/test

# GEMM
python -m src.pe_sim_gen gemm --out-width 64 --in-width 128 --dim 32 --out-dir ./output/gemm
```

## 📝 創建自己的配置文件

### JSON 格式

複製 `examples/template.json` 並修改參數：

```json
{
  "task": "conv",
  "mode": "k3c4",
  "out_ch": 8,
  "in_width": 128,
  "out_dir": "./output/my_test"
}
```

### YAML 格式

複製 `examples/template.yaml` 並修改參數：

```yaml
task: conv
mode: k3c4
out_ch: 8
in_width: 128
out_dir: ./output/my_test
```

## 🧪 測試範例

```bash
# 測試所有範例配置
./examples/test_all.sh

# 測試命令列參數
./examples/test_cli.sh
```

## ❓ 常見問題

**Q: YAML 配置無法使用？**
A: 請安裝 PyYAML: `pip install pyyaml`

**Q: 如何選擇 layout？**
A: PyTorch 專案用 `channels_first`，TensorFlow 專案用 `channels_last`

**Q: 什麼時候使用 `--no-ps`？**
A: 測試純卷積/GEMM 運算時，將 partial sum 設為 0

**Q: 各個 mode 的差異？**
A:
- k3c4: kernel=3, in_ch=4, stride=1
- k5c2: kernel=5, in_ch=2, stride=1 (特殊權重打包)
- k7c1: kernel=7, in_ch=1, stride=2 (特殊權重打包)
- k1c12: kernel=1, in_ch=12, stride=1 (1x1 卷積)
