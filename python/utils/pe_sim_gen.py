# 我想要有一個python工具可以生成hybridacc-pe測試資料:
# 1. 運算模式選擇:
#    (a) conv1D, kernel size = 3, input channel=4, stride = 1, kernel 數量 (1~16 組自選), input width (3~800 可設定)
#    (b) conv1D, kernel size = 5, input channel=2, stride = 1, kernel 數量 (1~16 組自選), input width (3~800 可設定)
#    (c) conv1D, kernel size = 7, input channel=1, stride = 2, kernel 數量 (1~16 組自選), input width (3~800 可設定)
#    (d) conv1D, kernel size = 1, input channel=12, stride = 1, kernel 數量 (1~16 組自選), input width (3~800 可設定)
#    (e) GEMM,  output width (3~800), input width (3~800)
# 2.  生成檔案:
#    (a) .bin/.hex 二選一
#    (b) 區分成 activatetion input / activation output / partial sum input / weight
# 3. 資料格式: fp16
# 4. 新增: 可選擇輸出資料 layout: channels_first (預設) 或 channels_last
#    channels_first:
#       activation_input: (C_in, W_in)
#       activation_output / ps_input: (C_out, W_out)
#       weight: (C_out, C_in, K)
#    channels_last:
#       activation_input: (W_in, C_in)
#       activation_output / ps_input: (W_out, C_out)
#       weight: (K, C_in, C_out)  (對應常見 NHWC / TF filter 佈局)

import argparse
import numpy as np
from pathlib import Path
from typing import Tuple
import torch

MODES = {
    'a': dict(kernel_size=3, in_ch=4, stride=1),
    'b': dict(kernel_size=5, in_ch=2, stride=1),
    'c': dict(kernel_size=7, in_ch=1, stride=2),
    'd': dict(kernel_size=1, in_ch=12, stride=1),
}

def conv1d_valid(act: np.ndarray, weight: np.ndarray, stride: int) -> np.ndarray:
    import torch.nn.functional as F

    # act: (C_in, W_in), weight: (C_out, C_in, K)
    act_torch = torch.tensor(act[None, :, :], dtype=torch.float32)  # Add batch dimension: (1, C_in, W_in)
    weight_torch = torch.tensor(weight, dtype=torch.float32)  # (C_out, C_in, K)
    out_torch = F.conv1d(act_torch, weight_torch, stride=stride)  # Perform 1D convolution
    out = out_torch.numpy()[0]  # Remove batch dimension: (C_out, W_out)
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

def pack_weight_mode_b(weight_cf: np.ndarray, layout: str) -> np.ndarray:
    C_out, C_in, K = weight_cf.shape  # 這裡的 weight_cf 是 (C_out, C_in, K)
    assert (C_in, K) == (2, 5), "mode b 預期 C_in=2 且 K=5"

    packed = []
    for co in range(C_out):
        seq = weight_cf[co].reshape(-1)   
        out12 = np.empty(12, dtype=seq.dtype)
        out12[0] = seq[0]     # 0
        out12[1] = seq[3]     # 3
        out12[2] = seq[5]     # 6
        out12[3] = seq[8]     # 9
        out12[4] = seq[1]     # 1
        out12[5] = seq[4]     # 4
        out12[6] = seq[6]     # 7
        out12[7] = seq[9]     # 10
        out12[8] = seq[2]     # 2
        out12[9] = 0.0        # 5
        out12[10] = seq[7]    # 8
        out12[11] = 0.0       # 11

        if layout == 'channels_last':
            block = out12.reshape(6, 2)    # (6,2)
        else:
            block = out12.reshape(2, 6)    # (2,6)

        packed.append(block)

    return np.stack(packed, axis=0).copy(order='C')

def pack_weight_mode_c(weight_cf: np.ndarray, layout: str) -> np.ndarray:
    C_out, C_in, K = weight_cf.shape
    assert (C_in, K) == (1, 7), "mode c 預期 C_in=1 且 K=7"
    
    packed = []
    for co in range(C_out):
        seq = weight_cf[co].reshape(-1)   # 7 個
        out12 = np.empty(12, dtype=seq.dtype)
        out12[0] = seq[0]     # 0
        out12[1] = seq[3]     # 3
        out12[2] = seq[6]     # 6
        out12[3] = 0.0        # 9
        out12[4] = seq[1]     # 1
        out12[5] = seq[4]     # 4
        out12[6] = 0.0        # 7
        out12[7] = 0.0        # 10
        out12[8] = seq[2]     # 2
        out12[9] = seq[5]     # 5
        out12[10:] = 0.0

        if layout == 'channels_last':
            block = out12.reshape(12, 1)    # (12,1)
        else:
            block = out12.reshape(1, 12)    # (1,12)

        packed.append(block)

    return np.stack(packed, axis=0).copy(order='C')

