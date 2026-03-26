#pragma once

#include <cstdint>

namespace hybridacc::core {

enum class AluOp : uint8_t {
	NONE = 0,
	ADD,
	SUB,
	AND,
	OR,
	XOR,
	SLT,
	SLTU,
	SLL,
	SRL,
	SRA,
	COPY_B,
};

enum class BranchOp : uint8_t {
	NONE = 0,
	BEQ,
	BNE,
	BLT,
	BGE,
	BLTU,
	BGEU,
	JAL,
	JALR,
};

enum class MemOp : uint8_t {
	NONE = 0,
	LB,
	LH,
	LW,
	LBU,
	LHU,
	SB,
	SH,
	SW,
};

enum class CsrOp : uint8_t {
	NONE = 0,
	CSRRW,
	CSRRS,
	CSRRC,
	CSRRWI,
	CSRRSI,
	CSRRCI,
};

struct DecodedInstruction {
	bool valid = false;
	uint32_t pc = 0;
	uint32_t instruction = 0;
	uint8_t rd = 0;
	uint8_t rs1 = 0;
	uint8_t rs2 = 0;
	uint32_t imm = 0;
	uint16_t csr = 0;
	bool use_rs1 = false;
	bool use_rs2 = false;
	bool reg_write = false;
	bool mem_read = false;
	bool mem_write = false;
	bool halt = false;
	bool trap = false;
	AluOp alu_op = AluOp::NONE;
	BranchOp branch_op = BranchOp::NONE;
	MemOp mem_op = MemOp::NONE;
	CsrOp csr_op = CsrOp::NONE;
};

struct IfIdLatch {
	bool valid = false;
	uint32_t pc = 0;
	uint32_t instruction = 0;
};

struct IdExLatch {
	bool valid = false;
	DecodedInstruction decoded{};
	uint32_t rs1_value = 0;
	uint32_t rs2_value = 0;
};

struct ExMemLatch {
	bool valid = false;
	DecodedInstruction decoded{};
	uint32_t alu_result = 0;
	uint32_t rs2_value = 0;
	uint32_t csr_old_value = 0;
	uint32_t csr_new_value = 0;
	bool branch_taken = false;
	uint32_t branch_target = 0;
	bool fault = false;
	uint32_t fault_code = 0;
};

struct MemWbLatch {
	bool valid = false;
	uint8_t rd = 0;
	bool reg_write = false;
	uint32_t write_value = 0;
	bool csr_write = false;
	uint16_t csr = 0;
	uint32_t csr_value = 0;
	bool halt = false;
	bool fault = false;
	uint32_t fault_code = 0;
	uint32_t pc = 0;
};

struct MemoryTransaction {
	bool active = false;
	bool request_issued = false;
	bool uses_mmio = false;
	ExMemLatch exmem{};
};

} // namespace hybridacc::core