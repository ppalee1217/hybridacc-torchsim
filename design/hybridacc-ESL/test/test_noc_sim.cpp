#include <systemc>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <algorithm>
#include <iomanip>
#include <filesystem>
#include <functional>
#include "NetworkOnChip.hpp"
#include "NoC/NoCRouter.hpp"
#include "utils.hpp"
#include "tb_utils.hpp"

using namespace hybridacc;
using namespace sc_core;
using namespace sc_dt;
namespace fs = std::filesystem;

namespace {

struct ConvParams {
    int kernel_size = 0;
    int in_ch = 0;
    int out_ch = 0;
    int in_height = 0;
    int in_width = 0;
    int out_height = 0;
    int out_width = 0;
    int stride = 0;
    bool partial_sum_zero = false;
    bool ultra_mode = false;

    int temporal_wave_count = 0;
    int temporal_wave_out_h = 0;
    int temporal_wave_out_ch = 0;
    int temporal_wave_in_ch = 0;
};

struct GEMMParams {
    int M = 0;
    int N = 0;
    int K = 0;
    int grid_m = 1;
    int grid_n = 1;
    int grid_k = 1;
    int pe_m = 12;
    int pe_n = 8;
    int pe_k = 32;
    bool partial_sum_zero = false;
    bool ultra_mode = false;

    int wave_m = 1;
    int wave_n = 1;
    int wave_k = 1;

    std::vector<int> grid_m_per_wave;
    std::vector<int> grid_n_per_wave;
    std::vector<int> grid_k_per_wave;
};

int get_cfg_int(const std::map<std::string, std::string>& cfg, const std::string& key, int default_val = 0) {
    auto it = cfg.find(key);
    if (it == cfg.end()) {
        return default_val;
    }
    return std::stoi(it->second);
}

bool get_cfg_bool(const std::map<std::string, std::string>& cfg, const std::string& key, bool default_val = false) {
    auto it = cfg.find(key);
    if (it == cfg.end()) {
        return default_val;
    }
    return (it->second == "True" || it->second == "true" || it->second == "1");
}

std::vector<int> parse_int_list(const std::string& raw) {
    std::string s = raw;
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) {
        return c == '[' || c == ']' || c == '(' || c == ')';
    }), s.end());
    std::replace(s.begin(), s.end(), ',', ' ');
    std::stringstream ss(s);
    std::vector<int> out;
    int val = 0;
    while (ss >> val) {
        out.push_back(val);
    }
    return out;
}

std::vector<int> get_cfg_list_int(const std::map<std::string, std::string>& cfg, const std::string& key) {
    auto it = cfg.find(key);
    if (it == cfg.end()) {
        return {};
    }
    return parse_int_list(it->second);
}

ConvParams parse_conv_params_from_config(const std::map<std::string, std::string>& cfg) {
    ConvParams p;
    p.kernel_size = get_cfg_int(cfg, "kernel_size");
    p.in_ch = get_cfg_int(cfg, "in_ch");
    p.out_ch = get_cfg_int(cfg, "out_ch");
    p.in_height = get_cfg_int(cfg, "in_height");
    p.in_width = get_cfg_int(cfg, "in_width");
    p.out_height = get_cfg_int(cfg, "out_height");
    p.out_width = get_cfg_int(cfg, "out_width");
    p.stride = get_cfg_int(cfg, "stride");
    p.partial_sum_zero = get_cfg_bool(cfg, "partial_sum_zero");
    p.ultra_mode = get_cfg_bool(cfg, "ultra_mode");

    p.temporal_wave_count = get_cfg_int(cfg, "temporal_wave_count");
    p.temporal_wave_out_h = get_cfg_int(cfg, "temporal_wave_out_h");
    p.temporal_wave_out_ch = get_cfg_int(cfg, "temporal_wave_out_ch");
    p.temporal_wave_in_ch = get_cfg_int(cfg, "temporal_wave_in_ch");
    return p;
}

GEMMParams parse_gemm_params_from_config(const std::map<std::string, std::string>& cfg) {
    GEMMParams p;
    p.M = get_cfg_int(cfg, "M");
    p.N = get_cfg_int(cfg, "N");
    p.K = get_cfg_int(cfg, "K");
    p.grid_m = get_cfg_int(cfg, "grid_m", 1);
    p.grid_n = get_cfg_int(cfg, "grid_n", 1);
    p.grid_k = get_cfg_int(cfg, "grid_k", 1);
    p.pe_m = get_cfg_int(cfg, "pe_m", 12);
    p.pe_n = get_cfg_int(cfg, "pe_n", 8);
    p.pe_k = get_cfg_int(cfg, "pe_k", 32);
    p.partial_sum_zero = get_cfg_bool(cfg, "partial_sum_zero");
    p.ultra_mode = get_cfg_bool(cfg, "ultra_mode");

    p.wave_m = get_cfg_int(cfg, "wave_m", 1);
    p.wave_n = get_cfg_int(cfg, "wave_n", 1);
    p.wave_k = get_cfg_int(cfg, "wave_k", 1);

    p.grid_m_per_wave = get_cfg_list_int(cfg, "grid_m_per_wave");
    p.grid_n_per_wave = get_cfg_list_int(cfg, "grid_n_per_wave");
    p.grid_k_per_wave = get_cfg_list_int(cfg, "grid_k_per_wave");
    return p;
}

size_t ceil_div(size_t a, size_t b) {
    return (b == 0) ? 0 : ((a + b - 1) / b);
}

struct WaveRange {
    size_t start = 0;
    size_t end = 0;
};

WaveRange get_wave_range(size_t total, int waves, int wave_idx) {
    if (waves <= 0) {
        return {0, total};
    }
    size_t chunk = ceil_div(total, static_cast<size_t>(waves));
    size_t start = static_cast<size_t>(wave_idx) * chunk;
    if (start >= total) {
        return {total, total};
    }
    size_t end = std::min(total, start + chunk);
    return {start, end};
}

WaveRange get_wave_tile_range(const std::vector<int>& tiles_per_wave, int wave_idx, size_t total_tiles, int fallback_waves) {
    if (tiles_per_wave.empty()) {
        return get_wave_range(total_tiles, fallback_waves, wave_idx);
    }
    if (wave_idx < 0 || static_cast<size_t>(wave_idx) >= tiles_per_wave.size()) {
        return {total_tiles, total_tiles};
    }
    size_t start = 0;
    for (int i = 0; i < wave_idx; ++i) {
        start += static_cast<size_t>(tiles_per_wave[i]);
    }
    size_t end = start + static_cast<size_t>(tiles_per_wave[wave_idx]);
    if (start > total_tiles) {
        start = total_tiles;
    }
    if (end > total_tiles) {
        end = total_tiles;
    }
    return {start, end};
}

size_t sum_tiles(const std::vector<int>& tiles) {
    size_t sum = 0;
    for (int v : tiles) {
        if (v > 0) {
            sum += static_cast<size_t>(v);
        }
    }
    return sum;
}

int max_tile_or_default(const std::vector<int>& tiles, int default_val) {
    if (tiles.empty()) {
        return default_val;
    }
    int max_val = 0;
    for (int v : tiles) {
        if (v > max_val) {
            max_val = v;
        }
    }
    return max_val;
}

} // namespace

// -----------------------------------------------------------------------------
// TestBench
class NoCSimTestBench : public sc_module {
public:
    // Parameters
    static constexpr size_t NUM_PORTS = 3;
    static constexpr size_t PORT_BANDWIDTH = 64;
    static constexpr size_t NUM_PES_PER_PORT = 16;
    static constexpr size_t TOTAL_PES = NUM_PORTS * NUM_PES_PER_PORT;
    static constexpr uint64_t MAX_WAIT_CYCLES = 10000;

    // Ports
    sc_clock clk;
    sc_signal<bool> reset_n;

    sc_signal<bool> command_mode;
    sc_signal<sc_uint<32>> command_data;

    // New Valid-Ready Signals (Triple Plane)
    VRDSIG<request_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>, uint16_t>> noc_ps_sig;  // NoC-PS
    VRDSIG<request_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>, uint16_t>> noc_pd_sig;  // NoC-PD
    VRDSIG<request_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>, uint16_t>> noc_pli_sig; // NoC-PLI (Local Network Plane - Write)
    VRDSIG<noc_addr_req_t>    noc_plo_sig; // NoC-PLO (Local Network Plane - Read)
    VRDSIG<response_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>>> noc_plo_resp_sig; // NoC-PLO Response

    // DUT
    NetworkOnChip<NUM_PORTS, PORT_BANDWIDTH, NUM_PES_PER_PORT> noc;

    // Test Data
    std::string test_data_dir;
    std::map<std::string, std::string> config;
    std::vector<int32_t> scan_chain_data;
    std::vector<fp16_t> input_activation;
    std::vector<fp16_t> input_weight;
    std::vector<fp16_t> input_partial_sum;
    std::vector<fp16_t> expected_output;
    std::vector<fp16_t> output_partial_sum;
    std::vector<uint16_t> pe_program;

    ConvParams conv;
    GEMMParams gemm;

    // Response handling
    struct RespMeta {
        uint64_t base_idx;
        bool     ultra;
        uint64_t per_port_stride; // stride between ports when ultra mode
        uint8_t  ports;

        // Provide streaming operator so sc_fifo::dump can print contents
        friend std::ostream& operator<<(std::ostream& os, const RespMeta& m) {
            os << "{base_idx=" << m.base_idx
               << ", ultra=" << (m.ultra ? "true" : "false")
               << ", per_port_stride=" << m.per_port_stride
               << ", ports=" << static_cast<int>(m.ports) << "}";
            return os;
        }
    };

    sc_fifo<RespMeta> rx_idx_fifo;
    float verify_tolerance;

    // Statistics
    uint64_t total_sent_bytes;
    uint64_t total_received_bytes;
    int total_macs;

    // Synchronization
    sc_event start_traffic_event;
    sc_event scan_chain_event;
    sc_event scan_chain_done_event;
    sc_event program_event;
    sc_event program_done_event;
    sc_event pe_start_event;
    sc_event pe_start_done_event;
    sc_event ps_done_event;
    sc_event pd_done_event;
    sc_event pli_done_event;
    sc_event plo_done_event;

