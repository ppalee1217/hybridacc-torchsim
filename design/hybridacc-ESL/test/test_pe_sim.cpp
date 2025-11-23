#include "pe_wrapper.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <systemc>
#include <fstream>
#include <cstdint>

using namespace hybridacc::test;

// Helper function to read binary file
std::vector<uint8_t> read_binary_file(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Failed to read file: " << filename << std::endl;
        return {};
    }

    return buffer;
}

// Helper function to convert bytes to fp16 values
std::vector<uint16_t> bytes_to_fp16(const std::vector<uint8_t>& bytes) {
    std::vector<uint16_t> fp16_values;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        uint16_t value = static_cast<uint16_t>(bytes[i]) |
                        (static_cast<uint16_t>(bytes[i + 1]) << 8);
        fp16_values.push_back(value);
    }
    return fp16_values;
}

// Helper function to convert bytes to uint64_t
std::vector<uint64_t> bytes_to_uint64(const std::vector<uint8_t>& bytes) {
    std::vector<uint64_t> values;
    for (size_t i = 0; i + 7 < bytes.size(); i += 8) {
        uint64_t value = 0;
        for (int j = 0; j < 8; j++) {
            value |= (static_cast<uint64_t>(bytes[i + j]) << (j * 8));
        }
        values.push_back(value);
    }
    return values;
}

// Helper function to parse meta.txt file
struct ConvParams {
    int kernel_size;
    int in_channels;
    int out_channels;
    int out_width;
    int groups_per_output;
};

ConvParams parse_meta_file(const std::string& filename) {
    ConvParams params = {0};
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open meta file: " << filename << std::endl;
        return params;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);

        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        if (key == "kernel_size") {
            params.kernel_size = std::stoi(value);
        } else if (key == "in_ch") {  // Changed from "in_channels"
            params.in_channels = std::stoi(value);
        } else if (key == "out_ch") {  // Changed from "out_channels"
            params.out_channels = std::stoi(value);
        } else if (key == "out_width") {
            params.out_width = std::stoi(value);
        }
    }

    // Calculate groups_per_output (out_channels / 4, since 4 fp16 per 64-bit)
    params.groups_per_output = params.out_channels / 4;

    return params;
}

