#pragma once

#include "Core/PipelineTypes.hpp"

namespace hybridacc::core {

struct FetchStage {
	static IfIdLatch latch(uint32_t pc, uint32_t instruction) {
		return IfIdLatch{true, pc, instruction};
	}
};

} // namespace hybridacc::core