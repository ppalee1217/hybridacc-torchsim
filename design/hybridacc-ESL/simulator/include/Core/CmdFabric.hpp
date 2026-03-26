#pragma once

#include <systemc>

#include <cstdint>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

SC_MODULE(CmdFabric) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_in<bool> core_mmio_req_valid_i{"core_mmio_req_valid_i"};
	sc_in<MmioRequest> core_mmio_req_i{"core_mmio_req_i"};
	sc_out<bool> core_mmio_resp_valid_o{"core_mmio_resp_valid_o"};
	sc_out<MmioResponse> core_mmio_resp_o{"core_mmio_resp_o"};

	sc_out<bool> dma_mmio_req_valid_o{"dma_mmio_req_valid_o"};
	sc_out<DmaMmioRequest> dma_mmio_req_o{"dma_mmio_req_o"};
	sc_in<bool> dma_mmio_resp_valid_i{"dma_mmio_resp_valid_i"};
	sc_in<MmioResponse> dma_mmio_resp_i{"dma_mmio_resp_i"};

	sc_out<bool> dma_stream_valid_o{"dma_stream_valid_o"};
	sc_out<sc_uint<32>> dma_stream_data_o{"dma_stream_data_o"};

	sc_out<bool> cluster_req_valid_o{"cluster_req_valid_o"};
	sc_out<ClusterMmioRequest> cluster_req_o{"cluster_req_o"};
	sc_in<bool> cluster_req_ready_i{"cluster_req_ready_i"};

	sc_out<bool> nlu_req_valid_o{"nlu_req_valid_o"};
	sc_out<NluMmioRequest> nlu_req_o{"nlu_req_o"};
	sc_in<bool> nlu_req_ready_i{"nlu_req_ready_i"};

	sc_out<bool> plic_mmio_req_valid_o{"plic_mmio_req_valid_o"};
	sc_out<MmioRequest> plic_mmio_req_o{"plic_mmio_req_o"};
	sc_in<bool> plic_mmio_resp_valid_i{"plic_mmio_resp_valid_i"};
	sc_in<MmioResponse> plic_mmio_resp_i{"plic_mmio_resp_i"};

	SC_CTOR(CmdFabric) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	uint32_t cluster_mask_lo() const { return cluster_mask_lo_reg_; }
	uint32_t cluster_mask_hi() const { return cluster_mask_hi_reg_; }
	ClusterMmioRequest last_cluster_request() const { return last_cluster_request_; }
	uint32_t last_fault_addr() const { return last_fault_addr_reg_; }
	uint32_t last_target_id() const { return last_target_id_reg_; }

