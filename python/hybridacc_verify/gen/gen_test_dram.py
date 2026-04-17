#!/usr/bin/env python3
"""Generate DRAM mirror image + golden output for HybridAcc ESL simulation.

Supports conv2d_3x3, conv2d_1x1, gemm, and multi-layer workloads.

Reads the hardware IR from hacc-compile output to determine exact DRAM layout,
generates random fp16 test data, computes golden output, and writes:
  - dram_init.bin     : DRAM mirror for hybridacc-sim --mirror
  - golden_output.bin : expected output (final layer) for verification
  - golden_meta.txt   : metadata for post-sim comparison

Usage:
    python -m hybridacc_verify.gen.gen_test_dram \\
        --ir output/build/hardware_ir.json \\
        --workload design/hybridacc-cc/example/conv2d_3x3_example_test.yaml \\
        --output-dir output/build \\
        [--seed 42]
"""
import argparse
import json
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F
import yaml


# ---------------------------------------------------------------------------
# Golden computations (fp16 arithmetic via fp32 accumulation → fp16 cast)
# ---------------------------------------------------------------------------

def fp16_conv2d_golden(inp: np.ndarray, weight: np.ndarray,
                       stride: int = 1, padding: int = 0) -> np.ndarray:
    """Compute conv2d golden in fp16.

    inp:    [N, H, W, IC]  fp16
    weight: [OC, KH, KW, IC] or [OC, IC] fp16
    output: [N, OH, OW, OC] fp16
    """
    if weight.ndim == 2:
        weight = weight[:, None, None, :]

    N, H, W, IC = inp.shape
    OC, KH, KW, IC2 = weight.shape
    assert IC == IC2, f"IC mismatch: input {IC} vs weight {IC2}"

    input_nchw = torch.from_numpy(inp.astype(np.float32, copy=False)).permute(0, 3, 1, 2).contiguous()
    weight_oihw = torch.from_numpy(weight.astype(np.float32, copy=False)).permute(0, 3, 1, 2).contiguous()

    with torch.no_grad():
        out = F.conv2d(input_nchw, weight_oihw, stride=stride, padding=padding)

    return out.permute(0, 2, 3, 1).contiguous().numpy().astype(np.float16, copy=False)


def fp16_gemm_golden(A: np.ndarray, B: np.ndarray) -> np.ndarray:
    """Compute C = A @ B in fp16.

    A: [M, K] fp16
    B: [K, N] fp16
    C: [M, N] fp16
    """
    M, K = A.shape
    K2, N = B.shape
    assert K == K2, f"K mismatch: A {K} vs B {K2}"

    a32 = torch.from_numpy(A.astype(np.float32, copy=False))
    b32 = torch.from_numpy(B.astype(np.float32, copy=False))

    with torch.no_grad():
        out = torch.matmul(a32, b32)

    return out.numpy().astype(np.float16, copy=False)


# ---------------------------------------------------------------------------
# Layer-level golden computation
# ---------------------------------------------------------------------------

def _compute_layer_golden(op, tensors_data, wl_tensors, rng, layer_tp):
    """Compute golden output for a single layer.

    Args:
        op: Operation dict from workload YAML
        tensors_data: dict mapping tensor name → np.ndarray (fp16)
        wl_tensors: tensor definitions from workload YAML
        rng: numpy RandomState
        layer_tp: tiling_params from hardware_ir for this layer

    Returns:
        output tensor name, output np.ndarray (fp16)
    """
    op_type = op["type"]
    stride = op.get("attrs", {}).get("stride", 1)
    padding = op.get("attrs", {}).get("padding", 0)

    if op_type in ("conv2d_3x3", "conv2d_1x1"):
        input_name = op["inputs"][0]
        weight_name = op["inputs"][1]
        output_name = op["outputs"][0]

        inp = tensors_data[input_name]
        weight = tensors_data[weight_name]
        output_shape = tuple(wl_tensors[output_name]["shape"])

        golden = fp16_conv2d_golden(inp, weight, stride=stride, padding=padding)
        assert golden.shape == output_shape, \
            f"Shape mismatch for {output_name}: golden {golden.shape} vs expected {output_shape}"
        return output_name, golden

    elif op_type == "gemm":
        a_name = op["inputs"][0]
        b_name = op["inputs"][1]
        output_name = op["outputs"][0]

        A = tensors_data[a_name]
        B = tensors_data[b_name]
        output_shape = tuple(wl_tensors[output_name]["shape"])

        golden = fp16_gemm_golden(A, B)
        assert golden.shape == output_shape, \
            f"Shape mismatch for {output_name}: golden {golden.shape} vs expected {output_shape}"
        return output_name, golden

    else:
        raise ValueError(f"Unsupported op type: {op_type}")


