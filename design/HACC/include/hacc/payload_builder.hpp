/**
 * @file payload_builder.hpp
 * @brief HACC Compiler Runtime Payload Builder.
 *
 * Converts compiler artifacts (profiles, AGU, DMA rules, PE/scan payloads)
 * into serialized 32-bit word streams ready for MCU consumption.
 *
 * @see HACC.md §2.2 Runtime Payload Builder
 * @see HACC.md §5.4–§5.6 Section format suggestions
 */
#pragma once
#include "hacc/ir.hpp"

namespace hacc {

/**
 * @brief Runtime payload builder.
 *
 * Serializes all compiler artifacts into the compact word-stream
 * format that MCU firmware reads or streams to target peripherals.
 */
class PayloadBuilder {
public:
    /**
     * @brief Build all runtime payload sections.
     *
     * Serializes profiles, AGU configs, DMA rules, NLU rules, PE payload,
     * scan-chain payload, blocks, patches, and the job descriptor
     * into the HaccPackage inside the compiler context.
     *
     * @param[in,out] ctx  Compiler context (reads all artifacts, writes package sections).
     */
    static void build(CompilerContext &ctx);

private:
    /**
     * @brief Serialize a JobDesc into a word vector.
     * @param job  The job descriptor.
     * @return Vector of 32-bit words.
     */
    static std::vector<uint32_t> serialize_job(const JobDesc &job);

    /**
     * @brief Serialize block descriptors into a word vector.
     * @param blocks  Block descriptor array.
     * @return Vector of 32-bit words.
     */
    static std::vector<uint32_t> serialize_blocks(const std::vector<BlockDesc> &blocks);

    /**
     * @brief Serialize profile configs into a word vector.
     * @param profiles  Profile config array.
     * @return Vector of 32-bit words.
     */
    static std::vector<uint32_t> serialize_profiles(const std::vector<ProfileConfig> &profiles);

    /**
     * @brief Serialize AGU configs into a word vector.
     * @param agus  AGU config array.
     * @return Vector of 32-bit words.
     */
    static std::vector<uint32_t> serialize_agus(const std::vector<AguConfig> &agus);

    /**
     * @brief Serialize DMA rules into a word vector.
     * @param rules  DMA rule array.
     * @return Vector of 32-bit words.
     */
    static std::vector<uint32_t> serialize_dma_rules(const std::vector<DmaRule> &rules);

    /**
     * @brief Serialize NLU rules into a word vector.
     * @param rules  NLU rule array.
     * @return Vector of 32-bit words.
     */
    static std::vector<uint32_t> serialize_nlu_rules(const std::vector<NluRule> &rules);

    /**
     * @brief Serialize patch entries into a word vector.
     * @param patches  Patch entry array.
     * @return Vector of 32-bit words.
     */
    static std::vector<uint32_t> serialize_patches(const std::vector<PatchEntry> &patches);
};

} // namespace hacc