    int clock_period_ns;

    SC_HAS_PROCESS(NoCSimTestBench);

    NoCSimTestBench(sc_module_name name, std::string data_dir, float verify_tolerance, int clock_period_ns)
        : sc_module(name),
          clk("clk", clock_period_ns, SC_NS),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          noc_ps_sig("noc_ps_sig"),
          noc_pd_sig("noc_pd_sig"),
          noc_pli_sig("noc_pli_sig"),
          noc_plo_sig("noc_plo_sig"),
          noc_plo_resp_sig("noc_plo_resp_sig"),
          noc("NoC_DUT", NetWorkOnChipConfig(4, 32)),
          test_data_dir(data_dir),
          rx_idx_fifo("rx_idx_fifo", 1024),
          verify_tolerance(verify_tolerance),
          total_sent_bytes(0),
          total_received_bytes(0),
          total_macs(0),
          clock_period_ns(clock_period_ns)
    {
        // Connect DUT
        noc.clk(clk);
        noc.reset_n(reset_n);
        noc.command_mode(command_mode);
        noc.command_data(command_data);

        // Connect Valid-Ready Interfaces
        connect_vr_signals(noc.noc_ps_in, noc_ps_sig);
        connect_vr_signals(noc.noc_pd_in, noc_pd_sig);
        connect_vr_signals(noc.noc_pli_in, noc_pli_sig);
        connect_vr_signals(noc.noc_plo_in, noc_plo_sig);
        connect_vr_signals(noc.noc_plo_out, noc_plo_resp_sig);

        SC_THREAD(test_main);

        SC_THREAD(response_sink); // Add sink to accept responses
        sensitive << clk.posedge_event();

        SC_THREAD(ps_sender);
        sensitive << clk.posedge_event();

        SC_THREAD(pd_sender);
        sensitive << clk.posedge_event();

        SC_THREAD(pli_sender);
        sensitive << clk.posedge_event();

        SC_THREAD(plo_sender);
        sensitive << clk.posedge_event();
    }

    /*   * @function load_test_data
     * @description Loads test data from binary files located in the specified test data directory.
     *              It reads scan chain data, input activations, weights, partial sums, expected output,
     *              and PE program instructions into corresponding member variables.
    */
    void load_test_data() {
        std::cout << "[TB] Loading test data from " << test_data_dir << std::endl;
        config = read_config_file(test_data_dir + "/config.txt");;

        scan_chain_data = read_binary_file<int32_t>(test_data_dir + "/scan_chain.bin");
        input_activation = read_binary_file<fp16_t>(test_data_dir + "/input_activation.bin");
        input_weight = read_binary_file<fp16_t>(test_data_dir + "/input_weight.bin");
        input_partial_sum = read_binary_file<fp16_t>(test_data_dir + "/input_partial_sum.bin");
        expected_output = read_binary_file<fp16_t>(test_data_dir + "/output_partial_sum.bin"); // gold output
        output_partial_sum.resize(expected_output.size(), 0); // Initialize output buffer
        pe_program = read_binary_file<uint16_t>(test_data_dir + "/pe_program.bin");

        std::cout << "[TB] Data loaded." << std::endl;
        std::cout << "  Scan Chain: " << scan_chain_data.size() << " entries" << std::endl;
        std::cout << "  Activation: " << input_activation.size() << " floats" << std::endl;
        std::cout << "  Weight: " << input_weight.size() << " floats" << std::endl;
        std::cout << "  PE program: " << pe_program.size() << " instructions" << std::endl;
    }

    void send_command(message_command_t cmd, uint32_t param = 0) {
        uint32_t cmd_data = (param & 0xFFFFFFF0) | (static_cast<uint32_t>(cmd) & 0x0F);
        command_mode.write(true);
        command_data.write(cmd_data);
        wait(clock_period_ns, SC_NS);
        command_mode.write(false);
    }

    bool wait_ready(sc_signal<bool>& ready_sig, const char* name, uint64_t timeout_cycles = MAX_WAIT_CYCLES) {
        for (uint64_t waited = 0; waited < timeout_cycles; ++waited) {
            wait(clk.negedge_event());
            if (ready_sig.read()) {
                wait(clk.posedge_event());
                return true;
            }
        }
        std::cerr << "[TB] ERROR: Timeout waiting ready on " << name << std::endl;
        return false;
    }

    void send_data_ps(uint16_t tag, const sc_biguint<256>& data_val, bool ultra_mode = false, size_t mask = 0xF) {
        request_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>, uint16_t> req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(tag) & 0x3F) | (ultra_mode ? 0x40 : 0x00);
        req.mask = mask;
        // req.is_w = true;

        noc_ps_sig.data_sig.write(req);
        noc_ps_sig.valid_sig.write(true);

        if (!wait_ready(noc_ps_sig.ready_sig, "noc_ps", MAX_WAIT_CYCLES * 100)) { // Allow longer timeout for PS which may have more processing
            noc_ps_sig.valid_sig.write(false);
            std::cerr << "[TB] req=" << req << std::endl;
            sc_stop();
            return;
        }

