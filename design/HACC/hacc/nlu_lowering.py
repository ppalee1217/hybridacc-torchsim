from __future__ import annotations

from .ir import CompilerContext, NluRule, OpType
from .planner import Planner


class NluLowering:
    @staticmethod
    def lower(ctx: CompilerContext) -> None:
        if not ctx.op.allow_nlu:
            return

        schedule = ctx.schedule
        bytes_per_elem = Planner.dtype_bytes(ctx.op.dtype)
        nlu_id = 0
        for d0 in range(schedule.loop_extents[0]):
            for d1 in range(schedule.loop_extents[1]):
                for d2 in range(schedule.loop_extents[2]):
                    for d3 in range(schedule.loop_extents[3]):
                        if ctx.op.op_type == OpType.CONV2D:
                            tile = schedule.conv_tile()
                            global_oh = schedule.loop_starts[0] + d0
                            global_ow = schedule.loop_starts[1] + d1
                            global_oc = schedule.loop_starts[2] + d2
                            valid_tile_h = min(tile.tile_h, max(1, ctx.op.output.h - global_oh * tile.tile_h))
                            valid_tile_w = min(tile.tile_w, max(1, ctx.op.output.w - global_ow * tile.tile_w))
                            valid_tile_oc = min(tile.tile_oc, max(1, ctx.op.output.c - global_oc * tile.tile_oc))
                            tile_elems = valid_tile_oc * valid_tile_h * valid_tile_w
                        else:
                            tile = schedule.gemm_tile()
                            global_m = schedule.loop_starts[0] + d0
                            global_n = schedule.loop_starts[1] + d1
                            valid_tile_m = min(tile.tile_m, max(1, ctx.op.gemm().M - global_m * tile.tile_m))
                            valid_tile_n = min(tile.tile_n, max(1, ctx.op.gemm().N - global_n * tile.tile_n))
                            tile_elems = valid_tile_m * valid_tile_n
                        ctx.nlu_rules.append(
                            NluRule(
                                mode=0,
                                src_addr=0,
                                dst_addr=0,
                                word_count=(tile_elems * bytes_per_elem + 3) // 4,
                                nlu_id=nlu_id % 4,
                            )
                        )
                        nlu_id += 1