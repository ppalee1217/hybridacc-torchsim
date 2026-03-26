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
	sc_in<DmaMmioRequest> mmio_req_i{"mmio_req_i"};
	sc_out<bool> mmio_resp_valid_o{"mmio_resp_valid_o"};
	sc_out<MmioResponse> mmio_resp_o{"mmio_resp_o"};

	sc_in<bool> stream_valid_i{"stream_valid_i"};
	sc_in<sc_uint<32>> stream_data_i{"stream_data_i"};
	sc_out<bool> stream_ready_o{"stream_ready_o"};

	sc_out<bool> dma_req_valid_o{"dma_req_valid_o"};
	sc_out<DmaRequest> dma_req_o{"dma_req_o"};
	sc_in<bool> dma_req_ready_i{"dma_req_ready_i"};
	sc_out<bool> dma_irq_o{"dma_irq_o"};

	SC_CTOR(DmaEngine) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	uint32_t status() const { return status_reg_; }
	uint32_t error_code() const { return error_code_reg_; }

private:
	uint32_t status_reg_ = 0u;
	uint32_t error_code_reg_ = 0u;
	uint32_t cluster_mask_reg_ = 0u;
	uint32_t word_count_reg_ = 0u;
	uint32_t addr_reg_ = 0u;
	std::array<uint32_t, 2> payload_words_{};
	uint32_t payload_count_ = 0u;

	void seq_process() {
		mmio_resp_valid_o.write(false);
		mmio_resp_o.write(MmioResponse{});
		stream_ready_o.write(true);
		dma_req_valid_o.write(false);
		dma_req_o.write(DmaRequest{});
		dma_irq_o.write(false);
		wait();

		while (true) {
			mmio_resp_valid_o.write(false);
			dma_req_valid_o.write(false);
			dma_irq_o.write(false);

			if (stream_valid_i.read() && payload_count_ < payload_words_.size()) {
				payload_words_[payload_count_++] = stream_data_i.read().to_uint();
			}

			if (mmio_req_valid_i.read()) {
				MmioResponse response{};
				const auto request = mmio_req_i.read();
				const uint32_t offset = request.addr - kDmaMmioBase;
				if (request.write) {
					switch (offset) {
					case 0x000u:
						if ((request.wdata & kDmaCtrlStartBit) != 0u) {
							DmaRequest dma_request{};
							dma_request.cluster_mask = cluster_mask_reg_;
							dma_request.addr = addr_reg_;
							dma_request.word_count = word_count_reg_;
							dma_request.data = payload_count_ >= 2u ? (static_cast<uint64_t>(payload_words_[1]) << 32) | payload_words_[0] : 0u;
							status_reg_ = kDmaStatusBusyBit;
							if (dma_req_ready_i.read()) {
								dma_req_valid_o.write(true);
								dma_req_o.write(dma_request);
								status_reg_ = kDmaStatusDoneBit;
								dma_irq_o.write(true);
								payload_count_ = 0u;
							}
						}
						break;
					case 0x008u: cluster_mask_reg_ = request.wdata; break;
					case 0x00Cu: word_count_reg_ = request.wdata; break;
					case 0x010u: addr_reg_ = request.wdata; break;
					case 0x014u: error_code_reg_ &= ~request.wdata; break;
					default: break;
					}
				} else {
					switch (offset) {
					case 0x000u: response.rdata = status_reg_; break;
					case 0x008u: response.rdata = cluster_mask_reg_; break;
					case 0x00Cu: response.rdata = word_count_reg_; break;
					case 0x010u: response.rdata = addr_reg_; break;
					case 0x014u: response.rdata = error_code_reg_; break;
					default: break;
					}
				}
				mmio_resp_o.write(response);
				mmio_resp_valid_o.write(true);
			}

			wait();
		}
	}
};

} // namespace hybridacc::core