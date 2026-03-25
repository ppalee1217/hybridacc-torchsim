/**
 * @file test_compiler.cpp
 * @brief End-to-end test cases for the HACC AI Compiler.
 *
 * Demonstrates the full compilation pipeline with Conv2D and GEMM examples,
 * validates output consistency, and writes HACC-ELF + debug JSON artifacts.
 *
 * Test cases:
 * 1. Conv2D 3x3, 64ch -> 128ch, 56x56 spatial (standard ResNet-like layer)
 * 2. Conv2D 1x1, 128ch -> 256ch, 28x28 spatial (pointwise)
 * 3. Conv2D 3x3 with NLU post-processing (ReLU)
 * 4. GEMM 256x256x512 (medium-size FC layer)
 * 5. GEMM 1024x1024x2048 (large FC layer)
 * 6. GEMM with NLU post-processing
 */
#include "hacc/compiler.hpp"
#include "hacc/frontend.hpp"
#include <iostream>
#include <fstream>
#include <cassert>
#include <cstdlib>

/**
 * @brief Check a boolean condition and report pass/fail.
 * @param cond      The condition to check.
 * @param msg       Test description.
 * @param failures  Mutable failure counter.
 */
static void check(bool cond, const char *msg, int &failures) {
    if (cond) {
        std::cout << "  [PASS] " << msg << "\n";
    } else {
        std::cout << "  [FAIL] " << msg << "\n";
        ++failures;
    }
}

/**
 * @brief Test: Conv2D 3x3, 64->128ch, 56x56.
 *
 * Verifies standard convolution compilation produces valid ELF and correct
 * block/wave counts.
 */
static int test_conv2d_3x3() {
    std::cout << "\n=== Test: Conv2D 3x3 (64->128ch, 56x56) ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_conv2d(
        "conv3x3_resnet", hacc::DataType::INT16,
        /*n=*/1, /*ic=*/64, /*ih=*/56, /*iw=*/56,
        /*oc=*/128, /*kh=*/3, /*kw=*/3,
        /*stride_h=*/1, /*stride_w=*/1,
        /*pad_h=*/1, /*pad_w=*/1);

    check(op.output.h == 56, "output height = 56", failures);
    check(op.output.w == 56, "output width = 56", failures);
    check(op.output.c == 128, "output channels = 128", failures);

    hacc::HaccCompiler compiler;
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(result.context.schedule.total_waves() > 0, "total waves > 0", failures);
    check(result.context.blocks.size() > 0, "block count > 0", failures);
    check(result.package.core_firmware.size() > 0, "firmware non-empty", failures);
    check(result.package.pe_section.size() > 0, "PE payload non-empty", failures);
    check(result.package.block_section.size() > 0, "block section non-empty", failures);
    check(result.package.profile_section.size() > 0, "profile section non-empty", failures);

    compiler.write_outputs(result, "/tmp/hacc_conv3x3");

    std::cout << "  Waves: " << result.context.schedule.total_waves()
              << ", Blocks: " << result.context.blocks.size()
              << ", Patches: " << result.context.patches.size() << "\n";
    return failures;
}

/**
 * @brief Test: Conv2D 1x1 pointwise, 128->256ch, 28x28.
 */
static int test_conv2d_1x1() {
    std::cout << "\n=== Test: Conv2D 1x1 (128->256ch, 28x28) ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_conv2d(
        "conv1x1_pointwise", hacc::DataType::INT16,
        1, 128, 28, 28,
        256, 1, 1,
        1, 1, 0, 0);

    check(op.output.h == 28, "output height = 28", failures);
    check(op.output.w == 28, "output width = 28", failures);

    hacc::HaccCompiler compiler;
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(result.context.blocks.size() > 0, "block count > 0", failures);
    check(result.package.job_section.size() == 19, "job section = 19 words", failures);

    compiler.write_outputs(result, "/tmp/hacc_conv1x1");
    return failures;
}

/**
 * @brief Test: Conv2D 3x3 with NLU (ReLU) post-processing.
 */
