#!/usr/bin/env python3
"""Generate DRAM mirror image + golden output for conv2d_3x3_example_test.

Reads the hardware IR from hacc-compile output to determine exact DRAM layout,
generates random fp16 test data, computes golden conv2d, and writes:
  - dram_init.bin   : DRAM mirror for hybridacc-sim --mirror
  - golden_output.bin : expected output for verification
  - golden_meta.txt   : metadata for post-sim comparison

Usage:
    python scripts/gen/gen_conv3x3_test_dram.py \
        --ir output/hacc_conv3x3_test_build/hardware_ir.json \
        --workload design/hybridacc-cc/example/conv2d_3x3_example_test.yaml \
        --output-dir output/hacc_conv3x3_test_build \
        [--seed 42]
"""
import argparse
import json
import os

import numpy as np
import yaml


def fp16_conv2d_golden(inp: np.ndarray, weight: np.ndarray,
                       stride: int = 1, padding: int = 0) -> np.ndarray:
    """Compute conv2d in fp16 arithmetic.

    inp:    [N, H, W, IC]  fp16
    weight: [OC, KH, KW, IC] fp16
    output: [N, OH, OW, OC] fp16
    """
    N, H, W, IC = inp.shape
    OC, KH, KW, IC2 = weight.shape
    assert IC == IC2

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


def main():
    parser = argparse.ArgumentParser(description="Generate DRAM mirror for conv2d test")
    parser.add_argument("--ir", required=True, help="Path to hardware_ir.json from hacc-compile")
    parser.add_argument("--workload", required=True, help="Path to workload YAML")
    parser.add_argument("--output-dir", required=True, help="Output directory")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    args = parser.parse_args()

    # -- Load workload YAML for tensor shapes --
    with open(args.workload) as f:
        wl = yaml.safe_load(f)

    tensors = wl["tensors"]
    input_shape = tuple(tensors["input"]["shape"])    # NHWC
    weight_shape = tuple(tensors["weight"]["shape"])   # OIHW (OC, KH, KW, IC)
    output_shape = tuple(tensors["output"]["shape"])   # NHWC

    op = wl["ops"][0]
    stride = op["attrs"].get("stride", 1)
    padding = op["attrs"].get("padding", 0)

    # -- Load hardware IR for DRAM layout --
    with open(args.ir) as f:
        ir = json.load(f)
    tp = ir["layers"][0]["tiling_params"]
    dram_base = wl["hardware"].get("dram_base", 0x80000000)
    dram_weight_base = tp["dram_weight_base"]
    dram_input_base = tp["dram_input_base"]
    dram_output_base = tp["dram_output_base"]

    print(f"Input shape:  {input_shape}")
    print(f"Weight shape: {weight_shape}")
    print(f"Output shape: {output_shape}")
    print(f"DRAM weight @ 0x{dram_weight_base:08X}")
    print(f"DRAM input  @ 0x{dram_input_base:08X}")
    print(f"DRAM output @ 0x{dram_output_base:08X}")

    # -- Generate random test data --
    rng = np.random.RandomState(args.seed)
    inp = rng.uniform(-1.0, 1.0, input_shape).astype(np.float16)
    weight = rng.uniform(-0.5, 0.5, weight_shape).astype(np.float16)

    # -- Compute golden output --
    golden = fp16_conv2d_golden(inp, weight, stride=stride, padding=padding)
    assert golden.shape == output_shape, f"Shape mismatch: {golden.shape} vs {output_shape}"

    print(f"Golden output shape: {golden.shape}, range: [{golden.min():.4f}, {golden.max():.4f}]")

    # -- Build DRAM image --
    weight_bytes = weight.tobytes()
    input_bytes = inp.tobytes()
    output_bytes = golden.tobytes()

    weight_offset = dram_weight_base - dram_base
    input_offset = dram_input_base - dram_base
    output_offset = dram_output_base - dram_base

    dram_end = output_offset + len(output_bytes)
    dram_size = dram_end + 4096  # padding
    dram = bytearray(dram_size)

    dram[weight_offset:weight_offset + len(weight_bytes)] = weight_bytes
    dram[input_offset:input_offset + len(input_bytes)] = input_bytes
    # Output region left zeroed — firmware will fill it via DMA writeback

    # -- Write files --
    os.makedirs(args.output_dir, exist_ok=True)

    dram_path = os.path.join(args.output_dir, "dram_init.bin")
    with open(dram_path, "wb") as f:
        f.write(dram)

    golden_path = os.path.join(args.output_dir, "golden_output.bin")
    with open(golden_path, "wb") as f:
        f.write(output_bytes)

    meta_path = os.path.join(args.output_dir, "golden_meta.txt")
    with open(meta_path, "w") as f:
        f.write(f"dram_base=0x{dram_base:08X}\n")
        f.write(f"dram_weight_base=0x{dram_weight_base:08X}\n")
        f.write(f"dram_input_base=0x{dram_input_base:08X}\n")
        f.write(f"dram_output_base=0x{dram_output_base:08X}\n")
        f.write(f"input_shape={list(input_shape)}\n")
        f.write(f"weight_shape={list(weight_shape)}\n")
        f.write(f"output_shape={list(output_shape)}\n")
        f.write(f"dram_image_bytes={dram_size}\n")
        f.write(f"golden_output_bytes={len(output_bytes)}\n")
        f.write(f"output_dram_offset={output_offset}\n")
        f.write(f"seed={args.seed}\n")

    print(f"\nGenerated:")
    print(f"  DRAM mirror:    {dram_path} ({dram_size} bytes)")
    print(f"  Golden output:  {golden_path} ({len(output_bytes)} bytes)")
    print(f"  Metadata:       {meta_path}")


if __name__ == "__main__":
    main()
