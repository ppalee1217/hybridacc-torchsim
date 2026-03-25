from __future__ import annotations

from dataclasses import dataclass, field, replace
from pathlib import Path

from .block_builder import BlockBuilder
from .cluster_lowering import ClusterLowering
from .elf_packager import ElfPackager
from .firmware_emitter import FirmwareEmitter
from .frontend import Frontend
from .ir import BlockFlag, CompilerContext, DmaRule, HaccPackage, OpIR, OpType, ScheduleIR
from .nlu_lowering import NluLowering
from .payload_builder import PayloadBuilder
from .planner import HwConstraint, Planner


@dataclass(slots=True)
class CompileResult:
    success: bool = False
    error: str = ""
    package: HaccPackage = field(default_factory=HaccPackage)
    debug_json: str = ""
    context: CompilerContext = field(default_factory=CompilerContext)


class HaccCompiler:
    def __init__(self, hw: HwConstraint | None = None) -> None:
        self.hw = hw or HwConstraint()

    def compile(self, op: OpIR) -> CompileResult:
        result = CompileResult()
        if not Frontend.validate(op):
            result.error = f"Frontend validation failed for: {op.name}"
            return result

        ctx = CompilerContext(op=op)
        ctx.schedules = Planner.plan(op, self.hw)
        if not ctx.schedules:
            result.error = f"Planner returned no schedule for: {op.name}"
            return result

        ctx.schedule = ctx.schedules[0]
        for schedule in ctx.schedules:
            for partitioned_schedule in self.expand_cluster_partitions(schedule):
                stage_ctx = CompilerContext(op=op, schedule=partitioned_schedule, schedules=[partitioned_schedule])
                ClusterLowering.lower(stage_ctx)
                NluLowering.lower(stage_ctx)
                self.append_dma_rules(stage_ctx)
                BlockBuilder.build(stage_ctx)
                self.merge_stage_context(ctx, stage_ctx)

        self.normalize_block_flags(ctx)
        PayloadBuilder.build(ctx)
        FirmwareEmitter.emit(ctx)
        ctx.package.debug_json = ElfPackager.generate_debug_json(ctx.package, ctx)

        result.success = True
        result.package = ctx.package
        result.debug_json = ctx.package.debug_json
        result.context = ctx
        return result

    def append_dma_rules(self, ctx: CompilerContext) -> None:
        op = ctx.op
        schedule = ctx.schedule
        bytes_per_elem = Planner.dtype_bytes(op.dtype)

        for _ in range(schedule.total_waves()):
            if op.op_type == OpType.CONV2D:
                conv = op.conv()
                tile = schedule.conv_tile()
                kernel_h = schedule.window_h or conv.kernel_h
                kernel_w = schedule.window_w or conv.kernel_w
                tile_input_h = ((tile.tile_h - 1) * conv.stride_h) + kernel_h
                tile_input_w = ((tile.tile_w - 1) * conv.stride_w) + kernel_w
                tile_input_elems = tile.tile_ic * tile_input_h * tile_input_w
                tile_weight_elems = tile.tile_oc * tile.tile_ic * kernel_h * kernel_w
                tile_output_elems = tile.tile_oc * tile.tile_h * tile.tile_w
                word_count = (tile_input_elems + tile_weight_elems + tile_output_elems) * bytes_per_elem // 4
            else:
                tile = schedule.gemm_tile()
                word_count = (tile.tile_m * tile.tile_k + tile.tile_k * tile.tile_n) * bytes_per_elem // 4

            ctx.dma_rules.append(DmaRule(cluster_mask=schedule.cluster_mask, word_count=word_count))

    def expand_cluster_partitions(self, schedule: ScheduleIR) -> list[ScheduleIR]:
        if schedule.num_clusters <= 1 or schedule.cluster_axis == "none":
            return [schedule]

        try:
            axis_index = schedule.loop_names.index(schedule.cluster_axis)
        except ValueError:
            return [schedule]

        total_extent = schedule.loop_extents[axis_index]
        if total_extent <= 1:
            return [schedule]

        partition_count = min(schedule.num_clusters, total_extent)
        base = total_extent // partition_count
        rem = total_extent % partition_count
        partitions: list[ScheduleIR] = []
        cursor = schedule.loop_starts[axis_index]

        for cluster_index in range(partition_count):
            local_extent = base + (1 if cluster_index < rem else 0)
            if local_extent <= 0:
                continue

            loop_extents = list(schedule.loop_extents)
            loop_starts = list(schedule.loop_starts)
            loop_extents[axis_index] = local_extent
            loop_starts[axis_index] = cursor
            partitions.append(
                replace(
                    schedule,
                    stage_name=f"{schedule.stage_name}_cluster{cluster_index}",
                    num_clusters=1,
                    cluster_mask=1 << cluster_index,
                    cluster_index=cluster_index,
                    loop_extents=loop_extents,
                    loop_starts=loop_starts,
                )
            )
            cursor += local_extent

        return partitions or [schedule]

    def merge_stage_context(self, target: CompilerContext, stage: CompilerContext) -> None:
        profile_offset = len(target.profiles)
        agu_offset = len(target.agus)
        dma_offset = len(target.dma_rules)
        nlu_offset = len(target.nlu_rules)
        patch_offset = len(target.patches)
        pe_offset = len(target.pe_payload.words)
        scan_offset = len(target.scan_payload.words)

        for block in stage.blocks:
            block.profile_rule_idx += profile_offset
            block.dma_rule_idx += dma_offset
            block.agu_rule_idx += agu_offset
            if stage.nlu_rules:
                block.nlu_rule_idx += nlu_offset
            block.pe_payload_idx += pe_offset
            block.scan_payload_idx += scan_offset
            block.patch_begin += patch_offset

        for patch in stage.patches:
            patch.profile_idx += profile_offset
            patch.dma_idx += dma_offset

        target.profiles.extend(stage.profiles)
        target.agus.extend(stage.agus)
        target.dma_rules.extend(stage.dma_rules)
        target.nlu_rules.extend(stage.nlu_rules)
        target.blocks.extend(stage.blocks)
        target.patches.extend(stage.patches)
        target.pe_payload.words.extend(stage.pe_payload.words)
        target.scan_payload.words.extend(stage.scan_payload.words)

    def normalize_block_flags(self, ctx: CompilerContext) -> None:
        if not ctx.blocks:
            return

        first_flag = int(BlockFlag.BLOCK_FLAG_FIRST_BLOCK)
        last_flag = int(BlockFlag.BLOCK_FLAG_LAST_BLOCK)
        for index, block in enumerate(ctx.blocks):
            block.block_flags &= ~(first_flag | last_flag)
            if index == 0:
                block.block_flags |= first_flag
            if index == len(ctx.blocks) - 1:
                block.block_flags |= last_flag

    def write_outputs(self, result: CompileResult, prefix: str) -> bool:
        if not result.success:
            return False
        prefix_path = Path(prefix)
        elf_path = prefix_path.with_suffix(".hacc.elf")
        json_path = prefix_path.with_suffix(".debug.json")
        ElfPackager.write_elf(result.package, str(elf_path))
        json_path.write_text(result.debug_json, encoding="utf-8")
        return True