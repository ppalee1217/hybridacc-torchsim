/**
 * @file test_pe_replay.cpp
 * @brief Replay a full-ESL run's PE inputs into the ESL's own ProcessElements (M21 WP-J J2).
 *
 * Ruling: agent_run/261021-m21-decomposition/decide-counterfactual/decision.md. The harness instantiates only
 * pe::ProcessElement (router + IF_ID/EXE_M/EXE_A, same FIFO depth and clock) and the local-network chaining of
 * NetworkOnChip (PE[b][r].ln_plo -> PE[b+1][r].ln_pli); NoC, MBUS, HDDU and SPM are replaced by replay sources
 * driven from the full run's --pe-log (H lines: every accepted handshake with payload) and --wave-log.
 *
 *   test_pe_replay PE_LOG WAVE_LOG NUM_BUS PES_PER_BUS PE_FIFO_DEPTH MODE OUT_PE_LOG
 *                  [MAX_CYCLES [SETTLE [IN_FIFO_DEPTH [PERTURB]]]]
 *
 * PE_FIFO_DEPTH must be the full ESL's: HybridAcc builds each cluster's NoC with NetWorkOnChipConfig{4, 32}
 * (simulator/include/HybridAcc.hpp), i.e. 32-entry PE router FIFOs, not ProcessElement's default of 4.
 * IN_FIFO_DEPTH (M21 WP-J J4, default 0 = PE_FIFO_DEPTH) sets only the PS, PD and PLI FIFOs, as the ESL's
 * --pe-in-fifo-depth.
 * PERTURB (M21 WP-J J4, ideal mode only; '-' = none) is an ESL --pe-perturb SPEC applied to the ideal offers with
 * the ESL adapter's rules (NoC/PePortDelay.hpp, same PE index bus * rows + row): a selected data token cannot
 * complete until its gap has passed since the previous handshake on that port, and the PLO request port is closed
 * for the first plo_req_closed cycles of every 32. It measures the PEs' own response to input timing, without the
 * NoC.
 *
 * MODE
 *   timed  every recorded input is offered so that it can complete on its recorded edge (valid from the edge
 *          before), PLO responses are accepted only on their recorded edges, router configuration and the
 *          spatial-tail override are applied at their recorded times. A faithful harness reproduces the PEs'
 *          H and Q lines exactly (the equivalence check).
 *   ideal  inputs are offered as soon as they are causally eligible and PLO responses are always accepted:
 *          - phases: phase k is wave k (its recorded window START..W6) plus the gap before it; phase k is
 *            released when every input and PLO response of phase k-1 has completed, and its router
 *            configuration and spatial-tail override are applied then;
 *          - PE commands (ps with the command address) are barriers on their PE: a command waits for every
 *            earlier input of that PE, and later inputs wait SETTLE cycles after the command's handshake. A
 *            command takes effect in the router after its handshake (START flushes every FIFO); with SETTLE 1
 *            the first wave's data were flushed and the replay never completed on a GEMM cell, with 2..18 it
 *            completed with identical outputs (agent_run/261021-m21-decomposition, dev). Default 2. The
 *            recordings never deliver data sooner than 18 cycles after a START.
 *          Local-network partial sums still come from the upstream PE of this run; nothing recorded is fed
 *          back as a PE output.
 *
 * OUT_PE_LOG is the replayed PEs' own --pe-log (same format), plus the W lines the harness applied and
 *   P k=<phase> release=<ps> data_start=<ps> end=<ps>
 *       phase k released at `release`; its first data input (not a command) offered at `data_start` (the edge
 *       after which it can complete); its last input or PLO response at `end`; written when the phase completes.
 *       The PE-side time of the wave is end - data_start (phase 1's release also covers program loading). Exit 0 when every recorded input and PLO response completed.
 */
#include <systemc>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ProcessElement.hpp"
#include "PE/PeLog.hpp"
#include "NoC/PePortDelay.hpp"
#include "Utils/utils.hpp"

using namespace sc_core;
using namespace hybridacc;
using namespace hybridacc::pe;

