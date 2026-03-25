/**
 * @file nlu_lowering.hpp
 * @brief HACC Compiler NLU Lowering Backend.
 *
 * Generates NLU configuration rules for post-processing phases
 * (e.g., ReLU, softmax, layernorm) when the operation requires NLU.
 *
 * @see HACC.md §2.2 NLU Lowering Backend
 */
#pragma once
#include "hacc/ir.hpp"

namespace hacc {

/**
 * @brief NLU lowering backend.
 *
 * Responsible for generating .hacc.nlu rules that the MCU will
 * apply to NLU MMIO / payload window after cluster computation.
 */
class NluLowering {
public:
    /**
     * @brief Lower NLU configuration from the compiler context.
     *
     * If the operation has allow_nlu set, generates NLU rules for
     * each block. Otherwise, produces an empty NLU rule set.
     *
     * @param[in,out] ctx  Compiler context (reads op/schedule, writes nlu_rules).
     */
    static void lower(CompilerContext &ctx);
};

} // namespace hacc
