#pragma once
/**
 * @file DramBurst.hpp
 * @brief The DMA's DRAM-side burst size S (M22 researcher ruling 2026-10-22; --dram-burst-bytes S).
 *
 * DmaEngine sends AXI4 INCR bursts of at most S bytes on its 64-bit DRAM port: the contiguous beats of one command,
 * cut at S-aligned addresses (so never across 4 KB), at address discontinuities and at load-pad fill beats; a partial
 * block is not padded and a fill beat reads no DRAM. S is a power of two from 8 to 2048 (256 beats, the AXI4 INCR
 * limit at 64 bits), and a burst must fit the engine's transfer buffer (DmaEngine::kDataBufferDepth beats), since a
 * read reserves buffer space for all its beats. S = 8 is the single-beat engine before the ruling. The cluster side
 * stays 8-byte single-beat AXI4-Lite. Set before the run starts.
 */

namespace hacc {
inline unsigned& dram_burst_bytes() {
    static unsigned s = 64;
    return s;
}
}  // namespace hacc
