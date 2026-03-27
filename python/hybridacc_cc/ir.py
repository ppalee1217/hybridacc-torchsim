"""IR dataclass definitions for hybridacc-cc compiler.

Defines WorkloadIR (Stage 0 output), HardwareIR (Stage 1 output),
and all supporting data structures as specified in 00_Overview.md §6.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Stage 0: WorkloadIR
# ---------------------------------------------------------------------------


@dataclass
class TensorDesc:
    name: str
    shape: List[int]
    dtype: str  # "fp16"
    layout: str  # "NHWC", "MK", "KN", "MN"


@dataclass
class OpDesc:
    op_type: str  # "conv2d_3x3" | "conv2d_1x1" | "gemm"
    name: str
    inputs: List[TensorDesc]
    outputs: List[TensorDesc]
    attrs: Dict[str, Any] = field(default_factory=dict)


@dataclass
class HardwareDesc:
    num_clusters: int  # 1..16
    num_pes: int  # per cluster (default 64)
    num_bus: int  # per cluster (default 4)
    spm_banks_per_group: int  # (default 3)
    spm_bank_depth: int  # words per bank, each word = 8 bytes (default 4096)
    dram_base: int  # external DRAM base (default 0x80000000)

    @property
    def group_capacity(self) -> int:
        """Per-group linear capacity (bytes) = banks_per_group * bank_depth * 8"""
        return self.spm_banks_per_group * self.spm_bank_depth * 8

    @property
    def half_group_capacity(self) -> int:
        return self.group_capacity // 2

    @property
    def bank_depth_bytes(self) -> int:
        return self.spm_bank_depth * 8

    @property
    def parallel_ping_base(self) -> int:
        """Parallel region ping base = group_linear_words * 8"""
        return self.spm_banks_per_group * self.spm_bank_depth * 8

    @property
    def parallel_pong_base(self) -> int:
        return self.parallel_ping_base + self.bank_depth_bytes // 2

    @property
    def half_parallel(self) -> int:
        return self.bank_depth_bytes // 2


@dataclass
class WorkloadIR:
    name: str
    hardware: HardwareDesc
    ops: List[OpDesc]


# ---------------------------------------------------------------------------
# Stage 1: HardwareIR
# ---------------------------------------------------------------------------


@dataclass
class AguBankConfig:
    base_addr: int
    base_addr_h: int  # reserved, always 0
    iter0: int
    iter1: int
    iter2: int
    iter3: int
    stride0: int
    stride1: int
    stride2: int
    stride3: int
    ctrl: int  # bit0=start, bit3=ultra
    lane_cfg: int
    tag_base: int
    tag_stride0: int
    tag_stride1: int
    tag_ctrl: int
    mask_cfg: int

    def to_regs(self) -> List[int]:
        """Return 15-element register array matching AguRegs in firmware."""
        return [
            self.base_addr,
            self.base_addr_h,
            (self.iter1 << 16) | (self.iter0 & 0xFFFF),  # ITER01
            (self.iter3 << 16) | (self.iter2 & 0xFFFF),  # ITER23
            self.stride0,
            self.stride1,
            self.stride2,
            self.stride3,
            self.ctrl,
            self.lane_cfg,
            self.tag_base,
            self.tag_stride0,
            self.tag_stride1,
            self.tag_ctrl,
            self.mask_cfg,
        ]


@dataclass
class ScanChainEntry:
    ps_id: int
    pd_id: int
    pli_id: int
    plo_id: int
    route_mode: int  # PERouterMode: 0..3
    enable: bool

    def encode(self) -> int:
        """Encode scan-chain entry to 32-bit NoC command word."""
        NOC_CMD_SCAN_CHAIN = 8
        v = (self.ps_id & 0x3F) << 4
        v |= (self.pd_id & 0x3F) << 10
        v |= (self.pli_id & 0x3F) << 16
        v |= (self.plo_id & 0x3F) << 22
        v |= (self.route_mode & 0x03) << 28
        v |= (int(self.enable) & 1) << 30
        return (v & 0xFFFFFFF0) | (NOC_CMD_SCAN_CHAIN & 0x0F)


@dataclass
class PeProgramRef:
    template_name: str
    params: Dict[str, int]


@dataclass
class HdduConfig:
    plane_en: int   # bitmask of enabled planes
    plane_mode: int  # software-defined mode flag


@dataclass
class SpmGroupLayout:
    """Per-group SPM address layout."""
    ping_base: int
    pong_base: int
    size: int  # single buffer size (bytes)
    pingpong: bool = True
    spm_mode: str = "linear"  # "linear" or "parallel"


@dataclass
class SpmPerGroupLayout:
    ps: SpmGroupLayout   # Group 0
    pd: SpmGroupLayout   # Group 1
    pli: SpmGroupLayout  # Group 2
    plo: SpmGroupLayout  # Group 3


@dataclass
class TilingResult:
    """Tiling search result."""
    loop_dims: List[str]
    loop_bounds: Dict[str, int]
    total_waves: int
    reduction_dims: List[str]


@dataclass
class ClusterMapping:
    """Multi-cluster spatial tile assignment."""
    active_clusters: int
    cluster_mask: int
    split_dim: Optional[str]
    tiles_per_cluster: int
    shared_tensor: Optional[str]


@dataclass
class TilingParams:
    """Compile-time tiling constants for firmware runtime computation.

    Replaces static WaveConfig[] + DmaTransferDesc[] arrays.
    Firmware uses base + idx * stride to compute per-wave configs at runtime.
    """
    # Loop bounds
    num_oc_tiles: int
    num_h_tiles: int
    num_w_tiles: int   # GEMM: 1
    num_ic_tiles: int  # Conv2D: IC, GEMM: K (reduction dim)

    # SPM ping/pong base per group [PS, PD, PLI, PLO]
    spm_ping: List[int]
    spm_pong: List[int]

    # AGU ping/pong base per bank [PS, PD, PLI, PLO]
    agu_ping: List[int]
    agu_pong: List[int]

    # SPM group mapping (PLI/PLO swap)
    spm_map_even: int  # 0xE4
    spm_map_odd: int   # 0xD8

    # DRAM base addresses
    dram_weight_base: int
    dram_input_base: int
    dram_output_base: int

    # DRAM strides (per tile index increment)
    dram_ps_oc_stride: int
    dram_ps_ic_stride: int
    dram_pd_h_stride: int
    dram_pd_w_stride: int
    dram_pd_ic_stride: int
    dram_out_oc_stride: int
    dram_out_h_stride: int
    dram_out_w_stride: int

    # DMA word counts (constant per wave)
    dma_ps_words: int
    dma_pd_words: int
    dma_plo_words: int

    # Weight reuse
    ps_reuse_across_spatial: bool

    # Parallel-mode DMA params
    bank_depth_bytes: int
    parallel_groups: int   # bitmask
    dma_ps_words_per_bank: int
    dma_plo_words_per_bank: int


@dataclass
class LayerHwConfig:
    name: str
    op_type: str

    # Cluster config
    target_cluster_mask: int
    spm_config_map: int  # default 0xE4

    # HDDU
    hddu: HdduConfig
    agu_ps: AguBankConfig
    agu_pd: AguBankConfig
    agu_pli: AguBankConfig
    agu_plo: AguBankConfig

    # NoC
    scan_chain: List[ScanChainEntry]
    pe_program: PeProgramRef

    # SPM per-group layout
    spm_layout: SpmPerGroupLayout

    # Tiling
    tiling: TilingResult

    # Cluster mapping
    cluster_mapping: ClusterMapping

    # Tiling parameters (compile-time constants)
    tiling_params: TilingParams


@dataclass
class HardwareIR:
    workload_name: str
    hardware: HardwareDesc
    layers: List[LayerHwConfig]
