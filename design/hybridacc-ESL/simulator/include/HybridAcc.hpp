#pragma once

#include <systemc>

#include "Core.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {

template <unsigned NUM_CLUSTERS = 4, unsigned NUM_NLU = 1, unsigned ISRAM_BYTES = 16384, unsigned DATA_SRAM_BYTES = 65536>
SC_MODULE(HybridAcc) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};
	sc_in<sc_uint<NUM_CLUSTERS>> cluster_irq_i{"cluster_irq_i"};
	sc_in<sc_uint<NUM_NLU>> nlu_irq_i{"nlu_irq_i"};

	sc_out<bool> dma_req_valid_o{"dma_req_valid_o"};
	sc_out<core::DmaRequest> dma_req_o{"dma_req_o"};
	sc_in<bool> dma_req_ready_i{"dma_req_ready_i"};
	sc_out<bool> cluster_req_valid_o{"cluster_req_valid_o"};
	sc_out<core::ClusterMmioRequest> cluster_req_o{"cluster_req_o"};
	sc_in<bool> cluster_req_ready_i{"cluster_req_ready_i"};
	sc_out<bool> nlu_req_valid_o{"nlu_req_valid_o"};
	sc_out<core::NluMmioRequest> nlu_req_o{"nlu_req_o"};
	sc_in<bool> nlu_req_ready_i{"nlu_req_ready_i"};
	sc_out<bool> controller_irq_o{"controller_irq_o"};

	CoreController<NUM_CLUSTERS, NUM_NLU, ISRAM_BYTES, DATA_SRAM_BYTES> core{"core"};

	SC_CTOR(HybridAcc) {
		core.clk(clk);
		core.reset_n(reset_n);
		core.cluster_irq_i(cluster_irq_i);
		core.nlu_irq_i(nlu_irq_i);
		core.dma_req_valid_o(dma_req_valid_o);
		core.dma_req_o(dma_req_o);
		core.dma_req_ready_i(dma_req_ready_i);
		core.cluster_req_valid_o(cluster_req_valid_o);
		core.cluster_req_o(cluster_req_o);
		core.cluster_req_ready_i(cluster_req_ready_i);
		core.nlu_req_valid_o(nlu_req_valid_o);
		core.nlu_req_o(nlu_req_o);
		core.nlu_req_ready_i(nlu_req_ready_i);
		core.controller_irq_o(controller_irq_o);
	}

	void load_instruction(uint32_t byte_addr, uint32_t value) { core.load_instruction(byte_addr, value); }
	void load_data_word(uint32_t byte_addr, uint32_t value) { core.load_data_word(byte_addr, value); }
	uint32_t read_data_word(uint32_t byte_addr) const { return core.read_data_word(byte_addr); }
	void host_set_boot_addr(uint32_t addr) { core.host_set_boot_addr(addr); }
	void host_set_trap_vector(uint32_t addr) { core.host_set_trap_vector(addr); }
	void host_set_core_enable(bool enable) { core.host_set_core_enable(enable); }
	uint32_t debug_read_pc() const { return core.debug_read_pc(); }
	uint32_t debug_read_gpr(uint32_t index) const { return core.debug_read_gpr(index); }
	bool debug_is_halted() const { return core.debug_is_halted(); }
	core::MmioRequest debug_last_mmio_request() const { return core.debug_last_mmio_request(); }
	core::ClusterMmioRequest debug_last_cluster_request() const { return core.debug_last_cluster_request(); }
	uint32_t debug_cluster_mask_lo() const { return core.debug_cluster_mask_lo(); }
};

} // namespace hybridacc
