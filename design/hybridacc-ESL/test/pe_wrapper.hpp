#pragma once

#include <systemc.h>
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <map>  // 添加 map header
#include "ProcessElement.hpp"

namespace hybridacc {
namespace test {

/**
 * PE Wrapper Class for Unit Testing
 *
 * This wrapper provides a simplified interface for testing the ProcessElement
 * with automatic signal management, test utilities, and performance monitoring.
 */
class PEWrapper {
public:
    /**
     * Constructor
     * @param name SystemC module name
     * @param clock_period Clock period in time units (default: 10ns)
     */
    PEWrapper(const std::string& name = "PE_Test", sc_time clock_period = sc_time(10, SC_NS));

    /**
     * Destructor - cleanup resources
     */
    ~PEWrapper();

    // === Test Control Interface ===

    /**
     * Reset the PE and initialize all signals
     * @param reset_cycles Number of cycles to hold reset (default: 5)
     */
    void reset(int reset_cycles = 5);

    /**
     * Start PE execution
     */
    void start();

    /**
     * Stop PE execution
     */
    void stop();

    /**
     * Run simulation for specified number of cycles
     * @param cycles Number of clock cycles to run
     */
    void run_cycles(int cycles);

    /**
     * Run until PE halts or timeout
     * @param max_cycles Maximum cycles before timeout (default: 10000)
     * @return true if PE halted normally, false if timeout
     */
    bool run_until_halt(int max_cycles = 10000);

    // === Program Loading Interface ===

    /**
     * Load program from binary file
     * @param filename Path to binary file
     * @return true if successful
     */
    bool load_program(const std::string& filename);

    /**
     * Load program from instruction array
     * @param instructions Vector of 16-bit instructions
     * @return true if successful
     */
    bool load_program(const std::vector<uint16_t>& instructions);

    /**
     * Load single instruction at specific address
     * @param addr Memory address
     * @param instruction 16-bit instruction
     */
    void load_instruction(uint16_t addr, uint16_t instruction);

    // === Data Interface ===

    /**
     * Send data through NoC request interface
     * @param addr Target address
     * @param data 64-bit data
     * @return true if accepted
     */
    bool send_noc_request(uint64_t addr, uint64_t data);

    /**
     * Send read request through NoC interface
     * @param addr Target address
     * @return true if accepted
     */
    bool send_noc_read_request(uint64_t addr);

    /**
     * Read data from NoC response interface
     * @param data Reference to store received data
     * @return true if data available
     */
    bool read_noc_response(uint64_t& data);

    /**
     * Send data through Local Network PLI (Port Local Input)
     * @param data 64-bit data
     * @return true if accepted
     */
    bool send_pli_data(uint64_t data);

    /**
     * Read data from Local Network PLO (Port Local Output)
     * @param data Reference to store received data
     * @return true if data available
     */
    bool read_plo_data(uint64_t& data);

    // === Status and Monitoring Interface ===

    /**
     * Check if PE is running
     */
    bool is_running() const;

    /**
     * Check if PE is halted
     */
    bool is_halted() const;

    /**
     * Check if PE is busy
     */
    bool is_busy() const;

    /**
     * Get current cycle count
     */
    uint64_t get_cycle_count() const;

    /**
     * Get executed instruction count
     */
    uint64_t get_instruction_count() const;

    /**
     * Get performance metrics
     */
    struct PerformanceMetrics {
        uint64_t total_cycles;
        uint64_t instruction_count;
        uint64_t stall_cycles;
        double ipc;  // Instructions per cycle
        std::map<std::string, uint64_t> stall_breakdown;
    };

    PerformanceMetrics get_performance_metrics() const;

    // === Debug Interface ===

    /**
     * Enable/disable debug logging
     * @param enable True to enable debug output
     */
    void set_debug(bool enable);

    /**
     * Check and print halted status of each stage
     */
    void check_halted() const;

    /**
     * Dump current PE state to console
     */
    void dump_state() const;

    /**
     * Dump the instruction memory to a file
     */
    void dump_instruction_memory() const;

    /**
     * Print stage status to console
     */
    void print_stage_status() const {
        pe->print_stage_status();
    }

    /**
     * Save execution trace to file
     * @param filename Output file path
     */
    void save_trace(const std::string& filename) const;

    // === Assertion Helpers ===

    /**
     * Assert that PE completes execution within timeout
     * @param max_cycles Maximum allowed cycles
     * @param message Error message if assertion fails
     */
    void assert_completion(int max_cycles, const std::string& message = "");

    /**
     * Assert expected instruction count
     * @param expected Expected number of instructions
     * @param message Error message if assertion fails
     */
    void assert_instruction_count(uint64_t expected, const std::string& message = "");

    /**
     * Assert performance criteria
     * @param min_ipc Minimum required IPC
     * @param message Error message if assertion fails
     */
    void assert_performance(double min_ipc, const std::string& message = "");

    // === Router Control Interface ===

    /**
     * Set router mode
     * @param mode Router mode (PLI/PLO routing configuration)
     */
    void set_router_mode(hybridacc::pe::PERouterMode mode);

    /**
     * Set router enable
     * @param enable Enable/disable router
     */
    void set_router_enable(bool enable);

private:
    // SystemC components
    std::unique_ptr<sc_clock> clk;
    std::unique_ptr<hybridacc::pe::ProcessElement> pe;

    // Signal connections
    sc_signal<bool> reset_n;
    // Note: pe_start is removed - PE is controlled by router through NoC commands
    sc_signal<bool> pe_busy;

    // Router control signals
    sc_signal<bool> router_enable;
    sc_signal<hybridacc::pe::PERouterMode> router_mode;

    // NoC interface signals
    sc_signal<noc_request_t> noc_req_in;
    sc_signal<bool> noc_req_in_valid;
    sc_signal<bool> noc_req_in_ready;
    sc_signal<noc_response_t> noc_resp_out;
    sc_signal<bool> noc_resp_out_valid;
    sc_signal<bool> noc_resp_out_ready;

    // Local Network signals
    sc_signal<uint64_t> ln_pli_in_data;
    sc_signal<bool> ln_pli_in_valid;
    sc_signal<bool> ln_pli_in_ready;
    sc_signal<uint64_t> ln_plo_out_data;
    sc_signal<bool> ln_plo_out_valid;
    sc_signal<bool> ln_plo_out_ready;

    // Test state
    sc_time clock_period;
    bool debug_enabled;
    uint64_t stall_cycle_count;
    std::vector<std::string> execution_trace;

    // Private methods
    void connect_signals();
    void monitor_signals();
    void log_debug(const std::string& message) const;
    void update_stall_monitoring();
};

} // namespace test
} // namespace hybridacc