/**
 * @file ir.hpp
 * @brief HACC Compiler Intermediate Representation data structures.
 *
 * Defines all IR layers used across the compiler pipeline:
 * - OpIR:       Frontend operation semantics
 * - ScheduleIR: Tiling & scheduling decisions
 * - MappingIR:  Hardware resource mapping
 * - BlockIR:    Block-level compressed control
 * - PayloadIR:  Runtime payload tables for MCU consumption
 * - PackageIR:  Final HACC-ELF section collection
 *
 * All structures correspond to the HACC AI Compiler specification §3–§5.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace hacc {

/// @defgroup ir_enums IR Enumeration Types
/// @{

/**
 * @brief Supported operation types.
 */
enum class OpType {
    CONV2D,  ///< 2D convolution (any kernel size)
    GEMM,    ///< General matrix multiply
};

/**
 * @brief Supported data types.
 */
enum class DataType {
    INT8,    ///< 8-bit integer
    INT16,   ///< 16-bit integer
    FP16,    ///< 16-bit floating point
    FP32,    ///< 32-bit floating point
};

/**
 * @brief Block flags for controlling MCU behavior.
 */
enum BlockFlag : uint32_t {
    BLOCK_FLAG_NONE       = 0,
    BLOCK_FLAG_NLU_PHASE  = (1u << 0), ///< Block includes NLU post-processing
    BLOCK_FLAG_LAST_BLOCK = (1u << 1), ///< Last block in job
    BLOCK_FLAG_FIRST_BLOCK = (1u << 2), ///< First block in job
};

/**
 * @brief Target stream sink selector.
 */
enum class StreamDst : uint8_t {
    DMA         = 0, ///< DMA stream sink
    NOC_CMD     = 1, ///< Cluster NoC command sink
    HDDU_PROF   = 2, ///< Cluster HDDU/profile payload sink
    NLU_CONFIG  = 3, ///< NLU config payload sink
};

/// @}

/// @defgroup ir_tensors Tensor Shape Descriptions
/// @{

/**
 * @brief 4D tensor shape in NCHW layout.
 */
struct TensorShape {
    uint32_t n = 1; ///< Batch
    uint32_t c = 1; ///< Channels
    uint32_t h = 1; ///< Height
    uint32_t w = 1; ///< Width

    /** @brief Total element count. */
    uint32_t numel() const { return n * c * h * w; }
};

/**
 * @brief Convolution attributes.
 */
struct ConvAttr {
    uint32_t kernel_h  = 3; ///< Kernel height
    uint32_t kernel_w  = 3; ///< Kernel width
    uint32_t stride_h  = 1; ///< Vertical stride
    uint32_t stride_w  = 1; ///< Horizontal stride
    uint32_t pad_h     = 1; ///< Vertical padding
    uint32_t pad_w     = 1; ///< Horizontal padding
    uint32_t dilation_h = 1; ///< Vertical dilation
    uint32_t dilation_w = 1; ///< Horizontal dilation
    uint32_t groups    = 1; ///< Convolution groups
};

/**
 * @brief GEMM attributes.
 */
struct GemmAttr {
    uint32_t M = 0; ///< M dimension
    uint32_t N = 0; ///< N dimension
    uint32_t K = 0; ///< K dimension
    bool trans_a = false; ///< Transpose A
    bool trans_b = false; ///< Transpose B
};

/// @}

/// @defgroup ir_frontend Frontend IR
/// @{

/**
 * @brief Operation IR — captures user-specified operation semantics.
 *
 * This is the compiler's input representation, parsed from the frontend
 * op description (JSON or programmatic API).
 *
 * @see HACC.md §2.2 Frontend Graph / Op Parser
 */
struct OpIR {
    std::string   name;       ///< Layer / operation name
    OpType        op_type;    ///< Operation kind
    DataType      dtype = DataType::INT16; ///< Data type

    TensorShape   input;      ///< Input tensor shape (NCHW)
    TensorShape   weight;     ///< Weight tensor shape (KCRS for conv, MxK for GEMM)
    TensorShape   output;     ///< Output tensor shape

    ConvAttr      conv_attr;  ///< Conv-specific attributes (valid when op_type == CONV2D)
    GemmAttr      gemm_attr;  ///< GEMM-specific attributes (valid when op_type == GEMM)

    bool allow_nlu = false;   ///< Whether NLU post-processing is required
};

/// @}

/// @defgroup ir_schedule Schedule IR
/// @{

/**
 * @brief Tile configuration — how the problem is split into hardware tiles.
 */
struct TileConfig {
    uint32_t tile_h  = 1; ///< Tile height (output spatial)
    uint32_t tile_w  = 1; ///< Tile width (output spatial)
    uint32_t tile_ic = 1; ///< Tile input channels
    uint32_t tile_oc = 1; ///< Tile output channels
    uint32_t tile_m  = 1; ///< Tile M (GEMM)
    uint32_t tile_n  = 1; ///< Tile N (GEMM)
    uint32_t tile_k  = 1; ///< Tile K (GEMM)
};

