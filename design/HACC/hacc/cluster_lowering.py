from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
import json
import os
from typing import Tuple

from .ir import AguConfig, CompilerContext, OpType, PayloadSection, ProfileConfig, OpIR
from .planner import Planner

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PE_KERNEL_JSON_DIR = os.path.join(PROJECT_ROOT, "kernel", "json")
MAX_TEMPLATE_LOOP_TRIP_COUNT = 0x400


@dataclass(frozen=True)
class LoopFactorization:
    inner_count: int
    outer_count: int


class BaseOpLowering(ABC):
    @staticmethod
    def load_pe_kernel_template(template_path: str) -> dict:
        with open(template_path, "r", encoding="utf-8") as template_file:
            return json.load(template_file)

    @staticmethod
    def encode_template_parameter(word: int, disasm: str, value: int) -> int:
        if disasm.startswith(("LDMA.ADDR", "SDMA.ADDR")):
            encoded_value = value
        elif disasm.startswith(("LDMA.LEN", "SDMA.LEN", "LDMA.LOOP", "SDMA.LOOP", "LOOPIN")):
            if value <= 0:
                raise ValueError(f"Template parameter must be positive for instruction: {disasm}")
            encoded_value = value - 1
        else:
            raise ValueError(f"Unsupported template patch instruction: {disasm}")

        if not 0 <= encoded_value <= 0x3FF:
            raise ValueError(f"Template parameter out of 10-bit range: {value} for instruction {disasm}")

        return (word & 0x3F) | (encoded_value << 6)

    @classmethod
    def materialize_template_payload(cls, template: dict, parameters: dict[str, int]) -> list[int]:
        parameter_defaults = {
            entry["index"]: {"name": entry["name"], "default": entry["default"]}
            for entry in template["parameters"]
        }
        instructions = [int(entry["word"], 16) for entry in template["instructions"]]

        for patch in template["patches"]:
            param = parameter_defaults[patch["param_index"]]
            instruction = template["instructions"][patch["offset"]]
            actual_value = parameters.get(param["name"], param["default"])
            instructions[patch["offset"]] = cls.encode_template_parameter(
                instructions[patch["offset"]],
                instruction["disasm"],
                actual_value,
            )

        return instructions

    @staticmethod
    def factorize_loop_trip_count(total_count: int, loop_name: str) -> LoopFactorization:
        if total_count <= 0:
            raise ValueError(f"{loop_name} must be positive: {total_count}")
        if total_count <= MAX_TEMPLATE_LOOP_TRIP_COUNT:
            return LoopFactorization(inner_count=total_count, outer_count=1)

        # Prefer the largest valid inner loop first so the steady-state body runs
        # as long as possible before paying the outer-loop control overhead.
        # Both counters must fit the template's 10-bit immediate field, and the
        # product must match exactly because the PE program is static.
        for inner_count in range(MAX_TEMPLATE_LOOP_TRIP_COUNT, 0, -1):
            if total_count % inner_count != 0:
                continue

            outer_count = total_count // inner_count
            if outer_count <= MAX_TEMPLATE_LOOP_TRIP_COUNT:
                return LoopFactorization(inner_count=inner_count, outer_count=outer_count)

        raise ValueError(
            f"{loop_name}={total_count} cannot be represented exactly with nested 10-bit loop counters"
        )

    def get_pe_kernel_template(self, ctx: CompilerContext) -> str | None:
        template_file = self.template_filename(ctx)
        if template_file is None:
            return None

        template_path = os.path.join(PE_KERNEL_JSON_DIR, template_file)
        if not os.path.exists(template_path):
            return None
        return template_path

    def gen_pe_payload(self, ctx: CompilerContext) -> PayloadSection:
        template_path = self.get_pe_kernel_template(ctx)
        if template_path is None:
            print("Warning: Using legacy PE payload generation. Consider adding a JSON template for better maintainability.")
            return PayloadSection(cluster_mask=ctx.schedule.cluster_mask, words=[0xDEADBEEF])  # Placeholder for legacy payload

        kernel_template = self.load_pe_kernel_template(template_path)
        payload = PayloadSection(cluster_mask=ctx.schedule.cluster_mask)
        payload.words = self.materialize_template_payload(
            kernel_template,
            self.resolve_template_parameters(ctx),
        )
        return payload

    @abstractmethod
    def template_filename(self, ctx: CompilerContext) -> str | None:
        raise NotImplementedError

    @abstractmethod
    def resolve_template_parameters(self, ctx: CompilerContext) -> dict[str, int]:
        raise NotImplementedError

    @abstractmethod
    def compute_profile(
        self,
        ctx: CompilerContext,
        d0: int,
        d1: int,
        d2: int,
        d3: int,
    ) -> Tuple[AguConfig, AguConfig, AguConfig, int, int]:
        raise NotImplementedError


