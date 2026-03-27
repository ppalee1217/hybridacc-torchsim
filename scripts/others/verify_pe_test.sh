#!/bin/bash
set -euo pipefail

PYTHON="uv run"
ENTRYPOINT="python/utils/pe_sim_verify.py"

# 預設路徑，可用環境變數 SIM / EXP 覆蓋
SIM=${SIM:-./design/hybridacc-isa/output/sim/conv1d_pol.bin}
EXP=${EXP:-./design/hybridacc-isa/data/conv3x3/activation_output.bin}
CSV=${CSV:-./design/hybridacc-isa/output/sim/conv1d_pol.csv}

if [[ ! -f "$SIM" ]]; then
  echo "找不到模擬輸出檔: $SIM" >&2
  exit 2
fi
if [[ ! -f "$EXP" ]]; then
  echo "找不到期望輸出檔: $EXP" >&2
  exit 2
fi

# 其餘參數(如 --rtol / --atol / --show / --quiet) 直接往下傳
$PYTHON $ENTRYPOINT --sim "$SIM" --expected "$EXP" "$@" --show 20
