#include <systemc>

#include <cstdint>
#include <iostream>

#include "HybridAcc.hpp"

using namespace sc_core;

namespace {

constexpr uint32_t kNop = 0x00000013u;

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

	hybridacc::HybridAcc<4, 1> dut{"dut"};
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
	dut.host_set_core_enable(true);

	dut.load_instruction(0x00000000u, encode_addi(1, 0, 3));
	dut.load_instruction(0x00000004u, encode_addi(4, 0, 1));
	dut.load_instruction(0x00000008u, encode_lui(2, 0x20000u));
	dut.load_instruction(0x0000000Cu, encode_sw(1, 2, 8));
	dut.load_instruction(0x00000010u, kNop);
	dut.load_instruction(0x00000014u, encode_lui(3, 0x50001u));
	dut.load_instruction(0x00000018u, kNop);
	dut.load_instruction(0x0000001Cu, encode_sw(4, 3, 0));
	dut.load_instruction(0x00000020u, kEbreak);

	sc_start(2, SC_NS);
	reset_n.write(true);
	sc_start(60, SC_NS);

	if (!dut.debug_is_halted()) {
		std::cerr << "integration core did not halt" << std::endl;
		return 1;
	}
	const auto request = dut.debug_last_cluster_request();
	if (!request.is_broadcast) {
		const auto mmio = dut.debug_last_mmio_request();
		std::cerr << "expected broadcast cluster request"
			<< ", pc=0x" << std::hex << dut.debug_read_pc()
			<< ", x1=0x" << dut.debug_read_gpr(1)
			<< ", x2=0x" << dut.debug_read_gpr(2)
			<< ", x3=0x" << dut.debug_read_gpr(3)
			<< ", x4=0x" << dut.debug_read_gpr(4)
			<< ", cluster_mask_lo=0x" << dut.debug_cluster_mask_lo()
			<< ", last_mmio_addr=0x" << mmio.addr
			<< ", last_mmio_data=0x" << mmio.wdata
			<< ", last_mmio_write=" << mmio.write
			<< ", last_req_addr=0x" << request.addr
			<< std::dec << std::endl;
		return 2;
	}
	if (request.target_mask != 3u) {
		std::cerr << "unexpected cluster mask: " << request.target_mask << std::endl;
		return 3;
	}
	if (request.addr != 0x1000u) {
		std::cerr << "unexpected cluster local addr: 0x" << std::hex << request.addr << std::endl;
		return 4;
	}
	if (request.wdata != 1u) {
		std::cerr << "unexpected cluster write data: " << request.wdata << std::endl;
		return 5;
	}

	return 0;
}