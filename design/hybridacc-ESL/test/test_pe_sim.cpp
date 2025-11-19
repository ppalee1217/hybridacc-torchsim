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
    std::cout << "\n[Step 6] Sending activation data and partial sums..." << std::endl;

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

    // Send data in interleaved manner (simulating the processing pass)
    size_t max_items = std::max(activations.size(), ps_inputs.size());

    for (size_t i = 0; i < max_items; i++) {
        // Send activation if available
        if (i < activations.size()) {
            if (!pe_wrapper.send_noc_request(PD_ADDR, activations[i])) {
                std::cerr << "Failed to send activation " << i << std::endl;
            }
        }

        // Send partial sum input if available
        if (i < ps_inputs.size()) {
            if (!pe_wrapper.send_noc_request(PLI_ADDR, ps_inputs[i])) {
                std::cerr << "Failed to send partial sum " << i << std::endl;
            }
        }

        // Give PE some cycles to process
        if (i % 10 == 0) {
            pe_wrapper.run_cycles(5);
            std::cout << "Progress: " << i << " / " << max_items << std::endl;
        }
    }

    std::cout << "All input data sent." << std::endl;

    // Step 7: Run simulation and collect outputs
    std::cout << "\n[Step 7] Running simulation and collecting outputs..." << std::endl;
    std::vector<uint64_t> outputs;

    // Run for a certain number of cycles and try to read PLO output
    const int MAX_CYCLES = 100000;
    const uint16_t PLO_ADDR = 0x00C0; // Channel PLO (11), ID 0

    for (int cycle = 0; cycle < MAX_CYCLES; cycle++) {
        // Try to read PLO data via NoC response
        uint64_t output_data;
        if (pe_wrapper.read_noc_response(output_data)) {
            outputs.push_back(output_data);
            std::cout << "Received output " << outputs.size() << ": 0x"
                     << std::hex << output_data << std::dec << std::endl;
        }

        pe_wrapper.run_cycles(1);

        // Check if PE is halted
        if (pe_wrapper.is_halted()) {
            std::cout << "PE halted after " << cycle << " cycles" << std::endl;
            break;
        }

        // Progress indicator
        if (cycle % 1000 == 0 && cycle > 0) {
            std::cout << "Cycle: " << cycle << ", Outputs collected: " << outputs.size() << std::endl;
            pe_wrapper.dump_state();
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