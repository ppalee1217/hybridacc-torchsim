#!/usr/bin/env python3
import ast
import json
import struct
import math
import sys
import os
import argparse

import numpy as np

def fp16_to_float(h):
    sign = (h >> 15) & 1
    exp = (h >> 10) & 0x1F
    frac = h & 0x3FF
    if exp == 0:
        return (-1)**sign * 2**(-14) * (frac / 1024.0)
    if exp == 31:
        return float('inf') if frac == 0 else float('nan')
    return (-1)**sign * 2**(exp - 15) * (1 + frac / 1024.0)


def _load_final_layer_info(dir_path):
    ir_path = os.path.join(dir_path, 'hardware_ir.json')
    if not os.path.isfile(ir_path):
        return None
    with open(ir_path, 'r') as f:
        ir = json.load(f)
    layers = ir.get('layers') or []
    return layers[-1] if layers else None


def _extract_sparse_region(records, base_addr, length_bytes):
    region = bytearray(length_bytes)
    for addr, length, data in records:
        overlap_start = max(base_addr, addr)
        overlap_end = min(base_addr + length_bytes, addr + length)
        if overlap_start < overlap_end:
            src_offset = overlap_start - addr
            dst_offset = overlap_start - base_addr
            copy_len = overlap_end - overlap_start
            region[dst_offset:dst_offset + copy_len] = data[src_offset:src_offset + copy_len]
    return bytes(region)


def _unpack_gemm_single_wave(packed_bytes, meta, layer_info):
    output_shape = ast.literal_eval(meta['output_shape'])
    rows, cols = int(output_shape[0]), int(output_shape[1])

    pe_params = layer_info['pe_program']['params']
    agu_plo = layer_info['agu_plo']
    grid_n = int(pe_params['GRID_N_PER_WAVE'])
    pe_n = int(agu_plo['iter1'])
    row_words = int(agu_plo['iter0'])
    rows_per_word = 4
    block_words = pe_n * row_words
    block_bytes = block_words * 8

    active_blocks = sorted(
        (entry for entry in layer_info['scan_chain'] if entry.get('enable') and entry.get('plo_id', 63) != 63),
        key=lambda entry: entry['plo_id'],
    )

    actual = np.zeros((rows, cols), dtype=np.float16)
    for block_index, entry in enumerate(active_blocks):
        start = block_index * block_bytes
        end = start + block_bytes
        block = np.frombuffer(packed_bytes[start:end], dtype=np.float16).reshape(pe_n, row_words, rows_per_word)
        plo_id = int(entry['plo_id'])
        m_idx = plo_id // grid_n
        n_idx = plo_id % grid_n
        for local_n in range(pe_n):
            global_n = n_idx * pe_n + local_n
            if global_n >= cols:
                continue
            for row_word in range(row_words):
                row_base = m_idx * row_words * rows_per_word + row_word * rows_per_word
                for lane in range(rows_per_word):
                    global_m = row_base + lane
                    if global_m < rows:
                        actual[global_m, global_n] = block[local_n, row_word, lane]

    return actual.tobytes()

def main():
    parser = argparse.ArgumentParser(description="Compare HybridAcc DRAM output with golden reference")
    parser.add_argument("dir", help="Directory containing dram_init.bin.out, golden_output.bin, and golden_meta.txt")
    parser.add_argument("--tolerance", type=float, default=0.99, help="Cosine similarity threshold for PASS/FAIL (default 0.99)")
    args = parser.parse_args()

    meta_path = os.path.join(args.dir, 'golden_meta.txt')
    golden_path = os.path.join(args.dir, 'golden_output.bin')
    dram_path = os.path.join(args.dir, 'dram_init.bin.out')

    if not all(os.path.isfile(p) for p in [meta_path, golden_path, dram_path]):
        print(f"Error: Missing required files in {args.dir}")
        sys.exit(1)

    # Read golden meta
    meta = {}
    with open(meta_path, 'r') as f:
        for line in f:
            if '=' in line:
                k, v = line.strip().split('=', 1)
                meta[k] = v

    final_layer = _load_final_layer_info(args.dir)

    dram_output_base = int(meta['dram_output_base'], 16)
    golden_bytes = int(meta['golden_output_bytes'])

    # Read golden
    with open(golden_path, 'rb') as f:
        golden = f.read()

    if len(golden) != golden_bytes:
        print(f"Warning: golden_output.bin size ({len(golden)}) does not match meta ({golden_bytes})")
        golden_bytes = len(golden)

    # Read sparse DRAM dump
    with open(dram_path, 'rb') as f:
        # Check sparse format magic
        magic_bytes = f.read(4)
        if len(magic_bytes) < 4:
            print("Error: dram_init.bin.out is too short")
            sys.exit(1)

        magic = struct.unpack('<I', magic_bytes)[0]
        if magic != 0x53505253:  # 'SPRS' little-endian
            print(f"Error: dram_init.bin.out is not in sparse format (magic=0x{magic:08x})")
            # Try to read as flat format as a fallback
            f.seek(0)
            records = [(meta.get('dram_base', 0x80000000), os.path.getsize(dram_path), f.read())]
        else:
            f.read(4)  # Skip version

            records = []
            while True:
                addr_data = f.read(4)
                if not addr_data: break
                addr = struct.unpack('<I', addr_data)[0]
                length = struct.unpack('<I', f.read(4))[0]
                if addr == 0 and length == 0:
                    break  # Sentinel
                data = f.read(length)
                records.append((addr, length, data))

    if (
        final_layer is not None
        and final_layer.get('op_type') == 'gemm'
        and final_layer.get('tiling', {}).get('total_waves') == 1
        and 'output_shape' in meta
    ):
        packed_bytes = _extract_sparse_region(
            records,
            dram_output_base,
            int(final_layer['tiling_params']['dma_plo_words']) * 8,
        )
        actual = _unpack_gemm_single_wave(packed_bytes, meta, final_layer)
    else:
        actual = _extract_sparse_region(records, dram_output_base, golden_bytes)

    # Compute metrics
    dot = 0.0
    norm_g = 0.0
    norm_a = 0.0
    mse_sum = 0.0
    max_diff = 0.0
    exact = 0

    total = golden_bytes // 2
    for i in range(total):
        g_raw = struct.unpack('<H', golden[i*2:i*2+2])[0]
        a_raw = struct.unpack('<H', actual[i*2:i*2+2])[0]
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

    cosine_sim = dot / (math.sqrt(norm_g) * math.sqrt(norm_a)) if norm_g > 0 and norm_a > 0 else 0.0
    mse = mse_sum / total if total > 0 else 0.0

    print("=== HybridAcc Output Verification ===")
    if 'output_shape' in meta:
        print(f"Output shape: {meta['output_shape']} = {total} fp16 words")
    print(f"Cosine similarity: {cosine_sim:.8f}")
    print(f"MSE: {mse:.10e}")
    print(f"Max absolute diff: {max_diff:.6f}")
    print(f"Exact matches: {exact}/{total} ({100*exact/total:.1f}%)")

    passed = cosine_sim >= args.tolerance
    print(f"RESULT: {'PASS' if passed else 'FAIL'} (threshold: cosine_sim >= {args.tolerance})")

    sys.exit(0 if passed else 1)

if __name__ == '__main__':
    main()
