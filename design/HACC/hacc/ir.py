from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum, IntFlag
from typing import Union


class OpType(IntEnum):
    CONV2D = 0
    GEMM = 1


class DataType(IntEnum):
    INT8 = 0
    INT16 = 1
    FP16 = 2
    FP32 = 3


class BlockFlag(IntFlag):
    BLOCK_FLAG_NONE = 0
    BLOCK_FLAG_NLU_PHASE = 1 << 0
    BLOCK_FLAG_LAST_BLOCK = 1 << 1
    BLOCK_FLAG_FIRST_BLOCK = 1 << 2


@dataclass(slots=True)
class TensorShape:
    n: int = 1
    c: int = 1
    h: int = 1
    w: int = 1

    def numel(self) -> int:
        return self.n * self.c * self.h * self.w


@dataclass(slots=True)
class ConvAttr:
    kernel_h: int = 3
    kernel_w: int = 3
    stride_h: int = 1
    stride_w: int = 1
    pad_h: int = 1
    pad_w: int = 1
    dilation_h: int = 1
    dilation_w: int = 1
    groups: int = 1


@dataclass(slots=True)
class GemmAttr:
    M: int = 0
    N: int = 0
    K: int = 0
    trans_a: bool = False
    trans_b: bool = False


OpAttr = Union[ConvAttr, GemmAttr]


@dataclass(slots=True)
class OpIR:
    name: str = ""
    op_type: OpType = OpType.CONV2D
    dtype: DataType = DataType.INT16
    input: TensorShape = field(default_factory=TensorShape)
    weight: TensorShape = field(default_factory=TensorShape)
    output: TensorShape = field(default_factory=TensorShape)
    attr: OpAttr = field(default_factory=ConvAttr)
    allow_nlu: bool = False

    def conv(self) -> ConvAttr:
        if not isinstance(self.attr, ConvAttr):
            raise TypeError("OpIR attr is not ConvAttr")
        return self.attr

    def gemm(self) -> GemmAttr:
        if not isinstance(self.attr, GemmAttr):
            raise TypeError("OpIR attr is not GemmAttr")
        return self.attr


@dataclass(slots=True)
class ConvTileConfig:
    tile_h: int = 1
    tile_w: int = 1
    tile_ic: int = 1
    tile_oc: int = 1


@dataclass(slots=True)
class GemmTileConfig:
    tile_m: int = 1
    tile_n: int = 1
    tile_k: int = 1


TileConfig = Union[ConvTileConfig, GemmTileConfig]


@dataclass(slots=True)
class ScheduleIR:
    tile: TileConfig = field(default_factory=ConvTileConfig)
    stage_name: str = ""
    num_clusters: int = 1
    cluster_mask: int = 1
    cluster_index: int = -1
    cluster_axis: str = "none"
    loop_extents: list[int] = field(default_factory=lambda: [1, 1, 1, 1])
    loop_starts: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    loop_names: list[str] = field(default_factory=lambda: ["dim0", "dim1", "dim2", "dim3"])
    window_h: int = 0
    window_w: int = 0
    input_h_offset: int = 0
    input_w_offset: int = 0
    activation_buffer_tiles: int = 1
    weight_buffer_tiles: int = 1
    output_buffer_tiles: int = 1

    def total_waves(self) -> int:
        total = 1
        for extent in self.loop_extents:
            total *= extent
        return total

    def conv_tile(self) -> ConvTileConfig:
        if not isinstance(self.tile, ConvTileConfig):
            raise TypeError("ScheduleIR tile is not ConvTileConfig")
        return self.tile

    def gemm_tile(self) -> GemmTileConfig:
        if not isinstance(self.tile, GemmTileConfig):
            raise TypeError("ScheduleIR tile is not GemmTileConfig")
        return self.tile


@dataclass(slots=True)
class AguConfig:
    base_addr: int = 0
    iter0: int = 1
    stride0: int = 1
    iter1: int = 1
    stride1: int = 0
    iter2: int = 1
    stride2: int = 0


