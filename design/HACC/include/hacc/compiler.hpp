/**
 * @file compiler.hpp
 * @brief HACC AI Compiler — End-to-end compilation pipeline.
 *
 * Orchestrates the full compilation flow from frontend OpIR to HACC-ELF:
 * @code
 *   Frontend -> Planner -> ClusterLowering -> NluLowering
 *            -> BlockBuilder -> PayloadBuilder -> FirmwareEmitter
 *            -> ElfPackager
 * @endcode
 *
 * @see HACC.md §2 Compilation chain layering
 */
#pragma once
#include "hacc/ir.hpp"
#include "hacc/planner.hpp"
#include <string>

namespace hacc {

/**
 * @brief Compilation result returned by the compiler.
 */
struct CompileResult {
    bool            success = false; ///< Whether compilation succeeded.
    std::string     error;           ///< Error message if failed.
    HaccPackage     package;         ///< Output package (valid on success).
    std::string     debug_json;      ///< Stage0 debug JSON.
    CompilerContext  context;         ///< Full compiler context for inspection.
};

/**
 * @brief HACC AI Compiler — top-level API.
 *
 * Provides a single `compile()` entry point that drives the full pipeline.
 * Optionally writes ELF and debug JSON to files.
 *
 * Usage:
 * @code
 *   auto op = Frontend::create_conv2d("conv1", DataType::INT16,
 *                                     1, 64, 56, 56, 128, 3, 3, 1, 1, 1, 1);
 *   HaccCompiler compiler;
 *   auto result = compiler.compile(op);
 *   if (result.success) {
 *       compiler.write_outputs(result, "/tmp/conv1");
 *   }
 * @endcode
 */
class HaccCompiler {
public:
    /**
     * @brief Construct a compiler with hardware constraints.
     * @param hw  Hardware constraints for the target platform.
     */
    explicit HaccCompiler(const HwConstraint &hw = HwConstraint{});

    /**
     * @brief Compile an operation through the full pipeline.
     *
     * @param op  Validated OpIR from the frontend.
     * @return CompileResult with package and debug artifacts.
     */
    CompileResult compile(const OpIR &op);

    /**
     * @brief Write compilation outputs to files.
     *
     * Produces:
     * - `<prefix>.hacc.elf`   — HACC-ELF binary for the loader.
     * - `<prefix>.debug.json` — Stage0 review JSON.
     *
     * @param result  The compilation result.
     * @param prefix  Output file path prefix (without extension).
     * @return true on success.
     */
    bool write_outputs(const CompileResult &result, const std::string &prefix);

private:
    HwConstraint hw_; ///< Target hardware constraints.
};

} // namespace hacc
