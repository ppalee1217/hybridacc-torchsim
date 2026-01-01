import argparse
import math
import os
import sys
import struct
from typing import Sequence, Tuple, Dict, Any

try:
    import numpy as np  # type: ignore
    _HAVE_NUMPY = True
except Exception:  # pragma: no cover
    _HAVE_NUMPY = False

try:
    import pandas as pd  # type: ignore
    _HAVE_PANDAS = True
except Exception:  # pragma: no cover
    _HAVE_PANDAS = False


def _read_fp16_file(path: str):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) % 2 != 0:
        raise ValueError(f"檔案長度不是 2 的倍數 (fp16) : {path}")
    if _HAVE_NUMPY:
        arr = np.frombuffer(data, dtype=np.float16)
        return arr
    # fallback without numpy
    count = len(data) // 2
    vals = []
    unpack = struct.Struct('<e').unpack_from  # half precision
    for i in range(count):
        vals.append(unpack(data, i * 2)[0])
    return vals  # python list of floats


def _to_uint16_sequence(seq):
    if _HAVE_NUMPY and isinstance(seq, np.ndarray):
        return seq.view(np.uint16)
    out = []
    pack = struct.Struct('<e')
    for v in seq:
        # pack to half then unpack raw 16 bits
        b = pack.pack(v)
        out.append(int.from_bytes(b, 'little'))
    return out


def _fp16_components(seq):
    """回傳 (u16_array, sign, exp, mantissa)；若無 numpy 則回傳 list。"""
    if _HAVE_NUMPY and isinstance(seq, np.ndarray):
        u16 = seq.view(np.uint16)
        sign = (u16 >> 15) & 0x1
        exp = (u16 >> 10) & 0x1F
        mant = u16 & 0x3FF
        return u16, sign, exp, mant
    # fallback list 實作
    u16_list = _to_uint16_sequence(seq)
    sign = [(v >> 15) & 0x1 for v in u16_list]
    exp = [(v >> 10) & 0x1F for v in u16_list]
    mant = [v & 0x3FF for v in u16_list]
    return u16_list, sign, exp, mant


def compare(sim, exp, rtol: float, atol: float) -> Dict[str, Any]:
    if len(sim) != len(exp):
        raise ValueError(f"長度不相等: sim={len(sim)} expected={len(exp)} (元素個數)")
    if _HAVE_NUMPY and isinstance(sim, np.ndarray):
        exp_arr = exp if isinstance(exp, np.ndarray) else np.array(exp, dtype=np.float16)
        sim_arr = sim
        sim32 = sim_arr.astype(np.float32)
        exp32 = exp_arr.astype(np.float32)
        # NaN masks
        sim_nan = np.isnan(sim32)
        exp_nan = np.isnan(exp32)
        both_nan = sim_nan & exp_nan  # 忽略
        mismatch_nan = sim_nan ^ exp_nan  # 一邊 NaN 視為失敗
        diff = sim32 - exp32
        # 只在非 NaN 對上計算 allowed 與 abs_diff
        abs_diff = np.abs(diff)
        allowed = atol + rtol * np.abs(exp32)
        # 初始 fail 判斷 (數值差異)
        fail_mask = abs_diff > allowed
        # 忽略 both_nan
        if both_nan.any():
            fail_mask = fail_mask & (~both_nan)
        # 加入 mismatch NaN 直接 fail
        if mismatch_nan.any():
            fail_mask = fail_mask | mismatch_nan
        fail_indices = np.nonzero(fail_mask)[0]
        # 統計: 只針對雙方都非 NaN 的元素 (包含 both_nan? -> both_nan 也排除以免污染誤差; diff 視為 0 不影響)
        valid_mask = ~(sim_nan | exp_nan)
        if valid_mask.any():
            diff_valid = (sim32 - exp32)[valid_mask].astype(np.float64)
            mse = float(np.mean(diff_valid ** 2))
            rms = math.sqrt(mse)
            max_abs = float(np.max(np.abs(diff_valid)))
        else:
            mse = rms = max_abs = 0.0
        return {
            'count': diff.size,
            'fail_indices': fail_indices.tolist(),
            'num_fail': int(fail_indices.size),
            'max_abs': max_abs,
            'rms': rms,
            'mse': mse,
            'sim': sim_arr,
            'exp': exp_arr,
        }
    # fallback
    num = len(sim)
    fail_indices = []
    sq = 0.0
    max_abs = 0.0
    valid_count = 0
    for i, (s, e) in enumerate(zip(sim, exp)):
        s_nan = math.isnan(s)
        e_nan = math.isnan(e)
        if s_nan and e_nan:
            # 忽略
            continue
        if s_nan ^ e_nan:  # mismatch NaN -> fail
            fail_indices.append(i)
            continue
        d = float(s) - float(e)
        ad = abs(d)
        if ad > max_abs:
            max_abs = ad
        sq += d * d
        valid_count += 1
        if ad > atol + rtol * abs(e):
            fail_indices.append(i)
    mse = (sq / valid_count) if valid_count else 0.0
    rms = math.sqrt(mse)
    return {
        'count': num,
        'fail_indices': fail_indices,
        'num_fail': len(fail_indices),
        'max_abs': max_abs,
        'rms': rms,
        'mse': mse,
        'sim': sim,
        'exp': exp,
    }


