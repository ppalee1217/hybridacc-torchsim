/**
 * @file block_builder.hpp
 * @brief HACC Compiler Block Builder.
 *
 * Aggregates wave-level information into compressed block descriptors
 * and generates patch entries for exception overrides.
 *
 * @see HACC.md §3.2 Block, §5.2 .hacc.block, §5.3 .hacc.patch
 */
#pragma once
#include "hacc/ir.hpp"

namespace hacc {

/**
 * @brief Block builder.
 *
 * Responsible for:
 * 1. Creating block descriptors from the schedule loop structure.
 * 2. Assigning rule indices that reference profile/DMA/AGU/PE/NLU tables.
 * 3. Generating patch entries for boundary/residual waves.
 */
class BlockBuilder {
public:
    /**
     * @brief Build block descriptors and patches from the compiler context.
     *
     * @param[in,out] ctx  Compiler context (reads schedule/profiles, writes blocks/patches).
     */
    static void build(CompilerContext &ctx);
};

} // namespace hacc
