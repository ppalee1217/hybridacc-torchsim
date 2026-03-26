#pragma once

#include <systemc>

#include <cstdint>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

template <unsigned NUM_CLUSTERS = 4, unsigned NUM_NLU = 1>
SC_MODULE(Plic) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};
	sc_in<sc_uint<NUM_CLUSTERS>> cluster_irq_i{"cluster_irq_i"};
	sc_in<sc_uint<NUM_NLU>> nlu_irq_i{"nlu_irq_i"};
	sc_in<bool> dma_irq_i{"dma_irq_i"};

	sc_in<bool> mmio_req_valid_i{"mmio_req_valid_i"};
	sc_in<MmioRequest> mmio_req_i{"mmio_req_i"};
	sc_out<bool> mmio_resp_valid_o{"mmio_resp_valid_o"};
	sc_out<MmioResponse> mmio_resp_o{"mmio_resp_o"};
	sc_out<bool> meip_o{"meip_o"};
	sc_out<sc_uint<32>> pending_lo_o{"pending_lo_o"};

	SC_CTOR(Plic) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	uint32_t pending_bits() const { return pending_bits_; }
	uint32_t enable_bits() const { return enable_bits_; }

private:
	uint32_t pending_bits_ = 0u;
	uint32_t enable_bits_ = 0u;
	uint32_t claimed_id_ = 0u;

	void sample_sources() {
		for (unsigned i = 0; i < NUM_CLUSTERS; ++i) {
			if (((cluster_irq_i.read().to_uint() >> i) & 0x1u) != 0u) {
				pending_bits_ |= (1u << i);
			}
		}
		if (dma_irq_i.read()) {
			pending_bits_ |= (1u << NUM_CLUSTERS);
		}
		for (unsigned i = 0; i < NUM_NLU; ++i) {
			if (((nlu_irq_i.read().to_uint() >> i) & 0x1u) != 0u) {
				pending_bits_ |= (1u << (NUM_CLUSTERS + 1u + i));
			}
		}
	}

	uint32_t choose_claim() const {
		const uint32_t active = pending_bits_ & enable_bits_;
		for (uint32_t bit = 0; bit < 32u; ++bit) {
			if ((active & (1u << bit)) != 0u) {
				return bit + 1u;
			}
		}
		return 0u;
	}

	void seq_process() {
		mmio_resp_valid_o.write(false);
		mmio_resp_o.write(MmioResponse{});
		meip_o.write(false);
		pending_lo_o.write(0u);
		wait();

		while (true) {
			sample_sources();
			mmio_resp_valid_o.write(false);
			pending_lo_o.write(pending_bits_);
			meip_o.write((pending_bits_ & enable_bits_) != 0u);

			if (mmio_req_valid_i.read()) {
				MmioResponse response{};
				const auto request = mmio_req_i.read();
				const uint32_t offset = request.addr - kPlicBase;
				if (request.write) {
					switch (offset) {
					case kPlicEnableLoOffset:
						enable_bits_ = request.wdata;
						break;
					case kPlicCompleteOffset:
						if (request.wdata != 0u) {
							pending_bits_ &= ~(1u << (request.wdata - 1u));
							claimed_id_ = 0u;
						}
						break;
					default:
						break;
					}
				} else {
					switch (offset) {
					case kPlicPendingLoOffset: response.rdata = pending_bits_; break;
					case kPlicEnableLoOffset: response.rdata = enable_bits_; break;
					case kPlicClaimOffset:
						claimed_id_ = choose_claim();
						response.rdata = claimed_id_;
						break;
					default:
						break;
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