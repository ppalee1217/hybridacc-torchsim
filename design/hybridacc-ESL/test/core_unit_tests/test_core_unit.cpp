#include <systemc>

#include <cstdint>
#include <iostream>

#include "Core/CoreController.hpp"
#include "Core/DecodeStage.hpp"

using namespace sc_core;

namespace {

uint32_t encode_lui(uint32_t rd, uint32_t imm20) {
	return (imm20 << 12) | (rd << 7) | 0x37u;
}

uint32_t encode_addi(uint32_t rd, uint32_t rs1, int32_t imm) {
	return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) | (0x0u << 12) | (rd << 7) | 0x13u;
}

uint32_t encode_sw(uint32_t rs2, uint32_t rs1, int32_t imm) {
	const uint32_t uimm = static_cast<uint32_t>(imm) & 0xFFFu;
	return ((uimm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (0x2u << 12) | ((uimm & 0x1Fu) << 7) | 0x23u;
}

uint32_t encode_lw(uint32_t rd, uint32_t rs1, int32_t imm) {
	return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) | (0x2u << 12) | (rd << 7) | 0x03u;
}

uint32_t encode_r_type(uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t funct3, uint32_t funct7) {
	return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x33u;
}

uint32_t encode_mul(uint32_t rd, uint32_t rs1, uint32_t rs2) {
	return encode_r_type(rd, rs1, rs2, 0x0u, 0x01u);
}

uint32_t encode_mulh(uint32_t rd, uint32_t rs1, uint32_t rs2) {
	return encode_r_type(rd, rs1, rs2, 0x1u, 0x01u);
}

uint32_t encode_mulhsu(uint32_t rd, uint32_t rs1, uint32_t rs2) {
	return encode_r_type(rd, rs1, rs2, 0x2u, 0x01u);
}

uint32_t encode_mulhu(uint32_t rd, uint32_t rs1, uint32_t rs2) {
	return encode_r_type(rd, rs1, rs2, 0x3u, 0x01u);
}

uint32_t encode_div(uint32_t rd, uint32_t rs1, uint32_t rs2) {
	return encode_r_type(rd, rs1, rs2, 0x4u, 0x01u);
}

constexpr uint32_t kEbreak = 0x00100073u;

} // namespace

