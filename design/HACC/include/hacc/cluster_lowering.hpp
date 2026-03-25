/**
 * @file cluster_lowering.hpp
 * @brief HACC Compiler Cluster Lowering Backend.
 *
 * Generates PE program payload, AGU configurations, profile configs,
 * and scan-chain payloads from the scheduled OpIR.
 *
 * @see HACC.md §2.2 Cluster Lowering Backend
 */
#pragma once
#include "hacc/ir.hpp"

namespace hacc {

/**
 * @brief Cluster lowering backend.
 *
 * Responsible for:
 * 1. Generating PE program payload (.hacc.pe).
 * 2. Computing AGU configurations (.hacc.agu).
 * 3. Producing profile configs (.hacc.profile).
 * 4. Generating scan-chain payload (.hacc.scan).
 */
class ClusterLowering {
public:
    /**
     * @brief Lower the operation into cluster-level artifacts.
     *
     * Populates profiles, AGU configs, PE payload, and scan-chain payload
     * in the compiler context.
     *
     * @param[in,out] ctx  Compiler context (reads op/schedule, writes cluster artifacts).
     */
    static void lower(CompilerContext &ctx);

private:
    /**
     * @brief Generate PE program payload for Conv2D.
     *
     * Produces a simplified PE instruction sequence based on the tile
     * configuration and convolution attributes.
     *
     * @param ctx  Compiler context.
     * @return PE payload section.
     */
    static PayloadSection gen_pe_conv2d(const CompilerContext &ctx);

    /**
     * @brief Generate PE program payload for GEMM.
     *
     * @param ctx  Compiler context.
     * @return PE payload section.
     */
    static PayloadSection gen_pe_gemm(const CompilerContext &ctx);

    /**
     * @brief Compute AGU configuration for one wave of Conv2D.
     *
     * @param ctx       Compiler context.
     * @param oh_idx    Output height tile index.
     * @param oc_idx    Output channel tile index.
     * @param ic_idx    Input channel tile index.
     * @return AGU config for this wave.
     */
    static AguConfig compute_agu_conv2d(const CompilerContext &ctx,
                                        uint32_t oh_idx, uint32_t oc_idx, uint32_t ic_idx);

    /**
     * @brief Compute AGU configuration for one wave of GEMM.
     *
     * @param ctx    Compiler context.
     * @param m_idx  M tile index.
     * @param n_idx  N tile index.
     * @param k_idx  K tile index.
     * @return AGU config for this wave.
     */
    static AguConfig compute_agu_gemm(const CompilerContext &ctx,
                                      uint32_t m_idx, uint32_t n_idx, uint32_t k_idx);
};

} // namespace hacc
