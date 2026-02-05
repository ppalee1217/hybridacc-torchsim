#include <systemc>

#include <cassert>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "ProcessElement.hpp"
#include "utils.hpp"
#include "tb_utils.hpp"

using namespace sc_core;
using namespace hybridacc;

namespace {

static constexpr int DEFAULT_CLOCK_PERIOD_NS = 10;
static constexpr int PE_GAP_CYCLES = 1;

struct ConvParams {
    int kernel_size = 0;
    int in_ch = 0;
    int out_ch = 0;
    int out_width = 0;
    int in_width = 0;
    int groups_per_output = 0; // out_ch / 4
};

ConvParams parse_conv_params_from_meta(const std::string& meta_path) {
    ConvParams p;
    auto meta = read_config_file(meta_path);
    if (meta.count("kernel_size")) p.kernel_size = std::stoi(meta["kernel_size"]);
    if (meta.count("in_ch")) p.in_ch = std::stoi(meta["in_ch"]);
    if (meta.count("out_ch")) p.out_ch = std::stoi(meta["out_ch"]);
    if (meta.count("out_width")) p.out_width = std::stoi(meta["out_width"]);
    if (meta.count("in_width")) p.in_width = std::stoi(meta["in_width"]);
    if (p.out_ch > 0) p.groups_per_output = p.out_ch / 4;
    return p;
}

uint64_t pack_fp16x4(const std::vector<fp16_t>& vals, size_t base_idx, int lanes, uint8_t& mask_out) {
    uint64_t data = 0;
    mask_out = 0;
    for (int lane = 0; lane < lanes; lane++) {
        const size_t idx = base_idx + static_cast<size_t>(lane);
        if (idx < vals.size()) {
            data |= (static_cast<uint64_t>(vals[idx]) << (lane * 16));
            mask_out |= static_cast<uint8_t>(1U << lane);
        }
    }
    return data;
}

} // namespace

// -----------------------------------------------------------------------------
// TestBench
class PESimTestBench : public sc_module {
public:
    // Clock and reset
    sc_clock clk;
    sc_signal<bool> reset_n;

    // Router config
    sc_signal<bool> router_enable;
    sc_signal<PERouterMode> router_mode;

    sc_signal<bool> pe_busy;

    // NoC interfaces (Valid/Ready)
    VRDSIG<noc_request_t> ps_sig;
    VRDSIG<noc_request_t> pd_sig;
    VRDSIG<noc_request_t> pli_sig;
    VRDSIG<noc_addr_req_t> plo_req_sig;
    VRDSIG<noc_response_t> plo_resp_sig;

    // Local network interfaces (unused in BUS/BUS routing, but must be bound)
    VRDSIG<uint64_t> ln_pli_sig;
    VRDSIG<uint64_t> ln_plo_sig;

    // DUT
    pe::ProcessElement pe;

    // Configuration
    std::string data_dir;
    double verify_tolerance;
    int clock_period_ns;

    // Data
    ConvParams conv;
    std::vector<uint16_t> program;
    std::vector<uint64_t> weights;
    std::vector<fp16_t> activations;
    std::vector<uint64_t> ps_inputs;
    std::vector<fp16_t> expected_fp16;
    std::vector<uint64_t> received_vectors;

    // Synchronization
    sc_event start_traffic_event;
    sc_event ps_program_event;
    sc_event ps_program_done_event;
    sc_event ps_start_event;
    sc_event ps_start_done_event;
    sc_event ps_done_event;
    sc_event pd_done_event;
    sc_event pli_done_event;
    sc_event plo_req_done_event;
    sc_event outputs_done_event;

    SC_HAS_PROCESS(PESimTestBench);

