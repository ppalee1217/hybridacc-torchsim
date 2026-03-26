#pragma once

#include <array>
#include <cstdint>

#include "Core/PipelineTypes.hpp"
#include "Core/Types.hpp"

namespace hybridacc::core {

struct WritebackStage {
	static void commit_gpr(std::array<uint32_t, kCoreGprCount>& gprs, const MemWbLatch& memwb) {
		if (memwb.valid && memwb.reg_write && memwb.rd != 0u) {
			gprs[memwb.rd] = memwb.write_value;
		}
		gprs[0] = 0u;
	}
};

} // namespace hybridacc::core