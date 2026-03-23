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

	sc_in<bool> mcu_cmd_req_valid_i{"mcu_cmd_req_valid_i"};
	sc_in<McuCmdReq> mcu_cmd_req_i{"mcu_cmd_req_i"};
	sc_out<bool> mcu_cmd_req_ready_o{"mcu_cmd_req_ready_o"};
	sc_out<bool> mcu_cmd_resp_valid_o{"mcu_cmd_resp_valid_o"};
	sc_out<McuCmdResp> mcu_cmd_resp_o{"mcu_cmd_resp_o"};

	sc_out<bool> dma_mmio_req_valid_o{"dma_mmio_req_valid_o"};
	sc_out<DmaMmioReq> dma_mmio_req_o{"dma_mmio_req_o"};
	sc_in<bool> dma_mmio_req_ready_i{"dma_mmio_req_ready_i"};
	sc_in<bool> dma_mmio_resp_valid_i{"dma_mmio_resp_valid_i"};
	sc_in<McuCmdResp> dma_mmio_resp_i{"dma_mmio_resp_i"};

	sc_out<bool> dma_stream_valid_o{"dma_stream_valid_o"};
	sc_out<sc_uint<32>> dma_stream_data_o{"dma_stream_data_o"};
	sc_out<sc_uint<8>> dma_stream_flags_o{"dma_stream_flags_o"};
	sc_in<bool> dma_stream_ready_i{"dma_stream_ready_i"};

	sc_out<bool> cluster_cmd_valid_o{"cluster_cmd_valid_o"};
	sc_out<ClusterCommand> cluster_cmd_o{"cluster_cmd_o"};
	sc_in<bool> cluster_cmd_ready_i{"cluster_cmd_ready_i"};

	sc_out<bool> nlu_cmd_valid_o{"nlu_cmd_valid_o"};
	sc_out<NluCommand> nlu_cmd_o{"nlu_cmd_o"};
	sc_in<bool> nlu_cmd_ready_i{"nlu_cmd_ready_i"};

	SC_CTOR(CmdFabric) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

private:
	void seq_process() {
		mcu_cmd_req_ready_o.write(true);
		mcu_cmd_resp_valid_o.write(false);
		mcu_cmd_resp_o.write(McuCmdResp{});
		dma_mmio_req_valid_o.write(false);
		dma_mmio_req_o.write(DmaMmioReq{});
		dma_stream_valid_o.write(false);
		dma_stream_data_o.write(0u);
		dma_stream_flags_o.write(0u);
		cluster_cmd_valid_o.write(false);
		cluster_cmd_o.write(ClusterCommand{});
		nlu_cmd_valid_o.write(false);
		nlu_cmd_o.write(NluCommand{});
		wait();

		while (true) {
			mcu_cmd_resp_valid_o.write(false);
			dma_mmio_req_valid_o.write(false);
			dma_stream_valid_o.write(false);
			cluster_cmd_valid_o.write(false);
			nlu_cmd_valid_o.write(false);

			if (dma_mmio_resp_valid_i.read()) {
				mcu_cmd_resp_o.write(dma_mmio_resp_i.read());
				mcu_cmd_resp_valid_o.write(true);
			}

			if (mcu_cmd_req_valid_i.read()) {
				const McuCmdReq req = mcu_cmd_req_i.read();
				McuCmdResp resp{};
				resp.done = true;
				const bool is_dma_addr = req.addr >= kDmaMmioBase && req.addr < kClusterCmdBase;
				const bool is_cluster_addr = req.addr >= kClusterCmdBase && req.addr < kNluMmioBase;
				const bool is_nlu_addr = req.addr >= kNluMmioBase;

				if ((req.kind == CommandKind::MMIO_WRITE || req.kind == CommandKind::MMIO_READ) && is_dma_addr) {
					DmaMmioReq mmio_req{};
					mmio_req.write = req.kind == CommandKind::MMIO_WRITE;
					mmio_req.addr = req.addr;
					mmio_req.data = req.data;
					dma_mmio_req_o.write(mmio_req);
					dma_mmio_req_valid_o.write(true);
					if (!dma_mmio_req_ready_i.read()) {
						resp.done = false;
					}
					mcu_cmd_resp_o.write(resp);
					mcu_cmd_resp_valid_o.write(true);
				} else if (req.kind == CommandKind::STREAM_WORD && req.stream_dst == StreamDestination::DMA) {
					dma_stream_data_o.write(req.data);
					dma_stream_flags_o.write(req.stream_flags);
					dma_stream_valid_o.write(true);
					resp.done = dma_stream_ready_i.read();
					mcu_cmd_resp_o.write(resp);
					mcu_cmd_resp_valid_o.write(true);
				} else if ((req.kind == CommandKind::MMIO_WRITE || req.kind == CommandKind::MMIO_WRITE_BROADCAST || req.kind == CommandKind::STREAM_WORD) && is_cluster_addr) {
					ClusterCommand cmd{};
					cmd.cluster_mask = req.target_mask == 0u ? 1u : req.target_mask;
					cmd.addr = req.addr - kClusterCmdBase;
					cmd.data = req.data;
					cmd.stream_flags = static_cast<uint8_t>(req.stream_flags);
					cmd.op = req.kind == CommandKind::STREAM_WORD ? (req.stream_dst == StreamDestination::CLUSTER_NOC ? ClusterCommandOp::NOC_STREAM : ClusterCommandOp::HDDU_STREAM) : ClusterCommandOp::MMIO_WRITE;
					cluster_cmd_o.write(cmd);
					cluster_cmd_valid_o.write(true);
					resp.done = cluster_cmd_ready_i.read();
					mcu_cmd_resp_o.write(resp);
					mcu_cmd_resp_valid_o.write(true);
				} else if ((req.kind == CommandKind::MMIO_WRITE || req.kind == CommandKind::MMIO_WRITE_BROADCAST || req.kind == CommandKind::STREAM_WORD) && (is_nlu_addr || req.stream_dst == StreamDestination::NLU_CFG)) {
					NluCommand cmd{};
					cmd.nlu_mask = req.target_mask == 0u ? 1u : req.target_mask;
					cmd.addr = is_nlu_addr ? (req.addr - kNluMmioBase) : 0u;
					cmd.data = req.data;
					cmd.stream_flags = static_cast<uint8_t>(req.stream_flags);
					nlu_cmd_o.write(cmd);
					nlu_cmd_valid_o.write(true);
					resp.done = nlu_cmd_ready_i.read();
					mcu_cmd_resp_o.write(resp);
					mcu_cmd_resp_valid_o.write(true);
				} else {
					mcu_cmd_resp_o.write(resp);
					mcu_cmd_resp_valid_o.write(true);
				}
			}

			wait();
		}
	}
};

} // namespace hybridacc::core