    PESimTestBench(sc_module_name name, std::string data_dir, double verify_tolerance, int clock_period_ns)
        : sc_module(name),
          clk("clk", clock_period_ns, SC_NS),
          reset_n("reset_n"),
          router_enable("router_enable"),
          router_mode("router_mode"),
          pe_busy("pe_busy"),
          ps_sig("ps_sig"),
          pd_sig("pd_sig"),
          pli_sig("pli_sig"),
          plo_req_sig("plo_req_sig"),
          plo_resp_sig("plo_resp_sig"),
          ln_pli_sig("ln_pli_sig"),
          ln_plo_sig("ln_plo_sig"),
          pe("PE_DUT"),
          data_dir(std::move(data_dir)),
          verify_tolerance(verify_tolerance),
          clock_period_ns(clock_period_ns)
    {
        // DUT bind
        pe.clk(clk);
        pe.reset_n(reset_n);
        pe.router_enable(router_enable);
        pe.router_mode(router_mode);
        pe.pe_busy(pe_busy);
        connect_vr_signals(pe.noc_ps_in, ps_sig);
        connect_vr_signals(pe.noc_pd_in, pd_sig);
        connect_vr_signals(pe.noc_pli_in, pli_sig);
        connect_vr_signals(pe.noc_plo_in, plo_req_sig);
        connect_vr_signals(pe.noc_plo_out, plo_resp_sig);

        connect_vr_signals(pe.ln_pli, ln_pli_sig);
        connect_vr_signals(pe.ln_plo, ln_plo_sig);

        SC_THREAD(test_main);

        SC_THREAD(ps_sender);
        sensitive << clk.posedge_event();

        SC_THREAD(pd_sender);
        sensitive << clk.posedge_event();

        SC_THREAD(pli_sender);
        sensitive << clk.posedge_event();

        SC_THREAD(plo_request_thread);
        sensitive << clk.posedge_event();

        SC_THREAD(plo_response_sink);
        sensitive << clk.posedge_event();
    }

private:
    void wait_cycles(int cycles) {
        for (int i = 0; i < cycles; i++) {
            wait(clk.posedge_event());
        }
    }

    void reset_dut(int reset_cycles) {
        reset_n.write(false);
        router_enable.write(false);
        router_mode.write(PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);

        // Note: VR/Ready interfaces are driven by their dedicated threads.
        // Avoid driving them here to prevent sc_signal multi-driver errors.
        ln_pli_sig.valid_sig.write(false);
        ln_plo_sig.ready_sig.write(true); // always accept LN stream if ever enabled

        wait_cycles(reset_cycles);

        reset_n.write(true);
        wait_cycles(1);
    }

    void send_req(VRDSIG<noc_request_t>& sig, const noc_request_t& req) {
        sig.data_sig.write(req);
        sig.valid_sig.write(true);

        // Avoid a SystemC delta-cycle race: ready is typically produced by SC_METHOD(s)
        // that run *after* the clock edge. If we sample ready immediately at posedge,
        // we may see a stale value from the previous cycle and drop a beat.
        while (true) {
            wait(clk.posedge_event());
            wait(SC_ZERO_TIME);
            if (sig.ready_sig.read()) {
                // Handshake has occurred on this clock edge.
                // Deassert valid immediately (same cycle) so we don't
                // accidentally resend the same beat on the next edge.
                sig.valid_sig.write(false);
                break;
            }
        }
    }

    void send_addr_req(VRDSIG<noc_addr_req_t>& sig, const noc_addr_req_t& req) {
        sig.data_sig.write(req);
        sig.valid_sig.write(true);

        while (true) {
            wait(clk.posedge_event());
            wait(SC_ZERO_TIME);
            if (sig.ready_sig.read()) {
                sig.valid_sig.write(false);
                break;
            }
        }
    }

    void load_test_data_conv2d() {
        const std::string inst_file = data_dir + "/pe_program.bin";
        const std::string weight_file = data_dir + "/weight.bin";
        const std::string activation_file = data_dir + "/activation_input.bin";
        const std::string ps_input_file = data_dir + "/ps_input.bin";
        const std::string expected_output_file = data_dir + "/activation_output.bin";
        const std::string meta_file = data_dir + "/meta.txt";

        conv = parse_conv_params_from_meta(meta_file);
        if (conv.kernel_size <= 0 || conv.in_ch <= 0 || conv.out_ch <= 0 || conv.out_width <= 0 || conv.groups_per_output <= 0 || conv.in_width <= 0) {
            std::cerr << "[TB] Invalid conv params from meta.txt" << std::endl;
            sc_stop();
            return;
        }

        program = read_binary_file<uint16_t>(inst_file);
        weights = read_binary_file<uint64_t>(weight_file);
        activations = read_binary_file<fp16_t>(activation_file);
        ps_inputs = read_binary_file<uint64_t>(ps_input_file);
        expected_fp16 = read_binary_file<fp16_t>(expected_output_file);

        std::cout << "[TB] Loaded data from " << data_dir << std::endl;
        std::cout << "  Program: " << program.size() << " instructions" << std::endl;
        std::cout << "  Weights: " << weights.size() << " x uint64" << std::endl;
        std::cout << "  Activations: " << activations.size() << " x fp16" << std::endl;
        std::cout << "  PS Inputs: " << ps_inputs.size() << " x uint64" << std::endl;
        std::cout << "  Expected: " << expected_fp16.size() << " x fp16" << std::endl;
        std::cout << "[TB] Meta:" << std::endl;
        std::cout << "  kernel_size=" << conv.kernel_size
                  << " in_ch=" << conv.in_ch
                  << " out_ch=" << conv.out_ch
                  << " out_width=" << conv.out_width
                  << " in_width=" << conv.in_width
                  << " groups_per_output=" << conv.groups_per_output << std::endl;
    }

