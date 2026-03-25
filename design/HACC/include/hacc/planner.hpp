/**
 * @file planner.hpp
 * @brief HACC Compiler Schedule & Resource Planner.
 *
 * Determines tiling strategy, cluster allocation, and loop structure
 * for a given operation.
 *
 * @see HACC.md §2.2 Schedule & Resource Planner
 * @see ClusterCompiler.md §6 Timing Cost Model
 */
#pragma once
#include "hacc/ir.hpp"

namespace hacc {

/**
 * @brief Hardware constraint parameters for the cost model.
 *
 * @see ClusterCompiler.md §6.1 Mandatory parameters
 */
struct HwConstraint {
    uint32_t pe_array_rows = 12;  ///< PE array rows per cluster
    uint32_t pe_array_cols = 8;   ///< PE array columns per cluster
    uint32_t mac_per_pe    = 32;  ///< MACs per PE per cycle
    uint32_t num_clusters  = 4;   ///< Total available clusters
    uint32_t spm_size_kb   = 1024; ///< Cluster SPM size in KB
    uint32_t bw_dram_to_spm_bpc = 64; ///< DRAM-to-SPM bandwidth (bytes/cycle)
};

/**
 * @brief Schedule & resource planner.
 *
 * Responsible for:
 * 1. Generating candidate tilings based on hardware constraints.
 * 2. Evaluating candidates via a simple cost model.
 * 3. Selecting the optimal schedule and producing ScheduleIR.
 */
class Planner {
public:
    /**
     * @brief Plan a schedule for the given operation.
     *
     * @param op   Validated OpIR from the frontend.
     * @param hw   Hardware constraints for cost evaluation.
     * @return ScheduleIR with tiling, loop structure, and cluster assignment.
     */
    static ScheduleIR plan(const OpIR &op, const HwConstraint &hw);

private:
    /**
     * @brief Plan tiling for Conv2D.
     *
     * Splits output height, output channels, and input channels
     * into tiles that fit within SPM capacity.
     *
     * @param op  Conv2D OpIR.
     * @param hw  Hardware constraints.
     * @return ScheduleIR.
     */
    static ScheduleIR plan_conv2d(const OpIR &op, const HwConstraint &hw);

    /**
     * @brief Plan tiling for GEMM.
     *
     * Splits M, N, K dimensions into tiles based on PE array size
     * and available memory.
     *
     * @param op  GEMM OpIR.
     * @param hw  Hardware constraints.
     * @return ScheduleIR.
     */
    static ScheduleIR plan_gemm(const OpIR &op, const HwConstraint &hw);

    /**
     * @brief Compute ceiling division.
     * @param a  Numerator.
     * @param b  Denominator (must be > 0).
     * @return ceil(a / b).
     */
    static uint32_t ceil_div(uint32_t a, uint32_t b) {
        return (a + b - 1) / b;
    }
};

} // namespace hacc