private:
	uint32_t cluster_mask_lo_reg_ = 0u;
	uint32_t cluster_mask_hi_reg_ = 0u;
	uint32_t mmio_err_status_reg_ = 0u;
	uint32_t last_target_id_reg_ = 0u;
	uint32_t last_fault_addr_reg_ = 0u;
	uint32_t last_fault_info_reg_ = 0u;
	uint32_t dma_status_mirror_reg_ = 0u;
	uint32_t dma_err_code_mirror_reg_ = 0u;
	uint32_t boot_reason_reg_ = 0u;
	ClusterMmioRequest last_cluster_request_{};

	void respond_local(const MmioRequest& request, MmioResponse& response) {
		const uint32_t offset = request.addr - kLocalMmioBase;
		if (request.write) {
			switch (offset) {
			case kLocalClusterMaskLoOffset: cluster_mask_lo_reg_ = request.wdata; break;
			case kLocalClusterMaskHiOffset: cluster_mask_hi_reg_ = request.wdata; break;
			case kLocalMmioErrStatusOffset: mmio_err_status_reg_ &= ~request.wdata; break;
			case kLocalDmaErrCodeOffset: dma_err_code_mirror_reg_ &= ~request.wdata; break;
			case kLocalSwIrqSetOffset: boot_reason_reg_ = request.wdata; break;
			case kLocalSwIrqClrOffset: boot_reason_reg_ &= ~request.wdata; break;
			default: break;
			}
		} else {
			switch (offset) {
			case kLocalCoreStatusOffset: response.rdata = 0u; break;
			case kLocalClusterMaskLoOffset: response.rdata = cluster_mask_lo_reg_; break;
			case kLocalClusterMaskHiOffset: response.rdata = cluster_mask_hi_reg_; break;
			case kLocalMmioErrStatusOffset: response.rdata = mmio_err_status_reg_; break;
			case kLocalLastTargetIdOffset: response.rdata = last_target_id_reg_; break;
			case kLocalLastFaultAddrOffset: response.rdata = last_fault_addr_reg_; break;
			case kLocalLastFaultInfoOffset: response.rdata = last_fault_info_reg_; break;
			case kLocalDmaStatusOffset: response.rdata = dma_status_mirror_reg_; break;
			case kLocalDmaErrCodeOffset: response.rdata = dma_err_code_mirror_reg_; break;
			case kLocalBootReasonOffset: response.rdata = boot_reason_reg_; break;
			default: break;
			}
		}
	}

	void seq_process() {
		core_mmio_resp_valid_o.write(false);
		core_mmio_resp_o.write(MmioResponse{});
		dma_mmio_req_valid_o.write(false);
		dma_mmio_req_o.write(DmaMmioRequest{});
		dma_stream_valid_o.write(false);
		dma_stream_data_o.write(0u);
		cluster_req_valid_o.write(false);
		cluster_req_o.write(ClusterMmioRequest{});
		nlu_req_valid_o.write(false);
		nlu_req_o.write(NluMmioRequest{});
		plic_mmio_req_valid_o.write(false);
		plic_mmio_req_o.write(MmioRequest{});
		wait();

		while (true) {
			core_mmio_resp_valid_o.write(false);
			dma_mmio_req_valid_o.write(false);
			dma_stream_valid_o.write(false);
			cluster_req_valid_o.write(false);
			nlu_req_valid_o.write(false);
			plic_mmio_req_valid_o.write(false);

			if (dma_mmio_resp_valid_i.read()) {
				dma_status_mirror_reg_ = dma_mmio_resp_i.read().rdata;
			}

			if (core_mmio_req_valid_i.read()) {
				const auto request = core_mmio_req_i.read();
				MmioResponse response{};
				if (is_local_mmio(request.addr)) {
					respond_local(request, response);
					core_mmio_resp_o.write(response);
					core_mmio_resp_valid_o.write(true);
				} else if (is_dma_stream_window(request.addr) && request.write) {
					dma_stream_valid_o.write(true);
					dma_stream_data_o.write(request.wdata);
					core_mmio_resp_o.write(response);
					core_mmio_resp_valid_o.write(true);
				} else if (is_dma_mmio(request.addr)) {
					dma_mmio_req_valid_o.write(true);
					dma_mmio_req_o.write(DmaMmioRequest{request.write, request.addr, request.wdata, request.wstrb});
					core_mmio_resp_o.write(dma_mmio_resp_valid_i.read() ? dma_mmio_resp_i.read() : MmioResponse{});
					core_mmio_resp_valid_o.write(true);
				} else if (is_plic_mmio(request.addr)) {
					plic_mmio_req_valid_o.write(true);
					plic_mmio_req_o.write(request);
					core_mmio_resp_o.write(plic_mmio_resp_valid_i.read() ? plic_mmio_resp_i.read() : MmioResponse{});
					core_mmio_resp_valid_o.write(true);
				} else if (is_cluster_unicast_mmio(request.addr) || is_cluster_broadcast_mmio(request.addr)) {
					ClusterMmioRequest cluster_request{};
					cluster_request.write = request.write;
					cluster_request.is_broadcast = is_cluster_broadcast_mmio(request.addr);
					cluster_request.target_mask = cluster_request.is_broadcast ? cluster_mask_lo_reg_ : (1u << ((request.addr - kClusterMmioBase) / kClusterStride));
					cluster_request.target_id = cluster_request.is_broadcast ? 0u : ((request.addr - kClusterMmioBase) / kClusterStride);
					cluster_request.addr = cluster_request.is_broadcast ? (request.addr - kClusterBroadcastBase) : ((request.addr - kClusterMmioBase) % kClusterStride);
					cluster_request.wdata = request.wdata;
					cluster_request.wstrb = request.wstrb;
					last_cluster_request_ = cluster_request;
					last_target_id_reg_ = cluster_request.target_id;
					last_fault_addr_reg_ = request.addr;
					cluster_req_valid_o.write(true);
					cluster_req_o.write(cluster_request);
					core_mmio_resp_o.write(response);
					core_mmio_resp_valid_o.write(true);
				} else if (is_nlu_mmio(request.addr)) {
					NluMmioRequest nlu_request{};
					nlu_request.write = request.write;
					nlu_request.target_id = (request.addr - kNluMmioBase) / kNluStride;
					nlu_request.target_mask = 1u << nlu_request.target_id;
					nlu_request.addr = (request.addr - kNluMmioBase) % kNluStride;
					nlu_request.wdata = request.wdata;
					nlu_request.wstrb = request.wstrb;
					nlu_req_valid_o.write(true);
					nlu_req_o.write(nlu_request);
					core_mmio_resp_o.write(response);
					core_mmio_resp_valid_o.write(true);
				} else {
					response.error = true;
					response.error_code = 1u;
					mmio_err_status_reg_ |= 1u;
					last_fault_addr_reg_ = request.addr;
					core_mmio_resp_o.write(response);
					core_mmio_resp_valid_o.write(true);
				}
			}

			wait();
		}
	}
};

} // namespace hybridacc::core