"""Stage 1: Operator Lowering — WorkloadIR → HardwareIR.

Implements tiling, SPM layout, AGU config, scan-chain, PE param calculation,
cluster mapping, and TilingParams generation for each operator type.
"""

from __future__ import annotations

import math
from typing import Dict, List, Optional, Tuple

from .ir import (
    AguBankConfig,
    ClusterMapping,
    HardwareDesc,
    HardwareIR,
    HdduConfig,
    LayerHwConfig,
    OpDesc,
    PeProgramRef,
    ScanChainEntry,
    SpmGroupLayout,
    SpmPerGroupLayout,
    TilingParams,
    TilingResult,
    WorkloadIR,
)

PKT_SIZE = 8  # bytes per SPM transaction (64-bit)


class TilingFailed(Exception):
    pass


# ===================================================================
# SPM Constants
# ===================================================================

_SPM_MAP_EVEN = 0xE4  # port0→g0, port1→g1, port2→g2, port3→g3
_SPM_MAP_ODD = 0xD8   # port0→g0, port1→g1, port2→g3, port3→g2


# ===================================================================
# Scan-chain computation (shared by all operators)
# ===================================================================

def compute_scan_chain(num_pes: int, num_bus: int) -> List[ScanChainEntry]:
    """Compute scan-chain entries for PE array.

    Convention:
    - First PE on each bus: PLI_FROM_BUS (mode 1)
    - Last PE on each bus: PLO_TO_BUS (mode 2)
    - Middle PEs: PLI_FROM_LN_PLO_TO_LN (mode 0)

    Entries returned in forward order. Firmware sends in reverse.
    """
    entries: List[ScanChainEntry] = []
    pes_per_bus = num_pes // num_bus

    for bus_idx in range(num_bus):
        for pe_local in range(pes_per_bus):
            gid = bus_idx * pes_per_bus + pe_local
            ps_id = gid
            pd_id = gid

            if pe_local == 0:
                route_mode = 1  # PLI_FROM_BUS_PLO_TO_LN
                pli_id = bus_idx
                plo_id = gid
            elif pe_local == pes_per_bus - 1:
                route_mode = 2  # PLI_FROM_LN_PLO_TO_BUS
                pli_id = gid
                plo_id = bus_idx
            else:
                route_mode = 0  # PLI_FROM_LN_PLO_TO_LN
                pli_id = gid
                plo_id = gid

            entries.append(ScanChainEntry(
                ps_id=ps_id, pd_id=pd_id,
                pli_id=pli_id, plo_id=plo_id,
                route_mode=route_mode, enable=True,
            ))

    return entries


# ===================================================================
# Cluster mapping
# ===================================================================

def compute_cluster_mapping(
    num_clusters: int,
    tiling: TilingResult,
    op_type: str,
) -> ClusterMapping:
    if op_type in ("conv2d_3x3", "conv2d_1x1"):
        oc_tiles = tiling.loop_bounds.get("oc_tile", 1)
        spatial_tiles = (
            tiling.loop_bounds.get("h_tile", 1) *
            tiling.loop_bounds.get("w_tile", 1)
        )
        if oc_tiles >= num_clusters:
            split_dim = "oc_tile"
            total = oc_tiles
            shared = "input"
        elif spatial_tiles >= num_clusters:
            split_dim = "spatial"
            total = spatial_tiles
            shared = "weight"
        else:
            if oc_tiles >= spatial_tiles:
                split_dim = "oc_tile"
                total = oc_tiles
                shared = "input"
            else:
                split_dim = "spatial"
                total = spatial_tiles
                shared = "weight"
    elif op_type == "gemm":
        n_tiles = tiling.loop_bounds.get("n_tile", 1)
        m_tiles = tiling.loop_bounds.get("m_tile", 1)
        if n_tiles >= m_tiles:
            split_dim = "n_tile"
            total = n_tiles
            shared = "A"
        else:
            split_dim = "m_tile"
            total = m_tiles
            shared = "B"
    else:
        total = 1
        split_dim = None
        shared = None

    active = min(num_clusters, total)
    tiles_per = math.ceil(total / active) if active > 0 else 0
    mask = (1 << active) - 1

    return ClusterMapping(
        active_clusters=active,
        cluster_mask=mask,
        split_dim=split_dim,
        tiles_per_cluster=tiles_per,
        shared_tensor=shared,
    )


