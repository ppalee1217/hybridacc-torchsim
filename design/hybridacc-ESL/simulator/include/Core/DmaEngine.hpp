#pragma once

#include <systemc>

#include <array>
#include <cstdint>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

SC_MODULE(DmaEngine) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_in<bool> mmio_req_valid_i{"mmio_req_valid_i"};
	sc_in<DmaMmioReq> mmio_req_i{"mmio_req_i"};
	sc_out<bool> mmio_req_ready_o{"mmio_req_ready_o"};
	sc_out<bool> mmio_resp_valid_o{"mmio_resp_valid_o"};
	sc_out<McuCmdResp> mmio_resp_o{"mmio_resp_o"};

	sc_in<bool> stream_valid_i{"stream_valid_i"};
	sc_in<sc_uint<32>> stream_data_i{"stream_data_i"};
	sc_in<sc_uint<8>> stream_flags_i{"stream_flags_i"};
	sc_out<bool> stream_ready_o{"stream_ready_o"};

	sc_out<bool> dma_req_valid_o{"dma_req_valid_o"};
	sc_out<DmaRequest> dma_req_o{"dma_req_o"};
	sc_in<bool> dma_req_ready_i{"dma_req_ready_i"};
	sc_out<bool> dma_irq_o{"dma_irq_o"};

	SC_CTOR(DmaEngine) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

private:
	uint32_t dma_mode_reg_ = 0u;
	uint32_t dma_target_mask_reg_ = 0u;
	uint32_t dma_word_count_reg_ = 0u;
	std::array<uint32_t, 2> payload_words_{};
	uint32_t payload_count_ = 0u;

	void seq_process() {
		mmio_req_ready_o.write(true);
		mmio_resp_valid_o.write(false);
		mmio_resp_o.write(McuCmdResp{});
		stream_ready_o.write(true);
		dma_req_valid_o.write(false);
		dma_req_o.write(DmaRequest{});
		dma_irq_o.write(false);
		wait();

		while (true) {
			mmio_resp_valid_o.write(false);
			dma_req_valid_o.write(false);
			dma_irq_o.write(false);

			if (stream_valid_i.read()) {
				if (payload_count_ < payload_words_.size()) {
					payload_words_[payload_count_] = stream_data_i.read().to_uint();
				}
				payload_count_ += 1u;
			}

			if (mmio_req_valid_i.read()) {
				const DmaMmioReq req = mmio_req_i.read();
				McuCmdResp resp{};
				resp.done = true;
				if (req.write) {
					switch (req.addr - kDmaMmioBase) {
					case 0x008u: dma_mode_reg_ = req.data; break;
					case 0x00Cu: dma_target_mask_reg_ = req.data; break;
					case 0x010u: dma_word_count_reg_ = req.data; break;
					case 0x000u:
						if ((req.data & 0x1u) != 0u) {
							DmaRequest dma_req{};
							dma_req.kind = DmaReqKind::WRITE64;
							dma_req.cluster_mask = dma_target_mask_reg_;
							dma_req.addr = payload_count_ > 0u ? payload_words_[0] : 0x480u;
							dma_req.data = payload_count_ > 1u ? (static_cast<uint64_t>(payload_words_[0]) << 32) | payload_words_[1] : 0ull;
							dma_req.word_count = dma_word_count_reg_;
							if (dma_req_ready_i.read()) {
								dma_req_valid_o.write(true);
								dma_req_o.write(dma_req);
								dma_irq_o.write(true);
								payload_count_ = 0u;
							}
						}
						break;
					default:
						break;
					}
				} else {
					resp.rdata = 0u;
				}
				mmio_resp_o.write(resp);
				mmio_resp_valid_o.write(true);
			}

			wait();
		}
	}
};

} // namespace hybridacc::core