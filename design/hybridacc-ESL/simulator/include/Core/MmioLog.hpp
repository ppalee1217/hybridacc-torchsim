#pragma once
/**
 * @file MmioLog.hpp
 * @brief Optional log of every MCU MMIO access, one line per completed access (M20).
 *
 * Enabled by `--mmio-log FILE`. CoreMcu writes "W|R addr data wstrb time_ps" once per
 * MMIO request, at the first rising edge the request is seen (reads log data 0: the
 * response is not known yet). The hook reads signal values and writes the file only;
 * it changes no simulated state. Used to check that a host run of the same firmware
 * (cc schedule extraction) issues the same writes in the same order.
 */
#include <cstdio>

namespace hacc {
inline std::FILE*& mmio_log() {
    static std::FILE* file = nullptr;
    return file;
}
}  // namespace hacc
