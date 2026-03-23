#pragma once

#include <systemc>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

SC_MODULE(ClusterDataFabric) {
public:
	sc_in<DataFabricReq> loader_req_i{"loader_req_i"};
	sc_in<bool> loader_req_valid_i{"loader_req_valid_i"};
	sc_out<bool> loader_req_ready_o{"loader_req_ready_o"};
	sc_in<DataFabricReq> dma_req_i{"dma_req_i"};
	sc_in<bool> dma_req_valid_i{"dma_req_valid_i"};
	sc_out<bool> dma_req_ready_o{"dma_req_ready_o"};
	sc_out<DataFabricReq> req_o{"req_o"};
	sc_out<bool> req_valid_o{"req_valid_o"};
	sc_in<bool> req_ready_i{"req_ready_i"};

	SC_CTOR(ClusterDataFabric) {
		SC_METHOD(comb_process);
		sensitive << loader_req_i << loader_req_valid_i << dma_req_i << dma_req_valid_i << req_ready_i;
	}

private:
	void comb_process() {
		loader_req_ready_o.write(false);
		dma_req_ready_o.write(false);
		req_o.write(DataFabricReq{});
		req_valid_o.write(false);
		if (loader_req_valid_i.read()) {
			req_o.write(loader_req_i.read());
			req_valid_o.write(true);
			loader_req_ready_o.write(req_ready_i.read());
		} else if (dma_req_valid_i.read()) {
			req_o.write(dma_req_i.read());
			req_valid_o.write(true);
			dma_req_ready_o.write(req_ready_i.read());
		}
	}
};

} // namespace hybridacc::core