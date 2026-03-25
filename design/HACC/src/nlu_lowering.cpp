/**
 * @file nlu_lowering.cpp
 * @brief HACC Compiler NLU Lowering Backend implementation.
 */
#include "hacc/nlu_lowering.hpp"
#include "hacc/isa.hpp"

namespace hacc {

void NluLowering::lower(CompilerContext &ctx) {
    if (!ctx.op.allow_nlu) {
        /* No NLU phase required — leave nlu_rules empty. */
        return;
    }

    const auto &s = ctx.schedule;
    uint32_t bpe = (ctx.op.dtype == DataType::INT8) ? 1 : 2;

    /*
     * Generate one NLU rule per output tile (loop_oh * loop_oc iterations).
     * Each rule configures one NLU unit to process the output tile data.
     *
     * NLU mode 0 = ReLU (default when allow_nlu is set).
     */
    uint32_t nlu_id = 0;
    for (uint32_t d0 = 0; d0 < s.loop_extents[0]; ++d0) {
        for (uint32_t d1 = 0; d1 < s.loop_extents[1]; ++d1) {
            NluRule rule;
            rule.mode       = 0; /* ReLU */
            rule.nlu_id     = nlu_id % 4; /* round-robin across NLU units */

            /* Source = output tile in SPM (after cluster compute). */
            uint32_t tile_elems = 0;
            if (ctx.op.op_type == OpType::CONV2D) {
                tile_elems = s.tile.tile_oc * s.tile.tile_h * s.tile.tile_w;
            } else {
                tile_elems = s.tile.tile_m * s.tile.tile_n;
            }
            rule.word_count = (tile_elems * bpe + 3) / 4; /* in 32-bit words */
            rule.src_addr   = 0; /* placeholder: computed at runtime by MCU */
            rule.dst_addr   = 0; /* in-place */

            ctx.nlu_rules.push_back(rule);
            ++nlu_id;
        }
    }
}

} // namespace hacc
