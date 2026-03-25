from __future__ import annotations

from .ir import AguConfig, BlockDesc, CompilerContext, DmaRule, JobDesc, NluRule, PatchEntry, ProfileConfig

PROFILE_WORDS = 3 + 3 * 7
AGU_WORDS = 7
DMA_RULE_WORDS = 5
NLU_RULE_WORDS = 5
BLOCK_WORDS = 19
PATCH_WORDS = 5
JOB_WORDS = 19


class PayloadBuilder:
    @staticmethod
    def build(ctx: CompilerContext) -> None:
        package = ctx.package
        package.profile_section = PayloadBuilder.serialize_profiles(ctx.profiles)
        package.agu_section = PayloadBuilder.serialize_agus(ctx.agus)
        package.dma_section = PayloadBuilder.serialize_dma_rules(ctx.dma_rules)
        package.nlu_section = PayloadBuilder.serialize_nlu_rules(ctx.nlu_rules)
        package.block_section = PayloadBuilder.serialize_blocks(ctx.blocks)
        package.patch_section = PayloadBuilder.serialize_patches(ctx.patches)
        package.pe_section = list(ctx.pe_payload.words)
        package.scan_section = list(ctx.scan_payload.words)

        job = JobDesc()
        cursor = JOB_WORDS * 4

        job.block_table_base = cursor
        job.block_count = len(ctx.blocks)
        cursor += len(package.block_section) * 4

        job.profile_table_base = cursor
        job.profile_count = len(ctx.profiles)
        cursor += len(package.profile_section) * 4

        job.dma_table_base = cursor
        job.dma_count = len(ctx.dma_rules)
        cursor += len(package.dma_section) * 4

        job.agu_table_base = cursor
        job.agu_count = len(ctx.agus)
        required_cluster_mask = 0
        for block in ctx.blocks:
            required_cluster_mask |= block.cluster_mask
        job.required_cluster_mask = required_cluster_mask or ctx.schedule.cluster_mask

        job.nlu_table_base = cursor
        job.nlu_count = len(ctx.nlu_rules)
        cursor += len(package.nlu_section) * 4

        job.pe_table_base = cursor
        job.pe_count = len(package.pe_section)
        cursor += len(package.pe_section) * 4

        job.scan_table_base = cursor
        job.scan_count = len(package.scan_section)
        cursor += len(package.scan_section) * 4

        job.patch_table_base = cursor
        job.patch_count = len(ctx.patches)
        job.required_cluster_mask = ctx.schedule.cluster_mask
        job.required_caps = 0x3F
        package.job_section = PayloadBuilder.serialize_job(job)

    @staticmethod
    def serialize_job(job: JobDesc) -> list[int]:
        return [
            job.block_table_base,
            job.block_count,
            job.profile_table_base,
            job.profile_count,
            job.dma_table_base,
            job.dma_count,
            job.agu_table_base,
            job.agu_count,
            job.nlu_table_base,
            job.nlu_count,
            job.pe_table_base,
            job.pe_count,
            job.scan_table_base,
            job.scan_count,
            job.patch_table_base,
            job.patch_count,
            job.required_cluster_mask,
            job.required_caps,
            job.job_flags,
        ]

    @staticmethod
    def serialize_blocks(blocks: list[BlockDesc]) -> list[int]:
        output: list[int] = []
        for block in blocks:
            output.append(block.loop_rank)
            output.extend(block.loop_extent)
            output.extend(
                [
                    block.repeat_count,
                    block.cluster_mask,
                    block.profile_rule_idx,
                    block.dma_rule_idx,
                    block.agu_rule_idx,
                    block.pe_payload_idx,
                    block.scan_payload_idx,
                    block.nlu_rule_idx,
                    block.rule_stride,
                    block.nlu_rule_stride,
                    block.total_waves,
                    block.patch_begin,
                    block.patch_count,
                    block.block_flags,
                ]
            )
        return output

    @staticmethod
    def serialize_profiles(profiles: list[ProfileConfig]) -> list[int]:
        output: list[int] = []
        for profile in profiles:
            output.extend([profile.pe_mode, profile.pe_rows, profile.pe_cols])
            for agu in (profile.agu_ifmap, profile.agu_weight, profile.agu_ofmap):
                output.extend(PayloadBuilder.serialize_single_agu(agu))
        return output

    @staticmethod
    def serialize_single_agu(agu: AguConfig) -> list[int]:
        return [agu.base_addr, agu.iter0, agu.stride0, agu.iter1, agu.stride1, agu.iter2, agu.stride2]

    @staticmethod
    def serialize_agus(agus: list[AguConfig]) -> list[int]:
        output: list[int] = []
        for agu in agus:
            output.extend(PayloadBuilder.serialize_single_agu(agu))
        return output

    @staticmethod
    def serialize_dma_rules(rules: list[DmaRule]) -> list[int]:
        output: list[int] = []
        for rule in rules:
            output.extend([rule.mode, rule.cluster_mask, rule.word_count, rule.src_addr, rule.dst_addr])
        return output

    @staticmethod
    def serialize_nlu_rules(rules: list[NluRule]) -> list[int]:
        output: list[int] = []
        for rule in rules:
            output.extend([rule.mode, rule.src_addr, rule.dst_addr, rule.word_count, rule.nlu_id])
        return output

    @staticmethod
    def serialize_patches(patches: list[PatchEntry]) -> list[int]:
        output: list[int] = []
        for patch in patches:
            output.extend([patch.wave_id, patch.valid_mask, patch.profile_idx, patch.dma_idx, patch.cluster_mask])
        return output