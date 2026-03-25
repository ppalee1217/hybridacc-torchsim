from __future__ import annotations

from . import isa
from .ir import CompilerContext


class FirmwareEmitter:
    @staticmethod
    def emit(ctx: CompilerContext) -> None:
        code: list[int] = []
        FirmwareEmitter.emit_bootstrap(code)
        FirmwareEmitter.emit_block_loop(code)
        code.append(isa.encode_format_r(isa.Opcode.OP_EI, 0, 0, 0, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_WFI, 0, isa.RegAlias.REG_ZERO, 0, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_DI, 0, 0, 0, 1))
        code.append(isa.encode_format_r(isa.Opcode.OP_HLT, 0, 0, 0, 0))
        FirmwareEmitter.emit_irq_handler(code)
        ctx.package.core_firmware = code

    @staticmethod
    def emit_load_imm32(code: list[int], rd: int, imm: int) -> None:
        code.append(isa.encode_format_i(isa.Opcode.OP_MOVI, rd, 0, imm & 0x3FFFF))
        if imm > 0x3FFFF:
            code.append(isa.encode_format_i(isa.Opcode.OP_MOVHI, rd, 0, (imm >> 18) & 0x3FFFF))

    @staticmethod
    def emit_bootstrap(code: list[int]) -> None:
        FirmwareEmitter.emit_load_imm32(code, isa.RegAlias.REG_R1, isa.DATA_SRAM_BASE)
        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R2, isa.RegAlias.REG_R1, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R2, isa.RegAlias.REG_R2, isa.RegAlias.REG_R1, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R3, isa.RegAlias.REG_R1, 4))

        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R4, isa.RegAlias.REG_R1, 40))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R4, isa.RegAlias.REG_R4, isa.RegAlias.REG_R1, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R5, isa.RegAlias.REG_R1, 44))
        code.append(isa.encode_format_r(isa.Opcode.OP_STRM, isa.STRM_DST_CLUSTER_NOC, isa.RegAlias.REG_R4, isa.RegAlias.REG_R5, 0))

        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R4, isa.RegAlias.REG_R1, 48))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R4, isa.RegAlias.REG_R4, isa.RegAlias.REG_R1, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R5, isa.RegAlias.REG_R1, 52))
        code.append(isa.encode_format_r(isa.Opcode.OP_STRM, isa.STRM_DST_CLUSTER_NOC, isa.RegAlias.REG_R4, isa.RegAlias.REG_R5, 0))

        FirmwareEmitter.emit_load_imm32(code, isa.RegAlias.REG_R7, isa.DMA_MMIO_BASE + isa.DMA_CTRL)
        FirmwareEmitter.emit_load_imm32(code, isa.RegAlias.REG_R9, isa.CLUSTER_CMD_BASE)

    @staticmethod
    def emit_block_loop(code: list[int]) -> None:
        block_bytes = 19 * 4
        profile_bytes = 24 * 4
        dma_rule_bytes = 5 * 4
        off_cluster_mask = 6 * 4
        off_profile_rule_idx = 7 * 4
        off_dma_rule_idx = 8 * 4
        off_total_waves = 15 * 4
        job_profile_base = 2 * 4
        job_dma_base = 4 * 4

        code.append(isa.encode_format_i(isa.Opcode.OP_MOVI, isa.RegAlias.REG_R8, 0, 0))
        block_loop_start = len(code)
        code.append(isa.encode_format_r(isa.Opcode.OP_CMP, 0, isa.RegAlias.REG_R8, isa.RegAlias.REG_R3, 0))
        block_bge_idx = len(code)
        code.append(0)

        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R10, isa.RegAlias.REG_R2, off_total_waves))
        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R6, isa.RegAlias.REG_R2, off_cluster_mask))

        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R11, isa.RegAlias.REG_R2, off_profile_rule_idx))
        code.append(isa.encode_format_i(isa.Opcode.OP_SHL, isa.RegAlias.REG_TMP, isa.RegAlias.REG_R11, 6))
        code.append(isa.encode_format_i(isa.Opcode.OP_SHL, isa.RegAlias.REG_R11, isa.RegAlias.REG_R11, 5))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R11, isa.RegAlias.REG_TMP, isa.RegAlias.REG_R11, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R5, isa.RegAlias.REG_R1, job_profile_base))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R5, isa.RegAlias.REG_R5, isa.RegAlias.REG_R1, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R11, isa.RegAlias.REG_R11, isa.RegAlias.REG_R5, 0))

        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R12, isa.RegAlias.REG_R2, off_dma_rule_idx))
        code.append(isa.encode_format_i(isa.Opcode.OP_SHL, isa.RegAlias.REG_TMP, isa.RegAlias.REG_R12, 4))
        code.append(isa.encode_format_i(isa.Opcode.OP_SHL, isa.RegAlias.REG_R12, isa.RegAlias.REG_R12, 2))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R12, isa.RegAlias.REG_TMP, isa.RegAlias.REG_R12, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_LDW, isa.RegAlias.REG_R5, isa.RegAlias.REG_R1, job_dma_base))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R5, isa.RegAlias.REG_R5, isa.RegAlias.REG_R1, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_ADD, isa.RegAlias.REG_R12, isa.RegAlias.REG_R12, isa.RegAlias.REG_R5, 0))

        code.append(isa.encode_format_i(isa.Opcode.OP_MOVI, isa.RegAlias.REG_R4, 0, 0))
        wave_loop_start = len(code)
        code.append(isa.encode_format_r(isa.Opcode.OP_CMP, 0, isa.RegAlias.REG_R4, isa.RegAlias.REG_R10, 0))
        wave_bge_idx = len(code)
        code.append(0)

        code.append(isa.encode_format_i(isa.Opcode.OP_MOVI, isa.RegAlias.REG_R5, 0, 24))
        code.append(isa.encode_format_r(isa.Opcode.OP_STRM, isa.STRM_DST_CLUSTER_HDDU, isa.RegAlias.REG_R11, isa.RegAlias.REG_R5, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_ADDI, isa.RegAlias.REG_R11, isa.RegAlias.REG_R11, profile_bytes))

        code.append(isa.encode_format_i(isa.Opcode.OP_MOVI, isa.RegAlias.REG_R5, 0, 5))
        code.append(isa.encode_format_r(isa.Opcode.OP_STRM, isa.STRM_DST_DMA, isa.RegAlias.REG_R12, isa.RegAlias.REG_R5, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_ADDI, isa.RegAlias.REG_R12, isa.RegAlias.REG_R12, dma_rule_bytes))

        code.append(isa.encode_format_i(isa.Opcode.OP_MOVI, isa.RegAlias.REG_R5, 0, 1))
        code.append(isa.encode_format_i(isa.Opcode.OP_MMIOW, isa.RegAlias.REG_R5, isa.RegAlias.REG_R7, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_MMIOWB, isa.RegAlias.REG_R6, isa.RegAlias.REG_R9, isa.RegAlias.REG_R5, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_WFI, 0, isa.RegAlias.REG_ZERO, 0, 0))
        code.append(isa.encode_format_i(isa.Opcode.OP_ADDI, isa.RegAlias.REG_R4, isa.RegAlias.REG_R4, 1))

        wave_back = (wave_loop_start - len(code)) & 0x03FFFFFF
        code.append(isa.encode_format_j(isa.Opcode.OP_B, wave_back))
        wave_done_pc = len(code)
        wave_fwd = (wave_done_pc - wave_bge_idx) & 0x3FFFF
        code[wave_bge_idx] = isa.encode_format_i(isa.Opcode.OP_BGE, isa.RegAlias.REG_R4, isa.RegAlias.REG_R10, wave_fwd)

        code.append(isa.encode_format_i(isa.Opcode.OP_ADDI, isa.RegAlias.REG_R2, isa.RegAlias.REG_R2, block_bytes))
        code.append(isa.encode_format_i(isa.Opcode.OP_ADDI, isa.RegAlias.REG_R8, isa.RegAlias.REG_R8, 1))
        block_back = (block_loop_start - len(code)) & 0x03FFFFFF
        code.append(isa.encode_format_j(isa.Opcode.OP_B, block_back))
        block_done_pc = len(code)
        block_fwd = (block_done_pc - block_bge_idx) & 0x3FFFF
        code[block_bge_idx] = isa.encode_format_i(isa.Opcode.OP_BGE, isa.RegAlias.REG_R8, isa.RegAlias.REG_R3, block_fwd)

    @staticmethod
    def emit_irq_handler(code: list[int]) -> None:
        code.append(isa.encode_format_i(isa.Opcode.OP_CSRRD, isa.RegAlias.REG_R8, 0, 0x008))
        code.append(isa.encode_format_r(isa.Opcode.OP_ACKIRQ, 0, isa.RegAlias.REG_R8, 0, 0))
        code.append(isa.encode_format_r(isa.Opcode.OP_IRET, 0, 0, 0, 2))