#!/bin/bash
# 測試所有範例配置文件

echo "======================================"
echo "HybridAcc PE 測試資料生成 - 範例測試"
echo "======================================"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

# 測試 JSON 配置
echo "[1/5] 測試 conv_k3c4.json (Mode k3c4, channels_last, bin)..."
python -m src.pe_sim_gen --config examples/conv_k3c4.json
echo ""

echo "[2/5] 測試 conv_k5c2.json (Mode k5c2, channels_first, hex)..."
python -m src.pe_sim_gen --config examples/conv_k5c2.json
echo ""

echo "[3/5] 測試 gemm.json (GEMM, bin)..."
python -m src.pe_sim_gen --config examples/gemm.json
echo ""

# 測試 YAML 配置
echo "[4/5] 測試 conv_k7c1.yaml (Mode k7c1, no PS, bin)..."
python -m src.pe_sim_gen --config examples/conv_k7c1.yaml
echo ""

echo "[5/5] 測試 gemm.yaml (GEMM, hex)..."
python -m src.pe_sim_gen --config examples/gemm.yaml
echo ""

echo "======================================"
echo "✓ 所有測試完成！"
echo "======================================"
echo ""
echo "生成的檔案位於 output/ 目錄下："
ls -lh output/