def generate_conv(mode: str, out_ch: int, in_width: int, fmt: str, out_dir: Path, seed: int = 0, no_ps: bool = False, layout: str = 'channels_first'):
    if mode not in MODES:
        raise ValueError("mode 必須為 a/b/c")
    if layout not in ('channels_first', 'channels_last'):
        raise ValueError("layout 需為 channels_first 或 channels_last")
    cfg = MODES[mode]
    k, in_ch, stride = cfg['kernel_size'], cfg['in_ch'], cfg['stride']
    if not (1 <= out_ch <= 16):
        raise ValueError("out_ch 範圍 1~16")
    if not (3 <= in_width <= 800):
        raise ValueError("in_width 範圍 3~800")
    if in_width < k:
        raise ValueError("input width 需 >= kernel size")
    np.random.seed(seed)
    # 內部統一使用 channels_first 計算
    act_in_cf = np.random.uniform(-1, 1, size=(in_ch, in_width)).astype(np.float32)
    weight_cf = np.random.uniform(-1, 1, size=(out_ch, in_ch, k)).astype(np.float32)
    conv_out_cf = conv1d_valid(act_in_cf, weight_cf, stride)
    ps_shape_cf = conv_out_cf.shape  # (C_out, W_out)
    ps_in_cf = np.zeros(ps_shape_cf, dtype=np.float32) if no_ps else np.random.uniform(-0.5, 0.5, size=ps_shape_cf).astype(np.float32)
    act_out_cf = conv_out_cf + ps_in_cf

    # 依 layout 轉換欲儲存的 tensor
    if layout == 'channels_first':
        act_in_save = act_in_cf
        ps_in_save = ps_in_cf
        act_out_save = act_out_cf
        if mode == 'b':
            weight_save = pack_weight_mode_b(weight_cf, layout='channels_first')  # (C_out, C_in, 6)
        elif mode == 'c':
            weight_save = pack_weight_mode_c(weight_cf, layout='channels_first')  # (C_out, C_in, 12)
        else:
            weight_save = weight_cf  # (C_out, C_in, K)

    else:  # channels_last
        act_in_save = np.transpose(act_in_cf, (1, 0))      # (W_in, C_in)
        ps_in_save  = np.transpose(ps_in_cf, (1, 0))       # (W_out, C_out)
        act_out_save = np.transpose(act_out_cf, (1, 0))    # (W_out, C_out)

        if mode == 'b':
            weight_save = pack_weight_mode_b(weight_cf, layout='channels_last')   # (C_out, 6, 2)
        elif mode == 'c':
            weight_save = pack_weight_mode_c(weight_cf, layout='channels_last')   # (C_out, 12, 1)
        else:
            weight_save = np.transpose(weight_cf, (0, 2, 1))  # (C_out, K, C_in)

    out_dir.mkdir(parents=True, exist_ok=True)
    save_array(act_in_save, out_dir / f"activation_input.{fmt}", fmt)
    save_array(weight_save, out_dir / f"weight.{fmt}", fmt)
    save_array(ps_in_save, out_dir / f"ps_input.{fmt}", fmt)
    save_array(act_out_save, out_dir / f"activation_output.{fmt}", fmt)
    meta = {
        "mode": mode, "kernel_size": k, "in_ch": in_ch, "stride": stride,
        "out_ch": out_ch, "in_width": in_width,
        "out_width": conv_out_cf.shape[1],
        "format": fmt, "seed": seed, "partial_sum_zero": no_ps,
        "layout": layout,
        "activation_input_shape_saved": act_in_save.shape,
        "activation_input_shape_internal_cf": act_in_cf.shape,
        "weight_shape_saved": weight_save.shape,
        "weight_shape_internal_cf": weight_cf.shape,
        "ps_input_shape_saved": ps_in_save.shape,
        "activation_output_shape_saved": act_out_save.shape,
    }
    (out_dir / "meta.txt").write_text("\n".join(f"{k}:{v}" for k, v in meta.items()), encoding='utf-8')
    print("資料生成完成:", out_dir)

