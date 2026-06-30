"""Stage 1: Operator Lowering — WorkloadIR → HardwareIR.

Implements tiling, SPM layout, AGU config, scan-chain, PE param calculation,
cluster mapping, and TilingParams generation for each operator type.
"""

from __future__ import annotations

import math
from typing import Dict, List, Optional, Tuple

from .frontend import CompilationError
from .ir import (
    AguBankConfig,
    ClusterMapping,
    HardwareDesc,
    HardwareIR,
    HdduConfig,
    LayerHwConfig,
    OpDesc,
    PeProgramRef,
    RelayoutDesc,
    ScanChainEntry,
    SpmGroupLayout,
    SpmPerGroupLayout,
    TilingParams,
    TilingResult,
    WorkloadIR,
)

PKT_SIZE = 8   # bytes per SPM transaction (64-bit)
WORD64 = 8     # bytes per word64 (AGU stride/base_addr unit)
DRAM_BOOT_RESERVED = 0x100000  # 1 MB reserved for manifest + ELF payload

_DMA_FILL_ZERO = 0
_DMA_FILL_EPSILON = 1
_DMA_EPILOGUE_NONE = 0
_DMA_EPILOGUE_RELU = 1


class TilingFailed(Exception):
    pass


# ---------------------------------------------------------------------------
# Lowering-stage validators
# ---------------------------------------------------------------------------

# op_type -> per-plane expected SPM mode.
_SPM_MODE_EXPECTED: Dict[str, Dict[str, str]] = {
    "conv2d_3x3": {
        "ps":  "linear",
        "pd":  "linear",
        "pli": "linear",
        "plo": "linear",
    },
}


def _has_duplicate_enabled_ids(scan_chain: List[ScanChainEntry], field_name: str) -> bool:
    seen = set()
    for entry in scan_chain:
        if not entry.enable:
            continue
        value = getattr(entry, field_name)
        if value in seen:
            return True
        seen.add(value)
    return False


def _conv1x1_multi_bus(layer: LayerHwConfig) -> bool:
    return _has_duplicate_enabled_ids(layer.scan_chain, "pli_id")


def _conv1x1_resident_oc_tiles(layer: LayerHwConfig) -> int:
    return max(1, layer.tiling_params.conv1x1_resident_oc_tiles)


def _choose_conv1x1_resident_oc_tiles(num_oc_tiles: int, num_bus: int) -> int:
    upper = min(num_oc_tiles, num_bus)
    for candidate in range(upper, 1, -1):
        if num_oc_tiles % candidate == 0:
            return candidate
    return 1


def _coerce_bool_attr(value: object, op_name: str, attr_name: str) -> bool:
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
    raise TilingFailed(
        f"{op_name}: attr '{attr_name}' must be a bool-like value, got {value!r}"
    )


def _resolve_output_epilogue(op: OpDesc) -> int:
    attrs = op.attrs
    activation = attrs.get("activation")
    if activation is None:
        activation = attrs.get("epilogue")
    if activation is not None:
        if not isinstance(activation, str):
            raise TilingFailed(
                f"{op.name}: attr 'activation' must be a string, got {type(activation).__name__}"
            )
        activation_l = activation.strip().lower()
        if activation_l in ("", "none", "identity"):
            return _DMA_EPILOGUE_NONE
        if activation_l == "relu":
            return _DMA_EPILOGUE_RELU
        raise TilingFailed(
            f"{op.name}: unsupported activation '{activation}'"
        )

    for attr_name in ("relu", "fuse_relu", "output_relu"):
        if attr_name in attrs and _coerce_bool_attr(attrs[attr_name], op.name, attr_name):
            return _DMA_EPILOGUE_RELU
    return _DMA_EPILOGUE_NONE


def _expected_spm_modes(layer: LayerHwConfig) -> Dict[str, str]:
    if layer.op_type == "conv2d_1x1":
        resident_oc_tiles = _conv1x1_resident_oc_tiles(layer)
        if resident_oc_tiles > 1:
            return {
                "ps": "parallel",
                "pd": "linear",
                "pli": "parallel",
                "plo": "parallel",
            }
        multi_bus = _conv1x1_multi_bus(layer)
        return {
            "ps": "linear",
            "pd": "parallel" if multi_bus else "linear",
            "pli": "parallel" if multi_bus else "linear",
            "plo": "parallel" if multi_bus else "linear",
        }
    if layer.op_type == "gemm":
        grid_k_per_wave = layer.pe_program.params.get("GRID_K_PER_WAVE", 1)
        use_ultra = grid_k_per_wave > 1
        return {
            "ps": "parallel" if use_ultra else "linear",
            "pd": "parallel" if use_ultra else "linear",
            "pli": "linear",
            "plo": "linear",
        }

    return _SPM_MODE_EXPECTED.get(layer.op_type, {})


def _expected_agu_ultra(layer: LayerHwConfig) -> Dict[str, bool]:
    if layer.op_type == "conv2d_3x3":
        use_ultra = False
    elif layer.op_type == "conv2d_1x1":
        resident_oc_tiles = _conv1x1_resident_oc_tiles(layer)
        if resident_oc_tiles > 1:
            return {
                "ps": True,
                "pd": False,
                "pli": True,
                "plo": True,
            }
        use_ultra = _conv1x1_multi_bus(layer)
        return {
            "ps": False,
            "pd": use_ultra,
            "pli": use_ultra,
            "plo": use_ultra,
        }
    elif layer.op_type == "gemm":
        use_ultra = layer.pe_program.params.get("GRID_K_PER_WAVE", 1) > 1
    else:
        return {}

    return {
        "ps": use_ultra,
        "pd": use_ultra,
        "pli": False,
        "plo": False,
    }


def _assert_spm_agu_consistency(layer: LayerHwConfig) -> None:
    """E009: SPM mode and AGU ultra mode must match the lowered transport path.

    Conv2D 3x3 is always linear/normal. Conv2D 1x1 infers multi-bus ultra
    transport from scan-chain ID reuse. GEMM K-chain keeps PS/PD on the
    ultra/parallel path, but PLI/PLO stay linear/non-ultra because reduction
    partial sums flow through the PE-local accumulation path rather than a
    per-bus ultra broadcast.

    Raises CompilationError with code E009 on mismatch.
    """
    expected_modes = _expected_spm_modes(layer)
    expected_ultra = _expected_agu_ultra(layer)
    if expected_modes is None or not expected_ultra:
        return  # unknown op type — leave to other validators

    planes = (
        ("ps",  layer.spm_layout.ps,  layer.agu_ps),
        ("pd",  layer.spm_layout.pd,  layer.agu_pd),
        ("pli", layer.spm_layout.pli, layer.agu_pli),
        ("plo", layer.spm_layout.plo, layer.agu_plo),
    )
    # Planes disabled in HDDU (plane_en bit 0) cannot run their AGU and thus
    # have no observable SPM/AGU coupling — skip them.
    plane_en = layer.hddu.plane_en
    plane_bit = {"ps": 0, "pd": 1, "pli": 2, "plo": 3}
    mismatches: List[str] = []
    for name, spm, agu in planes:
        if ((plane_en >> plane_bit[name]) & 0x1) == 0:
            continue
        want_mode = expected_modes[name]
        want_ultra = expected_ultra[name]
        got_mode = spm.spm_mode
        got_ultra = bool(agu.ctrl & 0x8)
        if got_mode != want_mode or got_ultra != want_ultra:
            mismatches.append(
                f"{name.upper()}: spm_mode={got_mode}/agu_ultra={got_ultra}"
                f" (want spm_mode={want_mode}/agu_ultra={want_ultra})"
            )
    if mismatches:
        raise CompilationError(
            "validation",
            layer.name,
            "E009: SPM/AGU mode mismatch ("
            + f"op_type={layer.op_type}; " + "; ".join(mismatches)
            + ")",
        )


# ===================================================================
# SPM Constants
# ===================================================================

_SPM_MAP_EVEN = 0xE4  # port0→g0, port1→g1, port2→g2, port3→g3
_SPM_MAP_ODD = 0xB4   # port0→g0, port1→g1, port2→g3, port3→g2  (swap PLI↔PLO only)


# ===================================================================
# Scan-chain computation — operator-specific
# ===================================================================

def compute_scan_chain_conv2d(
    num_pes: int,
    num_bus: int,
    kernel_height: int,
    out_h: int,
    stride: int = 1,
) -> List[ScanChainEntry]:
    """Compute scan-chain entries for Conv2D PE array (normal mode).

    Semantic mapping:
    - Each bus handles one kernel-height row (bus_idx = kernel row).
    - Each PE within a bus handles one output row.
    - ps_id = bus_idx  (weight broadcast per bus)
    - pd_id = (bus_idx + pe_local) * stride  (unique input row per PE)
    - PLI: only bus 0 reads from bus (IB->OL)
    - PLO: only last active bus writes to bus (IL->OB)
    - Middle buses: IL->OL (pass through LN chain)

    Entries returned in forward order. Firmware sends in reverse.
    """
    entries: List[ScanChainEntry] = []
    pes_per_bus = num_pes // num_bus

    for bus_idx in range(num_bus):
        for pe_local in range(pes_per_bus):
            active = (bus_idx < kernel_height and pe_local < out_h)

            if active:
                ps_id = bus_idx
                pd_id = (bus_idx + pe_local) * stride

                if kernel_height == 1:
                    # Single bus: read from bus, write to bus
                    route_mode = 3  # PLI_FROM_BUS_PLO_TO_BUS
                    pli_id = pe_local
                    plo_id = pe_local
                elif bus_idx == 0:
                    route_mode = 1  # PLI_FROM_BUS_PLO_TO_LN
                    pli_id = pe_local
                    plo_id = 63
                elif bus_idx == kernel_height - 1:
                    route_mode = 2  # PLI_FROM_LN_PLO_TO_BUS
                    pli_id = 63
                    plo_id = pe_local
                else:
                    route_mode = 0  # PLI_FROM_LN_PLO_TO_LN
                    pli_id = 63
                    plo_id = 63
            else:
                ps_id = 63
                pd_id = 63
                pli_id = 63
                plo_id = 63
                route_mode = 3  # PLI_FROM_BUS_PLO_TO_BUS (passthrough)

            entries.append(ScanChainEntry(
                ps_id=ps_id, pd_id=pd_id,
                pli_id=pli_id, plo_id=plo_id,
                route_mode=route_mode, enable=active,
            ))

    return entries


