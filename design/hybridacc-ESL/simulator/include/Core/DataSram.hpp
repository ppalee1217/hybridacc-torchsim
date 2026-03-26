#pragma once

#include <systemc>

#include <cstdint>
#include <vector>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

template <unsigned DATA_SRAM_BYTES = 65536>
SC_MODULE(DataSram) {
public:
	static constexpr unsigned kWordCount = DATA_SRAM_BYTES / 4u;

	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_in<bool> loader_req_valid_i{"loader_req_valid_i"};
	sc_in<sc_uint<32>> loader_req_addr_i{"loader_req_addr_i"};
	sc_in<sc_uint<32>> loader_req_wdata_i{"loader_req_wdata_i"};
	sc_in<sc_uint<4>> loader_req_wstrb_i{"loader_req_wstrb_i"};

	sc_in<bool> mcu_req_valid_i{"mcu_req_valid_i"};
	sc_in<bool> mcu_req_write_i{"mcu_req_write_i"};
	sc_in<sc_uint<32>> mcu_req_addr_i{"mcu_req_addr_i"};
	sc_in<sc_uint<32>> mcu_req_wdata_i{"mcu_req_wdata_i"};
	sc_in<sc_uint<4>> mcu_req_wstrb_i{"mcu_req_wstrb_i"};
	sc_out<bool> mcu_resp_valid_o{"mcu_resp_valid_o"};
	sc_out<sc_uint<32>> mcu_resp_rdata_o{"mcu_resp_rdata_o"};

	SC_CTOR(DataSram) : words_(kWordCount, 0u) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	void load_word(uint32_t byte_addr, uint32_t value) {
		const std::size_t index = static_cast<std::size_t>((byte_addr - kDataSramBase) >> 2);
		if (index >= words_.size()) {
			words_.resize(index + 1u, 0u);
		}
		words_[index] = value;
	}

	uint32_t read_word(uint32_t byte_addr) const {
		if (byte_addr < kDataSramBase) {
			return 0u;
		}
		const std::size_t index = static_cast<std::size_t>((byte_addr - kDataSramBase) >> 2);
		return index < words_.size() ? words_[index] : 0u;
	}

private:
	std::vector<uint32_t> words_;

	void apply_write(uint32_t byte_addr, uint32_t data, uint32_t strb) {
		if (byte_addr < kDataSramBase) {
			return;
		}
		const std::size_t index = static_cast<std::size_t>((byte_addr - kDataSramBase) >> 2);
		if (index >= words_.size()) {
			words_.resize(index + 1u, 0u);
		}
		uint32_t value = words_[index];
		for (uint32_t byte_idx = 0; byte_idx < 4u; ++byte_idx) {
			if ((strb & (1u << byte_idx)) != 0u) {
				value &= ~(0xFFu << (byte_idx * 8u));
				value |= (data & (0xFFu << (byte_idx * 8u)));
			}
		}
		words_[index] = value;
	}

	void seq_process() {
		mcu_resp_valid_o.write(false);
		mcu_resp_rdata_o.write(0u);
		wait();

		while (true) {
			mcu_resp_valid_o.write(false);
			if (loader_req_valid_i.read()) {
				apply_write(loader_req_addr_i.read().to_uint(), loader_req_wdata_i.read().to_uint(), loader_req_wstrb_i.read().to_uint());
			}
			if (mcu_req_valid_i.read()) {
				if (mcu_req_write_i.read()) {
					apply_write(mcu_req_addr_i.read().to_uint(), mcu_req_wdata_i.read().to_uint(), mcu_req_wstrb_i.read().to_uint());
					mcu_resp_rdata_o.write(0u);
				} else {
					mcu_resp_rdata_o.write(read_word(mcu_req_addr_i.read().to_uint()));
				}
				mcu_resp_valid_o.write(true);
			}
			wait();
		}
	}
};

} // namespace hybridacc::core