#!/usr/bin/env python3
"""Verify Layer 0 output from a multi-layer e2e simulation.

Reads dram_init.bin.out, extracts layer 0 output region, computes golden
reference from the original test data, and reports cosine similarity.

Usage:
    python scripts/verify_layer0.py output/e2e_2layer
"""
import struct
import math
import sys
import os
import json

import numpy as np
import yaml


def fp16_conv2d_golden(inp, weight, stride=1, padding=0):
    N, H, W, IC = inp.shape
    OC, KH, KW, IC2 = weight.shape
    OH = (H + 2 * padding - KH) // stride + 1
    OW = (W + 2 * padding - KW) // stride + 1
    out = np.zeros((N, OH, OW, OC), dtype=np.float16)
    inp32 = inp.astype(np.float32)
    w32 = weight.astype(np.float32)
    for n in range(N):
        for oh in range(OH):
            for ow in range(OW):
                for oc in range(OC):
                    acc = np.float32(0.0)
                    for kh in range(KH):
                        for kw in range(KW):
                            ih = oh * stride - padding + kh
                            iw = ow * stride - padding + kw
                            if 0 <= ih < H and 0 <= iw < W:
                                for ic in range(IC):
                                    acc += inp32[n, ih, iw, ic] * w32[oc, kh, kw, ic]
                    out[n, oh, ow, oc] = np.float16(acc)
    return out


def fp16_to_float(h):
    sign = (h >> 15) & 1
    exp = (h >> 10) & 0x1F
    frac = h & 0x3FF
    if exp == 0:
        return (-1)**sign * 2**(-14) * (frac / 1024.0)
    if exp == 31:
        return float('inf') if frac == 0 else float('nan')
    return (-1)**sign * 2**(exp - 15) * (1 + frac / 1024.0)


def read_sparse_dram(path):
    """Read sparse DRAM dump and return list of (addr, data) tuples."""
    records = []
    with open(path, 'rb') as f:
        magic = struct.unpack('<I', f.read(4))[0]
        if magic != 0x53505253:
            raise ValueError(f"Not a sparse DRAM: magic=0x{magic:08x}")
        f.read(4)  # version
        while True:
            addr_data = f.read(4)
            if not addr_data:
                break
            addr = struct.unpack('<I', addr_data)[0]
            length = struct.unpack('<I', f.read(4))[0]
            if addr == 0 and length == 0:
                break
            data = f.read(length)
            records.append((addr, length, data))
    return records


def extract_region(records, base, size):
    """Extract a byte region from sparse DRAM records."""
    result = bytearray(size)
    for addr, length, data in records:
        overlap_start = max(base, addr)
        overlap_end = min(base + size, addr + length)
        if overlap_start < overlap_end:
            src_offset = overlap_start - addr
            dst_offset = overlap_start - base
            copy_len = overlap_end - overlap_start
            result[dst_offset:dst_offset + copy_len] = data[src_offset:src_offset + copy_len]
    return bytes(result)


