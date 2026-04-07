#pragma once

/**
 * @file CoreMcu.hpp
 * @brief Convenience wrapper — imports rv32i_mcu::CoreMcu into the
 *        hybridacc::core namespace so CoreController can use it as
 *        simply "CoreMcu".
 */

#include "Core/rv32i_mcu/CoreMcu.hpp"

namespace hybridacc {
namespace core {

using CoreMcu = rv32i_mcu::CoreMcu;

} // namespace core
} // namespace hybridacc