class Conv2dLowering(BaseOpLowering):
    def template_filename(self, ctx: CompilerContext) -> str | None:
        tile = ctx.schedule.conv_tile()
        conv = ctx.op.conv()
        return f"conv1d_k{ctx.schedule.window_w or conv.kernel_w}c{tile.tile_ic}s{conv.stride_w}.json"

    def resolve_template_parameters(self, ctx: CompilerContext) -> dict[str, int]:
        tile = ctx.schedule.conv_tile()
        conv = ctx.op.conv()
        bytes_per_elem = Planner.dtype_bytes(ctx.op.dtype)
        kernel_w = ctx.schedule.window_w or conv.kernel_w
        kernel_loop = self.factorize_loop_trip_count(
            ctx.schedule.total_waves(),
            "kernel sets",
        )

        return {
            "KERNEL_DMA_LEN": (tile.tile_oc * tile.tile_ic * kernel_w * bytes_per_elem) // 8,
            "OUTPUT_WINDOW_CNT_MINUS_ONE": tile.tile_w - 1,
            "KERNEL_COUNT": tile.tile_oc,
            "KERNEL_LOOP_INNER": kernel_loop.inner_count,
            "KERNEL_LOOP_OUTER": kernel_loop.outer_count,
        }

    def compute_profile(
        self,
        ctx: CompilerContext,
        oh_idx: int,
        ow_idx: int,
        oc_idx: int,
        ic_idx: int,
    ) -> Tuple[AguConfig, AguConfig, AguConfig, int, int]:
        tile = ctx.schedule.conv_tile()
        op = ctx.op
        conv = op.conv()
        bytes_per_elem = Planner.dtype_bytes(op.dtype)
        kernel_h = ctx.schedule.window_h or conv.kernel_h
        kernel_w = ctx.schedule.window_w or conv.kernel_w
        global_oh = ctx.schedule.loop_starts[0] + oh_idx
        global_ow = ctx.schedule.loop_starts[1] + ow_idx
        global_oc = ctx.schedule.loop_starts[2] + oc_idx
        global_ic = ctx.schedule.loop_starts[3] + ic_idx
        valid_tile_h = min(tile.tile_h, max(1, op.output.h - global_oh * tile.tile_h))
        valid_tile_w = min(tile.tile_w, max(1, op.output.w - global_ow * tile.tile_w))
        valid_tile_oc = min(tile.tile_oc, max(1, op.output.c - global_oc * tile.tile_oc))
        valid_tile_ic = min(tile.tile_ic, max(1, op.input.c - global_ic * tile.tile_ic))

        in_row_start = ctx.schedule.input_h_offset + global_oh * tile.tile_h * conv.stride_h
        in_col_start = ctx.schedule.input_w_offset + global_ow * tile.tile_w * conv.stride_w
        in_ch_start = global_ic * tile.tile_ic
        oc_start = global_oc * tile.tile_oc
        input_window_h = (valid_tile_h - 1) * conv.stride_h + kernel_h
        input_window_w = (valid_tile_w - 1) * conv.stride_w + kernel_w

        print(f"Profile for OH={global_oh}, OW={global_ow}, OC={global_oc}, IC={global_ic}:")
        print(f"  Valid tile size: {valid_tile_h}x{valid_tile_w}x{valid_tile_oc}x{valid_tile_ic}")
        print(f"  Input window size: {input_window_h}x{input_window_w}")
        print(f"  Ifmap base address: {(in_ch_start * op.input.h * op.input.w + in_row_start * op.input.w + in_col_start) * bytes_per_elem:#010x}")
        print(f"  Weight base address: {((oc_start * op.input.c + in_ch_start) * kernel_h * kernel_w + ctx.schedule.input_h_offset * conv.kernel_w) * bytes_per_elem:#010x}")
        print(f"  Ofmap base address: {(oc_start * op.output.h * op.output.w + global_oh * tile.tile_h * op.output.w + global_ow * tile.tile_w) * bytes_per_elem:#010x}")


        ifmap_agu = AguConfig(
            base_addr=(in_ch_start * op.input.h * op.input.w + in_row_start * op.input.w + in_col_start) * bytes_per_elem,
            iter0=input_window_w,
            stride0=bytes_per_elem,
            iter1=input_window_h,
            stride1=op.input.w * bytes_per_elem,
            iter2=valid_tile_ic,
            stride2=op.input.h * op.input.w * bytes_per_elem,
        )
        weight_kernel_plane = conv.kernel_h * conv.kernel_w
        weight_base_elems = ((oc_start * op.input.c + in_ch_start) * weight_kernel_plane) + ctx.schedule.input_h_offset * conv.kernel_w
        weight_agu = AguConfig(
            base_addr=weight_base_elems * bytes_per_elem,
            iter0=kernel_w,
            stride0=bytes_per_elem,
            iter1=kernel_h,
            stride1=conv.kernel_w * bytes_per_elem,
            iter2=valid_tile_oc * valid_tile_ic,
            stride2=weight_kernel_plane * bytes_per_elem,
        )
        ofmap_base_elems = oc_start * op.output.h * op.output.w + global_oh * tile.tile_h * op.output.w + global_ow * tile.tile_w
        ofmap_agu = AguConfig(
            base_addr=ofmap_base_elems * bytes_per_elem,
            iter0=valid_tile_w,
            stride0=bytes_per_elem,
            iter1=valid_tile_h,
            stride1=op.output.w * bytes_per_elem,
            iter2=valid_tile_oc,
            stride2=op.output.h * op.output.w * bytes_per_elem,
        )
        return ifmap_agu, weight_agu, ofmap_agu, valid_tile_oc, valid_tile_ic


