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
import yaml


HARDWARE_PAD_EPS = np.float16(np.nextafter(np.float16(0), np.float16(1)))


# ---------------------------------------------------------------------------
# Golden computations (match RTL fp16 multiply + fp16 accumulate semantics)
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

    inp16 = inp.astype(np.float16, copy=False)
    weight16 = weight.astype(np.float16, copy=False)

    if padding > 0:
        inp16 = np.pad(
            inp16,
            ((0, 0), (padding, padding), (padding, padding), (0, 0)),
            mode="constant",
            constant_values=np.float16(0.0),
        )

    OH = ((H + 2 * padding - KH) // stride) + 1
    OW = ((W + 2 * padding - KW) // stride) + 1
    out = np.zeros((N, OH, OW, OC), dtype=np.float16)

    for kh in range(KH):
        h_slice = slice(kh, kh + OH * stride, stride)
        for kw in range(KW):
            w_slice = slice(kw, kw + OW * stride, stride)
            input_patch = inp16[:, h_slice, w_slice, :]

            for ic in range(IC):
                prod = (
                    input_patch[..., ic:ic + 1]
                    * weight16[:, kh, kw, ic].reshape(1, 1, 1, OC)
                ).astype(np.float16, copy=False)
                out = (out + prod).astype(np.float16, copy=False)

    return out


def fp16_gemm_golden(A: np.ndarray, B: np.ndarray) -> np.ndarray:
    """Compute C = A @ B in fp16.

    A: [M, K] fp16
    B: [K, N] fp16
    C: [M, N] fp16
    """
    M, K = A.shape
    K2, N = B.shape
    assert K == K2, f"K mismatch: A {K} vs B {K2}"

    a16 = A.astype(np.float16, copy=False)
    b16 = B.astype(np.float16, copy=False)
    out = np.zeros((M, N), dtype=np.float16)

    for k in range(K):
        prod = (a16[:, k:k + 1] * b16[k:k + 1, :]).astype(np.float16, copy=False)
        out = (out + prod).astype(np.float16, copy=False)

    return out


def _coerce_bool_attr(value, attr_name: str) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value != 0
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in ("1", "true", "yes", "on"):
            return True
        if lowered in ("0", "false", "no", "off"):
            return False
    raise ValueError(f"attr '{attr_name}' must be bool-like, got {value!r}")


def _relu_requested(op: dict) -> bool:
    attrs = op.get("attrs", {})
    activation = attrs.get("activation")
    if activation is None:
        activation = attrs.get("epilogue")
    if activation is not None:
        if not isinstance(activation, str):
            raise ValueError(
                f"{op['name']}: attr 'activation' must be a string, got {type(activation).__name__}"
            )
        activation_l = activation.strip().lower()
        if activation_l in ("", "none", "identity"):
            return False
        if activation_l == "relu":
            return True
        raise ValueError(f"{op['name']}: unsupported activation '{activation}'")

    for attr_name in ("relu", "fuse_relu", "output_relu"):
        if attr_name in attrs and _coerce_bool_attr(attrs[attr_name], attr_name):
            return True
    return False


def _apply_output_epilogue(op: dict, output: np.ndarray) -> np.ndarray:
    if _relu_requested(op):
        return np.maximum(output, np.float16(0.0)).astype(np.float16, copy=False)
    return output


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
        golden = _apply_output_epilogue(op, golden)
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
        golden = _apply_output_epilogue(op, golden)
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


def _make_conv_halo_canvas_nhwc(shape: tuple[int, int, int, int], padding: int) -> np.ndarray:
    """Create an NHWC canvas whose border uses the hardware-safe halo value."""
    if padding <= 0:
        return np.zeros(shape, dtype=np.float16)

    N, H, W, C = shape
    canvas = np.zeros((N, H + 2 * padding, W + 2 * padding, C), dtype=np.float16)
    canvas[:, :padding, :, :] = HARDWARE_PAD_EPS
    canvas[:, -padding:, :, :] = HARDWARE_PAD_EPS
    canvas[:, :, :padding, :] = HARDWARE_PAD_EPS
    canvas[:, :, -padding:, :] = HARDWARE_PAD_EPS
    return canvas


def _pad_conv_input_nhwc(inp: np.ndarray, padding: int) -> np.ndarray:
    """Materialize a padded NHWC tensor for conv source inputs.

    The 3x3 datapath can deadlock on exact-zero halo values, so the hardware
    mirror uses the smallest positive fp16 value while golden math still uses
    logical zero padding.
    """
    if padding <= 0:
        return inp
    padded = _make_conv_halo_canvas_nhwc(inp.shape, padding)
    padded[:, padding:padding + inp.shape[1], padding:padding + inp.shape[2], :] = inp
    return padded


def _pack_gemm_a_pd(A: np.ndarray, layer: dict) -> bytes:
    """Pack GEMM A[M, K] into the PD DMA-visible packed wave layout."""
    if A.ndim != 2:
        raise ValueError(f"GEMM activation must be rank-2, got shape {A.shape}")

    tp = layer["tiling_params"]
    meta = _gemm_wave_meta(layer)
    tp = layer["tiling_params"]
    M, K = A.shape

    tile_bytes = int(tp.get("dma_pd_words", 0)) * 8
    if tile_bytes <= 0:
        raise ValueError("Invalid GEMM dma_pd_words=0")

    packed_region = bytearray(
        _gemm_region_size(
            int(tp.get("num_oc_tiles", 1)),
            int(tp.get("dram_pd_oc_stride", 0)),
            int(tp.get("num_ic_tiles", 1)),
            int(tp.get("dram_pd_ic_stride", 0)),
            tile_bytes,
        )
    )

    for m_wave in range(int(tp.get("num_oc_tiles", 1))):
        m_tile_start, m_tiles_active = _gemm_wave_tile_range(
            m_wave, int(meta["grid_m_per_wave"]), int(meta["grid_m"])
        )
        for k_wave in range(int(tp.get("num_ic_tiles", 1))):
            k_stage_start, k_stages_active = _gemm_wave_tile_range(
                k_wave, int(meta["grid_k_per_wave"]), int(meta["grid_k"])
            )
            packed_tile = np.zeros(
                (
                    int(meta["k_tile_dim"]),
                    int(meta["grid_m_per_wave"]),
                    int(meta["tile_d_words"]),
                    int(meta["rows_per_word"]),
                ),
                dtype=np.float16,
            )
            active_k = min(int(meta["k_tile_dim"]), k_stages_active * int(meta["pe_k"]))
            for local_k in range(active_k):
                k_global = k_stage_start * int(meta["pe_k"]) + local_k
                if k_global >= K:
                    continue
                for local_m_tile in range(m_tiles_active):
                    m_global_tile = m_tile_start + local_m_tile
                    row_base = m_global_tile * int(meta["pe_m"])
                    for row_word in range(int(meta["tile_d_words"])):
                        row_word_base = row_base + row_word * int(meta["rows_per_word"])
                        for lane in range(int(meta["rows_per_word"])):
                            m_global = row_word_base + lane
                            if m_global >= M:
                                continue
                            packed_tile[local_k, local_m_tile, row_word, lane] = A[m_global, k_global]

            offset = (
                m_wave * int(tp.get("dram_pd_oc_stride", 0))
                + k_wave * int(tp.get("dram_pd_ic_stride", 0))
            )
            packed_region[offset:offset + tile_bytes] = np.ascontiguousarray(packed_tile).tobytes()

    return bytes(packed_region)


def _gemm_wave_meta(layer: dict) -> dict[str, int]:
    pe_params = layer.get("pe_program", {}).get("params", {})
    tp = layer.get("tiling_params", {})
    agu_ps = layer.get("agu_ps", {})
    agu_pd = layer.get("agu_pd", {})
    rows_per_word = 4
    grid_m = max(1, int(pe_params.get("GRID_M", 1)))
    grid_n = max(1, int(pe_params.get("GRID_N", 1)))
    grid_k = max(1, int(pe_params.get("GRID_K", 1)))
    grid_m_per_wave = max(1, int(pe_params.get("GRID_M_PER_WAVE", grid_m)))
    grid_n_per_wave = max(1, int(pe_params.get("GRID_N_PER_WAVE", grid_n)))
    grid_k_per_wave = max(1, int(pe_params.get("GRID_K_PER_WAVE", grid_k)))
    tile_d_words = max(1, int(agu_pd.get("iter0", 1)))
    tile_w_words = max(1, int(agu_ps.get("iter0", 1)))
    pe_m = tile_d_words * rows_per_word
    pe_n = max(1, int(pe_params.get("OUTPUT_DIM", tile_w_words * rows_per_word)))
    pe_k = max(1, int(pe_params.get("K_TILE_DIM", pe_params.get("INPUT_DIM", 1))))
    ps_words = max(0, int(tp.get("dma_ps_words", 0)))
    ps_words_per_k = max(1, tile_w_words * grid_n_per_wave)
    pd_words = max(0, int(tp.get("dma_pd_words", 0)))
    pd_words_per_k = max(1, tile_d_words * grid_m_per_wave)
    k_tile_dim = max(
        1,
        ps_words // ps_words_per_k,
        pd_words // pd_words_per_k,
        pe_k * grid_k_per_wave,
    )
    return {
        "grid_m": grid_m,
        "grid_n": grid_n,
        "grid_k": grid_k,
        "grid_m_per_wave": grid_m_per_wave,
        "grid_n_per_wave": grid_n_per_wave,
        "grid_k_per_wave": grid_k_per_wave,
        "tile_d_words": tile_d_words,
        "tile_w_words": tile_w_words,
        "rows_per_word": rows_per_word,
        "pe_m": pe_m,
        "pe_n": pe_n,
        "pe_k": pe_k,
        "k_tile_dim": k_tile_dim,
    }


def _gemm_wave_tile_range(wave_idx: int, tiles_per_wave: int, total_tiles: int) -> tuple[int, int]:
    start = wave_idx * tiles_per_wave
    end = min(total_tiles, start + tiles_per_wave)
    return start, max(0, end - start)


def _gemm_region_size(count_outer: int, outer_stride: int, count_inner: int,
                      inner_stride: int, tile_bytes: int) -> int:
    last_offset = 0
    if count_outer > 0:
        last_offset += (count_outer - 1) * outer_stride
    if count_inner > 0:
        last_offset += (count_inner - 1) * inner_stride
    return last_offset + tile_bytes


def _pack_gemm_b_ps(B: np.ndarray, layer: dict) -> bytes:
    """Pack GEMM B[K, N] into the PS DMA-visible packed wave layout."""
    if B.ndim != 2:
        raise ValueError(f"GEMM weight must be rank-2, got shape {B.shape}")

    tp = layer["tiling_params"]
    meta = _gemm_wave_meta(layer)
    K, N = B.shape
    tile_bytes = int(tp.get("dma_ps_words", 0)) * 8
    if tile_bytes <= 0:
        raise ValueError("Invalid GEMM dma_ps_words=0")

    packed_region = bytearray(
        _gemm_region_size(
            int(tp.get("num_h_tiles", 1)),
            int(tp.get("dram_ps_h_stride", 0)),
            int(tp.get("num_ic_tiles", 1)),
            int(tp.get("dram_ps_ic_stride", 0)),
            tile_bytes,
        )
    )

    for n_wave in range(int(tp.get("num_h_tiles", 1))):
        n_tile_start, n_tiles_active = _gemm_wave_tile_range(
            n_wave, int(meta["grid_n_per_wave"]), int(meta["grid_n"])
        )
        for k_wave in range(int(tp.get("num_ic_tiles", 1))):
            k_stage_start, k_stages_active = _gemm_wave_tile_range(
                k_wave, int(meta["grid_k_per_wave"]), int(meta["grid_k"])
            )
            packed_tile = np.zeros(
                (
                    int(meta["k_tile_dim"]),
                    int(meta["grid_n_per_wave"]),
                    int(meta["tile_w_words"]),
                    int(meta["rows_per_word"]),
                ),
                dtype=np.float16,
            )
            active_k = min(int(meta["k_tile_dim"]), k_stages_active * int(meta["pe_k"]))
            for local_k in range(active_k):
                k_global = k_stage_start * int(meta["pe_k"]) + local_k
                if k_global >= K:
                    continue
                for local_n_tile in range(n_tiles_active):
                    n_global_tile = n_tile_start + local_n_tile
                    col_base = n_global_tile * int(meta["pe_n"])
                    for word_idx in range(int(meta["tile_w_words"])):
                        word_base = col_base + word_idx * int(meta["rows_per_word"])
                        for lane in range(int(meta["rows_per_word"])):
                            n_global = word_base + lane
                            if n_global >= N:
                                continue
                            packed_tile[local_k, local_n_tile, word_idx, lane] = B[k_global, n_global]

            offset = (
                n_wave * int(tp.get("dram_ps_h_stride", 0))
                + k_wave * int(tp.get("dram_ps_ic_stride", 0))
            )
            packed_region[offset:offset + tile_bytes] = np.ascontiguousarray(packed_tile).tobytes()

    return bytes(packed_region)


def _pack_gemm_c_output(C: np.ndarray, layer: dict) -> bytes:
    """Pack GEMM C[M, N] into the PLO DMA-visible packed wave layout."""
    if C.ndim != 2:
        raise ValueError(f"GEMM output must be rank-2, got shape {C.shape}")

    tp = layer["tiling_params"]
    meta = _gemm_wave_meta(layer)
    M, N = C.shape
    tile_bytes = int(tp.get("dma_plo_words", 0)) * 8
    if tile_bytes <= 0:
        raise ValueError("Invalid GEMM dma_plo_words=0")

    packed_region = bytearray(
        _gemm_region_size(
            int(tp.get("num_oc_tiles", 1)),
            int(tp.get("dram_out_oc_stride", 0)),
            int(tp.get("num_h_tiles", 1)),
            int(tp.get("dram_out_h_stride", 0)),
            tile_bytes,
        )
    )

    for m_wave in range(int(tp.get("num_oc_tiles", 1))):
        m_tile_start, m_tiles_active = _gemm_wave_tile_range(
            m_wave, int(meta["grid_m_per_wave"]), int(meta["grid_m"])
        )
        for n_wave in range(int(tp.get("num_h_tiles", 1))):
            n_tile_start, n_tiles_active = _gemm_wave_tile_range(
                n_wave, int(meta["grid_n_per_wave"]), int(meta["grid_n"])
            )
            packed_tile = np.zeros(
                (
                    int(meta["grid_m_per_wave"]),
                    int(meta["grid_n_per_wave"]),
                    int(meta["pe_n"]),
                    int(meta["tile_d_words"]),
                    int(meta["rows_per_word"]),
                ),
                dtype=np.float16,
            )
            for local_m_tile in range(m_tiles_active):
                m_global_tile = m_tile_start + local_m_tile
                row_base = m_global_tile * int(meta["pe_m"])
                for local_n_tile in range(n_tiles_active):
                    n_global_tile = n_tile_start + local_n_tile
                    col_base = n_global_tile * int(meta["pe_n"])
                    for n_local in range(int(meta["pe_n"])):
                        n_global = col_base + n_local
                        if n_global >= N:
                            continue
                        for row_word in range(int(meta["tile_d_words"])):
                            row_word_base = row_base + row_word * int(meta["rows_per_word"])
                            for lane in range(int(meta["rows_per_word"])):
                                m_global = row_word_base + lane
                                if m_global >= M:
                                    continue
                                packed_tile[local_m_tile, local_n_tile, n_local, row_word, lane] = C[m_global, n_global]

            offset = (
                m_wave * int(tp.get("dram_out_oc_stride", 0))
                + n_wave * int(tp.get("dram_out_h_stride", 0))
            )
            packed_region[offset:offset + tile_bytes] = np.ascontiguousarray(packed_tile).tobytes()

    return bytes(packed_region)


def _tile_output(output: np.ndarray, layer: dict) -> bytes:
    """Rearrange NHWC output into the firmware's tile-packed DRAM layout.

    Target layout: [N, num_oc_tiles, num_h_tiles, num_w_tiles,
                    tile_h, tile_w, tile_oc] contiguous.
    Edge tiles are zero-padded to the full wave tile size because firmware
    writes back a fixed-size tile for every (oc, h, w) wave.
    """
    tp = layer["tiling_params"]
    pe_params = layer["pe_program"]["params"]

    num_oc_tiles = tp["num_oc_tiles"]
    num_h_tiles = tp["num_h_tiles"]
    num_w_tiles = tp["num_w_tiles"]
    tile_h = tp["tile_h_out"]
    tile_w = tp["tile_w_out"]
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

    expected_total_bytes = N * num_oc_tiles * num_h_tiles * num_w_tiles * tp["dram_out_w_stride"]
    output_bytes = np.ascontiguousarray(packed).tobytes()
    assert len(output_bytes) == expected_total_bytes, \
        f"Packed output size mismatch: {len(output_bytes)} vs expected {expected_total_bytes}"
    return output_bytes


def _parallel_bank_span(tp: dict, plane: str) -> int:
    plane_bit = {"ps": 0, "pd": 1, "pli": 2, "plo": 3}[plane]
    if (tp.get("parallel_groups", 0) & (1 << plane_bit)) == 0:
        return 1

    rows_key = {
        "pd": "dma_pd_rows_per_bank",
        "pli": "dma_pli_rows_per_bank",
        "plo": "dma_plo_rows_per_bank",
    }.get(plane)
    total_rows_key = "tile_h_in" if plane == "pd" else "tile_h_out"

    if rows_key is None:
        return 1

    rows_per_bank = tp.get(rows_key, 0)
    total_rows = tp.get(total_rows_key, 0)
    if rows_per_bank <= 0 or total_rows <= 0:
        return 1
    return max(1, (total_rows + rows_per_bank - 1) // rows_per_bank)


def _bias_region_size(tp: dict) -> int:
    last_offset = 0
    num_oc = int(tp.get("num_oc_tiles", 1))
    num_h = int(tp.get("num_h_tiles", 1))
    if num_oc > 0:
        last_offset += (num_oc - 1) * int(tp.get("dram_bias_oc_stride", 0))
    if num_h > 0:
        last_offset += (num_h - 1) * int(tp.get("dram_bias_h_stride", 0))
    return last_offset + int(tp.get("dma_pli_words", 0)) * 8


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
    output_tensor_map = {}
    for op in ops:
        for name in op["outputs"]:
            output_tensor_names.add(name)
    for idx, op in enumerate(ops):
        for name in op["outputs"]:
            output_tensor_map[name] = idx

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
            a_offset = tp["dram_input_base"] - dram_base   # A mapped to PD/input slot
            b_offset = tp["dram_weight_base"] - dram_base  # B mapped to PS/weight slot
            if i == 0 or a_name not in output_tensor_names:
                dram_regions.append((a_offset, _pack_gemm_a_pd(tensors_data[a_name], layer)))
            if i == 0 or b_name not in output_tensor_names:
                dram_regions.append((b_offset, _pack_gemm_b_ps(tensors_data[b_name], layer)))

    # Compute total DRAM size
    # Golden output in tile-packed layout matching firmware DMA writeback order
    output_offset = dram_output_base - dram_base
    num_oc_tiles_final = final_tp.get("num_oc_tiles", 1)
    num_h_tiles_final = final_tp.get("num_h_tiles", 1)
    num_w_tiles_final = final_tp.get("num_w_tiles", 1)
    if final_op["type"] == "gemm":
        output_bytes = _pack_gemm_c_output(final_golden, layers[-1])
    elif final_golden.ndim == 4 and (num_oc_tiles_final > 1 or num_h_tiles_final > 1 or num_w_tiles_final > 1):
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
        if bias_base > dram_base and tp.get("dma_pli_words", 0) > 0:
            bias_end = (bias_base - dram_base) + _bias_region_size(tp)
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