    void program_pe() {
        // Load program through PS command messages (addr=0x40)
        for (size_t i = 0; i < program.size(); i++) {
            uint64_t cmd = 2; // CMD_LOAD_PROGRAM
            cmd |= (static_cast<uint64_t>(i * sizeof(uint16_t)) << 4);
            cmd |= (static_cast<uint64_t>(program[i]) << 16);

            noc_request_t req{};
            req.addr = 0x40;
            req.data = cmd;
            req.mask = 0;
            send_req(ps_sig, req);
        }
    }

    void start_pe() {
        // CMD_START_PE = 4
        noc_request_t req{};
        req.addr = 0x40;
        req.data = 4;
        req.mask = 0;
        send_req(ps_sig, req);
        wait_cycles(5);
    }

    // -----------------------------------------------------------------
    // Threads

    void test_main() {
        sc_report_handler::set_actions("/IEEE_Std_1666/deprecated", SC_DO_NOTHING);

        std::cout << "========================================" << std::endl;
        std::cout << "PE Simulation Test (conv2d)" << std::endl;
        std::cout << "========================================" << std::endl;

        reset_dut(10);
        router_enable.write(true);
        router_mode.write(PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);
        wait_cycles(1);

        load_test_data_conv2d();
        if (program.empty()) {
            std::cerr << "[TB] Empty program" << std::endl;
            sc_stop();
            return;
        }

        std::cout << "\n[Step 1] Programming PE..." << std::endl;
        ps_program_event.notify(SC_ZERO_TIME);
        wait(ps_program_done_event);

        std::cout << "\n[Step 2] Starting PE..." << std::endl;
        ps_start_event.notify(SC_ZERO_TIME);
        wait(ps_start_done_event);

        std::cout << "\n[Step 3] Starting traffic threads..." << std::endl;
        start_traffic_event.notify(SC_ZERO_TIME);

        // Wait until we collected all expected vectors (or timeout)
        const size_t expected_vectors = static_cast<size_t>(conv.out_width) * static_cast<size_t>(conv.groups_per_output);
        const uint64_t max_wait_cycles = 200000; // generous
        uint64_t waited = 0;
        while (received_vectors.size() < expected_vectors && waited < max_wait_cycles) {
            wait(clk.posedge_event());
            waited++;
            if (waited % 20000 == 0) {
                std::cout << "[TB] Waiting outputs... cycles=" << waited
                          << " received_vectors=" << received_vectors.size() << "/" << expected_vectors
                          << " pe_cycles=" << pe.get_cycle_count() << std::endl;
            }
        }

        if (received_vectors.size() < expected_vectors) {
            std::cerr << "[TB] Output timeout: received_vectors=" << received_vectors.size()
                      << " expected=" << expected_vectors << std::endl;
        } else {
            std::cout << "[TB] Outputs collected: " << received_vectors.size() << " vectors" << std::endl;
        }

        // Wait for halt (bounded)
        std::cout << "\n[Step 4] Waiting for PE halt..." << std::endl;
        const uint64_t halt_timeout = 200000;
        uint64_t halt_waited = 0;
        while (!pe.is_halted() && halt_waited < halt_timeout) {
            wait(clk.posedge_event());
            halt_waited++;
        }
        std::cout << "[TB] PE halted=" << (pe.is_halted() ? "Yes" : "No")
                  << " after " << halt_waited << " cycles" << std::endl;

        // Verification
        std::cout << "\n[Step 5] Verifying outputs..." << std::endl;
        std::vector<uint16_t> received_fp16;
        received_fp16.reserve(received_vectors.size() * 4);
        for (uint64_t v : received_vectors) {
            for (int lane = 0; lane < 4; lane++) {
                received_fp16.push_back(static_cast<uint16_t>((v >> (lane * 16)) & 0xFFFFU));
            }
        }

        std::cout << "Expected fp16: " << expected_fp16.size() << std::endl;
        std::cout << "Received fp16: " << received_fp16.size() << std::endl;

        auto stats = verify_fp16_vectors(expected_fp16, received_fp16, verify_tolerance);
        std::cout << "----------------------------------------" << std::endl;
        std::cout << "Verification Results:" << std::endl;
        std::cout << "  Total Elements: " << stats.total_elements << std::endl;
        std::cout << "  Mismatches: " << stats.mismatches << " (Tolerance: " << verify_tolerance << ")" << std::endl;
        std::cout << "  Cosine Similarity: " << stats.cosine_similarity << std::endl;
        std::cout << std::scientific << "  Max Difference: " << stats.max_diff << std::dec << std::endl;
        std::cout << std::scientific << "  MSE: " << stats.mse << std::dec << std::endl;
        std::cout << "----------------------------------------" << std::endl;

        if (stats.cosine_similarity > 0.99 && stats.mismatches == 0) {
            std::cout << "✓ Test PASSED" << std::endl;
        } else {
            std::cout << "✗ Test FAILED" << std::endl;
        }

        // Stats
        std::cout << "\n[Step 6] Performance Metrics:" << std::endl;
        const uint64_t total_cycles = pe.get_cycle_count();
        const uint64_t instr = pe.get_instruction_count();
        const double ipc = (total_cycles > 0) ? (static_cast<double>(instr) / static_cast<double>(total_cycles)) : 0.0;
        std::cout << "  Total Cycles: " << total_cycles << std::endl;
        std::cout << "  Instructions: " << instr << std::endl;
        std::cout << "  IPC: " << std::fixed << std::setprecision(3) << ipc << std::endl;

        std::cout << "\n========================================" << std::endl;
        std::cout << "Test completed." << std::endl;
        std::cout << "========================================" << std::endl;

        sc_stop();
    }