/**
 * @brief Schedule IR — tiling and scheduling decisions.
 *
 * @see HACC.md §2.2 Schedule & Resource Planner
 */
struct ScheduleIR {
    TileConfig    tile;              ///< Tile sizes
    uint32_t      num_clusters = 1;  ///< Number of clusters to use
    uint32_t      cluster_mask = 1;  ///< Bitmask of active clusters

    /** @brief Loop extents for block iteration (up to 4 dims). */
    std::array<uint32_t, 4> loop_extents = {1, 1, 1, 1};

    /** @brief Loop dimension names for debug. */
    std::array<std::string, 4> loop_names = {"dim0", "dim1", "dim2", "dim3"};

    /** @brief Total number of waves = product of loop_extents. */
    uint32_t total_waves() const {
        uint32_t n = 1;
        for (auto e : loop_extents) n *= e;
        return n;
    }
};

/// @}

/// @defgroup ir_mapping Mapping IR
/// @{

/**
 * @brief AGU (Address Generation Unit) configuration for one tensor.
 */
struct AguConfig {
    uint32_t base_addr = 0;  ///< Base address in SPM
    uint32_t iter0     = 1;  ///< Iteration count dim 0
    uint32_t stride0   = 1;  ///< Stride dim 0 (in words)
    uint32_t iter1     = 1;  ///< Iteration count dim 1
    uint32_t stride1   = 0;  ///< Stride dim 1
    uint32_t iter2     = 1;  ///< Iteration count dim 2
    uint32_t stride2   = 0;  ///< Stride dim 2
};

/**
 * @brief Profile configuration for a cluster.
 *
 * Describes the HDDU register window values to apply before computation.
 */
struct ProfileConfig {
    uint32_t pe_mode   = 0;  ///< PE computation mode
    uint32_t pe_rows   = 12; ///< Active PE rows
    uint32_t pe_cols   = 8;  ///< Active PE columns
    AguConfig agu_ifmap;     ///< AGU for input feature map
    AguConfig agu_weight;    ///< AGU for weights
    AguConfig agu_ofmap;     ///< AGU for output
};

/**
 * @brief DMA rule for one transfer.
 */
struct DmaRule {
    uint32_t mode          = 0;  ///< Transfer mode
    uint32_t cluster_mask  = 1;  ///< Target cluster mask
    uint32_t word_count    = 0;  ///< 32-bit words to transfer
    uint32_t src_addr      = 0;  ///< External DRAM source address
    uint32_t dst_addr      = 0;  ///< Local SPM destination address
};

/**
 * @brief NLU (Non-Linear Unit) rule.
 */
struct NluRule {
    uint32_t mode        = 0;  ///< NLU operation mode (relu, softmax, etc.)
    uint32_t src_addr    = 0;  ///< Source data address
    uint32_t dst_addr    = 0;  ///< Destination address
    uint32_t word_count  = 0;  ///< Data word count
    uint32_t nlu_id      = 0;  ///< Target NLU unit ID
};

/// @}

/// @defgroup ir_block Block IR
/// @{

/**
 * @brief Patch entry — exception override for specific wave instances.
 *
 * @see HACC.md §5.3 .hacc.patch
 */
struct PatchEntry {
    uint32_t wave_id       = 0;  ///< Target wave ID (or loop index tuple hash)
    uint32_t valid_mask    = 0;  ///< Bitmask of fields to override
    uint32_t profile_idx   = 0;  ///< Overridden profile index
    uint32_t dma_idx       = 0;  ///< Overridden DMA rule index
    uint32_t cluster_mask  = 0;  ///< Overridden cluster mask
};

/**
 * @brief Block descriptor — compressed control fragment for MCU consumption.
 *
 * @see HACC.md §3.2 Block, §5.2 .hacc.block
 */
struct BlockDesc {
    uint32_t loop_rank     = 1;  ///< Number of loop dimensions
    std::array<uint32_t, 4> loop_extent = {1, 1, 1, 1}; ///< Per-dim upper bounds
    uint32_t repeat_count  = 1;  ///< Block repeat count
    uint32_t cluster_mask  = 1;  ///< Active cluster bitmask

    uint32_t profile_rule_idx   = 0; ///< Index into profile table
    uint32_t dma_rule_idx       = 0; ///< Index into DMA table
    uint32_t agu_rule_idx       = 0; ///< Index into AGU table
    uint32_t pe_payload_idx     = 0; ///< Index into PE payload table
    uint32_t scan_payload_idx   = 0; ///< Index into scan-chain payload table
    uint32_t nlu_rule_idx       = 0; ///< Index into NLU table

