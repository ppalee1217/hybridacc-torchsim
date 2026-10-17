#pragma once
/**
 * @file SpmLog.hpp
 * @brief Optional log of every request the SPM accepts (M20 SPM interaction).
 *
 * Enabled by `--spm-log FILE`. In the cycle ScratchpadMemory accepts a request it writes
 *   "P spm port group wen laddr time_ps"   for a NoC port (group-local word address), or
 *   "D spm wen gaddr strb time_ps"          for the DMA (global byte address, byte strobe).
 * `spm` numbers the SPM instances in construction order. Bank and row follow from the SPM's
 * address rules (doc/component/SPM.md §3.1). The hook reads values and writes the file only;
 * it changes no simulated state. Used to check, against the cc schedule IR, that the
 * firmware's waits order every pair of conflicting SPM accesses.
 */
#include <cstdio>

namespace hacc {
inline std::FILE*& spm_log() {
    static std::FILE* file = nullptr;
    return file;
}
inline unsigned next_spm_log_id() {
    static unsigned next = 0;
    return next++;
}
}  // namespace hacc