namespace {

enum Port { PS = 0, PD = 1, PLI = 2, PLOREQ = 3, NPORT = 4 };
const char* const PORT_NAME[NPORT] = {"ps", "pd", "pli", "plo_req"};

struct Tok {
    uint64_t t = 0, d = 0, m = 0;
    uint16_t a = 0;
    int phase = 0;
    int seg = 0;       // commands on this PE before it
    bool cmd = false;  // ps with PE_CMD_ADDRESS
};
struct Cfg { uint64_t t; int en, mode, phase; };
struct Spatial { uint64_t t; bool configure; unsigned h, w, rows, loop_pc, loop_end_pc; int phase; };

std::map<std::string, std::string> kv(const std::string& line) {
    std::map<std::string, std::string> out;
    std::istringstream in(line.substr(2));
    std::string f;
    while (in >> f) {
        const auto eq = f.find('=');
        if (eq != std::string::npos) out[f.substr(0, eq)] = f.substr(eq + 1);
    }
    return out;
}
uint64_t num(const std::string& s, int base = 10) { return std::strtoull(s.c_str(), nullptr, base); }

}  // namespace

SC_MODULE(ReplayTB) {
    static constexpr uint64_t C = 2000;  // ps per 500 MHz cycle, as the ESL main
    sc_clock clk;
    sc_signal<bool> reset_n;
    int nb, nr;
    bool ideal;
    std::vector<std::unique_ptr<ProcessElement>> pes;
    std::vector<std::unique_ptr<sc_signal<bool>>> en, busy, halted;
    std::vector<std::unique_ptr<sc_signal<PERouterMode>>> mode;
    std::vector<std::unique_ptr<VRDSIG<noc_request_t>>> ps, pd, pli;
    std::vector<std::unique_ptr<VRDSIG<noc_addr_req_t>>> ploreq;
    std::vector<std::unique_ptr<VRDSIG<noc_response_t>>> resp;
    std::vector<std::unique_ptr<VRDSIG<uint64_t>>> ln;  // (nb + 1) x nr, as NetworkOnChip

    // Recorded run.
    std::vector<std::pair<uint64_t, uint64_t>> windows;  // (START, W6)
    std::vector<std::vector<Tok>> in;                     // [pe * NPORT + port]
    std::vector<std::vector<uint64_t>> resp_t;            // [pe] recorded PLO response edges
    std::vector<std::vector<int>> resp_phase;
    std::vector<std::vector<Cfg>> cfg;                    // [pe]
    std::vector<Spatial> spatial;
    // Replay state.
    std::vector<size_t> idx, resp_idx, cfg_idx, cmds_done;
    std::vector<uint64_t> cmd_done_t;   // [pe] edge of the last command handshake
    uint64_t settle = 0;                // ideal: cycles after a command before the next segment's inputs
    std::vector<std::vector<size_t>> data_total_upto, data_done_upto;  // [pe][seg] cumulative data tokens
    size_t spatial_idx = 0;
    bool perturb = false;                               // ideal: PERTURB given
    std::vector<uint64_t> data_n, last_fire;            // [pe * 3 + port] data tokens done, last handshake (ps)
    int released = 1;
    std::vector<uint64_t> release_t, phase_end, data_start;
    int n_phase = 0;
    bool finished = false;
    uint64_t max_t;

    int pe_of(int b, int r) const { return b * nr + r; }
    int phase_of(uint64_t t) const {
        for (size_t k = 0; k < windows.size(); ++k)
            if (t <= windows[k].second) return static_cast<int>(k) + 1;
        return static_cast<int>(windows.size()) + 1;
    }

    SC_HAS_PROCESS(ReplayTB);
    ReplayTB(sc_module_name name, int nb_, int nr_, size_t fifo_depth, bool ideal_, uint64_t max_cycles,
             uint64_t settle_cycles, size_t in_fifo_depth = 0)
        : sc_module(name), clk("clk", 2, SC_NS), reset_n("reset_n"), nb(nb_), nr(nr_), ideal(ideal_),
          max_t(max_cycles * C) {
        settle = settle_cycles;
        const int n = nb * nr;
        for (int i = 0; i <= nb; ++i)
            for (int r = 0; r < nr; ++r)
                ln.emplace_back(new VRDSIG<uint64_t>(sc_gen_unique_name("ln")));
        for (int b = 0; b < nb; ++b) {
            for (int r = 0; r < nr; ++r) {
                pes.emplace_back(new ProcessElement(sc_gen_unique_name("pe"), fifo_depth, in_fifo_depth));
                en.emplace_back(new sc_signal<bool>(sc_gen_unique_name("en")));
                mode.emplace_back(new sc_signal<PERouterMode>(sc_gen_unique_name("mode")));
                busy.emplace_back(new sc_signal<bool>(sc_gen_unique_name("busy")));
                halted.emplace_back(new sc_signal<bool>(sc_gen_unique_name("halted")));
                ps.emplace_back(new VRDSIG<noc_request_t>(sc_gen_unique_name("ps")));
                pd.emplace_back(new VRDSIG<noc_request_t>(sc_gen_unique_name("pd")));
                pli.emplace_back(new VRDSIG<noc_request_t>(sc_gen_unique_name("pli")));
                ploreq.emplace_back(new VRDSIG<noc_addr_req_t>(sc_gen_unique_name("ploreq")));
                resp.emplace_back(new VRDSIG<noc_response_t>(sc_gen_unique_name("resp")));
                ProcessElement& p = *pes.back();
                const int i = pe_of(b, r);
                p.clk(clk);
                p.reset_n(reset_n);
                p.router_enable(*en[i]);
                p.router_mode(*mode[i]);
                p.pe_busy(*busy[i]);
                p.pe_halted(*halted[i]);
                connect_vr_signals(p.noc_ps_in, *ps[i]);
                connect_vr_signals(p.noc_pd_in, *pd[i]);
                connect_vr_signals(p.noc_pli_in, *pli[i]);
                connect_vr_signals(p.noc_plo_in, *ploreq[i]);
                connect_vr_signals(p.noc_plo_out, *resp[i]);
                connect_vr_signals(p.ln_pli, *ln[b * nr + r]);
                connect_vr_signals(p.ln_plo, *ln[(b + 1) * nr + r]);
                p.set_log_position(b, r);
            }
        }
        in.assign(n * NPORT, {});
        resp_t.assign(n, {});
        resp_phase.assign(n, {});
        cfg.assign(n, {});
        idx.assign(n * NPORT, 0);
        resp_idx.assign(n, 0);
        cfg_idx.assign(n, 0);
        cmds_done.assign(n, 0);
        cmd_done_t.assign(n, 0);
        SC_METHOD(drive);
        sensitive << clk.posedge_event();
        dont_initialize();
    }

    bool load(const std::string& pe_log, const std::string& wave_log) {
        std::ifstream w(wave_log);
        std::string line;
        while (std::getline(w, line)) {
            if (line.rfind("E ", 0) != 0) continue;
            auto r = kv(line);
            if (r["answered"] == "-") { std::fprintf(stderr, "wave without W6\n"); return false; }
            windows.emplace_back(num(r["start"]), num(r["answered"]));
        }
        n_phase = static_cast<int>(windows.size()) + 1;
        release_t.assign(n_phase + 2, 0);
        phase_end.assign(n_phase + 2, 0);
        data_start.assign(n_phase + 2, 0);
        std::ifstream f(pe_log);
        while (std::getline(f, line)) {
            if (line.size() < 2) continue;
            const char kind = line[0];
            auto r = kv(line);
            if (kind == 'W') {
                Spatial s{num(r["t"]), r["cmd"] == "configure", 0, 0, 0, 0, 0, 0};
                if (s.configure) {
                    s.h = num(r["h"]); s.w = num(r["w"]); s.rows = num(r["rows"]);
                    s.loop_pc = num(r["loop_pc"]); s.loop_end_pc = num(r["loop_end_pc"]);
                }
                s.phase = phase_of(s.t);
                spatial.push_back(s);
                continue;
            }
            if (kind != 'H' && kind != 'Q') continue;
            const int b = std::atoi(r["b"].c_str()), rr = std::atoi(r["r"].c_str());
            if (b >= nb || rr >= nr) { std::fprintf(stderr, "PE %d.%d outside %dx%d\n", b, rr, nb, nr); return false; }
            const int pe = pe_of(b, rr);
            const uint64_t t = num(r["t"]);
            if (kind == 'Q') {
                const int e = std::atoi(r["en"].c_str()), m = std::atoi(r["mode"].c_str());
                if (cfg[pe].empty() || cfg[pe].back().en != e || cfg[pe].back().mode != m)
                    cfg[pe].push_back({t, e, m, phase_of(t)});
                continue;
            }
            const std::string& p = r["p"];
            if (p == "plo_resp") { resp_t[pe].push_back(t); resp_phase[pe].push_back(phase_of(t)); continue; }
            int port = -1;
            for (int q = 0; q < NPORT; ++q) if (p == PORT_NAME[q]) port = q;
            if (port < 0) continue;  // ln_in / ln_out: produced by the PEs themselves
            Tok k;
            k.t = t;
            k.d = r["d"] == "-" ? 0 : num(r["d"], 16);
            k.a = static_cast<uint16_t>(num(r["a"], 16));
            k.m = r["m"] == "-" ? 0 : num(r["m"], 16);
            k.phase = phase_of(t);
            k.cmd = port == PS && k.a == PE_CMD_ADDRESS;
            in[pe * NPORT + port].push_back(k);
        }
        // Segments: a token's segment is the number of commands on its PE recorded before it.
        data_total_upto.assign(nb * nr, {});
        data_done_upto.assign(nb * nr, {});
        for (int pe = 0; pe < nb * nr; ++pe) {
            std::vector<uint64_t> cmd_t;
            for (const Tok& k : in[pe * NPORT + PS]) if (k.cmd) cmd_t.push_back(k.t);
            const size_t nseg = cmd_t.size() + 1;
            data_total_upto[pe].assign(nseg, 0);
            data_done_upto[pe].assign(nseg, 0);
            for (int q = 0; q < NPORT; ++q) {
                size_t ci = 0;
                for (Tok& k : in[pe * NPORT + q]) {
                    if (k.cmd) { k.seg = static_cast<int>(ci++); continue; }
                    int s = 0;
                    while (s < static_cast<int>(cmd_t.size()) && cmd_t[s] < k.t) ++s;
                    k.seg = s;
                    data_total_upto[pe][s] += 1;
                }
            }
            for (size_t s = 1; s < nseg; ++s) data_total_upto[pe][s] += data_total_upto[pe][s - 1];
        }
        return true;
    }

    template <typename T>
    void offer(VRDSIG<T>& sig, bool valid, const T& data) {
        sig.valid_sig.write(valid);
        if (valid) sig.data_sig.write(data);
    }

    bool eligible(int pe, const Tok& k, uint64_t now) const {
        if (!ideal) return now + C >= k.t;
        if (k.phase > released) return false;
        if (k.cmd) {
            if (cmds_done[pe] != static_cast<size_t>(k.seg)) return false;
            return data_done_upto[pe][k.seg] == data_total_upto[pe][k.seg];
        }
        if (cmds_done[pe] < static_cast<size_t>(k.seg)) return false;
        // A command takes effect in the router after its handshake (START flushes every FIFO), so the next
        // segment's inputs wait `settle` cycles after it.
        return k.seg == 0 || now + C >= cmd_done_t[pe] + settle * C;
    }

    /// PERTURB: whether input i of port q may be offered now (the ESL adapter's gap and gate rules).
    bool perturb_ok(int pe, int q, const Tok& k, uint64_t now) const {
        if (!perturb) return true;
        if (q < 3) {
            if (k.cmd) return true;
            const unsigned need = hacc::pe_input_delay(pe, q, data_n[pe * 3 + q]);
            return need == 0 || now >= last_fire[pe * 3 + q] + need * C;
        }
        const unsigned closed = hacc::pe_perturb().plo_req_closed;
        return closed == 0 || ((now / C + 1) % 32) >= closed;
    }

    void mark_done(int pe, const Tok& k) {
        if (k.cmd) { ++cmds_done[pe]; cmd_done_t[pe] = sc_time_stamp().value(); return; }
        for (size_t s = k.seg; s < data_done_upto[pe].size(); ++s) ++data_done_upto[pe][s];
    }

    bool phase_complete(int k) const {
        for (int pe = 0; pe < nb * nr; ++pe) {
            for (int q = 0; q < NPORT; ++q) {
                const auto& v = in[pe * NPORT + q];
                const size_t i = idx[pe * NPORT + q];
                if (i < v.size() && v[i].phase <= k) return false;
            }
            const size_t i = resp_idx[pe];
            if (i < resp_phase[pe].size() && resp_phase[pe][i] <= k) return false;
        }
        return true;
    }

    void apply_spatial(const Spatial& s) {
        if (std::FILE* f = hacc::pe_log()) {
            if (s.configure)
                std::fprintf(f, "W cmd=configure h=%u w=%u rows=%u loop_pc=%u loop_end_pc=%u t=%llu\n", s.h, s.w,
                             s.rows, s.loop_pc, s.loop_end_pc, static_cast<unsigned long long>(sc_time_stamp().value()));
            else
                std::fprintf(f, "W cmd=clear h=- w=- rows=- loop_pc=- loop_end_pc=- t=%llu\n",
                             static_cast<unsigned long long>(sc_time_stamp().value()));
        }
        for (auto& p : pes) {
            if (s.configure) p->if_id_stage.configure_spatial_window_loop(s.loop_pc, s.loop_end_pc, s.w);
            else p->if_id_stage.clear_spatial_window_loop();
        }
    }

    void release(int k, uint64_t now) {
        release_t[k] = now;
        for (int pe = 0; pe < nb * nr; ++pe)
            while (cfg_idx[pe] < cfg[pe].size() && cfg[pe][cfg_idx[pe]].phase <= k) {
                en[pe]->write(cfg[pe][cfg_idx[pe]].en != 0);
                mode[pe]->write(static_cast<PERouterMode>(cfg[pe][cfg_idx[pe]].mode));
                ++cfg_idx[pe];
            }
        while (spatial_idx < spatial.size() && spatial[spatial_idx].phase <= k) apply_spatial(spatial[spatial_idx++]);
    }

    void log_phase(int k, uint64_t now) {
        if (std::FILE* f = hacc::pe_log())
            std::fprintf(f, "P k=%d release=%llu data_start=%llu end=%llu t=%llu\n", k,
                         static_cast<unsigned long long>(release_t[k]), static_cast<unsigned long long>(data_start[k]),
                         static_cast<unsigned long long>(phase_end[k]), static_cast<unsigned long long>(now));
    }

    void drive() {
        const uint64_t now = sc_time_stamp().value();
        if (finished) return;
        if (now >= max_t) { std::fprintf(stderr, "[REPLAY] max cycles reached\n"); finished = true; sc_stop(); return; }
        if (!reset_n.read()) return;
        const int n = nb * nr;
        // Debug (REPLAY_DEBUG="pe begin_ps end_ps"): per-edge valid/ready of one PE's input ports.
        static int dbg_pe = -1; static uint64_t dbg_b = 0, dbg_e = 0; static bool dbg_init = false;
        if (!dbg_init) {
            dbg_init = true;
            if (const char* s = std::getenv("REPLAY_DEBUG")) std::sscanf(s, "%d %llu %llu", &dbg_pe,
                reinterpret_cast<unsigned long long*>(&dbg_b), reinterpret_cast<unsigned long long*>(&dbg_e));
        }
        if (dbg_pe >= 0 && now >= dbg_b && now <= dbg_e)
            std::fprintf(stderr, "[DBG] t=%llu ps v%d r%d pd v%d r%d pli v%d r%d idx_pli=%zu next_t=%llu\n",
                         static_cast<unsigned long long>(now), ps[dbg_pe]->valid_sig.read(), ps[dbg_pe]->ready_sig.read(),
                         pd[dbg_pe]->valid_sig.read(), pd[dbg_pe]->ready_sig.read(), pli[dbg_pe]->valid_sig.read(),
                         pli[dbg_pe]->ready_sig.read(), idx[dbg_pe * NPORT + PLI],
                         static_cast<unsigned long long>(idx[dbg_pe * NPORT + PLI] < in[dbg_pe * NPORT + PLI].size()
                             ? in[dbg_pe * NPORT + PLI][idx[dbg_pe * NPORT + PLI]].t : 0));
        // 1. Handshakes the PEs completed at this edge.
        for (int pe = 0; pe < n; ++pe) {
            VRDSIG<noc_request_t>* req[3] = {ps[pe].get(), pd[pe].get(), pli[pe].get()};
            for (int q = 0; q < NPORT; ++q) {
                const bool fire = q < 3 ? (req[q]->valid_sig.read() && req[q]->ready_sig.read())
                                        : (ploreq[pe]->valid_sig.read() && ploreq[pe]->ready_sig.read());
                if (!fire) continue;
                const Tok& k = in[pe * NPORT + q][idx[pe * NPORT + q]++];
                if (q < 3) { last_fire[pe * 3 + q] = now; if (!k.cmd) ++data_n[pe * 3 + q]; }
                mark_done(pe, k);
                phase_end[k.phase] = now;
            }
            if (resp[pe]->valid_sig.read() && resp[pe]->ready_sig.read()) {
                const size_t i = resp_idx[pe]++;
                if (i < resp_phase[pe].size()) phase_end[resp_phase[pe][i]] = now;
            }
        }
        // 2. Phases (ideal) and configuration.
        if (ideal) {
            if (release_t[released] == 0) release(released, now);
            while (released < n_phase && phase_complete(released)) {
                log_phase(released, now);
                ++released;
                release(released, now);
            }
        } else {
            for (int pe = 0; pe < n; ++pe)
                while (cfg_idx[pe] < cfg[pe].size() && now + C >= cfg[pe][cfg_idx[pe]].t) {
                    en[pe]->write(cfg[pe][cfg_idx[pe]].en != 0);
                    mode[pe]->write(static_cast<PERouterMode>(cfg[pe][cfg_idx[pe]].mode));
                    ++cfg_idx[pe];
                }
            while (spatial_idx < spatial.size() && now >= spatial[spatial_idx].t) apply_spatial(spatial[spatial_idx++]);
        }
        // 3. Offer the next input on every port; PLO responses.
        bool all_done = true;
        for (int pe = 0; pe < n; ++pe) {
            VRDSIG<noc_request_t>* req[3] = {ps[pe].get(), pd[pe].get(), pli[pe].get()};
            for (int q = 0; q < NPORT; ++q) {
                const auto& v = in[pe * NPORT + q];
                const size_t i = idx[pe * NPORT + q];
                const bool more = i < v.size();
                all_done = all_done && !more;
                const bool ok = more && eligible(pe, v[i], now) && perturb_ok(pe, q, v[i], now);
                if (ok && !v[i].cmd && data_start[v[i].phase] == 0) data_start[v[i].phase] = now;
                if (q < 3) {
                    noc_request_t r;
                    if (ok) { r.data = v[i].d; r.addr = v[i].a; r.mask = static_cast<size_t>(v[i].m); }
                    offer(*req[q], ok, r);
                } else {
                    offer(*ploreq[pe], ok, ok ? noc_addr_req_t(v[i].a) : noc_addr_req_t());
                }
            }
            const size_t ri = resp_idx[pe];
            all_done = all_done && ri >= resp_t[pe].size();
            const bool rdy = ideal ? true : (ri < resp_t[pe].size() && now + C == resp_t[pe][ri]);
            resp[pe]->ready_sig.write(rdy);
        }
        if (all_done) {
            if (ideal) log_phase(released, now);
            finished = true;
            sc_stop();
        }
    }
};

