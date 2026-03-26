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
	sc_out<bool> core_enable_o{"core_enable_o"};
	sc_out<sc_uint<32>> core_boot_addr_o{"core_boot_addr_o"};
	sc_out<sc_uint<32>> core_trap_vector_o{"core_trap_vector_o"};
	sc_out<bool> controller_irq_o{"controller_irq_o"};

	SC_CTOR(BootHostIf) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	void host_push_manifest(const ManifestPacket& packet) {
		manifest_queue_.push_back(packet);
	}

	void set_core_boot_addr(uint32_t addr) { core_boot_addr_reg_ = addr; }
	void set_core_trap_vector(uint32_t addr) { core_trap_vector_reg_ = addr; }
	void set_core_enable(bool enable) { core_enable_reg_ = enable; }

	uint32_t core_boot_addr() const { return core_boot_addr_reg_; }
	uint32_t core_trap_vector() const { return core_trap_vector_reg_; }
	bool core_enabled() const { return core_enable_reg_; }

private:
	std::deque<ManifestPacket> manifest_queue_{};
	ManifestPacket active_packet_{};
	bool streaming_active_ = false;
	std::size_t payload_index_ = 0u;
	uint32_t core_boot_addr_reg_ = kIsramBase;
	uint32_t core_trap_vector_reg_ = kIsramBase;
	bool core_enable_reg_ = true;

	void seq_process() {
		manifest_header_o.write(ManifestHeader{});
		manifest_header_valid_o.write(false);
		manifest_payload_o.write(ManifestPayloadBeat{});
		manifest_payload_valid_o.write(false);
		core_enable_o.write(true);
		core_boot_addr_o.write(core_boot_addr_reg_);
		core_trap_vector_o.write(core_trap_vector_reg_);
		controller_irq_o.write(false);
		wait();

		while (true) {
			manifest_header_valid_o.write(false);
			manifest_payload_valid_o.write(false);
			core_enable_o.write(core_enable_reg_);
			core_boot_addr_o.write(core_boot_addr_reg_);
			core_trap_vector_o.write(core_trap_vector_reg_);
			controller_irq_o.write(loader_done_i.read() || loader_error_i.read() || runtime_error_i.read());

			if (!streaming_active_ && !manifest_queue_.empty()) {
				active_packet_ = manifest_queue_.front();
				streaming_active_ = true;
				payload_index_ = 0u;
			}

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
					manifest_payload_o.write(ManifestPayloadBeat{active_packet_.payload_words[data_index], data_index + 1u == active_packet_.payload_words.size()});
					manifest_payload_valid_o.write(true);
					if (manifest_payload_ready_i.read()) {
						if (data_index + 1u == active_packet_.payload_words.size()) {
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