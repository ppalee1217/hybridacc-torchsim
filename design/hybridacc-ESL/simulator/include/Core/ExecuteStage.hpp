#pragma once

#include <cstdint>

#include "Core/PipelineTypes.hpp"

namespace hybridacc::core {

struct ExecuteStage {
	static uint32_t alu(AluOp op, uint32_t lhs, uint32_t rhs) {
		switch (op) {
		case AluOp::ADD: return lhs + rhs;
		case AluOp::SUB: return lhs - rhs;
		case AluOp::AND: return lhs & rhs;
		case AluOp::OR: return lhs | rhs;
		case AluOp::XOR: return lhs ^ rhs;
		case AluOp::SLT: return static_cast<int32_t>(lhs) < static_cast<int32_t>(rhs) ? 1u : 0u;
		case AluOp::SLTU: return lhs < rhs ? 1u : 0u;
		case AluOp::SLL: return lhs << (rhs & 0x1Fu);
		case AluOp::SRL: return lhs >> (rhs & 0x1Fu);
		case AluOp::SRA: return static_cast<uint32_t>(static_cast<int32_t>(lhs) >> (rhs & 0x1Fu));
		case AluOp::COPY_B: return rhs;
		case AluOp::MUL:    return lhs * rhs;
		case AluOp::MULH:   return static_cast<uint32_t>(static_cast<uint64_t>(
		    static_cast<int64_t>(static_cast<int32_t>(lhs)) *
		    static_cast<int64_t>(static_cast<int32_t>(rhs))) >> 32);
		case AluOp::MULHSU: return static_cast<uint32_t>(static_cast<uint64_t>(
		    static_cast<int64_t>(static_cast<int32_t>(lhs)) *
		    static_cast<int64_t>(static_cast<uint64_t>(rhs))) >> 32);
		case AluOp::MULHU:  return static_cast<uint32_t>(
		    (static_cast<uint64_t>(lhs) * static_cast<uint64_t>(rhs)) >> 32);
		default: return 0u;
		}
	}

	static bool branch_taken(BranchOp op, uint32_t lhs, uint32_t rhs) {
		switch (op) {
		case BranchOp::BEQ: return lhs == rhs;
		case BranchOp::BNE: return lhs != rhs;
		case BranchOp::BLT: return static_cast<int32_t>(lhs) < static_cast<int32_t>(rhs);
		case BranchOp::BGE: return static_cast<int32_t>(lhs) >= static_cast<int32_t>(rhs);
		case BranchOp::BLTU: return lhs < rhs;
		case BranchOp::BGEU: return lhs >= rhs;
		default: return false;
		}
	}
};

} // namespace hybridacc::core