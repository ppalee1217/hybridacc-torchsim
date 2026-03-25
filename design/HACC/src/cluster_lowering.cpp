/**
 * @file cluster_lowering.cpp
 * @brief HACC Compiler Cluster Lowering Backend implementation.
 */
#include "hacc/cluster_lowering.hpp"
#include "hacc/isa.hpp"

namespace hacc {

void ClusterLowering::lower(CompilerContext &ctx) {
    const auto &op = ctx.op;
    const auto &s  = ctx.schedule;

    /* Generate PE program payload. */
    if (op.op_type == OpType::CONV2D) {
        ctx.pe_payload = gen_pe_conv2d(ctx);
    } else {
        ctx.pe_payload = gen_pe_gemm(ctx);
    }

    /* Generate scan-chain payload (simplified — route config words). */
    ctx.scan_payload.cluster_mask = s.cluster_mask;
    ctx.scan_payload.words = {
        0x00000001, /* scan-chain enable */
        s.cluster_mask, /* route mask */
        0x00000000  /* scan-chain end marker */
    };

    /* Generate AGU configs and profiles for each wave. */
    uint32_t wave_id = 0;
    for (uint32_t d0 = 0; d0 < s.loop_extents[0]; ++d0) {
        for (uint32_t d1 = 0; d1 < s.loop_extents[1]; ++d1) {
            for (uint32_t d2 = 0; d2 < s.loop_extents[2]; ++d2) {
                AguConfig agu;
                if (op.op_type == OpType::CONV2D) {
                    agu = compute_agu_conv2d(ctx, d0, d1, d2);
                } else {
                    agu = compute_agu_gemm(ctx, d0, d1, d2);
                }
                ctx.agus.push_back(agu);

                ProfileConfig prof;
                prof.pe_rows   = (op.op_type == OpType::CONV2D) ? s.tile.tile_oc : s.tile.tile_m;
                prof.pe_cols   = (op.op_type == OpType::CONV2D) ? s.tile.tile_ic : s.tile.tile_n;
                prof.agu_ifmap  = agu;
                prof.agu_weight = agu;
                prof.agu_ofmap  = agu;
                ctx.profiles.push_back(prof);

                ++wave_id;
            }
        }
    }
}

PayloadSection ClusterLowering::gen_pe_conv2d(const CompilerContext &ctx) {
    PayloadSection ps;
    ps.cluster_mask = ctx.schedule.cluster_mask;

    const auto &t = ctx.schedule.tile;
    const auto &ca = ctx.op.conv_attr;

    /*
     * Simplified PE program for Conv2D:
     *   CLEAR.P
     *   LOOP tile_oc
     *     LOOP tile_ic
     *       LOOP kh * kw
     *         VMULR / accumulate
     *       LOOPEND
     *     LOOPEND
     *     VPSUMR (partial sum reduce)
     *   LOOPEND
     *   HALT
     *
     * Encoded as placeholder instruction words for now.
     */
    ps.words.push_back(0xDEAD0001); /* CLEAR.P */

    uint32_t inner_loop = ca.kernel_h * ca.kernel_w;
    ps.words.push_back(0xA0000000 | t.tile_oc);  /* LOOP tile_oc */
    ps.words.push_back(0xA0000000 | t.tile_ic);  /* LOOP tile_ic */
    ps.words.push_back(0xA0000000 | inner_loop);  /* LOOP kh*kw */
    ps.words.push_back(0xB0000001);               /* VMULR 1, 1 */
    ps.words.push_back(0xC0000000);               /* LOOPEND */
    ps.words.push_back(0xC0000000);               /* LOOPEND */
    ps.words.push_back(0xD0000001);               /* VPSUMR */
    ps.words.push_back(0xC0000000);               /* LOOPEND */
    ps.words.push_back(0xFF000000);               /* HALT */

    return ps;
}

PayloadSection ClusterLowering::gen_pe_gemm(const CompilerContext &ctx) {
    PayloadSection ps;
    ps.cluster_mask = ctx.schedule.cluster_mask;

    const auto &t = ctx.schedule.tile;

    /*
     * Simplified PE program for GEMM:
     *   CLEAR.P
     *   LOOP tile_k
     *     TSTORE (load B vectors)
     *     LOOP tile_n
     *       VMULR (MAC)
     *     LOOPEND
     *   LOOPEND
     *   VPSUMR (psum reduce)
     *   HALT
     */
    ps.words.push_back(0xDEAD0001);              /* CLEAR.P */
    ps.words.push_back(0xA0000000 | t.tile_k);   /* LOOP tile_k */
    ps.words.push_back(0xE0000001);              /* TSTORE (load vectors) */
    ps.words.push_back(0xA0000000 | t.tile_n);   /* LOOP tile_n */
    ps.words.push_back(0xB0000001);              /* VMULR 1, 1 */
    ps.words.push_back(0xC0000000);              /* LOOPEND */
    ps.words.push_back(0xC0000000);              /* LOOPEND */
    ps.words.push_back(0xD0000001);              /* VPSUMR */
    ps.words.push_back(0xFF000000);              /* HALT */

    return ps;
}

AguConfig ClusterLowering::compute_agu_conv2d(const CompilerContext &ctx,
                                              uint32_t oh_idx, uint32_t oc_idx,
                                              uint32_t ic_idx) {
    const auto &t  = ctx.schedule.tile;
    const auto &op = ctx.op;
    uint32_t bpe = (op.dtype == DataType::INT8) ? 1 : 2;

    AguConfig agu;
    /* Input tile base: offset by (oh_idx * tile_h * stride) rows + ic_idx * tile_ic channels. */
    uint32_t in_row_start = oh_idx * t.tile_h * op.conv_attr.stride_h;
    uint32_t in_ch_start  = ic_idx * t.tile_ic;
    agu.base_addr = (in_ch_start * op.input.h * op.input.w + in_row_start * op.input.w) * bpe;
    agu.iter0   = t.tile_h;
    agu.stride0 = op.input.w * bpe;
    agu.iter1   = t.tile_ic;
    agu.stride1 = op.input.h * op.input.w * bpe;

    return agu;
}

AguConfig ClusterLowering::compute_agu_gemm(const CompilerContext &ctx,
                                            uint32_t m_idx, uint32_t n_idx,
                                            uint32_t k_idx) {
    const auto &t  = ctx.schedule.tile;
    const auto &op = ctx.op;
    uint32_t bpe = (op.dtype == DataType::INT8) ? 1 : 2;

    AguConfig agu;
    /* A-tile base: A[m_idx * tile_m, k_idx * tile_k]. */
    agu.base_addr = (m_idx * t.tile_m * op.gemm_attr.K + k_idx * t.tile_k) * bpe;
    agu.iter0   = t.tile_m;
    agu.stride0 = op.gemm_attr.K * bpe;
    agu.iter1   = t.tile_k;
    agu.stride1 = bpe;

    return agu;
}

} // namespace hacc
