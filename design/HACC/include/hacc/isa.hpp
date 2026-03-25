/**
 * @file isa.hpp
 * @brief HACC Core MCU ISA encoding definitions.
 *
 * Defines opcode constants, instruction format helpers, and MCU address map
 * constants as specified in the HACC Core Controller ISA specification.
 *
 * @note Fixed 32-bit little-endian instructions, 4-byte aligned fetch.
 */
#pragma once
#include <cstdint>
#include <string>

namespace hacc {
namespace isa {

/// @defgroup isa_opcodes ISA Opcode Constants
/// @{

/** @brief Opcodes for Format-I / Format-R / Format-B / Format-J / Format-X. */
enum Opcode : uint8_t {
    OP_MOVI    = 0x01, ///< Load small immediate
    OP_MOVHI   = 0x02, ///< Write high immediate
    OP_MOV     = 0x03, ///< Register move
    OP_ADD     = 0x04, ///< Add
    OP_ADDI    = 0x05, ///< Add immediate
    OP_SUB     = 0x06, ///< Subtract
    OP_AND     = 0x07, ///< Bitwise AND
    OP_OR      = 0x08, ///< Bitwise OR
    OP_XOR     = 0x09, ///< Bitwise XOR
    OP_SHL     = 0x0A, ///< Logical left shift
    OP_SHR     = 0x0B, ///< Logical right shift
    OP_CMP     = 0x0C, ///< Compare (update Z/N/C/V)
    OP_CMPI    = 0x0D, ///< Compare immediate
    OP_B       = 0x10, ///< Unconditional branch
    OP_BEQ     = 0x11, ///< Branch if equal
    OP_BNE     = 0x12, ///< Branch if not equal
    OP_BLT     = 0x13, ///< Branch if signed less-than
    OP_BGE     = 0x14, ///< Branch if signed greater/equal
    OP_CALL    = 0x15, ///< Call subroutine (imm26)
    OP_CALLR   = 0x16, ///< Call subroutine (register)
    OP_RET     = 0x17, ///< Return from subroutine
    OP_NOP     = 0x18, ///< No operation
    OP_HLT     = 0x19, ///< Halt
    OP_LDW     = 0x20, ///< Load 32-bit word
    OP_STW     = 0x21, ///< Store 32-bit word
    OP_LDB     = 0x22, ///< Load byte (zero-extend)
    OP_STB     = 0x23, ///< Store byte
    OP_PUSH    = 0x24, ///< Push to stack
    OP_POP     = 0x25, ///< Pop from stack
    OP_CSRRD   = 0x28, ///< Read CSR
    OP_CSRWR   = 0x29, ///< Write CSR
    OP_CSRSI   = 0x2A, ///< CSR set bits
    OP_CSRCL   = 0x2B, ///< CSR clear bits
    OP_MMIOW   = 0x30, ///< Blocking MMIO write
    OP_MMIOR   = 0x31, ///< Blocking MMIO read
    OP_MMIOWB  = 0x32, ///< Broadcast blocking write
    OP_MMIORD  = 0x33, ///< Broadcast read
    OP_STRM    = 0x38, ///< Stream from local memory
    OP_STRMI   = 0x39, ///< Stream immediate word
    OP_STRMC   = 0x3A, ///< Stream control token
    OP_WFI     = 0x3C, ///< Wait for interrupt
    OP_WAIT    = 0x3D, ///< Wait for event
    OP_ACKIRQ  = 0x3E, ///< Acknowledge IRQ
    OP_EI      = 0x3F, ///< Enable global IRQ
    OP_DI      = 0x3F, ///< Disable global IRQ (subop differs)
    OP_IRET    = 0x3F, ///< Return from ISR (subop differs)
};

/// @}

/// @defgroup isa_regs Register Aliases
/// @{

enum RegAlias : uint8_t {
    REG_ZERO = 0,   ///< r0 = always zero
    REG_R1   = 1,   ///< r1 = argument / scratch
    REG_R2   = 2,   ///< r2
    REG_R3   = 3,   ///< r3
    REG_R4   = 4,   ///< r4
    REG_R5   = 5,   ///< r5
    REG_R6   = 6,   ///< r6
    REG_R7   = 7,   ///< r7
    REG_R8   = 8,   ///< r8
    REG_R9   = 9,   ///< r9  = callee-saved
    REG_R10  = 10,  ///< r10
    REG_R11  = 11,  ///< r11
    REG_R12  = 12,  ///< r12
    REG_SP   = 13,  ///< r13 = stack pointer
    REG_LR   = 14,  ///< r14 = link register
    REG_TMP  = 15,  ///< r15 = scratch / veneer temporary
};

/// @}

/// @defgroup isa_memmap Memory Address Map Constants
/// @{

constexpr uint32_t ISRAM_BASE          = 0x00000000; ///< Instruction SRAM base
constexpr uint32_t DATA_SRAM_BASE      = 0x10000000; ///< Data SRAM base
constexpr uint32_t LOCAL_CSR_BASE      = 0x20000000; ///< Local CSR window
constexpr uint32_t DMA_MMIO_BASE       = 0x30000000; ///< DMA MMIO window
constexpr uint32_t CLUSTER_CMD_BASE    = 0x40000000; ///< Cluster command window
constexpr uint32_t NLU_MMIO_BASE       = 0x50000000; ///< NLU MMIO window

constexpr uint32_t CLUSTER_STRIDE      = 0x00010000; ///< Per-cluster address stride
constexpr uint32_t NLU_STRIDE          = 0x00001000; ///< Per-NLU address stride

/// @}

/// @defgroup isa_dma DMA Register Offsets
/// @{

constexpr uint32_t DMA_CTRL            = 0x000; ///< DMA control
constexpr uint32_t DMA_STATUS          = 0x004; ///< DMA status
constexpr uint32_t DMA_MODE            = 0x008; ///< DMA mode
constexpr uint32_t DMA_TARGET_MASK     = 0x00C; ///< Target cluster mask
constexpr uint32_t DMA_WORD_COUNT      = 0x010; ///< Stream word count
constexpr uint32_t DMA_STREAM_DATA     = 0x100; ///< Stream data (WO)
constexpr uint32_t DMA_STREAM_CTRL     = 0x104; ///< Stream control (WO)

/// @}

/// @defgroup isa_stream Stream Destination Selectors
/// @{

constexpr uint8_t STRM_DST_DMA        = 0; ///< DMA stream sink
constexpr uint8_t STRM_DST_CLUSTER_NOC = 1; ///< Cluster NoC command sink
constexpr uint8_t STRM_DST_CLUSTER_HDDU = 2; ///< Cluster HDDU/profile payload sink
constexpr uint8_t STRM_DST_NLU_CONFIG  = 3; ///< NLU config payload sink

/// @}

/**
 * @brief Encode a Format-I instruction.
 * @param opc  6-bit opcode.
 * @param rd   4-bit destination register.
 * @param rs1  4-bit source register.
 * @param imm  18-bit immediate.
 * @return Encoded 32-bit instruction word.
 */
inline uint32_t encode_format_i(uint8_t opc, uint8_t rd, uint8_t rs1, uint32_t imm) {
    return ((uint32_t)(opc & 0x3F) << 26) |
           ((uint32_t)(rd  & 0x0F) << 22) |
           ((uint32_t)(rs1 & 0x0F) << 18) |
           (imm & 0x3FFFF);
}

/**
 * @brief Encode a Format-R instruction.
 * @param opc  6-bit opcode.
 * @param rd   4-bit destination register.
 * @param rs1  4-bit source register 1.
 * @param rs2  4-bit source register 2.
 * @param func 14-bit function code.
 * @return Encoded 32-bit instruction word.
 */
inline uint32_t encode_format_r(uint8_t opc, uint8_t rd, uint8_t rs1,
                                uint8_t rs2, uint16_t func) {
    return ((uint32_t)(opc & 0x3F) << 26) |
           ((uint32_t)(rd  & 0x0F) << 22) |
           ((uint32_t)(rs1 & 0x0F) << 18) |
           ((uint32_t)(rs2 & 0x0F) << 14) |
           (func & 0x3FFF);
}

/**
 * @brief Encode a Format-J instruction.
 * @param opc  6-bit opcode.
 * @param imm  26-bit immediate (signed, <<2).
 * @return Encoded 32-bit instruction word.
 */
inline uint32_t encode_format_j(uint8_t opc, uint32_t imm) {
    return ((uint32_t)(opc & 0x3F) << 26) | (imm & 0x03FFFFFF);
}

} // namespace isa
} // namespace hacc