# CSV 欄位定義 (沿用)
_def_csv_header = [
    'index',
    'sim_hex','sim_sign','sim_exp','sim_mantissa','sim_float',
    'expected_hex','expected_sign','expected_exp','expected_mantissa','expected_float',
    'abs_diff','mismatch'
]

def dump_csv(sim_seq, exp_seq, result, path: str):
    if not _HAVE_PANDAS:
        raise RuntimeError('需要 pandas 以輸出 CSV，請先安裝: pip install pandas')
    sim_u16, sim_sign, sim_exp, sim_mant = _fp16_components(sim_seq)
    exp_u16, exp_sign, exp_exp, exp_mant = _fp16_components(exp_seq)
    fail_set = set(result['fail_indices'])
    total = result['count']
    data = {col: [] for col in _def_csv_header}
    for i in range(total):
        s_val = float(sim_seq[i])
        e_val = float(exp_seq[i])
        abs_diff = abs(s_val - e_val)
        data['index'].append(i)
        data['sim_hex'].append(f"0x{int(sim_u16[i]):04x}")
        data['sim_sign'].append(int(sim_sign[i]))
        data['sim_exp'].append(int(sim_exp[i]))
        data['sim_mantissa'].append(int(sim_mant[i]))
        data['sim_float'].append(s_val)
        data['expected_hex'].append(f"0x{int(exp_u16[i]):04x}")
        data['expected_sign'].append(int(exp_sign[i]))
        data['expected_exp'].append(int(exp_exp[i]))
        data['expected_mantissa'].append(int(exp_mant[i]))
        data['expected_float'].append(e_val)
        data['abs_diff'].append(abs_diff)
        data['mismatch'].append(1 if i in fail_set else 0)
    df = pd.DataFrame(data)
    df.to_csv(path, index=False)


def main(argv: Sequence[str]) -> int:
    p = argparse.ArgumentParser(description='Compare two fp16 binary files.')
    p.add_argument('--sim', required=True, help='模擬輸出檔案 (.bin)')
    p.add_argument('--expected', required=True, help='期望/黃金輸出檔案 (.bin)')
    p.add_argument('--rtol', type=float, default=1e-2, help='相對誤差容忍 (default 1e-2)')
    p.add_argument('--atol', type=float, default=1e-3, help='絕對誤差容忍 (default 1e-3)')
    p.add_argument('--show', type=int, default=10, help='顯示前 N 個 mismatch (default 10)')
    p.add_argument('--quiet', action='store_true', help='僅用退出碼表示結果')
    p.add_argument('--dump-csv', dest='dump_csv', help='(需要 pandas) 輸出詳細 CSV: hex, sign, exp, mantissa, float, diff')
    args = p.parse_args(argv)

    if not os.path.isfile(args.sim):
        print(f"找不到模擬檔案: {args.sim}", file=sys.stderr)
        return 2
    if not os.path.isfile(args.expected):
        print(f"找不到期望檔案: {args.expected}", file=sys.stderr)
        return 2

    try:
        sim = _read_fp16_file(args.sim)
        exp = _read_fp16_file(args.expected)
        result = compare(sim, exp, args.rtol, args.atol)
    except Exception as e:
        print(f"讀取/比較失敗: {e}", file=sys.stderr)
        return 2

    # 若需要輸出 CSV
    if args.dump_csv:
        try:
            dump_csv(result['sim'], result['exp'], result, args.dump_csv)
            if not args.quiet:
                print(f"CSV 已輸出: {args.dump_csv}")
        except Exception as e:
            print(f"輸出 CSV 失敗: {e}", file=sys.stderr)
            return 2

    num_fail = result['num_fail']
    count = result['count']

    if not args.quiet:
        print(f"資料筆數: {count}")
        print(f"允許誤差: rtol={args.rtol} atol={args.atol}")
        print(f"不吻合數: {num_fail}")
        print(f"最大絕對誤差: {result['max_abs']:.6g}")
        print(f"RMSE: {result['rms']:.6g}  MSE: {result['mse']:.6g}")

    if num_fail and args.show > 0:
        # 取得原始 16-bit pattern 以利除錯
        sim_u16 = _to_uint16_sequence(result['sim'])
        exp_u16 = _to_uint16_sequence(result['exp'])
        # 計算差異並排序 (由大到小)
        diffs = [
            (idx, abs(float(result['sim'][idx]) - float(result['exp'][idx])))
            for idx in result['fail_indices']
        ]
        diffs.sort(key=lambda x: x[1], reverse=True)
        print('前幾個差異 (由大到小): (index sim(exp16) expected(exp16) abs_diff)')
        for idx, ad in diffs[: args.show]:
            s = float(result['sim'][idx])
            e = float(result['exp'][idx])
            print(f"{idx} {s:.6g}(0x{sim_u16[idx]:04x}) {e:.6g}(0x{exp_u16[idx]:04x}) {ad:.6g}")

    if num_fail == 0:
        if not args.quiet:
            print('結果: PASS')
        return 0
    else:
        if not args.quiet:
            print('結果: FAIL')
        return 1


if __name__ == '__main__':  # pragma: no cover
    sys.exit(main(sys.argv[1:]))