# ---------------------------------------------------------------------------
# DRAM image building
# ---------------------------------------------------------------------------

def _get_tensor_bytes(arr: np.ndarray) -> bytes:
    """Convert fp16 ndarray to raw bytes."""
    return arr.astype(np.float16).tobytes()


def _tile_weight_ic(weight: np.ndarray, num_ic_tiles: int, tile_ic: int,
                    num_oc_tiles: int) -> bytes:
    """Rearrange weight [OC, KH, KW, IC] → IC-tiled DRAM layout.

    Target layout: [num_oc_tiles, num_ic_tiles, tile_oc, KH, KW, tile_ic] contiguous.
    For conv1x1: weight shape is [OC, IC] (KH=KW=1), treated as [OC, 1, 1, IC].
    """
    if weight.ndim == 2:
        OC, IC = weight.shape
        KH, KW = 1, 1
        weight = weight.reshape(OC, KH, KW, IC)
    else:
        OC, KH, KW, IC = weight.shape
    tile_oc = OC // num_oc_tiles
    assert IC == num_ic_tiles * tile_ic, \
        f"IC={IC} != num_ic_tiles={num_ic_tiles} * tile_ic={tile_ic}"
    assert OC == num_oc_tiles * tile_oc, \
        f"OC={OC} != num_oc_tiles={num_oc_tiles} * tile_oc={tile_oc}"
    # [OC, KH, KW, IC] → [num_oc_tiles, tile_oc, KH, KW, num_ic_tiles, tile_ic]
    w = weight.reshape(num_oc_tiles, tile_oc, KH, KW, num_ic_tiles, tile_ic)
    # Transpose to: [num_oc_tiles, num_ic_tiles, tile_oc, KH, KW, tile_ic]
    w = w.transpose(0, 4, 1, 2, 3, 5)
    return w.astype(np.float16).tobytes()


def _tile_input_ic(inp: np.ndarray, num_ic_tiles: int, tile_ic: int) -> bytes:
    """Rearrange input [N, H, W, IC] → IC-tiled DRAM layout.

    Tiled layout: [N, num_ic_tiles, H, W, tile_ic] contiguous.
    """
    N, H, W, IC = inp.shape
    assert IC == num_ic_tiles * tile_ic, \
        f"IC={IC} != num_ic_tiles={num_ic_tiles} * tile_ic={tile_ic}"
    x = inp.reshape(N, H, W, num_ic_tiles, tile_ic)
    x = x.transpose(0, 3, 1, 2, 4)  # [N, num_ic_tiles, H, W, tile_ic]
    return x.astype(np.float16).tobytes()


def _tile_output(output: np.ndarray, layer: dict) -> bytes:
    """Rearrange NHWC output into the firmware's tile-packed DRAM layout.

    Target layout: [N, num_oc_tiles, num_h_tiles, num_w_tiles,
                    tile_h, tile_w, tile_oc] contiguous.
    Edge tiles are zero-padded to the full wave tile size because firmware
    writes back a fixed-size tile for every (oc, h, w) wave.
    """
    tp = layer["tiling_params"]
    agu_plo = layer["agu_plo"]
    pe_params = layer["pe_program"]["params"]

    num_oc_tiles = tp["num_oc_tiles"]
    num_h_tiles = tp["num_h_tiles"]
    num_w_tiles = tp["num_w_tiles"]
    tile_h = agu_plo["iter1"]
    tile_w = agu_plo["iter2"]
    tile_oc = pe_params["KERNEL_COUNT"]

    expected_tile_bytes = tp["dram_out_w_stride"]
    actual_tile_bytes = tile_h * tile_w * tile_oc * 2
    assert actual_tile_bytes == expected_tile_bytes, \
        f"Tile byte mismatch: {actual_tile_bytes} vs dram_out_w_stride {expected_tile_bytes}"

    N, H_out, W_out, OC = output.shape
    packed = np.zeros(
        (N, num_oc_tiles, num_h_tiles, num_w_tiles, tile_h, tile_w, tile_oc),
        dtype=np.float16,
    )

    for oc_idx in range(num_oc_tiles):
        oc_start = oc_idx * tile_oc
        oc_end = min(oc_start + tile_oc, OC)
        for h_idx in range(num_h_tiles):
            h_start = h_idx * tile_h
            h_end = min(h_start + tile_h, H_out)
            for w_idx in range(num_w_tiles):
                w_start = w_idx * tile_w
                w_end = min(w_start + tile_w, W_out)
                packed[:, oc_idx, h_idx, w_idx,
                       :h_end - h_start,
                       :w_end - w_start,
                       :oc_end - oc_start] = output[:, h_start:h_end, w_start:w_end, oc_start:oc_end]

    expected_total_bytes = num_oc_tiles * num_h_tiles * num_w_tiles * tp["dma_plo_words"] * 8
    output_bytes = np.ascontiguousarray(packed).tobytes()
    assert len(output_bytes) == expected_total_bytes, \
        f"Packed output size mismatch: {len(output_bytes)} vs expected {expected_total_bytes}"
    return output_bytes


