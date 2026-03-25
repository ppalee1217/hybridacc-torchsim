/**
 * @file block_builder.cpp
 * @brief HACC Compiler Block Builder implementation.
 */
#include "hacc/block_builder.hpp"

namespace hacc {

void BlockBuilder::build(CompilerContext &ctx) {
    const auto &s = ctx.schedule;

    /*
     * Compressed block strategy:
     * Instead of creating one block per d1 iteration (which produces many
     * near-identical blocks differing only in rule index offsets), fold
     * all three loop dimensions (d0, d1, d2) into a single block.
     *
     * The MCU iterates d0 (outermost) -> d1 -> d2 (innermost), matching
     * the cluster-lowering profile/AGU generation order.  Rule indices
     * advance sequentially: profile_idx = base + wave_counter.
     *
     * rule_stride records the per-d0-step wave count (d1 * d2) so the MCU
     * can reconstruct per-dimension indices when needed (NLU, barriers).
     */
    uint32_t d0 = s.loop_extents[0]; /* spatial tiles (conv) / tile_m (gemm) */
    uint32_t d1 = s.loop_extents[1]; /* output channel  (conv) / tile_n (gemm) */
    uint32_t d2 = s.loop_extents[2]; /* input channel   (conv) / tile_k (gemm) */

    BlockDesc blk;

    /* Compute effective loop rank (number of non-trivial dimensions). */
    uint32_t rank = 0;
    if (d0 > 1) ++rank;
    if (d1 > 1) ++rank;
    if (d2 > 1) ++rank;
    blk.loop_rank = (rank > 0) ? rank : 1;

    blk.loop_extent   = {d0, d1, d2, 1};
    blk.repeat_count  = 1;
    blk.cluster_mask  = s.cluster_mask;

    /* All rule tables start at index 0 — MCU uses sequential access. */
    blk.profile_rule_idx = 0;
    blk.dma_rule_idx     = 0;
    blk.agu_rule_idx     = 0;
    blk.pe_payload_idx   = 0;
    blk.scan_payload_idx = 0;

    /* rule_stride = waves per d0 step = d1 * d2 (matching d0-major layout). */
    blk.rule_stride      = d1 * d2;
    blk.nlu_rule_stride  = 0;
    blk.total_waves      = d0 * d1 * d2;

    /* NLU rules are generated in d0->d1 order (total = d0 * d1). */
    if (!ctx.nlu_rules.empty()) {
        blk.nlu_rule_idx    = 0;
        blk.nlu_rule_stride = d1;  /* NLU rules per d0 step */
        blk.block_flags    |= BLOCK_FLAG_NLU_PHASE;
    }

    /* Patches: detect boundary tiles for residual handling. */
    blk.patch_begin = 0;
    if (ctx.op.op_type == OpType::CONV2D) {
        uint32_t full_h = ctx.op.output.h;
        uint32_t tile_h = s.tile.tile_h;
        if (full_h % tile_h != 0) {
            /*
             * Last d0 tile is a residual (partial spatial tile).
             * Generate one patch per (d1, d2) combination at the last d0.
             */
            uint32_t last_d0 = d0 - 1;
            for (uint32_t d1_idx = 0; d1_idx < d1; ++d1_idx) {
                for (uint32_t d2_idx = 0; d2_idx < d2; ++d2_idx) {
                    PatchEntry pe;
                    pe.wave_id    = last_d0 * d1 * d2 + d1_idx * d2 + d2_idx;
                    pe.valid_mask = 0x01; /* override profile (residual tile_h) */
                    pe.profile_idx = pe.wave_id;
                    ctx.patches.push_back(pe);
                }
            }
        }
    }
    blk.patch_count = static_cast<uint32_t>(ctx.patches.size());

    /* Single compressed block is both first and last. */
    blk.block_flags |= BLOCK_FLAG_FIRST_BLOCK | BLOCK_FLAG_LAST_BLOCK;

    ctx.blocks.push_back(blk);
}

} // namespace hacc
