/**
 * @file payload_builder.cpp
 * @brief HACC Compiler Runtime Payload Builder implementation.
 */
#include "hacc/payload_builder.hpp"

namespace hacc {

/**
 * @brief Number of 32-bit words per serialized ProfileConfig.
 *
 * Layout: pe_mode, pe_rows, pe_cols, then 3 AGU configs (7 words each) = 24 words.
 */
static constexpr uint32_t PROFILE_WORDS = 3 + 3 * 7;

/** @brief Number of 32-bit words per serialized AguConfig. */
static constexpr uint32_t AGU_WORDS = 7;

/** @brief Number of 32-bit words per serialized DmaRule. */
static constexpr uint32_t DMA_RULE_WORDS = 5;

/** @brief Number of 32-bit words per serialized NluRule. */
static constexpr uint32_t NLU_RULE_WORDS = 5;

/** @brief Number of 32-bit words per serialized BlockDesc. */
static constexpr uint32_t BLOCK_WORDS = 19;

/** @brief Number of 32-bit words per serialized PatchEntry. */
static constexpr uint32_t PATCH_WORDS = 5;

/** @brief Number of 32-bit words per serialized JobDesc. */
static constexpr uint32_t JOB_WORDS = 19;

void PayloadBuilder::build(CompilerContext &ctx) {
    auto &pkg = ctx.package;

    /* Serialize all tables. */
    pkg.profile_section = serialize_profiles(ctx.profiles);
    pkg.agu_section     = serialize_agus(ctx.agus);
    pkg.dma_section     = serialize_dma_rules(ctx.dma_rules);
    pkg.nlu_section     = serialize_nlu_rules(ctx.nlu_rules);
    pkg.block_section   = serialize_blocks(ctx.blocks);
    pkg.patch_section   = serialize_patches(ctx.patches);
    pkg.pe_section      = ctx.pe_payload.words;
    pkg.scan_section    = ctx.scan_payload.words;

    /* Build job descriptor with table layout offsets. */
    JobDesc job;
    uint32_t cursor = JOB_WORDS * 4; /* Start after job section itself. */

    job.block_table_base   = cursor;
    job.block_count        = static_cast<uint32_t>(ctx.blocks.size());
    cursor += static_cast<uint32_t>(pkg.block_section.size()) * 4;

    job.profile_table_base = cursor;
    job.profile_count      = static_cast<uint32_t>(ctx.profiles.size());
    cursor += static_cast<uint32_t>(pkg.profile_section.size()) * 4;

    job.dma_table_base     = cursor;
    job.dma_count          = static_cast<uint32_t>(ctx.dma_rules.size());
    cursor += static_cast<uint32_t>(pkg.dma_section.size()) * 4;

    job.agu_table_base     = cursor;
    job.agu_count          = static_cast<uint32_t>(ctx.agus.size());
    cursor += static_cast<uint32_t>(pkg.agu_section.size()) * 4;

    job.nlu_table_base     = cursor;
    job.nlu_count          = static_cast<uint32_t>(ctx.nlu_rules.size());
    cursor += static_cast<uint32_t>(pkg.nlu_section.size()) * 4;

    job.pe_table_base      = cursor;
    job.pe_count           = static_cast<uint32_t>(pkg.pe_section.size());
    cursor += static_cast<uint32_t>(pkg.pe_section.size()) * 4;

    job.scan_table_base    = cursor;
    job.scan_count         = static_cast<uint32_t>(pkg.scan_section.size());
    cursor += static_cast<uint32_t>(pkg.scan_section.size()) * 4;

    job.patch_table_base   = cursor;
    job.patch_count        = static_cast<uint32_t>(ctx.patches.size());

    job.required_cluster_mask = ctx.schedule.cluster_mask;
    job.required_caps      = 0x3F; /* All capabilities required */
    job.job_flags          = 0;

    pkg.job_section = serialize_job(job);
}

std::vector<uint32_t> PayloadBuilder::serialize_job(const JobDesc &job) {
    return {
        job.block_table_base,  job.block_count,
        job.profile_table_base, job.profile_count,
        job.dma_table_base,    job.dma_count,
        job.agu_table_base,    job.agu_count,
        job.nlu_table_base,    job.nlu_count,
        job.pe_table_base,     job.pe_count,
        job.scan_table_base,   job.scan_count,
        job.patch_table_base,  job.patch_count,
        job.required_cluster_mask,
        job.required_caps,
        job.job_flags,
    };
}

std::vector<uint32_t> PayloadBuilder::serialize_blocks(const std::vector<BlockDesc> &blocks) {
    std::vector<uint32_t> out;
    out.reserve(blocks.size() * BLOCK_WORDS);
    for (const auto &b : blocks) {
        out.push_back(b.loop_rank);
        for (auto e : b.loop_extent) out.push_back(e);
        out.push_back(b.repeat_count);
        out.push_back(b.cluster_mask);
        out.push_back(b.profile_rule_idx);
        out.push_back(b.dma_rule_idx);
        out.push_back(b.agu_rule_idx);
        out.push_back(b.pe_payload_idx);
        out.push_back(b.scan_payload_idx);
        out.push_back(b.nlu_rule_idx);
        out.push_back(b.rule_stride);
        out.push_back(b.nlu_rule_stride);
        out.push_back(b.total_waves);
        out.push_back(b.patch_begin);
        out.push_back(b.patch_count);
        out.push_back(b.block_flags);
    }
    return out;
}

std::vector<uint32_t> PayloadBuilder::serialize_profiles(
        const std::vector<ProfileConfig> &profiles) {
    std::vector<uint32_t> out;
    out.reserve(profiles.size() * PROFILE_WORDS);
    for (const auto &p : profiles) {
        out.push_back(p.pe_mode);
        out.push_back(p.pe_rows);
        out.push_back(p.pe_cols);

        auto push_agu = [&](const AguConfig &a) {
            out.push_back(a.base_addr);
            out.push_back(a.iter0);
            out.push_back(a.stride0);
            out.push_back(a.iter1);
            out.push_back(a.stride1);
            out.push_back(a.iter2);
            out.push_back(a.stride2);
        };
        push_agu(p.agu_ifmap);
        push_agu(p.agu_weight);
        push_agu(p.agu_ofmap);
    }
    return out;
}

std::vector<uint32_t> PayloadBuilder::serialize_agus(const std::vector<AguConfig> &agus) {
    std::vector<uint32_t> out;
    out.reserve(agus.size() * AGU_WORDS);
    for (const auto &a : agus) {
        out.push_back(a.base_addr);
        out.push_back(a.iter0);
        out.push_back(a.stride0);
        out.push_back(a.iter1);
        out.push_back(a.stride1);
        out.push_back(a.iter2);
        out.push_back(a.stride2);
    }
    return out;
}

std::vector<uint32_t> PayloadBuilder::serialize_dma_rules(const std::vector<DmaRule> &rules) {
    std::vector<uint32_t> out;
    out.reserve(rules.size() * DMA_RULE_WORDS);
    for (const auto &r : rules) {
        out.push_back(r.mode);
        out.push_back(r.cluster_mask);
        out.push_back(r.word_count);
        out.push_back(r.src_addr);
        out.push_back(r.dst_addr);
    }
    return out;
}

std::vector<uint32_t> PayloadBuilder::serialize_nlu_rules(const std::vector<NluRule> &rules) {
    std::vector<uint32_t> out;
    out.reserve(rules.size() * NLU_RULE_WORDS);
    for (const auto &r : rules) {
        out.push_back(r.mode);
        out.push_back(r.src_addr);
        out.push_back(r.dst_addr);
        out.push_back(r.word_count);
        out.push_back(r.nlu_id);
    }
    return out;
}

std::vector<uint32_t> PayloadBuilder::serialize_patches(const std::vector<PatchEntry> &patches) {
    std::vector<uint32_t> out;
    out.reserve(patches.size() * PATCH_WORDS);
    for (const auto &p : patches) {
        out.push_back(p.wave_id);
        out.push_back(p.valid_mask);
        out.push_back(p.profile_idx);
        out.push_back(p.dma_idx);
        out.push_back(p.cluster_mask);
    }
    return out;
}

} // namespace hacc