// Main test runner
int sc_main(int argc, char* argv[]) {
    sc_core::sc_report_handler::set_actions("/IEEE_Std_1666/deprecated",
        sc_core::SC_DO_NOTHING);

    std::cout << "========================================" << std::endl;
    std::cout << "Conv3x3 Processing Pass Simulation Test" << std::endl;
    std::cout << "========================================" << std::endl;

    // Create PE Wrapper
    PEWrapper pe_wrapper("PE_Conv3x3", sc_time(10, SC_NS));
    pe_wrapper.set_debug(true);

    // Data paths
    const std::string data_dir = "/home/yoyo/work/MasterResearch/HybridAcc/output/data/conv3x3/";
    const std::string inst_file = data_dir + "conv1d_k3.bin";
    const std::string weight_file = data_dir + "weight.bin";
    const std::string activation_file = data_dir + "activation_input.bin";
    const std::string ps_input_file = data_dir + "ps_input.bin";
    const std::string expected_output_file = data_dir + "activation_output.bin";
    const std::string meta_file = data_dir + "meta.txt";

    // Parse convolution parameters from meta.txt
    std::cout << "\n[Step 0] Parsing convolution parameters from meta.txt..." << std::endl;
    ConvParams conv_params = parse_meta_file(meta_file);
    if (conv_params.kernel_size == 0 || conv_params.out_width == 0) {
        std::cerr << "Failed to parse meta.txt or invalid parameters" << std::endl;
        return 1;
    }
    std::cout << "Convolution parameters:" << std::endl;
    std::cout << "  kernel_size: " << conv_params.kernel_size << std::endl;
    std::cout << "  in_channels: " << conv_params.in_channels << std::endl;
    std::cout << "  out_channels: " << conv_params.out_channels << std::endl;
    std::cout << "  out_width: " << conv_params.out_width << std::endl;
    std::cout << "  groups_per_output: " << conv_params.groups_per_output << std::endl;

    // Step 1: Reset PE
    std::cout << "\n[Step 1] Resetting PE..." << std::endl;
    pe_wrapper.reset(10);
    std::cout << "PE Reset completed." << std::endl;

    // Step 2: Initialize PE Router
    std::cout << "\n[Step 2] Initializing PE Router..." << std::endl;
    // CMD_INIT format (based on PE_ROUTER_* definitions):
    // [30] enable (1 bit) - PE_ROUTER_EN_ID_OFFSET = 30
    // [29:28] mode (2 bits) - PE_ROUTER_MODE_ID_OFFSET = 28
    // [27:22] plo_id (6 bits) - PE_ROUTER_PLO_ID_OFFSET = 22
    // [21:16] pli_id (6 bits) - PE_ROUTER_PLI_ID_OFFSET = 16
    // [15:10] pd_id (6 bits) - PE_ROUTER_PD_ID_OFFSET = 10
    // [9:4] ps_id (6 bits) - PE_ROUTER_PS_ID_OFFSET = 4
    // [3:0] command (4 bits) = CMD_INIT (1)

    // Note: With the new router_enable and router_mode ports,
    // we also need to set these signals directly
    pe_wrapper.set_router_mode(hybridacc::pe::PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);
    pe_wrapper.set_router_enable(true);

    uint64_t init_cmd = 1; // CMD_INIT
    init_cmd |= (0ULL << 4);   // ps_id = 0 (bit 4-9)
    init_cmd |= (0ULL << 10);  // pd_id = 0 (bit 10-15)
    init_cmd |= (0ULL << 16);  // pli_id = 0 (bit 16-21)
    init_cmd |= (0ULL << 22);  // plo_id = 0 (bit 22-27)
    init_cmd |= (0b11ULL << 28); // mode = PLI_FROM_BUS_PLO_TO_BUS (bit 28-29)
    init_cmd |= (1ULL << 30);  // enable = 1 (bit 30)

    if (!pe_wrapper.send_noc_request(0x100, init_cmd)) {
        std::cerr << "Failed to send INIT command" << std::endl;
        return 1;
    }
    pe_wrapper.run_cycles(5);
    std::cout << "PE Router initialized with:" << std::endl;
    std::cout << "  - ps_id=0, pd_id=0, pli_id=0, plo_id=0" << std::endl;
    std::cout << "  - mode=PLI_FROM_BUS_PLO_TO_BUS (0b11)" << std::endl;
    std::cout << "  - enable=true" << std::endl;

    // Step 3: Load Program
    std::cout << "\n[Step 3] Loading program..." << std::endl;
    if (!pe_wrapper.load_program(inst_file)) {
        std::cerr << "Failed to load program from: " << inst_file << std::endl;
        return 1;
    }
    pe_wrapper.dump_instruction_memory();
    std::cout << "Program loaded successfully." << std::endl;

    // Step 4: Start PE
    std::cout << "\n[Step 4] Starting PE..." << std::endl;
    pe_wrapper.start();
    std::cout << "PE started." << std::endl;

    // Step 5: Load weights (PS channel)
    std::cout << "\n[Step 5] Loading weights..." << std::endl;
    auto weight_bytes = read_binary_file(weight_file);
    if (weight_bytes.empty()) {
        std::cerr << "Failed to read weight file" << std::endl;
        return 1;
    }
    auto weights = bytes_to_uint64(weight_bytes);
    std::cout << "Loaded " << weights.size() << " weight vectors (64-bit each)" << std::endl;

    // Send weights through NoC PS channel (address format: [7:6]=00 for PS, [5:0]=ps_id)
    const uint16_t PS_ADDR = 0x0000; // Channel PS, ID 0
    for (size_t i = 0; i < weights.size(); i++) {
        if (!pe_wrapper.send_noc_request(PS_ADDR, weights[i])) {
            std::cerr << "Failed to send weight " << i << std::endl;
            return 1;
        }
        if (i % 100 == 0) {
            std::cout << "Sent " << i << " / " << weights.size() << " weights" << std::endl;
        }
    }
    std::cout << "All weights sent." << std::endl;

    // Step 6: Send activation data (PD channel) and partial sums (PLI channel)
    std::cout << "\n[Step 6] Sending activation data and partial sums with proper convolution timing..." << std::endl;

    auto activation_bytes = read_binary_file(activation_file);
    auto ps_input_bytes = read_binary_file(ps_input_file);

    if (activation_bytes.empty() || ps_input_bytes.empty()) {
        std::cerr << "Failed to read data files" << std::endl;
        return 1;
    }

    auto activations = bytes_to_fp16(activation_bytes);
    auto ps_inputs = bytes_to_uint64(ps_input_bytes);

    std::cout << "Loaded " << activations.size() << " activations (fp16 each)" << std::endl;
    std::cout << "Loaded " << ps_inputs.size() << " partial sum inputs (64-bit each)" << std::endl;

    const uint16_t PD_ADDR = 0x0040;  // Channel PD (01), ID 0
    const uint16_t PLI_ADDR = 0x0080; // Channel PLI (10), ID 0
    const uint16_t PLO_ADDR = 0x00C0; // Channel PLO (11), ID 0

    // Use parameters from meta.txt
    std::vector<uint64_t> outputs;
    outputs.reserve(conv_params.out_width * conv_params.groups_per_output);

    // For each output position
    for (int out_pos = 0; out_pos < conv_params.out_width; out_pos++) {
        // Step 6a: Send KERNEL_SIZE activation groups
        // Each group contains in_channels fp16 values
        for (int k = 0; k < conv_params.kernel_size; k++) {
            int act_base_idx = (out_pos + k) * conv_params.in_channels;  // Base index for this kernel position

            // Send in_channels fp16 activations (one group)
            for (int c = 0; c < conv_params.in_channels; c++) {
                int act_idx = act_base_idx + c;
                if (act_idx < activations.size()) {
                    if (!pe_wrapper.send_noc_request(PD_ADDR, activations[act_idx])) {
                        std::cerr << "Failed to send activation at pos=" << out_pos
                                  << " k=" << k << " c=" << c << std::endl;
                    }
                } else {
                    std::cerr << "Warning: activation index " << act_idx << " out of range" << std::endl;
                }
            }

            // Give PE time to process
            pe_wrapper.run_cycles(5);
        }

        // Step 6b: Send partial sum inputs (groups_per_output groups)
        int ps_base_idx = out_pos * conv_params.groups_per_output;
        for (int g = 0; g < conv_params.groups_per_output; g++) {
            int ps_idx = ps_base_idx + g;
            if (ps_idx < ps_inputs.size()) {
                if (!pe_wrapper.send_noc_request(PLI_ADDR, ps_inputs[ps_idx])) {
                    std::cerr << "Failed to send ps_input at pos=" << out_pos
                              << " group=" << g << std::endl;
                }
            } else {
                std::cerr << "Warning: ps_input index " << ps_idx << " out of range" << std::endl;
            }
            pe_wrapper.run_cycles(5);
        }

        // Step 6c: Run computation and collect outputs
        pe_wrapper.run_cycles(50);  // Give time for computation

        for (int g = 0; g < conv_params.groups_per_output; g++) {
            // 先發送讀取請求到 PLO 通道
            if (!pe_wrapper.send_noc_read_request(PLO_ADDR)) {
                std::cerr << "Failed to send PLO read request at pos=" << out_pos
                          << " group=" << g << std::endl;
                continue;
            }

            // 然後讀取響應
            uint64_t output_data;
            if (pe_wrapper.read_noc_response(output_data)) {
                outputs.push_back(output_data);
                if (outputs.size() % 100 == 0) {
                    std::cout << "Received output " << outputs.size() << ": 0x"
                              << std::hex << output_data << std::dec << std::endl;
                }
            } else {
                std::cerr << "Warning: Failed to receive output at pos=" << out_pos
                          << " group=" << g << " (total received: " << outputs.size() << ")" << std::endl;
            }
        }

        // Progress indicator
        if (out_pos % 100 == 0) {
            std::cout << "Progress: " << out_pos << " / " << conv_params.out_width
                      << " (outputs collected: " << outputs.size() << ")" << std::endl;
        }
    }

    std::cout << "All input data sent and outputs collected: " << outputs.size() << std::endl;

    // Step 7: Wait for PE to complete
    std::cout << "\n[Step 7] Waiting for PE to complete..." << std::endl;
    const int MAX_WAIT_CYCLES = 10000;
    for (int cycle = 0; cycle < MAX_WAIT_CYCLES; cycle++) {
        // Try to read any remaining PLO data
        uint64_t output_data;
        if (pe_wrapper.read_noc_response(output_data)) {
            outputs.push_back(output_data);
            std::cout << "Received additional output " << outputs.size() << ": 0x"
                     << std::hex << output_data << std::dec << std::endl;
        }

        if (pe_wrapper.is_halted()) {
            std::cout << "PE halted after " << cycle << " additional cycles" << std::endl;
            break;
        }
        pe_wrapper.run_cycles(1);

        if (cycle % 1000 == 0 && cycle > 0) {
            std::cout << "Waiting... cycle: " << cycle << ", outputs: " << outputs.size() << std::endl;
        }
    }

    // Step 8: Verify outputs
    std::cout << "\n[Step 8] Verifying outputs..." << std::endl;
    auto expected_bytes = read_binary_file(expected_output_file);
    auto expected_outputs = bytes_to_uint64(expected_bytes);

    std::cout << "Expected outputs: " << expected_outputs.size() << std::endl;
    std::cout << "Received outputs: " << outputs.size() << std::endl;

    if (outputs.size() == expected_outputs.size()) {
        int mismatches = 0;
        for (size_t i = 0; i < outputs.size(); i++) {
            if (outputs[i] != expected_outputs[i]) {
                mismatches++;
                if (mismatches <= 10) { // Print first 10 mismatches
                    std::cout << "Mismatch at index " << i << ": "
                             << "expected 0x" << std::hex << expected_outputs[i]
                             << ", got 0x" << outputs[i] << std::dec << std::endl;
                }
            }
        }

        if (mismatches == 0) {
            std::cout << "✓ All outputs match! Test PASSED." << std::endl;
        } else {
            std::cout << "✗ Found " << mismatches << " mismatches. Test FAILED." << std::endl;
        }
    } else {
        std::cout << "✗ Output count mismatch. Test FAILED." << std::endl;
    }

    // Step 9: Print performance metrics
    std::cout << "\n[Step 9] Performance Metrics:" << std::endl;
    pe_wrapper.dump_state();
    auto metrics = pe_wrapper.get_performance_metrics();
    std::cout << "Total Cycles: " << metrics.total_cycles << std::endl;
    std::cout << "Instructions Executed: " << metrics.instruction_count << std::endl;
    std::cout << "IPC: " << metrics.ipc << std::endl;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test completed." << std::endl;
    std::cout << "========================================" << std::endl;

    sc_stop();
    return 0;
}