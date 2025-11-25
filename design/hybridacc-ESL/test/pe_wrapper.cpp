#include "pe_wrapper.hpp"
#include <iostream>
#include <iomanip>
#include <cassert>

namespace hybridacc {
namespace test {

PEWrapper::PEWrapper(const std::string& name, sc_time clock_period)
    : clock_period(clock_period), debug_enabled(false), stall_cycle_count(0) {

    // Create clock
    clk = std::make_unique<sc_clock>("clk", clock_period);

    // Create PE instance
    pe = std::make_unique<hybridacc::pe::ProcessElement>(name.c_str());

    // Connect signals
    connect_signals();

    log_debug("PEWrapper created with name: " + name);
}

PEWrapper::~PEWrapper() {
    log_debug("PEWrapper destroyed");
}

void PEWrapper::connect_signals() {
    // Connect clock and reset
    pe->clk(*clk);
    pe->reset_n(reset_n);

    // Connect control signals
    // Note: pe_start is removed - controlled by router through NoC commands
    pe->pe_busy(pe_busy);

    // Connect router control signals
    pe->router_enable(router_enable);
    pe->router_mode(router_mode);

    // Connect NoC interface
    pe->noc_req_in(noc_req_in);
    pe->noc_req_in_valid(noc_req_in_valid);
    pe->noc_req_in_ready(noc_req_in_ready);
    pe->noc_resp_out(noc_resp_out);
    pe->noc_resp_out_valid(noc_resp_out_valid);
    pe->noc_resp_out_ready(noc_resp_out_ready);

    // Connect Local Network interface
    pe->ln_pli_in_data(ln_pli_in_data);
    pe->ln_pli_in_valid(ln_pli_in_valid);
    pe->ln_pli_in_ready(ln_pli_in_ready);
    pe->ln_plo_out_data(ln_plo_out_data);
    pe->ln_plo_out_valid(ln_plo_out_valid);
    pe->ln_plo_out_ready(ln_plo_out_ready);

    // Initialize signals
    reset_n.write(false);
    // pe_start signal removed
    router_enable.write(false);
    router_mode.write(hybridacc::pe::PERouterMode::PLI_FROM_LN_PLO_TO_LN);
    noc_req_in_valid.write(false);
    noc_resp_out_ready.write(false);
    ln_pli_in_valid.write(false);
    ln_plo_out_ready.write(true);
}

void PEWrapper::reset(int reset_cycles) {
    log_debug("Resetting PE for " + std::to_string(reset_cycles) + " cycles");

    // Assert reset
    reset_n.write(false);
    // pe_start signal removed - reset is controlled by router

    // Run reset cycles
    for (int i = 0; i < reset_cycles; i++) {
        sc_start(clock_period);
    }

    // Deassert reset
    reset_n.write(true);
    sc_start(clock_period);

    // Reset counters
    stall_cycle_count = 0;
    execution_trace.clear();

    log_debug("Reset completed");
}

void PEWrapper::start() {
    log_debug("Starting PE execution");

    // Send CMD_START_PE through NoC to start the PE
    // Command format: CMD_START_PE (4)
    uint64_t cmd = 4; // CMD_START_PE

    if (!send_noc_request(0x100, cmd)) { // PE_CMD_ADDRESS = 0x100
        log_debug("Failed to send START command");
        return;
    }

    log_debug("START command sent successfully");

    // Wait a few cycles for the PE to process the start command
    // and begin execution (pipeline latency)
    run_cycles(5);
}

void PEWrapper::stop() {
    log_debug("Stopping PE execution");

    // Send CMD_STOP_PE through NoC to stop the PE
    // Command format: CMD_STOP_PE (3)
    uint64_t cmd = 3; // CMD_STOP_PE

    if (!send_noc_request(0x100, cmd)) {
        log_debug("Failed to send STOP command");
        return;
    }

    log_debug("STOP command sent successfully");
}

void PEWrapper::run_cycles(int cycles) {
    log_debug("Running for " + std::to_string(cycles) + " cycles");

    for (int i = 0; i < cycles; i++) {
        monitor_signals();
        sc_start(clock_period);
        update_stall_monitoring();
    }

    log_debug("Completed " + std::to_string(cycles) + " cycles");
}

bool PEWrapper::run_until_halt(int max_cycles) {
    log_debug("Running until halt (max " + std::to_string(max_cycles) + " cycles)");

    for (int i = 0; i < max_cycles; i++) {
        monitor_signals();
        sc_start(clock_period);
        update_stall_monitoring();

        if (is_halted()) {
            log_debug("PE halted after " + std::to_string(i + 1) + " cycles");
            return true;
        }
    }

    log_debug("Timeout reached after " + std::to_string(max_cycles) + " cycles");
    return false;
}

bool PEWrapper::load_program(const std::string& filename) {
    log_debug("Loading program from file: " + filename);

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        log_debug("Failed to open file: " + filename);
        return false;
    }

    std::vector<uint16_t> instructions;
    uint16_t instruction;

    while (file.read(reinterpret_cast<char*>(&instruction), sizeof(instruction))) {
        instructions.push_back(instruction);
    }