    void ps_sender() {
        // Ensure reset is released before any bus activity
        while (!reset_n.read()) {
            wait(reset_n.value_changed_event());
        }

        // Step 1: program PE via PS bus
        wait(ps_program_event);
        program_pe();
        ps_program_done_event.notify(SC_ZERO_TIME);

        // Step 2: start PE via PS bus
        wait(ps_start_event);
        start_pe();
        ps_start_done_event.notify(SC_ZERO_TIME);

        // Step 3: stream weights after traffic starts
        wait(start_traffic_event);
        std::cout << "[PS] Streaming weights..." << std::endl;
        for (size_t i = 0; i < weights.size(); i++) {
            noc_request_t req{};
            req.addr = 0x0000;
            req.data = weights[i];
            req.mask = 0;
            send_req(ps_sig, req);
            if (i % 256 == 0) {
                std::cout << "[PS] Sent weights " << i << "/" << weights.size() << std::endl;
            }
            wait_cycles(PE_GAP_CYCLES);
        }
        std::cout << "[PS] Completed streaming weights." << std::endl;
        ps_done_event.notify(SC_ZERO_TIME);
    }

    void pli_sender() {
        wait(start_traffic_event);
        const size_t expected_vectors = static_cast<size_t>(conv.out_width) * static_cast<size_t>(conv.groups_per_output);
        const size_t want_ps_inputs = expected_vectors;
        if (ps_inputs.size() < want_ps_inputs) {
            std::cout << "[PLI] Warning: ps_inputs smaller than expected ("
                      << ps_inputs.size() << " < " << want_ps_inputs << ")" << std::endl;
        }

        for (int out_pos = 0; out_pos < conv.out_width; out_pos++) {
            const size_t base = static_cast<size_t>(out_pos) * static_cast<size_t>(conv.groups_per_output);
            for (int g = 0; g < conv.groups_per_output; g++) {
                const size_t idx = base + static_cast<size_t>(g);
                if (idx >= ps_inputs.size()) break;
                noc_request_t req{};
                req.addr = 0x0080;
                req.data = ps_inputs[idx];
                req.mask = 0;
                send_req(pli_sig, req);
                wait_cycles(PE_GAP_CYCLES);
            }
        }

        pli_done_event.notify(SC_ZERO_TIME);
    }

