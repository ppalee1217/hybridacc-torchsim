import torch
import math
import numpy as np
from typing import Dict, Any, List, Optional, Tuple
from ..utils.config import ScanChainConfig, PERouterMode, ClusterConvConfig, ClusterGemmConfig, ConvMode
from ..utils.data import TestData, ClusterTestData
from ..utils.io import compile_pe_program
from ..model.conv import golden_conv2d
from ..model.gemm import golden_gemm
from .pe_gen import DataGenerator
from pathlib import Path


def _num_words64_from_shape(shape: List[int]) -> int:
    elems = int(np.prod(shape))
    return (elems + 3) // 4


def _to_group_local_word_addr(addr_bytes: int) -> int:
    spm_word_bytes = 8
    spm_group_span_words = 8192 * 4
    return (int(addr_bytes) // spm_word_bytes) % spm_group_span_words


def _ceil_div_int(a: int, b: int) -> int:
    if b <= 0:
        return 0
    return (a + b - 1) // b


def _get_wave_range(total: int, waves: int, wave_idx: int) -> Tuple[int, int]:
    if waves <= 0:
        return 0, total
    per_wave = (total + waves - 1) // waves
    start = wave_idx * per_wave
    end = min(start + per_wave, total)
    return start, end


def _get_wave_tile_range(tiles_per_wave: List[int], wave_idx: int, total_tiles: int, fallback_waves: int) -> Tuple[int, int]:
    if tiles_per_wave:
        start = 0
        for i in range(min(wave_idx, len(tiles_per_wave))):
            start += int(tiles_per_wave[i])
        count = int(tiles_per_wave[wave_idx]) if wave_idx < len(tiles_per_wave) else 0
        if start >= total_tiles:
            start = total_tiles
        end = min(start + count, total_tiles)
        return start, end
    return _get_wave_range(total_tiles, fallback_waves, wave_idx)


def _new_agu_cfg(enable: bool = False, ultra: bool = False) -> Dict[str, Any]:
    return {
        "base_addr": 0,
        "base_addr_h": 0,
        "iter0": 1,
        "iter1": 1,
        "iter2": 1,
        "iter3": 1,
        "stride0": 0,
        "stride1": 0,
        "stride2": 0,
        "stride3": 0,
        "lane_cfg": 0,
        "tag_base": 0,
        "tag_stride0": 0,
        "tag_stride1": 0,
        "tag_ctrl": 0,
        "mask_cfg": 0xF,
        "ultra": bool(ultra),
        "enable": bool(enable),
    }


def _compile_cluster_plans_gemm(
    meta: Dict[str, Any],
    runtime_addr: Dict[str, int],
    runtime_addr_per_wave: Optional[List[Dict[str, int]]] = None,
) -> List[Dict[str, Any]]:
    plans: List[Dict[str, Any]] = []

    M = int(meta["M"])
    N = int(meta["N"])
    grid_m = int(meta["grid_m"])
    grid_n = int(meta["grid_n"])
    grid_k = int(meta["grid_k"])
    wave_m = int(meta["wave_m"])
    wave_n = int(meta["wave_n"])
    wave_k = int(meta["wave_k"])
    ultra_mode = bool(meta.get("ultra_mode", False))

    pe_m = int(meta.get("pe_m", 12))
    pe_n = int(meta.get("pe_n", 8))
    pe_k = int(meta.get("pe_k", 32))

    grid_m_per_wave = [int(v) for v in meta.get("grid_m_per_wave", [])]
    grid_n_per_wave = [int(v) for v in meta.get("grid_n_per_wave", [])]
    grid_k_per_wave = [int(v) for v in meta.get("grid_k_per_wave", [])]
    agu_ultra_overrides: Dict[str, bool] = {
        str(k): bool(v) for k, v in dict(meta.get("agu_ultra_overrides", {})).items()
    }

    waves_k = len(grid_k_per_wave) if grid_k_per_wave else wave_k
    waves_n = len(grid_n_per_wave) if grid_n_per_wave else wave_n
    waves_m = len(grid_m_per_wave) if grid_m_per_wave else wave_m
    waves_k = max(1, int(waves_k))
    waves_n = max(1, int(waves_n))
    waves_m = max(1, int(waves_m))

    row_w = _ceil_div_int(N, 4)
    col_d = _ceil_div_int(M, 4)
    tile_w = max(1, _ceil_div_int(pe_n, 4))
    tile_d = max(1, _ceil_div_int(pe_m, 4))

    wave_idx = 0
    for wk in range(waves_k):
        for wn in range(waves_n):
            for wm in range(waves_m):
                k_r = _get_wave_tile_range(grid_k_per_wave, wk, grid_k, waves_k)
                n_r = _get_wave_tile_range(grid_n_per_wave, wn, grid_n, waves_n)
                m_r = _get_wave_tile_range(grid_m_per_wave, wm, grid_m, waves_m)
                wave_runtime_addr = runtime_addr_per_wave[wave_idx] if (runtime_addr_per_wave and wave_idx < len(runtime_addr_per_wave)) else runtime_addr
                wave_idx += 1
                if k_r[0] >= k_r[1] or n_r[0] >= n_r[1] or m_r[0] >= m_r[1]:
                    continue

                ps_local_base = _to_group_local_word_addr(wave_runtime_addr["weight"])
                pd_local_base = _to_group_local_word_addr(wave_runtime_addr["activation"])
                pli_local_base = _to_group_local_word_addr(wave_runtime_addr["partial_sum"])
                plo_local_base = _to_group_local_word_addr(wave_runtime_addr["output"])

                n_tiles = n_r[1] - n_r[0]
                for kt in range(k_r[0], k_r[1]):
                    for mt in range(m_r[0], m_r[1]):
                        plan = {
                            "name": f"GEMM_K{wk}_N{wn}_M{wm}_KT{kt}_MT{mt}",
                            "global_mask": 0xF,
                            "ultra_mode": ultra_mode,
                            "agu_ps": _new_agu_cfg(True, ultra_mode),
                            "agu_pd": _new_agu_cfg(True, ultra_mode),
                            "agu_pli": _new_agu_cfg(False, ultra_mode),
                            "agu_plo": _new_agu_cfg(False, ultra_mode),
                        }

                        for agu_key, ultra_val in agu_ultra_overrides.items():
                            if agu_key in plan and isinstance(plan[agu_key], dict):
                                plan[agu_key]["ultra"] = bool(ultra_val)

                        k_off = kt * pe_k
                        n_off = n_r[0] * pe_n
                        m_off = mt * pe_m

                        ps_base_words = (k_off * row_w) + (n_off // 4)
                        agu_ps = plan["agu_ps"]
                        agu_ps["base_addr"] = int(ps_local_base + ps_base_words)
                        agu_ps["iter0"] = int(tile_w)
                        agu_ps["iter1"] = int(pe_k)
                        agu_ps["iter2"] = int(n_tiles)
                        agu_ps["iter3"] = 1
                        agu_ps["stride0"] = 1
                        agu_ps["stride1"] = int(row_w)
                        agu_ps["stride2"] = int(tile_w)
                        agu_ps["stride3"] = int(pe_k * row_w)
                        agu_ps["tag_base"] = 0 if ultra_mode else int(kt * grid_n + n_r[0])
                        agu_ps["tag_stride0"] = 1
                        agu_ps["tag_stride1"] = 1
                        agu_ps["tag_ctrl"] = 2

                        pd_base_words = (k_off * col_d) + (m_off // 4)
                        agu_pd = plan["agu_pd"]
                        agu_pd["base_addr"] = int(pd_local_base + pd_base_words)
                        agu_pd["iter0"] = int(tile_d)
                        agu_pd["iter1"] = int(pe_k)
                        agu_pd["iter2"] = 1
                        agu_pd["iter3"] = 1
                        agu_pd["stride0"] = 1
                        agu_pd["stride1"] = int(col_d)
                        agu_pd["stride2"] = int(pe_k * col_d)
                        agu_pd["stride3"] = int(tile_d)
                        agu_pd["tag_base"] = 0 if ultra_mode else int(kt * grid_m + mt)
                        agu_pd["tag_stride0"] = 0
                        agu_pd["tag_stride1"] = 1
                        agu_pd["tag_ctrl"] = 0

                        if wk == 0:
                            pli_base_words = (n_off * col_d) + (m_off // 4)
                            agu_pli = plan["agu_pli"]
                            agu_pli["enable"] = True
                            agu_pli["base_addr"] = int(pli_local_base + pli_base_words)
                            agu_pli["iter0"] = int(tile_d)
                            agu_pli["iter1"] = int(pe_n)
                            agu_pli["iter2"] = int(n_tiles)
                            agu_pli["iter3"] = 1
                            agu_pli["stride0"] = 1
                            agu_pli["stride1"] = int(col_d)
                            agu_pli["stride2"] = int(pe_n * col_d)
                            agu_pli["stride3"] = int(tile_d)
                            agu_pli["tag_base"] = 0 if ultra_mode else int(mt * grid_n + n_r[0])
                            agu_pli["tag_stride0"] = 1
                            agu_pli["tag_stride1"] = 1
                            agu_pli["tag_ctrl"] = 2

                        if wk == waves_k - 1:
                            plo_base_words = (n_off * col_d) + (m_off // 4)
                            agu_plo = plan["agu_plo"]
                            agu_plo["enable"] = True
                            agu_plo["base_addr"] = int(plo_local_base + plo_base_words)
                            agu_plo["iter0"] = int(tile_d)
                            agu_plo["iter1"] = int(pe_n)
                            agu_plo["iter2"] = int(n_tiles)
                            agu_plo["iter3"] = 1
                            agu_plo["stride0"] = 1
                            agu_plo["stride1"] = int(col_d)
                            agu_plo["stride2"] = int(pe_n * col_d)
                            agu_plo["stride3"] = int(tile_d)
                            agu_plo["tag_base"] = 0 if ultra_mode else int(mt * grid_n + n_r[0])
                            agu_plo["tag_stride0"] = 1
                            agu_plo["tag_stride1"] = 1
                            agu_plo["tag_ctrl"] = 2

                        plans.append(plan)

    return plans


def _compile_cluster_plans_conv2d(
    meta: Dict[str, Any],
    wave_schedule: Dict[str, int],
    runtime_addr: Dict[str, int],
    runtime_addr_per_wave: Optional[List[Dict[str, int]]] = None,
) -> List[Dict[str, Any]]:
    plans: List[Dict[str, Any]] = []

    ks = meta["kernel_size"]
    kernel_size = int(ks[0] if isinstance(ks, list) else ks)
    stride = int(meta["stride"])
    ultra_mode = bool(meta.get("ultra_mode", False))
    agu_ultra_overrides = meta.get("agu_ultra_overrides", {})
    shapes = meta.get("tensor_shapes", {})

    if "activation" not in shapes or "output" not in shapes:
        raise ValueError("tensor_shapes.activation/output are required for conv2d cluster plan generation")

    act_shape = [int(v) for v in shapes["activation"]]
    out_shape = [int(v) for v in shapes["output"]]
    in_h, in_w, in_ch = int(act_shape[1]), int(act_shape[2]), int(act_shape[3])
    out_h, out_w, out_ch = int(out_shape[1]), int(out_shape[2]), int(out_shape[3])

    waves_h = max(1, int(wave_schedule.get("temporal_wave_out_h", 1)))
    waves_oc = max(1, int(wave_schedule.get("temporal_wave_out_ch", 1)))
    waves_ic = max(1, int(wave_schedule.get("temporal_wave_in_ch", 1)))

    loop_in_height = in_h
    loop_out_height = out_h
    if ultra_mode:
        num_ports = 3
        loop_in_height = max(1, in_h // num_ports)
        loop_out_height = max(1, out_h // num_ports)

    wave_idx = 0
    for wh in range(waves_h):
        for woc in range(waves_oc):
            for wic in range(waves_ic):
                oh_r = _get_wave_range(loop_out_height, waves_h, wh)
                oc_r = _get_wave_range(out_ch, waves_oc, woc)
                ic_r = _get_wave_range(in_ch, waves_ic, wic)
                wave_runtime_addr = runtime_addr_per_wave[wave_idx] if (runtime_addr_per_wave and wave_idx < len(runtime_addr_per_wave)) else runtime_addr
                wave_idx += 1
                if oh_r[0] >= oh_r[1] or oc_r[0] >= oc_r[1] or ic_r[0] >= ic_r[1]:
                    continue

                ih_start = int(oh_r[0]) * stride
                ih_end = min(loop_in_height, int(oh_r[1]) * stride + kernel_size - 1)
                if ih_start >= ih_end:
                    continue

                ps_local_base = _to_group_local_word_addr(wave_runtime_addr["weight"])
                pd_local_base = _to_group_local_word_addr(wave_runtime_addr["activation"])
                pli_local_base = _to_group_local_word_addr(wave_runtime_addr["partial_sum"])
                plo_local_base = _to_group_local_word_addr(wave_runtime_addr["output"])

                count_oc = oc_r[1] - oc_r[0]
                count_oc_pack = max(1, _ceil_div_int(count_oc, 4))
                count_ic_pack = max(1, _ceil_div_int(ic_r[1] - ic_r[0], 4))

                plan = {
                    "name": f"CONV_H{wh}_OC{woc}_IC{wic}",
                    "global_mask": 0xF,
                    "ultra_mode": ultra_mode,
                    "agu_ps": _new_agu_cfg(True, ultra_mode),
                    "agu_pd": _new_agu_cfg(True, ultra_mode),
                    # Keep partial-sum read/write AGUs active on every wave.
                    # DMA writeback policy is independent from in-cluster accumulation traffic.
                    "agu_pli": _new_agu_cfg(True, ultra_mode),
                    "agu_plo": _new_agu_cfg(True, ultra_mode),
                }

                for agu_key, ultra_val in agu_ultra_overrides.items():
                    if agu_key in plan and isinstance(plan[agu_key], dict):
                        plan[agu_key]["ultra"] = bool(ultra_val)

                agu_ps = plan["agu_ps"]
                agu_ps["iter0"] = int(count_ic_pack)
                agu_ps["iter1"] = int(kernel_size)
                agu_ps["iter2"] = int(kernel_size)
                agu_ps["iter3"] = int(count_oc)
                agu_ps["stride0"] = 1
                agu_ps["stride1"] = int(count_ic_pack)
                agu_ps["stride2"] = int(kernel_size * count_ic_pack)
                agu_ps["stride3"] = int(kernel_size * kernel_size * count_ic_pack)
                agu_ps["base_addr"] = int(ps_local_base)
                agu_ps["tag_base"] = 0
                agu_ps["tag_stride0"] = 1
                agu_ps["tag_stride1"] = 1
                agu_ps["tag_ctrl"] = 2

                agu_pd = plan["agu_pd"]
                agu_pd["iter0"] = int(count_ic_pack)
                agu_pd["iter1"] = int(ih_end - ih_start)
                agu_pd["iter2"] = int(in_w)
                agu_pd["iter3"] = 1
                agu_pd["stride0"] = 1
                agu_pd["stride1"] = int(in_w * count_ic_pack)
                agu_pd["stride2"] = int(count_ic_pack)
                agu_pd["stride3"] = 0
                agu_pd["base_addr"] = int(pd_local_base)
                agu_pd["tag_base"] = 0
                agu_pd["tag_stride0"] = 1
                agu_pd["tag_stride1"] = 1
                agu_pd["tag_ctrl"] = 1

                for key, base in (("agu_pli", pli_local_base), ("agu_plo", plo_local_base)):
                    agu = plan[key]
                    agu["base_addr"] = int(base)
                    agu["iter0"] = int(count_oc_pack)
                    agu["iter1"] = int(oh_r[1] - oh_r[0])
                    agu["iter2"] = int(out_w)
                    agu["iter3"] = 1
                    agu["stride0"] = 1
                    agu["stride1"] = int(out_w * count_oc_pack)
                    agu["stride2"] = int(count_oc_pack)
                    agu["stride3"] = 0
                    agu["tag_base"] = 0
                    agu["tag_stride0"] = 1
                    agu["tag_stride1"] = 1
                    agu["tag_ctrl"] = 1

                plans.append(plan)

    return plans


def _build_runtime_addr_per_wave(spm_cfg: Dict[str, Any], dma_cfg: Dict[str, Any]) -> List[Dict[str, int]]:
    def _resolve_section_name(section_ref: Any) -> Optional[str]:
        if isinstance(section_ref, str):
            return section_ref
        if isinstance(section_ref, list):
            for v in section_ref:
                if isinstance(v, str):
                    return v
        return None

    section_lookup: Dict[str, Dict[str, Any]] = {}
    for group in spm_cfg.get("groups", []):
        for sec in group.get("sections", []):
            sec_name = sec.get("name")
            if sec_name:
                section_lookup[str(sec_name)] = sec

    tensor_mapping = spm_cfg.get("tensor_mapping", {})
    per_wave_addrs: List[Dict[str, int]] = []
    for wave in dma_cfg.get("waves", []):
        runtime_sections = wave.get("runtime_sections", {}) if isinstance(wave, dict) else {}
        addrs: Dict[str, int] = {}
        for tensor_name in ["weight", "activation", "partial_sum", "output"]:
            mapping = tensor_mapping.get(tensor_name, {})
            spm_mode = str(mapping.get("spm_mode", "linear"))
            sec_name_ref = runtime_sections.get(tensor_name, mapping.get("section", mapping.get("ping_section")))
            sec_name = _resolve_section_name(sec_name_ref)
            sec = section_lookup.get(sec_name) if sec_name is not None else None
            if sec is not None:
                base_key = "local_parallel_base" if spm_mode == "parallel" else "local_linear_base"
                addrs[tensor_name] = int(sec.get(base_key, 0))
            else:
                fallback_key = "parallel_spm_addr" if spm_mode == "parallel" else "linear_spm_addr"
                addrs[tensor_name] = int(mapping.get(fallback_key, mapping.get("spm_addr", 0)))
        per_wave_addrs.append(addrs)
    return per_wave_addrs

# SPM Addressing:
# Linear Region: Each group has a contiguous linear region for DMA access (e.g., 12288 words).
# Parallel Region: Each group has a parallel region divided into rows for PE access (e.g., 8192 words with 256 rows of 32 words each).
# Note: The exact sizes can be tuned based on the expected tensor sizes and access patterns.

# DMA Plan:
# For each temporal wave, we select either the ping or pong section for each tensor based on the wave_id (even/odd).
# We generate DMA transfers for activation, weight, and partial sum tensors, calculating the source DRAM address based on the cumulative offset for each tensor.
# The destination SPM address is determined by the selected section's global parallel base (for PE access) and global linear base (for DMA).

def _build_spm_dma_plan(
    dram_mapping: Dict[str, int],
    tensor_words64: Dict[str, int],
    temporal_wave_count: int,
    tensor_shapes: Optional[Dict[str, List[int]]] = None,
    wave_schedule: Optional[Dict[str, int]] = None,
    use_parallel_for_noc: bool = False,
    tensor_mbus_shared: Optional[Dict[str, bool]] = None,
    tensor_section_mode: Optional[Dict[str, str]] = None,
    tensor_spm_mode: Optional[Dict[str, str]] = None,
    base_spm_addr: int = 0,
    align_words: int = 256,
) -> Dict[str, Any]:
    word_bytes = 8
    num_groups = 4
    banks_per_group = 3
    bank_depth_words = 8192
    group_linear_words = bank_depth_words * banks_per_group      # 24576
    group_parallel_base = group_linear_words                     # 24576
    group_span_words = bank_depth_words * (banks_per_group + 1) # 32768
    group_linear_bytes = group_linear_words * word_bytes
    group_parallel_base_bytes = group_parallel_base * word_bytes
    group_span_bytes = group_span_words * word_bytes

    role_to_group = {
        "weight": 0,       # PS
        "activation": 1,   # PD
        "partial_sum": 2,  # PLI
        "output": 3,       # PLO
    }

    channel_names = {
        0: "PS",
        1: "PD",
        2: "PLI",
        3: "PLO",
    }
    channel_name_by_group = {v: k for k, v in channel_names.items()}

    def align_up(words: int) -> int:
        return max(align_words, ((max(words, 1) + align_words - 1) // align_words) * align_words)

    def split_evenly(total_words: int, count: int) -> List[int]:
        count = max(1, count)
        base = total_words // count
        rem = total_words % count
        return [base + 1 if i < rem else base for i in range(count)]

    def build_wave_coords(wave_count: int, waves_h: int, waves_oc: int, waves_ic: int) -> List[Tuple[int, int, int]]:
        coords: List[Tuple[int, int, int]] = []
        if waves_h * waves_oc * waves_ic == wave_count:
            for wh in range(waves_h):
                for woc in range(waves_oc):
                    for wic in range(waves_ic):
                        coords.append((wh, woc, wic))
            return coords
        for wave_id in range(wave_count):
            coords.append((wave_id, 0, 0))
        return coords

    def split_by_wave_dependency(
        total_words: int,
        coords: List[Tuple[int, int, int]],
        active_fn,
        key_fn,
    ) -> List[int]:
        per_wave = [0 for _ in coords]
        if total_words <= 0 or not coords:
            return per_wave

        key_to_first_wave: Dict[Tuple[int, ...], int] = {}
        for wave_id, coord in enumerate(coords):
            if not active_fn(coord):
                continue
            key = key_fn(coord)
            if key not in key_to_first_wave:
                key_to_first_wave[key] = wave_id

        if not key_to_first_wave:
            return per_wave

        chunks = split_evenly(total_words, len(key_to_first_wave))
        for words, wave_id in zip(chunks, key_to_first_wave.values()):
            per_wave[wave_id] = int(words)
        return per_wave

    def build_addr_gen4d(base_addr: int, iter_vals: List[int], stride_vals: List[int]) -> Dict[str, Any]:
        return {
            "base_addr": int(base_addr),
            "iter": [int(v) for v in iter_vals],
            "stride": [int(v) for v in stride_vals],
            "unit": "byte",
        }

    def wave_dim_range(total: int, waves: int, wave_idx: int) -> Tuple[int, int]:
        chunks = split_evenly(max(0, int(total)), max(1, int(waves)))
        start = int(sum(chunks[:max(0, int(wave_idx))]))
        if wave_idx < 0 or wave_idx >= len(chunks):
            return start, 0
        return start, int(chunks[wave_idx])

    def build_packed_4d_slice_addr_gen(base_addr: int,
                                       shape: List[int],
                                       starts: List[int],
                                       counts: List[int]) -> Optional[Dict[str, Any]]:
        if len(shape) != 4 or len(starts) != 4 or len(counts) != 4:
            return None

        d0, d1, d2, d3_elems = [int(v) for v in shape]
        s0, s1, s2, s3 = [int(v) for v in starts]
        c0, c1, c2, c3_words = [int(v) for v in counts]
        d3_words = _ceil_div_int(d3_elems, 4)

        if d0 <= 0 or d1 <= 0 or d2 <= 0 or d3_words <= 0:
            return None
        if c0 <= 0 or c1 <= 0 or c2 <= 0 or c3_words <= 0:
            return None
        if s0 < 0 or s1 < 0 or s2 < 0 or s3 < 0:
            return None
        if s0 + c0 > d0 or s1 + c1 > d1 or s2 + c2 > d2 or s3 + c3_words > d3_words:
            return None

        stride3 = 8
        stride2 = d3_words * stride3
        stride1 = d2 * stride2
        stride0 = d1 * stride1
        base = int(base_addr) + s0 * stride0 + s1 * stride1 + s2 * stride2 + s3 * stride3

        return build_addr_gen4d(
            base,
            [c0, c1, c2, c3_words],
            [stride0, stride1, stride2, stride3],
        )

    def build_compact_linear_addr_gen(base_addr: int, word_count: int) -> Optional[Dict[str, Any]]:
        words = int(word_count)
        if words <= 0:
            return None
        return build_packed_4d_slice_addr_gen(int(base_addr), [1, 1, words, 4], [0, 0, 0, 0], [1, 1, words, 1])

    def pack_spm_map(port_to_group: Dict[str, int]) -> int:
        ports = ["PS", "PD", "PLI", "PLO"]
        map_val = 0
        for p_idx, p_name in enumerate(ports):
            g = int(port_to_group.get(p_name, p_idx)) & 0x3
            map_val |= (g << (p_idx * 2))
        return int(map_val)

    groups = []
    section_lookup: Dict[str, Dict[str, Any]] = {}

    linear_half = group_linear_words // 2
    parallel_half = bank_depth_words // 2
    bank_linear_half = bank_depth_words // 2

    ultra_bank_linear_mode = bool(use_parallel_for_noc and wave_schedule and tensor_shapes)

    # Per-tensor policy:
    # - section_mode: "group" or "bank" controls section naming/allocation.
    # - spm_mode: "linear" or "parallel" controls AGU runtime base addressing.
    default_section_mode = "bank" if ultra_bank_linear_mode else "group"
    section_mode_cfg: Dict[str, str] = {}
    for tensor_name in role_to_group:
        if bool((tensor_mbus_shared or {}).get(tensor_name, False)):
            section_mode_cfg[tensor_name] = "group"
            continue
        raw_mode = str((tensor_section_mode or {}).get(tensor_name, default_section_mode)).lower()
        section_mode_cfg[tensor_name] = raw_mode if raw_mode in ("group", "bank") else default_section_mode

    group_use_bank_sections: Dict[int, bool] = {g: False for g in range(num_groups)}
    for tensor_name, group_id in role_to_group.items():
        if section_mode_cfg[tensor_name] == "bank":
            group_use_bank_sections[int(group_id)] = True

    for g in range(num_groups):
        group_base_words = base_spm_addr + g * group_span_words
        group_base_bytes = group_base_words * word_bytes
        group_sections = []
        if group_use_bank_sections.get(g, False):
            for b in range(banks_per_group):
                bank_linear_words = b * bank_depth_words
                for tag, half_off in (("ping", 0), ("pong", bank_linear_half)):
                    sec_name = f"g{g}_b{b}_{tag}"
                    local_linear_words = bank_linear_words + half_off
                    # Keep ping/pong phase reflected in parallel metadata address, too.
                    local_parallel_words = group_parallel_base + half_off
                    global_linear_words = group_base_words + local_linear_words
                    global_parallel_words = group_base_words + local_parallel_words
                    sec = {
                        "name": sec_name,
                        "tag": tag,
                        "bank_id": int(b),
                        "local_linear_base": local_linear_words * word_bytes,
                        "local_parallel_base": local_parallel_words * word_bytes,
                        "global_linear_addr": global_linear_words * word_bytes,
                        "global_parallel_addr": global_parallel_words * word_bytes,
                        "size_words64_linear": bank_linear_half,
                        "size_words64_parallel": parallel_half,
                    }
                    group_sections.append(sec)
                    section_lookup[sec_name] = sec
        else:
            section_layout = [
                ("ping", 0, 0),
                ("pong", linear_half, parallel_half),
            ]
            for tag, linear_off, parallel_row_off in section_layout:
                sec_name = f"g{g}_{tag}"
                local_linear_words = linear_off
                local_parallel_words = group_parallel_base + parallel_row_off
                global_linear_words = group_base_words + local_linear_words
                global_parallel_words = group_base_words + local_parallel_words
                sec = {
                    "name": sec_name,
                    "tag": tag,
                    "local_linear_base": local_linear_words * word_bytes,
                    "local_parallel_base": local_parallel_words * word_bytes,
                    "global_linear_addr": global_linear_words * word_bytes,
                    "global_parallel_addr": global_parallel_words * word_bytes,
                    "size_words64_linear": linear_half,
                    "size_words64_parallel": parallel_half,
                }
                group_sections.append(sec)
                section_lookup[sec_name] = sec

        groups.append({
            "group_id": g,
            "noc_channel": channel_names[g],
            "sections": group_sections,
        })

    tensor_mapping: Dict[str, Dict[str, Any]] = {}
    wave_count = max(1, int(temporal_wave_count))
    waves_h = max(1, int((wave_schedule or {}).get("temporal_wave_out_h", 1)))
    waves_oc = max(1, int((wave_schedule or {}).get("temporal_wave_out_ch", 1)))
    waves_ic = max(1, int((wave_schedule or {}).get("temporal_wave_in_ch", 1)))
    both_ich_och_tiled = (waves_oc > 1 and waves_ic > 1)
    wave_coords = build_wave_coords(wave_count, waves_h, waves_oc, waves_ic)

    for tensor_name, group_id in role_to_group.items():
        section_mode = section_mode_cfg[tensor_name]
        if section_mode == "bank":
            ping_name: Any = [f"g{group_id}_b{b}_ping" for b in range(banks_per_group)]
            pong_name: Any = [f"g{group_id}_b{b}_pong" for b in range(banks_per_group)]
            primary_ping = ping_name[0]
        else:
            ping_name = f"g{group_id}_ping"
            pong_name = f"g{group_id}_pong"
            primary_ping = ping_name

        ping_sec = section_lookup[primary_ping]
        words = int(tensor_words64.get(tensor_name, 0))

        if wave_schedule:
            if tensor_name == "activation":
                # Default loop assumption: ICH inner, OCH outer.
                # When OCH and ICH are both tiled, activation kept for inner-loop reuse
                # gets overwritten before next OCH, so it must be reloaded per (wh,och,ich).
                if both_ich_och_tiled:
                    per_wave_words = split_by_wave_dependency(
                        words * waves_oc,
                        wave_coords,
                        active_fn=lambda c: True,
                        key_fn=lambda c: (c[0], c[1], c[2]),
                    )
                else:
                    per_wave_words = split_by_wave_dependency(
                        words,
                        wave_coords,
                        active_fn=lambda c: c[1] == 0,
                        key_fn=lambda c: (c[0], c[2]),
                    )
            elif tensor_name == "weight":
                # Weight depends on both OCH/ICH tile indices.
                per_wave_words = split_by_wave_dependency(
                    words,
                    wave_coords,
                    active_fn=lambda c: c[0] == 0,
                    key_fn=lambda c: (c[1], c[2]),
                )
            elif tensor_name == "partial_sum":
                # Input partial-sum is needed at the first ICH wave of each OCH tile.
                per_wave_words = split_by_wave_dependency(
                    words,
                    wave_coords,
                    active_fn=lambda c: c[2] == 0,
                    key_fn=lambda c: (c[0], c[1]),
                )
            else:
                # Output partial-sum writeback is done at the last ICH wave of each OCH tile.
                per_wave_words = split_by_wave_dependency(
                    words,
                    wave_coords,
                    active_fn=lambda c: c[2] == (waves_ic - 1),
                    key_fn=lambda c: (c[0], c[1]),
                )
        else:
            per_wave_words = split_evenly(words, wave_count)

        requested_spm_mode = str((tensor_spm_mode or {}).get(tensor_name, "")).lower()
        if bool((tensor_mbus_shared or {}).get(tensor_name, False)):
            use_parallel = False
        elif requested_spm_mode in ("linear", "parallel"):
            use_parallel = requested_spm_mode == "parallel"
        elif section_mode == "bank":
            use_parallel = True
        else:
            use_parallel = bool(
                use_parallel_for_noc and (words <= int(ping_sec["size_words64_parallel"]))
            )
        runtime_base = int(ping_sec["local_parallel_base"] if use_parallel else ping_sec["local_linear_base"])
        tensor_mapping[tensor_name] = {
            "group_id": group_id,
            "noc_channel": channel_names[group_id],
            "ping_section": ping_name,
            "pong_section": pong_name,
            "section": primary_ping,
            "section_mode": section_mode,
            "spm_addr": runtime_base,
            "parallel_spm_addr": int(ping_sec["local_parallel_base"]),
            "linear_spm_addr": int(ping_sec["local_linear_base"]),
            "spm_mode": "parallel" if use_parallel else "linear",
            "double_buffer": (wave_count > 1),
            "size_words64": words,
            "per_wave_words64": per_wave_words,
        }

    def section_ref_to_name(section_ref: Any) -> str:
        if isinstance(section_ref, str):
            return section_ref
        if isinstance(section_ref, list):
            for item in section_ref:
                if isinstance(item, str):
                    return item
        return ""

    def section_on_group(section_ref: Any, target_group: int) -> str:
        section_name = section_ref_to_name(section_ref)
        if section_name.startswith("g") and "_" in section_name:
            _, suffix = section_name.split("_", 1)
            candidate = f"g{int(target_group)}_{suffix}"
            if candidate in section_lookup:
                return candidate
        ping_fallback = f"g{int(target_group)}_ping"
        if ping_fallback in section_lookup:
            return ping_fallback
        bank_ping_fallback = f"g{int(target_group)}_b0_ping"
        if bank_ping_fallback in section_lookup:
            return bank_ping_fallback
        return section_name

    def build_tensor_slice_addr_gen(tensor_name: str,
                                    coord: Tuple[int, int, int],
                                    words: int) -> Tuple[Optional[Dict[str, Any]], Optional[Dict[str, Any]]]:
        if not tensor_shapes or not wave_schedule:
            return None, None
        raw_shape = tensor_shapes.get(tensor_name)
        if not raw_shape or len(raw_shape) != 4:
            return None, None

        shape = [int(v) for v in raw_shape]
        wh, woc, wic = coord
        starts = [0, 0, 0, 0]
        counts = [0, 0, 0, 0]

        if tensor_name == "activation":
            n, h, w, c = shape
            h_start, h_count = wave_dim_range(h, waves_h, wh)
            c_words_total = _ceil_div_int(c, 4)
            c_start, c_count = wave_dim_range(c_words_total, waves_ic, wic)
            starts = [0, h_start, 0, c_start]
            counts = [n, h_count, w, c_count]
        elif tensor_name == "weight":
            oc, kh, kw, ic = shape
            oc_start, oc_count = wave_dim_range(oc, waves_oc, woc)
            ic_words_total = _ceil_div_int(ic, 4)
            ic_start, ic_count = wave_dim_range(ic_words_total, waves_ic, wic)
            starts = [oc_start, 0, 0, ic_start]
            counts = [oc_count, kh, kw, ic_count]
        elif tensor_name in ("partial_sum", "output"):
            n, oh, ow, oc = shape
            oh_start, oh_count = wave_dim_range(oh, waves_h, wh)
            oc_words_total = _ceil_div_int(oc, 4)
            oc_start, oc_count = wave_dim_range(oc_words_total, waves_oc, woc)
            starts = [0, oh_start, 0, oc_start]
            counts = [n, oh_count, ow, oc_count]
        else:
            return None, None

        expected_words = int(counts[0] * counts[1] * counts[2] * counts[3])
        if expected_words != int(words) or expected_words <= 0:
            return None, {
                "tensor": tensor_name,
                "wave_coord": [int(wh), int(woc), int(wic)],
                "shape": shape,
                "starts": [int(v) for v in starts],
                "counts": [int(v) for v in counts],
                "expected_words64": int(expected_words),
                "actual_words64": int(words),
                "used": False,
                "reason": "shape_words_mismatch",
            }

        addr_gen = build_packed_4d_slice_addr_gen(int(dram_mapping[tensor_name]), shape, starts, counts)
        if addr_gen is None:
            return None, {
                "tensor": tensor_name,
                "wave_coord": [int(wh), int(woc), int(wic)],
                "shape": shape,
                "starts": [int(v) for v in starts],
                "counts": [int(v) for v in counts],
                "expected_words64": int(expected_words),
                "actual_words64": int(words),
                "used": False,
                "reason": "addr_gen_build_failed",
            }

        return addr_gen, {
            "tensor": tensor_name,
            "wave_coord": [int(wh), int(woc), int(wic)],
            "shape": shape,
            "starts": [int(v) for v in starts],
            "counts": [int(v) for v in counts],
            "expected_words64": int(expected_words),
            "actual_words64": int(words),
            "used": True,
            "reason": "ok",
        }

    waves = []
    current_section_by_key: Dict[Tuple[str, int], str] = {}
    has_transfer_by_key: Dict[Tuple[str, int], bool] = {}

    def tensor_effective_group(tensor_name: str, port_to_group: Dict[str, int]) -> int:
        if tensor_name == "partial_sum":
            return int(port_to_group["PLI"])
        if tensor_name == "output":
            return int(port_to_group["PLO"])
        return int(tensor_mapping[tensor_name]["group_id"])

    def section_tag_name(section_name: str) -> str:
        if isinstance(section_name, str) and section_name.startswith("g") and "_" in section_name:
            return section_name.rsplit("_", 1)[1]
        return "ping"

    def section_bank_id(section_name: str) -> int:
        if isinstance(section_name, str):
            parts = section_name.split("_")
            if len(parts) >= 3 and parts[1].startswith("b"):
                try:
                    return int(parts[1][1:])
                except ValueError:
                    return 0
        return 0

    def section_matches_group(section_name: str, group_id: int) -> bool:
        return isinstance(section_name, str) and section_name.startswith(f"g{int(group_id)}_")

    def section_with_tag(group_id: int, tag: str) -> str:
        candidate = f"g{int(group_id)}_{str(tag)}"
        if candidate in section_lookup:
            return candidate
        bank_candidate = f"g{int(group_id)}_b0_{str(tag)}"
        if bank_candidate in section_lookup:
            return bank_candidate
        return f"g{int(group_id)}_ping"

    def section_with_bank_tag(group_id: int, bank_id: int, tag: str) -> str:
        candidate = f"g{int(group_id)}_b{int(bank_id)}_{str(tag)}"
        if candidate in section_lookup:
            return candidate
        return section_with_tag(group_id, tag)

    def assert_wave_transfer_invariants(wave_id: int,
                                        transfer: Dict[str, Any],
                                        runtime_sections: Dict[str, str],
                                        port_to_group: Dict[str, int]) -> None:
        tensor_name = str(transfer.get("tensor", ""))
        if tensor_name not in ("partial_sum", "output"):
            return

        expected_group = int(port_to_group["PLI"] if tensor_name == "partial_sum" else port_to_group["PLO"])
        actual_group = int(transfer.get("group_id", -1))
        if actual_group != expected_group:
            raise ValueError(
                f"DMA invariant violated at wave {wave_id}: tensor={tensor_name} "
                f"group_id={actual_group} expected_group={expected_group}"
            )

        transfer_section = str(transfer.get("section", ""))
        if not section_matches_group(transfer_section, expected_group):
            raise ValueError(
                f"DMA invariant violated at wave {wave_id}: tensor={tensor_name} "
                f"section={transfer_section} expected_group=g{expected_group}"
            )

        runtime_section = str(runtime_sections.get(tensor_name, ""))
        if transfer_section != runtime_section:
            transfer_mode = str(transfer.get("partition_mode", ""))
            if transfer_mode == "ultra_bank":
                runtime_phase = section_tag_name(runtime_section)
                transfer_phase = section_tag_name(transfer_section)
                if transfer_phase != runtime_phase:
                    raise ValueError(
                        f"DMA invariant violated at wave {wave_id}: tensor={tensor_name} "
                        f"runtime_phase={runtime_phase} transfer_phase={transfer_phase}"
                    )
            else:
                raise ValueError(
                    f"DMA invariant violated at wave {wave_id}: tensor={tensor_name} "
                    f"runtime_section={runtime_section} transfer_section={transfer_section}"
                )

    def split_linear_chunks(total_words: int, chunk_count: int) -> List[Tuple[int, int]]:
        sizes = split_evenly(max(0, int(total_words)), max(1, int(chunk_count)))
        out: List[Tuple[int, int]] = []
        start = 0
        for sz in sizes:
            sz_i = int(sz)
            if sz_i > 0:
                out.append((int(start), sz_i))
            start += sz_i
        return out

    def split_slice_info_on_height(slice_info: Dict[str, Any], chunk_count: int) -> List[Dict[str, Any]]:
        starts = [int(v) for v in slice_info.get("starts", [])]
        counts = [int(v) for v in slice_info.get("counts", [])]
        shape = [int(v) for v in slice_info.get("shape", [])]
        if len(starts) != 4 or len(counts) != 4 or len(shape) != 4:
            return []

        h_chunks = split_evenly(max(0, counts[1]), max(1, int(chunk_count)))
        out: List[Dict[str, Any]] = []
        h_cursor = starts[1]
        for part_idx, h_cnt in enumerate(h_chunks):
            h_cnt_i = int(h_cnt)
            if h_cnt_i <= 0:
                continue
            part_starts = list(starts)
            part_counts = list(counts)
            part_starts[1] = int(h_cursor)
            part_counts[1] = h_cnt_i
            part_words = int(part_counts[0] * part_counts[1] * part_counts[2] * part_counts[3])
            if part_words <= 0:
                h_cursor += h_cnt_i
                continue
            out.append({
                "part_idx": int(part_idx),
                "starts": [int(v) for v in part_starts],
                "counts": [int(v) for v in part_counts],
                "shape": [int(v) for v in shape],
                "words": int(part_words),
            })
            h_cursor += h_cnt_i
        return out

    def split_slice_info_on_packed_channel(slice_info: Dict[str, Any], chunk_count: int) -> List[Dict[str, Any]]:
        starts = [int(v) for v in slice_info.get("starts", [])]
        counts = [int(v) for v in slice_info.get("counts", [])]
        shape = [int(v) for v in slice_info.get("shape", [])]
        if len(starts) != 4 or len(counts) != 4 or len(shape) != 4:
            return []

        c_chunks = split_evenly(max(0, counts[3]), max(1, int(chunk_count)))
        out: List[Dict[str, Any]] = []
        c_cursor = starts[3]
        for part_idx, c_cnt in enumerate(c_chunks):
            c_cnt_i = int(c_cnt)
            if c_cnt_i <= 0:
                continue
            part_starts = list(starts)
            part_counts = list(counts)
            part_starts[3] = int(c_cursor)
            part_counts[3] = c_cnt_i
            part_words = int(part_counts[0] * part_counts[1] * part_counts[2] * part_counts[3])
            if part_words <= 0:
                c_cursor += c_cnt_i
                continue
            out.append({
                "part_idx": int(part_idx),
                "starts": [int(v) for v in part_starts],
                "counts": [int(v) for v in part_counts],
                "shape": [int(v) for v in shape],
                "words": int(part_words),
            })
            c_cursor += c_cnt_i
        return out

    ultra_bank_partition = bool(
        use_parallel_for_noc
        and tensor_shapes
        and wave_schedule
        and len(tensor_shapes.get("activation", [])) == 4
        and len(tensor_shapes.get("output", [])) == 4
    )
    ultra_partition_count = max(1, int(banks_per_group))

    src_offsets = {
        "activation": 0,
        "weight": 0,
        "partial_sum": 0,
    }
    out_offset = 0
    next_ps_phase_by_tile: Dict[Tuple[int, int], str] = {}
    last_output_phase_by_tile: Dict[Tuple[int, int], str] = {}

    for wave_id in range(wave_count):
        transfers = []
        selected_sections: Dict[str, str] = {}

        coord = wave_coords[wave_id] if wave_id < len(wave_coords) else (wave_id, 0, 0)
        wh, woc, wic = coord
        swap_pli_plo = bool(waves_ic > 1 and wic > 0 and (wic % 2 == 1))
        port_to_group = {
            "PS": channel_name_by_group["PS"],
            "PD": channel_name_by_group["PD"],
            "PLI": 3 if swap_pli_plo else 2,
            "PLO": 2 if swap_pli_plo else 3,
        }
        spm_map_val = pack_spm_map(port_to_group)
        spm_map_reason = "swap_pli_plo_for_psum_reuse" if swap_pli_plo else "default"

        for tensor_name in ["activation", "weight", "partial_sum", "output"]:
            mapping = tensor_mapping[tensor_name]
            words = int(mapping["per_wave_words64"][wave_id]) if wave_id < len(mapping["per_wave_words64"]) else 0

            if tensor_name in ("partial_sum", "output"):
                continue

            eff_group_id = tensor_effective_group(tensor_name, port_to_group)
            ping_sec = section_on_group(mapping["ping_section"], eff_group_id)
            pong_sec = section_on_group(mapping["pong_section"], eff_group_id)
            state_key = (tensor_name, eff_group_id)
            sec_name = current_section_by_key.get(state_key, ping_sec)

            if words > 0:
                if has_transfer_by_key.get(state_key, False):
                    sec_name = pong_sec if sec_name == ping_sec else ping_sec
                else:
                    has_transfer_by_key[state_key] = True

                current_section_by_key[state_key] = sec_name

            selected_sections[tensor_name] = sec_name

        tile_key = (int(wh), int(woc))
        expected_ps_phase = str(next_ps_phase_by_tile.get(tile_key, "ping"))
        ps_group_id = tensor_effective_group("partial_sum", port_to_group)
        out_group_id = tensor_effective_group("output", port_to_group)
        ps_sec_name = section_with_tag(ps_group_id, expected_ps_phase)
        # Double-buffer handoff: consume from one phase, produce to the opposite phase.
        out_phase = "pong" if expected_ps_phase == "ping" else "ping"
        out_sec_name = section_with_tag(out_group_id, out_phase)

        if wic > 0:
            prev_out_phase = last_output_phase_by_tile.get(tile_key)
            if prev_out_phase is None:
                raise ValueError(
                    f"DMA invariant violated at wave {wave_id}: missing previous output phase for tile={tile_key}"
                )
            if expected_ps_phase != prev_out_phase:
                raise ValueError(
                    f"DMA invariant violated at wave {wave_id}: partial_sum phase mismatch for tile={tile_key} "
                    f"expected={prev_out_phase} actual={expected_ps_phase}"
                )

        selected_sections["partial_sum"] = ps_sec_name
        selected_sections["output"] = out_sec_name
        last_output_phase_by_tile[tile_key] = out_phase
        next_ps_phase_by_tile[tile_key] = out_phase

        for tensor_name in ["activation", "weight", "partial_sum"]:
            mapping = tensor_mapping[tensor_name]
            words = int(mapping["per_wave_words64"][wave_id]) if wave_id < len(mapping["per_wave_words64"]) else 0
            if words <= 0:
                continue

            sec_name = selected_sections[tensor_name]
            eff_group_id = tensor_effective_group(tensor_name, port_to_group)
            sec = section_lookup[sec_name]
            src_addr_base = int(dram_mapping[tensor_name]) + src_offsets[tensor_name] * word_bytes
            section_words = int(sec.get("size_words64_linear", words))
            bank_slot_words = section_words // ultra_partition_count if ultra_partition_count > 0 else 0

            part_descs: List[Dict[str, Any]] = []
            tensor_section_mode = str(mapping.get("section_mode", "group"))
            # Bank-mode tensors use per-bank partitioned DMA descriptors.
            if ultra_bank_partition and tensor_section_mode == "bank" and bank_slot_words > 0:
                if tensor_name in ("activation", "partial_sum"):
                    wave_src_gen, wave_slice_info = build_tensor_slice_addr_gen(tensor_name, coord, words)
                    if wave_slice_info and bool(wave_slice_info.get("used", False)):
                        # GEMM activation uses a packed 4D layout [1, M, 1, K*4].
                        # Split along packed-channel (K) to match K-axis bank partition.
                        is_gemm_activation_shape = (
                            tensor_name == "activation"
                            and len(tensor_shapes.get("activation", [])) == 4
                            and int(tensor_shapes["activation"][0]) == 1
                            and int(tensor_shapes["activation"][2]) == 1
                            and int(tensor_shapes["activation"][3]) % 4 == 0
                        )
                        if is_gemm_activation_shape:
                            slice_parts = split_slice_info_on_packed_channel(wave_slice_info, ultra_partition_count)
                        else:
                            slice_parts = split_slice_info_on_height(wave_slice_info, ultra_partition_count)
                        for sp in slice_parts:
                            src_gen = build_packed_4d_slice_addr_gen(
                                int(dram_mapping[tensor_name]),
                                [int(v) for v in sp["shape"]],
                                [int(v) for v in sp["starts"]],
                                [int(v) for v in sp["counts"]],
                            )
                            if src_gen is None:
                                continue
                            part_descs.append({
                                "part_idx": int(sp["part_idx"]),
                                "words": int(sp["words"]),
                                "src_addr_gen": src_gen,
                                "src_slice_info": {
                                    "tensor": tensor_name,
                                    "wave_coord": [int(wh), int(woc), int(wic)],
                                    "shape": [int(v) for v in sp["shape"]],
                                    "starts": [int(v) for v in sp["starts"]],
                                    "counts": [int(v) for v in sp["counts"]],
                                    "expected_words64": int(sp["words"]),
                                    "actual_words64": int(sp["words"]),
                                    "used": True,
                                    "reason": "ultra_bank_height_partition",
                                },
                            })
                else:
                    for part_idx, (src_off_words, part_words) in enumerate(split_linear_chunks(words, ultra_partition_count)):
                        part_src_addr = src_addr_base + int(src_off_words) * word_bytes
                        src_gen = build_compact_linear_addr_gen(part_src_addr, part_words)
                        if src_gen is None:
                            continue
                        part_descs.append({
                            "part_idx": int(part_idx),
                            "words": int(part_words),
                            "src_addr_gen": src_gen,
                        })

            if not part_descs:
                transfer = {
                    "tensor": tensor_name,
                    "group_id": eff_group_id,
                    "section": sec_name,
                    "direction": "dram_to_spm",
                    "src_dram_addr": src_addr_base,
                    "dst_spm_addr": int(sec["global_linear_addr"]),
                    "dst_parallel_spm_addr": int(sec["global_parallel_addr"]),
                    "size_words64": words,
                }

                src_addr_gen = build_compact_linear_addr_gen(src_addr_base, words)
                if src_addr_gen is not None:
                    transfer["src_addr_gen"] = src_addr_gen
                dst_addr_gen = build_compact_linear_addr_gen(int(sec["global_linear_addr"]), words)
                if dst_addr_gen is not None:
                    transfer["dst_addr_gen"] = dst_addr_gen

                tensor_src_addr_gen, slice_info = build_tensor_slice_addr_gen(tensor_name, coord, words)
                if tensor_src_addr_gen is not None:
                    transfer["src_addr_gen"] = tensor_src_addr_gen
                if slice_info is not None:
                    transfer["src_slice_info"] = slice_info

                assert_wave_transfer_invariants(wave_id, transfer, selected_sections, port_to_group)
                transfers.append(transfer)
            else:
                for pd in part_descs:
                    part_idx = int(pd["part_idx"])
                    part_words = int(pd["words"])
                    if part_words <= 0:
                        continue
                    if part_words > bank_slot_words:
                        raise ValueError(
                            f"DMA ultra partition overflow at wave {wave_id}: tensor={tensor_name} "
                            f"part_words={part_words} bank_slot_words={bank_slot_words}"
                        )

                    part_section_name = section_with_bank_tag(eff_group_id, part_idx, section_tag_name(sec_name))
                    part_sec = section_lookup[part_section_name]
                    dst_addr = int(part_sec["global_linear_addr"])
                    transfer = {
                        "tensor": tensor_name,
                        "group_id": eff_group_id,
                        "section": part_section_name,
                        "direction": "dram_to_spm",
                        "src_dram_addr": int(pd["src_addr_gen"].get("base_addr", src_addr_base)),
                        "dst_spm_addr": int(dst_addr),
                        "dst_parallel_spm_addr": int(part_sec["global_parallel_addr"]),
                        "size_words64": int(part_words),
                        "partition_idx": part_idx,
                        "partition_count": int(ultra_partition_count),
                        "partition_mode": "ultra_bank",
                    }
                    transfer["src_addr_gen"] = pd["src_addr_gen"]
                    dst_addr_gen = build_compact_linear_addr_gen(dst_addr, part_words)
                    if dst_addr_gen is not None:
                        transfer["dst_addr_gen"] = dst_addr_gen
                    if "src_slice_info" in pd:
                        transfer["src_slice_info"] = pd["src_slice_info"]

                    assert_wave_transfer_invariants(wave_id, transfer, selected_sections, port_to_group)
                    transfers.append(transfer)

            src_offsets[tensor_name] += words

        output_mapping = tensor_mapping["output"]
        output_words = int(output_mapping["per_wave_words64"][wave_id]) if wave_id < len(output_mapping["per_wave_words64"]) else 0
        if output_words > 0:
            output_sec_name = selected_sections["output"]
            output_group_id = tensor_effective_group("output", port_to_group)
            output_sec = section_lookup[output_sec_name]
            dst_addr = int(dram_mapping["output"]) + out_offset * word_bytes
            section_words = int(output_sec.get("size_words64_linear", output_words))
            bank_slot_words = section_words // ultra_partition_count if ultra_partition_count > 0 else 0
            output_section_mode = str(output_mapping.get("section_mode", "group"))

            output_part_descs: List[Dict[str, Any]] = []
            if ultra_bank_partition and output_section_mode == "bank" and bank_slot_words > 0:
                wave_dst_gen, wave_dst_slice = build_tensor_slice_addr_gen("output", coord, output_words)
                if wave_dst_slice and bool(wave_dst_slice.get("used", False)):
                    for sp in split_slice_info_on_height(wave_dst_slice, ultra_partition_count):
                        dst_gen = build_packed_4d_slice_addr_gen(
                            int(dram_mapping["output"]),
                            [int(v) for v in sp["shape"]],
                            [int(v) for v in sp["starts"]],
                            [int(v) for v in sp["counts"]],
                        )
                        if dst_gen is None:
                            continue
                        output_part_descs.append({
                            "part_idx": int(sp["part_idx"]),
                            "words": int(sp["words"]),
                            "dst_addr_gen": dst_gen,
                            "dst_slice_info": {
                                "tensor": "output",
                                "wave_coord": [int(wh), int(woc), int(wic)],
                                "shape": [int(v) for v in sp["shape"]],
                                "starts": [int(v) for v in sp["starts"]],
                                "counts": [int(v) for v in sp["counts"]],
                                "expected_words64": int(sp["words"]),
                                "actual_words64": int(sp["words"]),
                                "used": True,
                                "reason": "ultra_bank_height_partition",
                            },
                        })

            if not output_part_descs:
                transfer = {
                    "tensor": "output",
                    "group_id": output_group_id,
                    "section": output_sec_name,
                    "direction": "spm_to_dram",
                    "src_spm_addr": int(output_sec["global_linear_addr"]),
                    "src_parallel_spm_addr": int(output_sec["global_parallel_addr"]),
                    "dst_dram_addr": dst_addr,
                    "size_words64": output_words,
                }

                src_addr_gen = build_compact_linear_addr_gen(int(output_sec["global_linear_addr"]), output_words)
                if src_addr_gen is not None:
                    transfer["src_addr_gen"] = src_addr_gen
                dst_addr_gen = build_compact_linear_addr_gen(dst_addr, output_words)
                if dst_addr_gen is not None:
                    transfer["dst_addr_gen"] = dst_addr_gen

                tensor_dst_addr_gen, dst_slice_info = build_tensor_slice_addr_gen("output", coord, output_words)
                if tensor_dst_addr_gen is not None:
                    transfer["dst_addr_gen"] = tensor_dst_addr_gen
                if dst_slice_info is not None:
                    transfer["dst_slice_info"] = dst_slice_info

                assert_wave_transfer_invariants(wave_id, transfer, selected_sections, port_to_group)
                transfers.append(transfer)
            else:
                for pd in output_part_descs:
                    part_idx = int(pd["part_idx"])
                    part_words = int(pd["words"])
                    if part_words <= 0:
                        continue
                    if part_words > bank_slot_words:
                        raise ValueError(
                            f"DMA ultra partition overflow at wave {wave_id}: tensor=output "
                            f"part_words={part_words} bank_slot_words={bank_slot_words}"
                        )

                    part_section_name = section_with_bank_tag(output_group_id, part_idx, section_tag_name(output_sec_name))
                    part_sec = section_lookup[part_section_name]
                    src_spm_addr = int(part_sec["global_linear_addr"])
                    transfer = {
                        "tensor": "output",
                        "group_id": output_group_id,
                        "section": part_section_name,
                        "direction": "spm_to_dram",
                        "src_spm_addr": int(src_spm_addr),
                        "src_parallel_spm_addr": int(part_sec["global_parallel_addr"]),
                        "dst_dram_addr": int(pd["dst_addr_gen"].get("base_addr", dst_addr)),
                        "size_words64": int(part_words),
                        "partition_idx": part_idx,
                        "partition_count": int(ultra_partition_count),
                        "partition_mode": "ultra_bank",
                    }
                    src_addr_gen = build_compact_linear_addr_gen(src_spm_addr, part_words)
                    if src_addr_gen is not None:
                        transfer["src_addr_gen"] = src_addr_gen
                    transfer["dst_addr_gen"] = pd["dst_addr_gen"]
                    if "dst_slice_info" in pd:
                        transfer["dst_slice_info"] = pd["dst_slice_info"]

                    assert_wave_transfer_invariants(wave_id, transfer, selected_sections, port_to_group)
                    transfers.append(transfer)

            out_offset += output_words
            actual_phase = section_tag_name(output_sec_name)
            expected_phase = last_output_phase_by_tile.get((int(wh), int(woc)))
            if expected_phase is not None and actual_phase != expected_phase:
                raise ValueError(
                    f"DMA invariant violated at wave {wave_id}: output phase mismatch for tile={(int(wh), int(woc))} "
                    f"expected={expected_phase} actual={actual_phase}"
                )

        runtime_sections_out: Dict[str, Any] = {}
        for tensor_name in ["activation", "weight", "partial_sum", "output"]:
            sec_name = selected_sections[tensor_name]
            mapping = tensor_mapping[tensor_name]
            if str(mapping.get("section_mode", "group")) == "bank":
                eff_group_id = tensor_effective_group(tensor_name, port_to_group)
                sec_tag = section_tag_name(sec_name)
                runtime_sections_out[tensor_name] = [
                    section_with_bank_tag(eff_group_id, b, sec_tag) for b in range(banks_per_group)
                ]
            else:
                runtime_sections_out[tensor_name] = sec_name

        waves.append({
            "wave_id": wave_id,
            "sync": {
                "compute_plan_idx": wave_id,
                "signal": "dma_wave_done",
            },
            "spm_map": {
                "map_val": int(spm_map_val),
                "port_to_group": {
                    "PS": int(port_to_group["PS"]),
                    "PD": int(port_to_group["PD"]),
                    "PLI": int(port_to_group["PLI"]),
                    "PLO": int(port_to_group["PLO"]),
                },
                "reason": spm_map_reason,
            },
            "runtime_sections": runtime_sections_out,
            "transfers": transfers,
        })

    return {
        "spm": {
            "base_addr": base_spm_addr * word_bytes,
            "addr_unit": "byte",
            "topology": {
                "num_groups": num_groups,
                "banks_per_group": banks_per_group,
                "bank_depth_words": bank_depth_words,
                "group_linear_words": group_linear_words,
                "group_parallel_base": group_parallel_base,
                "group_span_words": group_span_words,
                "group_linear_bytes": group_linear_bytes,
                "group_parallel_base_bytes": group_parallel_base_bytes,
                "group_span_bytes": group_span_bytes,
            },
            "groups": groups,
            "tensor_mapping": tensor_mapping,
        },
        "dma": {
            "engine": "tb_single_sc_process",
            "waves": waves,
        },
    }

def init_seed(seed: int):
    torch.manual_seed(seed)
    np.random.seed(seed)

def generate_conv2d_test(config: ClusterConvConfig, assembler_exe: str) -> List[ClusterTestData]:
    """
    Generate Conv2d test case based on config.
    """
    print("Generating Conv2d test data...")
    config.validate()

    # Shapes from config
    N = 1
    H, W, C = config.input_h, config.input_w, config.input_c
    OC, KH, KW = config.out_ch, config.kernel_h, config.kernel_w
    stride = config.stride
    padding = config.padding
    num_pes = config.num_pes
    num_bus = config.num_bus

    init_seed(config.seed)

    # Generate random data
    input_act = torch.randn(N, H, W, C).numpy()
    weight = torch.randn(OC, KH, KW, C).numpy()

    test_data_list = []

    # Determine split configuration
    splits = []
    if KH > num_bus:
        if KH == 5 and num_bus == 4:
             splits = [3, 2]
        else:
             # Generic split: chunks of num_bus
             remaining = KH
             while remaining > 0:
                 splits.append(min(remaining, num_bus))
                 remaining -= min(remaining, num_bus)
    else:
        splits = [KH]

    current_kh_start = 0
    previous_output = None

    # Calculate expected final output shape (based on full kernel)
    # We assume padding applies to the full operation.
    # For the split parts, we will manually slice the input to produce this exact output height.
    out_h_final = (H + 2*padding - KH) // stride + 1
    out_w_final = (W + 2*padding - KW) // stride + 1

    for idx, split_kh in enumerate(splits):
        # Slice weights
        weight_part = weight[:, current_kh_start:current_kh_start+split_kh, :, :]

        # Calculate required input height for this split to match out_h_final
        # H_in = (H_out - 1) * stride + K - 2*P_part
        # We assume P_part=0 for height as we slice the valid region.
        req_h_part = (out_h_final - 1) * stride + split_kh

        input_slice_start = current_kh_start
        input_slice_end = input_slice_start + req_h_part

        # Slice inputs
        # Note: This assumes the original input is large enough (i.e. padding=0 or handled)
        if input_slice_end > H:
             # Fallback or error handling if padding was expected to extend input
             # For the user's specific case (H=20, K=5, P=0), this works.
             print(f"Warning: Input slice [{input_slice_start}:{input_slice_end}] exceeds input height {H}")
             input_slice_end = H

        input_act_part = input_act[:, input_slice_start:input_slice_end, :, :]

        if previous_output is None:
             input_ps_part = torch.randn(N, out_h_final, out_w_final, OC).numpy() # NHWC
        else:
             input_ps_part = previous_output

        # Calculate Golden
        # We use padding=(0, padding) to disable height padding (since we sliced)
        # but keep width padding.
        output_part = golden_conv2d(input_act_part, weight_part, input_ps_part, stride=stride, padding=(0, padding))
        previous_output = output_part

        # Pack weights if needed
        weight_packed = weight_part
        if KW == 5 and C == 2:
             # Pack for k5c2
             # weight_part: (OC, split_kh, 5, 2)
             w_flat = weight_part.reshape(-1, 5, 2).transpose(0, 2, 1) # (N, 2, 5)
             packed = DataGenerator.pack_weight_mode_b(w_flat, 'channels_last') # (N, 6, 2)
             weight_packed = packed.reshape(OC, split_kh, 6, 2)
        elif KW == 7 and C == 1:
             # Pack for k7c1
             # weight_part: (OC, split_kh, 7, 1)
             w_flat = weight_part.reshape(-1, 7, 1).transpose(0, 2, 1) # (N, 1, 7)
             packed = DataGenerator.pack_weight_mode_c(w_flat, 'channels_last') # (N, 12, 1)
             weight_packed = packed.reshape(OC, split_kh, 12, 1)


        # Construct scan chain for this split
        scan_chain = []

        def get_route_mode(row_idx: int, kh: int) -> int:
            if row_idx == 0: # First row
                return PERouterMode.PLI_FROM_BUS_PLO_TO_LN
            elif row_idx == kh-1: # Last row
                return PERouterMode.PLI_FROM_LN_PLO_TO_BUS
            else: # Middle rows
                return PERouterMode.PLI_FROM_LN_PLO_TO_LN

        num_pes_per_bus = num_pes // num_bus

        # Temporal wave count split by output height, output channels, input channels
        if config.ultra_mode:
            out_h_waves = math.ceil(out_h_final / (num_pes_per_bus * num_bus))
        else:
            out_h_waves = math.ceil(out_h_final / num_pes_per_bus)

        out_ch_waves = math.ceil(OC / 16) # Assuming PE processes 16 output channels per wave
        channels_per_packet = ConvMode.channels_from_kernel_size(KW)
        in_ch_waves = math.ceil(C / channels_per_packet)
        temporal_wave_count = out_h_waves * out_ch_waves * in_ch_waves

        for i in range(num_bus):
            for j in range(num_pes_per_bus):
                if config.ultra_mode:
                    # Ultra Mode: Distribute workload across all buses
                    output_row_idx = j
                    enable = (output_row_idx < out_h_final)
                    route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS
                    pd_id = output_row_idx * stride if enable else 63
                    ps_id = 0 if enable else 63
                    pli_id = output_row_idx if enable else 63
                    plo_id = output_row_idx if enable else 63
                else: # Normal Mode: Each bus handles a chunk of the kernel height
                    enable = (i < split_kh and j < out_h_final)
                    route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS

                    if enable:
                        route_mode = get_route_mode(i, split_kh)

                    ps_id = i if enable else 63
                    pd_id = (i+j)*stride if enable else 63
                    pli_id = j if (i==0 and enable) else 63
                    plo_id = j if (i==split_kh-1 and enable) else 63

                cfg = ScanChainConfig(
                    ps_id=ps_id,
                    pd_id=pd_id,
                    pli_id=pli_id,
                    plo_id=plo_id,
                    route_mode=route_mode,
                    enable=enable
                )
                scan_chain.append(cfg)

        # Compile PE program for this split
        Path(config.out_dir).mkdir(parents=True, exist_ok=True)
        compile_pe_program(
            input_asm=Path(config.pe_program),
            assambler_exe=Path(assembler_exe),
            output_bin=Path(config.out_dir) / Path(f"pe_program.bin"),
            output_json=Path(config.out_dir) / Path(f"pe_program.json")
        )

        # Construct hardware dict
        hardware_config = {
            "num_pes": num_pes,
            "num_bus": num_bus,
            "spm_bank_size": 8192,  # Assuming 8192 words per bank as default
            "spm_groups": 4         # Default SPM groups
        }

        # Construct software dict
        dram_mapping = {
            "activation": 0x00000000,
            "weight": 0x10000000,
            "partial_sum": 0x20000000,
            "output": 0x30000000,
            "pe_program": 0x40000000
        }

        software_config = {
            "files": {
                "activation": "input_activation.bin",
                "weight": "input_weight.bin",
                "partial_sum": "input_partial_sum.bin",
                "output": "output_partial_sum.bin",
                "pe_program": "pe_program.bin"
            },
            "dram_mapping": dram_mapping,
            "wave_schedule": {
                "temporal_wave_count": temporal_wave_count,
                "temporal_wave_out_h": out_h_waves,
                "temporal_wave_out_ch": out_ch_waves,
                "temporal_wave_in_ch": in_ch_waves
            }
        }

        tensor_words64 = {
            "activation": _num_words64_from_shape(list(input_act_part.shape)),
            "weight": _num_words64_from_shape(list(weight_packed.shape)),
            "partial_sum": _num_words64_from_shape(list(input_ps_part.shape)),
            "output": _num_words64_from_shape(list(output_part.shape)),
        }
        spm_dma_config = _build_spm_dma_plan(
            dram_mapping,
            tensor_words64,
            temporal_wave_count,
            tensor_shapes={
                "activation": list(input_act_part.shape),
                "weight": list(weight_packed.shape),
                "partial_sum": list(input_ps_part.shape),
                "output": list(output_part.shape),
            },
            wave_schedule=software_config["wave_schedule"],
            use_parallel_for_noc=bool(config.ultra_mode),
            tensor_mbus_shared=(
                {
                    "weight": True,
                    "activation": False,
                    "partial_sum": False,
                    "output": False,
                }
                if bool(config.ultra_mode)
                else None
            ),
        )
        software_config["spm"] = spm_dma_config["spm"]
        software_config["dma"] = spm_dma_config["dma"]
        runtime_addr_per_wave = _build_runtime_addr_per_wave(software_config["spm"], software_config["dma"])

        spm_tensor_mapping = software_config["spm"]["tensor_mapping"]
        addr_weight_run = int(spm_tensor_mapping["weight"]["spm_addr"])
        addr_act_run = int(spm_tensor_mapping["activation"]["spm_addr"])
        addr_psum_run = int(spm_tensor_mapping["partial_sum"]["spm_addr"])
        addr_out_run = int(spm_tensor_mapping["output"]["spm_addr"])

        agu_by_tensor = {
            "weight": "agu_ps",
            "activation": "agu_pd",
            "partial_sum": "agu_pli",
            "output": "agu_plo",
        }
        tensor_shared = {
            "weight": bool(spm_tensor_mapping.get("weight", {}).get("section_mode") == "group" and spm_tensor_mapping.get("weight", {}).get("spm_mode") == "linear"),
            "activation": bool(spm_tensor_mapping.get("activation", {}).get("section_mode") == "group" and spm_tensor_mapping.get("activation", {}).get("spm_mode") == "linear"),
            "partial_sum": bool(spm_tensor_mapping.get("partial_sum", {}).get("section_mode") == "group" and spm_tensor_mapping.get("partial_sum", {}).get("spm_mode") == "linear"),
            "output": bool(spm_tensor_mapping.get("output", {}).get("section_mode") == "group" and spm_tensor_mapping.get("output", {}).get("spm_mode") == "linear"),
        }
        agu_ultra_overrides: Dict[str, bool] = {}
        for tensor_name, shared in tensor_shared.items():
            if shared:
                agu_ultra_overrides[agu_by_tensor[tensor_name]] = False

        cluster_plans = _compile_cluster_plans_conv2d(
            {
                "kernel_size": [split_kh, KW],
                "stride": stride,
                "ultra_mode": bool(config.ultra_mode),
                "agu_ultra_overrides": agu_ultra_overrides,
                "tensor_shapes": {
                    "activation": list(input_act_part.shape),
                    "output": list(output_part.shape),
                },
            },
            software_config["wave_schedule"],
            {
                "weight": addr_weight_run,
                "activation": addr_act_run,
                "partial_sum": addr_psum_run,
                "output": addr_out_run,
            },
            runtime_addr_per_wave=runtime_addr_per_wave,
        )
        software_config["cluster_plans"] = cluster_plans

        name_suffix = f"_part{idx+1}" if len(splits) > 1 else ""

        # Test config for documentation and debugging
        test_config = {
            "meta": {
                "name": f"conv2d_custom{name_suffix}",
                "mode": "conv2d",
                "ultra_mode": config.ultra_mode,
                "seed": config.seed,
                "tensor_shapes": {
                    "activation": list(input_act_part.shape),
                    "weight": list(weight_packed.shape),
                    "partial_sum": list(input_ps_part.shape),
                    "output": list(output_part.shape)
                },
                "kernel_size": [split_kh, KW],
                "stride": stride,
                "padding": padding
            },
            "hardware": hardware_config,
            "software": software_config
        }

        test_data_list.append(ClusterTestData(
            name=f"conv2d_custom{name_suffix}",
            description=f"Conv2d {split_kh}x{KW} (Split {idx+1}/{len(splits)})",
            inputs={
                "activation": input_act_part,
                "weight": weight_packed,
                "partial_sum": input_ps_part
            },
            outputs={
                "partial_sum": output_part
            },
            scan_chain=scan_chain,
            config=test_config
        ))

        current_kh_start += split_kh

    return test_data_list

def generate_gemm_test(config: ClusterGemmConfig, assembler_exe: str = "ha-asm") -> ClusterTestData:
    """
    Generate GEMM test case based on config.
    C = A * B + D (Input PS)
    Mapping strategy:
    - Tile the M, N dimensions onto a grid of PEs.
    - Split K dimension across Buses for spatial accumulation (NoC vertical accumulation).
    """
    print("Generating GEMM test data with K-axis NoC accumulation...")
    config.validate()

    M, N, K = config.M, config.N, config.K
    num_pes = config.num_pes # Hardware Total PEs (e.g., 64)
    num_bus = config.num_bus # Hardware Buses (e.g., 3)

    # PE Capability
    PE_M, PE_N = 12, 8
    PE_K = 32 # PE processes 32 K-dim per step/pass

    # Calculate Grid Size
    grid_m = (M + PE_M - 1) // PE_M
    grid_n = (N + PE_N - 1) // PE_N
    grid_k = (K + PE_K - 1) // PE_K  # K-splits

    print(f"Grid Layout: M={grid_m}, N={grid_n}, K_split={grid_k}")

    # Total PEs needed: M_grid * N_grid * K_grid
    active_pes_count = grid_m * grid_n * grid_k

    # Calculate Temporal Waves if hardware resources are insufficient
    pes_per_bus = num_pes // num_bus
    pes_per_layer = grid_m * grid_n # PEs needed for one K-slice (one bus)

    # Choose M/N tile shape per wave to fit PE budget
    def choose_mn_tiles(grid_m: int, grid_n: int, pe_budget: int):
        best = None
        for m_tiles in range(min(grid_m, pe_budget), 0, -1):
            max_n = min(grid_n, pe_budget // m_tiles)
            for n_tiles in range(max_n, 0, -1):
                waves_m = math.ceil(grid_m / m_tiles)
                waves_n = math.ceil(grid_n / n_tiles)
                waves = waves_m * waves_n
                area = m_tiles * n_tiles
                aspect = abs((grid_m / max(grid_n, 1)) - (m_tiles / max(n_tiles, 1)))
                score = (waves, -n_tiles, -m_tiles, -area, aspect)
                if best is None or score < best[0]:
                    best = (score, m_tiles, n_tiles, waves_m, waves_n)
        if best is None:
            return 1, 1, grid_m, grid_n
        _, m_tiles, n_tiles, waves_m, waves_n = best
        return m_tiles, n_tiles, waves_m, waves_n

    m_tiles_per_wave, n_tiles_per_wave, wave_m, wave_n = choose_mn_tiles(grid_m, grid_n, pes_per_bus)

    def split_tiles(total_tiles: int, waves: int) -> List[int]:
        if waves <= 0:
            return []
        base = total_tiles // waves
        rem = total_tiles % waves
        tiles = [base + 1 if i < rem else base for i in range(waves)]
        return tiles

    # Number of waves needed for K-dimension (if K-splits > Buses)
    k_tiles_per_wave = num_bus if num_bus > 0 else 1
    wave_k = math.ceil(grid_k / k_tiles_per_wave)

    grid_m_per_wave = split_tiles(grid_m, wave_m)
    grid_n_per_wave = split_tiles(grid_n, wave_n)
    grid_k_per_wave = split_tiles(grid_k, wave_k)

    temporal_wave_count = wave_m * wave_n * wave_k

    # We map K-splits to Buses.
    # Requirement: We need at least grid_k buses to chain them vertically efficiently.
    # (Or complex folding, but assuming 1-to-1 mapping for this test)
    if grid_k > num_bus:
        print(f"Warning: K-split ({grid_k}) > Num Buses ({num_bus}). Accumulation chain might not fit simply.")
        # We proceed but data might be truncated or wrap-around logic is needed.
        # For this specific user request (K=96/32=3, Bus=3), it fits perfectly.

    torch.manual_seed(config.seed)
    np.random.seed(config.seed)

    # Generate random data
    A = torch.randn(M, K).numpy()
    B = torch.randn(K, N).numpy()
    D = torch.randn(M, N).numpy() # Input PS

    # Calculate GEMM
    C = golden_gemm(A, B, D)

    # Rename keys to match Conv2D filenames (activation, weight, partial_sum)
    # This ensures test_noc_sim.cpp loads them correctly.
    inputs = {
        "activation": A,
        "weight": B,
        "partial_sum": D
    }
    outputs = {
        "partial_sum": C
    }

    # --- Scan Chain Construction ---
    scan_chain = []

    # Calculate physical layout
    # We assign Bus `b` to handle `K-split = b`.
    # Inside Bus, we place the (M, N) grid.

    pes_per_bus = num_pes // num_bus

    def get_route_mode(k_idx: int, k_total: int) -> int:
        # Chain flow: Bus 0 (Start) -> ... -> Bus N (End)
        if k_idx == 0:
            # First stage: Read from BUS (or zero), output to Neighbor (Next Stage)
            return PERouterMode.PLI_FROM_BUS_PLO_TO_LN
        elif k_idx == k_total - 1:
            # Last stage: Read from Neighbor, Accumulate, output to BUS (Final Memory)
            return PERouterMode.PLI_FROM_LN_PLO_TO_BUS
        else:
            # Middle stage: Read from Neighbor, Accumulate, output to Neighbor
            return PERouterMode.PLI_FROM_LN_PLO_TO_LN

    for b in range(num_bus):
        # Current K-slice index
        k_idx = b

        # Check if this bus is part of the active K-chain
        is_active_k_layer = (k_idx < grid_k)

        # Determine Routing Mode for this layer
        r_mode = get_route_mode(k_idx, grid_k) if is_active_k_layer else PERouterMode.PLI_FROM_BUS_PLO_TO_BUS

        for j in range(pes_per_bus):
            # Map j to (m, n) within this layer
            # Simple Row-Major mapping of the MxN grid
            # Capability per Bus: pes_per_bus
            # Required: grid_m * grid_n

            m_idx = j // grid_n
            n_idx = j % grid_n

            is_active_pe = is_active_k_layer and (m_idx < grid_m)

            if is_active_pe:
                # Active PE
                # ps_id: Tiled Weight (B_kn) -> Shared by PEs with same (k, n)
                # pd_id: Tiled Input Act (A_mk) -> Shared by PEs with same (k, m)
                # pli_id: PS Input (D_mn) -> Only for first bus (k=0), Shared by (m, n)
                # plo_id: PS Output (C_mn) -> Only for last bus (k=last), Shared by (m, n)

                if config.ultra_mode:
                    # Ultra Mode: Reuse tags across buses
                    ps_id = n_idx
                    pd_id = m_idx
                else:
                    # Normal Mode
                    # ps_id (Weight B)
                    ps_id = k_idx * grid_n + n_idx
                    # pd_id (Input A)
                    pd_id = k_idx * grid_m + m_idx

                # pli_id (PS Input) - used only if route_mode reads from BUS
                pli_id = (m_idx * grid_n + n_idx) if k_idx == 0 else 63

                # plo_id (PS Output) - used only if route_mode writes to BUS
                plo_id = (m_idx * grid_n + n_idx) if k_idx == grid_k - 1 else 63

                enable = True
                route_mode = r_mode
            else:
                # Inactive PE
                ps_id, pd_id, pli_id, plo_id = 63, 63, 63, 63
                enable = False
                route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS # Default/Passthrough

            cfg = ScanChainConfig(
                ps_id=ps_id,
                pd_id=pd_id,
                pli_id=pli_id,
                plo_id=plo_id,
                route_mode=route_mode,
                enable=enable
            )
            scan_chain.append(cfg)

    print(f"GEMM K-Split Scan-Chain Generated.")
    print(f"  Mapping: K-split {grid_k} layers mapped to first {grid_k} buses.")
    print(f"  Temporal Waves: {temporal_wave_count} (M waves: {wave_m}, N waves: {wave_n}, K waves: {wave_k})")
    print(f"  Wave Tile Size: M={m_tiles_per_wave}, N={n_tiles_per_wave}, K={k_tiles_per_wave}")
    print(f"  Per-wave tiles: M={grid_m_per_wave}, N={grid_n_per_wave}, K={grid_k_per_wave}")

    Path(config.out_dir).mkdir(parents=True, exist_ok=True)
    compile_pe_program(
        input_asm=Path(config.pe_program),
        assambler_exe=Path(assembler_exe),
        output_bin=Path(config.out_dir) / Path("pe_program.bin"),
        output_json=Path(config.out_dir) / Path("pe_program.json"),
    )

    hardware_config = {
        "num_pes": num_pes,
        "num_bus": num_bus,
        "spm_bank_size": 8192,  # Assuming 8192 words per bank
        "spm_groups": 4,
    }

    dram_mapping = {
        "activation": 0x00000000,
        "weight": 0x10000000,
        "partial_sum": 0x20000000,
        "output": 0x30000000,
        "pe_program": 0x40000000,
    }

    software_config = {
        "files": {
            "activation": "input_activation.bin",
            "weight": "input_weight.bin",
            "partial_sum": "input_partial_sum.bin",
            "output": "output_partial_sum.bin",
            "pe_program": "pe_program.bin",
        },
        "dram_mapping": dram_mapping,
        "wave_schedule": {
            "temporal_wave_count": temporal_wave_count,
            "temporal_wave_out_h": 1,
            "temporal_wave_out_ch": wave_n,
            "temporal_wave_in_ch": wave_k,
        },
    }

    # GEMM addressing policy aligned with test_noc_sim dataflow:
    # 1) non-ultra: all tensors use linear/group sections.
    # 2) ultra + single K wave (no K-split): all tensors use parallel/bank sections.
    # 3) ultra + K-split (>1): weight/activation use parallel/bank, partial_sum/output use linear/group.
    if not bool(config.ultra_mode):
        gemm_tensor_section_mode = {
            "weight": "group",
            "activation": "group",
            "partial_sum": "group",
            "output": "group",
        }
        gemm_tensor_spm_mode = {
            "weight": "linear",
            "activation": "linear",
            "partial_sum": "linear",
            "output": "linear",
        }
    elif grid_k > 1:
        gemm_tensor_section_mode = {
            "weight": "bank",
            "activation": "bank",
            "partial_sum": "group",
            "output": "group",
        }
        gemm_tensor_spm_mode = {
            "weight": "parallel",
            "activation": "parallel",
            "partial_sum": "linear",
            "output": "linear",
        }
    else:
        gemm_tensor_section_mode = {
            "weight": "bank",
            "activation": "bank",
            "partial_sum": "bank",
            "output": "bank",
        }
        gemm_tensor_spm_mode = {
            "weight": "parallel",
            "activation": "parallel",
            "partial_sum": "parallel",
            "output": "parallel",
        }

    tensor_words64 = {
        "activation": _num_words64_from_shape(list(A.shape)),
        "weight": _num_words64_from_shape(list(B.shape)),
        "partial_sum": _num_words64_from_shape(list(D.shape)),
        "output": _num_words64_from_shape(list(C.shape)),
    }
    spm_dma_config = _build_spm_dma_plan(
        dram_mapping,
        tensor_words64,
        temporal_wave_count,
        tensor_shapes={
            # GEMM tensors are represented in packed 4D for DMA slicing, where
            # dim3 is fp16 elements and dim3_words = ceil(dim3/4) is the 64-bit word axis.
            "activation": [1, M, 1, K * 4],
            "weight": [1, K, 1, N * 4],
            "partial_sum": [1, M, 1, N * 4],
            "output": [1, M, 1, N * 4],
        },
        wave_schedule=software_config["wave_schedule"],
        use_parallel_for_noc=bool(config.ultra_mode),
        tensor_section_mode=gemm_tensor_section_mode,
        tensor_spm_mode=gemm_tensor_spm_mode,
    )
    software_config["spm"] = spm_dma_config["spm"]
    software_config["dma"] = spm_dma_config["dma"]
    runtime_addr_per_wave = _build_runtime_addr_per_wave(software_config["spm"], software_config["dma"])

    spm_tensor_mapping = software_config["spm"]["tensor_mapping"]
    addr_weight_run = int(spm_tensor_mapping["weight"]["spm_addr"])
    addr_act_run = int(spm_tensor_mapping["activation"]["spm_addr"])
    addr_psum_run = int(spm_tensor_mapping["partial_sum"]["spm_addr"])
    addr_out_run = int(spm_tensor_mapping["output"]["spm_addr"])

    meta_config = {
        "name": f"gemm_{M}x{N}x{K}",
        "mode": "gemm",
        "M": M,
        "N": N,
        "K": K,
        "seed": config.seed,
        "grid_m": grid_m,
        "grid_n": grid_n,
        "grid_k": grid_k,
        "wave_m": wave_m,
        "wave_n": wave_n,
        "wave_k": wave_k,
        "grid_m_per_wave": grid_m_per_wave,
        "grid_n_per_wave": grid_n_per_wave,
        "grid_k_per_wave": grid_k_per_wave,
        "ultra_mode": bool(config.ultra_mode),
        # AGU ultra-mode behavior can differ from global ultra_mode for GEMM K-split.
        # In ultra K-split mode, PLO requests are standard (non-ultra) in test_noc_sim.
        "agu_ultra_overrides": (
            {"agu_plo": False}
            if bool(config.ultra_mode) and grid_k > 1
            else {}
        ),
        "tensor_shapes": {
            "activation": [M, K],
            "weight": [K, N],
            "partial_sum": [M, N],
            "output": [M, N],
        },
    }

    software_config["cluster_plans"] = _compile_cluster_plans_gemm(
        meta_config,
        {
            "weight": addr_weight_run,
            "activation": addr_act_run,
            "partial_sum": addr_psum_run,
            "output": addr_out_run,
        },
        runtime_addr_per_wave=runtime_addr_per_wave,
    )

    test_config = {
        "meta": meta_config,
        "hardware": hardware_config,
        "software": software_config,
    }

    return ClusterTestData(
        name=f"gemm_{M}x{N}x{K}",
        description=f"GEMM M={M}, N={N}, K={K}, K-Split",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config=test_config,
    )
