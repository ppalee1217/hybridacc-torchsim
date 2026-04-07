#!/usr/bin/env python3
"""Golden data generator for HybridAcc core-level conv2d_3x3 test.

Generates input/weight tensors and golden conv2d output in fp16,
writes them as binary files matching DRAM layout expected by firmware.

Usage:
    python -m hybridacc_verify.gen.core_conv2d_golden \\
        --output-dir /path/to/output \\
        [--seed 42]
"""
import argparse
import os
import struct
import numpy as np


# ── Conv2d parameters matching conv2d_3x3_example.yaml ──
INPUT_SHAPE = (1, 14, 14, 16)   # NHWC
WEIGHT_SHAPE = (16, 3, 3, 16)   # OC, KH, KW, IC
OUTPUT_SHAPE = (1, 12, 12, 16)  # NHWC
STRIDE = 1
PADDING = 0

# ── DRAM layout matching firmware_data.c ──
DRAM_BASE = 0x80000000
DRAM_WEIGHT_BASE = 0x80000000
DRAM_INPUT_BASE = 0x80001200
DRAM_OUTPUT_BASE = 0x80002A80

# Total DRAM image size (covers all tensors)
DRAM_IMAGE_SIZE = DRAM_OUTPUT_BASE + OUTPUT_SHAPE[1] * OUTPUT_SHAPE[2] * OUTPUT_SHAPE[3] * 2 - DRAM_BASE


def fp16_conv2d_golden(inp: np.ndarray, weight: np.ndarray) -> np.ndarray:
    """Compute conv2d in fp16 arithmetic (no padding, stride=1).

    inp:    [N, H, W, IC]  fp16
    weight: [OC, KH, KW, IC] fp16
    output: [N, OH, OW, OC] fp16
    """
    N, H, W, IC = inp.shape
    OC, KH, KW, IC2 = weight.shape
    assert IC == IC2, f"IC mismatch: input {IC} vs weight {IC2}"

    OH = H - KH + 1
    OW = W - KW + 1
    out = np.zeros((N, OH, OW, OC), dtype=np.float16)

    # Compute in fp32 then cast to fp16 for golden comparison
    inp32 = inp.astype(np.float32)
    w32 = weight.astype(np.float32)

    for n in range(N):
        for oh in range(OH):
            for ow in range(OW):
                for oc in range(OC):
                    acc = np.float32(0.0)
                    for kh in range(KH):
                        for kw in range(KW):
                            for ic in range(IC):
                                acc += inp32[n, oh + kh, ow + kw, ic] * w32[oc, kh, kw, ic]
                    out[n, oh, ow, oc] = np.float16(acc)

    return out


def generate_golden(seed: int = 42) -> dict:
    """Generate random tensors and compute golden output."""
    rng = np.random.RandomState(seed)

    # Generate small fp16 values to avoid overflow
    inp = rng.uniform(-1.0, 1.0, INPUT_SHAPE).astype(np.float16)
    weight = rng.uniform(-0.5, 0.5, WEIGHT_SHAPE).astype(np.float16)

    golden_output = fp16_conv2d_golden(inp, weight)

    return {
        'input': inp,
        'weight': weight,
        'golden_output': golden_output,
    }


def write_dram_image(output_dir: str, data: dict) -> str:
    """Write DRAM binary image with weight, input, and zeroed output region."""
    os.makedirs(output_dir, exist_ok=True)

    # Allocate DRAM image
    dram_size = DRAM_IMAGE_SIZE + 4096  # extra padding
    dram = bytearray(dram_size)

    # Write weight at offset 0
    weight_offset = DRAM_WEIGHT_BASE - DRAM_BASE
    weight_bytes = data['weight'].tobytes()
    dram[weight_offset:weight_offset + len(weight_bytes)] = weight_bytes

    # Write input
    input_offset = DRAM_INPUT_BASE - DRAM_BASE
    input_bytes = data['input'].tobytes()
    dram[input_offset:input_offset + len(input_bytes)] = input_bytes

    # Output region is zeroed (will be filled by hardware)

    # Write files
    dram_path = os.path.join(output_dir, 'dram_init.bin')
    with open(dram_path, 'wb') as f:
        f.write(dram)

    golden_path = os.path.join(output_dir, 'golden_output.bin')
    with open(golden_path, 'wb') as f:
        f.write(data['golden_output'].tobytes())

    # Also write individual tensors for debugging
    for name in ['input', 'weight', 'golden_output']:
        path = os.path.join(output_dir, f'{name}.bin')
        with open(path, 'wb') as f:
            f.write(data[name].tobytes())

    # Write metadata
    meta_path = os.path.join(output_dir, 'golden_meta.txt')
    with open(meta_path, 'w') as f:
        f.write(f"dram_base=0x{DRAM_BASE:08X}\n")
        f.write(f"dram_weight_base=0x{DRAM_WEIGHT_BASE:08X}\n")
        f.write(f"dram_input_base=0x{DRAM_INPUT_BASE:08X}\n")
        f.write(f"dram_output_base=0x{DRAM_OUTPUT_BASE:08X}\n")
        f.write(f"weight_shape={list(WEIGHT_SHAPE)}\n")
        f.write(f"input_shape={list(INPUT_SHAPE)}\n")
        f.write(f"output_shape={list(OUTPUT_SHAPE)}\n")
        f.write(f"dram_image_bytes={dram_size}\n")
        f.write(f"golden_output_bytes={data['golden_output'].nbytes}\n")
        f.write(f"weight_bytes={data['weight'].nbytes}\n")
        f.write(f"input_bytes={data['input'].nbytes}\n")

    print(f"Generated golden data in {output_dir}")
    print(f"  Weight: {data['weight'].shape} fp16 = {data['weight'].nbytes} bytes")
    print(f"  Input:  {data['input'].shape} fp16 = {data['input'].nbytes} bytes")
    print(f"  Output: {data['golden_output'].shape} fp16 = {data['golden_output'].nbytes} bytes")
    print(f"  DRAM image: {dram_size} bytes")

    return dram_path


def main():
    parser = argparse.ArgumentParser(description='Generate conv2d golden data')
    parser.add_argument('--output-dir', required=True, help='Output directory')
    parser.add_argument('--seed', type=int, default=42, help='Random seed')
    args = parser.parse_args()

    data = generate_golden(seed=args.seed)
    write_dram_image(args.output_dir, data)


if __name__ == '__main__':
    main()
