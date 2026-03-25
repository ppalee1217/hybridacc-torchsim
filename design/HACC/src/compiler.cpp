/**
 * @file compiler.cpp
 * @brief HACC AI Compiler — End-to-end pipeline implementation.
 */
#include "hacc/compiler.hpp"
#include "hacc/frontend.hpp"
#include "hacc/planner.hpp"
#include "hacc/cluster_lowering.hpp"
#include "hacc/nlu_lowering.hpp"
#include "hacc/block_builder.hpp"
#include "hacc/payload_builder.hpp"
#include "hacc/firmware_emitter.hpp"
#include "hacc/elf_packager.hpp"
#include <fstream>
#include <iostream>

namespace hacc {

HaccCompiler::HaccCompiler(const HwConstraint &hw) : hw_(hw) {}

CompileResult HaccCompiler::compile(const OpIR &op) {
    CompileResult result;

    /* Stage 1: Validate frontend input. */
    if (!Frontend::validate(op)) {
        result.error = "Frontend validation failed for: " + op.name;
        return result;
    }

    CompilerContext ctx;
    ctx.op = op;

    /* Stage 2: Schedule & resource planning. */
    ctx.schedule = Planner::plan(op, hw_);
    std::cout << "[Compiler] " << op.name
              << ": scheduled " << ctx.schedule.total_waves() << " waves"
              << " on " << ctx.schedule.num_clusters << " cluster(s)\n";

    /* Stage 3: Cluster lowering — PE/scan payload, AGU, profiles. */
    ClusterLowering::lower(ctx);
    std::cout << "[Compiler] " << op.name
              << ": " << ctx.profiles.size() << " profiles, "
              << ctx.agus.size() << " AGU configs, "
              << ctx.pe_payload.words.size() << " PE words\n";

    /* Stage 4: NLU lowering. */
    NluLowering::lower(ctx);
    if (!ctx.nlu_rules.empty()) {
        std::cout << "[Compiler] " << op.name
                  << ": " << ctx.nlu_rules.size() << " NLU rules\n";
    }

    /* Stage 5: DMA rule generation (one rule per wave). */
    uint32_t bpe = (op.dtype == DataType::INT8) ? 1 : 2;
    for (uint32_t i = 0; i < ctx.schedule.total_waves(); ++i) {
        DmaRule dma;
        dma.cluster_mask = ctx.schedule.cluster_mask;
        if (op.op_type == OpType::CONV2D) {
            uint32_t tile_input_elems = ctx.schedule.tile.tile_ic *
                (ctx.schedule.tile.tile_h * op.conv_attr.stride_h + op.conv_attr.kernel_h - 1) *
                op.input.w;
            uint32_t tile_weight_elems = ctx.schedule.tile.tile_oc *
                ctx.schedule.tile.tile_ic *
                op.conv_attr.kernel_h * op.conv_attr.kernel_w;
            dma.word_count = (tile_input_elems + tile_weight_elems) * bpe / 4;
        } else {
            uint32_t a_elems = ctx.schedule.tile.tile_m * ctx.schedule.tile.tile_k;
            uint32_t b_elems = ctx.schedule.tile.tile_k * ctx.schedule.tile.tile_n;
            dma.word_count = (a_elems + b_elems) * bpe / 4;
        }
        ctx.dma_rules.push_back(dma);
    }

    /* Stage 6: Block builder. */
    BlockBuilder::build(ctx);
    std::cout << "[Compiler] " << op.name
              << ": " << ctx.blocks.size() << " blocks, "
              << ctx.patches.size() << " patches\n";

    /* Stage 7: Runtime payload builder (serialize all tables). */
    PayloadBuilder::build(ctx);

    /* Stage 8: MCU firmware emitter. */
    FirmwareEmitter::emit(ctx);
    std::cout << "[Compiler] " << op.name
              << ": firmware " << ctx.package.core_firmware.size() << " words\n";

    /* Stage 9: Generate debug JSON. */
    ctx.package.debug_json = ElfPackager::generate_debug_json(ctx.package, ctx);

    result.success    = true;
    result.package    = ctx.package;
    result.debug_json = ctx.package.debug_json;
    result.context    = ctx;

    return result;
}

bool HaccCompiler::write_outputs(const CompileResult &result, const std::string &prefix) {
    if (!result.success) return false;

    /* Write HACC-ELF. */
    std::string elf_path = prefix + ".hacc.elf";
    if (!ElfPackager::write_elf(result.package, elf_path)) {
        std::cerr << "[Compiler] Failed to write ELF: " << elf_path << "\n";
        return false;
    }
    std::cout << "[Compiler] Wrote " << elf_path << "\n";

    /* Write debug JSON. */
    std::string json_path = prefix + ".debug.json";
    std::ofstream jfs(json_path);
    if (!jfs) {
        std::cerr << "[Compiler] Failed to write JSON: " << json_path << "\n";
        return false;
    }
    jfs << result.debug_json;
    std::cout << "[Compiler] Wrote " << json_path << "\n";

    return true;
}

} // namespace hacc
