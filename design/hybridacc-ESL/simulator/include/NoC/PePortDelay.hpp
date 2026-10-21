#pragma once
/**
 * @file PePortDelay.hpp
 * @brief Optional handshake perturbation at the PE ports (M21 WP-J J3, separability test).
 *
 * Ruling: agent_run/261021-m21-decomposition/decide-separability/decision.md. Enabled by `--pe-perturb SPEC`;
 * without it NetworkOnChip binds the PEs to the buses directly and nothing here is instantiated.
 *
 * A PortDelay sits on one valid/ready channel between the MBUS and a PE. It adds no buffering and never reorders
 * or duplicates: the payload passes through, and the adapter only withholds valid downstream and ready upstream
 * (backpressure). It cannot delay a token from the moment it is offered: the MBUS raises a PE's valid only when
 * every target of the multicast is ready (MBUS::comb_pd_routing), so a token is invisible to the adapter until
 * the adapter itself is ready (a first version that waited to see the token deadlocked).
 *   - gap: a selected data token cannot complete until L more cycles after the previous handshake on this port
 *     than it otherwise could; an unselected token passes in the same cycle (with every gap 0 and no gate, the
 *     channel behaves as a plain wire);
 *   - gate: the channel is closed for the first W cycles of every 32-cycle period of simulated time.
 * PE commands (PS with PE_CMD_ADDRESS) are never delayed and do not count as data tokens.
 *
 * SPEC is a comma list of key=value (all default 0):
 *   ps_every, ps_delay / pd_every, pd_delay / pli_every, pli_delay
 *       every `every`-th data token on that input port (index % every == every - 1) gets a gap of `delay` cycles
 *   stagger      on a PE with an odd linear index (bus * rows + row), every 8th data token on each input port gets
 *                a gap of this many cycles (skew between the targets of a multicast)
 *   seed         nonzero: each data token on ps/pd/pli gets, independently, with probability 1/8, a gap of 1, 4 or
 *                16 cycles (splitmix64 of seed, PE, port and token index)
 *   plo_req_closed, plo_resp_closed
 *                cycles closed per 32-cycle period on the PLO request (bus -> PE) and response (PE -> bus) channels.
 *                Do not use plo_resp_closed: the MBUS collects a PLO response only in the cycle after its request
 *                (MBUS::comb_plo_routing registers rx_mask for one cycle), so a delayed response is never taken and
 *                the full ESL hangs too; the response channel has no backpressure to perturb.
 * Gaps on one token add up.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <systemc>

#include "Utils/utils.hpp"

namespace hacc {

struct PePerturb {
    bool enabled = false;
    unsigned every[3] = {0, 0, 0};   // ps, pd, pli
    unsigned delay[3] = {0, 0, 0};
    unsigned stagger = 0;
    uint64_t seed = 0;
    unsigned plo_req_closed = 0;
    unsigned plo_resp_closed = 0;
    std::string spec;
};
inline PePerturb& pe_perturb() {
    static PePerturb p;
    return p;
}
/// Parse SPEC into pe_perturb(); false on an unknown key or a bad value.
inline bool parse_pe_perturb(const std::string& spec) {
    PePerturb& p = pe_perturb();
    p = PePerturb();
    p.enabled = true;
    p.spec = spec;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        pos = end + 1;
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = item.substr(0, eq);
        char* tail = nullptr;
        const unsigned long long v = std::strtoull(item.c_str() + eq + 1, &tail, 10);
        if (!tail || *tail != '\0') return false;
        static const char* const ports[3] = {"ps", "pd", "pli"};
        bool known = false;
        for (int i = 0; i < 3; ++i) {
            if (k == std::string(ports[i]) + "_every") { p.every[i] = static_cast<unsigned>(v); known = true; }
            if (k == std::string(ports[i]) + "_delay") { p.delay[i] = static_cast<unsigned>(v); known = true; }
        }
        if (k == "stagger") { p.stagger = static_cast<unsigned>(v); known = true; }
        if (k == "seed") { p.seed = v; known = true; }
        if (k == "plo_req_closed") { p.plo_req_closed = static_cast<unsigned>(v); known = true; }
        if (k == "plo_resp_closed") { p.plo_resp_closed = static_cast<unsigned>(v); known = true; }
        if (!known) return false;
    }
    return true;
}
inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
/// Cycles to hold data token `n` on input port `port` (0 ps, 1 pd, 2 pli) of PE `pe` (linear index).
inline unsigned pe_input_delay(int pe, int port, uint64_t n) {
    const PePerturb& p = pe_perturb();
    unsigned d = 0;
    if (p.every[port] && n % p.every[port] == p.every[port] - 1) d += p.delay[port];
    if (p.stagger && (pe & 1) && n % 8 == 7) d += p.stagger;
    if (p.seed) {
        const uint64_t h = splitmix64(p.seed ^ (static_cast<uint64_t>(pe) << 40) ^ (static_cast<uint64_t>(port) << 32) ^ n);
        static const unsigned choices[3] = {1, 4, 16};
        if (h % 8 == 0) d += choices[(h >> 8) % 3];
    }
    return d;
}

/// One perturbed valid/ready channel (see the file comment). `is_cmd` tells PE commands apart (never delayed).
template <typename T>
SC_MODULE(PortDelay) {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> reset_n;
    sc_core::sc_in<T> up_data;
    sc_core::sc_in<bool> up_valid;
    sc_core::sc_out<bool> up_ready;
    sc_core::sc_out<T> dn_data;
    sc_core::sc_out<bool> dn_valid;
    sc_core::sc_in<bool> dn_ready;

    std::function<unsigned(uint64_t)> delay_of;  // data token index -> cycles; empty = none
    std::function<bool(const T&)> is_cmd;        // empty = no commands on this channel
    unsigned closed_cycles = 0;                  // gate: cycles closed per 32-cycle period
    static constexpr unsigned PERIOD = 32;

    sc_core::sc_signal<bool> release_sig;  // the current data token may pass, for this cycle
    sc_core::sc_signal<bool> gate_sig;     // the gate is open, for this cycle

    SC_HAS_PROCESS(PortDelay);
    explicit PortDelay(sc_core::sc_module_name name)
        : sc_module(name), clk("clk"), reset_n("reset_n"), up_data("up_data"), up_valid("up_valid"),
          up_ready("up_ready"), dn_data("dn_data"), dn_valid("dn_valid"), dn_ready("dn_ready"),
          release_sig("release_sig"), gate_sig("gate_sig") {
        SC_METHOD(comb);
        sensitive << up_data << up_valid << dn_ready << release_sig << gate_sig;
        SC_METHOD(seq);
        sensitive << clk.pos();
    }

    uint64_t data_index = 0;  // data tokens handed downstream so far
    uint64_t since = 0;       // edges since the previous handshake on this channel (or reset)

    bool cmd(const T& v) const { return is_cmd && is_cmd(v); }

    void comb() {
        const bool open = gate_sig.read() && (release_sig.read() || cmd(up_data.read()));
        dn_data.write(up_data.read());
        dn_valid.write(up_valid.read() && open);
        up_ready.write(dn_ready.read() && open);
    }

    void seq() {
        if (!reset_n.read()) {
            data_index = 0; since = 0;
            release_sig.write(!delay_of || delay_of(0) == 0);
            gate_sig.write(true);
            return;
        }
        const uint64_t cycle = sc_core::sc_time_stamp().value() / 2000;  // 500 MHz, as the ESL main
        // Values as this edge's processes see them.
        const bool valid = up_valid.read();
        const bool is_command = valid && cmd(up_data.read());
        const bool fired = dn_valid.read() && dn_ready.read();
        if (fired) {
            if (!is_command) ++data_index;
            since = 0;
        } else {
            ++since;
        }
        // The next data token may complete once `need` edges have passed since the previous handshake.
        const unsigned need = delay_of ? delay_of(data_index) : 0;
        release_sig.write(need == 0 || since >= need);
        gate_sig.write(closed_cycles == 0 || ((cycle + 1) % PERIOD) >= closed_cycles);
    }
};

}  // namespace hacc