def compute_scan_chain_ultra(
    num_pes: int,
    num_bus: int,
    out_h: int,
    stride: int = 1,
) -> List[ScanChainEntry]:
    """Compute scan-chain for ultra/GEMM mode (PLI_FROM_BUS_PLO_TO_BUS).

    All active PEs read PLI from bus and write PLO to bus.
    - ps_id = 0  (weight shared across all PEs via ultra broadcast)
    - pd_id = pe_local * stride
    - pli_id = pe_local
    - plo_id = pe_local
    """
    entries: List[ScanChainEntry] = []
    pes_per_bus = num_pes // num_bus

    for bus_idx in range(num_bus):
        for pe_local in range(pes_per_bus):
            active = pe_local < out_h

            if active:
                ps_id = 0
                pd_id = pe_local * stride
                pli_id = pe_local
                plo_id = pe_local
                route_mode = 3  # PLI_FROM_BUS_PLO_TO_BUS
            else:
                ps_id = 63
                pd_id = 63
                pli_id = 63
                plo_id = 63
                route_mode = 3

            entries.append(ScanChainEntry(
                ps_id=ps_id, pd_id=pd_id,
                pli_id=pli_id, plo_id=plo_id,
                route_mode=route_mode, enable=active,
            ))

    return entries


def compute_scan_chain_conv1x1(
    num_pes: int,
    num_bus: int,
    tile_h_per_bus: int,
    active_buses: int,
    stride: int = 1,
) -> List[ScanChainEntry]:
    """Conv2D 1×1 scan-chain (H-lane + W-time model).

    Each active PE owns one output H row. PLI/PLO always go through MBUS
    (route_mode=3 == PLI_FROM_BUS_PLO_TO_BUS) regardless of normal vs
    ultra path, because conv1x1 has no kernel-height accumulation across
    rows: each row is an independent reduction over IC.

    - normal path: only bus 0 active (active_buses=1, single H-stripe).
    - ultra path: first `active_buses` buses active in parallel, each
      handling its own H-stripe of `tile_h_per_bus` rows.

    For both paths, single-row case (tile_h_per_bus * active_buses == 1)
    naturally lands on (IB, OB) per the open-issue-1 resolution.
    """
    entries: List[ScanChainEntry] = []
    pes_per_bus = num_pes // num_bus

    for bus_idx in range(num_bus):
        bus_active = bus_idx < active_buses
        for pe_local in range(pes_per_bus):
            active = bus_active and pe_local < tile_h_per_bus

            if active:
                ps_id = 0
                pd_id = pe_local * stride
                pli_id = pe_local
                plo_id = pe_local
                route_mode = 3  # PLI_FROM_BUS_PLO_TO_BUS (IB, OB)
            else:
                ps_id = 63
                pd_id = 63
                pli_id = 63
                plo_id = 63
                route_mode = 3

            entries.append(ScanChainEntry(
                ps_id=ps_id, pd_id=pd_id,
                pli_id=pli_id, plo_id=plo_id,
                route_mode=route_mode, enable=active,
            ))

    return entries


def compute_scan_chain_gemm(
    num_pes: int,
    num_bus: int,
    grid_m: int,
    grid_n: int,
    grid_k: int,
    use_ultra: bool,
) -> List[ScanChainEntry]:
    """GEMM K-chain scan-chain aligned with noc_gen.generate_gemm_test.

    Each bus represents one K-stage. Within a bus, PE j maps to
    (m_idx = j // grid_n, n_idx = j % grid_n).

    Route mode (single-row resolution: grid_k==1 → (IB, OB)):
    - first stage = 1 (IB, OL)
    - middle      = 0 (IL, OL)
    - last        = 2 (IL, OB)
    - single (grid_k==1) = 3 (IB, OB)

    ID rules:
    - ultra: ps_id = n_idx, pd_id = m_idx (tags reused across buses)
    - normal: ps_id = k_idx*grid_n + n_idx, pd_id = k_idx*grid_m + m_idx
    - pli_id valid only on first stage (or single)
    - plo_id valid only on last stage (or single)
    """
    entries: List[ScanChainEntry] = []
    pes_per_bus = num_pes // num_bus

    def route_mode(k_idx: int, k_total: int) -> int:
        if k_total == 1:
            return 3  # PLI_FROM_BUS_PLO_TO_BUS
        if k_idx == 0:
            return 1  # PLI_FROM_BUS_PLO_TO_LN
        if k_idx == k_total - 1:
            return 2  # PLI_FROM_LN_PLO_TO_BUS
        return 0      # PLI_FROM_LN_PLO_TO_LN

    for bus_idx in range(num_bus):
        k_idx = bus_idx
        bus_active_k = k_idx < grid_k
        rmode = route_mode(k_idx, grid_k) if bus_active_k else 3

        for pe_local in range(pes_per_bus):
            if not bus_active_k:
                entries.append(ScanChainEntry(
                    ps_id=63, pd_id=63, pli_id=63, plo_id=63,
                    route_mode=rmode, enable=False,
                ))
                continue

            m_idx = pe_local // grid_n
            n_idx = pe_local % grid_n
            active = (m_idx < grid_m)

            if active:
                if use_ultra:
                    ps_id = n_idx
                    pd_id = m_idx
                else:
                    ps_id = k_idx * grid_n + n_idx
                    pd_id = k_idx * grid_m + m_idx
                pli_id = (m_idx * grid_n + n_idx) if k_idx == 0 else 63
                plo_id = (m_idx * grid_n + n_idx) if k_idx == grid_k - 1 else 63
                entries.append(ScanChainEntry(
                    ps_id=ps_id, pd_id=pd_id,
                    pli_id=pli_id, plo_id=plo_id,
                    route_mode=rmode, enable=True,
                ))
            else:
                entries.append(ScanChainEntry(
                    ps_id=63, pd_id=63, pli_id=63, plo_id=63,
                    route_mode=rmode, enable=False,
                ))

    return entries


def _pes_per_bus(hw: HardwareDesc) -> int:
    """Return the number of output lanes available per bus."""
    return hw.num_pes // hw.num_bus


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