int sc_main(int argc, char* argv[]) {
    if (argc < 8) {
        std::fprintf(stderr, "usage: %s PE_LOG WAVE_LOG NUM_BUS PES_PER_BUS PE_FIFO_DEPTH timed|ideal OUT_PE_LOG "
                             "[MAX_CYCLES [SETTLE [IN_FIFO_DEPTH [PERTURB]]]]\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[6];
    if (mode != "timed" && mode != "ideal") { std::fprintf(stderr, "MODE must be timed or ideal\n"); return 2; }
    const uint64_t max_cycles = argc > 8 ? std::strtoull(argv[8], nullptr, 10) : 40000000ull;
    const uint64_t settle = argc > 9 ? std::strtoull(argv[9], nullptr, 10) : 2ull;
    const size_t in_fifo_depth = argc > 10 ? std::strtoul(argv[10], nullptr, 10) : 0;
    const std::string perturb = argc > 11 ? argv[11] : "-";
    if (perturb != "-") {
        if (mode != "ideal") { std::fprintf(stderr, "PERTURB needs ideal mode\n"); return 2; }
        if (!hacc::parse_pe_perturb(perturb) || hacc::pe_perturb().plo_resp_closed) {
            std::fprintf(stderr, "bad PERTURB %s\n", perturb.c_str());
            return 2;
        }
    }
    hacc::pe_log() = std::fopen(argv[7], "w");
    if (!hacc::pe_log()) { std::fprintf(stderr, "cannot open %s\n", argv[7]); return 2; }
    ReplayTB tb("replay", std::atoi(argv[3]), std::atoi(argv[4]), std::strtoul(argv[5], nullptr, 10), mode == "ideal",
                max_cycles, settle, in_fifo_depth);
    if (!tb.load(argv[1], argv[2])) return 2;
    tb.perturb = perturb != "-";
    tb.data_n.assign(static_cast<size_t>(tb.nb * tb.nr * 3), 0);
    tb.last_fire.assign(static_cast<size_t>(tb.nb * tb.nr * 3), 0);
    tb.reset_n.write(false);
    sc_start(sc_time(10, SC_NS));  // five 2 ns periods, as the ESL main
    tb.reset_n.write(true);
    sc_start();
    std::fclose(hacc::pe_log());
    hacc::pe_log() = nullptr;
    bool complete = true;
    for (size_t i = 0; i < tb.idx.size(); ++i) complete = complete && tb.idx[i] == tb.in[i].size();
    for (size_t i = 0; i < tb.resp_idx.size(); ++i) complete = complete && tb.resp_idx[i] == tb.resp_t[i].size();
    if (!complete) {  // where it stopped: the next input on every port of the first PEs
        for (int pe = 0; pe < std::min(2, static_cast<int>(tb.resp_idx.size())); ++pe) {
            std::fprintf(stderr, "[STUCK] pe=%d cmds_done=%zu released=%d\n", pe, tb.cmds_done[pe], tb.released);
            for (int q = 0; q < NPORT; ++q) {
                const auto& v = tb.in[pe * NPORT + q];
                const size_t i = tb.idx[pe * NPORT + q];
                if (i < v.size())
                    std::fprintf(stderr, "  %s idx=%zu/%zu next t=%llu seg=%d phase=%d cmd=%d a=%x d=%llx done_upto=%zu total_upto=%zu\n",
                                 PORT_NAME[q], i, v.size(), static_cast<unsigned long long>(v[i].t), v[i].seg, v[i].phase,
                                 v[i].cmd ? 1 : 0, v[i].a, static_cast<unsigned long long>(v[i].d),
                                 tb.data_done_upto[pe][v[i].seg], tb.data_total_upto[pe][v[i].seg]);
            }
        }
    }
    std::printf("[REPLAY] mode=%s complete=%d end=%llu\n", mode.c_str(), complete ? 1 : 0,
                static_cast<unsigned long long>(sc_time_stamp().value()));
    return complete ? 0 : 1;
}
