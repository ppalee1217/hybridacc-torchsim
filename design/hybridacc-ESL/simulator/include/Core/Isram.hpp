#pragma once

#include <systemc>

#include <cstdint>
#include <vector>

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

template <unsigned ISRAM_BYTES = 16384>
SC_MODULE(Isram) {
public:
	static constexpr unsigned kWordCount = ISRAM_BYTES / 4u;
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_in<bool> core_if_req_valid_i{"core_if_req_valid_i"};
	sc_in<sc_uint<32>> core_if_addr_i{"core_if_addr_i"};
	sc_out<sc_uint<32>> core_if_rdata_o{"core_if_rdata_o"};

	sc_in<bool> loader_wr_valid_i{"loader_wr_valid_i"};
	sc_in<sc_uint<32>> loader_wr_addr_i{"loader_wr_addr_i"};
	sc_in<sc_uint<32>> loader_wr_data_i{"loader_wr_data_i"};
	sc_in<sc_uint<4>> loader_wr_strb_i{"loader_wr_strb_i"};

	SC_CTOR(Isram) : words_(kWordCount, 0u) {
		SC_METHOD(comb_fetch_process);
		sensitive << core_if_req_valid_i << core_if_addr_i;

		SC_CTHREAD(seq_loader_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	void load_instruction(uint32_t byte_addr, uint32_t value) {
		const std::size_t index = static_cast<std::size_t>(byte_addr >> 2);
		if (index >= words_.size()) {
			words_.resize(index + 1u, 0u);
		}
		words_[index] = value;
	}

	uint32_t read_instruction(uint32_t byte_addr) const {
		const std::size_t index = static_cast<std::size_t>(byte_addr >> 2);
		return index < words_.size() ? words_[index] : 0u;
	}

private:
	std::vector<uint32_t> words_;

	void comb_fetch_process() {
		if (!core_if_req_valid_i.read()) {
			core_if_rdata_o.write(0u);
			return;
		}
		core_if_rdata_o.write(read_instruction(core_if_addr_i.read().to_uint()));
	}

	void seq_loader_process() {
		wait();
		while (true) {
			if (loader_wr_valid_i.read()) {
				const uint32_t addr = loader_wr_addr_i.read().to_uint();
				const std::size_t index = static_cast<std::size_t>(addr >> 2);
				if (index >= words_.size()) {
					words_.resize(index + 1u, 0u);
				}
				uint32_t value = words_[index];
				const uint32_t incoming = loader_wr_data_i.read().to_uint();
				const uint32_t strb = loader_wr_strb_i.read().to_uint();
				for (uint32_t byte_idx = 0; byte_idx < 4u; ++byte_idx) {
					if ((strb & (1u << byte_idx)) != 0u) {
						value &= ~(0xFFu << (byte_idx * 8u));
						value |= (incoming & (0xFFu << (byte_idx * 8u)));
					}
				}
				words_[index] = value;
			}
			wait();
		}
	}
};

} // namespace hybridacc::core