def _lower_conv2d_3x3(op: OpDesc, hw: HardwareDesc,
                      tensor_base: int = 0,
                      dram_input_override: int = 0,
                      output_ic_tiled: bool = False,
                      input_nhwc_packs: int = 0,
                      output_pad: int = 0,
                      use_runtime_load_pad: bool = False) -> LayerHwConfig:
    act = op.inputs[0]
    wt = op.inputs[1]
    out = op.outputs[0]

    N, H_in, W_in, C_in = act.shape
    OC, KH, KW, _ = wt.shape
    _, H_out, W_out, _ = out.shape

    stride = op.attrs.get("stride", 1)
    padding = op.attrs.get("padding", 0)

    if padding < 0:
        raise TilingFailed(
            f"{op.name}: conv2d_3x3 padding must be >= 0, got {padding}"
        )

    if padding > 0 and not use_runtime_load_pad:
        raise TilingFailed(
            f"{op.name}: padded conv2d_3x3 requires runtime DMA load-pad"
        )
    if output_pad > 0:
        raise TilingFailed(
            f"{op.name}: padded output canvas path is no longer supported"
        )

    # Fixed tile dims
    tile_ic = 4
    tile_oc = min(OC, 16)
    in_ch_pack = tile_ic // 4  # = 1
    out_ch_pack = tile_oc // 4

    half_cap = hw.half_group_capacity
    halo = KH - 1  # = 2
    pes_per_bus = _pes_per_bus(hw)

    # -- Tiling search --
    num_ic_tiles = C_in // tile_ic
    num_oc_tiles = math.ceil(OC / tile_oc)

    ps_wave = tile_oc * KH * KW * in_ch_pack * PKT_SIZE
    if ps_wave > half_cap:
        raise TilingFailed(f"{op.name}: weight tile {ps_wave}B > half_cap {half_cap}B")

    tile_h_out = tile_w_out = None
    # Conv2D normal mode maps one output row per PE within a bus.
    # Any tile taller than pes_per_bus would leave rows with no consumer.
    max_tile_h_out = min(H_out, pes_per_bus)
    max_tile_w_out = W_out
    for th in range(max_tile_h_out, 0, -1):
        th_in = th + halo
        for tw in range(max_tile_w_out, 0, -1):
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
    last_h_out = H_out - (num_h_tiles - 1) * tile_h_out
    last_w_out = W_out - (num_w_tiles - 1) * tile_w_out

    pd_wave = tile_h_in * tile_w_in * in_ch_pack * PKT_SIZE
    pli_wave = tile_h_out * tile_w_out * out_ch_pack * PKT_SIZE
    total_waves = num_oc_tiles * num_h_tiles * num_w_tiles * num_ic_tiles

    # NHWC-input mode: when reading from a previous layer's NHWC output
    # and this layer has multi-IC tiles, DMA loads the full NHWC data and
    # the PD AGU uses larger strides to select the correct IC channels.
    pd_ic_agu_offset = 0
    if input_nhwc_packs > 0 and num_ic_tiles > 1:
        pd_wave = tile_h_in * tile_w_in * input_nhwc_packs * PKT_SIZE
        pd_ic_agu_offset = 1  # one word offset per IC tile

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

    # -- AGU configs — all addresses/strides in word64 units --
    agu_ps = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=in_ch_pack, iter1=KW, iter2=KH, iter3=tile_oc,
        stride0=1,
        stride1=in_ch_pack,
        stride2=KW * in_ch_pack,
        stride3=KH * KW * in_ch_pack,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=2,
        mask_cfg=0xF,
    )

    # PD AGU: when input_nhwc_packs > 0, stride over full OC packs per position
    if input_nhwc_packs > 0 and num_ic_tiles > 1:
        pd_stride0 = 1
        pd_stride1 = tile_w_in * input_nhwc_packs
        pd_stride2 = input_nhwc_packs
    else:
        pd_stride0 = 1
        pd_stride1 = tile_w_in * in_ch_pack
        pd_stride2 = in_ch_pack
    agu_pd = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=in_ch_pack, iter1=tile_h_in, iter2=tile_w_in, iter3=1,
        stride0=pd_stride0,
        stride1=pd_stride1,
        stride2=pd_stride2,
        stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=1,
        mask_cfg=0xF,
    )
    # -- PLI/PLO stride selection --
    # IC-tiled: [out_ch_pack, tile_h, tile_w] — contiguous per IC tile
    # NHWC:     [tile_h, tile_w, out_ch_pack] — channels packed contiguously
    if output_ic_tiled:
        plo_s0 = tile_h_out * tile_w_out   # IC tile stride (dim0 = ch pack)
        plo_s1 = tile_w_out                 # row stride within IC tile
        plo_s2 = 1                          # column stride
    else:
        plo_s0 = 1                          # adjacent packs
        plo_s1 = tile_w_out * out_ch_pack   # row stride
        plo_s2 = out_ch_pack                # column stride

    agu_pli = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h_out, iter2=tile_w_out, iter3=1,
        stride0=plo_s0,
        stride1=plo_s1,
        stride2=plo_s2,
        stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=1,
        mask_cfg=0xF,
    )
    agu_plo = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h_out, iter2=tile_w_out, iter3=1,
        stride0=plo_s0,
        stride1=plo_s1,
        stride2=plo_s2,
        stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=1,
        mask_cfg=0xF,
    )

    # -- PE template params --
    vectors_per_kernel = KH * in_ch_pack  # = 3
    pe_params = {
        "KERNEL_DMA_LEN": tile_oc * vectors_per_kernel,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": tile_w_out - 1,
        "KERNEL_COUNT": tile_oc,
        "KERNEL_LOOP_INNER": num_ic_tiles,
        "KERNEL_LOOP_OUTER": num_oc_tiles * num_h_tiles * num_w_tiles,
    }
    pe_prog = PeProgramRef(template_name="conv1d_k3c4s1_template", params=pe_params)

    # -- HDDU --
    hddu = HdduConfig(plane_en=0xF, plane_mode=0x1)

    # -- Scan-chain --
    scan_chain = compute_scan_chain_conv2d(
        hw.num_pes, hw.num_bus,
        kernel_height=KH,
        out_h=tile_h_out,
        stride=stride,
    )

    # -- Cluster mapping --
    cluster_map = compute_cluster_mapping(hw.num_clusters, tiling, op.op_type)

    # -- TilingParams (compile-time constants for runtime) --
    dram_base = hw.dram_base
    if tensor_base == 0:
        tensor_base = dram_base + DRAM_BOOT_RESERVED

    # DRAM stride calculations
    # DRAM stores IC-tiled layout: weight [num_oc_tiles, num_ic_tiles, tile_oc, KH, KW, tile_ic]
    #                               input  [N, num_ic_tiles, H_in, W_in, tile_ic]
    dram_ps_oc_stride = num_ic_tiles * tile_oc * KH * KW * tile_ic * 2
    dram_ps_ic_stride = tile_oc * KH * KW * tile_ic * 2
    # NHWC-input mode: DRAM input is [N, H, W, C_in] (not IC-tiled).
    if input_nhwc_packs > 0 and num_ic_tiles > 1:
        dram_pd_h_stride = tile_h_out * stride * W_in * C_in * 2
        dram_pd_w_stride = tile_w_out * stride * C_in * 2
        dram_pd_ic_stride = 0
    else:
        dram_pd_h_stride = tile_h_out * stride * W_in * tile_ic * 2
        dram_pd_w_stride = tile_w_out * stride * tile_ic * 2
        dram_pd_ic_stride = H_in * W_in * tile_ic * 2
    # Output tile-packed layout: [num_oc, num_h, num_w, tile_h, tile_w, tile_oc]
    # Each DMA writeback writes one full tile linearly, strides must prevent overlap.
    plo_tile_bytes = tile_h_out * tile_w_out * tile_oc * 2
    dram_out_w_stride = plo_tile_bytes
    dram_out_h_stride = num_w_tiles * plo_tile_bytes
    dram_out_oc_stride = num_h_tiles * num_w_tiles * plo_tile_bytes
    dram_out_row_stride = tile_w_out * tile_oc * 2

    # DRAM tensor base addresses
    W_size = OC * KH * KW * C_in * 2
    I_size = N * H_in * W_in * C_in * 2
    output_region_size = num_oc_tiles * num_h_tiles * num_w_tiles * plo_tile_bytes

    dram_weight_base = tensor_base
    if dram_input_override:
        # Multi-layer: input comes from previous layer's output region
        dram_input_base = dram_input_override
        dram_output_base = tensor_base + W_size
    else:
        dram_input_base = tensor_base + W_size
        dram_output_base = tensor_base + W_size + I_size

    # PLI initialization: always provide a DRAM region for PLI DMA.
    # Even without explicit bias, PLI must be zeroed at ic=0 for each
    # OC tile to prevent accumulating stale partial sums from the
    # previous OC tile's last IC iteration.
    dma_pli_words = pli_wave // PKT_SIZE
    dram_bias_base = dram_output_base + output_region_size

    # DMA group bases (absolute SPM byte addresses for DMA engine)
    g0_dma = hw.spm_dma_group_base(0)  # PS
    g1_dma = hw.spm_dma_group_base(1)  # PD
    g2_dma = hw.spm_dma_group_base(2)  # PLI (even ic)
    g3_dma = hw.spm_dma_group_base(3)  # PLO (even ic)

    # AGU pong offsets in word64 units
    half_words = hw.half_group_words

    tiling_params = TilingParams(
        num_oc_tiles=num_oc_tiles,
        num_h_tiles=num_h_tiles,
        num_w_tiles=num_w_tiles,
        num_ic_tiles=num_ic_tiles,
        tile_h_out=tile_h_out,
        tile_w_out=tile_w_out,
        tile_h_in=tile_h_in,
        tile_w_in=tile_w_in,
        last_h_out=last_h_out,
        last_w_out=last_w_out,
        # DMA SPM addresses (with group bases) — bytes for DMA engine
        # PS/PD: ping/pong for double-buffering within their group
        # PLI/PLO: ping = even-ic group, pong = odd-ic group (bank swap)
        spm_ping=[g0_dma,            g1_dma,            g2_dma, g3_dma],
        spm_pong=[g0_dma + half_cap, g1_dma + half_cap, g3_dma, g2_dma],
        # AGU addresses (group-local, word64 units)
        # PS/PD: ping/pong for double-buffering
        # PLI/PLO: always offset 0 (bank swap handles group routing)
        agu_ping=[0, 0, 0, 0],
        agu_pong=[half_words, half_words, 0, 0],
        spm_map_even=_SPM_MAP_EVEN,
        spm_map_odd=_SPM_MAP_ODD,
        dram_weight_base=dram_weight_base,
        dram_input_base=dram_input_base,
        dram_output_base=dram_output_base,
        dram_ps_oc_stride=dram_ps_oc_stride,
        dram_ps_h_stride=0,
        dram_ps_ic_stride=dram_ps_ic_stride,
        dram_pd_oc_stride=0,
        dram_pd_h_stride=dram_pd_h_stride,
        dram_pd_w_stride=dram_pd_w_stride,
        dram_pd_ic_stride=dram_pd_ic_stride,
        dram_out_oc_stride=dram_out_oc_stride,
        dram_out_h_stride=dram_out_h_stride,
        dram_out_w_stride=dram_out_w_stride,
        dram_out_row_stride=dram_out_row_stride,
        dma_ps_words=ps_wave // PKT_SIZE,
        dma_pd_words=pd_wave // PKT_SIZE,
        dma_plo_words=pli_wave // PKT_SIZE,
        dram_bias_base=dram_bias_base,
        dram_bias_oc_stride=dma_pli_words * PKT_SIZE,
        dram_bias_h_stride=0,
        dma_pli_words=dma_pli_words,
        ps_reuse_across_spatial=False,  # disabled to ensure correct prefetching with double-buffering
        spatial_2d_dma=True,
        input_pad_enable=padding > 0,
        input_padding=padding,
        input_stride=stride,
        input_src_h=H_in,
        input_src_w=W_in,
        input_fill_mode=_DMA_FILL_EPSILON if padding > 0 else _DMA_FILL_ZERO,
        input_fill_value_lo=0,
        input_fill_value_hi=0,
        output_epilogue=_resolve_output_epilogue(op),
        output_epilogue_param0=0,
        pd_ic_agu_offset=pd_ic_agu_offset,
        bank_depth_bytes=hw.bank_depth_bytes,
        parallel_groups=0x0,  # no parallel groups for 3x3
        dma_pd_rows_per_bank=0,
        dma_pli_rows_per_bank=0,
        dma_plo_rows_per_bank=0,
        dma_ps_words_per_bank=0,
        dma_pd_words_per_bank=0,
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

def _lower_conv2d_1x1(op: OpDesc, hw: HardwareDesc,
                      tensor_base: int = 0,
                      dram_input_override: int = 0,
                      output_ic_tiled: bool = False,
                      input_nhwc_packs: int = 0,
                      output_pad: int = 0,
                      use_runtime_load_pad: bool = False) -> LayerHwConfig:
    act = op.inputs[0]
    wt = op.inputs[1]
    out = op.outputs[0]

    N, H_in, W_in, C_in = act.shape
    OC, KH, KW, _ = wt.shape
    _, H_out, W_out, _ = out.shape
    stride_v = op.attrs.get("stride", 1)
    padding = int(op.attrs.get("padding", 0))

    if padding < 0:
        raise TilingFailed(
            f"{op.name}: conv2d_1x1 padding must be >= 0, got {padding}"
        )
    if padding > 0 and not use_runtime_load_pad:
        raise TilingFailed(
            f"{op.name}: padded conv2d_1x1 requires runtime DMA load-pad"
        )
    if output_pad > 0:
        raise TilingFailed(
            f"{op.name}: padded output canvas path is no longer supported"
        )

    tile_ic = 12
    tile_oc = min(OC, 16)
    ic_words_per_pe = tile_ic // 4  # 12 fp16 per PE -> 3 word64 packets
    out_ch_pack = tile_oc // 4

    half_cap = hw.half_group_capacity
    pp = hw.parallel_ping_base
    ppong = hw.parallel_pong_base
    pes_per_bus = _pes_per_bus(hw)
    num_bus = hw.num_bus

    num_ic_tiles = C_in // tile_ic
    num_oc_tiles = math.ceil(OC / tile_oc)

    ps_wave = tile_oc * ic_words_per_pe * PKT_SIZE
    if ps_wave > half_cap:
        raise TilingFailed(f"{op.name}: weight tile exceeds half_cap")

    # ----------------------------------------------------------------
    # H-lane + W-time tiling model.
    # - Each active PE owns ONE local output H row.
    # - Within one wave, W is temporal inside the PE via
    #   OUTPUT_WINDOW_CNT_MINUS_ONE = tile_w_out - 1.
    # - If the per-bank PD/PLI footprint would overflow, the compiler
    #   splits W across multiple waves (num_w_tiles > 1).
    # - "normal path" (use_ultra=False): only bus 0 is active, so up
    #   to pes_per_bus rows fit per wave.
    # - "ultra path"  (use_ultra=True ): multiple buses are active in
    #   parallel; each bus carries up to pes_per_bus rows.
    # ----------------------------------------------------------------
    resident_oc_tiles = _choose_conv1x1_resident_oc_tiles(num_oc_tiles, num_bus)
    oc_parallel = resident_oc_tiles > 1
    use_ultra = (not oc_parallel) and (H_out > pes_per_bus)

    if padding > 0 and use_ultra:
        raise TilingFailed(
            f"{op.name}: conv2d_1x1 padding is not yet supported when output height "
            f"requires multi-bus ultra transport"
        )

    if oc_parallel:
        active_buses = resident_oc_tiles
        tile_h_per_bus = min(H_out, pes_per_bus)
    elif use_ultra:
        active_buses = min(num_bus, math.ceil(H_out / pes_per_bus))
        tile_h_per_bus = pes_per_bus
    else:
        active_buses = 1
        tile_h_per_bus = min(H_out, pes_per_bus)

    rows_per_wave = tile_h_per_bus if oc_parallel else tile_h_per_bus * active_buses
    tile_h = rows_per_wave        # wave-level "logical" H rows

    pd_bytes_per_w = tile_h_per_bus * ic_words_per_pe * PKT_SIZE
    pli_bytes_per_w = tile_h_per_bus * out_ch_pack * PKT_SIZE
    pd_cap = half_cap
    pli_cap = hw.half_parallel if (use_ultra or oc_parallel) else half_cap
    max_tile_w_pd = pd_cap // pd_bytes_per_w if pd_bytes_per_w > 0 else 0
    max_tile_w_pli = pli_cap // pli_bytes_per_w if pli_bytes_per_w > 0 else 0
    tile_w = min(W_out, max_tile_w_pd, max_tile_w_pli)
    if tile_w <= 0:
        overflow_kind = "bank half_cap" if (use_ultra or oc_parallel) else "half_cap"
        raise TilingFailed(
            f"{op.name}: conv1x1 cannot fit even tile_w=1 "
            f"(PD bytes/step={pd_bytes_per_w}, PLI bytes/step={pli_bytes_per_w}, "
            f"PD cap={pd_cap}, PLI cap={pli_cap}, mode={overflow_kind})"
        )

    pd_wave = tile_h_per_bus * tile_w * ic_words_per_pe * PKT_SIZE
    pli_wave = tile_h_per_bus * tile_w * out_ch_pack * PKT_SIZE

    num_h_tiles = math.ceil(H_out / tile_h)
    num_w_tiles = math.ceil(W_out / tile_w)
    last_h_out = H_out - (num_h_tiles - 1) * tile_h
    last_w_out = W_out - (num_w_tiles - 1) * tile_w
    num_oc_wave_groups = num_oc_tiles // resident_oc_tiles
    total_waves = num_oc_wave_groups * num_h_tiles * num_w_tiles * num_ic_tiles

    tiling = TilingResult(
        loop_dims=["oc_tile", "h_tile", "w_tile", "ic_tile"],
        loop_bounds={
            "oc_tile": num_oc_wave_groups, "h_tile": num_h_tiles,
            "w_tile": num_w_tiles, "ic_tile": num_ic_tiles,
        },
        total_waves=total_waves,
        reduction_dims=["ic_tile"],
    )

    ps_parallel = oc_parallel
    pd_parallel = use_ultra
    out_parallel = use_ultra or oc_parallel
    ps_mode = "parallel" if ps_parallel else "linear"
    pd_mode = "parallel" if pd_parallel else "linear"
    out_mode = "parallel" if out_parallel else "linear"
    ps_ctrl = 0x8 if ps_parallel else 0x0
    pd_ctrl = 0x8 if pd_parallel else 0x0
    out_ctrl = 0x8 if out_parallel else 0x0

    spm_layout = SpmPerGroupLayout(
        ps=SpmGroupLayout(
            ping_base=pp if ps_parallel else 0,
            pong_base=ppong if ps_parallel else half_cap,
            size=ps_wave,
            spm_mode=ps_mode,
        ),
        pd=SpmGroupLayout(
            ping_base=pp if pd_parallel else 0,
            pong_base=ppong if pd_parallel else half_cap,
            size=pd_wave,
            spm_mode=pd_mode,
        ),
        pli=SpmGroupLayout(
            ping_base=pp if out_parallel else 0,
            pong_base=ppong if out_parallel else half_cap,
            size=pli_wave,
            spm_mode=out_mode,
        ),
        plo=SpmGroupLayout(
            ping_base=pp if out_parallel else 0,
            pong_base=ppong if out_parallel else half_cap,
            size=pli_wave,
            spm_mode=out_mode,
        ),
    )

    # AGU - PS: och-resident path banks weights per bus; otherwise shared broadcast.
    pp_w = hw.parallel_ping_words
    ppong_w = hw.parallel_pong_words
    half_words = hw.half_group_words

    agu_ps = AguBankConfig(
        base_addr=pp_w if ps_parallel else 0, base_addr_h=0,
        iter0=ic_words_per_pe, iter1=1, iter2=1, iter3=tile_oc,
        stride0=1, stride1=0, stride2=0,
        stride3=ic_words_per_pe,
        ctrl=ps_ctrl, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=2,
        mask_cfg=0xF,
    )
    # AGU - PD: Linear/Normal — per-bus row addressing.
    # iter1 walks rows that the bus's PEs own (tile_h_per_bus). When ultra
    # is enabled the per-bus PD bank still only carries its own H stripe,
    # so we use tile_h_per_bus, not the wave-level tile_h.
    pd_ic_agu_offset = 0
    pd_stride1 = tile_w * ic_words_per_pe
    pd_stride2 = ic_words_per_pe
    if input_nhwc_packs > 0 and num_ic_tiles > 1:
        pd_wave = tile_h_per_bus * tile_w * input_nhwc_packs * PKT_SIZE
        pd_ic_agu_offset = ic_words_per_pe
        pd_stride1 = tile_w * input_nhwc_packs
        pd_stride2 = input_nhwc_packs

    agu_pd = AguBankConfig(
        base_addr=pp_w if pd_parallel else 0, base_addr_h=0,
        iter0=ic_words_per_pe, iter1=tile_h_per_bus, iter2=tile_w, iter3=1,
        stride0=1,
        stride1=pd_stride1,
        stride2=pd_stride2,
        stride3=0,
        ctrl=pd_ctrl, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=1,
        mask_cfg=0xF,
    )
    # AGU - PLI/PLO: bus-local oc tiles on och-resident path, or H stripes on ultra path.
    agu_pli = AguBankConfig(
        base_addr=pp_w if out_parallel else 0, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h_per_bus, iter2=tile_w, iter3=1,
        stride0=1,
        stride1=tile_w * out_ch_pack,
        stride2=out_ch_pack,
        stride3=0,
        ctrl=out_ctrl, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=1,
        mask_cfg=0xF,
    )
    agu_plo = AguBankConfig(
        base_addr=pp_w if out_parallel else 0, base_addr_h=0,
        iter0=out_ch_pack, iter1=tile_h_per_bus, iter2=tile_w, iter3=1,
        stride0=1,
        stride1=tile_w * out_ch_pack,
        stride2=out_ch_pack,
        stride3=0,
        ctrl=out_ctrl, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=1,
        mask_cfg=0xF,
    )

    # PE template params (W is fully temporal).
    pe_params = {
        "KERNEL_DMA_LEN": tile_oc * ic_words_per_pe,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": tile_w - 1,
        "KERNEL_COUNT": tile_oc,
        "KERNEL_LOOP_INNER": num_ic_tiles,
        "KERNEL_LOOP_OUTER": num_oc_wave_groups * num_h_tiles * num_w_tiles,
    }
    pe_prog = PeProgramRef(template_name="conv1d_k1c12s1_template", params=pe_params)
    hddu = HdduConfig(plane_en=0xF, plane_mode=0x1)
    # Conv1x1 scan-chain always maps one active PE to one H row.
    # Och-resident waves simply duplicate the same H rows across multiple buses.
    scan_chain = compute_scan_chain_conv1x1(
        hw.num_pes, hw.num_bus,
        tile_h_per_bus=tile_h_per_bus,
        active_buses=active_buses,
        stride=op.attrs.get("stride", 1),
    )
    cluster_map = compute_cluster_mapping(hw.num_clusters, tiling, op.op_type)

    dram_base = hw.dram_base
    if tensor_base == 0:
        tensor_base = dram_base + DRAM_BOOT_RESERVED
    # DRAM stores IC-tiled layout: weight [num_oc_tiles, num_ic_tiles, tile_oc, tile_ic]
    #                               input  [N, num_ic_tiles, H_in, W_in, tile_ic]
    dram_ps_oc_stride = num_ic_tiles * tile_oc * tile_ic * 2
    dram_ps_ic_stride = tile_oc * tile_ic * 2
    if input_nhwc_packs > 0 and num_ic_tiles > 1:
        dram_pd_h_stride = tile_h * stride_v * W_in * C_in * 2
        dram_pd_w_stride = tile_w * stride_v * C_in * 2
        dram_pd_ic_stride = 0
    else:
        dram_pd_h_stride = tile_h * stride_v * W_in * tile_ic * 2
        dram_pd_w_stride = tile_w * stride_v * tile_ic * 2
        dram_pd_ic_stride = H_in * W_in * tile_ic * 2
    # Output tile-packed layout: [num_oc, num_h, num_w, tile_h, tile_w, tile_oc]
    plo_tile_bytes = tile_h * tile_w * tile_oc * 2
    dram_out_w_stride = plo_tile_bytes
    dram_out_h_stride = num_w_tiles * plo_tile_bytes
    dram_out_oc_stride = num_h_tiles * num_w_tiles * plo_tile_bytes
    dram_out_row_stride = tile_w * tile_oc * 2

    bdb = hw.bank_depth_bytes

    W_size = OC * C_in * 2
    I_size = N * H_in * W_in * C_in * 2
    output_region_size = num_oc_tiles * num_h_tiles * num_w_tiles * plo_tile_bytes

    dram_weight_base = tensor_base
    if dram_input_override:
        dram_input_base = dram_input_override
        dram_output_base = tensor_base + W_size
    else:
        dram_input_base = tensor_base + W_size
        dram_output_base = tensor_base + W_size + I_size

    # PLI initialization: always provide a DRAM region for PLI DMA.
    dma_pli_words = pli_wave // PKT_SIZE
    dram_bias_base = dram_output_base + output_region_size

    # DMA group bases
    g0_dma = hw.spm_dma_group_base(0)
    g1_dma = hw.spm_dma_group_base(1)
    g2_dma = hw.spm_dma_group_base(2)
    g3_dma = hw.spm_dma_group_base(3)

    tiling_params = TilingParams(
        num_oc_tiles=num_oc_tiles, num_h_tiles=num_h_tiles,
        num_w_tiles=num_w_tiles, num_ic_tiles=num_ic_tiles,
        tile_h_out=tile_h,
        tile_w_out=tile_w,
        tile_h_in=tile_h,
        tile_w_in=tile_w,
        last_h_out=last_h_out,
        last_w_out=last_w_out,
        spm_ping=[g0_dma,            g1_dma,                          g2_dma,                          g3_dma],
        spm_pong=[g0_dma + (hw.half_parallel if ps_parallel else half_cap),
                  g1_dma + (hw.half_parallel if pd_parallel else half_cap),
                  g3_dma,
                  g2_dma],
        agu_ping=[pp_w if ps_parallel else 0,
                  pp_w if pd_parallel else 0,
                  pp_w if out_parallel else 0,
                  pp_w if out_parallel else 0],
        agu_pong=[ppong_w if ps_parallel else half_words,
                  ppong_w if pd_parallel else half_words,
                  pp_w if out_parallel else 0,
                  pp_w if out_parallel else 0],
        spm_map_even=_SPM_MAP_EVEN, spm_map_odd=_SPM_MAP_ODD,
        dram_weight_base=dram_weight_base,
        dram_input_base=dram_input_base,
        dram_output_base=dram_output_base,
        dram_ps_oc_stride=dram_ps_oc_stride,
        dram_ps_h_stride=0,
        dram_ps_ic_stride=dram_ps_ic_stride,
        dram_pd_oc_stride=0,
        dram_pd_h_stride=dram_pd_h_stride,
        dram_pd_w_stride=dram_pd_w_stride,
        dram_pd_ic_stride=dram_pd_ic_stride,
        dram_out_oc_stride=dram_out_oc_stride,
        dram_out_h_stride=dram_out_h_stride,
        dram_out_w_stride=dram_out_w_stride,
        dram_out_row_stride=dram_out_row_stride,
        dma_ps_words=(resident_oc_tiles * ps_wave) // PKT_SIZE if ps_parallel else ps_wave // PKT_SIZE,
        dma_pd_words=pd_wave // PKT_SIZE,
        dma_plo_words=pli_wave // PKT_SIZE,
        dram_bias_base=dram_bias_base,
        dram_bias_oc_stride=dma_pli_words * PKT_SIZE,
        dram_bias_h_stride=0,
        dma_pli_words=dma_pli_words,
        ps_reuse_across_spatial=False,
        spatial_2d_dma=True,
        input_pad_enable=padding > 0,
        input_padding=padding,
        input_stride=stride_v,
        input_src_h=H_in,
        input_src_w=W_in,
        input_fill_mode=_DMA_FILL_ZERO,
        input_fill_value_lo=0,
        input_fill_value_hi=0,
        output_epilogue=_resolve_output_epilogue(op),
        output_epilogue_param0=0,
        pd_ic_agu_offset=pd_ic_agu_offset,
        bank_depth_bytes=bdb,
        parallel_groups=0xD if oc_parallel else (0xE if use_ultra else 0x0),
        dma_pd_rows_per_bank=tile_h_per_bus if pd_parallel else 0,
        dma_pli_rows_per_bank=tile_h_per_bus if use_ultra else 0,
        dma_plo_rows_per_bank=tile_h_per_bus if use_ultra else 0,
        dma_ps_words_per_bank=ps_wave // PKT_SIZE if ps_parallel else 0,
        dma_pd_words_per_bank=0,
        dma_plo_words_per_bank=pli_wave // PKT_SIZE if use_ultra else 0,
        conv1x1_resident_oc_tiles=resident_oc_tiles,
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

def _lower_gemm(op: OpDesc, hw: HardwareDesc,
                tensor_base: int = 0,
                dram_input_override: int = 0,
                output_ic_tiled: bool = False,
                input_nhwc_packs: int = 0,
                output_pad: int = 0,
                use_runtime_load_pad: bool = False) -> LayerHwConfig:
    A = op.inputs[0]
    B = op.inputs[1]
    C = op.outputs[0]

    M, K = A.shape
    _, N = B.shape
    ep = 4  # elements per packet

    # PE capability per noc_gen.generate_gemm_test:
    # - Each PE handles a (PE_M × PE_N) output sub-tile, reducing PE_K K-elements per pass.
    # - The (M, N) output is tiled onto a PE grid (grid_m × grid_n) inside one bus,
    #   with the K dimension chained across buses (grid_k stages).
    PE_M, PE_N, PE_K = 12, 8, 32

    half_cap = hw.half_group_capacity
    pp = hw.parallel_ping_base
    ppong = hw.parallel_pong_base
    pp_w = hw.parallel_ping_words
    ppong_w = hw.parallel_pong_words
    half_words = hw.half_group_words
    pes_per_bus = _pes_per_bus(hw)
    num_bus = hw.num_bus

    grid_m = math.ceil(M / PE_M)
    grid_n = math.ceil(N / PE_N)
    grid_k = math.ceil(K / PE_K)
    tile_w_words = PE_N // ep
    tile_d_words = math.ceil(PE_M / ep)

    # ----------------------------------------------------------------
    # Spatial tile selection:
    # Each bus carries one K-stage (up to num_bus stages chain together).
    # Within a bus we must fit grid_m_per_wave * grid_n_per_wave PEs into
    # pes_per_bus slots; if grid_m * grid_n exceeds that budget we tile
    # M/N temporally (wave_m, wave_n waves).
    # ----------------------------------------------------------------
    def _choose_mn_tiles(gm: int, gn: int, budget: int):
        best = None
        prefer_n_cap = max(1, budget // 2)
        single_k_wave = grid_k <= num_bus
        for mt in range(min(gm, budget), 0, -1):
            max_nt = min(gn, budget // mt)
            for nt in range(max_nt, 0, -1):
                wm = math.ceil(gm / mt)
                wn = math.ceil(gn / nt)
                waves = wm * wn
                area = mt * nt
                aspect = abs((gm / max(gn, 1)) - (mt / max(nt, 1)))
                balance = abs(wm - wn)
                if single_k_wave:
                    ps_wave_candidate = K * (nt * tile_w_words) * PKT_SIZE
                    pd_wave_candidate = (mt * tile_d_words * K) * PKT_SIZE
                    pli_wave_candidate = (mt * nt * PE_N * tile_d_words) * PKT_SIZE
                    ps_resident_bytes = wn * ps_wave_candidate
                    out_row_resident_bytes = wn * pli_wave_candidate
                    resident_full_n = 0
                    if (ps_resident_bytes <= half_cap) and (out_row_resident_bytes <= half_cap):
                        resident_m = min(
                            wm,
                            half_cap // max(pd_wave_candidate, 1),
                            half_cap // max(out_row_resident_bytes, 1),
                        )
                        if resident_m == wm:
                            resident_full_n = 1
                    single_n_resident = 1 if resident_full_n and wn == 1 else 0
                    n_bias = min(nt, prefer_n_cap)
                    # Single-K GEMM advances M before N at runtime. Reward
                    # wider N waves, but cap that bias at half the PE budget
                    # so we do not collapse equal-wave generic candidates into
                    # 1xN. If a candidate can already run fully resident,
                    # prefer that fast path first, and give extra weight to a
                    # single-N resident sweep because the runtime hides the
                    # deferred PD fill only during the first N tile.
                    score = (waves, -single_n_resident, -resident_full_n,
                             -n_bias, -mt, balance, aspect,
                             -area, -min(mt, nt), -max(mt, nt))
                else:
                    # For multi-K waves keep the older square-ish preference.
                    score = (waves, balance, aspect,
                             -area, -min(mt, nt), -max(mt, nt))
                if best is None or score < best[0]:
                    best = (score, mt, nt, wm, wn)
        if best is None:
            return 1, 1, gm, gn
        _, mt, nt, wm, wn = best
        return mt, nt, wm, wn

    grid_m_per_wave, grid_n_per_wave, wave_m, wave_n = _choose_mn_tiles(
        grid_m, grid_n, pes_per_bus
    )

    # K parallelism: one K-stage per bus. If grid_k > num_bus, run multiple
    # K-waves; each K-wave consumes up to num_bus stages.
    grid_k_per_wave = min(grid_k, num_bus)
    wave_k = math.ceil(grid_k / grid_k_per_wave)

    # Per-wave logical tile sizes (in elements).
    M_tile = grid_m_per_wave * PE_M
    N_tile = grid_n_per_wave * PE_N
    K_tile = grid_k_per_wave * PE_K
    M_tile_eff = min(M_tile, M)
    N_tile_eff = min(N_tile, N)
    K_tile_eff = min(K_tile, K)
    pe_k_eff = min(PE_K, K_tile_eff)

    # SPM wave footprints (bytes).
    # GEMM planes are DMA-visible as one packed wave tile per relevant wave
    # coordinate pair, not as raw row-major tensor slices.
    ps_row_words = grid_n_per_wave * tile_w_words

    ps_wave = K_tile_eff * ps_row_words * PKT_SIZE      # PS = B[K_tile, N_wave]
    # When the wave covers multiple M tiles in parallel (grid_m_per_wave > 1)
    # the SPM PD region must hold one A-slice per active m_idx PE so that
    # tagged AGU emissions can feed every (m_idx, n_idx) PE in the bus.
    pd_wave = (grid_m_per_wave * tile_d_words * K_tile_eff) * PKT_SIZE
    pli_wave = (grid_m_per_wave * grid_n_per_wave * PE_N * tile_d_words) * PKT_SIZE  # padded C[grid_m*PE_M, N_wave]

    if max(ps_wave, pd_wave, pli_wave) > half_cap:
        raise TilingFailed(
            f"{op.name}: GEMM PE-tile footprint exceeds half_cap "
            f"(ps={ps_wave}, pd={pd_wave}, pli={pli_wave}, half_cap={half_cap})"
        )

    # Loop bounds — keep generic 4D names so codegen/runtime are unchanged:
    #   oc_tile ↔ wave_m   (M waves)
    #   h_tile  ↔ wave_n   (N waves)
    #   w_tile  ↔ 1         (GEMM has no W dim)
    #   ic_tile ↔ wave_k   (K waves; reduction)
    num_m_tiles = wave_m
    num_n_tiles = wave_n
    num_k_tiles = wave_k
    total_waves = num_m_tiles * num_n_tiles * num_k_tiles

    def _choose_full_n_resident_tiles() -> tuple[int, int]:
        if num_k_tiles != 1 or num_n_tiles == 0 or num_m_tiles == 0:
            return 0, 0

        resident_n = num_n_tiles
        ps_resident_bytes = resident_n * ps_wave
        out_row_resident_bytes = resident_n * pli_wave
        if ps_resident_bytes > half_cap or out_row_resident_bytes > half_cap:
            return 0, 0

        resident_m = min(
            num_m_tiles,
            half_cap // max(pd_wave, 1),
            half_cap // max(out_row_resident_bytes, 1),
        )
        if resident_m == 0:
            return 0, 0
        return resident_m, resident_n

    def _choose_gemm_pd_preload_m_tiles(resident_m: int) -> int:
        if resident_m <= 0:
            return 0
        # PD background fill only runs during the first resident N-tile.
        # Preload the front half of the resident M window so the remaining
        # tail has one N-tile worth of compute/backpressure time to arrive.
        return min(resident_m, max(1, (resident_m + 1) // 2))

    resident_m_tiles, resident_n_tiles = _choose_full_n_resident_tiles()
    if resident_n_tiles == 1 and resident_m_tiles < num_m_tiles:
        # Partial resident mode only pays off when a resident PD slab can be
        # reused by later N waves. With a single N wave it reloads PS across
        # M chunks without reducing total PD traffic, so fall back to generic.
        # Keep the computed M-chunk capacity as a runtime hint so generic
        # single-N execution can still batch multiple M waves per HDDU run
        # without reloading PS for every chunk.
        resident_n_tiles = 0
    pd_preload_m_tiles = _choose_gemm_pd_preload_m_tiles(resident_m_tiles)
    ps_spm_bytes = resident_n_tiles * ps_wave if resident_n_tiles else ps_wave
    pd_spm_bytes = resident_m_tiles * pd_wave if resident_m_tiles else pd_wave
    out_spm_bytes = pli_wave
    if resident_m_tiles:
        if resident_n_tiles:
            out_spm_bytes = resident_m_tiles * resident_n_tiles * pli_wave
        elif num_n_tiles == 1:
            out_spm_bytes = resident_m_tiles * pli_wave

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

    # K-chain ultra mode is only needed when one wave spans multiple K stages
    # across buses. Single-stage GEMM follows the normal/linear path.
    use_ultra = grid_k_per_wave > 1

    spm_layout = SpmPerGroupLayout(
        ps=SpmGroupLayout(
            ping_base=pp if use_ultra else 0,
            pong_base=ppong if use_ultra else half_cap,
            size=ps_spm_bytes,
            spm_mode="parallel" if use_ultra else "linear",
        ),
        pd=SpmGroupLayout(
            ping_base=pp if use_ultra else 0,
            pong_base=ppong if use_ultra else half_cap,
            size=pd_spm_bytes,
            spm_mode="parallel" if use_ultra else "linear",
        ),
        pli=SpmGroupLayout(
            ping_base=0,
            pong_base=half_cap,
            size=out_spm_bytes,
            spm_mode="linear",
        ),
        plo=SpmGroupLayout(
            ping_base=0,
            pong_base=half_cap,
            size=out_spm_bytes,
            spm_mode="linear",
        ),
    )

    # GEMM plane mapping (matches noc_gen / test_noc_sim):
    # Plane mapping (matches noc_gen / test_noc_sim):
    #   PS  = B  (weights)
    #   PD  = A  (input activations)
    #   PLI/PLO = C  (partial sums)

    # AGU PS (B): one wave contains grid_n_per_wave tagged N slices.
    # In K-chain ultra mode each bus consumes only its local K stage, so the
    # AGU K iterator is PE-local even though the DMA-visible wave footprint
    # still spans all K stages in the wave.
    agu_ps = AguBankConfig(
        base_addr=pp_w if use_ultra else 0, base_addr_h=0,
        iter0=tile_w_words, iter1=pe_k_eff, iter2=grid_n_per_wave, iter3=1,
        stride0=1,
        stride1=ps_row_words,
        stride2=tile_w_words, stride3=0,
        ctrl=0x8 if use_ultra else 0x0, lane_cfg=0,
        # tag_base/tag_stride per ultra rule: ps_id == n_idx for the active
        # PE within its bus.  Cluster_gen is the actual per-cluster owner of
        # tag_base; this default reflects the single-cluster ultra case.
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=2,
        mask_cfg=0xF,
    )
    agu_pd = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=tile_d_words,
        iter1=grid_m_per_wave,
        iter2=pe_k_eff,
        iter3=1,
        stride0=1,
        stride1=tile_d_words,
        stride2=grid_m_per_wave * tile_d_words,
        stride3=tile_d_words,
        ctrl=0x8 if use_ultra else 0x0, lane_cfg=0,
        tag_base=0,
        tag_stride0=0,
        tag_stride1=1,
        tag_ctrl=1,
        mask_cfg=0xF,
    )
    # AGU PLI/PLO (C): one wave contains grid_m_per_wave * grid_n_per_wave
    # tagged C slices, where pli_id == m_idx * grid_n_per_wave + n_idx (see
    # compute_scan_chain_gemm). Even in K-chain mode these partial sums remain
    # wave-local linear data because the top/mid/bottom buses reduce through
    # the PE accumulation path instead of consuming an ultra fanout.
    pe_per_wave = grid_m_per_wave * grid_n_per_wave
    agu_pli = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=tile_d_words, iter1=PE_N, iter2=pe_per_wave, iter3=1,
        stride0=1,
        stride1=tile_d_words,
        stride2=PE_N * tile_d_words, stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=2,
        mask_cfg=0xF,
    )
    agu_plo = AguBankConfig(
        base_addr=0, base_addr_h=0,
        iter0=tile_d_words, iter1=PE_N, iter2=pe_per_wave, iter3=1,
        stride0=1,
        stride1=tile_d_words,
        stride2=PE_N * tile_d_words, stride3=0,
        ctrl=0x0, lane_cfg=0,
        tag_base=0, tag_stride0=1, tag_stride1=1, tag_ctrl=2,
        mask_cfg=0xF,
    )

    # gemm_template.json patches the PE-local PS DMA contract:
    # - SDMA preloads one PE-local K-stage of the PS/B tile into PE DM.
    # - LDMA.LHB then walks the same DM image as fp16 broadcasts.
    # - VPSUMR reduces one PE-local C tile [PE_M, N_wave].
    #
    # The PE program is shared by all buses in a K-chain, so its K-loop bounds
    # must be per-bus/per-PE rather than the full K-span of the wave.
    pe_ps_words = pe_k_eff * tile_w_words

    pe_params = {
        "KERNEL_DMA_STORE_LEN": pe_ps_words,
        "KERNEL_DMA_LOAD_LEN": pe_ps_words * ep,
        "INPUT_DIM": pe_k_eff,
        "OUTPUT_DIM_MINUS_ONE": min(PE_N, N_tile_eff) - 1,
        "PSUM_COUNT": (PE_M * PE_N) // ep,
        "NUM_OF_KERNEL_PREFETCH_SETS": num_k_tiles * num_n_tiles,
        "NUM_OF_KERNEL_LOAD_LOOP": num_n_tiles,
        "NUM_OF_KERNEL_REUSE_LOOP": num_m_tiles,
        "K_TILE_DIM": pe_k_eff,
        # GEMM PE-grid metadata (inspectable from IR for verification)
        "GRID_M": grid_m,
        "GRID_N": grid_n,
        "GRID_K": grid_k,
        "GRID_M_PER_WAVE": grid_m_per_wave,
        "GRID_N_PER_WAVE": grid_n_per_wave,
        "GRID_K_PER_WAVE": grid_k_per_wave,
    }
    pe_prog = PeProgramRef(template_name="gemm_template", params=pe_params)
    # The GEMM PE template always finishes with VPSUMR, which consumes PLI
    # even when the logical input partial sum is all zeros.
    hddu = HdduConfig(plane_en=0xF, plane_mode=0x2)
    # K-chain scan-chain: bus i carries K-stage i.  When grid_k_per_wave==1
    # the lone bus naturally takes route_mode=3 (IB, OB) per the open-issue-1
    # resolution. PLI valid only on first stage; PLO valid only on last.
    scan_chain = compute_scan_chain_gemm(
        hw.num_pes, hw.num_bus,
        grid_m=grid_m_per_wave,
        grid_n=grid_n_per_wave,
        grid_k=grid_k_per_wave,
        use_ultra=use_ultra,
    )
    cluster_map = compute_cluster_mapping(hw.num_clusters, tiling, op.op_type)

    dram_base = hw.dram_base
    if tensor_base == 0:
        tensor_base = dram_base + DRAM_BOOT_RESERVED
    # Reserve DRAM regions using the actual DMA-visible footprint, not only the
    # logical tensor byte size. GEMM uses padded PE-local tiles for PD/PLI/PLO,
    # so small edge tiles (for example M < PE_M) can be larger on DRAM than the
    # raw tensor itself. Using the raw size here causes adjacent regions to
    # overlap once gen_test_dram writes the packed DMA image.
    a_bytes = num_m_tiles * num_k_tiles * pd_wave
    b_bytes = num_n_tiles * num_k_tiles * ps_wave
    c_bytes = num_m_tiles * num_n_tiles * pli_wave
    bdb = hw.bank_depth_bytes

    # GEMM always needs a PLI source because gemv_template issues VPSUMR.
    # When the workload has no explicit input partial sum tensor, gen_test_dram
    # leaves this region zero-filled so the first reduction starts from zero.
    dram_bias_base = tensor_base + a_bytes + b_bytes + c_bytes
    dma_pli_words = pli_wave // PKT_SIZE

    g0_dma = hw.spm_dma_group_base(0)
    g1_dma = hw.spm_dma_group_base(1)
    g2_dma = hw.spm_dma_group_base(2)
    g3_dma = hw.spm_dma_group_base(3)

    # Plane mapping: PS=B, PD=A, output=C.
    # GEMM uses packed per-wave DMA-visible tiles. Each wave tile is placed in
    # a rectangular region indexed only by the wave coordinates the plane
    # logically depends on:
    #   PS  [n_wave, k_wave]
    #   PD  [m_wave, k_wave]
    #   PLI [m_wave, n_wave]
    #   PLO [m_wave, n_wave]
    if dram_input_override:
        # Multi-layer chaining (M14-S4-1): consume the producer layer's output
        # region in place (mirrors _lower_conv2d_3x3) and allocate only this
        # layer's B/C from tensor_base. dram_bias_base is left past the original
        # A|B|C reservation above, so it stays clear of C.
        # NOTE: this only chains the base *address*. The producer writes its
        # output in PLO packing while the PD load below reads PD packing, so the
        # intermediate still needs a PLO->PD relayout for functional correctness
        # (M14-S4-2); address chaining alone reaches EBREAK but not golden.
        dram_input_base = dram_input_override               # A (= producer output)
        dram_weight_base = tensor_base                      # B
        dram_output_base = tensor_base + b_bytes            # C
    else:
        dram_input_base = tensor_base                       # A
        dram_weight_base = tensor_base + a_bytes            # B
        dram_output_base = tensor_base + a_bytes + b_bytes  # C

    ps_tile_bytes = ps_wave
    pd_tile_bytes = pd_wave
    pli_tile_bytes = pli_wave
    plo_tile_bytes = pli_wave

    tiling_params = TilingParams(
        num_oc_tiles=num_m_tiles,
        num_h_tiles=num_n_tiles,
        num_w_tiles=1,
        num_ic_tiles=num_k_tiles,
        tile_h_out=1,
        tile_w_out=1,
        tile_h_in=1,
        tile_w_in=1,
        last_h_out=1,
        last_w_out=1,
        spm_ping=[g0_dma,            g1_dma,            g2_dma,            g3_dma],
        spm_pong=[g0_dma + (hw.half_parallel if use_ultra else half_cap),
              g1_dma + (hw.half_parallel if use_ultra else half_cap),
              g2_dma + half_cap,
              g3_dma + half_cap],
                    agu_ping=[pp_w if use_ultra else 0, pp_w if use_ultra else 0, 0, 0],
                    agu_pong=[ppong_w if use_ultra else half_words, ppong_w if use_ultra else half_words,
              half_words, half_words],
        spm_map_even=_SPM_MAP_EVEN, spm_map_odd=_SPM_MAP_ODD,
        dram_weight_base=dram_weight_base,                  # PS = B
        dram_input_base=dram_input_base,                    # PD = A
        dram_output_base=dram_output_base,                  # PLO = C
        dram_ps_oc_stride=0,                                # B not dep on M
        dram_ps_h_stride=ps_tile_bytes,                     # B N-wave step
        dram_ps_ic_stride=num_n_tiles * ps_tile_bytes,      # B K-wave step
        dram_pd_oc_stride=num_k_tiles * pd_tile_bytes,      # A M-wave step
        dram_pd_h_stride=0,                                 # A not dep on N
        dram_pd_w_stride=0,
        dram_pd_ic_stride=pd_tile_bytes,                    # A K-wave step
        dram_out_oc_stride=num_n_tiles * plo_tile_bytes,    # C M-wave step
        dram_out_h_stride=plo_tile_bytes,                   # C N-wave step
        dram_out_w_stride=0,
        dram_out_row_stride=0,
        dma_ps_words=ps_wave // PKT_SIZE,
        dma_pd_words=pd_wave // PKT_SIZE,
        dma_plo_words=pli_wave // PKT_SIZE,
        dram_bias_base=dram_bias_base,
        dram_bias_oc_stride=num_n_tiles * pli_tile_bytes,
        dram_bias_h_stride=pli_tile_bytes,
        dma_pli_words=dma_pli_words,
        ps_reuse_across_spatial=False,
        spatial_2d_dma=False,
        input_pad_enable=False,
        input_padding=0,
        input_stride=0,
        input_src_h=0,
        input_src_w=0,
        input_fill_mode=_DMA_FILL_ZERO,
        input_fill_value_lo=0,
        input_fill_value_hi=0,
        output_epilogue=_resolve_output_epilogue(op),
        output_epilogue_param0=0,
        pd_ic_agu_offset=0,
        bank_depth_bytes=bdb,
        parallel_groups=0x3 if use_ultra else 0x0,  # PS, PD
        dma_pd_rows_per_bank=0,
        dma_pli_rows_per_bank=0,
        dma_plo_rows_per_bank=0,
        dma_ps_words_per_bank=ps_row_words * pe_k_eff if use_ultra and ps_wave > 0 else 0,
        dma_pd_words_per_bank=grid_m_per_wave * tile_d_words * pe_k_eff if use_ultra and pd_wave > 0 else 0,
        dma_plo_words_per_bank=0,
        gemm_resident_m_tiles=resident_m_tiles,
        gemm_resident_n_tiles=resident_n_tiles,
        gemm_pd_preload_m_tiles=pd_preload_m_tiles,
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


def _is_single_wave(layer: LayerHwConfig) -> bool:
    tp = layer.tiling_params
    return (
        tp.num_oc_tiles == 1
        and tp.num_h_tiles == 1
        and tp.num_w_tiles == 1
        and tp.num_ic_tiles == 1
    )


def _gemm_packer_meta(layer: LayerHwConfig) -> Dict[str, int]:
    """Mirror the GEMM layout metadata used by gen_test_dram packers."""
    params = layer.pe_program.params
    tp = layer.tiling_params
    rows_per_word = 4
    grid_m = max(1, int(params.get("GRID_M", 1)))
    grid_n = max(1, int(params.get("GRID_N", 1)))
    grid_k = max(1, int(params.get("GRID_K", 1)))
    grid_m_per_wave = max(1, int(params.get("GRID_M_PER_WAVE", grid_m)))
    grid_n_per_wave = max(1, int(params.get("GRID_N_PER_WAVE", grid_n)))
    grid_k_per_wave = max(1, int(params.get("GRID_K_PER_WAVE", grid_k)))
    tile_d_words = max(1, int(layer.agu_pd.iter0))
    tile_w_words = max(1, int(layer.agu_ps.iter0))
    pe_n = max(1, int(params.get("OUTPUT_DIM", tile_w_words * rows_per_word)))
    pe_k = max(1, int(params.get("K_TILE_DIM", params.get("INPUT_DIM", 1))))
    ps_words_per_k = max(1, tile_w_words * grid_n_per_wave)
    pd_words_per_k = max(1, tile_d_words * grid_m_per_wave)
    k_tile_dim = max(
        1,
        tp.dma_ps_words // ps_words_per_k,
        tp.dma_pd_words // pd_words_per_k,
        pe_k * grid_k_per_wave,
    )
    return {
        "grid_m_per_wave": grid_m_per_wave,
        "grid_n_per_wave": grid_n_per_wave,
        "tile_d_words": tile_d_words,
        "rows_per_word": rows_per_word,
        "pe_n": pe_n,
        "k_tile_dim": k_tile_dim,
    }


def _derive_gemm_relayout_q(producer: LayerHwConfig,
                            consumer: LayerHwConfig) -> List[int]:
    """Derive the 12-fp16 PLO-to-PD block permutation from packer order."""
    plo = _gemm_packer_meta(producer)
    pd = _gemm_packer_meta(consumer)

    # _pack_gemm_c_output flatten order:
    # [m_tile, n_tile, n_local, row_word, lane].
    plo_positions = [
        (
            m_tile * plo["tile_d_words"] * plo["rows_per_word"]
            + row_word * plo["rows_per_word"] + lane,
            n_tile * plo["pe_n"] + n_local,
        )
        for m_tile in range(plo["grid_m_per_wave"])
        for n_tile in range(plo["grid_n_per_wave"])
        for n_local in range(plo["pe_n"])
        for row_word in range(plo["tile_d_words"])
        for lane in range(plo["rows_per_word"])
    ]

    # _pack_gemm_a_pd flatten order:
    # [k, m_tile, row_word, lane].
    pd_positions = [
        (
            m_tile * pd["tile_d_words"] * pd["rows_per_word"]
            + row_word * pd["rows_per_word"] + lane,
            k,
        )
        for k in range(pd["k_tile_dim"])
        for m_tile in range(pd["grid_m_per_wave"])
        for row_word in range(pd["tile_d_words"])
        for lane in range(pd["rows_per_word"])
    ]

    assert len(plo_positions) == producer.tiling_params.dma_plo_words * 4
    assert len(pd_positions) == consumer.tiling_params.dma_pd_words * 4
    assert len(plo_positions) == len(pd_positions)
    assert len(set(plo_positions)) == len(plo_positions)
    assert set(plo_positions) == set(pd_positions)

    plo_where = {position: i for i, position in enumerate(plo_positions)}
    p = [plo_where[position] for position in pd_positions]

    block_elements = 12
    assert len(p) % block_elements == 0
    q: List[int] = []
    for block_start in range(0, len(p), block_elements):
        source_start = p[block_start]
        assert source_start % block_elements == 0
        assert p[block_start:block_start + block_elements] == list(
            range(source_start, source_start + block_elements)
        )
        q.append(source_start // block_elements)

    assert sorted(q) == list(range(len(q)))
    return q


def lower_workload(wir: WorkloadIR) -> HardwareIR:
    """Lower all ops in a WorkloadIR to produce a HardwareIR.

    For multi-layer workloads, chains DRAM addresses so that each layer's
    input region follows the previous layer's output (or uses the previous
    layer's output directly for intermediate tensors).
    """
    layers: List[LayerHwConfig] = []
    relayouts: List[RelayoutDesc] = []

    # Build a map of tensor_name → producer_layer_index for chaining
    output_tensor_map: Dict[str, int] = {}  # tensor name → layer index
    for i, op in enumerate(wir.ops):
        for out_t in op.outputs:
            output_tensor_map[out_t.name] = i

    # Pre-analyze: determine which layers read from a previous layer's NHWC
    # output and need multi-IC-tile processing (PD-stride approach).
    # input_nhwc_packs_map[i] = number of OC packs in the producer layer's
    # output (0 if not reading from a previous layer's output or single IC tile).
    input_nhwc_packs_map: Dict[int, int] = {}
    for i, op in enumerate(wir.ops):
        inp_name = op.inputs[0].name
        if inp_name in output_tensor_map and output_tensor_map[inp_name] < i:
            consumer_ic = op.inputs[0].shape[-1]  # NHWC last dim = IC
            tile_ic = 12 if op.op_type == "conv2d_1x1" else 4
            if consumer_ic // tile_ic > 1:
                # Compute producer's out_ch_pack = min(OC, 16) // 4
                prev_idx = output_tensor_map[inp_name]
                prev_op = wir.ops[prev_idx]
                prev_oc = prev_op.outputs[0].shape[-1]  # NHWC last dim = OC
                input_nhwc_packs_map[i] = min(prev_oc, 16) // 4

    # Sequential DRAM allocation cursor
    dram_cursor = wir.hardware.dram_base + DRAM_BOOT_RESERVED

    for i, op in enumerate(wir.ops):
        fn = _LOWERING_DISPATCH.get(op.op_type)
        if fn is None:
            raise ValueError(f"Unsupported op type: {op.op_type}")

        # Check if input comes from a previous layer's output
        input_from_prev = (op.inputs[0].name in output_tensor_map
                           and output_tensor_map[op.inputs[0].name] < i)

        if input_from_prev:
            # Layer N's input = Layer (N-1)'s output.
            # Place this layer's weight right after previous layer's output+bias,
            # and use previous layer's dram_output_base as this layer's input.
            prev_idx = output_tensor_map[op.inputs[0].name]
            prev_tp = layers[prev_idx].tiling_params
            input_pad = int(op.attrs.get("padding", 0)) if op.op_type in ("conv2d_3x3", "conv2d_1x1") else 0
            if input_pad > 0 and (
                prev_tp.num_oc_tiles != 1
                or prev_tp.num_h_tiles != 1
                or prev_tp.num_w_tiles != 1
            ):
                raise TilingFailed(
                    f"{op.name}: padded conv input from previous layer currently "
                    f"requires producer output to be a single tile"
                )

            dram_input_base = prev_tp.dram_output_base

            prev_out_end = prev_tp.dram_output_base + _output_total_bytes(prev_tp)
            prev_bias_end = prev_tp.dram_bias_base + _bias_total_bytes(prev_tp) \
                if prev_tp.dram_bias_base > 0 else prev_out_end
            weight_base = max(prev_out_end, prev_bias_end)
            weight_base = (weight_base + 15) & ~15  # Align to 16-byte boundary

            layer = fn(op, wir.hardware,
                       tensor_base=weight_base,
                       dram_input_override=dram_input_base,
                       input_nhwc_packs=input_nhwc_packs_map.get(i, 0),
                       use_runtime_load_pad=input_pad > 0)

            prev_op = wir.ops[prev_idx]
            if (prev_op.op_type == "gemm"
                    and op.op_type == "gemm"
                    and _is_single_wave(layers[prev_idx])
                    and _is_single_wave(layer)):
                total_beats = prev_tp.dma_plo_words
                assert total_beats == layer.tiling_params.dma_pd_words
                assert total_beats * PKT_SIZE <= wir.hardware.group_capacity

                scratch_base = (dram_cursor + 15) & ~15
                scratch_end = scratch_base + total_beats * PKT_SIZE
                dram_cursor = (scratch_end + 15) & ~15

                # Preserve S4-1's GEMM override contract: tensor_base owns B/C,
                # while A is explicitly rebased to the relaid scratch region.
                layer = fn(op, wir.hardware,
                           tensor_base=dram_cursor,
                           dram_input_override=scratch_base,
                           input_nhwc_packs=input_nhwc_packs_map.get(i, 0),
                           use_runtime_load_pad=input_pad > 0)
                assert _is_single_wave(layer)
                assert total_beats == layer.tiling_params.dma_pd_words

                block_beats = 3  # 12 fp16 values per proven atomic block.
                q = _derive_gemm_relayout_q(layers[prev_idx], layer)
                assert len(q) * block_beats == total_beats
                relayouts.append(RelayoutDesc(
                    consumer_layer=i,
                    src_dram=prev_tp.dram_output_base,
                    dst_dram=scratch_base,
                    total_beats=total_beats,
                    block_beats=block_beats,
                    q=q,
                ))

        else:
            input_pad = int(op.attrs.get("padding", 0)) if op.op_type in ("conv2d_3x3", "conv2d_1x1") else 0
            layer = fn(op, wir.hardware,
                       tensor_base=dram_cursor,
                       input_nhwc_packs=input_nhwc_packs_map.get(i, 0),
                       use_runtime_load_pad=input_pad > 0)

        layers.append(layer)

        # E009: SPM/AGU mode consistency.
        _assert_spm_agu_consistency(layer)

        # Advance cursor past this layer's last allocation
        tp = layer.tiling_params
        end_candidates = [
            tp.dram_output_base + _output_total_bytes(tp),
        ]
        if tp.dram_bias_base > 0:
            end_candidates.append(tp.dram_bias_base + _bias_total_bytes(tp))
        dram_cursor = max(end_candidates)
        dram_cursor = (dram_cursor + 15) & ~15

    return HardwareIR(
        workload_name=wir.name,
        hardware=wir.hardware,
        layers=layers,
        relayouts=relayouts,
    )


def _output_total_bytes(tp: TilingParams) -> int:
    """Total output region size from tile-packed layout."""
    last_offset = 0
    if tp.num_oc_tiles > 0:
        last_offset += (tp.num_oc_tiles - 1) * tp.dram_out_oc_stride
    if tp.num_h_tiles > 0:
        last_offset += (tp.num_h_tiles - 1) * tp.dram_out_h_stride
    if tp.num_w_tiles > 0:
        last_offset += (tp.num_w_tiles - 1) * tp.dram_out_w_stride
    return last_offset + tp.dma_plo_words * PKT_SIZE


def _nhwc_pad_head_bytes(width: int, channels: int, padding: int) -> int:
    if padding <= 0:
        return 0
    padded_w = width + 2 * padding
    return (padding * padded_w + padding) * channels * 2


def _nhwc_padded_tensor_bytes(n: int, height: int, width: int,
                              channels: int, padding: int) -> int:
    return n * (height + 2 * padding) * (width + 2 * padding) * channels * 2


def _bias_total_bytes(tp: TilingParams) -> int:
    """Total bias/zero region size."""
    last_offset = 0
    if tp.num_oc_tiles > 0:
        last_offset += (tp.num_oc_tiles - 1) * tp.dram_bias_oc_stride
    if tp.num_h_tiles > 0:
        last_offset += (tp.num_h_tiles - 1) * tp.dram_bias_h_stride
    return last_offset + tp.dma_pli_words * PKT_SIZE
