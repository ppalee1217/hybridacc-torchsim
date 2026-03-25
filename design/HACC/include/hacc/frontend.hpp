/**
 * @file frontend.hpp
 * @brief HACC Compiler Frontend — Op parsing and semantic validation.
 *
 * Parses operation descriptions and produces a validated OpIR.
 *
 * @see HACC.md §2.2 Frontend Graph / Op Parser, Semantic Validator
 */
#pragma once
#include "hacc/ir.hpp"
#include <string>

namespace hacc {

/**
 * @brief Frontend parser and semantic validator.
 *
 * Responsible for:
 * 1. Accepting programmatic operation descriptions.
 * 2. Validating tensor shapes and attributes.
 * 3. Producing a clean OpIR for downstream compilation.
 */
class Frontend {
public:
    /**
     * @brief Create a Conv2D operation IR from explicit parameters.
     *
     * @param name       Layer name (e.g., "conv_layer_1").
     * @param dtype      Data type for computation.
     * @param n          Batch size.
     * @param ic         Input channels.
     * @param ih         Input height.
     * @param iw         Input width.
     * @param oc         Output channels.
     * @param kh         Kernel height.
     * @param kw         Kernel width.
     * @param stride_h   Vertical stride.
     * @param stride_w   Horizontal stride.
     * @param pad_h      Vertical padding.
     * @param pad_w      Horizontal padding.
     * @param with_nlu   Whether to apply NLU post-processing (e.g., ReLU).
     * @return Validated OpIR.
     * @throws std::invalid_argument if shapes are inconsistent.
     */
    static OpIR create_conv2d(const std::string &name,
                              DataType dtype,
                              uint32_t n, uint32_t ic, uint32_t ih, uint32_t iw,
                              uint32_t oc, uint32_t kh, uint32_t kw,
                              uint32_t stride_h, uint32_t stride_w,
                              uint32_t pad_h, uint32_t pad_w,
                              bool with_nlu = false);

    /**
     * @brief Create a GEMM operation IR from explicit parameters.
     *
     * @param name      Layer name.
     * @param dtype     Data type.
     * @param M         M dimension.
     * @param N         N dimension.
     * @param K         K dimension.
     * @param trans_a   Transpose input A.
     * @param trans_b   Transpose input B.
     * @param with_nlu  Whether to apply NLU post-processing.
     * @return Validated OpIR.
     * @throws std::invalid_argument if dimensions are zero.
     */
    static OpIR create_gemm(const std::string &name,
                            DataType dtype,
                            uint32_t M, uint32_t N, uint32_t K,
                            bool trans_a = false, bool trans_b = false,
                            bool with_nlu = false);

    /**
     * @brief Validate an OpIR for semantic correctness.
     *
     * Checks output shape consistency, non-zero dimensions, and attribute sanity.
     *
     * @param op  The operation IR to validate.
     * @return true if valid; false otherwise with error logged to stderr.
     */
    static bool validate(const OpIR &op);
};

} // namespace hacc
