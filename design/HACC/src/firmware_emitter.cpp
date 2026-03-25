/**
 * @file firmware_emitter.cpp
 * @brief HACC Compiler MCU Firmware Emitter implementation.
 *
 * Generates a self-contained MCU firmware program that implements
 * the complete block runtime loop per ISA.md §9.2 and §10.1.
 */
#include "hacc/firmware_emitter.hpp"
#include "hacc/isa.hpp"

namespace hacc {

using namespace isa;

void FirmwareEmitter::emit(CompilerContext &ctx) {
    std::vector<uint32_t> code;

    /*
     * Firmware layout:
     *   [0]     Bootstrap: read job, stream PE/scan, setup
     *   [N]     Block loop: iterate blocks, apply profile/DMA, launch
     *   [M]     Epilogue: HLT
     *   [M+1]   IRQ handler
     */
    emit_bootstrap(code, ctx);
    emit_block_loop(code, ctx);

    /* Epilogue: enable IRQ, wait, disable, halt. */
    code.push_back(encode_format_r(OP_EI, 0, 0, 0, 0));     /* EI */
    code.push_back(encode_format_r(OP_WFI, 0, REG_ZERO, 0, 0)); /* WFI r0 */
    code.push_back(encode_format_r(OP_DI, 0, 0, 0, 1));     /* DI (subop=1) */
    code.push_back(encode_format_r(OP_HLT, 0, 0, 0, 0));    /* HLT */

    /* IRQ handler. */
    emit_irq_handler(code);

    ctx.package.core_firmware = std::move(code);
}

void FirmwareEmitter::emit_load_imm32(std::vector<uint32_t> &code, uint8_t rd, uint32_t imm) {
    /* MOVI rd, imm[17:0] */
    code.push_back(encode_format_i(OP_MOVI, rd, 0, imm & 0x3FFFF));
    if (imm > 0x3FFFF) {
        /* MOVHI rd, imm[31:18] */
        code.push_back(encode_format_i(OP_MOVHI, rd, 0, (imm >> 18) & 0x3FFFF));
    }
}

void FirmwareEmitter::emit_call(std::vector<uint32_t> &code, int32_t target_offset) {
    /* CALL imm26: lr = next_pc, PC += imm26 << 2. */
    uint32_t imm = static_cast<uint32_t>(target_offset) & 0x03FFFFFF;
    code.push_back(encode_format_j(OP_CALL, imm));
}

void FirmwareEmitter::emit_bootstrap(std::vector<uint32_t> &code, const CompilerContext &ctx) {
    /*
     * Bootstrap sequence:
     *   1. Load Data-SRAM base into r1 (job root — table offsets relative here).
     *   2. r2 = absolute block_table_base.
     *   3. r3 = block_count.
     *   4. Stream PE program via job.pe_table_base.
     *   5. Stream scan-chain via job.scan_table_base.
     *   6. Preload loop-invariant constants into r7, r9.
     */

    /* r1 = DATA_SRAM_BASE (job section location). */
    emit_load_imm32(code, REG_R1, DATA_SRAM_BASE);

    /* r2 = absolute block table address. */
    code.push_back(encode_format_i(OP_LDW, REG_R2, REG_R1, 0));  /* offset */
    code.push_back(encode_format_r(OP_ADD, REG_R2, REG_R2, REG_R1, 0)); /* abs */

    /* r3 = block_count. */
    code.push_back(encode_format_i(OP_LDW, REG_R3, REG_R1, 4));

    /* Stream PE program: load pe_table_base from job, compute absolute addr. */
    code.push_back(encode_format_i(OP_LDW, REG_R4, REG_R1, 40)); /* pe_table_base */
    code.push_back(encode_format_r(OP_ADD, REG_R4, REG_R4, REG_R1, 0));
    code.push_back(encode_format_i(OP_LDW, REG_R5, REG_R1, 44)); /* pe_count */
    code.push_back(encode_format_r(OP_STRM, STRM_DST_CLUSTER_NOC, REG_R4, REG_R5, 0));

    /* Stream scan-chain config: load scan_table_base from job. */
    code.push_back(encode_format_i(OP_LDW, REG_R4, REG_R1, 48)); /* scan_table_base */
    code.push_back(encode_format_r(OP_ADD, REG_R4, REG_R4, REG_R1, 0));
    code.push_back(encode_format_i(OP_LDW, REG_R5, REG_R1, 52)); /* scan_count */
    code.push_back(encode_format_r(OP_STRM, STRM_DST_CLUSTER_NOC, REG_R4, REG_R5, 0));

    /* Preload constants for block/wave loop into preserved registers. */
    emit_load_imm32(code, REG_R7, DMA_MMIO_BASE + DMA_CTRL);  /* r7 = DMA ctrl addr */
    emit_load_imm32(code, REG_R9, CLUSTER_CMD_BASE);           /* r9 = cluster cmd base */
}

void FirmwareEmitter::emit_block_loop(std::vector<uint32_t> &code, const CompilerContext &ctx) {
    /*
     * Two-level loop: outer over blocks, inner over waves.
     *
     * Register allocation (preserved from bootstrap):
     *   r1  = DATA_SRAM_BASE (job root)
     *   r2  = running block descriptor pointer (absolute)
     *   r3  = block_count
     *   r7  = DMA_MMIO_BASE + DMA_CTRL (constant)
     *   r9  = CLUSTER_CMD_BASE (constant)
     *
     * Per-block temporaries:
     *   r4  = wave counter
     *   r5  = scratch
     *   r6  = cluster_mask (from block)
     *   r8  = block counter
     *   r10 = total_waves (from block)
     *   r11 = running profile pointer
     *   r12 = running DMA pointer
     *   r15 = scratch (TMP)
     */
    constexpr uint32_t BLOCK_BYTES     = 19 * 4; /* serialized block size */
    constexpr uint32_t PROFILE_BYTES   = 24 * 4; /* 96: (1<<6) + (1<<5) */
    constexpr uint32_t DMA_RULE_BYTES  = 5 * 4;  /* 20: (1<<4) + (1<<2) */

    /* Field byte-offsets inside a serialized block descriptor. */
    constexpr uint32_t OFF_CLUSTER_MASK      = 6 * 4;   /* 24 */
    constexpr uint32_t OFF_PROFILE_RULE_IDX  = 7 * 4;   /* 28 */
    constexpr uint32_t OFF_DMA_RULE_IDX      = 8 * 4;   /* 32 */
    constexpr uint32_t OFF_TOTAL_WAVES       = 15 * 4;  /* 60 */

    /* Job field byte-offsets. */
    constexpr uint32_t JOB_PROFILE_BASE = 2 * 4;  /* 8  */
    constexpr uint32_t JOB_DMA_BASE     = 4 * 4;  /* 16 */

    /* --- Outer block loop --- */

    /* MOVI r8, 0 — block counter */
    code.push_back(encode_format_i(OP_MOVI, REG_R8, 0, 0));

    uint32_t block_loop_start = static_cast<uint32_t>(code.size());

    /* CMP r8, r3 */
    code.push_back(encode_format_r(OP_CMP, 0, REG_R8, REG_R3, 0));

    /* BGE block_done (placeholder — patched later). */
    uint32_t block_bge_idx = static_cast<uint32_t>(code.size());
    code.push_back(0); /* placeholder */

    /* --- Block setup: load fields from descriptor at r2 --- */

    /* LDW r10, [r2 + OFF_TOTAL_WAVES] */
    code.push_back(encode_format_i(OP_LDW, REG_R10, REG_R2, OFF_TOTAL_WAVES));

    /* LDW r6, [r2 + OFF_CLUSTER_MASK] */
    code.push_back(encode_format_i(OP_LDW, REG_R6, REG_R2, OFF_CLUSTER_MASK));

    /*
     * Compute absolute profile start pointer:
     *   r11 = profile_table_base + profile_rule_idx * PROFILE_BYTES
     *   PROFILE_BYTES = 96 = (1<<6) + (1<<5)
     */
    code.push_back(encode_format_i(OP_LDW, REG_R11, REG_R2, OFF_PROFILE_RULE_IDX));
    code.push_back(encode_format_i(OP_SHL, REG_TMP, REG_R11, 6));  /* tmp = idx*64 */
    code.push_back(encode_format_i(OP_SHL, REG_R11, REG_R11, 5));  /* r11 = idx*32 */
    code.push_back(encode_format_r(OP_ADD, REG_R11, REG_TMP, REG_R11, 0)); /* idx*96 */
    code.push_back(encode_format_i(OP_LDW, REG_R5, REG_R1, JOB_PROFILE_BASE));
    code.push_back(encode_format_r(OP_ADD, REG_R5, REG_R5, REG_R1, 0));  /* abs base */
    code.push_back(encode_format_r(OP_ADD, REG_R11, REG_R11, REG_R5, 0)); /* ptr */

    /*
     * Compute absolute DMA start pointer:
     *   r12 = dma_table_base + dma_rule_idx * DMA_RULE_BYTES
     *   DMA_RULE_BYTES = 20 = (1<<4) + (1<<2)
     */
    code.push_back(encode_format_i(OP_LDW, REG_R12, REG_R2, OFF_DMA_RULE_IDX));
    code.push_back(encode_format_i(OP_SHL, REG_TMP, REG_R12, 4));  /* tmp = idx*16 */
    code.push_back(encode_format_i(OP_SHL, REG_R12, REG_R12, 2));  /* r12 = idx*4 */
    code.push_back(encode_format_r(OP_ADD, REG_R12, REG_TMP, REG_R12, 0)); /* idx*20 */
    code.push_back(encode_format_i(OP_LDW, REG_R5, REG_R1, JOB_DMA_BASE));
    code.push_back(encode_format_r(OP_ADD, REG_R5, REG_R5, REG_R1, 0));  /* abs base */
    code.push_back(encode_format_r(OP_ADD, REG_R12, REG_R12, REG_R5, 0)); /* ptr */

    /* --- Inner wave loop --- */

    /* MOVI r4, 0 — wave counter */
    code.push_back(encode_format_i(OP_MOVI, REG_R4, 0, 0));

    uint32_t wave_loop_start = static_cast<uint32_t>(code.size());

    /* CMP r4, r10 */
    code.push_back(encode_format_r(OP_CMP, 0, REG_R4, REG_R10, 0));

    /* BGE wave_done (placeholder). */
    uint32_t wave_bge_idx = static_cast<uint32_t>(code.size());
    code.push_back(0); /* placeholder */

    /* STRM profile to HDDU. */
    code.push_back(encode_format_i(OP_MOVI, REG_R5, 0, 24)); /* PROFILE_WORDS */
    code.push_back(encode_format_r(OP_STRM, STRM_DST_CLUSTER_HDDU, REG_R11, REG_R5, 0));
    code.push_back(encode_format_i(OP_ADDI, REG_R11, REG_R11, PROFILE_BYTES));

    /* STRM DMA rule. */
    code.push_back(encode_format_i(OP_MOVI, REG_R5, 0, 5));  /* DMA_RULE_WORDS */
    code.push_back(encode_format_r(OP_STRM, STRM_DST_DMA, REG_R12, REG_R5, 0));
    code.push_back(encode_format_i(OP_ADDI, REG_R12, REG_R12, DMA_RULE_BYTES));

    /* Start DMA: MMIOW [r7+0], r5(=1). */
    code.push_back(encode_format_i(OP_MOVI, REG_R5, 0, 1));
    code.push_back(encode_format_i(OP_MMIOW, REG_R5, REG_R7, 0));

    /* Start cluster via broadcast: MMIOWB r6, [r9+0], r5. */
    code.push_back(encode_format_r(OP_MMIOWB, REG_R6, REG_R9, REG_R5, 0));

    /* WFI r0 — wait for completion IRQ. */
    code.push_back(encode_format_r(OP_WFI, 0, REG_ZERO, 0, 0));

    /* ADDI r4, r4, 1 */
    code.push_back(encode_format_i(OP_ADDI, REG_R4, REG_R4, 1));

    /* B wave_loop_start */
    int32_t wave_back = static_cast<int32_t>(wave_loop_start) -
                        static_cast<int32_t>(code.size());
    code.push_back(encode_format_j(OP_B, static_cast<uint32_t>(wave_back) & 0x03FFFFFF));

    /* wave_done: patch the wave BGE target. */
    uint32_t wave_done_pc = static_cast<uint32_t>(code.size());
    int32_t wave_fwd = static_cast<int32_t>(wave_done_pc) -
                       static_cast<int32_t>(wave_bge_idx);
    code[wave_bge_idx] = encode_format_i(OP_BGE, REG_R4, REG_R10,
                                          static_cast<uint32_t>(wave_fwd) & 0x3FFFF);

    /* Advance block pointer: ADDI r2, r2, BLOCK_BYTES. */
    code.push_back(encode_format_i(OP_ADDI, REG_R2, REG_R2, BLOCK_BYTES));

    /* ADDI r8, r8, 1 */
    code.push_back(encode_format_i(OP_ADDI, REG_R8, REG_R8, 1));

    /* B block_loop_start */
    int32_t block_back = static_cast<int32_t>(block_loop_start) -
                         static_cast<int32_t>(code.size());
    code.push_back(encode_format_j(OP_B, static_cast<uint32_t>(block_back) & 0x03FFFFFF));

    /* block_done: patch the block BGE target. */
    uint32_t block_done_pc = static_cast<uint32_t>(code.size());
    int32_t block_fwd = static_cast<int32_t>(block_done_pc) -
                        static_cast<int32_t>(block_bge_idx);
    code[block_bge_idx] = encode_format_i(OP_BGE, REG_R8, REG_R3,
                                           static_cast<uint32_t>(block_fwd) & 0x3FFFF);
}

void FirmwareEmitter::emit_irq_handler(std::vector<uint32_t> &code) {
    /*
     * IRQ handler:
     *   CSRRD r8, IRQ_PENDING_LO
     *   ACKIRQ r8
     *   IRET
     */
    /* CSRRD r8, 0x008 (IRQ_PENDING_LO offset). */
    code.push_back(encode_format_i(OP_CSRRD, REG_R8, 0, 0x008));

    /* ACKIRQ r8 */
    code.push_back(encode_format_r(OP_ACKIRQ, 0, REG_R8, 0, 0));

    /* IRET */
    code.push_back(encode_format_r(OP_IRET, 0, 0, 0, 2)); /* subop=2 for IRET */
}

} // namespace hacc
