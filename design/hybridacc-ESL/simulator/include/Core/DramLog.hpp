#pragma once
/**
 * @file DramLog.hpp
 * @brief Optional log of the DRAM beats each DMA command reads or writes (M20 B2).
 *
 * Enabled by `--dram-log FILE`. DmaEngine writes "C cmd_tag src_kind dst_kind beats time_ps"
 * when a command starts running, then "R|W addr time_ps" for every DRAM request it issues for
 * that command, and "D cmd_tag error time_ps" at the edge the command completes (DONE_TAG takes
 * its tag and STATUS reads idle; error is 1 for a command that ended in the error state; M21, for
 * the C-05 issue timing). Every DMA DRAM request is one 8-byte beat (AXI len 0), so each R|W line is one beat.
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