    file.close();
    log_debug("Loaded " + std::to_string(instructions.size()) + " instructions");

    return load_program(instructions);
}

bool PEWrapper::load_program(const std::vector<uint16_t>& instructions) {
    log_debug("Loading program with " + std::to_string(instructions.size()) + " instructions");

    // Load instructions through NoC interface using PE router commands
    for (size_t i = 0; i < instructions.size(); i++) {
        // Create load program command
        uint64_t cmd = 2; // CMD_LOAD_PROGRAM
        cmd |= (static_cast<uint64_t>(i * sizeof(uint16_t)) << 4);  // Address
        cmd |= (static_cast<uint64_t>(instructions[i]) << 20); // Instruction data

        // Send command through NoC
        if (!send_noc_request(0x100, cmd)) { // PE_CMD_ADDRESS
            log_debug("Failed to send instruction " + std::to_string(i));
            return false;
        }

        // Wait a cycle for processing
        run_cycles(1);
    }

    log_debug("Program loaded successfully");
    return true;
}

void PEWrapper::load_instruction(uint16_t addr, uint16_t instruction) {
    log_debug("Loading instruction 0x" + std::to_string(instruction) +
              " at address 0x" + std::to_string(addr));

    uint64_t cmd = 2; // CMD_LOAD_PROGRAM
    cmd |= (static_cast<uint64_t>(addr) << 4);
    cmd |= (static_cast<uint64_t>(instruction) << 20);

    send_noc_request(0x100, cmd);
    run_cycles(1);
}

bool PEWrapper::send_noc_request(uint64_t addr, uint64_t data) {
    // 準備請求數據
    noc_request_t req;
    req.addr = addr;
    req.data = data;
    req.is_w = true;  // 寫入操作

    // 發送請求：設置數據和 valid 信號
    noc_req_in.write(req);
    noc_req_in_valid.write(true);

    //  等待握手成功（在同一個 cycle 內檢查 ready）
    sc_start(SC_ZERO_TIME);  // 讓組合邏輯穩定

    // 等待接收方的 ready 信號
    int timeout = 500;
    while (!noc_req_in_ready.read() && timeout > 0) {
        sc_start(clock_period);
        timeout--;
    }

    if (timeout == 0) {
        // 清除 valid 信號
        noc_req_in_valid.write(false);
        std::stringstream ss;
        ss << "NoC request timeout for addr: 0x" << std::hex << addr << ", data: 0x" << data << std::dec;
        log_debug(ss.str());
        assert(false && "NoC request timeout");
        return false;
    }

    //  握手成功：立即拉低 valid（在下一個時鐘邊緣之前）
    noc_req_in_valid.write(false);

    //  等待一個週期完成時序邏輯更新
    sc_start(clock_period);

    return true;
}

bool PEWrapper::send_noc_read_request(uint64_t addr) {
    // 準備讀取請求
    noc_request_t req;
    req.addr = addr;
    req.data = 0;  // 讀取時 data 不重要
    req.is_w = false;  // 讀取操作

    // 發送請求：設置數據和 valid 信號
    noc_req_in.write(req);
    noc_req_in_valid.write(true);

    //  等待握手成功
    sc_start(SC_ZERO_TIME);  // 讓組合邏輯穩定

    // 等待接收方的 ready 信號
    int timeout = 500;
    while (!noc_req_in_ready.read() && timeout > 0) {
        sc_start(clock_period);
        timeout--;
    }

    if (timeout == 0) {
        // 清除 valid 信號
        noc_req_in_valid.write(false);
        log_debug("NoC read request timeout - ready signal not received");
        return false;
    }

    //  握手成功：立即拉低 valid
    noc_req_in_valid.write(false);

    //  等待一個週期完成時序邏輯更新
    sc_start(clock_period);

    return true;
}

bool PEWrapper::read_noc_response(uint64_t& data) {
    // 設置 ready 信號，表示我們準備接收數據
    noc_resp_out_ready.write(true);

    // 等待 valid 信號
    int timeout = 500;  // 最多等待 100 個週期
    while (!noc_resp_out_valid.read() && timeout > 0) {
        sc_start(clock_period);
        timeout--;
    }

    if (timeout == 0) {
        noc_resp_out_ready.write(false);
        log_debug("NoC response timeout - valid signal not received");
        assert(false && "NoC response timeout");
        return false;
    }

    // 讀取數據
    noc_response_t resp = noc_resp_out.read();
    data = resp.data;

    // 等待一個週期完成握手
    sc_start(clock_period);

    // 可選：清除 ready 信號（或保持為 true 表示持續接收）
    noc_resp_out_ready.write(false);

    return true;
}

bool PEWrapper::send_pli_data(uint64_t data) {
    if (!ln_pli_in_ready.read()) {
        return false;
    }

    ln_pli_in_data.write(data);
    ln_pli_in_valid.write(true);

    sc_start(clock_period);

    ln_pli_in_valid.write(false);
    return true;
}

bool PEWrapper::read_plo_data(uint64_t& data) {
    if (!ln_plo_out_valid.read()) {
        return false;
    }

    data = ln_plo_out_data.read();
    return true;
}

bool PEWrapper::is_running() const {
    return pe->is_running();
}

bool PEWrapper::is_halted() const {
    return pe->is_halted();
}

bool PEWrapper::is_busy() const {
    return pe_busy.read();
}

uint64_t PEWrapper::get_cycle_count() const {
    return pe->get_cycle_count();
}

uint64_t PEWrapper::get_instruction_count() const {
    return pe->get_instruction_count();
}

PEWrapper::PerformanceMetrics PEWrapper::get_performance_metrics() const {
    PerformanceMetrics metrics;
    metrics.total_cycles = get_cycle_count();
    metrics.instruction_count = get_instruction_count();
    metrics.stall_cycles = stall_cycle_count;

    if (metrics.total_cycles > 0) {
        metrics.ipc = static_cast<double>(metrics.instruction_count) / metrics.total_cycles;
    } else {
        metrics.ipc = 0.0;
    }

    // TODO: Add detailed stall breakdown
    metrics.stall_breakdown["total"] = stall_cycle_count;

    return metrics;
}

void PEWrapper::set_debug(bool enable) {
    debug_enabled = enable;
    log_debug("Debug mode " + std::string(enable ? "enabled" : "disabled"));
}

void PEWrapper::dump_state() const {
    std::cout << "=== PE State Dump ===" << std::endl;
    std::cout << "Running: " << (is_running() ? "Yes" : "No") << std::endl;
    std::cout << "Halted: " << (is_halted() ? "Yes" : "No") << std::endl;
    std::cout << "Busy: " << (is_busy() ? "Yes" : "No") << std::endl;
    std::cout << "Cycles: " << get_cycle_count() << std::endl;
    std::cout << "Instructions: " << get_instruction_count() << std::endl;

    auto metrics = get_performance_metrics();
    std::cout << "IPC: " << std::fixed << std::setprecision(3) << metrics.ipc << std::endl;
    std::cout << "Stall Cycles: " << stall_cycle_count << std::endl;
    std::cout << "=====================" << std::endl;
}

void PEWrapper::dump_instruction_memory() const {
    pe->if_id_stage.IM.dump();
}

void PEWrapper::save_trace(const std::string& filename) const {
    log_debug("Saving execution trace to: " + filename);

    std::ofstream file(filename);
    if (!file.is_open()) {
        log_debug("Failed to open trace file: " + filename);
        return;
    }

    file << "Execution Trace" << std::endl;
    file << "===============" << std::endl;

    for (const auto& entry : execution_trace) {
        file << entry << std::endl;
    }

    file.close();
    log_debug("Trace saved successfully");
}

void PEWrapper::assert_completion(int max_cycles, const std::string& message) {
    bool completed = run_until_halt(max_cycles);
    if (!completed) {
        std::string error_msg = "PE did not complete within " + std::to_string(max_cycles) + " cycles";
        if (!message.empty()) {
            error_msg += ": " + message;
        }
        throw std::runtime_error(error_msg);
    }
}

void PEWrapper::assert_instruction_count(uint64_t expected, const std::string& message) {
    uint64_t actual = get_instruction_count();
    if (actual != expected) {
        std::string error_msg = "Expected " + std::to_string(expected) +
                               " instructions, got " + std::to_string(actual);
        if (!message.empty()) {
            error_msg += ": " + message;
        }
        throw std::runtime_error(error_msg);
    }
}

void PEWrapper::assert_performance(double min_ipc, const std::string& message) {
    auto metrics = get_performance_metrics();
    if (metrics.ipc < min_ipc) {
        std::string error_msg = "IPC " + std::to_string(metrics.ipc) +
                               " is below minimum " + std::to_string(min_ipc);
        if (!message.empty()) {
            error_msg += ": " + message;
        }
        throw std::runtime_error(error_msg);
    }
}

void PEWrapper::set_router_mode(hybridacc::pe::PERouterMode mode) {
    router_mode.write(mode);
    log_debug("Router mode set to: " + std::to_string(static_cast<int>(mode)));
}

void PEWrapper::set_router_enable(bool enable) {
    router_enable.write(enable);
    log_debug("Router enable set to: " + std::string(enable ? "true" : "false"));
}

void PEWrapper::monitor_signals() {
    if (!debug_enabled) return;

    std::string trace_entry = "Cycle " + std::to_string(get_cycle_count()) + ": ";

    if (is_running()) trace_entry += "RUNNING ";
    if (is_halted()) trace_entry += "HALTED ";
    if (is_busy()) trace_entry += "BUSY ";

    execution_trace.push_back(trace_entry);
}

void PEWrapper::log_debug(const std::string& message) const {
    if (debug_enabled) {
        std::cout << "[PE_DEBUG] " << message << std::endl;
    }
}

void PEWrapper::update_stall_monitoring() {
    // TODO: Monitor stall signals and update counters
    // This would require access to internal PE stall signals
}

} // namespace test
} // namespace hybridacc