from __future__ import annotations

from dataclasses import dataclass
from typing import List

from .ir import ConvTileConfig, DataType, GemmTileConfig, OpIR, OpType, ScheduleIR


CONV_CHANNELS_PER_PACKET = {
    1: 12,
    3: 4,
    5: 2,
    7: 1,
}

# For simplicity, we set a fixed maximum number of output channels per packet for convolution operations. In a real implementation, this could be determined based on the hardware capabilities and the specific convolution parameters (e.g., kernel size, stride, etc.) to maximize efficiency while ensuring that the data fits within the constraints of the hardware buffers and compute units.
CONV_OUTPUT_CHANNELS_PER_PACKET = 16

PE_ACTIVE_COLS = {
    OpType.CONV2D: 14,
    OpType.GEMM: 16,
}

@dataclass(slots=True)
class HwConstraint:
    pe_array_rows: int = 3 # Multicast bus counts per cluster
    max_pe_array_cols: int = 16 # PE counts per Multicast bus
    mac_per_pe: int = 4 # MAC units per PE
    num_clusters: int = 2 # Number of clusters available
    spm_bank_size_kb: int = 64 # Size of each SPM bank in KB
    spm_num_banks_per_group: int = 3 # Number of SPM banks per group
    spm_num_groups_per_cluster: int = 4 # Number of SPM bank groups per cluster
    bw_dram_to_spm_bpc: int = 64 # Bandwidth from DRAM to SPM in bytes per cycle

    def pe_array_cols(self, op_type: OpType) -> int:
        return min(self.max_pe_array_cols, PE_ACTIVE_COLS.get(op_type, self.max_pe_array_cols))

    @property
    def spm_size_bytes(self) -> int:
        return self.spm_bank_size_kb * 1024 * self.spm_num_banks_per_group * self.spm_num_groups_per_cluster

    @property
    def spm_group_size_bytes(self) -> int:
        return self.spm_bank_size_kb * 1024 * self.spm_num_banks_per_group

    @property
    def spm_group_buffer_bytes(self) -> int:
        return self.spm_group_size_bytes // 2


