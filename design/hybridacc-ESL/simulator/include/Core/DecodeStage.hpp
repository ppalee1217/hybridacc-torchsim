#pragma once

#include <cstdint>

#include "Core/PipelineTypes.hpp"

namespace hybridacc::core {

struct DecodeStage {
	static uint32_t sign_extend(uint32_t value, unsigned bits) {
		const uint32_t sign_bit = 1u << (bits - 1u);
		const uint32_t mask = (bits == 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
		value &= mask;
		return (value ^ sign_bit) - sign_bit;
	}

	static DecodedInstruction decode(const IfIdLatch& ifid) {
		DecodedInstruction decoded{};
		decoded.valid = ifid.valid;
		decoded.pc = ifid.pc;
		decoded.instruction = ifid.instruction;
		if (!ifid.valid) {
			return decoded;
		}

		const uint32_t inst = ifid.instruction;
		const uint32_t opcode = inst & 0x7Fu;
		const uint32_t funct3 = (inst >> 12) & 0x7u;
		const uint32_t funct7 = (inst >> 25) & 0x7Fu;
		decoded.rd = static_cast<uint8_t>((inst >> 7) & 0x1Fu);
		decoded.rs1 = static_cast<uint8_t>((inst >> 15) & 0x1Fu);
		decoded.rs2 = static_cast<uint8_t>((inst >> 20) & 0x1Fu);
		decoded.csr = static_cast<uint16_t>((inst >> 20) & 0xFFFu);

		switch (opcode) {
		case 0x37u:
			decoded.alu_op = AluOp::COPY_B;
			decoded.imm = inst & 0xFFFFF000u;
			decoded.reg_write = true;
			break;
		case 0x17u:
			decoded.alu_op = AluOp::ADD;
			decoded.imm = inst & 0xFFFFF000u;
			decoded.reg_write = true;
			decoded.use_rs1 = false;
			break;
		case 0x13u:
			decoded.use_rs1 = true;
			decoded.reg_write = true;
			decoded.imm = sign_extend(inst >> 20, 12u);
			switch (funct3) {
			case 0x0u: decoded.alu_op = AluOp::ADD; break;
			case 0x2u: decoded.alu_op = AluOp::SLT; break;
			case 0x3u: decoded.alu_op = AluOp::SLTU; break;
			case 0x4u: decoded.alu_op = AluOp::XOR; break;
			case 0x6u: decoded.alu_op = AluOp::OR; break;
			case 0x7u: decoded.alu_op = AluOp::AND; break;
			case 0x1u: decoded.alu_op = AluOp::SLL; decoded.imm = (inst >> 20) & 0x1Fu; break;
			case 0x5u:
				decoded.alu_op = ((inst >> 30) & 0x1u) != 0u ? AluOp::SRA : AluOp::SRL;
				decoded.imm = (inst >> 20) & 0x1Fu;
				break;
			default: decoded.trap = true; break;
			}
			break;
		case 0x33u:
			decoded.use_rs1 = true;
			decoded.use_rs2 = true;
			decoded.reg_write = true;
			switch (funct3) {
		case 0x0u:
			decoded.alu_op = (funct7 == 0x01u) ? AluOp::MUL
			               : (funct7 == 0x20u) ? AluOp::SUB : AluOp::ADD;
			break;
		case 0x1u: decoded.alu_op = (funct7 == 0x01u) ? AluOp::MULH   : AluOp::SLL;  break;
		case 0x2u: decoded.alu_op = (funct7 == 0x01u) ? AluOp::MULHSU : AluOp::SLT;  break;
		case 0x3u: decoded.alu_op = (funct7 == 0x01u) ? AluOp::MULHU  : AluOp::SLTU; break;
		case 0x4u:
			if (funct7 == 0x01u) decoded.trap = true;
			else decoded.alu_op = AluOp::XOR;
			break;
		case 0x5u:
			if (funct7 == 0x01u) decoded.trap = true;
			else decoded.alu_op = (funct7 == 0x20u) ? AluOp::SRA : AluOp::SRL;
			break;
		case 0x6u:
			if (funct7 == 0x01u) decoded.trap = true;
			else decoded.alu_op = AluOp::OR;
			break;
		case 0x7u:
			if (funct7 == 0x01u) decoded.trap = true;
			else decoded.alu_op = AluOp::AND;
			break;
			default: decoded.trap = true; break;
			}
			break;
		case 0x03u:
			decoded.use_rs1 = true;
			decoded.reg_write = true;
			decoded.mem_read = true;
			decoded.imm = sign_extend(inst >> 20, 12u);
			switch (funct3) {
			case 0x0u: decoded.mem_op = MemOp::LB; break;
			case 0x1u: decoded.mem_op = MemOp::LH; break;
			case 0x2u: decoded.mem_op = MemOp::LW; break;
			case 0x4u: decoded.mem_op = MemOp::LBU; break;
			case 0x5u: decoded.mem_op = MemOp::LHU; break;
			default: decoded.trap = true; break;
			}
			break;
		case 0x23u:
			decoded.use_rs1 = true;
			decoded.use_rs2 = true;
			decoded.mem_write = true;
			decoded.imm = sign_extend(((inst >> 25) << 5) | ((inst >> 7) & 0x1Fu), 12u);
			switch (funct3) {
			case 0x0u: decoded.mem_op = MemOp::SB; break;
			case 0x1u: decoded.mem_op = MemOp::SH; break;
			case 0x2u: decoded.mem_op = MemOp::SW; break;
			default: decoded.trap = true; break;
			}
			break;
		case 0x63u:
			decoded.use_rs1 = true;
			decoded.use_rs2 = true;
			decoded.imm = sign_extend((((inst >> 31) & 0x1u) << 12) | (((inst >> 7) & 0x1u) << 11) |
				(((inst >> 25) & 0x3Fu) << 5) | (((inst >> 8) & 0xFu) << 1), 13u);
			switch (funct3) {
			case 0x0u: decoded.branch_op = BranchOp::BEQ; break;
			case 0x1u: decoded.branch_op = BranchOp::BNE; break;
			case 0x4u: decoded.branch_op = BranchOp::BLT; break;
			case 0x5u: decoded.branch_op = BranchOp::BGE; break;
			case 0x6u: decoded.branch_op = BranchOp::BLTU; break;
			case 0x7u: decoded.branch_op = BranchOp::BGEU; break;
			default: decoded.trap = true; break;
			}
			break;
		case 0x6Fu:
			decoded.branch_op = BranchOp::JAL;
			decoded.reg_write = true;
			decoded.imm = sign_extend((((inst >> 31) & 0x1u) << 20) | (((inst >> 12) & 0xFFu) << 12) |
				(((inst >> 20) & 0x1u) << 11) | (((inst >> 21) & 0x3FFu) << 1), 21u);
			break;
		case 0x67u:
			decoded.branch_op = BranchOp::JALR;
			decoded.use_rs1 = true;
			decoded.reg_write = true;
			decoded.imm = sign_extend(inst >> 20, 12u);
			break;
		case 0x73u:
			if (funct3 == 0u) {
				decoded.halt = (inst == 0x00100073u);
				decoded.trap = !decoded.halt && (inst != 0x00000013u);
			} else {
				decoded.reg_write = true;
				decoded.use_rs1 = funct3 < 0x5u;
				switch (funct3) {
				case 0x1u: decoded.csr_op = CsrOp::CSRRW; break;
				case 0x2u: decoded.csr_op = CsrOp::CSRRS; break;
				case 0x3u: decoded.csr_op = CsrOp::CSRRC; break;
				case 0x5u: decoded.csr_op = CsrOp::CSRRWI; break;
				case 0x6u: decoded.csr_op = CsrOp::CSRRSI; break;
				case 0x7u: decoded.csr_op = CsrOp::CSRRCI; break;
				default: decoded.trap = true; break;
				}
				decoded.imm = decoded.rs1;
			}
			break;
		default:
			decoded.trap = true;
			break;
		}

		return decoded;
	}
};

} // namespace hybridacc::core