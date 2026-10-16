#pragma once
/**
 * @file MmioLog.hpp
 * @brief Optional log of every MCU MMIO access, one line per completed access (M20).
 *
 * Enabled by `--mmio-log FILE`. CoreMcu writes "W|R addr data wstrb time_ps" when an
 * MMIO request completes (request active and response valid at a rising edge). It only
 * writes to the file and reads signal values; it does not change simulated state, so
 * cycle counts are the same with and without it. Used to check that a host run of the
 * same firmware (cc schedule extraction) issues the same writes in the same order.
 */
#include <cstdio>

namespace hacc {
inline std::FILE*& mmio_log() {
    static std::FILE* file = nullptr;
    return file;
}
}  // namespace hacc