class Planner:
    @staticmethod
    def ceil_div(a: int, b: int) -> int:
        return (a + b - 1) // b

    @staticmethod
    def dtype_bytes(dtype: DataType) -> int:
        if dtype == DataType.INT8:
            return 1
        if dtype in (DataType.INT16, DataType.FP16):
            return 2
        return 4

    @staticmethod
    def conv_channels_per_packet(kernel_w: int) -> int:
        return CONV_CHANNELS_PER_PACKET.get(kernel_w, 1)

    @staticmethod
    def choose_conv_cluster_axis(loop_oc: int, loop_ow: int, loop_oh: int, hw: HwConstraint) -> tuple[str, int]:
        if loop_oc > 1:
            return "tile_oc", min(hw.num_clusters, loop_oc)
        if loop_ow > 1:
            return "tile_ow", min(hw.num_clusters, loop_ow)
        if loop_oh > 1:
            return "tile_oh", min(hw.num_clusters, loop_oh)
        return "none", 1

    @staticmethod
    def choose_gemm_cluster_axis(loop_m: int, loop_n: int, hw: HwConstraint) -> tuple[str, int]:
        if loop_n > 1:
            return "tile_n", min(hw.num_clusters, loop_n)
        if loop_m > 1:
            return "tile_m", min(hw.num_clusters, loop_m)
        return "none", 1

    @staticmethod
    def gemm_tile_m_capacity(hw: HwConstraint) -> int:
        return max(1, hw.pe_array_rows * hw.mac_per_pe)

    @staticmethod
    def gemm_tile_n_capacity(hw: HwConstraint) -> int:
        return max(1, hw.pe_array_cols(OpType.GEMM)  // 2)

    @staticmethod
    def gemm_tile_k_capacity(hw: HwConstraint) -> int:
        return max(1, hw.pe_array_cols(OpType.GEMM) * max(1, hw.mac_per_pe // 2))

    @staticmethod
    def choose_cluster_axis(hw: HwConstraint, axis_tiles: list[tuple[str, int]]) -> tuple[str, int]:
        best_axis = "none"
        best_tiles = 1
        for axis_name, tile_count in axis_tiles:
            if tile_count > best_tiles:
                best_axis = axis_name
                best_tiles = tile_count

        if best_tiles <= 1:
            return "none", 1
        return best_axis, min(hw.num_clusters, best_tiles)

    @staticmethod
    def split_conv_kernel(kernel_h: int, max_kernel_h: int) -> list[tuple[int, int]]:
        if kernel_h <= 0:
            return [(0, 0)]

        splits: list[tuple[int, int]] = []
        offset = 0
        while offset < kernel_h:
            split_h = min(max_kernel_h, kernel_h - offset)
            splits.append((offset, split_h))
            offset += split_h
        return splits

    @staticmethod
    def choose_conv_spatial_tile(op: OpIR, hw: HwConstraint, split_kh: int, tile: ConvTileConfig) -> tuple[int, int]:
        conv = op.conv()
        bytes_per_elem = Planner.dtype_bytes(op.dtype)
        input_budget = hw.spm_group_buffer_bytes
        output_budget = hw.spm_group_buffer_bytes
        weight_budget = hw.spm_group_buffer_bytes
        max_tile_h = min(op.output.h, hw.pe_array_cols(OpType.CONV2D))
        max_tile_w = op.output.w
        weight_mem = tile.tile_oc * tile.tile_ic * split_kh * conv.kernel_w * bytes_per_elem

        # Since weights are stationary, we need to ensure the tile size fits in the weight buffer first. If it doesn't fit, we have no choice but to reduce the tile size, even if it means underutilizing the compute units.
        assert weight_mem <= weight_budget, "Tile size too large for weight buffer"

        # We iterate over possible tile heights starting from the maximum possible (which would fully utilize the PE array) down to 1, and for each tile height, we calculate the maximum tile width that would fit in both the input and output buffers. We choose the largest tile width that fits within the constraints.
        for tile_h in range(max_tile_h, 0, -1):
            in_h = (tile_h - 1) * conv.stride_h + split_kh
            input_row_bytes = tile.tile_ic * in_h * bytes_per_elem
            output_row_bytes = tile.tile_oc * tile_h * bytes_per_elem

            if input_row_bytes <= 0 or output_row_bytes <= 0:
                continue

            max_input_width = input_budget // input_row_bytes
            if max_input_width < conv.kernel_w:
                continue

            max_tile_w_from_input = ((max_input_width - conv.kernel_w) // conv.stride_w) + 1
            max_tile_w_from_output = output_budget // output_row_bytes
            tile_w = min(max_tile_w, max_tile_w_from_input, max_tile_w_from_output)
            if tile_w > 0:
                return tile_h, tile_w

        # warning: if we can't find any tile size that fits the constraints, we fall back to a tile size of 1x1, which will result in very poor performance but at least allows the operation to complete rather than failing outright. In practice, this should be avoided by choosing reasonable hardware constraints and/or allowing for some flexibility in tile sizes.
        print("Warning: Could not find a valid tile size that fits the buffer constraints. Falling back to 1x1 tile, which may result in poor performance.")
        return 1, 1

    @staticmethod
    def choose_gemm_tile_k(op: OpIR, hw: HwConstraint, tile: GemmTileConfig) -> int:
        gemm = op.gemm()
        return min(gemm.K, Planner.gemm_tile_k_capacity(hw))

    @staticmethod
    def buffer_tile_count(group_buffer_bytes: int, tile_bytes: int) -> int:
        if tile_bytes <= 0:
            return 1
        return max(1, group_buffer_bytes // tile_bytes)

    @staticmethod
    def plan(op: OpIR, hw: HwConstraint) -> List[ScheduleIR]:
        if op.op_type == OpType.CONV2D:
            return Planner.plan_conv2d(op, hw)
        elif op.op_type == OpType.GEMM:
            return Planner.plan_gemm(op, hw)
        else:
            raise ValueError(f"Unsupported op type: {op.op_type}")

    @staticmethod
    def plan_conv2d(op: OpIR, hw: HwConstraint) -> List[ScheduleIR]:
        conv = op.conv()
        schedules: List[ScheduleIR] = []
        channels_per_packet = Planner.conv_channels_per_packet(conv.kernel_w)
        kernel_splits = Planner.split_conv_kernel(conv.kernel_h, hw.pe_array_rows)

        for split_idx, (input_h_offset, split_kh) in enumerate(kernel_splits):
            tile = ConvTileConfig()
            tile.tile_oc = max(1, min(op.output.c, CONV_OUTPUT_CHANNELS_PER_PACKET))
            tile.tile_ic = max(1, min(op.input.c, channels_per_packet))
            tile.tile_h, tile.tile_w = Planner.choose_conv_spatial_tile(op, hw, split_kh, tile)

            loop_oh = Planner.ceil_div(op.output.h, tile.tile_h)
            loop_ow = Planner.ceil_div(op.output.w, tile.tile_w)
            loop_oc = Planner.ceil_div(op.output.c, tile.tile_oc)
            loop_ic = Planner.ceil_div(op.input.c, tile.tile_ic)
            cluster_axis, num_clusters = Planner.choose_conv_cluster_axis(loop_oc, loop_ow, loop_oh, hw)

            bytes_per_elem = Planner.dtype_bytes(op.dtype)
            input_tile_bytes = tile.tile_ic * (((tile.tile_h - 1) * conv.stride_h) + split_kh) * (((tile.tile_w - 1) * conv.stride_w) + conv.kernel_w) * bytes_per_elem
            weight_tile_bytes = tile.tile_oc * tile.tile_ic * split_kh * conv.kernel_w * bytes_per_elem
            output_tile_bytes = tile.tile_oc * tile.tile_h * tile.tile_w * bytes_per_elem

            schedule = ScheduleIR(
                tile=tile,
                stage_name=f"conv2d_split_{split_idx}",
                num_clusters=num_clusters,
                cluster_mask=(1 << num_clusters) - 1,
                cluster_index=-1,
                cluster_axis=cluster_axis,
                loop_extents=[loop_oh, loop_ow, loop_oc, loop_ic],
                loop_starts=[0, 0, 0, 0],
                loop_names=["tile_oh", "tile_ow", "tile_oc", "tile_ic"],
                window_h=split_kh,
                window_w=conv.kernel_w,
                input_h_offset=input_h_offset,
                input_w_offset=0,
                activation_buffer_tiles=Planner.buffer_tile_count(hw.spm_group_buffer_bytes, input_tile_bytes),
                weight_buffer_tiles=Planner.buffer_tile_count(hw.spm_group_buffer_bytes, weight_tile_bytes),
                output_buffer_tiles=Planner.buffer_tile_count(hw.spm_group_buffer_bytes, output_tile_bytes),
            )

            schedules.append(schedule)

        return schedules

    @staticmethod
    def plan_gemm(op: OpIR, hw: HwConstraint) -> List[ScheduleIR]:
        tile = GemmTileConfig()
        gemm = op.gemm()
        tile.tile_m = min(gemm.M, Planner.gemm_tile_m_capacity(hw))
        tile.tile_n = min(gemm.N, Planner.gemm_tile_n_capacity(hw))
        tile.tile_k = Planner.choose_gemm_tile_k(op, hw, tile)

        loop_m = Planner.ceil_div(gemm.M, tile.tile_m)
        loop_n = Planner.ceil_div(gemm.N, tile.tile_n)
        loop_k = Planner.ceil_div(gemm.K, tile.tile_k)
        cluster_axis, num_clusters = Planner.choose_gemm_cluster_axis(loop_m, loop_n, hw)

        bytes_per_elem = Planner.dtype_bytes(op.dtype)
        activation_tile_bytes = tile.tile_m * tile.tile_k * bytes_per_elem
        weight_tile_bytes = tile.tile_k * tile.tile_n * bytes_per_elem
        output_tile_bytes = tile.tile_m * tile.tile_n * bytes_per_elem

        schedule = ScheduleIR(
            tile=tile,
            stage_name="gemm_main",
            num_clusters=num_clusters,
            cluster_mask=(1 << num_clusters) - 1,
            cluster_index=-1,
            cluster_axis=cluster_axis,
            loop_extents=[loop_m, loop_n, loop_k, 1],
            loop_starts=[0, 0, 0, 0],
            loop_names=["tile_m", "tile_n", "tile_k", "unused"],
            window_h=tile.tile_k,
            window_w=tile.tile_n,
            activation_buffer_tiles=Planner.buffer_tile_count(hw.spm_group_buffer_bytes, activation_tile_bytes),
            weight_buffer_tiles=Planner.buffer_tile_count(hw.spm_group_buffer_bytes, weight_tile_bytes),
            output_buffer_tiles=Planner.buffer_tile_count(hw.spm_group_buffer_bytes, output_tile_bytes),
        )
        return [schedule]