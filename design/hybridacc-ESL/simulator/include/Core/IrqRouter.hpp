#pragma once

#include <systemc>

#include <cstdint>

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

template <unsigned NUM_CLUSTERS = 4, unsigned NUM_NLU = 1>
SC_MODULE(IrqRouter) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_in<sc_uint<NUM_CLUSTERS>> cluster_irq_i{"cluster_irq_i"};
	sc_in<sc_uint<NUM_NLU>> nlu_irq_i{"nlu_irq_i"};
	sc_in<bool> dma_irq_i{"dma_irq_i"};

	sc_in<sc_uint<32>> irq_enable_lo_i{"irq_enable_lo_i"};
	sc_in<sc_uint<32>> irq_enable_hi_i{"irq_enable_hi_i"};
	sc_in<sc_uint<32>> irq_ack_lo_i{"irq_ack_lo_i"};
	sc_in<sc_uint<32>> irq_ack_hi_i{"irq_ack_hi_i"};

	sc_out<sc_uint<32>> irq_pending_lo_o{"irq_pending_lo_o"};
	sc_out<sc_uint<32>> irq_pending_hi_o{"irq_pending_hi_o"};
	sc_out<sc_uint<8>> irq_cause_id_o{"irq_cause_id_o"};
	sc_out<bool> irq_req_o{"irq_req_o"};
	sc_out<sc_uint<32>> irq_vector_o{"irq_vector_o"};

	SC_CTOR(IrqRouter) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

private:
	uint32_t pending_lo_ = 0u;
	uint32_t pending_hi_ = 0u;

	void seq_process() {
		irq_pending_lo_o.write(0u);
		irq_pending_hi_o.write(0u);
		irq_cause_id_o.write(0u);
		irq_req_o.write(false);
		irq_vector_o.write(0x100u);
		wait();

		while (true) {
			pending_lo_ &= ~irq_ack_lo_i.read().to_uint();
			pending_hi_ &= ~irq_ack_hi_i.read().to_uint();

			for (unsigned index = 0; index < NUM_CLUSTERS; ++index) {
				if (((cluster_irq_i.read().to_uint() >> index) & 0x1u) != 0u) {
					pending_lo_ |= (1u << index);
				}
			}
			if (dma_irq_i.read()) {
				pending_lo_ |= (1u << 16);
			}
			for (unsigned index = 0; index < NUM_NLU; ++index) {
				if (((nlu_irq_i.read().to_uint() >> index) & 0x1u) != 0u) {
					pending_lo_ |= (1u << (20u + index));
				}
			}

			irq_pending_lo_o.write(pending_lo_);
			irq_pending_hi_o.write(pending_hi_);
			const uint32_t active_lo = pending_lo_ & irq_enable_lo_i.read().to_uint();
			const uint32_t active_hi = pending_hi_ & irq_enable_hi_i.read().to_uint();
			irq_req_o.write(active_lo != 0u || active_hi != 0u);
			uint8_t cause_id = 0u;
			for (uint8_t bit = 0; bit < 32u; ++bit) {
				if ((active_lo & (1u << bit)) != 0u) {
					cause_id = bit;
					break;
				}
			}
			if (active_lo == 0u) {
				for (uint8_t bit = 0; bit < 32u; ++bit) {
					if ((active_hi & (1u << bit)) != 0u) {
						cause_id = static_cast<uint8_t>(32u + bit);
						break;
					}
				}
			}
			irq_cause_id_o.write(cause_id);
			wait();
		}
	}
};

} // namespace hybridacc::core