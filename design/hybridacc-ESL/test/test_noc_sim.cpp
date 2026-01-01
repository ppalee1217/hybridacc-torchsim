#include <systemc>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <iomanip>
#include <filesystem>
#include <functional>
#include "NetworkOnChip.hpp"
#include "NoC/NoCRouter.hpp" // Include for router_req_t/resp_t
#include "utils.hpp"
#include "tb_utils.hpp"

using namespace hybridacc;
using namespace sc_core;
using namespace sc_dt;
namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// TestBench
class NoCSimTestBench : public sc_module {
public:
    // Ports
    sc_clock clk;
    sc_signal<bool> reset_n;

    sc_signal<bool> command_mode;
    sc_signal<sc_uint<32>> command_data;

    // New Valid-Ready Signals (Dual Plane)
    VRDSIG<noc::router_req_t> req0_sig; // NoC-0 (Push Plane)
    VRDSIG<noc::router_req_t> req1_sig; // NoC-1 (Local Network Plane)
    VRDSIG<noc::router_resp_t> resp1_sig; // NoC-1 Response

    // DUT
    NetworkOnChip noc;

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

    // Response handling
    sc_fifo<uint64_t> rx_idx_fifo;
    float verify_tolerance;

    // Statistics
    uint64_t total_sent_bytes;
    uint64_t total_received_bytes;
    int total_macs;

    // Synchronization
    sc_event start_traffic_event;
    sc_event noc0_done_event;
    sc_event noc1_done_event;

    // Parameters
    static constexpr size_t NUM_PORTS = 4;
    static constexpr size_t NUM_PES_PER_PORT = 16;
    static constexpr size_t TOTAL_PES = NUM_PORTS * NUM_PES_PER_PORT;
    int clock_period_ns;

    SC_HAS_PROCESS(NoCSimTestBench);

    NoCSimTestBench(sc_module_name name, std::string data_dir, float verify_tolerance, int clock_period_ns)
        : sc_module(name),
          clk("clk", clock_period_ns, SC_NS),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          req0_sig("req0_sig"),
          req1_sig("req1_sig"),
          resp1_sig("resp1_sig"),
          noc("NoC_DUT", NUM_PORTS, NUM_PES_PER_PORT),
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
        connect_vr_signals(noc.req0_in, req0_sig);
        connect_vr_signals(noc.req1_in, req1_sig);
        connect_vr_signals(noc.resp1_out, resp1_sig);

        SC_THREAD(test_main);

        SC_THREAD(response_sink); // Add sink to accept responses
        sensitive << clk.posedge_event();

        SC_THREAD(noc0_thread);
        sensitive << clk.posedge_event();

