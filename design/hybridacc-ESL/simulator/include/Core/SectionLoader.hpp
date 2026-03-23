#pragma once

#include <systemc>

#include <cstdint>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

SC_MODULE(SectionLoader) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_in<ManifestHeader> manifest_header_i{"manifest_header_i"};
	sc_in<bool> manifest_header_valid_i{"manifest_header_valid_i"};
	sc_out<bool> manifest_header_ready_o{"manifest_header_ready_o"};
	sc_in<ManifestPayloadBeat> manifest_payload_i{"manifest_payload_i"};
	sc_in<bool> manifest_payload_valid_i{"manifest_payload_valid_i"};
	sc_out<bool> manifest_payload_ready_o{"manifest_payload_ready_o"};

	sc_out<bool> isram_wr_valid_o{"isram_wr_valid_o"};
	sc_out<sc_uint<32>> isram_wr_addr_o{"isram_wr_addr_o"};
	sc_out<sc_uint<32>> isram_wr_data_o{"isram_wr_data_o"};
	sc_out<sc_uint<4>> isram_wr_strb_o{"isram_wr_strb_o"};

	sc_out<bool> data_wr_valid_o{"data_wr_valid_o"};
	sc_out<sc_uint<32>> data_wr_addr_o{"data_wr_addr_o"};
	sc_out<sc_uint<32>> data_wr_data_o{"data_wr_data_o"};
	sc_out<sc_uint<4>> data_wr_strb_o{"data_wr_strb_o"};

	sc_out<bool> loader_busy_o{"loader_busy_o"};
	sc_out<bool> loader_done_o{"loader_done_o"};
	sc_out<bool> loader_error_o{"loader_error_o"};

	SC_CTOR(SectionLoader) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

private:
	ManifestHeader active_header_{};
	bool active_ = false;
	uint32_t word_index_ = 0u;

	void seq_process() {
		manifest_header_ready_o.write(false);
		manifest_payload_ready_o.write(false);
		isram_wr_valid_o.write(false);
		data_wr_valid_o.write(false);
		loader_busy_o.write(false);
		loader_done_o.write(false);
		loader_error_o.write(false);
		wait();

		while (true) {
			manifest_header_ready_o.write(false);
			manifest_payload_ready_o.write(false);
			isram_wr_valid_o.write(false);
			data_wr_valid_o.write(false);
			loader_done_o.write(false);
			loader_error_o.write(false);
			loader_busy_o.write(active_);

			if (!active_) {
				manifest_header_ready_o.write(true);
				if (manifest_header_valid_i.read()) {
					active_header_ = manifest_header_i.read();
					active_ = true;
					word_index_ = 0u;
					loader_busy_o.write(true);
					if (active_header_.word_count == 0u) {
						active_ = false;
						loader_done_o.write(true);
						loader_busy_o.write(false);
					}
				}
			} else {
				manifest_payload_ready_o.write(true);
				if (manifest_payload_valid_i.read()) {
					const ManifestPayloadBeat beat = manifest_payload_i.read();
					const uint32_t addr = active_header_.dst_base + (word_index_ * 4u);
					if (active_header_.dst_kind == DestinationKind::ISRAM || active_header_.section_type == SectionType::CORE) {
						isram_wr_valid_o.write(true);
						isram_wr_addr_o.write(addr);
						isram_wr_data_o.write(beat.data);
						isram_wr_strb_o.write(0xFu);
					} else {
						data_wr_valid_o.write(true);
						data_wr_addr_o.write(kDataSramBase + addr);
						data_wr_data_o.write(beat.data);
						data_wr_strb_o.write(0xFu);
					}
					word_index_ += 1u;
					if (beat.last || word_index_ >= active_header_.word_count) {
						active_ = false;
						loader_done_o.write(true);
						loader_busy_o.write(false);
					}
				}
			}

			wait();
		}
	}
};

} // namespace hybridacc::core