static int test_conv2d_with_nlu() {
    std::cout << "\n=== Test: Conv2D 3x3 + NLU (32->64ch, 14x14) ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_conv2d(
        "conv3x3_relu", hacc::DataType::INT16,
        1, 32, 14, 14,
        64, 3, 3,
        1, 1, 1, 1,
        /*with_nlu=*/true);

    hacc::HaccCompiler compiler;
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(result.context.nlu_rules.size() > 0, "NLU rules generated", failures);

    /* Verify blocks have NLU flag set. */
    bool has_nlu_flag = false;
    for (const auto &b : result.context.blocks) {
        if (b.block_flags & hacc::BLOCK_FLAG_NLU_PHASE) {
            has_nlu_flag = true;
            break;
        }
    }
    check(has_nlu_flag, "block NLU flag set", failures);

    compiler.write_outputs(result, "/tmp/hacc_conv3x3_nlu");
    return failures;
}

/**
 * @brief Test: GEMM 256x256x512.
 */
static int test_gemm_medium() {
    std::cout << "\n=== Test: GEMM 256x256x512 ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_gemm(
        "gemm_medium", hacc::DataType::INT16,
        256, 256, 512);

    check(op.gemm_attr.M == 256, "M = 256", failures);
    check(op.gemm_attr.N == 256, "N = 256", failures);
    check(op.gemm_attr.K == 512, "K = 512", failures);

    hacc::HaccCompiler compiler;
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(result.context.schedule.total_waves() > 0, "total waves > 0", failures);
    check(result.package.core_firmware.size() > 0, "firmware non-empty", failures);
    check(result.package.dma_section.size() > 0, "DMA section non-empty", failures);

    compiler.write_outputs(result, "/tmp/hacc_gemm_medium");

    std::cout << "  Tile: M=" << result.context.schedule.tile.tile_m
              << " N=" << result.context.schedule.tile.tile_n
              << " K=" << result.context.schedule.tile.tile_k << "\n";
    return failures;
}

/**
 * @brief Test: GEMM 1024x1024x2048 (large).
 */
static int test_gemm_large() {
    std::cout << "\n=== Test: GEMM 1024x1024x2048 ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_gemm(
        "gemm_large", hacc::DataType::INT16,
        1024, 1024, 2048);

    hacc::HwConstraint hw;
    hw.num_clusters = 8;
    hacc::HaccCompiler compiler(hw);
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(result.context.schedule.num_clusters <= 8, "clusters <= 8", failures);
    check(result.context.dma_rules.size() == result.context.schedule.total_waves(),
          "DMA rules = total waves", failures);

    compiler.write_outputs(result, "/tmp/hacc_gemm_large");
    return failures;
}

/**
 * @brief Test: GEMM with NLU post-processing.
 */
static int test_gemm_with_nlu() {
    std::cout << "\n=== Test: GEMM 128x64x256 + NLU ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_gemm(
        "gemm_relu", hacc::DataType::INT16,
        128, 64, 256,
        false, false, /*with_nlu=*/true);

    hacc::HaccCompiler compiler;
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(result.context.nlu_rules.size() > 0, "NLU rules generated", failures);

    compiler.write_outputs(result, "/tmp/hacc_gemm_nlu");
    return failures;
}

/**
 * @brief Test: Validate debug JSON content.
 */
static int test_debug_json() {
    std::cout << "\n=== Test: Debug JSON validation ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_conv2d(
        "conv_json_test", hacc::DataType::INT16,
        1, 16, 8, 8, 32, 3, 3, 1, 1, 1, 1);

    hacc::HaccCompiler compiler;
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(!result.debug_json.empty(), "debug JSON non-empty", failures);
    check(result.debug_json.find("\"stage\": \"stage0\"") != std::string::npos,
          "JSON contains stage0 marker", failures);
    check(result.debug_json.find("conv_json_test") != std::string::npos,
          "JSON contains op name", failures);
    check(result.debug_json.find("\"blocks\"") != std::string::npos,
          "JSON contains blocks section", failures);

    return failures;
}