def cosine_similarity(golden_bytes, actual_bytes):
    """Compute cosine similarity between two fp16 byte arrays."""
    total = len(golden_bytes) // 2
    dot = norm_g = norm_a = mse_sum = 0.0
    max_diff = 0.0
    exact = 0
    nonzero_a = 0

    for i in range(total):
        g_raw = struct.unpack('<H', golden_bytes[i*2:i*2+2])[0]
        a_raw = struct.unpack('<H', actual_bytes[i*2:i*2+2])[0]
        g = fp16_to_float(g_raw)
        a = fp16_to_float(a_raw)

        dot += g * a
        norm_g += g * g
        norm_a += a * a
        diff = abs(g - a)
        mse_sum += diff * diff
        max_diff = max(max_diff, diff)
        if g_raw == a_raw:
            exact += 1
        if a_raw != 0:
            nonzero_a += 1

    denom = math.sqrt(norm_g) * math.sqrt(norm_a)
    cosine_sim = dot / denom if denom > 0 else 0.0
    mse = mse_sum / total if total > 0 else 0.0

    return {
        'cosine_sim': cosine_sim,
        'mse': mse,
        'max_diff': max_diff,
        'exact': exact,
        'total': total,
        'nonzero_actual': nonzero_a,
    }


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <e2e_dir> [--workload <yaml>]")
        sys.exit(1)

    e2e_dir = sys.argv[1]
    workload_path = None
    if '--workload' in sys.argv:
        idx = sys.argv.index('--workload')
        workload_path = sys.argv[idx + 1]

    # Read meta
    meta = {}
    with open(os.path.join(e2e_dir, 'golden_meta.txt')) as f:
        for line in f:
            if '=' in line:
                k, v = line.strip().split('=', 1)
                meta[k] = v

    dram_base = int(meta['dram_base'], 16)
    seed = int(meta.get('seed', '42'))

    # Read hardware IR
    ir_path = os.path.join(e2e_dir, 'hardware_ir.json')
    with open(ir_path) as f:
        ir = json.load(f)

    layers = ir['layers']
    layer0_tp = layers[0]['tiling_params']
    layer0_output_base = layer0_tp['dram_output_base']
    layer0_input_base = layer0_tp['dram_input_base']
    layer0_weight_base = layer0_tp['dram_weight_base']

    # Find workload yaml
    if workload_path is None:
        wl_path = os.path.join(e2e_dir, 'workload_ir.json')
        if os.path.exists(wl_path):
            with open(wl_path) as f:
                wl = json.load(f)
        else:
            # Try to find it from the standard location
            candidates = [
                'design/hybridacc-cc/example/test/test_conv3x3_2layer.yaml',
            ]
            for c in candidates:
                full = os.path.join(os.path.dirname(os.path.dirname(e2e_dir)), c)
                if os.path.exists(full):
                    with open(full) as f:
                        wl = yaml.safe_load(f)
                    break
            else:
                print("Error: Cannot find workload YAML")
                sys.exit(1)
    else:
        with open(workload_path) as f:
            wl = yaml.safe_load(f)

    wl_tensors = wl['tensors']
    ops = wl['ops']

    # Regenerate test data with same seed
    rng = np.random.RandomState(seed)
    tensors_data = {}
    output_tensor_names = {name for op in ops for name in op['outputs']}

    for name, tdef in wl_tensors.items():
        if name not in output_tensor_names:
            shape = tuple(tdef['shape'])
            tensors_data[name] = rng.uniform(-1.0, 1.0, shape).astype(np.float16)

    # Compute layer 0 golden
    op0 = ops[0]
    stride = op0.get('attrs', {}).get('stride', 1)
    padding = op0.get('attrs', {}).get('padding', 0)
    inp_name = op0['inputs'][0]
    weight_name = op0['inputs'][1]
    out_name = op0['outputs'][0]

    golden_l0 = fp16_conv2d_golden(tensors_data[inp_name], tensors_data[weight_name],
                                    stride=stride, padding=padding)
    print(f"Layer 0 golden: shape={golden_l0.shape}, range=[{golden_l0.min():.4f}, {golden_l0.max():.4f}]")

    # Convert golden to tile-packed layout (matching firmware writeback)
    num_oc = layer0_tp.get('num_oc_tiles', 1)
    num_h = layer0_tp.get('num_h_tiles', 1)
    num_w = layer0_tp.get('num_w_tiles', 1)
    N, OH, OW, OC = golden_l0.shape

    if num_oc > 1 or num_h > 1 or num_w > 1:
        tile_oc = OC // num_oc
        tile_h = OH // num_h
        tile_w = OW // num_w
        g = golden_l0.reshape(N, num_h, tile_h, num_w, tile_w, num_oc, tile_oc)
        g = g.transpose(0, 5, 1, 3, 2, 4, 6)
        golden_l0_bytes = np.ascontiguousarray(g).astype(np.float16).tobytes()
    else:
        golden_l0_bytes = golden_l0.astype(np.float16).tobytes()

    output_size = len(golden_l0_bytes)
    print(f"Layer 0 output region: 0x{layer0_output_base:08X}, size={output_size} bytes")

    # Read DRAM output
    dram_out_path = os.path.join(e2e_dir, 'dram_init.bin.out')
    records = read_sparse_dram(dram_out_path)
    actual_bytes = extract_region(records, layer0_output_base, output_size)

    # Compare
    stats = cosine_similarity(golden_l0_bytes, actual_bytes)

    print(f"\n=== Layer 0 Output Verification ===")
    print(f"Output shape: {list(golden_l0.shape)} = {stats['total']} fp16 words")
    print(f"Cosine similarity: {stats['cosine_sim']:.8f}")
    print(f"MSE: {stats['mse']:.10e}")
    print(f"Max absolute diff: {stats['max_diff']:.6f}")
    print(f"Exact matches: {stats['exact']}/{stats['total']} ({100*stats['exact']/stats['total']:.1f}%)")
    print(f"Nonzero actual: {stats['nonzero_actual']}/{stats['total']} ({100*stats['nonzero_actual']/stats['total']:.1f}%)")

    passed = stats['cosine_sim'] >= 0.99
    print(f"RESULT: {'PASS' if passed else 'FAIL'} (threshold: cosine_sim >= 0.99)")

    # Also check layer 1 if available
    if len(layers) > 1:
        layer1_tp = layers[1]['tiling_params']
        layer1_output_base = layer1_tp['dram_output_base']

        # Compute layer 1 golden
        tensors_data[out_name] = golden_l0  # feed layer 0 output to layer 1
        op1 = ops[1]
        stride1 = op1.get('attrs', {}).get('stride', 1)
        padding1 = op1.get('attrs', {}).get('padding', 0)
        inp1_name = op1['inputs'][0]
        weight1_name = op1['inputs'][1]
        out1_name = op1['outputs'][0]

        golden_l1 = fp16_conv2d_golden(tensors_data[inp1_name], tensors_data[weight1_name],
                                        stride=stride1, padding=padding1)
        print(f"\nLayer 1 golden: shape={golden_l1.shape}, range=[{golden_l1.min():.4f}, {golden_l1.max():.4f}]")

        num_oc1 = layer1_tp.get('num_oc_tiles', 1)
        num_h1 = layer1_tp.get('num_h_tiles', 1)
        num_w1 = layer1_tp.get('num_w_tiles', 1)
        N1, OH1, OW1, OC1 = golden_l1.shape
        if num_oc1 > 1 or num_h1 > 1 or num_w1 > 1:
            tile_oc1 = OC1 // num_oc1
            tile_h1 = OH1 // num_h1
            tile_w1 = OW1 // num_w1
            g1 = golden_l1.reshape(N1, num_h1, tile_h1, num_w1, tile_w1, num_oc1, tile_oc1)
            g1 = g1.transpose(0, 5, 1, 3, 2, 4, 6)
            golden_l1_bytes = np.ascontiguousarray(g1).astype(np.float16).tobytes()
        else:
            golden_l1_bytes = golden_l1.astype(np.float16).tobytes()

        output1_size = len(golden_l1_bytes)
        actual1_bytes = extract_region(records, layer1_output_base, output1_size)

        stats1 = cosine_similarity(golden_l1_bytes, actual1_bytes)
        print(f"\n=== Layer 1 Output Verification ===")
        print(f"Output shape: {list(golden_l1.shape)} = {stats1['total']} fp16 words")
        print(f"Cosine similarity: {stats1['cosine_sim']:.8f}")
        print(f"MSE: {stats1['mse']:.10e}")
        print(f"Max absolute diff: {stats1['max_diff']:.6f}")
        print(f"Exact matches: {stats1['exact']}/{stats1['total']} ({100*stats1['exact']/stats1['total']:.1f}%)")
        print(f"Nonzero actual: {stats1['nonzero_actual']}/{stats1['total']} ({100*stats1['nonzero_actual']/stats1['total']:.1f}%)")

        passed1 = stats1['cosine_sim'] >= 0.99
        print(f"RESULT: {'PASS' if passed1 else 'FAIL'} (threshold: cosine_sim >= 0.99)")


if __name__ == '__main__':
    main()