# ===================================================================
# Conv2D 3×3 Lowering
# ===================================================================

def _lower_conv2d_3x3(op: OpDesc, hw: HardwareDesc) -> LayerHwConfig:
    act = op.inputs[0]
    wt = op.inputs[1]
    out = op.outputs[0]

    N, H_in, W_in, C_in = act.shape
    OC, KH, KW, _ = wt.shape
    _, H_out, W_out, _ = out.shape

    stride = op.attrs.get("stride", 1)
    padding = op.attrs.get("padding", 0)

    # Fixed tile dims
    tile_ic = 4
    tile_oc = min(OC, 16)
    in_ch_pack = tile_ic // 4  # = 1
    out_ch_pack = tile_oc // 4

    half_cap = hw.half_group_capacity
    halo = KH - 1  # = 2

    # -- Tiling search --
    num_ic_tiles = C_in // tile_ic
    num_oc_tiles = math.ceil(OC / tile_oc)

    ps_wave = tile_oc * KH * KW * in_ch_pack * PKT_SIZE
    if ps_wave > half_cap:
        raise TilingFailed(f"{op.name}: weight tile {ps_wave}B > half_cap {half_cap}B")

    tile_h_out = tile_w_out = None
    for th in range(H_out, 0, -1):
        th_in = th + halo
        for tw in range(W_out, 0, -1):
            tw_in = tw + halo
            pd = th_in * tw_in * in_ch_pack * PKT_SIZE
            pli = th * tw * out_ch_pack * PKT_SIZE
            if ps_wave <= half_cap and pd <= half_cap and pli <= half_cap:
                tile_h_out = th
                tile_w_out = tw
                break
        if tile_h_out is not None:
            break

    if tile_h_out is None:
        raise TilingFailed(f"{op.name}: cannot tile conv2d_3x3 to fit half_cap")

    tile_h_in = tile_h_out + halo
    tile_w_in = tile_w_out + halo
    num_h_tiles = math.ceil(H_out / tile_h_out)
    num_w_tiles = math.ceil(W_out / tile_w_out)

    pd_wave = tile_h_in * tile_w_in * in_ch_pack * PKT_SIZE
    pli_wave = tile_h_out * tile_w_out * out_ch_pack * PKT_SIZE
    total_waves = num_oc_tiles * num_h_tiles * num_w_tiles * num_ic_tiles

    # -- TilingResult --
    tiling = TilingResult(
        loop_dims=["oc_tile", "h_tile", "w_tile", "ic_tile"],
        loop_bounds={
            "oc_tile": num_oc_tiles,
            "h_tile": num_h_tiles,
            "w_tile": num_w_tiles,
            "ic_tile": num_ic_tiles,
        },
        total_waves=total_waves,
        reduction_dims=["ic_tile"],
    )

    # -- SPM Layout (all linear, forced ping-pong) --
    spm_layout = SpmPerGroupLayout(
        ps=SpmGroupLayout(ping_base=0, pong_base=half_cap, size=ps_wave, spm_mode="linear"),
        pd=SpmGroupLayout(ping_base=0, pong_base=half_cap, size=pd_wave, spm_mode="linear"),
        pli=SpmGroupLayout(ping_base=0, pong_base=half_cap, size=pli_wave, spm_mode="linear"),
        plo=SpmGroupLayout(ping_base=0, pong_base=half_cap, size=pli_wave, spm_mode="linear"),
    )

    # -- AGU configs (ping base; runtime swaps to pong) --
    agu_ps = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=in_ch_pack, iter1=KW, iter2=KH, iter3=tile_oc,
        stride0=PKT_SIZE,
        stride1=in_ch_pack * PKT_SIZE,
        stride2=KW * in_ch_pack * PKT_SIZE,
        stride3=KH * KW * in_ch_pack * PKT_SIZE,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    agu_pd = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=in_ch_pack, iter1=tile_h_in, iter2=tile_w_in, iter3=1,
        stride0=PKT_SIZE,
        stride1=tile_w_in * in_ch_pack * PKT_SIZE,
        stride2=in_ch_pack * PKT_SIZE,
        stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    agu_pli = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h_out, iter2=tile_w_out, iter3=1,
        stride0=PKT_SIZE,
        stride1=tile_w_out * out_ch_pack * PKT_SIZE,
        stride2=out_ch_pack * PKT_SIZE,
        stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    agu_plo = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h_out, iter2=tile_w_out, iter3=1,
        stride0=PKT_SIZE,
        stride1=tile_w_out * out_ch_pack * PKT_SIZE,
        stride2=out_ch_pack * PKT_SIZE,
        stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )

    # -- PE template params --
    vectors_per_kernel = KH * in_ch_pack  # = 3
    pe_params = {
        "KERNEL_DMA_LEN": tile_oc * vectors_per_kernel,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": tile_h_out * tile_w_out - 1,
        "KERNEL_COUNT": tile_oc,
        "KERNEL_LOOP_INNER": 1,
        "KERNEL_LOOP_OUTER": 1,
    }
    pe_prog = PeProgramRef(template_name="conv1d_k3c4s1_template", params=pe_params)

    # -- HDDU --
    hddu = HdduConfig(plane_en=0xF, plane_mode=0x1)

    # -- Scan-chain --
    scan_chain = compute_scan_chain(hw.num_pes, hw.num_bus)

    # -- Cluster mapping --
    cluster_map = compute_cluster_mapping(hw.num_clusters, tiling, op.op_type)

    # -- TilingParams (compile-time constants for runtime) --
    dram_base = hw.dram_base
    # Weight layout: [OC, KH, KW, C_in] contiguous in DRAM
    wt_tile_bytes = tile_oc * KH * KW * tile_ic * 2  # fp16
    act_tile_h_bytes = tile_h_in * W_in * tile_ic * 2
    out_tile_bytes = tile_h_out * W_out * tile_oc * 2

    # DRAM stride calculations
    dram_ps_oc_stride = tile_oc * KH * KW * C_in * 2
    dram_ps_ic_stride = tile_ic * 2  # IC is innermost in weight
    dram_pd_h_stride = tile_h_out * stride * W_in * C_in * 2
    dram_pd_w_stride = tile_w_out * stride * C_in * 2
    dram_pd_ic_stride = tile_ic * 2
    dram_out_oc_stride = tile_oc * 2
    dram_out_h_stride = tile_h_out * W_out * OC * 2
    dram_out_w_stride = tile_w_out * OC * 2

    tiling_params = TilingParams(
        num_oc_tiles=num_oc_tiles,
        num_h_tiles=num_h_tiles,
        num_w_tiles=num_w_tiles,
        num_ic_tiles=num_ic_tiles,
        spm_ping=[0, 0, 0, 0],
        spm_pong=[half_cap, half_cap, half_cap, half_cap],
        agu_ping=[0, 0, 0, 0],
        agu_pong=[half_cap, half_cap, half_cap, half_cap],
        spm_map_even=_SPM_MAP_EVEN,
        spm_map_odd=_SPM_MAP_ODD,
        dram_weight_base=dram_base,
        dram_input_base=dram_base + OC * KH * KW * C_in * 2,  # after weights
        dram_output_base=dram_base + OC * KH * KW * C_in * 2 + N * H_in * W_in * C_in * 2,
        dram_ps_oc_stride=dram_ps_oc_stride,
        dram_ps_ic_stride=dram_ps_ic_stride,
        dram_pd_h_stride=dram_pd_h_stride,
        dram_pd_w_stride=dram_pd_w_stride,
        dram_pd_ic_stride=dram_pd_ic_stride,
        dram_out_oc_stride=dram_out_oc_stride,
        dram_out_h_stride=dram_out_h_stride,
        dram_out_w_stride=dram_out_w_stride,
        dma_ps_words=ps_wave // PKT_SIZE,
        dma_pd_words=pd_wave // PKT_SIZE,
        dma_plo_words=pli_wave // PKT_SIZE,
        ps_reuse_across_spatial=True,  # weight depends on (oc, ic) only
        bank_depth_bytes=hw.bank_depth_bytes,
        parallel_groups=0x0,  # no parallel groups for 3x3
        dma_ps_words_per_bank=0,
        dma_plo_words_per_bank=0,
    )

    return LayerHwConfig(
        name=op.name,
        op_type=op.op_type,
        target_cluster_mask=cluster_map.cluster_mask,
        spm_config_map=_SPM_MAP_EVEN,
        hddu=hddu,
        agu_ps=agu_ps, agu_pd=agu_pd, agu_pli=agu_pli, agu_plo=agu_plo,
        scan_chain=scan_chain,
        pe_program=pe_prog,
        spm_layout=spm_layout,
        tiling=tiling,
        cluster_mapping=cluster_map,
        tiling_params=tiling_params,
    )


