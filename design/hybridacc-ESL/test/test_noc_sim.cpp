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

    // New Valid-Ready Signals (Triple Plane)
    VRDSIG<noc::router_req_t> req0_sig; // NoC-0 (Push Plane)
    VRDSIG<noc::router_req_t> req1_sig; // NoC-1 (Local Network Plane - Write)
    VRDSIG<noc_addr_req_t>    req2_sig; // NoC-2 (Local Network Plane - Read)
    VRDSIG<noc::router_resp_t> resp2_sig; // NoC-2 Response

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
    sc_event noc0_done_event;
    sc_event noc1_done_event;
    sc_event noc2_done_event;

    // Parameters
    static constexpr size_t NUM_PORTS = 3;
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
          req2_sig("req2_sig"),
          resp2_sig("resp2_sig"),
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
        connect_vr_signals(noc.req2_in, req2_sig);
        connect_vr_signals(noc.resp2_out, resp2_sig);

        SC_THREAD(test_main);

        SC_THREAD(response_sink); // Add sink to accept responses
        sensitive << clk.posedge_event();

        SC_THREAD(noc0_thread);
        sensitive << clk.posedge_event();

        SC_THREAD(noc1_thread);
        sensitive << clk.posedge_event();

        SC_THREAD(noc2_thread);
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

    void send_data_noc0(uint16_t channel, uint16_t tag, const sc_biguint<256>& data_val, bool ultra_mode = false, size_t mask = 0xF) {
        noc::router_req_t req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag)) | (ultra_mode ? 0x100 : 0x000);
        req.mask = mask;
        // req.is_w = true;

        req0_sig.data_sig.write(req);
        req0_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req0_sig.ready_sig.read() == false);

        req0_sig.valid_sig.write(false);
        total_sent_bytes += (ultra_mode ? (8 * NUM_PORTS) : 8); // Ultra: NUM_PORTS * 64 bits; Normal: 64 bits
    }

    void send_data_noc1(uint16_t channel, uint16_t tag, const sc_biguint<256>& data_val, bool ultra_mode = false, size_t mask = 0xF) {
        noc::router_req_t req;
        req.data = data_val;
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag)) | (ultra_mode ? 0x0100 : 0x0000);
        req.mask = mask;
        // req.is_w = true; // NoC1 is always write

        req1_sig.data_sig.write(req);
        req1_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req1_sig.ready_sig.read() == false);

        req1_sig.valid_sig.write(false);
        total_sent_bytes += (ultra_mode ? (8 * NUM_PORTS) : 8); // Ultra: NUM_PORTS * 64 bits; Normal: 64 bits
    }

    void recv_data_noc2(uint16_t& channel, uint16_t& tag, uint64_t base_idx, bool ultra_mode = false, uint64_t per_port_stride = 0, uint8_t ports = NUM_PORTS) {
        // Send Read Request via NoC2
        noc_addr_req_t req;
        // req.data = 0; // No data for NoC2
        req.addr = (static_cast<uint16_t>(channel) << 6) | (static_cast<uint16_t>(tag)) | (ultra_mode ? 0x0100 : 0x0000);
        // req.mask = 0;
        // req.is_w = false;

        req2_sig.data_sig.write(req);
        req2_sig.valid_sig.write(true);

        do {
            wait(clk.posedge_event());
        } while (req2_sig.ready_sig.read() == false);

        req2_sig.valid_sig.write(false);

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
            resp2_sig.ready_sig.write(rx_idx_fifo.num_free() > 0);
            wait();
            if (resp2_sig.valid_sig.read() && resp2_sig.ready_sig.read()) {
                // receive response
                noc::router_resp_t resp = resp2_sig.data_sig.read();
                RespMeta meta;
                if(rx_idx_fifo.nb_read(meta)) {
                    // GEMM-specific handling: variable-length response mapping
                    if (config["mode"] == "gemm") {
                        // Compute dimensions and column offset from base_idx (base_idx = m * N + n)
                        size_t M = config.count("M") ? static_cast<size_t>(std::stoul(config["M"])) : 0;
                        size_t N = config.count("N") ? static_cast<size_t>(std::stoul(config["N"])) : 0;
                        size_t base = meta.base_idx;
                        // column index is base % N (since base = m * N + n)
                        size_t col = (N > 0) ? (base % N) : 0;

                        std::cout << "[TB] Received response (GEMM): meta=" << meta << ", data=0x" << std::hex << resp.data << std::dec
                                  << ", status=" << static_cast<int>(resp.status) << std::endl;

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
                            std::cout << "[TB] GEMM Ultra response: ports=" << static_cast<int>(meta.ports) << ", per_port_stride=" << meta.per_port_stride << std::endl;
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
                    } else {
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

    void noc2_thread() {
        while(true) {
            wait(start_traffic_event);
            std::string mode = config["mode"];
            if (mode == "conv2d") {
                distribute_conv2d_noc2();
            } else if (mode == "gemm") {
                distribute_gemm_noc2();
            }
            noc2_done_event.notify();
        }
    }

    void distribute_conv2d_noc0() {
        std::cout << "[TB-NoC0] Distributing Conv2D Data (Weights & Activations)..." << std::endl;

        int kernel_size = std::stoi(config["kernel_size"]);
        int in_ch = std::stoi(config["in_ch"]);
        int out_ch = std::stoi(config["out_ch"]);
        int in_height = std::stoi(config["in_height"]);
        int in_width = std::stoi(config["in_width"]);
        bool ultra_mode = (config["ultra_mode"] == "True");

        std::cout << "[TB-NoC0] Kernel Size: " << kernel_size << std::endl;
        std::cout << "[TB-NoC0] Input Channels: " << in_ch << std::endl;
        std::cout << "[TB-NoC0] Output Channels: " << out_ch << std::endl;
        std::cout << "[TB-NoC0] Input Height: " << in_height << std::endl;
        std::cout << "[TB-NoC0] Input Width: " << in_width << std::endl;

        // 1. Weights (PS)
        std::cout << "[TB-NoC0] Sending Weights..." << std::endl;

        // index helpers
        Index4D weight_idx(out_ch, kernel_size, 3, 4);
        Index3D act_idx(in_height, in_width, in_ch);

        for(size_t och = 0; och < out_ch; ++och) {
            for(size_t kh = 0; kh < kernel_size; ++kh) {
                for(size_t kw = 0; kw < 3; ++kw) { // Assume kernel width is 3 for simplicity
                    sc_biguint<256> data_packet = 0;
                    for(size_t ich = 0; ich < 4; ++ich) {
                        size_t idx = weight_idx(och, kh, kw, ich);
                        fp16_t w = input_weight[idx];
                        data_packet.range((ich * 16) + 15, ich * 16) = w;
                    }
                    uint16_t channel = NOC_CHANNEL_PS; // PS
                    uint16_t tag = kh;
                    send_data_noc0(channel, tag, data_packet, false, 0xF);
                }
            }
        }


        // 2. Activations (PD)
        std::cout << "[TB-NoC0] Sending Activations..." << std::endl;

        size_t loop_in_height;
        if (ultra_mode) {
            loop_in_height = in_height / NUM_PORTS; // Split height among ports
        } else {
            loop_in_height = in_height;
        }

        // Determine max channels per packet based on mode
        // Ultra Mode: 3 ports share 192 bits -> 64 bits/port -> 4 fp16 channels/port
        // Normal Mode: 1 port uses 192 bits -> 12 fp16 channels
        size_t channels_per_packet = ultra_mode ? 4 : 12;

        for(size_t iw = 0; iw < in_width; ++iw) {
            std::cout << "[TB-NoC0] Processing input width index: " << iw << "/" << in_width << std::endl;
            for(size_t ih = 0; ih < loop_in_height; ++ih) {

                // Iterate over channel chunks
                for (size_t ich_base = 0; ich_base < in_ch; ich_base += channels_per_packet) {
                    sc_biguint<256> data_packet = 0;
                    size_t mask = 0;
                    size_t current_chunk_size = std::min(channels_per_packet, (size_t)(in_ch - ich_base));

                    if (ultra_mode) { // Ultra Mode: Each bus handles a chunk of the input height
                        for(size_t ih_u = 0; ih_u < NUM_PORTS; ++ih_u) {
                            for(size_t c_off = 0; c_off < current_chunk_size; ++c_off) {
                                size_t ich = ich_base + c_off;
                                size_t base_h = ih_u * loop_in_height + ih;
                                size_t idx = act_idx(base_h, iw, ich);
                                fp16_t a = input_activation[idx];
                                // Pack based on fixed 4-channel slots per port in Ultra Mode
                                size_t slot_idx = ih_u * 4 + c_off;
                                data_packet.range((slot_idx * 16) + 15, slot_idx * 16) = a;
                                mask |= (1 << slot_idx);
                            }
                        }
                    } else { // Normal Mode: Single bus handles all channels for the given (ih, iw)
                        for(size_t c_off = 0; c_off < current_chunk_size; ++c_off) {
                            size_t ich = ich_base + c_off;
                            size_t idx = act_idx(ih, iw, ich);
                            fp16_t a = input_activation[idx];
                            size_t slot_idx = c_off;
                            data_packet.range((slot_idx * 16) + 15, slot_idx * 16) = a;
                            mask |= (1 << slot_idx);
                        }
                    }

                    uint16_t channel = NOC_CHANNEL_PD; // PD
                    uint16_t tag = ih;
                    send_data_noc0(channel, tag, data_packet, ultra_mode, mask);
                }
            }
        }
    }

    void distribute_conv2d_noc1() {
        std::cout << "[TB-NoC1] Distributing Conv2D Data (Partial Sums - Write)..." << std::endl;

        int kernel_size = std::stoi(config["kernel_size"]);
        int stride = std::stoi(config["stride"]);
        int out_ch = std::stoi(config["out_ch"]);
        int out_height = std::stoi(config["out_height"]);
        int out_width = std::stoi(config["out_width"]);
        bool partial_sum_zero = (config["partial_sum_zero"] == "True");

        size_t loop_out_height;
        bool ultra_mode = (config["ultra_mode"] == "True");
        if (ultra_mode) {
            loop_out_height = out_height / NUM_PORTS; // Split height among ports
        } else {
            loop_out_height = out_height;
        }

        // index helper for partial sums
        Index3D ps_idx(out_height, out_width, out_ch);

        for(size_t ow = 0; ow < out_width; ++ow) {
            // Partial Sum In (PLI) - Write via NoC1
            std::cout << "[TB-NoC1] Sending Partial Sum In (PLI) for ow=" << ow << std::endl;
            for(size_t oh = 0; oh < loop_out_height; ++oh) {
                for(size_t och = 0; och < out_ch; och+=4) {
                    sc_biguint<256> data_packet = 0;
                    if (ultra_mode) {
                        for(size_t oh_u = 0; oh_u < NUM_PORTS; ++oh_u) {
                            for(size_t och_offset = 0; och_offset < 4; ++och_offset) {
                                size_t base_h = oh_u * loop_out_height + oh;
                                size_t idx = ps_idx(base_h, ow, (och + och_offset));
                                fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                                data_packet.range(((oh_u * 4 + och_offset) * 16) + 15, (oh_u * 4 + och_offset) * 16) = ps;
                            }
                        }
                    } else {
                        for(size_t och_offset = 0; och_offset < 4; ++och_offset) {
                            size_t idx = ps_idx(oh, ow, (och + och_offset));
                            fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                            data_packet.range(((och_offset) * 16) + 15, (och_offset) * 16) = ps;
                        }
                    }
                    uint16_t channel = NOC_CHANNEL_PLI; // PLI (NoC1 Channel 0)
                    uint16_t tag = oh;
                    send_data_noc1(channel, tag, data_packet, ultra_mode, 0xF);
                }
            }
        }
    }

    void distribute_conv2d_noc2() {
        std::cout << "[TB-NoC2] Distributing Conv2D Requests (Partial Sums - Read)..." << std::endl;

        int out_ch = std::stoi(config["out_ch"]);
        int out_height = std::stoi(config["out_height"]);
        int out_width = std::stoi(config["out_width"]);

        size_t loop_out_height;
        bool ultra_mode = (config["ultra_mode"] == "True");
        if (ultra_mode) {
            loop_out_height = out_height / NUM_PORTS; // Split height among ports
        } else {
            loop_out_height = out_height;
        }

        // helper index for Partial Sums
        Index3D ps_idx(out_height, out_width, out_ch);

        uint64_t plo_per_port_stride = static_cast<uint64_t>(loop_out_height) * out_width * out_ch;

        for(size_t ow = 0; ow < out_width; ++ow) {
             // Partial Sum Out (PLO) - Read via NoC2
            std::cout << "[TB-NoC2] Receiving Partial Sum Out (PLO) for ow=" << ow << std::endl;
            for(size_t oh = 0; oh < loop_out_height; ++oh) {
                for(size_t och = 0; och < out_ch; och+=4) {
                    uint16_t channel = NOC_CHANNEL_PLO; // PLO (NoC1 Channel 1)
                    uint16_t tag = oh;
                    size_t base_idx = ps_idx(oh, ow, och);
                    recv_data_noc2(channel, tag, base_idx, ultra_mode, (ultra_mode ? plo_per_port_stride : 0), NUM_PORTS);
                }
            }
        }
    }

    void distribute_gemm_noc0() {
        std::cout << "[TB-NoC0] Distributing GEMM Data (Weights & Activations)..." << std::endl;

        int M = config.count("M") ? std::stoi(config["M"]) : std::stoi(config["in_height"]);
        int K = config.count("K") ? std::stoi(config["K"]) : std::stoi(config["in_ch"]);
        int N = config.count("N") ? std::stoi(config["N"]) : std::stoi(config["out_ch"]);

        int grid_rows = config.count("grid_rows") ? std::stoi(config["grid_rows"]) : 1;
        int grid_cols = config.count("grid_cols") ? std::stoi(config["grid_cols"]) : 1;
        int grid_k = config.count("grid_k") ? std::stoi(config["grid_k"]) : 1;

        // PE Tile Sizes (Software/Arch definition)
        // Ensure these match the logic in noc_gen.py
        int PE_M = 12;
        int PE_N = 8;
        int PE_K = 32;

        // prepare index helpers
        Index2D weight_idx(K, N);
        Index2D act_idx(M, K);

        // 1. Weights (Matrix B) -> Port Static (PS)
        // ps_id = k_idx * grid_n + n_idx
        bool ultra_mode = (config["ultra_mode"] == "True");

        if (ultra_mode) {
             // In Ultra Mode (K-split GEMM), we distribute K different slices to different ports.
            // Port 0 gets K-tile 0, Port 1 gets K-tile 1, etc.
            // Packet format packs data for Port 0, Port 1, Port 2...
            // Tag = n_idx (shared).

            // Iterate through the depth of K-tile (PE_K)
            for(size_t k_offset = 0; k_offset < PE_K; ++k_offset) {
                for(size_t n_base = 0; n_base < N; n_base += 4) {
                    sc_biguint<256> data_packet = 0;

                    // For each port, we fill 4 fp16 values corresponding to its K-tile
                    // Total packet: [Port0 64b | Port1 64b | Port2 64b | ...]
                    for (size_t port = 0; port < NUM_PORTS; ++port) {
                        // Calculate global K for this port
                        // Assuming 1-to-1 mapping: Port P handles K-tile P.
                        int k_tile = port;
                        size_t k_global = k_tile * PE_K + k_offset;

                        if (k_global >= K) continue; // Padding or out of bounds

                        // Fill 4 elements along N
                        for(size_t n_offset = 0; n_offset < 4; ++n_offset) {
                            size_t n = n_base + n_offset;
                            if (n >= N) break;

                            size_t idx = weight_idx(k_global, n);
                            fp16_t w = input_weight[idx];

                            // Map to packet bits
                            // Port 0: bits 0-63. Port 1: bits 64-127...
                            // Within port: standard order
                            int bit_offset = (port * 64) + (n_offset * 16);
                            data_packet.range(bit_offset + 15, bit_offset) = w;
                        }
                    }

                    // Determine Tag (Shared n_idx)
                    int n_idx = n_base / PE_N;
                    // k_idx is not in tag for ultra mode (implicit by port)

                    uint16_t channel = NOC_CHANNEL_PS;
                    uint16_t tag = n_idx;

                    send_data_noc0(channel, tag, data_packet, true, 0xF);
                    std::cout << "[TB-NoC0] Sent Weight Packet (ULTRA) - n_idx:" << n_idx
                              << ", Tag: " << tag  << ", n_base:" << n_base << ", k_offset:" << k_offset << std::endl;
                }
            }
        } else {
            for(size_t k = 0; k < K; ++k) {
                for(size_t n_base = 0; n_base < N; n_base += 4) {
                    sc_biguint<256> data_packet = 0;
                    for(size_t n_offset = 0; n_offset < 4; ++n_offset) {
                        size_t n = n_base + n_offset;
                        if (n >= N) break;
                        size_t idx = weight_idx(k, n);
                        fp16_t w = input_weight[idx];
                        data_packet.range((n_offset * 16) + 15, n_offset * 16) = w;
                    }

                    // Determine Tag based on Scan Chain ID
                    // Map current data slice (n, k_base) to Grid ID
                    int n_idx = n_base / PE_N;
                    int k_idx = k / PE_K;

                    uint16_t channel = NOC_CHANNEL_PS;
                    uint16_t tag = k_idx * grid_cols + n_idx;
                    send_data_noc0(channel, tag, data_packet);
                    std::cout << "[TB-NoC0] Sent Weight Packet - n_idx:" << n_idx << ", k_idx:" << k_idx
                            << ", Tag: " << tag  << ", n_base:" << n_base << ", k:" << k << std::endl;
                }
            }
        }
        std::cout << "[TB-NoC0] Weights Distribution Complete." << std::endl;

        std::cout << "[TB-NoC0] Sending Activations..." << std::endl;
        // 2. Activations (Matrix A) -> Port Dynamic (PD)
        // pd_id = m_idx * grid_k + k_idx
        // bool ultra_mode = (config["ultra_mode"] == "True"); // Already read above
        size_t rows_per_port = ultra_mode ? (M / NUM_PORTS) : M;

        for(size_t k_offset = 0; k_offset < PE_K; k_offset++) {
            if (ultra_mode) {
                // In Ultra GEMM K-split mode:
                // We broadcast M to all ports (or split M? No, usually M is tiled).
                // Wait, previous logic for Ultra was M-split.
                // But K-split logic requires sending different K data to different ports.
                // The current gemm_ultra config uses grid_rows=4 (M=48 / 12).
                // It maps (M, N) grid to EACH bus.
                // So M is NOT split across buses. M is replicated/local to bus.
                // But each bus handles different K.
                // So we need to feed A[m, k(port)] to Port P.
                // We pack Data for Port 0 (k0), Port 1 (k1)...

                for(size_t m_base = 0; m_base < M; m_base += 4) {
                     sc_biguint<256> data_packet = 0;

                     for (size_t port = 0; port < NUM_PORTS; ++port) {
                        int k_tile = port;
                        size_t k_global = k_tile * PE_K + k_offset;
                        if (k_global >= K) continue;

                        for(size_t m_offset = 0; m_offset < 4; ++m_offset) {
                             size_t m = m_base + m_offset;
                             if (m >= M) break;
                             size_t idx = act_idx(m, k_global);
                             fp16_t a = input_activation[idx];

                             int bit_offset = (port * 64) + (m_offset * 16);
                             data_packet.range(bit_offset + 15, bit_offset) = a;
                        }
                     }

                     int m_idx = m_base / PE_M;
                     uint16_t channel = NOC_CHANNEL_PD;
                     uint16_t tag = m_idx; // Shared tag

                     send_data_noc0(channel, tag, data_packet, true, 0xF);
                     std::cout << "[TB-NoC0] Sent Activation Packet (ULTRA) - m_idx:" << m_idx
                               << ", Tag: " << tag  << ", m_base:" << m_base << ", k:" << k_offset << std::endl;
                }
            } else {
                for(size_t m_base = 0; m_base < M; m_base += 4) {
                    for(int k_tile = 0; k_tile < grid_k; ++k_tile) {
                        size_t k_global = k_tile * PE_K + k_offset;
                        if (k_global >= K) {
                            throw std::runtime_error("[TB-NoC0] Error: k_global exceeds K dimension!, k_global=" + std::to_string(k_global) + ", K=" + std::to_string(K) + ", k_tile=" + std::to_string(k_tile) + ", k_offset=" + std::to_string(k_offset));
                        }
                        sc_biguint<256> data_packet = 0;
                        for(size_t m_offset = 0; m_offset < 4; ++m_offset) {
                            size_t m = m_base + m_offset;
                            if (m >= M) break;
                            size_t idx = act_idx(m, k_global);
                            fp16_t a = input_activation[idx];
                            data_packet.range((m_offset * 16) + 15, m_offset * 16) = a;
                        }

                        // Determine Tag
                        int m_idx = m_base / PE_M;
                        int k_idx = k_tile;

                        uint16_t channel = NOC_CHANNEL_PD;
                        uint16_t tag = k_idx * grid_rows + m_idx;
                        send_data_noc0(channel, tag, data_packet, false, 0xF);
                        std::cout << "[TB-NoC0] Sent Activation Packet - m_idx:" << m_idx << ", k_idx:" << k_idx
                                << ", Tag: " << tag  << ", m_base:" << m_base << ", k:" << k_offset << std::endl;
                    }
                }
            }
        }
        std::cout << "[TB-NoC0] Activations Distribution Complete." << std::endl;
    }

    void distribute_gemm_noc1() {
        std::cout << "[TB-NoC1] Distributing GEMM Data (Partial Sums Input)..." << std::endl;

        int M = config.count("M") ? std::stoi(config["M"]) : std::stoi(config["in_height"]);
        int N = config.count("N") ? std::stoi(config["N"]) : std::stoi(config["out_ch"]);
        bool partial_sum_zero = (config["partial_sum_zero"] == "True");

        int grid_cols = config.count("grid_cols") ? std::stoi(config["grid_cols"]) : 1;
        int grid_k = config.count("grid_k") ? std::stoi(config["grid_k"]) : 1;

        int PE_M = 12;
        int PE_N = 8;
        // int PE_K = 32;

        Index2D ps_idx(M, N);

        // Input Partial Sums -> PLI
        // pli_id = (m_idx * grid_n + n_idx) ONLY FOR k_idx == 0
        // Packets are D_mn. Since K is vertical, the D matrix enters at top K layer (k=0).
        // Tag should match pli_id.

        bool ultra_mode = (config["ultra_mode"] == "True");
        size_t rows_per_port = ultra_mode ? (M / NUM_PORTS) : M;

        for(size_t n = 0; n < N; n++) {
            if (ultra_mode && grid_k > 1) {
                // K-Split Ultra Mode: PS Input goes to Bus 0 (Port 0) only via ULTRA packet (Mask 1)
                for(size_t m = 0; m < M; m+=4) {
                    sc_biguint<256> data_packet = 0;
                    for(size_t m_offset = 0; m_offset < 4; ++m_offset) {
                        if (m + m_offset >= M) break;
                        size_t idx = ps_idx(m + m_offset, n);
                        fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                        data_packet.range((m_offset * 16) + 15, m_offset * 16) = ps;
                    }
                    int m_idx = m / PE_M;
                    int n_idx = n / PE_N;
                    uint16_t channel = NOC_CHANNEL_PLI;
                    uint16_t tag = m_idx * grid_cols + n_idx;

                    send_data_noc1(channel, tag, data_packet, true, 0x1);
                    std::cout << "[TB-NoC1] Sent K-Split PS Input (ULTRA Port 0) - m:" << m << ", n:" << n << ", Tag:" << tag << std::endl;
                }
            } else if (ultra_mode) {
                // Send PLI per port region so each port receives its local rows
                for (size_t port = 0; port < NUM_PORTS; ++port) {
                    for(size_t m_local = 0; m_local < rows_per_port; m_local += 4) {
                        sc_biguint<256> data_packet = 0;
                        for(size_t m_offset = 0; m_offset < 4; ++m_offset) {
                            size_t m_global = port * rows_per_port + m_local + m_offset;
                            if (m_global >= M) break;
                            size_t idx = ps_idx(m_global, n);
                            fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                            data_packet.range((m_offset * 16) + 15, m_offset * 16) = ps;
                        }

                        int m_idx = (static_cast<int>(port) * static_cast<int>(rows_per_port) + static_cast<int>(m_local)) / PE_M;
                        int n_idx = n / PE_N;

                        uint16_t channel = NOC_CHANNEL_PLI;
                        uint16_t tag = m_idx * grid_cols + n_idx;

                        // Send with ULTRA flag so router distributes across ports
                        send_data_noc1(channel, tag, data_packet, true, 0xF);
                    }
                }
            } else {
                for(size_t m = 0; m < M; m+=4) {
                    // Prepare packet
                    sc_biguint<256> data_packet = 0;
                    for(size_t m_offset = 0; m_offset < 4; ++m_offset) {
                        if (m + m_offset >= M) break;
                        size_t idx = ps_idx(m + m_offset, n);
                        fp16_t ps = partial_sum_zero ? fp16_t(0) : input_partial_sum[idx];
                        data_packet.range((m_offset * 16) + 15, m_offset * 16) = ps;
                    }

                    int m_idx = m / PE_M;
                    int n_idx = n / PE_N;

                    uint16_t channel = NOC_CHANNEL_PLI;
                    uint16_t tag = m_idx * grid_cols + n_idx;

                    send_data_noc1(channel, tag, data_packet);
                    std::cout << "[TB-NoC1] Sent Partial Sum Input Packet - m_idx: " << m_idx << ", n_idx: " << n_idx << ", m: " << m << ", n: " << n
                              << ", Tag: " << tag << std::endl;
                }
            }
        }
    }

    void distribute_gemm_noc2() {
        std::cout << "[TB-NoC2] Distributing GEMM Requests (Partial Sums Output)..." << std::endl;

        int M = config.count("M") ? std::stoi(config["M"]) : std::stoi(config["in_height"]);
        int N = config.count("N") ? std::stoi(config["N"]) : std::stoi(config["out_ch"]);

        int grid_cols = config.count("grid_cols") ? std::stoi(config["grid_cols"]) : 1;
        int grid_k = config.count("grid_k") ? std::stoi(config["grid_k"]) : 1;
        int PE_M = 12;
        int PE_N = 8;

        Index2D ps_idx(M, N);

        bool ultra_mode = (config["ultra_mode"] == "True");
        size_t rows_per_port = ultra_mode ? (M / NUM_PORTS) : M;
        size_t per_port_stride = ultra_mode ? (rows_per_port * static_cast<size_t>(N)) : 0;

        for(size_t n = 0; n < N; ++n) {
             // Output Partial Sums -> PLO (Read)
             // plo_id = (m_idx * grid_n + n_idx) ONLY FOR k_idx == last
            if (ultra_mode && grid_k > 1) {
                // K-Split Ultra: Read from Bus 2 (Port 2) only.
                // Standard packet expected.
                for(size_t m = 0; m < M; m+=4) {
                    uint16_t channel = NOC_CHANNEL_PLO;
                    int m_idx = m / PE_M;
                    int n_idx = n / PE_N;
                    uint16_t tag = m_idx * grid_cols + n_idx;
                    size_t base_idx = ps_idx(m, n);

                    recv_data_noc2(channel, tag, base_idx);
                    std::cout << "[TB-NoC2] Requested K-Split PS Output (Port 2/Normal) - m:" << m << ", n:" << n << ", Tag:" << tag << std::endl;
                }
            } else if (ultra_mode && grid_k == 1) {
                // Request only the local base rows for port0; router will return NUM_PORTS chunks
                for (size_t m_local = 0; m_local < rows_per_port; m_local += 4) {
                    uint16_t channel = NOC_CHANNEL_PLO;

                    int m_idx = m_local / PE_M; // local tile index
                    int n_idx = n / PE_N;
                    uint16_t tag = m_idx * grid_cols + n_idx;

                    size_t base_idx = ps_idx(m_local, n);
                    recv_data_noc2(channel, tag, base_idx, true, per_port_stride, NUM_PORTS);
                    std::cout << "[TB-NoC2] Requested Partial Sum Output Packet (ULTRA) - m_idx: " << m_idx << ", n_idx: " << n_idx
                              << ", m_local: " << m_local << ", N base: " << n
                              << ", Tag: " << tag << ", per_port_stride: " << per_port_stride << std::endl;
                }
            } else {
                for(size_t m = 0; m < M; m+=4) {
                    uint16_t channel = NOC_CHANNEL_PLO;

                    int m_idx = m / PE_M;
                    int n_idx = n / PE_N;
                    uint16_t tag = m_idx * grid_cols + n_idx;

                    size_t base_idx = ps_idx(m, n);
                    recv_data_noc2(channel, tag, base_idx);
                    std::cout << "[TB-NoC2] Requested Partial Sum Output Packet - m_idx: " << m_idx << ", n_idx: " << n_idx
                              << ", m: " << m << ", N base: " << n
                              << ", Tag: " << tag << std::endl;
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
                if (!noc.pes[p][q].router_enable.read()) {
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
        start_traffic_event.notify(SC_ZERO_TIME);

        // 7. Wait for completion
        std::cout << "[TB] Running Simulation..." << std::endl;

        // Wait for all threads to finish sending/receiving with timeout
        sc_time start_wait = sc_time_stamp();
        sc_time timeout = sc_time(500, SC_MS);
        wait(timeout, noc0_done_event & noc1_done_event & noc2_done_event);

        if (sc_time_stamp() - start_wait >= timeout) {
             std::cerr << "[TB] ERROR: Simulation Timed Out waiting for NoC completion!" << std::endl;
             sc_stop();
             return;
        }

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
    std::cout << "  -c <clock_period_ns>   Set clock period in nanoseconds (default: 10)" << std::endl;
    std::cout << "  -v <tolerance>   Set verification tolerance (default: 0.02)" << std::endl;
    std::cout << "  -t <trace_file>  Set trace file path (default: trace.json)" << std::endl;
    std::cout << "  <data_dir>       Path to the test data directory (default: output/noc/conv2d)" << std::endl;
}

int sc_main(int argc, char* argv[]) {
    // Default parameters
    std::string data_dir = "output/noc/conv2d";
    std::string trace_file = "trace.json";
    float verify_tolerance = 0.02f;
    int clock_period_ns = 10;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" && i + 1 < argc) {
            verify_tolerance = std::stof(argv[++i]);
        } else if (arg == "-c" && i + 1 < argc) {
            clock_period_ns = std::stoi(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            trace_file = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else {
            data_dir = arg;
        }
    }

    // Instantiate TestBench
    NoCSimTestBench tb("NoCSimTestBench", data_dir, verify_tolerance, clock_period_ns);

    // Open trace file
    PerfettoTrace::getInstance().open(trace_file);

    sc_start();

    // Close trace file
    PerfettoTrace::getInstance().close();

    return 0;
}
