#include <systemc>

#include <cstdint>
#include <iostream>

#include "Core/CoreController.hpp"

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
	dut.load_instruction(0x00000014u, kEbreak);

	sc_start(2, SC_NS);
	reset_n.write(true);
	sc_start(40, SC_NS);

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

	return 0;
}