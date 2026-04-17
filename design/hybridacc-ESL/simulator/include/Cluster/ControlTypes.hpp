#pragma once

#include <cstdint>

namespace hybridacc {
namespace cluster {

enum class UnifiedCtrlBit : uint32_t {
	START = 0u,
	STOP = 1u,
	SOFT_RESET = 2u,
};

enum class UnifiedStatusBit : uint32_t {
	IDLE = 0u,
	BUSY = 1u,
	DONE = 2u,
	QUIESCED = 3u,
	ERROR = 4u,
};

enum class ClusterMode : uint32_t {
	DIRECT_DEBUG = 0u,
	LAYER_MANAGED = 1u,
};

enum class ClusterSubstate : uint32_t {
	IDLE = 0u,
	STARTING = 1u,
	RUNNING = 2u,
	STOPPING = 3u,
	WAIT_QUIESCED = 4u,
	SOFT_RESETTING = 5u,
	ERROR = 6u,
};

enum ClusterRegOffset : uint32_t {
	REG_MODE = 0x00u,
	REG_CTRL = 0x04u,
	REG_STATUS = 0x08u,
	REG_ERROR_CODE = 0x0Cu,
	REG_SUBSTATE = 0x10u,
};

constexpr uint32_t kClusterMmioBase = 0x2100u;
constexpr uint32_t kClusterMmioSize = 0x0100u;

template <typename EnumT>
constexpr uint32_t bit_mask(EnumT bit) {
	return 1u << static_cast<uint32_t>(bit);
}

} // namespace cluster
} // namespace hybridacc