    void pd_sender() {
        wait(start_traffic_event);
        std::cout << "[PD] Streaming partial dot-product positions..." << std::endl;
        for (int in_pos = 0; in_pos < conv.in_width; in_pos++) {
            send_pd_position(static_cast<size_t>(in_pos));
            wait_cycles(PE_GAP_CYCLES);
        }
        std::cout << "[PD] Completed streaming PD positions." << std::endl;
        pd_done_event.notify(SC_ZERO_TIME);
    }

    void send_pd_position(size_t position_idx) {
        const size_t base = position_idx * static_cast<size_t>(conv.in_ch);
        int in_ch_step = 0;
        switch (conv.kernel_size) {
            case 3:
                in_ch_step = 4;
                break;
            case 5:
                in_ch_step = 2; // last lane will be masked
                break;
            case 7:
                in_ch_step = 1; // last two lanes will be masked
                break;
            default:
                in_ch_step = 4;
                std::cerr << "[PD] Warning: unexpected kernel_size=" << conv.kernel_size << std::endl;
                break;
        }
        for (int c = 0; c < conv.in_ch; c += in_ch_step) {
            const size_t idx = base + static_cast<size_t>(c);
            if (idx >= activations.size()) {
                return;
            }
            uint8_t mask = 0;
            const uint64_t data = pack_fp16x4(activations, idx, in_ch_step, mask);
            if (mask == 0) {
                return;
            }
            noc_request_t req{};
            req.addr = 0x0040;
            req.data = data;
            req.mask = static_cast<size_t>(mask);
            send_req(pd_sig, req);
        }
    }

    void plo_request_thread() {
        wait(start_traffic_event);

        const size_t expected_vectors = static_cast<size_t>(conv.out_width) * static_cast<size_t>(conv.groups_per_output);
        for (size_t i = 0; i < expected_vectors; i++) {
            noc_addr_req_t req{};
            req.addr = 0x00C0;
            send_addr_req(plo_req_sig, req);
            if ((i + 1) % 256 == 0) {
                std::cout << "[PLO-REQ] Issued " << (i + 1) << "/" << expected_vectors << " read requests" << std::endl;
            }
        }

        plo_req_done_event.notify(SC_ZERO_TIME);
    }

    void plo_response_sink() {
        wait(start_traffic_event);
        plo_resp_sig.ready_sig.write(true);

        const size_t expected_vectors = static_cast<size_t>(conv.out_width) * static_cast<size_t>(conv.groups_per_output);

        while (received_vectors.size() < expected_vectors) {
            wait(clk.posedge_event());
            if (plo_resp_sig.valid_sig.read() && plo_resp_sig.ready_sig.read()) {
                const noc_response_t resp = plo_resp_sig.data_sig.read();
                if (resp.status == NOC_RESPONSE_STATUS::NOC_OK) {
                    received_vectors.push_back(resp.data);
                    if (received_vectors.size() % 256 == 0) {
                        std::cout << "[PLO-RSP] Received " << received_vectors.size() << "/" << expected_vectors << " vectors" << std::endl;
                    }
                    if (received_vectors.size() >= expected_vectors) {
                        outputs_done_event.notify(SC_ZERO_TIME);
                    }
                }
            }
        }

        // Stop consuming further responses to keep verification sizes consistent.
        plo_resp_sig.ready_sig.write(false);
    }

    // -----------------------------------------------------------------
    // Placeholder hooks for GEMM (TBD)
    void load_test_data_gemm_tbd() {
        std::cout << "[TB] GEMM flow is To Be Design" << std::endl;
    }
};

// -----------------------------------------------------------------------------
void print_usage() {
    std::cout << "Usage: ./test_pe_sim [options] <data_dir>" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -c <clock_period_ns>   Set clock period in ns (default: 10)" << std::endl;
    std::cout << "  -v <tolerance>         Set verification tolerance (default: 0.01)" << std::endl;
    std::cout << "  <data_dir>             Path to PE test data (default: output/pe-sim/conv_k3c4)" << std::endl;
}

int sc_main(int argc, char* argv[]) {
    std::string data_dir = "output/pe-sim/conv_k3c4";
    double verify_tolerance = 0.01;
    int clock_period_ns = DEFAULT_CLOCK_PERIOD_NS;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "-c" && i + 1 < argc) {
            clock_period_ns = std::stoi(argv[++i]);
            continue;
        }
        if (arg == "-v" && i + 1 < argc) {
            verify_tolerance = std::stod(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage();
            return 1;
        }
        data_dir = arg;
    }

    PESimTestBench tb("PESimTestBench", data_dir, verify_tolerance, clock_period_ns);
    sc_start();
    return 0;
}