@dataclass(slots=True)
class ProfileConfig:
    pe_mode: int = 0
    pe_rows: int = 12
    pe_cols: int = 8
    agu_ifmap: AguConfig = field(default_factory=AguConfig)
    agu_weight: AguConfig = field(default_factory=AguConfig)
    agu_ofmap: AguConfig = field(default_factory=AguConfig)


@dataclass(slots=True)
class DmaRule:
    mode: int = 0
    cluster_mask: int = 1
    word_count: int = 0
    src_addr: int = 0
    dst_addr: int = 0


@dataclass(slots=True)
class NluRule:
    mode: int = 0
    src_addr: int = 0
    dst_addr: int = 0
    word_count: int = 0
    nlu_id: int = 0


@dataclass(slots=True)
class PatchEntry:
    wave_id: int = 0
    valid_mask: int = 0
    profile_idx: int = 0
    dma_idx: int = 0
    cluster_mask: int = 0


@dataclass(slots=True)
class BlockDesc:
    loop_rank: int = 1
    loop_extent: list[int] = field(default_factory=lambda: [1, 1, 1, 1])
    repeat_count: int = 1
    cluster_mask: int = 1
    profile_rule_idx: int = 0
    dma_rule_idx: int = 0
    agu_rule_idx: int = 0
    pe_payload_idx: int = 0
    scan_payload_idx: int = 0
    nlu_rule_idx: int = 0
    rule_stride: int = 0
    nlu_rule_stride: int = 0
    total_waves: int = 1
    patch_begin: int = 0
    patch_count: int = 0
    block_flags: int = int(BlockFlag.BLOCK_FLAG_NONE)


@dataclass(slots=True)
class PayloadSection:
    cluster_mask: int = 0
    needs_sync: bool = False
    words: list[int] = field(default_factory=list)


@dataclass(slots=True)
class JobDesc:
    block_table_base: int = 0
    block_count: int = 0
    profile_table_base: int = 0
    profile_count: int = 0
    dma_table_base: int = 0
    dma_count: int = 0
    agu_table_base: int = 0
    agu_count: int = 0
    nlu_table_base: int = 0
    nlu_count: int = 0
    pe_table_base: int = 0
    pe_count: int = 0
    scan_table_base: int = 0
    scan_count: int = 0
    patch_table_base: int = 0
    patch_count: int = 0
    required_cluster_mask: int = 1
    required_caps: int = 0
    job_flags: int = 0


@dataclass(slots=True)
class HaccPackage:
    core_firmware: list[int] = field(default_factory=list)
    job_section: list[int] = field(default_factory=list)
    block_section: list[int] = field(default_factory=list)
    profile_section: list[int] = field(default_factory=list)
    dma_section: list[int] = field(default_factory=list)
    agu_section: list[int] = field(default_factory=list)
    nlu_section: list[int] = field(default_factory=list)
    pe_section: list[int] = field(default_factory=list)
    scan_section: list[int] = field(default_factory=list)
    patch_section: list[int] = field(default_factory=list)
    debug_json: str = ""


@dataclass(slots=True)
class CompilerContext:
    op: OpIR = field(default_factory=OpIR)
    schedule: ScheduleIR = field(default_factory=ScheduleIR)
    schedules: list[ScheduleIR] = field(default_factory=list)
    profiles: list[ProfileConfig] = field(default_factory=list)
    agus: list[AguConfig] = field(default_factory=list)
    dma_rules: list[DmaRule] = field(default_factory=list)
    nlu_rules: list[NluRule] = field(default_factory=list)
    blocks: list[BlockDesc] = field(default_factory=list)
    patches: list[PatchEntry] = field(default_factory=list)
    pe_payload: PayloadSection = field(default_factory=PayloadSection)
    scan_payload: PayloadSection = field(default_factory=PayloadSection)
    package: HaccPackage = field(default_factory=HaccPackage)