/**
 * @brief Test: ELF output is valid (can be read by elfdump).
 */
static int test_elf_output() {
    std::cout << "\n=== Test: ELF output validity ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_conv2d(
        "conv_elf_test", hacc::DataType::INT16,
        1, 16, 8, 8, 32, 3, 3, 1, 1, 1, 1);

    hacc::HaccCompiler compiler;
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);

    bool wrote = compiler.write_outputs(result, "/tmp/hacc_elf_test");
    check(wrote, "ELF file written", failures);

    /* Verify ELF magic by reading the file back. */
    std::ifstream ifs("/tmp/hacc_elf_test.hacc.elf", std::ios::binary);
    check(ifs.good(), "ELF file readable", failures);
    if (ifs.good()) {
        unsigned char magic[4];
        ifs.read(reinterpret_cast<char*>(magic), 4);
        check(magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F',
              "ELF magic valid", failures);
    }

    return failures;
}

/**
 * @brief Test: Block compression — verify multiple d1 iterations are merged.
 *
 * Uses a large GEMM that would produce 128 blocks without compression.
 * After compression, expects exactly 1 block with correct total_waves.
 */
static int test_block_compression() {
    std::cout << "\n=== Test: Block compression (GEMM 1024x1024x2048) ===\n";
    int failures = 0;

    auto op = hacc::Frontend::create_gemm(
        "gemm_compress", hacc::DataType::INT16,
        1024, 1024, 2048);

    hacc::HwConstraint hw;
    hw.num_clusters = 8;
    hacc::HaccCompiler compiler(hw);
    auto result = compiler.compile(op);

    check(result.success, "compilation succeeded", failures);
    check(result.context.blocks.size() == 1, "single compressed block", failures);

    if (!result.context.blocks.empty()) {
        const auto &blk = result.context.blocks[0];
        check(blk.total_waves == result.context.schedule.total_waves(),
              "block total_waves == schedule total_waves", failures);
        check(blk.rule_stride > 0, "rule_stride > 0", failures);
        check((blk.block_flags & hacc::BLOCK_FLAG_FIRST_BLOCK) != 0,
              "has FIRST_BLOCK flag", failures);
        check((blk.block_flags & hacc::BLOCK_FLAG_LAST_BLOCK) != 0,
              "has LAST_BLOCK flag", failures);
        check(blk.loop_extent[1] == result.context.schedule.loop_extents[1],
              "d1 folded into loop_extent[1]", failures);

        /* Compression ratio:  old block_words / new block_words. */
        uint32_t old_blocks = result.context.schedule.loop_extents[1];
        std::cout << "  Compressed: " << old_blocks << " blocks -> 1"
                  << ", total_waves=" << blk.total_waves
                  << ", rule_stride=" << blk.rule_stride
                  << ", block_section: " << (old_blocks * 19) << " -> 19 words\n";
    }

    compiler.write_outputs(result, "/tmp/hacc_compress_test");
    return failures;
}

/**
 * @brief Main test driver.
 *
 * Runs all test cases and reports summary.
 *
 * @return 0 if all tests pass, 1 otherwise.
 */
int main() {
    std::cout << "========================================\n";
    std::cout << " HACC AI Compiler — End-to-End Tests\n";
    std::cout << "========================================\n";

    int total_failures = 0;

    total_failures += test_conv2d_3x3();
    total_failures += test_conv2d_1x1();
    total_failures += test_conv2d_with_nlu();
    total_failures += test_gemm_medium();
    total_failures += test_gemm_large();
    total_failures += test_gemm_with_nlu();
    total_failures += test_debug_json();
    total_failures += test_elf_output();
    total_failures += test_block_compression();

    std::cout << "\n========================================\n";
    if (total_failures == 0) {
        std::cout << " ALL TESTS PASSED\n";
    } else {
        std::cout << " " << total_failures << " TEST(S) FAILED\n";
    }
    std::cout << "========================================\n";

    return total_failures == 0 ? 0 : 1;
}
