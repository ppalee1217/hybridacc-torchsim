#!/usr/bin/env python3
"""Compare HybridAcc DRAM output with golden reference.

Reads sparse DRAM dump from ESL simulation and compares the output region
against the pre-computed golden output using cosine similarity.

Usage:
    python -m hybridacc_verify.check.compare_golden <dir> [--tolerance 0.99]

The directory must contain:
  - dram_init.bin.out   : sparse DRAM dump from hybridacc-sim
  - golden_output.bin   : expected output from gen_test_dram
  - golden_meta.txt     : metadata (dram_output_base, golden_output_bytes, ...)
"""
import struct
import math
import sys
import os
import argparse


def fp16_to_float(h):
    """Convert a raw 16-bit half-precision value to Python float."""
    sign = (h >> 15) & 1
    exp = (h >> 10) & 0x1F
    frac = h & 0x3FF
    if exp == 0:
        return (-1)**sign * 2**(-14) * (frac / 1024.0)
    if exp == 31:
        return float('inf') if frac == 0 else float('nan')
    return (-1)**sign * 2**(exp - 15) * (1 + frac / 1024.0)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare HybridAcc DRAM output with golden reference"
    )
    parser.add_argument("dir",
                        help="Directory containing dram_init.bin.out, "
                             "golden_output.bin, and golden_meta.txt")
    parser.add_argument("--tolerance", type=float, default=0.99,
                        help="Cosine similarity threshold for PASS/FAIL "
                             "(default 0.99)")
    args = parser.parse_args(argv)

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

    dram_output_base = int(meta['dram_output_base'], 16)
    golden_bytes = int(meta['golden_output_bytes'])

    # Read golden
    with open(golden_path, 'rb') as f:
        golden = f.read()

    if len(golden) != golden_bytes:
        print(f"Warning: golden_output.bin size ({len(golden)}) "
              f"does not match meta ({golden_bytes})")
        golden_bytes = len(golden)

    # Read sparse DRAM dump
    with open(dram_path, 'rb') as f:
        magic_bytes = f.read(4)
        if len(magic_bytes) < 4:
            print("Error: dram_init.bin.out is too short")
            sys.exit(1)

        magic = struct.unpack('<I', magic_bytes)[0]
        if magic != 0x53505253:  # 'SPRS' little-endian
            print(f"Error: dram_init.bin.out is not in sparse format "
                  f"(magic=0x{magic:08x})")
            f.seek(0)
            records = [(meta.get('dram_base', 0x80000000),
                        os.path.getsize(dram_path), f.read())]
        else:
            f.read(4)  # Skip version
            records = []
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

    # Extract actual output bytes
    actual = bytearray(golden_bytes)
    for addr, length, data in records:
        overlap_start = max(dram_output_base, addr)
        overlap_end = min(dram_output_base + golden_bytes, addr + length)
        if overlap_start < overlap_end:
            src_offset = overlap_start - addr
            dst_offset = overlap_start - dram_output_base
            copy_len = overlap_end - overlap_start
            actual[dst_offset:dst_offset + copy_len] = \
                data[src_offset:src_offset + copy_len]

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

    denom = math.sqrt(norm_g) * math.sqrt(norm_a)
    cosine_sim = dot / denom if denom > 0 else 0.0
    mse = mse_sum / total if total > 0 else 0.0

    print("=== HybridAcc Output Verification ===")
    if 'output_shape' in meta:
        print(f"Output shape: {meta['output_shape']} = {total} fp16 words")
    if 'op_types' in meta:
        print(f"Op types: {meta['op_types']}")
    if 'num_layers' in meta:
        print(f"Num layers: {meta['num_layers']}")
    print(f"Cosine similarity: {cosine_sim:.8f}")
    print(f"MSE: {mse:.10e}")
    print(f"Max absolute diff: {max_diff:.6f}")
    print(f"Exact matches: {exact}/{total} ({100*exact/total:.1f}%)")

    passed = cosine_sim >= args.tolerance
    print(f"RESULT: {'PASS' if passed else 'FAIL'} "
          f"(threshold: cosine_sim >= {args.tolerance})")

    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