def main():
    parser = argparse.ArgumentParser(
        description="Generate DRAM mirror + golden output for HybridAcc ESL simulation"
    )
    parser.add_argument("--ir", required=True,
                        help="Path to hardware_ir.json from hacc-compile --dump-ir")
    parser.add_argument("--workload", required=True,
                        help="Path to workload YAML")
    parser.add_argument("--output-dir", required=True,
                        help="Output directory")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed (default: 42)")
    args = parser.parse_args()

    # -- Load workload YAML --
    with open(args.workload) as f:
        wl = yaml.safe_load(f)

    wl_tensors = wl["tensors"]
    ops = wl["ops"]
    dram_base = wl.get("hardware", {}).get("dram_base", 0x80000000)

    # -- Load hardware IR --
    with open(args.ir) as f:
        ir = json.load(f)

    layers = ir["layers"]
    assert len(layers) == len(ops), \
        f"Layer count mismatch: IR has {len(layers)} layers, workload has {len(ops)} ops"

    # -- Generate random test data --
    rng = np.random.RandomState(args.seed)
    tensors_data = {}  # name → np.ndarray (fp16)

    # Pre-generate all input tensors (those not produced by any op)
    output_tensor_names = set()
    for op in ops:
        for name in op["outputs"]:
            output_tensor_names.add(name)

    for name, tdef in wl_tensors.items():
        if name not in output_tensor_names:
            shape = tuple(tdef["shape"])
            # Use small range to avoid fp16 overflow
            tensors_data[name] = rng.uniform(-1.0, 1.0, shape).astype(np.float16)
            print(f"  Generated input tensor '{name}': shape={shape}, "
                  f"range=[{tensors_data[name].min():.4f}, {tensors_data[name].max():.4f}]")

    # -- Compute golden output layer by layer --
    for i, (op, layer) in enumerate(zip(ops, layers)):
        tp = layer["tiling_params"]
        out_name, golden = _compute_layer_golden(op, tensors_data, wl_tensors, rng, tp)
        tensors_data[out_name] = golden
        print(f"  Layer {i} ({op['type']}): output '{out_name}' shape={golden.shape}, "
              f"range=[{golden.min():.4f}, {golden.max():.4f}]")

    # -- Determine final output --
    final_op = ops[-1]
    final_output_name = final_op["outputs"][0]
    final_golden = tensors_data[final_output_name]
    final_tp = layers[-1]["tiling_params"]
    dram_output_base = final_tp["dram_output_base"]

    # -- Build DRAM image --
    # Collect all DRAM regions from all layers
    dram_regions = []  # (offset_from_base, data_bytes)

    for i, (op, layer) in enumerate(zip(ops, layers)):
        tp = layer["tiling_params"]
        num_ic_tiles = tp.get("num_ic_tiles", 1)
        num_oc_tiles = tp.get("num_oc_tiles", 1)

        # For each layer, write weight and input (if it's a source tensor)
        if op["type"] in ("conv2d_3x3", "conv2d_1x1"):
            weight_name = op["inputs"][1]
            weight_offset = tp["dram_weight_base"] - dram_base
            weight_arr = tensors_data[weight_name]
            if num_ic_tiles > 1:
                IC = weight_arr.shape[-1]
                tile_ic = IC // num_ic_tiles
                dram_regions.append((weight_offset,
                                     _tile_weight_ic(weight_arr, num_ic_tiles, tile_ic,
                                                     num_oc_tiles)))
            else:
                dram_regions.append((weight_offset, _get_tensor_bytes(weight_arr)))

            input_name = op["inputs"][0]
            # Only write input if it's a source tensor (not produced by previous layer)
            if i == 0 or input_name not in output_tensor_names:
                input_offset = tp["dram_input_base"] - dram_base
                inp_arr = tensors_data[input_name]
                if num_ic_tiles > 1:
                    IC = inp_arr.shape[-1]
                    tile_ic = IC // num_ic_tiles
                    dram_regions.append((input_offset,
                                         _tile_input_ic(inp_arr, num_ic_tiles, tile_ic)))
                else:
                    dram_regions.append((input_offset, _get_tensor_bytes(inp_arr)))

        elif op["type"] == "gemm":
            a_name = op["inputs"][0]
            b_name = op["inputs"][1]
            a_offset = tp["dram_weight_base"] - dram_base  # A mapped to weight slot
            b_offset = tp["dram_input_base"] - dram_base   # B mapped to input slot
            if i == 0 or a_name not in output_tensor_names:
                dram_regions.append((a_offset, _get_tensor_bytes(tensors_data[a_name])))
            if i == 0 or b_name not in output_tensor_names:
                dram_regions.append((b_offset, _get_tensor_bytes(tensors_data[b_name])))

    # Compute total DRAM size
    # Golden output in tile-packed layout matching firmware DMA writeback order
    output_offset = dram_output_base - dram_base
    num_oc_tiles_final = final_tp.get("num_oc_tiles", 1)
    num_h_tiles_final = final_tp.get("num_h_tiles", 1)
    num_w_tiles_final = final_tp.get("num_w_tiles", 1)
    if final_golden.ndim == 4 and (num_oc_tiles_final > 1 or num_h_tiles_final > 1 or num_w_tiles_final > 1):
        output_bytes = _tile_output(final_golden, layers[-1])
    else:
        output_bytes = _get_tensor_bytes(final_golden)
    dram_end = output_offset + len(output_bytes)
    dram_size = dram_end + 4096  # padding

    # Also account for all regions
    for off, data in dram_regions:
        end = off + len(data)
        if end > dram_size:
            dram_size = end + 4096

    # Account for PLI/bias zero-fill region (always allocated by lowering,
    # even without explicit bias, for accumulator initialization)
    for layer in layers:
        tp = layer["tiling_params"]
        bias_base = tp.get("dram_bias_base", 0)
        pli_words = tp.get("dma_pli_words", 0)
        num_oc = tp.get("num_oc_tiles", 1)
        if bias_base > dram_base and pli_words > 0:
            bias_end = (bias_base - dram_base) + num_oc * pli_words * 8
            if bias_end + 256 > dram_size:
                dram_size = bias_end + 256

    dram = bytearray(dram_size)

    for off, data in dram_regions:
        dram[off:off + len(data)] = data

    # Output region left zeroed — firmware will fill it via DMA writeback

    # -- Write files --
    os.makedirs(args.output_dir, exist_ok=True)

    dram_path = os.path.join(args.output_dir, "dram_init.bin")
    with open(dram_path, "wb") as f:
        f.write(dram)

    golden_path = os.path.join(args.output_dir, "golden_output.bin")
    with open(golden_path, "wb") as f:
        f.write(output_bytes)

    # Determine shapes for meta
    final_output_shape = tuple(wl_tensors[final_output_name]["shape"])

    meta_path = os.path.join(args.output_dir, "golden_meta.txt")
    with open(meta_path, "w") as f:
        f.write(f"dram_base=0x{dram_base:08X}\n")
        # Write per-layer DRAM bases
        for i, layer in enumerate(layers):
            tp = layer["tiling_params"]
            f.write(f"layer{i}_dram_weight_base=0x{tp['dram_weight_base']:08X}\n")
            f.write(f"layer{i}_dram_input_base=0x{tp['dram_input_base']:08X}\n")
            f.write(f"layer{i}_dram_output_base=0x{tp['dram_output_base']:08X}\n")
        f.write(f"dram_output_base=0x{dram_output_base:08X}\n")
        f.write(f"output_shape={list(final_output_shape)}\n")
        f.write(f"dram_image_bytes={dram_size}\n")
        f.write(f"golden_output_bytes={len(output_bytes)}\n")
        f.write(f"output_dram_offset={output_offset}\n")
        f.write(f"num_layers={len(layers)}\n")
        f.write(f"op_types={','.join(op['type'] for op in ops)}\n")
        f.write(f"seed={args.seed}\n")

    print(f"\nGenerated:")
    print(f"  DRAM mirror:    {dram_path} ({dram_size} bytes)")
    print(f"  Golden output:  {golden_path} ({len(output_bytes)} bytes)")
    print(f"  Metadata:       {meta_path}")
    print(f"  Op types:       {', '.join(op['type'] for op in ops)}")
    print(f"  Final output:   {final_output_name} shape={final_output_shape}")


if __name__ == "__main__":
    main()
