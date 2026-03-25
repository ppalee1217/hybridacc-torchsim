/**
 * @file frontend.cpp
 * @brief HACC Compiler Frontend implementation.
 */
#include "hacc/frontend.hpp"
#include <stdexcept>
#include <iostream>

namespace hacc {

OpIR Frontend::create_conv2d(const std::string &name,
                             DataType dtype,
                             uint32_t n, uint32_t ic, uint32_t ih, uint32_t iw,
                             uint32_t oc, uint32_t kh, uint32_t kw,
                             uint32_t stride_h, uint32_t stride_w,
                             uint32_t pad_h, uint32_t pad_w,
                             bool with_nlu) {
    OpIR op;
    op.name    = name;
    op.op_type = OpType::CONV2D;
    op.dtype   = dtype;

    op.input  = {n, ic, ih, iw};
    op.weight = {oc, ic, kh, kw};

    /* Compute output spatial dimensions. */
    uint32_t oh = (ih + 2 * pad_h - kh) / stride_h + 1;
    uint32_t ow = (iw + 2 * pad_w - kw) / stride_w + 1;
    op.output = {n, oc, oh, ow};

    op.conv_attr.kernel_h  = kh;
    op.conv_attr.kernel_w  = kw;
    op.conv_attr.stride_h  = stride_h;
    op.conv_attr.stride_w  = stride_w;
    op.conv_attr.pad_h     = pad_h;
    op.conv_attr.pad_w     = pad_w;

    op.allow_nlu = with_nlu;

    if (!validate(op)) {
        throw std::invalid_argument("Conv2D validation failed for: " + name);
    }
    return op;
}

OpIR Frontend::create_gemm(const std::string &name,
                           DataType dtype,
                           uint32_t M, uint32_t N, uint32_t K,
                           bool trans_a, bool trans_b,
                           bool with_nlu) {
    OpIR op;
    op.name    = name;
    op.op_type = OpType::GEMM;
    op.dtype   = dtype;

    op.input   = {1, 1, M, K};   /* A: M x K */
    op.weight  = {1, 1, K, N};   /* B: K x N */
    op.output  = {1, 1, M, N};   /* C: M x N */

    op.gemm_attr.M = M;
    op.gemm_attr.N = N;
    op.gemm_attr.K = K;
    op.gemm_attr.trans_a = trans_a;
    op.gemm_attr.trans_b = trans_b;

    op.allow_nlu = with_nlu;

    if (!validate(op)) {
        throw std::invalid_argument("GEMM validation failed for: " + name);
    }
    return op;
}

bool Frontend::validate(const OpIR &op) {
    /* All dimensions must be non-zero. */
    if (op.input.numel() == 0 || op.weight.numel() == 0 || op.output.numel() == 0) {
        std::cerr << "[Frontend] zero-dim tensor in op: " << op.name << "\n";
        return false;
    }

    if (op.op_type == OpType::CONV2D) {
        /* Output spatial consistency. */
        uint32_t expected_oh = (op.input.h + 2 * op.conv_attr.pad_h - op.conv_attr.kernel_h)
                               / op.conv_attr.stride_h + 1;
        uint32_t expected_ow = (op.input.w + 2 * op.conv_attr.pad_w - op.conv_attr.kernel_w)
                               / op.conv_attr.stride_w + 1;
        if (op.output.h != expected_oh || op.output.w != expected_ow) {
            std::cerr << "[Frontend] output shape mismatch in conv: " << op.name << "\n";
            return false;
        }
        if (op.conv_attr.kernel_h == 0 || op.conv_attr.kernel_w == 0) {
            std::cerr << "[Frontend] zero kernel size in conv: " << op.name << "\n";
            return false;
        }
    } else if (op.op_type == OpType::GEMM) {
        if (op.gemm_attr.M == 0 || op.gemm_attr.N == 0 || op.gemm_attr.K == 0) {
            std::cerr << "[Frontend] zero GEMM dimension in: " << op.name << "\n";
            return false;
        }
    }
    return true;
}

} // namespace hacc