def generate_gemm( out_width: int, in_width: int, dim: int, fmt: str, out_dir: Path,
                  seed: int = 0, no_ps: bool = False):
    
    if not (1 <= out_width <= 800):
        raise ValueError("out_width 範圍 3~800")
    if not (3 <= in_width <= 800):
        raise ValueError("in_width 範圍 3~800")

    np.random.seed(seed)

    A_cf = np.random.uniform(-1, 1, size=(dim, in_width)).astype(np.float32)   
    W_cf = np.random.uniform(-1, 1, size=(out_width, dim)).astype(np.float32)   
    # GEMM
    C_cf = W_cf @ A_cf
    ps_shape_cf = C_cf.shape                                          
    PS_cf = np.zeros(ps_shape_cf, dtype=np.float32) if no_ps else \
            np.random.uniform(-0.5, 0.5, size=ps_shape_cf).astype(np.float32)

    C_out_cf = C_cf + PS_cf
    
    W_save  = np.transpose(W_cf, (1, 0))

    out_dir.mkdir(parents=True, exist_ok=True)
    save_array(A_cf,  out_dir / f"activation_input.{fmt}", fmt)   # (dim, in_width)
    save_array(W_save,  out_dir / f"weight.{fmt}", fmt)             # (out_width, dim)
    save_array(PS_cf, out_dir / f"ps_input.{fmt}", fmt)           # (out_width, in_width)
    save_array(C_out_cf,  out_dir / f"activation_output.{fmt}", fmt)  # A_out (= C + PS) # (out_width, in_width)

    meta = {
        "task": "gemm",
        "dim": dim, "out_width": out_width, "in_width": in_width,
        "format": fmt, "seed": seed, "partial_sum_zero": no_ps,
        "A_shape_saved":  A_cf.shape,
        "W_shape_internal_cf": W_cf.shape,
        "W_shape_saved":  W_save.shape,
        "PS_shape_saved": PS_cf.shape,
        "C_shape_saved":  C_out_cf.shape,
    }
    (out_dir / "meta.txt").write_text(
        "\n".join(f"{k}:{v}" for k, v in meta.items()), encoding='utf-8'
    )
    print("GEMM 資料生成完成:", out_dir)

def parse_args():
    p = argparse.ArgumentParser(description="HybridAcc PE 測試資料生成工具 (fp16)")
    sub = p.add_subparsers(dest="task", required=True)

    # conv 子命令
    pc = sub.add_parser("conv", help="產生 conv1D 測試資料")
    pc.add_argument('--mode', choices=['a','b','c','d'], required=True)
    pc.add_argument('--out-ch', type=int, required=True)
    pc.add_argument('--in-width', type=int, required=True)
    pc.add_argument('--fmt', choices=['bin','hex'], default='bin')
    pc.add_argument('--out-dir', type=Path, required=True)
    pc.add_argument('--seed', type=int, default=0)
    pc.add_argument('--no-ps', action='store_true')
    pc.add_argument('--layout', choices=['channels_first','channels_last'], default='channels_last')

    # gemm 子命令
    pg = sub.add_parser("gemm", help="產生 GEMM 測試資料")
    pg.add_argument('--out-width', type=int, required=True)
    pg.add_argument('--in-width', type=int, required=True)
    pg.add_argument('--dim', type=int, required=True)
    pg.add_argument('--fmt', choices=['bin','hex'], default='bin')
    pg.add_argument('--out-dir', type=Path, required=True)
    pg.add_argument('--seed', type=int, default=0)
    pg.add_argument('--no-ps', action='store_true')
    return p.parse_args()

def main():
    args = parse_args()
    if args.task == 'gemm':
        generate_gemm(args.out_width, args.in_width, args.dim,
                      args.fmt, args.out_dir, args.seed, args.no_ps)
    else:  # conv
        generate_conv(args.mode, args.out_ch, args.in_width,
                      args.fmt, args.out_dir, args.seed, args.no_ps, args.layout)

if __name__ == "__main__":
    main()
