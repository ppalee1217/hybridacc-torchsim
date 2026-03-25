/**
 * @file planner.cpp
 * @brief HACC Compiler Schedule & Resource Planner implementation.
 */
#include "hacc/planner.hpp"
#include <algorithm>

namespace hacc {

ScheduleIR Planner::plan(const OpIR &op, const HwConstraint &hw) {
    switch (op.op_type) {
        case OpType::CONV2D: return plan_conv2d(op, hw);
        case OpType::GEMM:   return plan_gemm(op, hw);
    }
    return {};
}

ScheduleIR Planner::plan_conv2d(const OpIR &op, const HwConstraint &hw) {
    ScheduleIR s;

    /*
     * Tiling strategy:
     *   - tile_oc = min(oc, pe_rows)   — map output channels to PE rows
     *   - tile_ic = min(ic, pe_cols)   — map input channels to PE cols
     *   - tile_h  = pick largest that fits SPM
     *   - tile_w  = full width (no split)
     */
    s.tile.tile_oc = std::min(op.output.c, hw.pe_array_rows);
    s.tile.tile_ic = std::min(op.input.c, hw.pe_array_cols);
    s.tile.tile_w  = op.output.w;

    /* Estimate per-tile memory footprint and pick tile_h. */
    uint32_t bytes_per_elem = (op.dtype == DataType::INT8) ? 1 : 2;
    uint32_t spm_bytes = hw.spm_size_kb * 1024;

    /*
     * Per-tile memory: input tile + weight tile + output tile
     *   input:  tile_ic * (tile_h * stride + kh - 1) * (tile_w * stride + kw - 1) * bpe
     *   weight: tile_oc * tile_ic * kh * kw * bpe
     *   output: tile_oc * tile_h * tile_w * bpe
     */
    uint32_t weight_mem = s.tile.tile_oc * s.tile.tile_ic *
                          op.conv_attr.kernel_h * op.conv_attr.kernel_w * bytes_per_elem;

    uint32_t max_tile_h = op.output.h;
    for (uint32_t th = op.output.h; th >= 1; --th) {
        uint32_t in_h = th * op.conv_attr.stride_h + op.conv_attr.kernel_h - 1;
        uint32_t input_mem = s.tile.tile_ic * in_h * op.input.w * bytes_per_elem;
        uint32_t output_mem = s.tile.tile_oc * th * s.tile.tile_w * bytes_per_elem;
        if (input_mem + weight_mem + output_mem <= spm_bytes) {
            max_tile_h = th;
            break;
        }
        if (th == 1) { max_tile_h = 1; break; }
    }
    s.tile.tile_h = max_tile_h;

    /* Compute loop extents. */
    uint32_t loop_oh = ceil_div(op.output.h, s.tile.tile_h);
    uint32_t loop_oc = ceil_div(op.output.c, s.tile.tile_oc);
    uint32_t loop_ic = ceil_div(op.input.c,  s.tile.tile_ic);

    s.loop_extents = {loop_oh, loop_oc, loop_ic, 1};
    s.loop_names   = {"tile_oh", "tile_oc", "tile_ic", "unused"};

    /* Cluster assignment — use available clusters up to loop_oc. */
    s.num_clusters = std::min(hw.num_clusters, loop_oc);
    s.cluster_mask = (1u << s.num_clusters) - 1;

    return s;
}

ScheduleIR Planner::plan_gemm(const OpIR &op, const HwConstraint &hw) {
    ScheduleIR s;

    /*
     * GEMM tiling:
     *   - tile_m = min(M, pe_rows)
     *   - tile_n = min(N, pe_cols)
     *   - tile_k = pick to fit SPM
     */
    s.tile.tile_m = std::min(op.gemm_attr.M, hw.pe_array_rows);
    s.tile.tile_n = std::min(op.gemm_attr.N, hw.pe_array_cols);

    uint32_t bytes_per_elem = (op.dtype == DataType::INT8) ? 1 : 2;
    uint32_t spm_bytes = hw.spm_size_kb * 1024;

    /* tile_k: A-tile = tile_m * tile_k, B-tile = tile_k * tile_n, C-tile = tile_m * tile_n */
    uint32_t c_mem = s.tile.tile_m * s.tile.tile_n * bytes_per_elem * 2; /* accumulator wider */
    for (uint32_t tk = op.gemm_attr.K; tk >= 1; --tk) {
        uint32_t a_mem = s.tile.tile_m * tk * bytes_per_elem;
        uint32_t b_mem = tk * s.tile.tile_n * bytes_per_elem;
        if (a_mem + b_mem + c_mem <= spm_bytes) {
            s.tile.tile_k = tk;
            break;
        }
        if (tk == 1) { s.tile.tile_k = 1; break; }
    }

    uint32_t loop_m = ceil_div(op.gemm_attr.M, s.tile.tile_m);
    uint32_t loop_n = ceil_div(op.gemm_attr.N, s.tile.tile_n);
    uint32_t loop_k = ceil_div(op.gemm_attr.K, s.tile.tile_k);

    s.loop_extents = {loop_m, loop_n, loop_k, 1};
    s.loop_names   = {"tile_m", "tile_n", "tile_k", "unused"};

    s.num_clusters = std::min(hw.num_clusters, loop_m);
    s.cluster_mask = (1u << s.num_clusters) - 1;

    return s;
}

} // namespace hacc
