#!/bin/bash
# 測試命令列參數功能

echo "======================================"
echo "測試命令列參數模式"
echo "======================================"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# 測試 Conv 命令列
echo "[1/2] 測試 Conv 命令列參數..."
python -m src.pe_sim_gen conv \
  --mode k1c12 \
  --out-ch 12 \
  --in-width 100 \
  --fmt bin \
  --layout channels_last \
  --out-dir ./output/cli_conv_test

echo ""

# 測試 GEMM 命令列
echo "[2/2] 測試 GEMM 命令列參數..."
python -m src.pe_sim_gen gemm \
  --out-width 50 \
  --in-width 100 \
  --dim 25 \
  --fmt hex \
  --seed 999 \
  --out-dir ./output/cli_gemm_test

echo ""
echo "✓ 命令列測試完成！"
