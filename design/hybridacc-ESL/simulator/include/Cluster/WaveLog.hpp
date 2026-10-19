#pragma once
/**
 * @file WaveLog.hpp
 * @brief Optional per-wave HDDU event log (M21, F2 measurement contract).
 *
 * Enabled by `--wave-log FILE`. Each HDDU appends key=value lines; times are
 * sc_time_stamp() in ps at the rising edge its seq_process runs on, and every
 * signal is read as that edge's seq_process sees it (the values the hardware
 * acts on at that edge).
 *
 *   T hddu= epoch= cause=reset|soft_reset t=          a new reset epoch begins
 *   S hddu= epoch= seq= g= plane_en= plane_mode= t=   HDDU accepted a START
 *   R hddu= epoch= stop= soft_reset= error= no_plane= t=
 *                                                     a START write that is out of
 *                                                     contract (2026-10-18 F2 ruling):
 *                                                     not accepted, opens no wave
 *   E hddu= epoch= seq= g= start= agu_idle= drained= answered= done= stop= error= close=
 *     ps= pd= pli= plo_req= plo_resp= plo_spm= plo_ack= last_ps= last_pd= last_pli=
 *     last_plo_spm= overlap= t=                       one wave, written when it closes
 *   C hddu= cond=pe_halted|cluster_quiesced value=0|1 t=
 *                                                     the cluster condition a firmware wait
 *                                                     polls changed (first line: its value at
 *                                                     the first edge): NOC_STATUS all active PEs
 *                                                     halted, CLUSTER_STATUS QUIESCED
 *   M addr= data= t=                                  an MMIO read the MCU completed and the value
 *                                                     it returned (finds a wait's successful poll)
 *
 * In E: agu_idle is the first edge after the wave was busy with every AGU idle
 * (the condition DONE used before the D-36 fix); drained adds "no queue inside
 * HDDU holds data"; answered adds "every PLO write the SPM accepted has been
 * answered", which is after the SPM wrote it to SRAM; done is the first edge
 * DONE was set; stop is the STOP command; error is the first edge ERR_CODE was
 * non-zero while the wave was open. ps/pd/pli count HDDU->NoC send
 * handshakes, plo_req the PLO requests sent to the NoC, plo_resp the PLO data
 * the HDDU took from the NoC, plo_spm the PLO writes the SPM accepted and
 * plo_ack their write responses; last_* is the edge of the last such handshake.
 * A wave closes at the next accepted START, a reset or the end of simulation
 * (close=start|soft_reset|reset|end); overlap=1 when the next START found data
 * of this wave still inside HDDU. '-' marks an event that did not happen.
 * Which SPM row each PLO write reached, and when, is in --spm-log (port 3).
 *
 * The hooks only read signals and write the file; they change no simulated state.
 */
#include <cstdint>
#include <cstdio>

namespace hacc {
inline std::FILE*& wave_log() {
    static std::FILE* file = nullptr;
    return file;
}
inline unsigned next_wave_log_id() {
    static unsigned next = 0;
    return next++;
}

/// Observer state of one HDDU. Plain C++; never read by the simulated hardware.
struct WaveObserver {
    unsigned id = 0;
    uint64_t epoch = 0;
    uint64_t seq = 0;       // accepted STARTs in this epoch
    uint64_t global = 0;    // accepted STARTs over the whole run
    bool open = false;
    bool seen_busy = false;
    uint64_t start = 0;
    static constexpr uint64_t NONE = ~0ull;
    uint64_t agu_idle = NONE, drained = NONE, answered = NONE, done = NONE, stop = NONE, error = NONE;
    bool started_epochs = false;
    bool in_reset = false;  // reset_n is held over several edges; they are one epoch
    uint64_t n[7] = {0, 0, 0, 0, 0, 0, 0};          // ps pd pli plo_req plo_resp plo_spm plo_ack
    uint64_t last[4] = {NONE, NONE, NONE, NONE};    // ps pd pli plo_spm
    int64_t plo_outstanding = 0;                     // plo_spm - plo_ack, for "answered"
    int cond_value[2] = {-1, -1};                     // pe_halted, cluster_quiesced; -1 = not seen yet

    static void put(std::FILE* f, const char* key, uint64_t v) {
        if (v == NONE) std::fprintf(f, " %s=-", key);
        else std::fprintf(f, " %s=%llu", key, static_cast<unsigned long long>(v));
    }
    void close(const char* cause, bool overlap, uint64_t t) {
        std::FILE* f = wave_log();
        if (!f || !open) { open = false; return; }
        std::fprintf(f, "E hddu=%u epoch=%llu seq=%llu g=%llu", id,
                     static_cast<unsigned long long>(epoch), static_cast<unsigned long long>(seq),
                     static_cast<unsigned long long>(global));
        put(f, "start", start); put(f, "agu_idle", agu_idle); put(f, "drained", drained);
        put(f, "answered", answered); put(f, "done", done); put(f, "stop", stop); put(f, "error", error);
        std::fprintf(f, " close=%s", cause);
        static const char* const names[7] = {"ps", "pd", "pli", "plo_req", "plo_resp", "plo_spm", "plo_ack"};
        for (int i = 0; i < 7; ++i) put(f, names[i], n[i]);
        static const char* const lasts[4] = {"last_ps", "last_pd", "last_pli", "last_plo_spm"};
        for (int i = 0; i < 4; ++i) put(f, lasts[i], last[i]);
        std::fprintf(f, " overlap=%d t=%llu\n", overlap ? 1 : 0, static_cast<unsigned long long>(t));
        open = false;
    }
    void open_wave(uint64_t t) {
        ++seq; ++global;
        open = true; seen_busy = false; start = t;
        agu_idle = drained = answered = done = stop = error = NONE;
        for (auto& x : n) x = 0;
        for (auto& x : last) x = NONE;
        plo_outstanding = 0;
    }
    /// Called by the cluster once per edge with the two conditions firmware waits poll.
    void cluster_conditions(bool pe_halted, bool cluster_quiesced, uint64_t t) {
        std::FILE* f = wave_log();
        if (!f) return;
        static const char* const names[2] = {"pe_halted", "cluster_quiesced"};
        const int v[2] = {pe_halted ? 1 : 0, cluster_quiesced ? 1 : 0};
        for (int i = 0; i < 2; ++i) {
            if (v[i] == cond_value[i]) continue;
            cond_value[i] = v[i];
            std::fprintf(f, "C hddu=%u cond=%s value=%d t=%llu\n", id, names[i], v[i],
                         static_cast<unsigned long long>(t));
        }
    }
    void new_epoch(const char* cause, uint64_t t) {
        close(cause, false, t);
        if (started_epochs) ++epoch;
        started_epochs = true;
        seq = 0;
        if (std::FILE* f = wave_log())
            std::fprintf(f, "T hddu=%u epoch=%llu cause=%s t=%llu\n", id,
                         static_cast<unsigned long long>(epoch), cause, static_cast<unsigned long long>(t));
    }
};

/// Every live observer, so main() can close open waves when the simulation ends.
inline WaveObserver** wave_observers() {
    static WaveObserver* list[64] = {nullptr};
    return list;
}
inline void register_wave_observer(WaveObserver* o) {
    WaveObserver** list = wave_observers();
    for (int i = 0; i < 64; ++i) if (!list[i]) { list[i] = o; return; }
}
inline void close_open_waves(uint64_t t) {
    WaveObserver** list = wave_observers();
    for (int i = 0; i < 64 && list[i]; ++i) list[i]->close("end", false, t);
}
}  // namespace hacc