class GemmLowering(BaseOpLowering):
    def template_filename(self, ctx: CompilerContext) -> str | None:
        return "gemm.json"

    def resolve_template_parameters(self, ctx: CompilerContext) -> dict[str, int]:
        tile = ctx.schedule.gemm_tile()
        bytes_per_elem = Planner.dtype_bytes(ctx.op.dtype)
        vector_bytes = 8

        return {
            "KERNEL_DMA_STORE_LEN": max(1, (tile.tile_k * tile.tile_n * bytes_per_elem) // vector_bytes),
            "KERNEL_DMA_LOAD_LEN": max(1, (tile.tile_m * tile.tile_k * bytes_per_elem) // vector_bytes),
            "INPUT_DIM": tile.tile_m,
            "OUTPUT_DIM": tile.tile_n,
            "PSUM_COUNT": max(1, tile.tile_m * 2),
            "NUM_OF_KERNEL_SETS": tile.tile_k,
            "NUM_OF_N_TILES": max(1, tile.tile_n // 4),
            "NUM_OF_M_TILES": max(1, tile.tile_m // 6),
            "K_TILE_DIM": tile.tile_k,
        }

    def compute_profile(
        self,
        ctx: CompilerContext,
        m_idx: int,
        n_idx: int,
        k_idx: int,
        _unused_idx: int,
    ) -> Tuple[AguConfig, AguConfig, AguConfig, int, int]:
        tile = ctx.schedule.gemm_tile()
        op = ctx.op
        gemm = op.gemm()
        bytes_per_elem = Planner.dtype_bytes(op.dtype)
        global_m = ctx.schedule.loop_starts[0] + m_idx
        global_n = ctx.schedule.loop_starts[1] + n_idx
        global_k = ctx.schedule.loop_starts[2] + k_idx
        valid_tile_m = min(tile.tile_m, max(1, gemm.M - global_m * tile.tile_m))
        valid_tile_n = min(tile.tile_n, max(1, gemm.N - global_n * tile.tile_n))
        valid_tile_k = min(tile.tile_k, max(1, gemm.K - global_k * tile.tile_k))

        m_start = global_m * tile.tile_m
        n_start = global_n * tile.tile_n
        k_start = global_k * tile.tile_k

        ifmap_agu = AguConfig(
            base_addr=(m_start * gemm.K + k_start) * bytes_per_elem,
            iter0=valid_tile_m,
            stride0=gemm.K * bytes_per_elem,
            iter1=valid_tile_k,
            stride1=bytes_per_elem,
        )
        weight_agu = AguConfig(
            base_addr=(k_start * gemm.N + n_start) * bytes_per_elem,
            iter0=valid_tile_k,
            stride0=gemm.N * bytes_per_elem,
            iter1=valid_tile_n,
            stride1=bytes_per_elem,
        )
        ofmap_agu = AguConfig(
            base_addr=(m_start * gemm.N + n_start) * bytes_per_elem,
            iter0=valid_tile_m,
            stride0=gemm.N * bytes_per_elem,
            iter1=valid_tile_n,
            stride1=bytes_per_elem,
        )
        return ifmap_agu, weight_agu, ofmap_agu, valid_tile_m, valid_tile_n


class ClusterLowering:

    # Operation Loop Up Table
    lowering_table = {
        OpType.CONV2D: Conv2dLowering(),
        OpType.GEMM: GemmLowering(),
    }

    @staticmethod
    def lower(ctx: CompilerContext) -> None:
        op = ctx.op
        schedule = ctx.schedule
        lowering = ClusterLowering.lowering_table[op.op_type]

        ctx.pe_payload = lowering.gen_pe_payload(ctx)
        ctx.scan_payload.cluster_mask = schedule.cluster_mask
        ctx.scan_payload.words = [0x00000001, schedule.cluster_mask, 0x00000000]

        for d0 in range(schedule.loop_extents[0]):
            for d1 in range(schedule.loop_extents[1]):
                for d2 in range(schedule.loop_extents[2]):
                    for d3 in range(schedule.loop_extents[3]):
                        agu_ifmap, agu_weight, agu_ofmap, pe_rows, pe_cols = lowering.compute_profile(ctx, d0, d1, d2, d3)

                        ctx.agus.append(agu_ifmap)
                        ctx.profiles.append(
                            ProfileConfig(
                                pe_rows=pe_rows,
                                pe_cols=pe_cols,
                                agu_ifmap=agu_ifmap,
                                agu_weight=agu_weight,
                                agu_ofmap=agu_ofmap,
                            )
                        )

        print(f"Total profiles generated: {len(ctx.profiles)}")