int sc_main(int, char**) {
	sc_clock clk{"clk", 1, SC_NS};
	sc_signal<bool> reset_n{"reset_n"};
	sc_signal<sc_dt::sc_uint<4>> cluster_irq{"cluster_irq"};
	sc_signal<sc_dt::sc_uint<1>> nlu_irq{"nlu_irq"};
	sc_signal<bool> dma_req_valid{"dma_req_valid"};
	sc_signal<hybridacc::core::DmaRequest> dma_req{"dma_req"};
	sc_signal<bool> dma_req_ready{"dma_req_ready"};
	sc_signal<bool> cluster_req_valid{"cluster_req_valid"};
	sc_signal<hybridacc::core::ClusterMmioRequest> cluster_req{"cluster_req"};
	sc_signal<bool> cluster_req_ready{"cluster_req_ready"};
	sc_signal<bool> nlu_req_valid{"nlu_req_valid"};
	sc_signal<hybridacc::core::NluMmioRequest> nlu_req{"nlu_req"};
	sc_signal<bool> nlu_req_ready{"nlu_req_ready"};
	sc_signal<bool> controller_irq{"controller_irq"};

	hybridacc::CoreController<4, 1> dut{"dut"};
	dut.clk(clk);
	dut.reset_n(reset_n);
	dut.cluster_irq_i(cluster_irq);
	dut.nlu_irq_i(nlu_irq);
	dut.dma_req_valid_o(dma_req_valid);
	dut.dma_req_o(dma_req);
	dut.dma_req_ready_i(dma_req_ready);
	dut.cluster_req_valid_o(cluster_req_valid);
	dut.cluster_req_o(cluster_req);
	dut.cluster_req_ready_i(cluster_req_ready);
	dut.nlu_req_valid_o(nlu_req_valid);
	dut.nlu_req_o(nlu_req);
	dut.nlu_req_ready_i(nlu_req_ready);
	dut.controller_irq_o(controller_irq);

	cluster_irq.write(0);
	nlu_irq.write(0);
	dma_req_ready.write(true);
	cluster_req_ready.write(true);
	nlu_req_ready.write(true);
	reset_n.write(false);
	dut.host_set_boot_addr(0x00000000u);
	dut.host_set_trap_vector(0x00000080u);
	dut.host_set_core_enable(true);

	dut.load_instruction(0x00000000u, encode_lui(1, 0x10000u));
	dut.load_instruction(0x00000004u, encode_addi(2, 0, 42));
	dut.load_instruction(0x00000008u, encode_sw(2, 1, 0));
	dut.load_instruction(0x0000000Cu, encode_lw(3, 1, 0));
	dut.load_instruction(0x00000010u, encode_addi(4, 3, 1));
	// Zmmul tests
	dut.load_instruction(0x00000014u, encode_addi(5, 0, 7));       // x5 = 7
	dut.load_instruction(0x00000018u, encode_addi(6, 0, 8));       // x6 = 8
	dut.load_instruction(0x0000001Cu, encode_mul(7, 5, 6));        // x7 = 7*8 = 56
	dut.load_instruction(0x00000020u, encode_addi(8, 0, -1));      // x8 = 0xFFFFFFFF
	dut.load_instruction(0x00000024u, encode_mulhu(9, 8, 8));      // x9 = upper(0xFFFFFFFF * 0xFFFFFFFF) = 0xFFFFFFFE
	dut.load_instruction(0x00000028u, encode_mulh(10, 8, 8));      // x10 = upper((-1)*(-1) signed) = 0
	dut.load_instruction(0x0000002Cu, encode_mulhsu(11, 8, 8));    // x11 = upper(signed(-1)*unsigned(0xFFFFFFFF)) = 0xFFFFFFFF
	dut.load_instruction(0x00000030u, kEbreak);

	sc_start(2, SC_NS);
	reset_n.write(true);
	sc_start(120, SC_NS);

	if (!dut.debug_is_halted()) {
		std::cerr << "core did not halt" << std::endl;
		return 1;
	}
	if (dut.read_data_word(0x10000000u) != 42u) {
		std::cerr << "unexpected data SRAM value: " << dut.read_data_word(0x10000000u) << std::endl;
		return 2;
	}
	if (dut.debug_read_gpr(3) != 42u) {
		std::cerr << "lw result mismatch: " << dut.debug_read_gpr(3) << std::endl;
		return 3;
	}
	if (dut.debug_read_gpr(4) != 43u) {
		std::cerr << "addi after lw mismatch: " << dut.debug_read_gpr(4) << std::endl;
		return 4;
	}
	// Zmmul result checks
	if (dut.debug_read_gpr(7) != 56u) {
		std::cerr << "MUL 7*8 mismatch: " << dut.debug_read_gpr(7) << std::endl;
		return 5;
	}
	if (dut.debug_read_gpr(9) != 0xFFFFFFFEu) {
		std::cerr << "MULHU mismatch: 0x" << std::hex << dut.debug_read_gpr(9) << std::endl;
		return 6;
	}
	if (dut.debug_read_gpr(10) != 0x00000000u) {
		std::cerr << "MULH mismatch: 0x" << std::hex << dut.debug_read_gpr(10) << std::endl;
		return 7;
	}
	if (dut.debug_read_gpr(11) != 0xFFFFFFFFu) {
		std::cerr << "MULHSU mismatch: 0x" << std::hex << dut.debug_read_gpr(11) << std::endl;
		return 8;
	}
	// DIV/DIVU/REM/REMU trap test (decode-level: funct7=0x01, funct3=4..7 must trap)
	{
		using namespace hybridacc::core;
		const uint32_t div_funct3[] = {0x4u, 0x5u, 0x6u, 0x7u}; // DIV, DIVU, REM, REMU
		for (uint32_t f3 : div_funct3) {
			IfIdLatch ifid{true, 0u, encode_r_type(7, 5, 6, f3, 0x01u)};
			auto decoded = DecodeStage::decode(ifid);
			if (!decoded.trap) {
				std::cerr << "funct3=0x" << std::hex << f3 << " with funct7=0x01 should trap" << std::endl;
				return 9;
			}
		}
	}

	return 0;
}