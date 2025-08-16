# 我想要有一個python工具可以生成hybridacc-pe測試資料:
# 1. 運算模式選擇:
#    (a) conv1D, kernel size = 3, input channel=4, stride = 1, kernel 數量 (1~16 組自選), input width (3~800 可設定)
#    (b) conv1D, kernel size = 5, input channel=2, stride = 1, kernel 數量 (1~16 組自選), input width (3~800 可設定)
#    (c) conv1D, kernel size = 7, input channel=1, stride = 2, kernel 數量 (1~16 組自選), input width (3~800 可設定)
# 2.  生成檔案:
#    (a) .bin/.hex 二選一
#    (b) 區分成 activatetion input / activation output / partial sum input / weight
# 3. 資料格式: fp16

import argparse
import numpy as np
from pathlib import Path
from typing import Tuple

MODES = {
    'a': dict(kernel_size=3, in_ch=4, stride=1),
    'b': dict(kernel_size=5, in_ch=2, stride=1),
    'c': dict(kernel_size=7, in_ch=1, stride=2),
}

def conv1d_valid(act: np.ndarray, weight: np.ndarray, stride: int) -> np.ndarray:
    # act: (C_in, W_in), weight: (C_out, C_in, K)
    C_out, C_in, K = weight.shape
    _, W_in = act.shape
    W_out = (W_in - K) // stride + 1
    out = np.zeros((C_out, W_out), dtype=np.float32)
    for co in range(C_out):
        for w in range(W_out):
            acc = 0.0
            base = w * stride
            # 向量化卷積 (C_in, K)
            acc = np.sum(act[:, base:base+K] * weight[co])
            out[co, w] = acc
    return out

def save_array(arr: np.ndarray, path: Path, fmt: str):
    arr16 = arr.astype(np.float16)
    if fmt == 'bin':
        path.write_bytes(arr16.tobytes())
    else:
        # 轉成 uint16 bit pattern，小端視圖
        u16 = arr16.view(np.uint16).reshape(-1)
        with path.open('w') as f:
            for v in u16:
                f.write(f"{v:04x}\n")

def generate(mode: str, out_ch: int, in_width: int, fmt: str, out_dir: Path, seed: int = 0, no_ps: bool = False):
    if mode not in MODES:
        raise ValueError("mode 必須為 a/b/c")
    cfg = MODES[mode]
    k, in_ch, stride = cfg['kernel_size'], cfg['in_ch'], cfg['stride']
    if not (1 <= out_ch <= 16):
        raise ValueError("out_ch 範圍 1~16")
    if not (3 <= in_width <= 800):
        raise ValueError("in_width 範圍 3~800")
    if in_width < k:
        raise ValueError("input width 需 >= kernel size")
    np.random.seed(seed)
    act_in = np.random.uniform(-1, 1, size=(in_ch, in_width)).astype(np.float32)
    weight = np.random.uniform(-1, 1, size=(out_ch, in_ch, k)).astype(np.float32)
    conv_out = conv1d_valid(act_in, weight, stride)
    ps_shape = conv_out.shape
    ps_in = np.zeros(ps_shape, dtype=np.float32) if no_ps else np.random.uniform(-0.5, 0.5, size=ps_shape).astype(np.float32)
    act_out = conv_out + ps_in
    out_dir.mkdir(parents=True, exist_ok=True)
    save_array(act_in, out_dir / f"activation_input.{fmt}")
    save_array(weight, out_dir / f"weight.{fmt}")
    save_array(ps_in, out_dir / f"ps_input.{fmt}")
    save_array(act_out, out_dir / f"activation_output.{fmt}")
    meta = {
        "mode": mode, "kernel_size": k, "in_ch": in_ch, "stride": stride,
        "out_ch": out_ch, "in_width": in_width,
        "out_width": conv_out.shape[1],
        "format": fmt, "seed": seed, "partial_sum_zero": no_ps
    }
    (out_dir / "meta.txt").write_text("\n".join(f"{k}:{v}" for k, v in meta.items()), encoding='utf-8')

def parse_args():
    p = argparse.ArgumentParser(description="HybridAcc PE 測試資料生成工具 (fp16)")
    p.add_argument('--mode', choices=['a','b','c'], required=True, help='a/b/c 對應題述三種配置')
    p.add_argument('--out-ch', type=int, required=True, help='輸出 kernel(通道) 數 1~16')
    p.add_argument('--in-width', type=int, required=True, help='輸入寬度 3~800')
    p.add_argument('--fmt', choices=['bin','hex'], default='bin', help='輸出格式')
    p.add_argument('--out-dir', type=Path, required=True, help='輸出資料夾')
    p.add_argument('--seed', type=int, default=0, help='隨機種子')
    p.add_argument('--no-ps', action='store_true', help='partial sum 改為全 0')
    return p.parse_args()

def main():
    args = parse_args()
    generate(args.mode, args.out_ch, args.in_width, args.fmt, args.out_dir, args.seed, args.no_ps)

if __name__ == "__main__":
    main()