        noc_ps_sig.valid_sig.write(false);
        total_sent_bytes += (ultra_mode ? (8 * NUM_PORTS) : 8); // Ultra: NUM_PORTS * 64 bits; Normal: 64 bits
    }

    void send_data_pd(uint16_t tag, const sc_biguint<256>& data_val, bool ultra_mode = false, size_t mask = 0xF) {
        request_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>, uint16_t> req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(tag) & 0x3F) | (ultra_mode ? 0x40 : 0x00);
        req.mask = mask;

        noc_pd_sig.data_sig.write(req);
        noc_pd_sig.valid_sig.write(true);

        if (!wait_ready(noc_pd_sig.ready_sig, "noc_pd", MAX_WAIT_CYCLES * 100)) { // Allow longer timeout for PD which may have more processing
            noc_pd_sig.valid_sig.write(false);
            std::cerr << "[TB] req=" << req << std::endl;
            return;
        }

        noc_pd_sig.valid_sig.write(false);
        total_sent_bytes += (ultra_mode ? (8 * NUM_PORTS) : 8); // Ultra: NUM_PORTS * 64 bits; Normal: 64 bits
    }

    void send_data_pli(uint16_t tag, const sc_biguint<256>& data_val, bool ultra_mode = false, size_t mask = 0xF) {
        request_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>, uint16_t> req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(tag) & 0x3F) | (ultra_mode ? 0x40 : 0x00);
        req.mask = mask;
        // req.is_w = true; // NoC1 is always write

        noc_pli_sig.data_sig.write(req);
        noc_pli_sig.valid_sig.write(true);

        if (!wait_ready(noc_pli_sig.ready_sig, "noc_pli")) {
            noc_pli_sig.valid_sig.write(false);
            std::cerr << "[TB] req=" << req << std::endl;
            sc_stop();
            return;
        }

        noc_pli_sig.valid_sig.write(false);
        total_sent_bytes += (ultra_mode ? (8 * NUM_PORTS) : 8); // Ultra: NUM_PORTS * 64 bits; Normal: 64 bits
    }

    void recv_data_plo(uint16_t tag, uint64_t base_idx, bool ultra_mode = false, uint64_t per_port_stride = 0, uint8_t ports = NUM_PORTS) {
        // Send Read Request via NoC2
        noc_addr_req_t req;
        // req.data = 0; // No data for NoC2
        req.addr = (static_cast<uint16_t>(tag) & 0x3F) | (ultra_mode ? 0x40 : 0x00);
        // req.mask = 0;
        // req.is_w = false;

        noc_plo_sig.data_sig.write(req);
        noc_plo_sig.valid_sig.write(true);

        if (!wait_ready(noc_plo_sig.ready_sig, "noc_plo")) {
            noc_plo_sig.valid_sig.write(false);
            std::cerr << "[TB] req=" << req << std::endl;
            sc_stop();
            return;
        }

        noc_plo_sig.valid_sig.write(false);

        // Use blocking write to ensure we don't drop requests if FIFO is full
        RespMeta meta;
        meta.base_idx = base_idx;
        meta.ultra = ultra_mode;
        meta.per_port_stride = per_port_stride;
        meta.ports = ports;
        rx_idx_fifo.write(meta);

        total_received_bytes += (ultra_mode ? (8 * NUM_PORTS) : 8); // Ultra: NUM_PORTS * 64 bits; Normal: 64 bits
    }

    // Always accept responses
    void response_sink() {
        // wait for reset deassertion
        wait(start_traffic_event);
        while (true) {
            noc_plo_resp_sig.ready_sig.write(rx_idx_fifo.num_free() > 0);
            wait();
            if (noc_plo_resp_sig.valid_sig.read() && noc_plo_resp_sig.ready_sig.read()) {
                // receive response
                response_t<sc_biguint<NUM_PORTS*PORT_BANDWIDTH>> resp = noc_plo_resp_sig.data_sig.read();
                RespMeta meta;
                if(rx_idx_fifo.nb_read(meta)) {
                    // GEMM-specific handling: variable-length response mapping
                    if (config["mode"] == "gemm") {
                        // Compute dimensions and column offset from base_idx (base_idx = m * N + n)
                        size_t M = static_cast<size_t>(gemm.M);
                        size_t N = static_cast<size_t>(gemm.N);
                        size_t base = meta.base_idx;
                        // column index is base % N (since base = m * N + n)
                        size_t col = (N > 0) ? (base % N) : 0;


                        VERBOSE_LOG("[TB] Received response (GEMM): meta=" << meta << ", data=0x" << std::hex << resp.data << std::dec << ", status=" << static_cast<int>(resp.status));

                        if (!meta.ultra) {
                            // Limit to at most 4 words (one port returns 64 bits == 4x16)
                            size_t max_words = 4;
                            for (size_t w = 0; w < max_words; ++w) {
                                // For GEMM base = m * N + n; incrementing w moves across columns, so step by 1 in N
                                size_t idx = base + w * N;
                                if (idx >= output_partial_sum.size()) {
                                    std::cerr << "[TB] Warning: GEMM Received data index " << idx << " out of bounds!" << std::endl;
                                    continue;
                                }
                                sc_biguint<16> data_packet = resp.data.range(((w) * 16) + 15, (w) * 16);
                                fp16_t ps = static_cast<fp16_t>(data_packet.to_uint64());
                                output_partial_sum[idx] = ps;
                            }
                        } else {
                            // Ultra mode: treat each port chunk and place with per_port_stride
                            VERBOSE_LOG("[TB] GEMM Ultra response: ports=" << static_cast<int>(meta.ports) << ", per_port_stride=" << meta.per_port_stride);
                            for (uint8_t port = 0; port < meta.ports; ++port) {
                                sc_biguint<64> chunk = resp.data.range(64*port + 63, 64*port);
                                for (size_t och_offset = 0; och_offset < 4; ++och_offset) {
                                    // For GEMM ultra: each 16-bit word corresponds to an increment in M (row).
                                    // base = m * N + n, so advance by N for each row offset.
                                    size_t idx = base + static_cast<size_t>(port) * meta.per_port_stride + static_cast<size_t>(och_offset) * N;
                                    // For GEMM, ensure we don't cross column boundary
                                    size_t col_idx = (N > 0) ? (idx % N) : 0;
                                    if (idx >= output_partial_sum.size() || (N > 0 && col_idx < col && (idx != base))) {
                                        std::cerr << "[TB] Warning: GEMM Ultra Received data index " << idx << " out of bounds or invalid!" << std::endl;
                                        continue;
                                    }
                                    sc_biguint<16> data_packet = chunk.range(((och_offset) * 16) + 15, (och_offset) * 16);
                                    fp16_t ps = static_cast<fp16_t>(data_packet.to_uint64());
                                    output_partial_sum[idx] = ps;
                                }
                            }
                        }
                    } else { // Conv2D handling: fixed mapping based on output channel and spatial location
                        // Default: conv2d-style handling (fixed 4x16 in low 64 bits)
                        if (!meta.ultra) {
                            // Store received data (normal mode: resp contains 4x16bit in low 64 bits)
                            for (size_t och_offset = 0; och_offset < 4; ++och_offset) {
                                size_t idx = meta.base_idx + och_offset;
                                if (idx >= output_partial_sum.size()) {
                                    std::cerr << "[TB] Warning: Received data index " << idx << " out of bounds!" << std::endl;
                                    continue;
                                }
                                sc_biguint<16> data_packet = resp.data.range(((och_offset) * 16) + 15, (och_offset) * 16);
                                fp16_t ps = static_cast<fp16_t>(data_packet.to_uint64());
                                output_partial_sum[idx] = ps;
                            }
                        } else {
                            // Ultra mode: resp.data contains NUM_PORTS chunks of 64-bit each
                            for (uint8_t port = 0; port < meta.ports; ++port) {
                                sc_biguint<64> chunk = resp.data.range(64*port + 63, 64*port);
                                for (size_t och_offset = 0; och_offset < 4; ++och_offset) {
                                    size_t idx = meta.base_idx + static_cast<size_t>(port) * meta.per_port_stride + och_offset;
                                    if (idx >= output_partial_sum.size()) {
                                        std::cerr << "[TB] Warning: Ultra Received data index " << idx << " out of bounds!" << std::endl;
                                        continue;
                                    }
                                    sc_biguint<16> data_packet = chunk.range(((och_offset) * 16) + 15, (och_offset) * 16);
                                    fp16_t ps = static_cast<fp16_t>(data_packet.to_uint64());
                                    output_partial_sum[idx] = ps;
                                }
                            }
                        }
                    }
                } else {
                    std::cerr << "[TB] Warning: Received response but no index logged!" << std::endl;
                }
            }
        }
    }
    void configure_scan_chain() {
        std::cout << "[TB] Configuring Scan Chain..." << std::endl;
        ScanChainFormat sc_format;
        for (auto it = scan_chain_data.rbegin(); it != scan_chain_data.rend(); ++it) {
            sc_format = parse_scan_chain_data(static_cast<uint32_t>(*it));
            send_command(message_command_t::CMD_NOC_SCAN_CHAIN, static_cast<uint32_t>(*it));
        }
        wait(30, SC_NS); // Wait for configuration to take effect
    }

    void load_pe_program() {
        if (pe_program.empty()) {
            std::cerr << "[TB] No PE program loaded!" << std::endl;
            return;
        }
        std::cout << "[TB] Loading PE Program..." << std::endl;
        // Create load program command
        for (int i = 0; i < pe_program.size(); i++) {
            VERBOSE_LOG("[TB] Loading instruction " << i << ": 0x"
                      << std::hex << std::setw(4) << std::setfill('0') << pe_program[i] << std::dec);
            uint32_t param = 0;
            uint32_t pe_im_addr = static_cast<uint32_t>(i * sizeof(uint16_t)) & PE_ROUTER_IM_ADDR_MASK;
            uint32_t pe_im_data = static_cast<uint32_t>(pe_program[i]) & PE_ROUTER_IM_DATA_MASK;
            param |= (pe_im_addr << PE_ROUTER_IM_ADDR_OFFSET);  // Address
            param |= (pe_im_data << PE_ROUTER_IM_DATA_OFFSET); // Instruction data
            send_command(message_command_t::CMD_LOAD_PROGRAM, param);
        }
    }
    void ps_sender() {
        // Ensure reset is released before any bus activity
        while (!reset_n.read()) {
            wait(reset_n.value_changed_event());
        }

        // Step 1: configure scan-chain via command bus
        wait(scan_chain_event);
        configure_scan_chain();
        scan_chain_done_event.notify(SC_ZERO_TIME);

        // Step 2: load program via command bus
        wait(program_event);
        load_pe_program();
        program_done_event.notify(SC_ZERO_TIME);

        // Step 3: start PEs via command bus
        wait(pe_start_event);
        std::cout << "[TB] Starting PEs..." << std::endl;
        send_command(message_command_t::CMD_START_PE);
        pe_start_done_event.notify(SC_ZERO_TIME);

        // Step 4: stream PS data after traffic starts
        wait(start_traffic_event);
        std::string mode = config["mode"];
        if (mode == "conv2d") {
            distribute_conv2d_ps();
        } else if (mode == "gemm") {
            distribute_gemm_ps();
        }
        ps_done_event.notify(SC_ZERO_TIME);
    }

    void pd_sender() {
        wait(start_traffic_event);
        std::string mode = config["mode"];
        if (mode == "conv2d") {
            distribute_conv2d_pd();
        } else if (mode == "gemm") {
            distribute_gemm_pd();
        }
        pd_done_event.notify(SC_ZERO_TIME);
    }

    void pli_sender() {
        wait(start_traffic_event);
        std::string mode = config["mode"];
        if (mode == "conv2d") {
            distribute_conv2d_pli();
        } else if (mode == "gemm") {
            distribute_gemm_pli();
        }
        pli_done_event.notify(SC_ZERO_TIME);
    }

    void plo_sender() {
        wait(start_traffic_event);
        std::string mode = config["mode"];
        if (mode == "conv2d") {
            distribute_conv2d_plo();
        } else if (mode == "gemm") {
            distribute_gemm_plo();
        }
        plo_done_event.notify(SC_ZERO_TIME);
    }

    void distribute_conv2d_ps() {
        std::cout << "[TB-NoC-PS] Distributing Conv2D Weights (PS)..." << std::endl;

        int kernel_size = conv.kernel_size;
        int in_ch = conv.in_ch;
        int out_ch = conv.out_ch;
        int in_height = conv.in_height;
        int in_width = conv.in_width;
        bool ultra_mode = conv.ultra_mode;

        int wave_out_h = std::max(1, conv.temporal_wave_out_h);
        int wave_out_ch = std::max(1, conv.temporal_wave_out_ch);
        int wave_in_ch = std::max(1, conv.temporal_wave_in_ch);

        std::cout << "[TB-NoC-PS] Kernel Size: " << kernel_size << std::endl;
        std::cout << "[TB-NoC-PS] Input Channels: " << in_ch << std::endl;
        std::cout << "[TB-NoC-PS] Output Channels: " << out_ch << std::endl;
        std::cout << "[TB-NoC-PS] Input Height: " << in_height << std::endl;
        std::cout << "[TB-NoC-PS] Input Width: " << in_width << std::endl;

        // index helpers
        Index4D weight_idx(out_ch, kernel_size, 3, 4);
        Index3D act_idx(in_height, in_width, in_ch);

        size_t loop_in_height;
        if (ultra_mode) {
            loop_in_height = static_cast<size_t>(in_height) / NUM_PORTS; // Split height among ports
        } else {
            loop_in_height = static_cast<size_t>(in_height);
        }

        size_t loop_out_height = ultra_mode ? (static_cast<size_t>(conv.out_height) / NUM_PORTS)
                                            : static_cast<size_t>(conv.out_height);

        // Determine max channels per packet based on mode
        // Ultra Mode: 3 ports share NUM_PORTS*PORT_BANDWIDTH bits -> 64 bits/port -> 4 fp16 channels/port
        // Normal Mode: 1 port uses NUM_PORTS*PORT_BANDWIDTH bits -> 12 fp16 channels
        size_t channels_per_packet = ultra_mode ? 4 : 12;

        for (int wave_h = 0; wave_h < wave_out_h; ++wave_h) {
            WaveRange oh_range = get_wave_range(loop_out_height, wave_out_h, wave_h);
            if (oh_range.start >= oh_range.end) {
                continue;
            }

            size_t ih_start = oh_range.start * static_cast<size_t>(conv.stride);
            size_t ih_end = std::min(loop_in_height,
                                     oh_range.end * static_cast<size_t>(conv.stride) + static_cast<size_t>(kernel_size) - 1);

            for (int wave_oc = 0; wave_oc < wave_out_ch; ++wave_oc) {
                WaveRange och_range = get_wave_range(static_cast<size_t>(out_ch), wave_out_ch, wave_oc);
                if (och_range.start >= och_range.end) {
                    continue;
                }

                for (int wave_ic = 0; wave_ic < wave_in_ch; ++wave_ic) {
                    WaveRange ich_range = get_wave_range(static_cast<size_t>(in_ch), wave_in_ch, wave_ic);
                    if (ich_range.start >= ich_range.end) {
                        continue;
                    }

                    // 1. Weights (PS)
                    VERBOSE_LOG("[TB-NoC-PS] Sending Weights (wave h=" << wave_h
                              << " oc=" << wave_oc << " ic=" << wave_ic << ")");

                    for(size_t och = och_range.start; och < och_range.end; ++och) {
                        for(size_t kh = 0; kh < static_cast<size_t>(kernel_size); ++kh) {
                            for(size_t kw = 0; kw < 3; ++kw) { // Assume kernel width is 3 for simplicity
                                sc_biguint<256> data_packet = 0;
                                size_t mask = 0;
                                for(size_t ich = 0; ich < 4; ++ich) {
                                    size_t ich_global = ich_range.start + ich;
                                    if (ich_global >= static_cast<size_t>(in_ch) || ich_global >= 4) {
                                        continue;
                                    }
                                    size_t idx = weight_idx(och, kh, kw, ich_global);
                                    fp16_t w = input_weight[idx];
                                    data_packet.range((ich * 16) + 15, ich * 16) = w;
                                    mask |= (1 << ich);
                                }
                                if (mask == 0) {
                                    continue;
                                }
                                uint16_t tag = kh;
                                send_data_ps(tag, data_packet, false, mask);
                            }
                        }
                    }

                    VERBOSE_LOG("[TB-NoC-PS] Weights sent for wave h=" << wave_h
                              << " oc=" << wave_oc << " ic=" << wave_ic);

                }
            }
        }
    }

    void distribute_conv2d_pd() {
        std::cout << "[TB-NoC-PD] Distributing Conv2D Activations (PD)..." << std::endl;

        int kernel_size = conv.kernel_size;
        int in_ch = conv.in_ch;
        int in_height = conv.in_height;
        int in_width = conv.in_width;
        bool ultra_mode = conv.ultra_mode;

        int wave_out_h = std::max(1, conv.temporal_wave_out_h);
        int wave_out_ch = std::max(1, conv.temporal_wave_out_ch);
        int wave_in_ch = std::max(1, conv.temporal_wave_in_ch);

        // index helpers
        Index3D act_idx(in_height, in_width, in_ch);

        size_t loop_in_height;
        if (ultra_mode) {
            loop_in_height = static_cast<size_t>(in_height) / NUM_PORTS; // Split height among ports
        } else {
            loop_in_height = static_cast<size_t>(in_height);
        }

        size_t loop_out_height = ultra_mode ? (static_cast<size_t>(conv.out_height) / NUM_PORTS)
                                            : static_cast<size_t>(conv.out_height);

        // Determine max channels per packet based on mode
        size_t channels_per_packet = ultra_mode ? 4 : 12;

        for (int wave_h = 0; wave_h < wave_out_h; ++wave_h) {
            WaveRange oh_range = get_wave_range(loop_out_height, wave_out_h, wave_h);
            if (oh_range.start >= oh_range.end) {
                continue;
            }

            size_t ih_start = oh_range.start * static_cast<size_t>(conv.stride);
            size_t ih_end = std::min(loop_in_height,
                                        oh_range.end * static_cast<size_t>(conv.stride) + static_cast<size_t>(kernel_size) - 1);

            for (int wave_oc = 0; wave_oc < wave_out_ch; ++wave_oc) {
                for (int wave_ic = 0; wave_ic < wave_in_ch; ++wave_ic) {
                    WaveRange ich_range = get_wave_range(static_cast<size_t>(in_ch), wave_in_ch, wave_ic);
                    if (ich_range.start >= ich_range.end) {
                        continue;
                    }

                    VERBOSE_LOG("[TB-NoC-PD] Sending Activations (wave h=" << wave_h
                                << " oc=" << wave_oc << " ic=" << wave_ic << "), ultra_mode=" << ultra_mode << ")" );

                    for(size_t iw = 0; iw < static_cast<size_t>(in_width); ++iw) {
                        VERBOSE_LOG("[TB-NoC-PD] Processing input width index: " << iw << "/" << in_width);
                        for(size_t ih = ih_start; ih < ih_end; ++ih) {
                            // Iterate over channel chunks
                            for (size_t ich_base = ich_range.start; ich_base < ich_range.end; ich_base += channels_per_packet) {
                                sc_biguint<256> data_packet = 0;
                                size_t mask = 0;
                                size_t current_chunk_size = std::min(channels_per_packet, ich_range.end - ich_base);

                                if (ultra_mode) { // Ultra Mode: Each bus handles a chunk of the input height
                                    for(size_t ih_u = 0; ih_u < NUM_PORTS; ++ih_u) {
                                        for(size_t c_off = 0; c_off < current_chunk_size; ++c_off) {
                                            size_t ich = ich_base + c_off;
                                            if (ich >= static_cast<size_t>(in_ch)) {
                                                continue;
                                            }
                                            size_t base_h = ih_u * loop_in_height + ih;
                                            size_t idx = act_idx(base_h, iw, ich);
                                            fp16_t a = input_activation[idx];
                                            size_t slot_idx = ih_u * 4 + c_off;
                                            data_packet.range((slot_idx * 16) + 15, slot_idx * 16) = a;
                                            mask |= (1 << slot_idx);
                                        }
                                    }
                                } else { // Normal Mode
                                    for(size_t c_off = 0; c_off < current_chunk_size; ++c_off) {
                                        size_t ich = ich_base + c_off;
                                        if (ich >= static_cast<size_t>(in_ch)) {
                                            continue;
                                        }
                                        size_t idx = act_idx(ih, iw, ich);
                                        fp16_t a = input_activation[idx];
                                        size_t slot_idx = c_off;
                                        data_packet.range((slot_idx * 16) + 15, slot_idx * 16) = a;
                                        mask |= (1 << slot_idx);
                                    }
                                }

                                if (mask == 0) {
                                    continue;
                                }

                                uint16_t tag = (ih - ih_start);
                                send_data_pd(tag, data_packet, ultra_mode, mask);
                            }
                        }
                    }
                }
            }
        }
    }

    void distribute_conv2d_pli() {
        std::cout << "[TB-NoC-PLI] Distributing Conv2D Data (Partial Sums - Write)..." << std::endl;

        int out_ch = conv.out_ch;
        int out_height = conv.out_height;
        int out_width = conv.out_width;
        bool partial_sum_zero = conv.partial_sum_zero;

        int wave_out_h = std::max(1, conv.temporal_wave_out_h);
        int wave_out_ch = std::max(1, conv.temporal_wave_out_ch);
        int wave_in_ch = std::max(1, conv.temporal_wave_in_ch);

        size_t loop_out_height;
        bool ultra_mode = conv.ultra_mode;
        if (ultra_mode) {
            loop_out_height = static_cast<size_t>(out_height) / NUM_PORTS; // Split height among ports
        } else {
            loop_out_height = static_cast<size_t>(out_height);
        }

        // index helper for partial sums
        Index3D ps_idx(out_height, out_width, out_ch);

        for (int wave_h = 0; wave_h < wave_out_h; ++wave_h) {
            WaveRange oh_range = get_wave_range(loop_out_height, wave_out_h, wave_h);
            if (oh_range.start >= oh_range.end) {
                continue;
            }

            for (int wave_oc = 0; wave_oc < wave_out_ch; ++wave_oc) {
                WaveRange och_range = get_wave_range(static_cast<size_t>(out_ch), wave_out_ch, wave_oc);
                if (och_range.start >= och_range.end) {
                    continue;
                }

                for (int wave_ic = 0; wave_ic < wave_in_ch; ++wave_ic) {
                    if (wave_ic != 0) {
                        continue; // Only seed partial sums on the first in_ch wave
                    }

                    for(size_t ow = 0; ow < static_cast<size_t>(out_width); ++ow) {
                        // Partial Sum In (PLI) - Write via NoC1
                        VERBOSE_LOG("[TB-NoC-PLI] Sending Partial Sum In (PLI) for ow=" << ow
                                  << " (wave h=" << wave_h << " oc=" << wave_oc << "), ultra_mode=" << ultra_mode);
                        for(size_t oh = oh_range.start; oh < oh_range.end; ++oh) {
                            for(size_t och = och_range.start; och < och_range.end; och+=4) {
                                sc_biguint<256> data_packet = 0;
                                size_t mask = 0;
                                if (ultra_mode) {
                                    for(size_t oh_u = 0; oh_u < NUM_PORTS; ++oh_u) {
                                        for(size_t och_offset = 0; och_offset < 4; ++och_offset) {
                                            size_t och_idx = och + och_offset;
                                            if (och_idx >= static_cast<size_t>(out_ch) || och_idx >= och_range.end) {
                                                continue;
                                            }
                                            size_t base_h = oh_u * loop_out_height + oh;
                                            size_t idx = ps_idx(base_h, ow, och_idx);
                                            fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                                            size_t slot_idx = oh_u * 4 + och_offset;
                                            data_packet.range((slot_idx * 16) + 15, slot_idx * 16) = ps;
                                            mask |= (1 << slot_idx);
                                        }
                                    }
                                } else {
                                    for(size_t och_offset = 0; och_offset < 4; ++och_offset) {
                                        size_t och_idx = och + och_offset;
                                        if (och_idx >= static_cast<size_t>(out_ch) || och_idx >= och_range.end) {
                                            continue;
                                        }
                                        size_t idx = ps_idx(oh, ow, och_idx);
                                        fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                                        data_packet.range(((och_offset) * 16) + 15, (och_offset) * 16) = ps;
                                        mask |= (1 << och_offset);
                                    }
                                }

                                if (mask == 0) {
                                    continue;
                                }
                                uint16_t tag = (oh - oh_range.start);
                                send_data_pli(tag, data_packet, ultra_mode, mask);
                            }
                        }
                    }
                }
            }
        }
    }

    void distribute_conv2d_plo() {
        std::cout << "[TB-NoC-PLO] Distributing Conv2D Requests (Partial Sums - Read)..." << std::endl;

        int out_ch = conv.out_ch;
        int out_height = conv.out_height;
        int out_width = conv.out_width;

        int wave_out_h = std::max(1, conv.temporal_wave_out_h);
        int wave_out_ch = std::max(1, conv.temporal_wave_out_ch);
        int wave_in_ch = std::max(1, conv.temporal_wave_in_ch);

        size_t loop_out_height;
        bool ultra_mode = conv.ultra_mode;
        if (ultra_mode) {
            loop_out_height = static_cast<size_t>(out_height) / NUM_PORTS; // Split height among ports
        } else {
            loop_out_height = static_cast<size_t>(out_height);
        }

        // helper index for Partial Sums
        Index3D ps_idx(out_height, out_width, out_ch);

        uint64_t plo_per_port_stride = static_cast<uint64_t>(loop_out_height) * out_width * out_ch;

        for (int wave_h = 0; wave_h < wave_out_h; ++wave_h) {
            WaveRange oh_range = get_wave_range(loop_out_height, wave_out_h, wave_h);
            if (oh_range.start >= oh_range.end) {
                continue;
            }

            for (int wave_oc = 0; wave_oc < wave_out_ch; ++wave_oc) {
                WaveRange och_range = get_wave_range(static_cast<size_t>(out_ch), wave_out_ch, wave_oc);
                if (och_range.start >= och_range.end) {
                    continue;
                }

                for (int wave_ic = 0; wave_ic < wave_in_ch; ++wave_ic) {
                    if (wave_ic != wave_in_ch - 1) {
                        continue; // Only read after the final in_ch wave
                    }

                    for(size_t ow = 0; ow < static_cast<size_t>(out_width); ++ow) {
                        // Partial Sum Out (PLO) - Read via NoC2
                        VERBOSE_LOG("[TB-NoC-PLO] Receiving Partial Sum Out (PLO) for ow=" << ow
                                  << " (wave h=" << wave_h << " oc=" << wave_oc << "), ultra_mode=" << ultra_mode);
                        for(size_t oh = oh_range.start; oh < oh_range.end; ++oh) {
                            for(size_t och = och_range.start; och < och_range.end; och+=4) {
                                if (och >= static_cast<size_t>(out_ch)) {
                                    continue;
                                }
                                uint16_t tag = (oh - oh_range.start);
                                size_t base_idx = ps_idx(oh, ow, och);
                                recv_data_plo(tag, base_idx, ultra_mode, (ultra_mode ? plo_per_port_stride : 0), NUM_PORTS);
                            }
                        }
                    }
                }
            }
        }
    }

    void distribute_gemm_ps() {
        std::cout << "[TB-NoC-PS] Distributing GEMM Weights (PS)..." << std::endl;

        int K = gemm.K;
        int N = gemm.N;

        int grid_n = gemm.grid_n;

        // PE Tile Sizes (Software/Arch definition)
        int PE_N = gemm.pe_n;
        int PE_K = gemm.pe_k;

        // prepare index helpers
        Index2D weight_idx(K, N);

        // 1. Weights (Matrix B) -> Port Static (PS)
        bool ultra_mode = gemm.ultra_mode;
        int wave_k = gemm.grid_k_per_wave.empty() ? std::max(1, gemm.wave_k)
                               : static_cast<int>(gemm.grid_k_per_wave.size());
        int wave_n = gemm.grid_n_per_wave.empty() ? std::max(1, gemm.wave_n)
                               : static_cast<int>(gemm.grid_n_per_wave.size());

        for (int wave_k_idx = 0; wave_k_idx < wave_k; ++wave_k_idx) {
            WaveRange k_tile_range = get_wave_tile_range(gemm.grid_k_per_wave, wave_k_idx, static_cast<size_t>(gemm.grid_k), wave_k);
            size_t k_start = k_tile_range.start * static_cast<size_t>(PE_K);
            size_t k_end = std::min(static_cast<size_t>(K), k_tile_range.end * static_cast<size_t>(PE_K));
            if (k_start >= k_end) {
                continue;
            }

            for (int wave_n_idx = 0; wave_n_idx < wave_n; ++wave_n_idx) {
                WaveRange n_range = get_wave_tile_range(gemm.grid_n_per_wave, wave_n_idx, static_cast<size_t>(grid_n), wave_n);
                size_t n_start = n_range.start * static_cast<size_t>(PE_N);
                size_t n_end = std::min(static_cast<size_t>(N), n_range.end * static_cast<size_t>(PE_N));
                if (n_start >= n_end) {
                    continue;
                }

                if (ultra_mode) {
                    for (size_t k_offset = 0; k_offset < PE_K; ++k_offset) {
                        for (size_t n_base = 0; n_base < static_cast<size_t>(N); n_base += 4) {
                            size_t n_end_local = std::min(n_base + static_cast<size_t>(4), static_cast<size_t>(N));
                            if (n_end_local <= n_start || n_base >= n_end) {
                                continue;
                            }

                            sc_biguint<256> data_packet = 0;
                            size_t mask = 0;

                            for (size_t port = 0; port < NUM_PORTS; ++port) {
                                size_t k_tile = k_tile_range.start + port;
                                if (k_tile >= k_tile_range.end) {
                                    continue;
                                }
                                size_t k_global = k_tile * static_cast<size_t>(PE_K) + k_offset;

                                if (k_global < k_start || k_global >= k_end) {
                                    continue;
                                }

                                for (size_t n_offset = 0; n_offset < 4; ++n_offset) {
                                    size_t n = n_base + n_offset;
                                    if (n >= n_end_local || n < n_start || n >= n_end) {
                                        continue;
                                    }

                                    size_t idx = weight_idx(k_global, n);
                                    fp16_t w = input_weight[idx];

                                    int bit_offset = (port * 64) + (n_offset * 16);
                                    data_packet.range(bit_offset + 15, bit_offset) = w;
                                    mask |= (1 << n_offset);
                                }
                            }

                            if (mask == 0) {
                                continue;
                            }

                            int n_idx = static_cast<int>(n_base / PE_N);
                            uint16_t tag = static_cast<int>((n_base - n_start) / PE_N);

                            send_data_ps(tag, data_packet, true, mask);
                            VERBOSE_LOG("[TB-NoC-PS] Sent Weight Packet (ULTRA) - n_idx:" << n_idx
                                      << ", Tag: " << tag  << ", n_base:" << n_base << ", k_offset:" << k_offset);
                        }
                    }
                } else {
                    for (size_t k = k_start; k < k_end; ++k) {
                        for (size_t n_base = 0; n_base < static_cast<size_t>(N); n_base += 4) {
                            size_t n_end_local = std::min(n_base + static_cast<size_t>(4), static_cast<size_t>(N));
                            if (n_end_local <= n_start || n_base >= n_end) {
                                continue;
                            }

                            sc_biguint<256> data_packet = 0;
                            size_t mask = 0;
                            for (size_t n_offset = 0; n_offset < 4; ++n_offset) {
                                size_t n = n_base + n_offset;
                                if (n >= n_end_local || n < n_start || n >= n_end) {
                                    continue;
                                }
                                size_t idx = weight_idx(k, n);
                                fp16_t w = input_weight[idx];
                                data_packet.range((n_offset * 16) + 15, n_offset * 16) = w;
                                mask |= (1 << n_offset);
                            }

                            if (mask == 0) {
                                continue;
                            }

                            int n_idx = static_cast<int>(n_base / PE_N);
                            int k_idx = static_cast<int>(k / PE_K);

                            uint16_t tag = static_cast<int>(k_idx * grid_n + n_idx);
                            send_data_ps(tag, data_packet, false, mask);
                            VERBOSE_LOG("[TB-NoC-PS] Sent Weight Packet - n_idx:" << n_idx << ", k_idx:" << k_idx
                                    << ", Tag: " << tag  << ", n_base:" << n_base << ", k:" << k << ")");
                        }
                    }
                }
            }
        }
        std::cout << "[TB-NoC-PS] Weights Distribution Complete." << std::endl;
    }

    void distribute_gemm_pd() {
        std::cout << "[TB-NoC-PD] Distributing GEMM Activations (PD)..." << std::endl;

        int M = gemm.M;
        int K = gemm.K;

        int grid_m = gemm.grid_m;
        int grid_k = gemm.grid_k;
        int grid_n = gemm.grid_n;

        int PE_M = gemm.pe_m;
        int PE_K = gemm.pe_k;

        Index2D act_idx(M, K);

        bool ultra_mode = gemm.ultra_mode;
        int wave_k = gemm.grid_k_per_wave.empty() ? std::max(1, gemm.wave_k)
                       : static_cast<int>(gemm.grid_k_per_wave.size());
        int wave_n = gemm.grid_n_per_wave.empty() ? std::max(1, gemm.wave_n)
                       : static_cast<int>(gemm.grid_n_per_wave.size());
        int wave_m = gemm.grid_m_per_wave.empty() ? std::max(1, gemm.wave_m)
                       : static_cast<int>(gemm.grid_m_per_wave.size());
        for (int wave_k_idx = 0; wave_k_idx < wave_k; ++wave_k_idx) {
            WaveRange k_tile_range = get_wave_tile_range(gemm.grid_k_per_wave, wave_k_idx, static_cast<size_t>(gemm.grid_k), wave_k);
            size_t k_start = k_tile_range.start * static_cast<size_t>(PE_K);
            size_t k_end = std::min(static_cast<size_t>(K), k_tile_range.end * static_cast<size_t>(PE_K));
            if (k_start >= k_end) {
                continue;
            }

            for (int wave_n_idx = 0; wave_n_idx < wave_n; ++wave_n_idx) {
                WaveRange n_range = get_wave_tile_range(gemm.grid_n_per_wave, wave_n_idx, static_cast<size_t>(grid_n), wave_n);
                size_t n_tiles = n_range.end - n_range.start;

                int max_m_tiles = 0;
                if (ultra_mode && n_tiles > 0) {
                    max_m_tiles = static_cast<int>(NUM_PES_PER_PORT / n_tiles);
                }

                for (int wave_m_idx = 0; wave_m_idx < wave_m; ++wave_m_idx) {
                    WaveRange m_range = get_wave_tile_range(gemm.grid_m_per_wave, wave_m_idx, static_cast<size_t>(grid_m), wave_m);
                    size_t m_start = m_range.start * static_cast<size_t>(PE_M);
                    size_t m_end = std::min(static_cast<size_t>(M), m_range.end * static_cast<size_t>(PE_M));
                    if (m_start >= m_end) {
                        continue;
                    }

                    for (size_t k_offset = 0; k_offset < static_cast<size_t>(PE_K); k_offset++) {
                        if (ultra_mode) {
                            for (size_t m_base = m_start; m_base < m_end; m_base += 4) {
                                sc_biguint<256> data_packet = 0;

                                for (size_t port = 0; port < NUM_PORTS; ++port) {
                                    size_t k_tile = k_tile_range.start + port;
                                    if (k_tile >= k_tile_range.end) {
                                        continue;
                                    }
                                    size_t k_global = k_tile * static_cast<size_t>(PE_K) + k_offset;
                                    if (k_global < k_start || k_global >= k_end) {
                                        continue;
                                    }

                                    for (size_t m_offset = 0; m_offset < 4; ++m_offset) {
                                        size_t m = m_base + m_offset;
                                        if (m >= static_cast<size_t>(M)) break;
                                        size_t idx = act_idx(m, k_global);
                                        fp16_t a = input_activation[idx];

                                        int bit_offset = (port * 64) + (m_offset * 16);
                                        data_packet.range(bit_offset + 15, bit_offset) = a;
                                    }
                                }

                                int m_idx_local = static_cast<int>((m_base - m_start) / PE_M);
                                if (max_m_tiles > 0 && m_idx_local >= max_m_tiles) {
                                    std::cerr << "[TB-NoC-PD] Warning: wave-local m_idx out of range: " << m_idx_local << " (max " << max_m_tiles << ")" << std::endl;
                                    continue;
                                }
                                uint16_t tag = static_cast<uint16_t>(m_idx_local);

                                send_data_pd(tag, data_packet, true, 0xF);
                                VERBOSE_LOG("[TB-NoC-PD] Sent Activation Packet (ULTRA) - m_idx:" << m_idx_local
                                          << ", Tag: " << tag  << ", m_base:" << m_base << ", k:" << k_offset << ")");
                            }
                        } else {
                            for (size_t m_base = m_start; m_base < m_end; m_base += 4) {
                                for (int k_tile = 0; k_tile < grid_k; ++k_tile) {
                                    size_t k_global = static_cast<size_t>(k_tile) * PE_K + k_offset;
                                    if (k_global >= static_cast<size_t>(K)) {
                                        throw std::runtime_error("[TB-NoC-PD] Error: k_global exceeds K dimension!, k_global=" + std::to_string(k_global) + ", K=" + std::to_string(K) + ", k_tile=" + std::to_string(k_tile) + ", k_offset=" + std::to_string(k_offset));
                                    }
                                    if (k_global < k_start || k_global >= k_end) {
                                        continue;
                                    }

                                    sc_biguint<256> data_packet = 0;
                                    for (size_t m_offset = 0; m_offset < 4; ++m_offset) {
                                        size_t m = m_base + m_offset;
                                        if (m >= static_cast<size_t>(M)) break;
                                        size_t idx = act_idx(m, k_global);
                                        fp16_t a = input_activation[idx];
                                        data_packet.range((m_offset * 16) + 15, m_offset * 16) = a;
                                    }

                                    int m_idx = static_cast<int>(m_base / PE_M);
                                    int k_idx = k_tile;

                                    uint16_t tag = static_cast<uint16_t>(k_idx * grid_m + m_idx);
                                    send_data_pd(tag, data_packet, false, 0xF);
                                    VERBOSE_LOG("[TB-NoC-PD] Sent Activation Packet - m_idx:" << m_idx << ", k_idx:" << k_idx << ", Tag: " << tag  << ", m_base:" << m_base << ", k:" << k_offset << ")");
                                }
                            }
                        }
                    }
                }
            }
        }
        std::cout << "[TB-NoC-PD] Activations Distribution Complete." << std::endl;
    }

    void distribute_gemm_pli() {
        std::cout << "[TB-NoC-PLI] Distributing GEMM Data (Partial Sums Input)..." << std::endl;

        int M = gemm.M;
        int N = gemm.N;
        bool partial_sum_zero = gemm.partial_sum_zero;

        int grid_n = gemm.grid_n;
        int grid_k = gemm.grid_k;

        int PE_M = gemm.pe_m;
        int PE_N = gemm.pe_n;
        // int PE_K = 32;

        Index2D ps_idx(M, N);

        // Input Partial Sums -> PLI
        // pli_id = (m_idx * grid_n + n_idx) ONLY FOR k_idx == 0
        // Packets are D_mn. Since K is vertical, the D matrix enters at top K layer (k=0).
        // Tag should match pli_id.

        bool ultra_mode = gemm.ultra_mode;
        size_t rows_per_port = ultra_mode ? (M / NUM_PORTS) : M;
        int wave_k = gemm.grid_k_per_wave.empty() ? std::max(1, gemm.wave_k)
                               : static_cast<int>(gemm.grid_k_per_wave.size());
        int wave_n = gemm.grid_n_per_wave.empty() ? std::max(1, gemm.wave_n)
                               : static_cast<int>(gemm.grid_n_per_wave.size());
        int wave_m = gemm.grid_m_per_wave.empty() ? std::max(1, gemm.wave_m)
                               : static_cast<int>(gemm.grid_m_per_wave.size());

        for (int wave_k_idx = 0; wave_k_idx < wave_k; ++wave_k_idx) {
            if (wave_k_idx != 0) {
                continue;
            }

            for (int wave_n_idx = 0; wave_n_idx < wave_n; ++wave_n_idx) {
                WaveRange n_range = get_wave_tile_range(gemm.grid_n_per_wave, wave_n_idx, static_cast<size_t>(grid_n), wave_n);
                size_t n_start = n_range.start * static_cast<size_t>(PE_N);
                size_t n_end = std::min(static_cast<size_t>(N), n_range.end * static_cast<size_t>(PE_N));
                if (n_start >= n_end) {
                    continue;
                }

                size_t n_tiles = 0;
                int max_m_tiles = 0;
                if (ultra_mode) {
                    n_tiles = n_range.end - n_range.start;
                    if (n_tiles > 0) {
                        max_m_tiles = static_cast<int>(NUM_PES_PER_PORT / n_tiles);
                    }
                }

                for (int wave_m_idx = 0; wave_m_idx < wave_m; ++wave_m_idx) {
                    WaveRange m_range = get_wave_tile_range(gemm.grid_m_per_wave, wave_m_idx, static_cast<size_t>(gemm.grid_m), wave_m);
                    size_t m_start = m_range.start * static_cast<size_t>(PE_M);
                    size_t m_end = std::min(static_cast<size_t>(M), m_range.end * static_cast<size_t>(PE_M));
                    if (m_start >= m_end) {
                        continue;
                    }

                    for (size_t n = n_start; n < n_end; ++n) {

                    if (ultra_mode && grid_k > 1) {
                        // K-Split Ultra Mode: PS Input goes to Bus 0 (Port 0) only via ULTRA packet (Mask 1)
                        for (size_t m = m_start; m < m_end; m += 4) {
                            sc_biguint<256> data_packet = 0;
                            for (size_t m_offset = 0; m_offset < 4; ++m_offset) {
                                if (m + m_offset >= M) break;
                                size_t idx = ps_idx(m + m_offset, n);
                                fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                                data_packet.range((m_offset * 16) + 15, m_offset * 16) = ps;
                            }
                            int m_idx_local = static_cast<int>((m - m_start) / PE_M);
                            if (max_m_tiles > 0 && m_idx_local >= max_m_tiles) {
                                std::cerr << "[TB-NoC-PLI] Warning: wave-local m_idx out of range: " << m_idx_local
                                          << " (max " << max_m_tiles << ")" << std::endl;
                                continue;
                            }

                            int n_idx_local = static_cast<int>((n - n_start) / PE_N);
                            uint16_t tag = static_cast<uint16_t>(m_idx_local * n_tiles + n_idx_local);

                            send_data_pli(tag, data_packet, true, 0x1);
                            VERBOSE_LOG("[TB-NoC-PLI] Sent K-Split PS Input (ULTRA Port 0) - m:" << m << ", n:" << n << ", Tag:" << tag);
                        }
                    } else if (ultra_mode) {
                        // Send PLI per port region so each port receives its local rows
                        for (size_t port = 0; port < NUM_PORTS; ++port) {
                            size_t port_base = port * rows_per_port;
                            if (port_base >= static_cast<size_t>(M)) {
                                continue;
                            }
                            size_t m_local_start = (m_start > port_base) ? (m_start - port_base) : 0;
                            size_t m_local_end = (m_end > port_base) ? std::min(rows_per_port, m_end - port_base) : 0;
                            if (m_local_start >= m_local_end) {
                                continue;
                            }
                            for (size_t m_local = m_local_start; m_local < m_local_end; m_local += 4) {
                                sc_biguint<256> data_packet = 0;
                                for (size_t m_offset = 0; m_offset < 4; ++m_offset) {
                                    size_t m_global = port * rows_per_port + m_local + m_offset;
                                    if (m_global >= M) break;
                                    size_t idx = ps_idx(m_global, n);
                                    fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                                    data_packet.range((m_offset * 16) + 15, m_offset * 16) = ps;
                                }

                                int m_idx_local = static_cast<int>((m_local - m_local_start) / PE_M);
                                if (max_m_tiles > 0 && m_idx_local >= max_m_tiles) {
                                    std::cerr << "[TB-NoC-PLI] Warning: wave-local m_idx out of range: " << m_idx_local
                                              << " (max " << max_m_tiles << ")" << std::endl;
                                    continue;
                                }

                                int n_idx_local = static_cast<int>((n - n_start) / PE_N);
                                uint16_t tag = static_cast<uint16_t>(m_idx_local * n_tiles + n_idx_local);

                                // Send with ULTRA flag so router distributes across ports
                                send_data_pli(tag, data_packet, true, 0xF);
                            }
                        }
                    } else {
                        for (size_t m = m_start; m < m_end; m += 4) {
                            // Prepare packet
                            sc_biguint<256> data_packet = 0;
                            for (size_t m_offset = 0; m_offset < 4; ++m_offset) {
                                if (m + m_offset >= M) break;
                                size_t idx = ps_idx(m + m_offset, n);
                                fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                                data_packet.range((m_offset * 16) + 15, m_offset * 16) = ps;
                            }

                            int m_idx = m / PE_M;
                            int n_idx = n / PE_N;

                            uint16_t tag = m_idx * grid_n + n_idx;

                            send_data_pli(tag, data_packet);
                            VERBOSE_LOG("[TB-NoC-PLI] Sent Partial Sum Input Packet - m_idx: " << m_idx << ", n_idx: " << n_idx << ", m: " << m << ", n: " << n
                                      << ", Tag: " << tag << ")");
                        }
                    }
                    }
                }
            }
        }
    }

    void distribute_gemm_plo() {
        std::cout << "[TB-NoC-PLO] Distributing GEMM Requests (Partial Sums Output)..." << std::endl;

        int M = gemm.M;
        int N = gemm.N;

        int grid_n = gemm.grid_n;
        int grid_k = gemm.grid_k;
        int PE_M = gemm.pe_m;
        int PE_N = gemm.pe_n;

        Index2D ps_idx(M, N);

        bool ultra_mode = gemm.ultra_mode;
        size_t rows_per_port = ultra_mode ? (M / NUM_PORTS) : M;
        size_t per_port_stride = ultra_mode ? (rows_per_port * static_cast<size_t>(N)) : 0;
        int wave_k = gemm.grid_k_per_wave.empty() ? std::max(1, gemm.wave_k)
                               : static_cast<int>(gemm.grid_k_per_wave.size());
        int wave_n = gemm.grid_n_per_wave.empty() ? std::max(1, gemm.wave_n)
                               : static_cast<int>(gemm.grid_n_per_wave.size());
        int wave_m = gemm.grid_m_per_wave.empty() ? std::max(1, gemm.wave_m)
                               : static_cast<int>(gemm.grid_m_per_wave.size());

        for (int wave_k_idx = 0; wave_k_idx < wave_k; ++wave_k_idx) {
            if (wave_k_idx != (wave_k - 1)) {
                continue;
            }

            for (int wave_n_idx = 0; wave_n_idx < wave_n; ++wave_n_idx) {
                WaveRange n_range = get_wave_tile_range(gemm.grid_n_per_wave, wave_n_idx, static_cast<size_t>(grid_n), wave_n);
                size_t n_start = n_range.start * static_cast<size_t>(PE_N);
                size_t n_end = std::min(static_cast<size_t>(N), n_range.end * static_cast<size_t>(PE_N));
                if (n_start >= n_end) {
                    continue;
                }

                int max_m_tiles = 0;
                size_t n_tiles = 0;
                if (ultra_mode) {
                    n_tiles = n_range.end - n_range.start;
                    if (n_tiles > 0) {
                        max_m_tiles = static_cast<int>(NUM_PES_PER_PORT / n_tiles);
                    }
                }

                for (int wave_m_idx = 0; wave_m_idx < wave_m; ++wave_m_idx) {
                    WaveRange m_range = get_wave_tile_range(gemm.grid_m_per_wave, wave_m_idx, static_cast<size_t>(gemm.grid_m), wave_m);
                    size_t m_start = m_range.start * static_cast<size_t>(PE_M);
                    size_t m_end = std::min(static_cast<size_t>(M), m_range.end * static_cast<size_t>(PE_M));
                    if (m_start >= m_end) {
                        continue;
                    }

                    for (size_t n = n_start; n < n_end; ++n) {
                    // Output Partial Sums -> PLO (Read)
                    // plo_id = (m_idx * grid_n + n_idx) ONLY FOR k_idx == last

                    if (ultra_mode && grid_k > 1) {
                        // K-Split Ultra: Read from Bus 2 (Port 2) only.
                        // Standard packet expected.
                        for (size_t m = m_start; m < m_end; m += 4) {
                            int m_idx_local = static_cast<int>((m - m_start) / PE_M);
                            if (max_m_tiles > 0 && m_idx_local >= max_m_tiles) {
                                std::cerr << "[TB-NoC-PLO] Warning: wave-local m_idx out of range: " << m_idx_local
                                          << " (max " << max_m_tiles << ")" << std::endl;
                                continue;
                            }
                            int n_idx_local = static_cast<int>((n - n_start) / PE_N);
                            uint16_t tag = static_cast<uint16_t>(m_idx_local * n_tiles + n_idx_local);
                            size_t base_idx = ps_idx(m, n);

                            recv_data_plo(tag, base_idx);
                            VERBOSE_LOG("[TB-NoC-PLO] Requested K-Split PS Output (Port 2/Normal) - m:" << m << ", n:" << n << ", Tag:" << tag);
                        }
                    } else if (ultra_mode && grid_k == 1) {
                        // Request only the local base rows for port0; router will return NUM_PORTS chunks
                        for (size_t port = 0; port < NUM_PORTS; ++port) {
                            size_t port_base = port * rows_per_port;
                            if (port_base >= static_cast<size_t>(M)) {
                                continue;
                            }
                            size_t m_local_start = (m_start > port_base) ? (m_start - port_base) : 0;
                            size_t m_local_end = (m_end > port_base) ? std::min(rows_per_port, m_end - port_base) : 0;
                            if (m_local_start >= m_local_end) {
                                continue;
                            }
                            for (size_t m_local = m_local_start; m_local < m_local_end; m_local += 4) {
                                int m_idx_local = static_cast<int>((m_local - m_local_start) / PE_M);
                                if (max_m_tiles > 0 && m_idx_local >= max_m_tiles) {
                                    std::cerr << "[TB-NoC-PLO] Warning: wave-local m_idx out of range: " << m_idx_local
                                              << " (max " << max_m_tiles << ")" << std::endl;
                                    continue;
                                }
                                int n_idx_local = static_cast<int>((n - n_start) / PE_N);
                                uint16_t tag = static_cast<uint16_t>(n_idx_local);

                                size_t m_global = port_base + m_local;
                                size_t base_idx = ps_idx(m_global, n);
                                recv_data_plo(tag, base_idx, true, per_port_stride, NUM_PORTS);
                                VERBOSE_LOG("[TB-NoC-PLO] Requested Partial Sum Output Packet (ULTRA) - m_idx: " << m_idx_local << ", n_idx: " << n_idx_local
                                          << ", m_local: " << m_local << ", N base: " << n
                                          << ", Tag: " << tag << ", per_port_stride: " << per_port_stride << ")");
                            }
                        }
                    } else {
                        for (size_t m = m_start; m < m_end; m += 4) {
                            int m_idx_local = static_cast<int>((m - m_start) / PE_M);
                            if (max_m_tiles > 0 && m_idx_local >= max_m_tiles) {
                                std::cerr << "[TB-NoC-PLO] Warning: wave-local m_idx out of range: " << m_idx_local
                                          << " (max " << max_m_tiles << ")" << std::endl;
                                continue;
                            }
                            int n_idx_local = static_cast<int>((n - n_start) / PE_N);
                            uint16_t tag = static_cast<uint16_t>(m_idx_local * n_tiles + n_idx_local);

                            size_t base_idx = ps_idx(m, n);
                            recv_data_plo(tag, base_idx);
                            VERBOSE_LOG("[TB-NoC-PLO] Requested Partial Sum Output Packet - m_idx: " << m_idx_local << ", n_idx: " << n_idx_local
                                      << ", m: " << m << ", N base: " << n
                                      << ", Tag: " << tag << ")");
                        }
                    }
                    }
                }
            }
        }
    }

    /*
     * @function check_results
     * @description Compares the expected output with the actual output from the NoC simulation.
     *              It calculates the cosine similarity, maximum difference, and mean squared error (MSE)
     *              between the two outputs. It also counts the number of mismatches that exceed a specified tolerance.
     * @param expected - The expected output data as a vector of fp16_t.
     * @param actual - The actual output data from the NoC simulation as a vector of fp16_t.
     * @param tolerance - The acceptable difference between expected and actual values.
     */
    void check_results(const std::vector<fp16_t>& expected, const std::vector<fp16_t>& actual, double tolerance) {
        size_t errors = 0;
        // Calculate cosine similarity and max difference
        double dot_product = 0.0;
        double magnitude_received = 0.0;
        double magnitude_expected = 0.0;
        double max_diff = 0.0;
        double total_squared_error = 0.0;

        for (size_t i = 0; i < expected.size(); i++) {
            float received_val = fp16_to_float(actual[i]);
            float expected_val = fp16_to_float(expected[i]);

            dot_product += received_val * expected_val;
            magnitude_received += received_val * received_val;
            magnitude_expected += expected_val * expected_val;

            double diff = std::abs(received_val - expected_val);
            total_squared_error += diff * diff;

            if (diff > max_diff) {
                max_diff = diff;
            }
            if (diff > tolerance) {
                errors++;
                if (errors <= 60000) {
                    std::cout << "Mismatch at index " << i
                              << ": received=0x" << std::hex << actual[i]
                              << " expected=0x" << expected[i] << std::dec
                              << " diff=" << std::scientific << diff << std::dec <<std::endl;
                }
            }
        }

        magnitude_received = std::sqrt(magnitude_received);
        magnitude_expected = std::sqrt(magnitude_expected);

        double cosine_similarity = dot_product / (magnitude_received * magnitude_expected);
        double mse = total_squared_error / expected.size();

        std::cout << "----------------------------------------" << std::endl;
        std::cout << "Verification Results:" << std::endl;
        std::cout << "  Total Elements: " << expected.size() << std::endl;
        std::cout << "  Mismatches: " << errors << " (Tolerance: " << tolerance << ")" << std::endl;
        std::cout << "  Cosine Similarity: " << cosine_similarity << std::endl;
        std::cout << "  Max Difference: " << std::scientific << max_diff << std::dec << std::endl;
        std::cout << "  MSE: " << std::scientific << mse << std::dec << std::endl;
        std::cout << "----------------------------------------" << std::endl;

        if (cosine_similarity > 0.99) {
            std::cout << "✓ Outputs are highly similar! Test PASSED." << std::endl;
        } else {
            std::cout << "✗ Outputs are not similar enough. Test FAILED." << std::endl;
        }
    }

    /**     * @function print_statistics
     * @description Gathers and prints various performance statistics of the NoC simulation,
     *              including average instruction count, cycle count, IPC for PEs,
     *              total NoC cycle count, clock rate, data movement, throughput, and performance in MACs per second.
     */

    void print_statistics() {
        std::cout << "========================================" << std::endl;
        std::cout << "              Statistics                " << std::endl;
        std::cout << "========================================" << std::endl;

        // PE average stats
        uint64_t active_pe = 0;
        uint64_t total_instr = 0;
        uint64_t total_cycles = 0;
        double total_ipc = 0.0;
        for(size_t p = 0; p < NUM_PORTS; ++p) {
            for(size_t q = 0; q < NUM_PES_PER_PORT; ++q) {
                // check if PE is active (enabled)
                if (!noc.router_enable[p][q].read()) {
                    continue;
                }

                // read stats
                int64_t instr_count = noc.pes[p][q].instr_count_reg.read();
                uint64_t cycles = noc.pes[p][q].cycles_reg.read();
                double ipc = (cycles > 0) ? static_cast<double>(instr_count) / cycles : 0.0;
                total_instr += instr_count;
                total_cycles += cycles;
                total_ipc += ipc;
                active_pe++;
            }
        }
        double avg_ipc = total_ipc / active_pe;
        std::cout << "PEs Average instruction count: " << total_instr / active_pe << std::endl;
        std::cout << "PEs Average cycle count: " << total_cycles / active_pe << std::endl;
        std::cout << "PEs Average IPC: " << avg_ipc << std::endl;

        // Total NoC cycle count
        // Clock period is 10 ns.
        int ns_per_sec = 1000000000;
        double total_cycles_d = sc_time_stamp().to_seconds() * ns_per_sec / clock_period_ns;
        std::cout << "Total NoC cycle count: " << (uint64_t)total_cycles_d << std::endl;

        // Clock rate
        std::cout << "Clock rate: " << num_to_str(ns_per_sec / clock_period_ns, 1000) << "Hz" << std::endl;

        // Total NoC data movement
        std::cout << "Total NoC data movement: " << num_to_str(total_sent_bytes + total_received_bytes) << "B" << std::endl;

        // Throughput
        double total_time_sec = sc_time_stamp().to_seconds();
        double throughput = (total_sent_bytes + total_received_bytes) / total_time_sec;
        std::cout << "NoC Throughput: " << num_to_str(static_cast<uint64_t>(throughput)) << "B/s" << std::endl;

        // Performance (MACs per second, 2FLOPS equivalent 1 MAC)
        double macs_per_sec = total_macs / total_time_sec;
        std::cout << "Performance (MACs per second): " << num_to_str(static_cast<uint64_t>(macs_per_sec)) << "MACs/s"
        << "(" << num_to_str(static_cast<uint64_t>(macs_per_sec * 2)) << "FLOPS)" << std::endl;

        // Arithmetic Intensity
        double arithmetic_intensity = static_cast<double>(total_macs) / (total_sent_bytes + total_received_bytes);
        std::cout << "Arithmetic Intensity: " << arithmetic_intensity << " MACs/Byte" << std::endl;

        std::cout << "========================================" << std::endl;

    }

    void test_main() {
        // 1. Initialize
        reset_n.write(false);
        wait(clock_period_ns * 3, SC_NS);
        reset_n.write(true);
        wait(clock_period_ns, SC_NS);

        // 2. Load Data
        load_test_data();

        if (config.count("mode")) {
            if (config["mode"] == "conv2d") {
                conv = parse_conv_params_from_config(config);
                if (conv.kernel_size <= 0 || conv.in_ch <= 0 || conv.out_ch <= 0 || conv.in_height <= 0 || conv.in_width <= 0 || conv.out_height <= 0 || conv.out_width <= 0) {
                    std::cerr << "[TB] Invalid conv2d config in config.txt" << std::endl;
                    sc_stop();
                    return;
                }
            }
            else if (config["mode"] == "gemm") {
                gemm = parse_gemm_params_from_config(config);
                if (gemm.M <= 0 || gemm.K <= 0 || gemm.N <= 0) {
                    std::cerr << "[TB] Invalid gemm config in config.txt" << std::endl;
                    sc_stop();
                    return;
                }

                if (!gemm.grid_m_per_wave.empty() && sum_tiles(gemm.grid_m_per_wave) != static_cast<size_t>(gemm.grid_m)) {
                    std::cerr << "[TB] Invalid gemm config: grid_m_per_wave sum mismatch" << std::endl;
                    sc_stop();
                    return;
                }
                if (!gemm.grid_n_per_wave.empty() && sum_tiles(gemm.grid_n_per_wave) != static_cast<size_t>(gemm.grid_n)) {
                    std::cerr << "[TB] Invalid gemm config: grid_n_per_wave sum mismatch" << std::endl;
                    sc_stop();
                    return;
                }
                if (!gemm.grid_k_per_wave.empty() && sum_tiles(gemm.grid_k_per_wave) != static_cast<size_t>(gemm.grid_k)) {
                    std::cerr << "[TB] Invalid gemm config: grid_k_per_wave sum mismatch" << std::endl;
                    sc_stop();
                    return;
                }

                if (gemm.ultra_mode && gemm.grid_n > 0) {
                    int max_n_tiles = max_tile_or_default(gemm.grid_n_per_wave, gemm.grid_n);
                    int max_m_tiles = (max_n_tiles > 0) ? static_cast<int>(NUM_PES_PER_PORT / max_n_tiles) : 0;
                    if (!gemm.grid_m_per_wave.empty()) {
                        for (size_t i = 0; i < gemm.grid_m_per_wave.size(); ++i) {
                            if (max_m_tiles > 0 && gemm.grid_m_per_wave[i] > max_m_tiles) {
                                std::cerr << "[TB] Invalid gemm config: grid_m_per_wave exceeds per-port capacity" << std::endl;
                                sc_stop();
                                return;
                            }
                        }
                    }
                }
            }
        }

        // 3. Configure NoC (via events)
        scan_chain_event.notify(SC_ZERO_TIME);
        wait(scan_chain_done_event);
        noc.dump_state();

        // 4. Load Program (via events)
        program_event.notify(SC_ZERO_TIME);
        wait(program_done_event);

        // 5. Start PEs (via events)
        pe_start_event.notify(SC_ZERO_TIME);
        wait(pe_start_done_event);

        // 6. Distribute Data (Parallel)
        // Calculate MACs for stats
        std::string mode = config["mode"];
        if (mode == "conv2d") {
            int kernel_size = conv.kernel_size;
            int in_ch = conv.in_ch;
            int out_ch = conv.out_ch;
            int out_height = conv.out_height;
            int out_width = conv.out_width;
            total_macs = out_ch * out_height * out_width * kernel_size * kernel_size * in_ch;
        } else if (mode == "gemm") {
            total_macs = gemm.M * gemm.N * gemm.K;
        }

        std::cout << "[TB] Starting Parallel Data Distribution..." << std::endl;
        start_traffic_event.notify(SC_ZERO_TIME);

        // 7. Wait for completion
        std::cout << "[TB] Running Simulation..." << std::endl;

        // Wait for all sender threads to finish with timeout
        sc_time start_wait = sc_time_stamp();
        sc_time timeout = sc_time(500, SC_MS);
        wait(timeout, ps_done_event & pd_done_event & pli_done_event & plo_done_event);

        if (sc_time_stamp() - start_wait >= timeout) {
             std::cerr << "[TB] ERROR: Simulation Timed Out waiting for NoC completion!" << std::endl;
             sc_stop();
             return;
        }

        // Wait until all expected responses are received (rx_idx_fifo should be empty)
        uint64_t waited_cycles = 0;
        while (rx_idx_fifo.num_available() > 0 && waited_cycles < MAX_WAIT_CYCLES) {
            wait(clk.posedge_event());
            waited_cycles++;
        }
        if (rx_idx_fifo.num_available() > 0) {
            std::cerr << "[TB] ERROR: Timeout waiting for responses to drain (remaining="
                      << rx_idx_fifo.num_available() << ")" << std::endl;
            sc_stop();
            return;
        }

        // 8. Check Results
        std::cout << "[TB] Checking Results..." << std::endl;
        check_results(expected_output, output_partial_sum, verify_tolerance);

        // 9. Stop Simulation
        std::cout << "[TB] Simulation Finished." << std::endl;
        sc_stop();

        // 10. statistics
        print_statistics();
    }
};