    /**
     * @brief Rule index stride per d0 (outermost loop) iteration.
     *
     * Enables folding the d1 dimension into the block loop instead of
     * creating one block per d1.  MCU computes:
     *   rule_idx = base + sequential_wave_counter
     * The stride records d1*d2 so the MCU can reconstruct per-dimension
     * indices when needed (e.g., NLU indexing, barriers).
     */
    uint32_t rule_stride       = 0;

    /**
     * @brief NLU rule index stride per d0 (outermost loop) iteration.
     *
     * NLU rules are indexed by (d0, d1) pairs:
     *   nlu_idx = nlu_rule_idx + d0 * nlu_rule_stride + d1.
     */
    uint32_t nlu_rule_stride   = 0;

    /**
     * @brief Precomputed total wave count (product of all loop_extents).
     *
     * Stored explicitly because the MCU ISA lacks a hardware multiply
     * instruction, avoiding runtime multiplication of loop extents.
     */
    uint32_t total_waves       = 1;

    uint32_t patch_begin   = 0;  ///< Start index in patch table
    uint32_t patch_count   = 0;  ///< Number of patch entries

    uint32_t block_flags   = BLOCK_FLAG_NONE; ///< @see BlockFlag
};

/// @}

/// @defgroup ir_payload Payload IR
/// @{

/**
 * @brief Generic 32-bit word payload with header.
 *
 * Used for PE program, scan-chain, profile, DMA, NLU payloads that
 * the MCU streams to target peripherals via STRM instructions.
 */
struct PayloadSection {
    uint32_t cluster_mask  = 0;  ///< Target mask (0 = broadcast all)
    bool     needs_sync    = false; ///< Whether to sync before streaming
    std::vector<uint32_t> words;   ///< 32-bit payload words
};

/**
 * @brief Job root descriptor.
 *
 * @see HACC.md §5.1 .hacc.job
 */
struct JobDesc {
    uint32_t block_table_base   = 0;
    uint32_t block_count        = 0;
    uint32_t profile_table_base = 0;
    uint32_t profile_count      = 0;
    uint32_t dma_table_base     = 0;
    uint32_t dma_count          = 0;
    uint32_t agu_table_base     = 0;
    uint32_t agu_count          = 0;
    uint32_t nlu_table_base     = 0;
    uint32_t nlu_count          = 0;
    uint32_t pe_table_base      = 0;
    uint32_t pe_count           = 0;
    uint32_t scan_table_base    = 0;
    uint32_t scan_count         = 0;
    uint32_t patch_table_base   = 0;
    uint32_t patch_count        = 0;
    uint32_t required_cluster_mask = 1;
    uint32_t required_caps      = 0;
    uint32_t job_flags          = 0;
};

/// @}

/// @defgroup ir_package Package IR
/// @{

/**
 * @brief Complete HACC package — all sections for HACC-ELF emission.
 *
 * This is the final output of the compiler pipeline, ready for
 * ELF packaging and loader consumption.
 *
 * @see HACC.md §4 Package & Section Contract
 */
struct HaccPackage {
    /** @brief .hacc.core — MCU firmware binary. */
    std::vector<uint32_t> core_firmware;

    /** @brief .hacc.job — serialized JobDesc. */
    std::vector<uint32_t> job_section;

    /** @brief .hacc.block — serialized BlockDesc array. */
    std::vector<uint32_t> block_section;

    /** @brief .hacc.profile — profile rule headers + payloads. */
    std::vector<uint32_t> profile_section;

    /** @brief .hacc.dma — DMA rule headers + payloads. */
    std::vector<uint32_t> dma_section;

    /** @brief .hacc.agu — AGU configuration tables. */
    std::vector<uint32_t> agu_section;

    /** @brief .hacc.nlu — NLU rule tables. */
    std::vector<uint32_t> nlu_section;

    /** @brief .hacc.pe — PE program payload. */
    std::vector<uint32_t> pe_section;

    /** @brief .hacc.scan — Scan-chain payload. */
    std::vector<uint32_t> scan_section;

    /** @brief .hacc.patch — Exception override table. */
    std::vector<uint32_t> patch_section;

    /** @brief Stage0 debug JSON for review. */
    std::string debug_json;
};

/**
 * @brief Full compiler context passed between pipeline stages.
 */
struct CompilerContext {
    OpIR                         op;         ///< Frontend input
    ScheduleIR                   schedule;   ///< Tiling decisions
    std::vector<ProfileConfig>   profiles;   ///< Profile table
    std::vector<AguConfig>       agus;       ///< AGU table
    std::vector<DmaRule>         dma_rules;  ///< DMA rule table
    std::vector<NluRule>         nlu_rules;  ///< NLU rule table
    std::vector<BlockDesc>       blocks;     ///< Block descriptors
    std::vector<PatchEntry>      patches;    ///< Patch entries
    PayloadSection               pe_payload; ///< PE program payload
    PayloadSection               scan_payload; ///< Scan-chain payload
    HaccPackage                  package;    ///< Final output package
};

/// @}

} // namespace hacc