# ===================================================================
# Conv2D 1×1 Lowering
# ===================================================================

def _lower_conv2d_1x1(op: OpDesc, hw: HardwareDesc) -> LayerHwConfig:
    act = op.inputs[0]
    wt = op.inputs[1]
    out = op.outputs[0]

    N, H_in, W_in, C_in = act.shape
    OC, KH, KW, _ = wt.shape
    _, H_out, W_out, _ = out.shape

    tile_ic = 12
    tile_oc = min(OC, 16)
    in_ch_pack = tile_ic // 12  # = 1
    out_ch_pack = tile_oc // 4

    half_cap = hw.half_group_capacity
    pp = hw.parallel_ping_base
    ppong = hw.parallel_pong_base

    num_ic_tiles = C_in // tile_ic
    num_oc_tiles = math.ceil(OC / tile_oc)

    ps_wave = tile_oc * in_ch_pack * PKT_SIZE
    if ps_wave > half_cap:
        raise TilingFailed(f"{op.name}: weight tile exceeds half_cap")

    tile_h = tile_w = None
    for th in range(H_in, 0, -1):
        for tw in range(W_in, 0, -1):
            pd = th * tw * in_ch_pack * PKT_SIZE
            pli = th * tw * out_ch_pack * PKT_SIZE
            if pd <= half_cap and pli <= half_cap and ps_wave <= half_cap:
                tile_h = th
                tile_w = tw
                break
        if tile_h is not None:
            break

    if tile_h is None:
        raise TilingFailed(f"{op.name}: cannot tile conv2d_1x1")

    num_h_tiles = math.ceil(H_out / tile_h)
    num_w_tiles = math.ceil(W_out / tile_w)

    pd_wave = tile_h * tile_w * in_ch_pack * PKT_SIZE
    pli_wave = tile_h * tile_w * out_ch_pack * PKT_SIZE
    total_waves = num_oc_tiles * num_h_tiles * num_w_tiles * num_ic_tiles

    tiling = TilingResult(
        loop_dims=["oc_tile", "h_tile", "w_tile", "ic_tile"],
        loop_bounds={
            "oc_tile": num_oc_tiles, "h_tile": num_h_tiles,
            "w_tile": num_w_tiles, "ic_tile": num_ic_tiles,
        },
        total_waves=total_waves,
        reduction_dims=["ic_tile"],
    )

    # SPM layout: PS/PLI/PLO = Parallel, PD = Linear
    spm_layout = SpmPerGroupLayout(
        ps=SpmGroupLayout(ping_base=pp, pong_base=ppong, size=ps_wave, spm_mode="parallel"),
        pd=SpmGroupLayout(ping_base=0, pong_base=half_cap, size=pd_wave, spm_mode="linear"),
        pli=SpmGroupLayout(ping_base=pp, pong_base=ppong, size=pli_wave, spm_mode="parallel"),
        plo=SpmGroupLayout(ping_base=pp, pong_base=ppong, size=pli_wave, spm_mode="parallel"),
    )

    # AGU - PS: Parallel/Ultra
    agu_ps = AguBankConfig(
        base_addr=pp, base_addr_h=0,
        iter0=in_ch_pack, iter1=1, iter2=1, iter3=tile_oc,
        stride0=PKT_SIZE, stride1=0, stride2=0,
        stride3=in_ch_pack * PKT_SIZE,
        ctrl=0x8, lane_cfg=0,  # ultra mode
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=0,
        mask_cfg=0xF,
    )
    # AGU - PD: Linear/Normal
    agu_pd = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=in_ch_pack, iter1=tile_h, iter2=tile_w, iter3=1,
        stride0=PKT_SIZE,
        stride1=tile_w * in_ch_pack * PKT_SIZE,
        stride2=in_ch_pack * PKT_SIZE,
        stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    # AGU - PLI: Parallel/Ultra
    agu_pli = AguBankConfig(
        base_addr=pp, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h, iter2=tile_w, iter3=1,
        stride0=PKT_SIZE,
        stride1=tile_w * out_ch_pack * PKT_SIZE,
        stride2=out_ch_pack * PKT_SIZE,
        stride3=0,
        ctrl=0x8, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    # AGU - PLO: Parallel/Ultra
    agu_plo = AguBankConfig(
        base_addr=pp, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h, iter2=tile_w, iter3=1,
        stride0=PKT_SIZE,
        stride1=tile_w * out_ch_pack * PKT_SIZE,
        stride2=out_ch_pack * PKT_SIZE,
        stride3=0,
        ctrl=0x8, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )

    pe_params = {
        "KERNEL_DMA_LEN": tile_oc * in_ch_pack,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": tile_h * tile_w - 1,
        "KERNEL_COUNT": tile_oc,
        "KERNEL_LOOP_INNER": 1,
        "KERNEL_LOOP_OUTER": 1,
    }
    pe_prog = PeProgramRef(template_name="conv1d_k1c12s1_template", params=pe_params)
    hddu = HdduConfig(plane_en=0xF, plane_mode=0x1)
    scan_chain = compute_scan_chain(hw.num_pes, hw.num_bus)
    cluster_map = compute_cluster_mapping(hw.num_clusters, tiling, op.op_type)

    stride_v = op.attrs.get("stride", 1)
    dram_base = hw.dram_base
    dram_ps_oc_stride = tile_oc * C_in * 2
    dram_ps_ic_stride = tile_ic * 2
    dram_pd_h_stride = tile_h * stride_v * W_in * C_in * 2
    dram_pd_w_stride = tile_w * stride_v * C_in * 2
    dram_pd_ic_stride = tile_ic * 2
    dram_out_oc_stride = tile_oc * 2
    dram_out_h_stride = tile_h * W_out * OC * 2
    dram_out_w_stride = tile_w * OC * 2

    bdb = hw.bank_depth_bytes

    tiling_params = TilingParams(
        num_oc_tiles=num_oc_tiles, num_h_tiles=num_h_tiles,
        num_w_tiles=num_w_tiles, num_ic_tiles=num_ic_tiles,
        spm_ping=[pp, 0, pp, pp],
        spm_pong=[ppong, half_cap, ppong, ppong],
        agu_ping=[pp, 0, pp, pp],
        agu_pong=[ppong, half_cap, ppong, ppong],
        spm_map_even=_SPM_MAP_EVEN, spm_map_odd=_SPM_MAP_ODD,
        dram_weight_base=dram_base,
        dram_input_base=dram_base + OC * C_in * 2,
        dram_output_base=dram_base + OC * C_in * 2 + N * H_in * W_in * C_in * 2,
        dram_ps_oc_stride=dram_ps_oc_stride,
        dram_ps_ic_stride=dram_ps_ic_stride,
        dram_pd_h_stride=dram_pd_h_stride,
        dram_pd_w_stride=dram_pd_w_stride,
        dram_pd_ic_stride=dram_pd_ic_stride,
        dram_out_oc_stride=dram_out_oc_stride,
        dram_out_h_stride=dram_out_h_stride,
        dram_out_w_stride=dram_out_w_stride,
        dma_ps_words=ps_wave // PKT_SIZE,
        dma_pd_words=pd_wave // PKT_SIZE,
        dma_plo_words=pli_wave // PKT_SIZE,
        ps_reuse_across_spatial=True,
        bank_depth_bytes=bdb,
        parallel_groups=0xD,  # PS, PLI, PLO (groups 0, 2, 3)
        dma_ps_words_per_bank=ps_wave // PKT_SIZE,  # per-bank for parallel DMA
        dma_plo_words_per_bank=pli_wave // PKT_SIZE,
    )

    return LayerHwConfig(
        name=op.name, op_type=op.op_type,
        target_cluster_mask=cluster_map.cluster_mask,
        spm_config_map=_SPM_MAP_EVEN,
        hddu=hddu,
        agu_ps=agu_ps, agu_pd=agu_pd, agu_pli=agu_pli, agu_plo=agu_plo,
        scan_chain=scan_chain, pe_program=pe_prog,
        spm_layout=spm_layout, tiling=tiling,
        cluster_mapping=cluster_map, tiling_params=tiling_params,
    )


# ===================================================================
# GEMM Lowering
# ===================================================================

def _lower_gemm(op: OpDesc, hw: HardwareDesc) -> LayerHwConfig:
    A = op.inputs[0]
    B = op.inputs[1]
    C = op.outputs[0]

    M, K = A.shape
    _, N = B.shape
    ep = 4  # elements per packet

    half_cap = hw.half_group_capacity
    pp = hw.parallel_ping_base
    ppong = hw.parallel_pong_base

    # Tiling search
    M_tile = N_tile = K_tile = None
    for mt in range(min(M, 32), 0, -8):
        if mt <= 0:
            continue
        for nt in range(min(N, 48), 0, -12):
            if nt <= 0:
                continue
            for kt in range(min(K, 96), 0, -32):
                if kt <= 0:
                    continue
                ps = (mt * kt // ep) * PKT_SIZE
                pd = (kt * nt // ep) * PKT_SIZE
                pli = (mt * nt // ep) * PKT_SIZE
                if ps <= half_cap and pd <= half_cap and pli <= half_cap:
                    M_tile, N_tile, K_tile = mt, nt, kt
                    break
            if M_tile is not None:
                break
        if M_tile is not None:
            break

    if M_tile is None:
        raise TilingFailed(f"{op.name}: cannot tile GEMM")

    num_m_tiles = math.ceil(M / M_tile)
    num_n_tiles = math.ceil(N / N_tile)
    num_k_tiles = math.ceil(K / K_tile)

    ps_wave = (M_tile * K_tile // ep) * PKT_SIZE
    pd_wave = (K_tile * N_tile // ep) * PKT_SIZE
    pli_wave = (M_tile * N_tile // ep) * PKT_SIZE
    total_waves = num_n_tiles * num_m_tiles * num_k_tiles

    # Map GEMM dims to generic 4D loop: oc→M, h→N, w=1, ic→K
    tiling = TilingResult(
        loop_dims=["n_tile", "m_tile", "k_tile"],
        loop_bounds={
            "oc_tile": num_m_tiles,  # M mapped to oc
            "h_tile": num_n_tiles,   # N mapped to h
            "w_tile": 1,
            "ic_tile": num_k_tiles,  # K mapped to ic
            "n_tile": num_n_tiles,
            "m_tile": num_m_tiles,
            "k_tile": num_k_tiles,
        },
        total_waves=total_waves,
        reduction_dims=["k_tile"],
    )

    spm_layout = SpmPerGroupLayout(
        ps=SpmGroupLayout(ping_base=pp, pong_base=ppong, size=ps_wave, spm_mode="parallel"),
        pd=SpmGroupLayout(ping_base=0, pong_base=half_cap, size=pd_wave, spm_mode="linear"),
        pli=SpmGroupLayout(ping_base=pp, pong_base=ppong, size=pli_wave, spm_mode="parallel"),
        plo=SpmGroupLayout(ping_base=pp, pong_base=ppong, size=pli_wave, spm_mode="parallel"),
    )

    # AGU PS: Parallel/Ultra — A tile [M_tile, K_tile]
    agu_ps = AguBankConfig(
        base_addr=pp, base_addr_h=0,
        iter0=M_tile // ep, iter1=K_tile // ep,
        iter2=1, iter3=1,
        stride0=PKT_SIZE,
        stride1=(M_tile // ep) * PKT_SIZE,
        stride2=0, stride3=0,
        ctrl=0x8, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    # AGU PD: Linear/Normal — B tile [K_tile, N_tile]
    agu_pd = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=K_tile // ep, iter1=N_tile // ep,
        iter2=1, iter3=1,
        stride0=PKT_SIZE,
        stride1=(K_tile // ep) * PKT_SIZE,
        stride2=0, stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    # AGU PLI: Parallel/Ultra — C tile [M_tile, N_tile]
    agu_pli = AguBankConfig(
        base_addr=pp, base_addr_h=0,
        iter0=M_tile // ep, iter1=N_tile // ep,
        iter2=1, iter3=1,
        stride0=PKT_SIZE,
        stride1=(M_tile // ep) * PKT_SIZE,
        stride2=0, stride3=0,
        ctrl=0x8, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )
    # AGU PLO: Parallel/Ultra
    agu_plo = AguBankConfig(
        base_addr=pp, base_addr_h=0,
        iter0=M_tile // ep, iter1=N_tile // ep,
        iter2=1, iter3=1,
        stride0=PKT_SIZE,
        stride1=(M_tile // ep) * PKT_SIZE,
        stride2=0, stride3=0,
        ctrl=0x8, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=0, tag_ctrl=1,
        mask_cfg=0xF,
    )

    pe_params = {
        "KERNEL_DMA_STORE_LEN": M_tile * N_tile // ep,
        "KERNEL_DMA_LOAD_LEN": K_tile * max(N_tile, M_tile) // ep,
        "INPUT_DIM": K_tile,
        "OUTPUT_DIM": M_tile,
        "PSUM_COUNT": M_tile * N_tile // ep,
        "NUM_OF_KERNEL_SETS": num_k_tiles,
        "NUM_OF_N_TILES": num_n_tiles,
        "NUM_OF_M_TILES": num_m_tiles,
        "K_TILE_DIM": K_tile,
    }
    pe_prog = PeProgramRef(template_name="gemm_template", params=pe_params)
    hddu = HdduConfig(plane_en=0xB, plane_mode=0x2)
    scan_chain = compute_scan_chain(hw.num_pes, hw.num_bus)
    cluster_map = compute_cluster_mapping(hw.num_clusters, tiling, op.op_type)

    dram_base = hw.dram_base
    # A[M, K] contiguous, B[K, N] after A, C[M, N] after B
    a_bytes = M * K * 2
    b_bytes = K * N * 2
    bdb = hw.bank_depth_bytes

    tiling_params = TilingParams(
        num_oc_tiles=num_m_tiles,
        num_h_tiles=num_n_tiles,
        num_w_tiles=1,
        num_ic_tiles=num_k_tiles,
        spm_ping=[pp, 0, pp, pp],
        spm_pong=[ppong, half_cap, ppong, ppong],
        agu_ping=[pp, 0, pp, pp],
        agu_pong=[ppong, half_cap, ppong, ppong],
        spm_map_even=_SPM_MAP_EVEN, spm_map_odd=_SPM_MAP_ODD,
        dram_weight_base=dram_base,           # A matrix
        dram_input_base=dram_base + a_bytes,  # B matrix
        dram_output_base=dram_base + a_bytes + b_bytes,  # C matrix
        dram_ps_oc_stride=M_tile * K * 2,     # stride in M dim
        dram_ps_ic_stride=K_tile * 2,         # stride in K dim
        dram_pd_h_stride=K * N_tile * 2,      # B: stride in N dim (row of columns)
        dram_pd_w_stride=0,
        dram_pd_ic_stride=K_tile * N * 2,     # B: stride in K dim
        dram_out_oc_stride=M_tile * N * 2,    # C: stride in M dim
        dram_out_h_stride=N_tile * 2,         # C: stride in N dim
        dram_out_w_stride=0,
        dma_ps_words=ps_wave // PKT_SIZE,
        dma_pd_words=pd_wave // PKT_SIZE,
        dma_plo_words=pli_wave // PKT_SIZE,
        ps_reuse_across_spatial=False,
        bank_depth_bytes=bdb,
        parallel_groups=0xD,  # PS, PLI, PLO
        dma_ps_words_per_bank=ps_wave // (PKT_SIZE * 3) if ps_wave > 0 else 0,
        dma_plo_words_per_bank=pli_wave // (PKT_SIZE * 3) if pli_wave > 0 else 0,
    )

    return LayerHwConfig(
        name=op.name, op_type=op.op_type,
        target_cluster_mask=cluster_map.cluster_mask,
        spm_config_map=_SPM_MAP_EVEN,
        hddu=hddu,
        agu_ps=agu_ps, agu_pd=agu_pd, agu_pli=agu_pli, agu_plo=agu_plo,
        scan_chain=scan_chain, pe_program=pe_prog,
        spm_layout=spm_layout, tiling=tiling,
        cluster_mapping=cluster_map, tiling_params=tiling_params,
    )


# ===================================================================
# Public API
# ===================================================================

_LOWERING_DISPATCH = {
    "conv2d_3x3": _lower_conv2d_3x3,
    "conv2d_1x1": _lower_conv2d_1x1,
    "gemm": _lower_gemm,
}


def lower_workload(wir: WorkloadIR) -> HardwareIR:
    """Lower all ops in a WorkloadIR to produce a HardwareIR."""
    layers: List[LayerHwConfig] = []
    for op in wir.ops:
        fn = _LOWERING_DISPATCH.get(op.op_type)
        if fn is None:
            raise ValueError(f"Unsupported op type: {op.op_type}")
        layers.append(fn(op, wir.hardware))
    return HardwareIR(
        workload_name=wir.name,
        hardware=wir.hardware,
        layers=layers,
    )
