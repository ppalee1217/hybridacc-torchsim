/**
 * @file firmware_emitter.hpp
 * @brief HACC Compiler MCU Firmware Emitter.
 *
 * Generates the .hacc.core MCU firmware binary that reads job/block tables,
 * streams payloads, and controls DMA/cluster/NLU as specified in the ISA.
 *
 * @see ISA.md §9.2 Block runtime suggested flow
 * @see ISA.md §10.1 Minimal example
 * @see HACC.md §6 Firmware execution model
 */
#pragma once
#include "hacc/ir.hpp"

namespace hacc {

/**
 * @brief MCU firmware emitter.
 *
 * Generates a complete MCU firmware program (as instruction words)
 * that implements the block runtime loop:
 * 1. Read job root.
 * 2. Stream PE/scan payloads.
 * 3. For each block: expand loop, apply profile, stream DMA, launch cluster.
 * 4. Handle NLU phase if needed.
 * 5. Wait for completion via IRQ.
 */
class FirmwareEmitter {
public:
    /**
     * @brief Emit firmware binary into the compiler context.
     *
     * @param[in,out] ctx  Compiler context (reads all artifacts, writes package.core_firmware).
     */
    static void emit(CompilerContext &ctx);

private:
    /**
     * @brief Emit instructions to load a 32-bit immediate into a register.
     *
     * Uses MOVI + MOVHI pair for values > 18 bits.
     *
     * @param code  Output instruction vector.
     * @param rd    Destination register.
     * @param imm   32-bit immediate value.
     */
    static void emit_load_imm32(std::vector<uint32_t> &code, uint8_t rd, uint32_t imm);

    /**
     * @brief Emit a CALL instruction to a relative offset.
     *
     * @param code           Output instruction vector.
     * @param target_offset  Target PC offset in words from current PC.
     */
    static void emit_call(std::vector<uint32_t> &code, int32_t target_offset);

    /**
     * @brief Emit the bootstrap section (read job, stream PE/scan).
     *
     * @param code  Output instruction vector.
     * @param ctx   Compiler context.
     */
    static void emit_bootstrap(std::vector<uint32_t> &code, const CompilerContext &ctx);

    /**
     * @brief Emit the block loop body.
     *
     * @param code  Output instruction vector.
     * @param ctx   Compiler context.
     */
    static void emit_block_loop(std::vector<uint32_t> &code, const CompilerContext &ctx);

    /**
     * @brief Emit the IRQ handler.
     *
     * @param code  Output instruction vector.
     */
    static void emit_irq_handler(std::vector<uint32_t> &code);
};

} // namespace hacc
