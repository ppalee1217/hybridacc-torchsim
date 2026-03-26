#pragma once

#include <cstdint>

#include "Core/PipelineTypes.hpp"

namespace hybridacc::core {

struct MemoryStage {
	static uint8_t write_strobe(MemOp op, uint32_t addr) {
		switch (op) {
		case MemOp::SB: return static_cast<uint8_t>(1u << (addr & 0x3u));
		case MemOp::SH: return static_cast<uint8_t>(0x3u << (addr & 0x2u));
		case MemOp::SW: return 0xFu;
		default: return 0u;
		}
	}

	static uint32_t store_data(MemOp op, uint32_t value, uint32_t addr) {
		switch (op) {
		case MemOp::SB: return (value & 0xFFu) << ((addr & 0x3u) * 8u);
		case MemOp::SH: return (value & 0xFFFFu) << ((addr & 0x2u) * 8u);
		case MemOp::SW: return value;
		default: return 0u;
		}
	}

	static uint32_t load_data(MemOp op, uint32_t raw, uint32_t addr) {
		const uint32_t shift = (addr & 0x3u) * 8u;
		switch (op) {
		case MemOp::LB: return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>((raw >> shift) & 0xFFu)));
		case MemOp::LBU: return (raw >> shift) & 0xFFu;
		case MemOp::LH: {
			const uint32_t half_shift = (addr & 0x2u) * 8u;
			return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((raw >> half_shift) & 0xFFFFu)));
		}
		case MemOp::LHU: {
			const uint32_t half_shift = (addr & 0x2u) * 8u;
			return (raw >> half_shift) & 0xFFFFu;
		}
		case MemOp::LW: return raw;
		default: return raw;
		}
	}
};

} // namespace hybridacc::core