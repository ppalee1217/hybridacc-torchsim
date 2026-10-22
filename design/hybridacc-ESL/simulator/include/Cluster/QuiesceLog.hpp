#pragma once
/**
 * @file QuiesceLog.hpp
 * @brief Optional log of the parts of the cluster's QUIESCED status (M22, the TLS's cluster_quiesced model).
 *
 * Enabled by `--quiesce-log FILE`. CLUSTER_STATUS QUIESCED is !busy && noc_quiesced && spm_quiesced
 * (ClusterControlUnit::status_word); spm_quiesced is the SPM's NoC side (port skid and response FIFOs, group
 * read-metadata FIFOs) and its DMA side (the DMA AW/W/write/read FIFOs, pending B count and in-flight reads)
 * (ScratchpadMemory::spm_quiesced). One line each time a part changes (the first line: the values at the first
 * edge), read where the wave log's C lines are read:
 *
 *   Q cluster= busy=0|1 noc=0|1 spm_noc=0|1 spm_dma=0|1 quiesced=0|1 t=
 *
 * The hook only reads state and writes the file; it changes no simulated state.
 */
#include <cstdint>
#include <cstdio>

namespace hacc {
inline std::FILE*& quiesce_log() {
    static std::FILE* file = nullptr;
    return file;
}
inline unsigned next_quiesce_log_id() {
    static unsigned next = 0;
    return next++;
}

/// Observer state of one cluster. Plain C++; never read by the simulated hardware.
struct QuiesceObserver {
    unsigned id = 0;
    int last[5] = {-1, -1, -1, -1, -1};
    void observe(bool busy, bool noc, bool spm_noc, bool spm_dma, bool quiesced, uint64_t t) {
        std::FILE* f = quiesce_log();
        if (!f) return;
        const int v[5] = {busy, noc, spm_noc, spm_dma, quiesced};
        bool changed = false;
        for (int k = 0; k < 5; ++k) changed = changed || v[k] != last[k];
        if (!changed) return;
        for (int k = 0; k < 5; ++k) last[k] = v[k];
        std::fprintf(f, "Q cluster=%u busy=%d noc=%d spm_noc=%d spm_dma=%d quiesced=%d t=%llu\n", id, v[0], v[1], v[2],
                     v[3], v[4], static_cast<unsigned long long>(t));
    }
};
}  // namespace hacc
