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

    // New Valid-Ready Signals
    VRDSIG<noc::router_req_t> req_sig;
    VRDSIG<noc::router_resp_t> resp_sig;

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

    // Parameters
    static constexpr size_t NUM_PORTS = 4;
    static constexpr size_t NUM_PES_PER_PORT = 16;
    static constexpr size_t TOTAL_PES = NUM_PORTS * NUM_PES_PER_PORT;

    SC_HAS_PROCESS(NoCSimTestBench);

    NoCSimTestBench(sc_module_name name, std::string data_dir, float verify_tolerance)
        : sc_module(name),
          clk("clk", 10, SC_NS),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          req_sig("req_sig"),
          resp_sig("resp_sig"),
          noc("NoC_DUT", NUM_PORTS, NUM_PES_PER_PORT),
          test_data_dir(data_dir),
          rx_idx_fifo("rx_idx_fifo", 1024),
          verify_tolerance(verify_tolerance),
          total_sent_bytes(0),
          total_received_bytes(0)
    {
        // Connect DUT
        noc.clk(clk);
        noc.reset_n(reset_n);
        noc.command_mode(command_mode);
        noc.command_data(command_data);

        // Connect Valid-Ready Interfaces
        connect_vr_signals(noc.req_in, req_sig);
        connect_vr_signals(noc.resp_out, resp_sig);

        SC_THREAD(test_main);
        SC_THREAD(response_sink); // Add sink to accept responses
        sensitive << clk.posedge_event();
    }

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
        wait(10, SC_NS);
        command_mode.write(false);
        wait(10, SC_NS);
    }

    void send_data(uint16_t channel, uint16_t tag, const sc_biguint<256>& data_val, size_t mask = 0xF) {
        noc::router_req_t req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag));
        req.mask = mask;
        req.is_w = true;

        // std::cout << "[TB] Sending data packet: channel=" << channel
        //           << ", tag=" << tag << std::endl;

        req_sig.data_sig.write(req);
        req_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req_sig.ready_sig.read() == false);

        req_sig.valid_sig.write(false);
        total_sent_bytes += 32; // 256 bits = 32 bytes
    }

    void recv_data(uint16_t& channel, uint16_t& tag, uint64_t base_idx) {
        // Send Read Request
        noc::router_req_t req;
        req.data = 0;
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag));
        req.mask = 0;
        req.is_w = false;

        req_sig.data_sig.write(req);
        req_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req_sig.ready_sig.read() == false);

        req_sig.valid_sig.write(false);

        if(rx_idx_fifo.num_free() > 0) {
            rx_idx_fifo.nb_write(base_idx);
        } else {
            std::cerr << "[TB] Warning: rx_idx_fifo is full, cannot log received data index!" << std::endl;
        }

        total_sent_bytes += 32; // 256 bits = 32 bytes
    }

    // Always accept responses
    void response_sink() {
        // wait for reset deassertion
        wait(20, SC_NS);
        while (true) {
            resp_sig.ready_sig.write(rx_idx_fifo.num_free() > 0);
            wait();
            if (resp_sig.valid_sig.read() && resp_sig.ready_sig.read()) {
                // receive response
                noc::router_resp_t resp = resp_sig.data_sig.read();
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

    // return MACs count
    int distribute_data() {
        std::map<std::string, int (NoCSimTestBench::*)()> support_mode_fns = {
            {"conv2d", &NoCSimTestBench::distribute_conv2d_data},
            {"gemm", &NoCSimTestBench::distribute_gemm_data}
        };

        std::string mode = config["mode"];
        if (support_mode_fns.find(mode) != support_mode_fns.end()) {
            std::cout << "[TB] Distributing data for mode: " << mode << std::endl;
            return (this->*support_mode_fns[mode])();
        } else {
            std::cerr << "[TB] Unsupported mode for data distribution: " << mode << std::endl;
            return 0;
        }
    }

    int distribute_conv2d_data() {
        std::cout << "[TB] Distributing Conv2D Data..." << std::endl;

        int kernel_size = std::stoi(config["kernel_size"]);
        int in_ch = std::stoi(config["in_ch"]);
        int stride = std::stoi(config["stride"]);
        int out_ch = std::stoi(config["out_ch"]);
        int in_height = std::stoi(config["in_height"]);
        int in_width = std::stoi(config["in_width"]);
        int out_height = std::stoi(config["out_height"]);
        int out_width = std::stoi(config["out_width"]);
        bool partial_sum_zero = (config["partial_sum_zero"] == "True");

        // weight
        for(size_t och = 0; och < out_ch; ++och) {
            for(size_t kh = 0; kh < kernel_size; ++kh) {
                for(size_t kw = 0; kw < kernel_size; ++kw) {
                    // Prepare data packet
                    sc_biguint<256> data_packet = 0;
                    for(size_t ich = 0; ich < in_ch; ++ich) {
                        // NHWC format
                        size_t idx = och * kernel_size * kernel_size * in_ch +
                                     kh * kernel_size * in_ch +
                                     kw * in_ch + ich;
                        fp16_t w = input_weight[idx];
                        data_packet.range((ich * 16) + 15, ich * 16) = w;
                    }
                    // Send data packet
                    uint16_t channel = 0; // Weight channel (PS)
                    uint16_t tag = kh; // Use kh as tag

                    // std::cout << "[TB] Sending Weight Packet: OCH=" << och
                    //           << " KH=" << kh << " KW=" << kw << " Data="
                    //           << std::hex;
                    // for (int i = 0; i < 4; ++i) {
                    //     std::cout << std::setw(15)
                    //               << data_packet.range((i * 64) + 63, i * 64).to_string(SC_HEX);
                    //     if (i != 3) std::cout << "_";
                    // }

                    // std::cout << std::dec << std::endl;
                    send_data(channel, tag, data_packet);
                }
            }
        }

        // activation
        for(size_t iw = 0; iw < in_width; ++iw) {
            std::cout << "[TB] Processing input width index: " << iw << "/" << in_width << std::endl;
            for(size_t ih = 0; ih < in_height; ++ih) {
                sc_biguint<256> data_packet = 0;

                for(size_t ich = 0; ich < in_ch; ++ich) {
                    // Prepare data packet (NHWC format)
                    size_t idx = ih * in_width * in_ch +
                                 iw * in_ch + ich;
                    fp16_t a = input_activation[idx];
                    data_packet.range((ich * 16) + 15, ich * 16) = a;
                }

                // Send data packet
                uint16_t channel = 1; // Activation channel (PD)
                uint16_t tag = ih; // Use ih as tag

                send_data(channel, tag, data_packet, 0xF); // Full mask for activation
            }

            if(iw < kernel_size - 1) continue; // Need enough rows for convolution

            // partial sum in
            size_t ow = (iw - (kernel_size - 1)) / stride; // Calculate output width index

            for(size_t oh = 0; oh < out_height; ++oh) {
                for(size_t och = 0; och < out_ch; och+=4) {
                    // Prepare data packet (NHWC format)
                    sc_biguint<256> data_packet = 0;
                    for(size_t och_offset = 0; och_offset < 4; ++och_offset) {
                        size_t idx = oh * out_width * out_ch +
                                     ow * out_ch + (och + och_offset);
                        fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                        data_packet.range(((och_offset) * 16) + 15, (och_offset) * 16) = ps;
                    }

                    // Send data packet
                    uint16_t channel = 2; // Partial Sum In channel (PLI)
                    uint16_t tag = oh; // Use oh as tag

                    // std::cout << "[TB] Sending Partial Sum In Packet: OW=" << ow
                    //         << " OH=" << oh << " OCH=" << och
                    //         << " Data=" << std::hex;
                    // for (int i = 0; i < 4; ++i) {
                    //     std::cout << std::setw(15)
                    //             << data_packet.range((i * 64) + 63, i * 64).to_string(SC_HEX);
                    //     if (i != 3) std::cout << "_";
                    // }
                    // std::cout << std::dec << std::endl;

                    send_data(channel, tag, data_packet);
                }
            }

            // partial sum out
            for(size_t oh = 0; oh < out_height; ++oh) {
                for(size_t och = 0; och < out_ch; och+=4) {
                    uint16_t channel = 3; // Partial Sum Out channel (PLO)
                    uint16_t tag = oh; // Use oh as tag
                    size_t base_idx = oh * out_width * out_ch +
                                     ow * out_ch + och;
                    recv_data(channel, tag, base_idx);
                }
            }
        }

        int total_macs = out_ch * out_height * out_width * kernel_size * kernel_size * in_ch;
        return total_macs;
    }

    int distribute_gemm_data() {
        std::cout << "[TB] Distributing GEMM Data..." << std::endl;
        std::cout << "[TB] GEMM data distribution not implemented yet." << std::endl;
        // Implement GEMM data distribution if needed
        return 0;
    }

    void check_results(const std::vector<fp16_t>& expected, const std::vector<fp16_t>& actual, double tolerance) {
        size_t errors = 0;
        // Calculate cosine similarity and max difference
        double dot_product = 0.0;
        double magnitude_received = 0.0;
        double magnitude_expected = 0.0;
        double max_diff = 0.0;

        for (size_t i = 0; i < expected.size(); i++) {
            float received_val = fp16_to_float(actual[i]);
            float expected_val = fp16_to_float(expected[i]);

            dot_product += received_val * expected_val;
            magnitude_received += received_val * received_val;
            magnitude_expected += expected_val * expected_val;

            double diff = std::abs(received_val - expected_val);
            if (diff > max_diff) {
                max_diff = diff;
            }
            if (diff > tolerance) {
                std::cout << "Mismatch at index " << i
                          << ": received=0x" << std::hex << actual[i]
                          << " expected=0x" << expected[i] << std::dec
                          << " diff=" << std::scientific << diff << std::dec <<std::endl;
            }
        }

        magnitude_received = std::sqrt(magnitude_received);
        magnitude_expected = std::sqrt(magnitude_expected);

        double cosine_similarity = dot_product / (magnitude_received * magnitude_expected);

        std::cout << "Cosine Similarity: " << cosine_similarity << std::endl;
        std::cout << std::scientific << "Max Difference: " << max_diff << std::dec << std::endl;

        if (cosine_similarity > 0.99) {
            std::cout << "✓ Outputs are highly similar! Test PASSED." << std::endl;
        } else {
            std::cout << "✗ Outputs are not similar enough. Test FAILED." << std::endl;
        }
    }

    void test_main() {
        // 1. Initialize
        reset_n.write(false);
        wait(20, SC_NS);
        reset_n.write(true);
        wait(10, SC_NS);

        // 2. Load Data
        load_test_data();

        // 3. Configure NoC
        configure_scan_chain();

        // 4. Load Program
        load_pe_program();

        // 5. Start PEs
        std::cout << "[TB] Starting PEs..." << std::endl;
        send_command(message_command_t::CMD_START_PE);

        // 6. Distribute Data
        int total_macs = distribute_data();

        // 7. Wait for completion
        std::cout << "[TB] Running Simulation..." << std::endl;

        // Wait until all expected responses are received (rx_idx_fifo should be empty)
        while (rx_idx_fifo.num_available() > 0) {
            wait(10, SC_NS);
        }

        // 8. Check Results
        std::cout << "[TB] Checking Results..." << std::endl;
        check_results(expected_output, output_partial_sum, verify_tolerance);

        // 9. Stop Simulation
        std::cout << "[TB] Simulation Finished." << std::endl;
        sc_stop();

        // 10. statistics
        std::cout << "========================================" << std::endl;
        std::cout << "              Statistics                " << std::endl;
        std::cout << "========================================" << std::endl;

        // PE[0][0] stats
        uint64_t pe_instr_count = noc.pes[0][0].instr_count_reg.read();
        uint64_t pe_cycles = noc.pes[0][0].cycles_reg.read();

        std::cout << "PE[0][0] instruction count: " << pe_instr_count << std::endl;
        std::cout << "PE[0][0] cycle count: " << pe_cycles << std::endl;
        std::cout << "PE[0][0] IPC: " << (double)pe_instr_count / pe_cycles << std::endl;

        // Total NoC cycle count
        // Clock period is 10 ns.
        double total_cycles = sc_time_stamp().to_seconds() / 10e-9;
        std::cout << "Total NoC cycle count: " << (uint64_t)total_cycles << std::endl;

        // Clock rate
        std::cout << "Clock rate: 100 MHz" << std::endl;

        // Total NoC data movement
        std::cout << "Total NoC data movement: " << (total_sent_bytes + total_received_bytes) << " Bytes" << std::endl;

        // Throughput
        double total_time_sec = sc_time_stamp().to_seconds();
        double throughput = (total_sent_bytes + total_received_bytes) / total_time_sec / (1024.0 * 1024.0); // MB/s
        std::cout << "NoC Throughput: " << throughput << " MB/s" << std::endl;

        // Performance (MACs per second)
        double macs_per_sec = total_macs / total_time_sec;
        std::cout << "Performance (MACs per second): " << macs_per_sec << std::endl;

        std::cout << "========================================" << std::endl;

    }
};

int sc_main(int argc, char* argv[]) {
    std::string data_dir = "output/noc/conv2d"; // Default
    float vefiry_tolerance = 0.02f;
    if (argc > 1) {
        data_dir = argv[1];

        if (argc > 2) {
            vefiry_tolerance = std::stof(argv[2]);
        }

    }

    NoCSimTestBench tb("NoCSimTestBench", data_dir, vefiry_tolerance);

    // Open trace file
    PerfettoTrace::getInstance().open("trace.json");

    sc_start();

    // Close trace file
    PerfettoTrace::getInstance().close();

    return 0;
}
