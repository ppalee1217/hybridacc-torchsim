#pragma once

#include <systemc>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

SC_MODULE(ClusterDataFabric) {
public:
	sc_in<bool> dma_req_valid_i{"dma_req_valid_i"};
	sc_in<DmaRequest> dma_req_i{"dma_req_i"};
	sc_out<bool> dma_req_ready_o{"dma_req_ready_o"};

	SC_CTOR(ClusterDataFabric) {
		SC_METHOD(comb_process);
		sensitive << dma_req_valid_i << dma_req_i;
	}

private:
	void comb_process() {
		dma_req_ready_o.write(true);
	}
};

} // namespace hybridacc::core