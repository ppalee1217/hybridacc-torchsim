#pragma once
/**
 * @file ScalarPe.hpp
 * @brief Scalar endpoint that stands in for the PE array (M21 WP-J J3, separability test).
 *
 * Rulings: agent_run/261021-m21-decomposition/decide-separability/decision.md (researcher, 2026-10-21): inside the
 * full ESL the PE array is replaced by a scalar endpoint while HDDU/AGU, NoC/MBUS, SPM, DMA and firmware stay
 * real; the hybrid run's W6 predicts the real one. Enabled by `--scalar-pe FILE --scalar-rule bulk|linear`;
 * without it nothing here is instantiated.
 *
 * Every PE position gets a ScalarPe with ProcessElement's ports. It accepts every input whenever its router is
 * enabled (no backpressure from compute: that is the scalar hypothesis), counts data tokens per port to tell
 * which wave they belong to, and serves the recorded outputs of its PE (FILE) through the same PLO
 * request/response handshake as PErouter (request accepted only in IDLE with an output available; the response
 * registered and offered from the next cycle). Wave k's outputs become available by one of two rules:
 *   bulk    all at once, when every data operand of wave k has arrived and T0_k + P_k has passed;
 *   linear  output j of a PE's n outputs when the first ceil((j+1) N_k / n) operands of wave k have arrived and
 *           T0_k + F_k + (P_k - F_k) j / (n - 1) has passed (T0_k + P_k when n = 1);
 * T0_k is the edge of wave k's first data operand at any PE, N_k its data operands over all PEs, P_k and F_k (from
 * the ideal harness, J2) the cycles from the first data offered to the last input or PLO response, and to the
 * first PLO response. A PE is busy after a START and halted once the last wave of that START's invocation has all
 * its outputs available; it reports queued work while any output of a started wave is not yet served.
 * Local-network ports are tied off (the chain is inside the replaced array). The spatial-tail override is
 * replayed from FILE (active rows per HDDU START), since there is no PE program to inspect.
 *
 * FILE (text, one record per line):
 *   scalar-pe v1
 *   wave K P F N                    per wave, K from 0
 *   in B R K PS PD PLI              data tokens of PE (B, R) in wave K per port (absent = 0)
 *   out B R K N V0 V1 ...           the PE's PLO response values in wave K, in order (hex)
 *   start B R K                     a START on PE (B, R) begins an invocation whose first wave is K
 *   spatial ROWS|-                  one per HDDU START, in order: active rows, or '-' for a clear
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <systemc>

#include "Utils/utils.hpp"

namespace hacc {

struct ScalarPeOracle {
    bool linear = false;
    int waves = 0;
    std::vector<uint64_t> P, F, N;
    struct Pe {
        std::vector<std::array<uint64_t, 3>> in;      // [wave] ps, pd, pli
        std::vector<std::vector<uint64_t>> out;       // [wave] values
        std::vector<int> start_wave;                  // invocation i begins at wave start_wave[i]
    };
    std::map<std::pair<int, int>, Pe> pe;
    std::vector<int> spatial;                         // rows per configure call; -1 = clear
    // Runtime (shared by every endpoint).
    std::vector<uint64_t> T0, arrived;
    std::vector<bool> started;
    size_t spatial_idx = 0;

    Pe& at(int b, int r) {
        Pe& p = pe[{b, r}];
        if (p.in.size() < static_cast<size_t>(waves)) { p.in.resize(waves, {0, 0, 0}); p.out.resize(waves); }
        return p;
    }
    void arrive(int k, uint64_t t) {
        if (!started[k]) { started[k] = true; T0[k] = t; }
        ++arrived[k];
    }
    /// Whether output j of n in wave k is available at edge t (ps), by the selected rule.
    bool available(int k, size_t j, size_t n, uint64_t t) const {
        if (!started[k]) return false;
        const uint64_t C = 2000;
        if (!linear) return arrived[k] >= N[k] && t >= T0[k] + P[k] * C;
        const uint64_t need = ((j + 1) * N[k] + n - 1) / n;
        const uint64_t at = n > 1 ? F[k] + (P[k] - F[k]) * j / (n - 1) : P[k];
        return arrived[k] >= need && t >= T0[k] + at * C;
    }
    bool wave_done(int k, uint64_t t) const {  // every output of wave k available
        if (!started[k]) return false;
        return arrived[k] >= N[k] && t >= T0[k] + P[k] * 2000;
    }
};
inline std::unique_ptr<ScalarPeOracle>& scalar_pe() {
    static std::unique_ptr<ScalarPeOracle> p;
    return p;
}
inline bool load_scalar_pe(const std::string& path, const std::string& rule, std::string& err) {
    if (rule != "bulk" && rule != "linear") { err = "rule must be bulk or linear"; return false; }
    std::ifstream f(path);
    if (!f) { err = "cannot open " + path; return false; }
    auto o = std::make_unique<ScalarPeOracle>();
    o->linear = rule == "linear";
    std::string line;
    if (!std::getline(f, line) || line != "scalar-pe v1") { err = "not a scalar-pe v1 file"; return false; }
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string kind;
        if (!(in >> kind)) continue;
        if (kind == "wave") {
            int k; uint64_t P, F, N;
            in >> k >> P >> F >> N;
            if (k != o->waves) { err = "waves out of order"; return false; }
            o->P.push_back(P); o->F.push_back(F); o->N.push_back(N); ++o->waves;
        } else if (kind == "in") {
            int b, r, k; uint64_t a, d, l;
            in >> b >> r >> k >> a >> d >> l;
            if (k < 0 || k >= o->waves) { err = "in: wave out of range"; return false; }
            o->at(b, r).in[k] = {a, d, l};
        } else if (kind == "out") {
            int b, r, k; size_t n;
            in >> b >> r >> k >> n;
            if (k < 0 || k >= o->waves) { err = "out: wave out of range"; return false; }
            auto& v = o->at(b, r).out[k];
            std::string h;
            while (in >> h) v.push_back(std::stoull(h, nullptr, 16));
            if (v.size() != n) { err = "out: count differs from values"; return false; }
        } else if (kind == "start") {
            int b, r, k;
            in >> b >> r >> k;
            o->at(b, r).start_wave.push_back(k);
        } else if (kind == "spatial") {
            std::string v;
            in >> v;
            o->spatial.push_back(v == "-" ? -1 : std::stoi(v));
        } else {
            err = "unknown record " + kind; return false;
        }
    }
    for (int k = 0; k < o->waves; ++k) {
        size_t outs = 0;
        for (auto& kv : o->pe) if (static_cast<size_t>(k) < kv.second.out.size()) outs += kv.second.out[k].size();
        if (o->N[k] == 0 && outs) { err = "a wave with outputs has no data operands"; return false; }
        if (o->F[k] > o->P[k]) { err = "F > P"; return false; }
    }
    o->T0.assign(o->waves, 0);
    o->arrived.assign(o->waves, 0);
    o->started.assign(o->waves, false);
    scalar_pe() = std::move(o);
    return true;
}

/// One PE position of the replaced array (see the file comment).
SC_MODULE(ScalarPe) {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> reset_n;
    sc_core::sc_in<bool> router_enable;
    sc_core::sc_in<PERouterMode> router_mode;
    VRDIF<noc_request_t> noc_ps_in;
    VRDIF<noc_request_t> noc_pd_in;
    VRDIF<noc_request_t> noc_pli_in;
    VRDIF<noc_addr_req_t> noc_plo_in;
    VRDOF<noc_response_t> noc_plo_out;
    sc_core::sc_out<bool> pe_busy;
    sc_core::sc_out<bool> pe_halted;
    VRDIF<uint64_t> ln_pli;
    VRDOF<uint64_t> ln_plo;

    int bus = -1, row = -1;
    sc_core::sc_signal<bool> pending_reg, wait_resp_reg, has_out_reg, busy_reg, halted_reg;
    sc_core::sc_signal<noc_response_t> resp_reg;

    SC_HAS_PROCESS(ScalarPe);
    explicit ScalarPe(sc_core::sc_module_name name)
        : sc_module(name), clk("clk"), reset_n("reset_n"), router_enable("router_enable"), router_mode("router_mode"),
          noc_ps_in("noc_ps_in"), noc_pd_in("noc_pd_in"), noc_pli_in("noc_pli_in"), noc_plo_in("noc_plo_in"),
          noc_plo_out("noc_plo_out"), pe_busy("pe_busy"), pe_halted("pe_halted"), ln_pli("ln_pli"), ln_plo("ln_plo"),
          pending_reg("pending_reg"), wait_resp_reg("wait_resp_reg"), has_out_reg("has_out_reg"),
          busy_reg("busy_reg"), halted_reg("halted_reg"), resp_reg("resp_reg") {
        SC_METHOD(comb);
        sensitive << router_enable << router_mode << pending_reg << wait_resp_reg << has_out_reg << busy_reg
                  << halted_reg << resp_reg << reset_n;
        SC_METHOD(seq);
        sensitive << clk.pos();
    }

    // Per-PE runtime.
    std::array<uint64_t, 3> accepted{{0, 0, 0}};
    std::array<int, 3> wave_ptr{{0, 0, 0}};
    std::array<uint64_t, 3> in_wave{{0, 0, 0}};  // tokens of the current wave seen on each port
    std::vector<std::pair<int, size_t>> outs;    // (wave, index) in serving order
    size_t served = 0;
    int invocation = -1;

    void set_position(int b, int r) {
        bus = b; row = r;
        ScalarPeOracle& o = *scalar_pe();
        ScalarPeOracle::Pe& p = o.at(b, r);
        for (int k = 0; k < o.waves; ++k)
            for (size_t j = 0; j < p.out[k].size(); ++j) outs.emplace_back(k, j);
    }

    bool plo_to_bus() const {
        const PERouterMode m = router_mode.read();
        return m == PERouterMode::PLI_FROM_LN_PLO_TO_BUS || m == PERouterMode::PLI_FROM_BUS_PLO_TO_BUS;
    }

    void comb() {
        const bool en = reset_n.read() && router_enable.read();
        noc_ps_in.ready_out.write(en);
        noc_pd_in.ready_out.write(en);
        noc_pli_in.ready_out.write(en);
        noc_plo_in.ready_out.write(en && !wait_resp_reg.read() && plo_to_bus() && has_out_reg.read());
        noc_plo_out.valid_out.write(pending_reg.read());
        noc_plo_out.data_out.write(pending_reg.read() ? resp_reg.read() : noc_response_t());
        ln_pli.ready_out.write(false);
        ln_plo.valid_out.write(false);
        ln_plo.data_out.write(0);
        pe_busy.write(busy_reg.read());
        pe_halted.write(halted_reg.read());
    }

    void take(int port, uint64_t t) {
        ScalarPeOracle& o = *scalar_pe();
        ScalarPeOracle::Pe& p = o.at(bus, row);
        int& k = wave_ptr[port];
        while (k < o.waves && in_wave[port] >= p.in[k][port]) { ++k; in_wave[port] = 0; }
        if (k >= o.waves) {
            std::fprintf(stderr, "[SCALAR-PE] PE %d.%d port %d: more data tokens than recorded\n", bus, row, port);
            sc_core::sc_stop();
            return;
        }
        ++in_wave[port];
        ++accepted[port];
        o.arrive(k, t);
    }

    void seq() {
        if (!reset_n.read()) {
            pending_reg.write(false); wait_resp_reg.write(false); has_out_reg.write(false);
            busy_reg.write(false); halted_reg.write(false);
            return;
        }
        ScalarPeOracle& o = *scalar_pe();
        ScalarPeOracle::Pe& p = o.at(bus, row);
        const uint64_t t = sc_core::sc_time_stamp().value();
        // Inputs completed at this edge (values as this edge's processes see them).
        VRDIF<noc_request_t>* ports[3] = {&noc_ps_in, &noc_pd_in, &noc_pli_in};
        bool started_now = false;
        for (int q = 0; q < 3; ++q) {
            if (!(ports[q]->valid_in.read() && ports[q]->ready_out.read())) continue;
            const noc_request_t r = ports[q]->data_in.read();
            if (q == 0 && r.addr == PE_CMD_ADDRESS) {
                if ((r.data & 0xF) == 4) { ++invocation; started_now = true; }  // CMD_START_PE
                continue;
            }
            take(q, t);
        }
        // PLO request/response, as PErouter::comb_output_and_state.
        const bool fire = noc_plo_in.valid_in.read() && noc_plo_in.ready_out.read();
        const bool pending = pending_reg.read();
        const bool consumed = pending && noc_plo_out.ready_in.read();
        if (fire) {
            const auto& w = outs[served];
            noc_response_t resp;
            resp.data = p.out[w.first][w.second];
            resp.status = NOC_RESPONSE_STATUS::NOC_OK;
            resp_reg.write(resp);
            ++served;
            pending_reg.write(true);
        } else if (consumed) {
            pending_reg.write(false);
        }
        if (pending) wait_resp_reg.write(!consumed);
        else wait_resp_reg.write(false);
        // Whether the next output (after this edge's request, if any) is available in the next cycle; like
        // PErouter's FIFO, a request can be taken every cycle while the responses are taken.
        bool avail = false;
        if (served < outs.size()) {
            const auto& w = outs[served];
            avail = o.available(w.first, w.second, p.out[w.first].size(), t);
        }
        has_out_reg.write(avail);
        // Busy / halted for the current invocation.
        if (started_now) { busy_reg.write(true); halted_reg.write(false); }
        else if (invocation >= 0 && busy_reg.read()) {
            const int first = invocation < static_cast<int>(p.start_wave.size()) ? p.start_wave[invocation] : o.waves;
            const int last_wave = invocation + 1 < static_cast<int>(p.start_wave.size()) ? p.start_wave[invocation + 1] - 1
                                                                                       : o.waves - 1;
            if (first > last_wave || o.wave_done(last_wave, t)) {
                busy_reg.write(false); halted_reg.write(true);
            }
        }
    }

    /// Queued work for the NoC status: a response pending or an output of a started wave not served.
    bool pending_quiesce() const { return pending_reg.read(); }
    bool fifo_nonempty() const {
        if (served >= outs.size()) return false;
        return scalar_pe()->started[outs[served].first];
    }
};

}  // namespace hacc
