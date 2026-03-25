from __future__ import annotations

from .ir import BlockDesc, BlockFlag, CompilerContext, OpType, PatchEntry


class BlockBuilder:
    @staticmethod
    def build(ctx: CompilerContext) -> None:
        schedule = ctx.schedule
        d0, d1, d2, d3 = schedule.loop_extents[:4]
        rank = sum(1 for value in (d0, d1, d2, d3) if value > 1) or 1

        block = BlockDesc(
            loop_rank=rank,
            loop_extent=[d0, d1, d2, d3],
            repeat_count=1,
            cluster_mask=schedule.cluster_mask,
            profile_rule_idx=0,
            dma_rule_idx=0,
            agu_rule_idx=0,
            pe_payload_idx=0,
            scan_payload_idx=0,
            rule_stride=d1 * d2 * d3,
            nlu_rule_stride=0,
            total_waves=d0 * d1 * d2 * d3,
            patch_begin=0,
            patch_count=0,
            block_flags=int(BlockFlag.BLOCK_FLAG_FIRST_BLOCK | BlockFlag.BLOCK_FLAG_LAST_BLOCK),
        )

        if ctx.nlu_rules:
            block.nlu_rule_idx = 0
            block.nlu_rule_stride = d1 * d2 * d3
            block.block_flags |= int(BlockFlag.BLOCK_FLAG_NLU_PHASE)

        if ctx.op.op_type == OpType.CONV2D:
            full_h = ctx.op.output.h
            tile_h = schedule.conv_tile().tile_h
            global_last_tile_oh = (full_h - 1) // tile_h
            local_last_tile_oh = global_last_tile_oh - schedule.loop_starts[0]
            if full_h % tile_h != 0 and 0 <= local_last_tile_oh < d0:
                last_d0 = local_last_tile_oh
                for d1_idx in range(d1):
                    for d2_idx in range(d2):
                        for d3_idx in range(d3):
                            wave_id = ((last_d0 * d1 + d1_idx) * d2 + d2_idx) * d3 + d3_idx
                            ctx.patches.append(PatchEntry(wave_id=wave_id, valid_mask=0x01, profile_idx=wave_id))

        block.patch_count = len(ctx.patches)
        ctx.blocks.append(block)