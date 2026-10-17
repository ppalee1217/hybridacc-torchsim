#pragma once
/**
 * @file DramLog.hpp
 * @brief Optional log of the DRAM beats each DMA command reads or writes (M20 B2).
 *
 * Enabled by `--dram-log FILE`. DmaEngine writes "C cmd_tag src_kind dst_kind beats time_ps"
 * when a command starts running, then "R|W addr time_ps" for every DRAM request it issues for
 * that command. Every DMA DRAM request is one 8-byte beat (AXI len 0), so a line is a beat.
 * Out-of-window load-pad beats are synthesised and read no DRAM, so they get no line. The hook
 * reads values and writes the file only; it changes no simulated state. Used to check the
 * DRAM-side expansion of each DMA command in the cc schedule IR.
 */
#include <cstdio>

namespace hacc {
inline std::FILE*& dram_log() {
    static std::FILE* file = nullptr;
    return file;
}
}  // namespace hacc