        SC_THREAD(noc1_thread);
        sensitive << clk.posedge_event();
    }

    /*   * @function load_test_data
     * @description Loads test data from binary files located in the specified test data directory.
     *              It reads scan chain data, input activations, weights, partial sums, expected output,
     *              and PE program instructions into corresponding member variables.
    */
    void load_test_data() {
        std::cout << "[TB] Loading test data from " << test_data_dir << std::endl;
        config = read_config_file(test_data_dir + "/config.txt");

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

    void send_data_noc0(uint16_t channel, uint16_t tag, const sc_biguint<256>& data_val, size_t mask = 0xF) {
        noc::router_req_t req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag));
        req.mask = mask;
        req.is_w = true;

        req0_sig.data_sig.write(req);
        req0_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req0_sig.ready_sig.read() == false);

        req0_sig.valid_sig.write(false);
        total_sent_bytes += 32; // 256 bits = 32 bytes
    }

    void send_data_noc1(uint16_t channel, uint16_t tag, const sc_biguint<256>& data_val, size_t mask = 0xF) {
        noc::router_req_t req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag));
        req.mask = mask;
        req.is_w = true;

        req1_sig.data_sig.write(req);
        req1_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req1_sig.ready_sig.read() == false);

        req1_sig.valid_sig.write(false);
        total_sent_bytes += 32; // 256 bits = 32 bytes
    }

    void recv_data_noc1(uint16_t& channel, uint16_t& tag, uint64_t base_idx) {
        // Send Read Request
        noc::router_req_t req;
        req.data = 0;
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag));
        req.mask = 0;
        req.is_w = false;

        req1_sig.data_sig.write(req);
        req1_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req1_sig.ready_sig.read() == false);

        req1_sig.valid_sig.write(false);

        // Use blocking write to ensure we don't drop requests if FIFO is full
        rx_idx_fifo.write(base_idx);

        total_sent_bytes += 32; // 256 bits = 32 bytes
    }

    // Always accept responses
    void response_sink() {
        // wait for reset deassertion
        wait(start_traffic_event);
        while (true) {
            resp1_sig.ready_sig.write(rx_idx_fifo.num_free() > 0);
            wait();
            if (resp1_sig.valid_sig.read() && resp1_sig.ready_sig.read()) {
                // receive response
                noc::router_resp_t resp = resp1_sig.data_sig.read();
                uint64_t base_idx;
                if(rx_idx_fifo.nb_read(base_idx)) {
                    // Store received data
                    for (size_t och_offset = 0; och_offset < 4; ++och_offset) {
                        size_t idx = base_idx + och_offset;
                        if (idx >= output_partial_sum.size()) {
                            std::cerr << "[TB] Warning: Received data index " << idx << " out of bounds!" << std::endl;
                            continue;
                        }
                        sc_biguint<16> data_packet = resp.data.range(((och_offset) * 16) + 15, (och_offset) * 16);
                        fp16_t ps = static_cast<fp16_t>(data_packet.to_uint64());
                        output_partial_sum[idx] = ps;
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
            std::cout << "[TB] Loading instruction " << i << ": 0x"
                      << std::hex << std::setw(4) << std::setfill('0') << pe_program[i] << std::dec << std::endl;
            uint32_t param = 0;
            param |= (static_cast<uint32_t>(i * sizeof(uint16_t)) << 4);  // Address
            param |= (static_cast<uint32_t>(pe_program[i]) << 16); // Instruction data
            send_command(message_command_t::CMD_LOAD_PROGRAM, param);
        }
    }

    void noc0_thread() {
        while(true) {
            wait(start_traffic_event);
            std::string mode = config["mode"];
            if (mode == "conv2d") {
                distribute_conv2d_noc0();
            } else if (mode == "gemm") {
                distribute_gemm_noc0();
            }
            noc0_done_event.notify();
        }
    }

    void noc1_thread() {
        while(true) {
            wait(start_traffic_event);
            std::string mode = config["mode"];
            if (mode == "conv2d") {
                distribute_conv2d_noc1();
            } else if (mode == "gemm") {
                distribute_gemm_noc1();
            }
            noc1_done_event.notify();
        }
    }

    void distribute_conv2d_noc0() {
        std::cout << "[TB-NoC0] Distributing Conv2D Data (Weights & Activations)..." << std::endl;

        int kernel_size = std::stoi(config["kernel_size"]);
        int in_ch = std::stoi(config["in_ch"]);
        int out_ch = std::stoi(config["out_ch"]);
        int in_height = std::stoi(config["in_height"]);
        int in_width = std::stoi(config["in_width"]);

        std::cout << "[TB-NoC0] Kernel Size: " << kernel_size << std::endl;
        std::cout << "[TB-NoC0] Input Channels: " << in_ch << std::endl;
        std::cout << "[TB-NoC0] Output Channels: " << out_ch << std::endl;
        std::cout << "[TB-NoC0] Input Height: " << in_height << std::endl;
        std::cout << "[TB-NoC0] Input Width: " << in_width << std::endl;

        // 1. Weights (PS)
        std::cout << "[TB-NoC0] Sending Weights..." << std::endl;
        for(size_t och = 0; och < out_ch; ++och) {
            for(size_t kh = 0; kh < kernel_size; ++kh) {
                for(size_t kw = 0; kw < kernel_size; ++kw) {
                    sc_biguint<256> data_packet = 0;
                    for(size_t ich = 0; ich < in_ch; ++ich) {
                        size_t idx = och * kernel_size * kernel_size * in_ch +
                                     kh * kernel_size * in_ch +
                                     kw * in_ch + ich;
                        fp16_t w = input_weight[idx];
                        data_packet.range((ich * 16) + 15, ich * 16) = w;
                    }
                    uint16_t channel = NOC_CHANNEL_PS; // PS
                    uint16_t tag = kh;
                    send_data_noc0(channel, tag, data_packet);
                }
            }
        }


        // 2. Activations (PD)
        std::cout << "[TB-NoC0] Sending Activations..." << std::endl;
        for(size_t iw = 0; iw < in_width; ++iw) {
            std::cout << "[TB-NoC0] Processing input width index: " << iw << "/" << in_width << std::endl;
            for(size_t ih = 0; ih < in_height; ++ih) {
                sc_biguint<256> data_packet = 0;
                for(size_t ich = 0; ich < in_ch; ++ich) {
                    size_t idx = ih * in_width * in_ch +
                                 iw * in_ch + ich;
                    fp16_t a = input_activation[idx];
                    data_packet.range((ich * 16) + 15, ich * 16) = a;
                }
                uint16_t channel = NOC_CHANNEL_PD; // PD
                uint16_t tag = ih;
                send_data_noc0(channel, tag, data_packet, 0xF);
            }
        }
    }

    void distribute_conv2d_noc1() {
        std::cout << "[TB-NoC1] Distributing Conv2D Data (Partial Sums)..." << std::endl;

        int kernel_size = std::stoi(config["kernel_size"]);
        int stride = std::stoi(config["stride"]);
        int out_ch = std::stoi(config["out_ch"]);
        int out_height = std::stoi(config["out_height"]);
        int out_width = std::stoi(config["out_width"]);
        bool partial_sum_zero = (config["partial_sum_zero"] == "True");

        std::cout << "[TB-NoC1] Kernel Size: " << kernel_size << std::endl;
        std::cout << "[TB-NoC1] Stride: " << stride << std::endl;
        std::cout << "[TB-NoC1] Output Channels: " << out_ch << std::endl;
        std::cout << "[TB-NoC1] Output Height: " << out_height << std::endl;
        std::cout << "[TB-NoC1] Output Width: " << out_width << std::endl;

        for(size_t ow = 0; ow < out_width; ++ow) {
            // Partial Sum In (PLI)
            std::cout << "[TB-NoC1] Sending Partial Sum In (PLI) for ow=" << ow << std::endl;
            for(size_t oh = 0; oh < out_height; ++oh) {
                for(size_t och = 0; och < out_ch; och+=4) {
                    sc_biguint<256> data_packet = 0;
                    for(size_t och_offset = 0; och_offset < 4; ++och_offset) {
                        size_t idx = oh * out_width * out_ch +
                                     ow * out_ch + (och + och_offset);
                        fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                        data_packet.range(((och_offset) * 16) + 15, (och_offset) * 16) = ps;
                    }
                    uint16_t channel = NOC_CHANNEL_PLI; // PLI (NoC1 Channel 0)
                    uint16_t tag = oh;
                    send_data_noc1(channel, tag, data_packet);
                }
            }

            //wait(5000, SC_NS); // Small delay between PLI and PLO

            // Partial Sum Out (PLO)
            std::cout << "[TB-NoC1] Receiving Partial Sum Out (PLO) for ow=" << ow << std::endl;
            for(size_t oh = 0; oh < out_height; ++oh) {
                for(size_t och = 0; och < out_ch; och+=4) {
                    uint16_t channel = NOC_CHANNEL_PLO; // PLO (NoC1 Channel 1)
                    uint16_t tag = oh;
                    size_t base_idx = oh * out_width * out_ch +
                                     ow * out_ch + och;
                    recv_data_noc1(channel, tag, base_idx);
                }
            }
        }
    }

    void distribute_gemm_noc0() {
        std::cout << "[TB-NoC0] Distributing GEMM Data (Weights & Activations)..." << std::endl;

        int M = config.count("M") ? std::stoi(config["M"]) : std::stoi(config["in_height"]);
        int K = config.count("K") ? std::stoi(config["K"]) : std::stoi(config["in_ch"]);
        int N = config.count("N") ? std::stoi(config["N"]) : std::stoi(config["out_ch"]);

        // 1. Weights (Matrix B) -> PS
        for(size_t n = 0; n < N; ++n) {
            for(size_t k_base = 0; k_base < K; k_base += 16) {
                sc_biguint<256> data_packet = 0;
                for(size_t k_offset = 0; k_offset < 16; ++k_offset) {
                    size_t k = k_base + k_offset;
                    if (k >= K) break;
                    size_t idx = n * K + k;
                    fp16_t w = input_weight[idx];
                    data_packet.range((k_offset * 16) + 15, k_offset * 16) = w;
                }
                uint16_t channel = 0; // PS
                uint16_t tag = 0;
                send_data_noc0(channel, tag, data_packet);
            }
        }

        // 2. Activations (Matrix A) -> PD
        for(size_t m = 0; m < M; ++m) {
            for(size_t k_base = 0; k_base < K; k_base += 16) {
                sc_biguint<256> data_packet = 0;
                for(size_t k_offset = 0; k_offset < 16; ++k_offset) {
                    size_t k = k_base + k_offset;
                    if (k >= K) break;
                    size_t idx = m * K + k;
                    fp16_t a = input_activation[idx];
                    data_packet.range((k_offset * 16) + 15, k_offset * 16) = a;
                }
                uint16_t channel = 1; // PD
                uint16_t tag = m;
                send_data_noc0(channel, tag, data_packet, 0xF);
            }
        }
    }

    void distribute_gemm_noc1() {
        std::cout << "[TB-NoC1] Distributing GEMM Data (Partial Sums)..." << std::endl;

        int M = config.count("M") ? std::stoi(config["M"]) : std::stoi(config["in_height"]);
        int N = config.count("N") ? std::stoi(config["N"]) : std::stoi(config["out_ch"]);
        bool partial_sum_zero = (config["partial_sum_zero"] == "True");

        for(size_t m = 0; m < M; ++m) {
            // Input Partial Sums -> PLI
            for(size_t n = 0; n < N; n+=4) {
                sc_biguint<256> data_packet = 0;
                for(size_t n_offset = 0; n_offset < 4; ++n_offset) {
                    if (n + n_offset >= N) break;
                    size_t idx = m * N + (n + n_offset);
                    fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                    data_packet.range((n_offset * 16) + 15, n_offset * 16) = ps;
                }
                uint16_t channel = 0; // PLI
                uint16_t tag = m;
                send_data_noc1(channel, tag, data_packet);
            }

            // Output Partial Sums -> PLO
            for(size_t n = 0; n < N; n+=4) {
                uint16_t channel = 1; // PLO
                uint16_t tag = m;
                size_t base_idx = m * N + n;
                recv_data_noc1(channel, tag, base_idx);
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
        uint64_t total_instr = 0;
        uint64_t total_cycles = 0;
        double total_ipc = 0.0;
        for(size_t p = 0; p < NUM_PORTS; ++p) {
            for(size_t q = 0; q < NUM_PES_PER_PORT; ++q) {
                uint64_t instr_count = noc.pes[p][q].instr_count_reg.read();
                uint64_t cycles = noc.pes[p][q].cycles_reg.read();
                double ipc = (cycles > 0) ? static_cast<double>(instr_count) / cycles : 0.0;
                total_instr += instr_count;
                total_cycles += cycles;
                total_ipc += ipc;
            }
        }
        double avg_ipc = total_ipc / TOTAL_PES;
        std::cout << "PEs Average instruction count: " << total_instr / TOTAL_PES << std::endl;
        std::cout << "PEs Average cycle count: " << total_cycles / TOTAL_PES << std::endl;
        std::cout << "PEs Average IPC: " << avg_ipc << std::endl;

        // Total NoC cycle count
        // Clock period is 10 ns.
        double total_cycles_d = sc_time_stamp().to_seconds() / 10e-9;
        std::cout << "Total NoC cycle count: " << (uint64_t)total_cycles_d << std::endl;

        // Clock rate
        int ns_per_sec = 1000000000;
        std::cout << "Clock rate: " << num_to_str(ns_per_sec / clock_period_ns) << "Hz" << std::endl;

        // Total NoC data movement
        std::cout << "Total NoC data movement: " << num_to_str(total_sent_bytes + total_received_bytes) << " B" << std::endl;

        // Throughput
        double total_time_sec = sc_time_stamp().to_seconds();
        double throughput = (total_sent_bytes + total_received_bytes) / total_time_sec;
        std::cout << "NoC Throughput: " << num_to_str(static_cast<uint64_t>(throughput)) << " B/s" << std::endl;

        // Performance (MACs per second, 2FLOPS equivalent 1 MAC)
        double macs_per_sec = total_macs / total_time_sec;
        std::cout << "Performance (MACs per second): " << num_to_str(static_cast<uint64_t>(macs_per_sec)) << " MACs/s"
        << "(" << num_to_str(static_cast<uint64_t>(macs_per_sec * 2)) << " FLOPS)" << std::endl;

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

        // 3. Configure NoC
        configure_scan_chain();
        noc.dump_state();

        // 4. Load Program
        load_pe_program();

        // 5. Start PEs
        std::cout << "[TB] Starting PEs..." << std::endl;
        send_command(message_command_t::CMD_START_PE);

        // 6. Distribute Data (Parallel)
        // Calculate MACs for stats
        std::string mode = config["mode"];
        if (mode == "conv2d") {
            int kernel_size = std::stoi(config["kernel_size"]);
            int in_ch = std::stoi(config["in_ch"]);
            int out_ch = std::stoi(config["out_ch"]);
            int out_height = std::stoi(config["out_height"]);
            int out_width = std::stoi(config["out_width"]);
            total_macs = out_ch * out_height * out_width * kernel_size * kernel_size * in_ch;
        } else if (mode == "gemm") {
            int M = config.count("M") ? std::stoi(config["M"]) : std::stoi(config["in_height"]);
            int K = config.count("K") ? std::stoi(config["K"]) : std::stoi(config["in_ch"]);
            int N = config.count("N") ? std::stoi(config["N"]) : std::stoi(config["out_ch"]);
            total_macs = M * N * K;
        }

        std::cout << "[TB] Starting Parallel Data Distribution..." << std::endl;
        start_traffic_event.notify();

        // 7. Wait for completion
        std::cout << "[TB] Running Simulation..." << std::endl;

        // Wait for both threads to finish sending/receiving
        wait(noc0_done_event & noc1_done_event);

        // Wait until all expected responses are received (rx_idx_fifo should be empty)
        while (rx_idx_fifo.num_available() > 0) {
            wait(clock_period_ns, SC_NS);
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
    std::cout << "  -v <tolerance>   Set verification tolerance (default: 0.02)" << std::endl;
    std::cout << "  -t <trace_file>  Set trace file path (default: trace.json)" << std::endl;
    std::cout << "  <data_dir>       Path to the test data directory (default: output/noc/conv2d)" << std::endl;
}

int sc_main(int argc, char* argv[]) {
    std::string data_dir = "output/noc/conv2d"; // Default
    std::string trace_file = "trace.json";
    float verify_tolerance = 0.02f;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" && i + 1 < argc) {
            verify_tolerance = std::stof(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            trace_file = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else {
            data_dir = arg;
        }
    }

    NoCSimTestBench tb("NoCSimTestBench", data_dir, verify_tolerance,10);

    // Open trace file
    PerfettoTrace::getInstance().open(trace_file);

    sc_start();

    // Close trace file
    PerfettoTrace::getInstance().close();

    return 0;
}
