#pragma once

#include <systemc>

#include <cstdint>
#include <deque>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

SC_MODULE(BootHostIf) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_out<ManifestHeader> manifest_header_o{"manifest_header_o"};
	sc_out<bool> manifest_header_valid_o{"manifest_header_valid_o"};
	sc_in<bool> manifest_header_ready_i{"manifest_header_ready_i"};
	sc_out<ManifestPayloadBeat> manifest_payload_o{"manifest_payload_o"};
	sc_out<bool> manifest_payload_valid_o{"manifest_payload_valid_o"};
	sc_in<bool> manifest_payload_ready_i{"manifest_payload_ready_i"};

	sc_in<bool> loader_busy_i{"loader_busy_i"};
	sc_in<bool> loader_done_i{"loader_done_i"};
	sc_in<bool> loader_error_i{"loader_error_i"};
	sc_in<bool> runtime_error_i{"runtime_error_i"};
	sc_out<bool> core_start_o{"core_start_o"};
	sc_out<bool> controller_irq_o{"controller_irq_o"};

	SC_CTOR(BootHostIf) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	void host_push_manifest(const ManifestPacket& packet) {
		manifest_queue_.push_back(packet);
	}

	void host_write_run_config(uint32_t job_base, uint32_t range_begin, uint32_t range_count) {
		job_base_reg_ = job_base;
		range_begin_reg_ = range_begin;
		range_count_reg_ = range_count;
		run_enable_reg_ = true;
	}

	bool queue_empty() const { return manifest_queue_.empty() && !streaming_active_; }
	uint32_t job_base() const { return job_base_reg_; }
	uint32_t range_begin() const { return range_begin_reg_; }
	uint32_t range_count() const { return range_count_reg_; }

private:
	std::deque<ManifestPacket> manifest_queue_;
	ManifestPacket active_packet_{};
	bool streaming_active_ = false;
	std::size_t payload_index_ = 0u;
	uint32_t job_base_reg_ = 0u;
	uint32_t range_begin_reg_ = 0u;
	uint32_t range_count_reg_ = 0u;
	bool run_enable_reg_ = false;

	void seq_process() {
		manifest_header_valid_o.write(false);
		manifest_payload_valid_o.write(false);
		manifest_header_o.write(ManifestHeader{});
		manifest_payload_o.write(ManifestPayloadBeat{});
		core_start_o.write(false);
		controller_irq_o.write(false);
		wait();

		while (true) {
			controller_irq_o.write(loader_done_i.read() || loader_error_i.read() || runtime_error_i.read());
			core_start_o.write(run_enable_reg_ && !loader_busy_i.read() && manifest_queue_.empty() && !streaming_active_);

			if (!streaming_active_ && !manifest_queue_.empty()) {
				active_packet_ = manifest_queue_.front();
				streaming_active_ = true;
				payload_index_ = 0u;
			}

			manifest_header_valid_o.write(false);
			manifest_payload_valid_o.write(false);

			if (streaming_active_) {
				if (payload_index_ == 0u) {
					manifest_header_o.write(active_packet_.header());
					manifest_header_valid_o.write(true);
					if (manifest_header_ready_i.read()) {
						if (active_packet_.payload_words.empty()) {
							manifest_queue_.pop_front();
							streaming_active_ = false;
						} else {
							payload_index_ = 1u;
						}
					}
				} else {
					const std::size_t data_index = payload_index_ - 1u;
					ManifestPayloadBeat beat{};
					beat.data = active_packet_.payload_words[data_index];
					beat.last = (data_index + 1u) == active_packet_.payload_words.size();
					manifest_payload_o.write(beat);
					manifest_payload_valid_o.write(true);
					if (manifest_payload_ready_i.read()) {
						if (beat.last) {
							manifest_queue_.pop_front();
							streaming_active_ = false;
							payload_index_ = 0u;
						} else {
							payload_index_ += 1u;
						}
					}
				}
			}

			wait();
		}
	}
};

} // namespace hybridacc::core