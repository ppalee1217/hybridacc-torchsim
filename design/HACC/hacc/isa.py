from __future__ import annotations

from enum import IntEnum


class Opcode(IntEnum):
    OP_MOVI = 0x01
    OP_MOVHI = 0x02
    OP_MOV = 0x03
    OP_ADD = 0x04
    OP_ADDI = 0x05
    OP_SUB = 0x06
    OP_AND = 0x07
    OP_OR = 0x08
    OP_XOR = 0x09
    OP_SHL = 0x0A
    OP_SHR = 0x0B
    OP_CMP = 0x0C
    OP_CMPI = 0x0D
    OP_B = 0x10
    OP_BEQ = 0x11
    OP_BNE = 0x12
    OP_BLT = 0x13
    OP_BGE = 0x14
    OP_CALL = 0x15
    OP_CALLR = 0x16
    OP_RET = 0x17
    OP_NOP = 0x18
    OP_HLT = 0x19
    OP_LDW = 0x20
    OP_STW = 0x21
    OP_LDB = 0x22
    OP_STB = 0x23
    OP_PUSH = 0x24
    OP_POP = 0x25
    OP_CSRRD = 0x28
    OP_CSRWR = 0x29
    OP_CSRSI = 0x2A
    OP_CSRCL = 0x2B
    OP_MMIOW = 0x30
    OP_MMIOR = 0x31
    OP_MMIOWB = 0x32
    OP_MMIORD = 0x33
    OP_STRM = 0x38
    OP_STRMI = 0x39
    OP_STRMC = 0x3A
    OP_WFI = 0x3C
    OP_WAIT = 0x3D
    OP_ACKIRQ = 0x3E
    OP_EI = 0x3F
    OP_DI = 0x3F
    OP_IRET = 0x3F


class RegAlias(IntEnum):
    REG_ZERO = 0
    REG_R1 = 1
    REG_R2 = 2
    REG_R3 = 3
    REG_R4 = 4
    REG_R5 = 5
    REG_R6 = 6
    REG_R7 = 7
    REG_R8 = 8
    REG_R9 = 9
    REG_R10 = 10
    REG_R11 = 11
    REG_R12 = 12
    REG_SP = 13
    REG_LR = 14
    REG_TMP = 15


ISRAM_BASE = 0x00000000
DATA_SRAM_BASE = 0x10000000
LOCAL_CSR_BASE = 0x20000000
DMA_MMIO_BASE = 0x30000000
CLUSTER_CMD_BASE = 0x40000000
NLU_MMIO_BASE = 0x50000000

CLUSTER_STRIDE = 0x00010000
NLU_STRIDE = 0x00001000

DMA_CTRL = 0x000
DMA_STATUS = 0x004
DMA_MODE = 0x008
DMA_TARGET_MASK = 0x00C
DMA_WORD_COUNT = 0x010
DMA_STREAM_DATA = 0x100
DMA_STREAM_CTRL = 0x104

STRM_DST_DMA = 0
STRM_DST_CLUSTER_NOC = 1
STRM_DST_CLUSTER_HDDU = 2
STRM_DST_NLU_CONFIG = 3


def encode_format_i(opc: int, rd: int, rs1: int, imm: int) -> int:
    return ((opc & 0x3F) << 26) | ((rd & 0x0F) << 22) | ((rs1 & 0x0F) << 18) | (imm & 0x3FFFF)


def encode_format_r(opc: int, rd: int, rs1: int, rs2: int, func: int) -> int:
    return ((opc & 0x3F) << 26) | ((rd & 0x0F) << 22) | ((rs1 & 0x0F) << 18) | ((rs2 & 0x0F) << 14) | (func & 0x3FFF)


def encode_format_j(opc: int, imm: int) -> int:
    return ((opc & 0x3F) << 26) | (imm & 0x03FFFFFF)