void print_usage() {
    std::cout << "Usage: ./test_noc_sim [options] <data_dir>" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -c <clock_period_ns>   Set clock period in nanoseconds (default: 10)" << std::endl;
    std::cout << "  -t <tolerance>   Set verification tolerance (default: 0.02)" << std::endl;
    std::cout << "  -f <trace_file>  Set trace file path (default: trace.json)" << std::endl;
    std::cout << "  -v               Enable verbose logging" << std::endl;
    std::cout << "  <data_dir>       Path to the test data directory (default: output/noc/conv2d)" << std::endl;
}

int sc_main(int argc, char* argv[]) {
    // Default parameters
    std::string data_dir = "output/noc/conv2d";
    std::string trace_file = "trace.json";
    bool enable_trace = false;
    float verify_tolerance = 0.02f;
    int clock_period_ns = 10;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" && i + 1 < argc) {
            verify_tolerance = std::stof(argv[++i]);
        } else if (arg == "-c" && i + 1 < argc) {
            clock_period_ns = std::stoi(argv[++i]);
        } else if (arg == "-f" && i + 1 < argc) {
            trace_file = argv[++i];
            enable_trace = true;
        } else if (arg == "-v") {
           enable_verbose_logging(true); // Enable verbose logging
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else {
            data_dir = arg;
        }
    }

    // Instantiate TestBench
    NoCSimTestBench tb("NoCSimTestBench", data_dir, verify_tolerance, clock_period_ns);

    if (enable_trace) {
        tb.noc.enable_perffeto_trace(); // Enable tracing in the NetworkOnChip
        // Open trace file
        PerfettoTrace::getInstance().open(trace_file);
    }



    sc_start();

    if (enable_trace) {
        // Close trace file
        PerfettoTrace::getInstance().close();
    }

    return 0;
}
