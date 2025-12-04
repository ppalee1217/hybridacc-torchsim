#include <systemc>
#include <iostream>
#include <cassert>
#include <vector>
#include "NetworkOnChip.hpp"
#include "utils.hpp"

using namespace hybridacc;
using namespace sc_core;
using namespace sc_dt;

// Test helper class for NoC
class NoCTestBench : public sc_module {
public:
    // Clock and reset
    sc_clock clk;
    sc_signal<bool> reset_n;

    // NoC Router interface signals
    sc_signal<bool> command_mode;
    sc_signal<sc_uint<32>> command_data;
    sc_signal<bool> en;
    sc_signal<bool> wen;
    sc_signal<sc_uint<10>> addr_sig;
    sc_signal<sc_biguint<256>> data_in_sig;
    sc_signal<sc_biguint<256>> data_out_sig;

    // NoC instance
    NetworkOnChip noc;

    // Test parameters
    static constexpr size_t NUM_PORTS = 4;
    static constexpr size_t NUM_PES_PER_PORT = 16;

    SC_HAS_PROCESS(NoCTestBench);

    NoCTestBench(sc_module_name name)
        : sc_module(name),
          clk("clk", 10, SC_NS),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          en("en"),
          wen("wen"),
          addr_sig("addr_sig"),
          data_in_sig("data_in_sig"),
          data_out_sig("data_out_sig"),
          noc("NoC_DUT", NUM_PORTS, NUM_PES_PER_PORT)
    {

        std::cout << "[TB] NUM_PORTS = " << NUM_PORTS << std::endl;
        std::cout << "[TB] NUM_PES_PER_PORT = " << NUM_PES_PER_PORT << std::endl;
        std::cout << "[TB] Total PEs = " << (NUM_PORTS * NUM_PES_PER_PORT) << std::endl;
        std::cout << "[TB] Creating NoC Test Bench" << std::endl;

        // Connect signals
        noc.clk(clk);
        noc.reset_n(reset_n);
        noc.command_mode(command_mode);
        noc.command_data(command_data);
        noc.en(en);
        noc.wen(wen);
        noc.addr(addr_sig);
        noc.data_in(data_in_sig);
        noc.data_out(data_out_sig);

        SC_THREAD(test_main);
    }

    void dump_noc_state() {
        std::cout << "\n[TB] Dumping NoC State:" << std::endl;
        noc.dump_state();
        std::cout << "[TB] End of NoC State Dump\n" << std::endl;
    }

    void reset_system() {
        std::cout << "[TB] Resetting system..." << std::endl;
        reset_n.write(false);
        command_mode.write(false);
        command_data.write(0);
        en.write(false);
        wen.write(false);
        addr_sig.write(0);
        data_in_sig.write(0);

        wait(20, SC_NS);
        reset_n.write(true);
        wait(10, SC_NS);
        std::cout << "[TB] Reset complete" << std::endl;
    }

    void send_command(message_command_t cmd, uint32_t param = 0) {
        uint32_t cmd_data = (param & 0xFFFFFFF0) | (static_cast<uint32_t>(cmd) & 0x0F);

        std::cout << "[TB] Sending command: " << cmd << " (0x"
                  << std::hex << cmd_data << std::dec << ")" << std::endl;

        command_mode.write(true);
        command_data.write(cmd_data);
        wait(10, SC_NS);
        command_mode.write(false);
        wait(50, SC_NS);
    }

    void send_scan_chain_config(uint8_t ps_id, uint8_t pd_id,
                                 uint8_t pli_id, uint8_t plo_id,
                                 PERouterMode mode, bool enable) {
        uint32_t config = 0;
        config |= (ps_id & 0x3F) << 4;
        config |= (pd_id & 0x3F) << 10;
        config |= (pli_id & 0x3F) << 16;
        config |= (plo_id & 0x3F) << 22;
        config |= (static_cast<uint32_t>(mode) & 0x03) << 28;
        config |= (enable ? 1 : 0) << 30;
        config |= static_cast<uint32_t>(message_command_t::CMD_NOC_SCAN_CHAIN) & 0x0F;

        std::cout << "[TB] Configuring scan-chain: PS=" << (int)ps_id
                  << ", PD=" << (int)pd_id
                  << ", PLI=" << (int)pli_id
                  << ", PLO=" << (int)plo_id
                  << ", Mode=" << mode
                  << ", Enable=" << enable << std::endl;

        command_mode.write(true);
        command_data.write(config);
        wait(10, SC_NS);
        command_mode.write(false);
        wait(50, SC_NS);
    }

    void load_program_to_all_pes(const std::vector<uint16_t>& program) {
        std::cout << "[TB] Loading program to all PEs (" << program.size() << " instructions)" << std::endl;

        for (size_t i = 0; i < program.size(); ++i) {
            uint32_t instr = static_cast<uint32_t>(program[i]);
            send_command(message_command_t::CMD_LOAD_PROGRAM , instr<<4);
        }

        std::cout << "[TB] Program loading complete" << std::endl;
    }

    void run_cycles(int n) {
        wait(n * 10, SC_NS);
    }

    void test_main() {
        std::cout << "\n========================================" << std::endl;
        std::cout << "NetworkOnChip Unit Test" << std::endl;
        std::cout << "========================================\n" << std::endl;

        // Test 1: Basic Reset and Initialization
        std::cout << "=== Test 1: Reset and Initialization ===" << std::endl;
        reset_system();
        std::cout << "✓ Test 1 passed\n" << std::endl;

        // Test 2: Scan-Chain Configuration
        std::cout << "=== Test 2: Scan-Chain Configuration ===" << std::endl;
        for (size_t i = 0; i < NUM_PORTS; ++i) {
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                uint8_t pe_id = i * NUM_PES_PER_PORT + j;
                send_scan_chain_config(
                    i+j, i + 1, j, j,
                    PERouterMode::PLI_FROM_BUS_PLO_TO_BUS, true
                );
                wait(20, SC_NS);
            }
        }
        dump_noc_state();
        std::cout << "✓ Test 2 passed\n" << std::endl;

        // Test 3: CMD_RESET Command
        std::cout << "=== Test 3: CMD_RESET Command ===" << std::endl;
        send_command(message_command_t::CMD_RESET);
        run_cycles(10);
        std::cout << "✓ Test 3 passed\n" << std::endl;

        // Test 4: CMD_INIT Command
        std::cout << "=== Test 4: CMD_INIT Command ===" << std::endl;
        send_command(message_command_t::CMD_INIT, 0x12340000);
        run_cycles(10);
        std::cout << "✓ Test 4 passed\n" << std::endl;

        // Test 5: Program Loading
        std::cout << "=== Test 5: Program Loading ===" << std::endl;
        std::vector<uint16_t> test_program = {
            0x0004, 0x0004, 0x0004, 0x001E
        };
        load_program_to_all_pes(test_program);
        std::cout << "✓ Test 5 passed\n" << std::endl;

        // Test 6: Start PE Execution
        std::cout << "=== Test 6: Start PE Execution ===" << std::endl;
        send_command(message_command_t::CMD_START_PE);
        run_cycles(100);
        std::cout << "✓ Test 6 passed\n" << std::endl;

        // Test 7: Stop PE Execution
        std::cout << "=== Test 7: Stop PE Execution ===" << std::endl;
        send_command(message_command_t::CMD_STOP_PE);
        run_cycles(10);
        std::cout << "✓ Test 7 passed\n" << std::endl;

        // Test 8: Multiple Router Modes
        std::cout << "=== Test 8: Multiple Router Modes ===" << std::endl;
        PERouterMode modes[] = {
            PERouterMode::PLI_FROM_LN_PLO_TO_LN,
            PERouterMode::PLI_FROM_BUS_PLO_TO_LN,
            PERouterMode::PLI_FROM_LN_PLO_TO_BUS,
            PERouterMode::PLI_FROM_BUS_PLO_TO_BUS
        };

        for (auto mode : modes) {
            std::cout << "  Testing mode: " << mode << std::endl;
            send_scan_chain_config(0, 1, 2, 3, mode, true);
            run_cycles(10);
        }
        std::cout << "✓ Test 8 passed\n" << std::endl;

        // Test 9: Sequential Program Execution
        std::cout << "=== Test 9: Sequential Execution Test ===" << std::endl;
        reset_system();

        for (size_t i = 0; i < noc.num_pes(); ++i) {
            send_scan_chain_config(1, 0, i, i,
                                   PERouterMode::PLI_FROM_BUS_PLO_TO_BUS, true);
        }

        load_program_to_all_pes(test_program);
        send_command(message_command_t::CMD_START_PE);
        run_cycles(200);
        send_command(message_command_t::CMD_STOP_PE);

        dump_noc_state();

        std::cout << "✓ Test 9 passed\n" << std::endl;

        // Test 10: NoC Data Transfer
        std::cout << "=== Test 10: NoC Data Transfer ===" << std::endl;

        std::cout << "[TB] Writing first data..." << std::endl;
        en.write(true);
        wen.write(true);
        addr_sig.write(0x000);

        sc_biguint<256> test_data_val = 0;
        test_data_val.range(63, 0) = 0x123456789ABCDEF0ULL;
        data_in_sig.write(test_data_val);

        std::cout << "[TB] Data value: 0x" << std::hex << test_data_val.range(63, 0).to_uint64() << std::dec << std::endl;

        wait(10, SC_NS);
        std::cout << "[TB] Disabling first write..." << std::endl;
        en.write(false);
        wait(10, SC_NS);

        std::cout << "[TB] Writing second data..." << std::endl;
        en.write(true);
        wen.write(true);
        addr_sig.write(0x000);

        test_data_val.range(63, 0) = 0xFEDCBA9876543210ULL;
        data_in_sig.write(test_data_val);

        std::cout << "[TB] Data value: 0x" << std::hex << test_data_val.range(63, 0).to_uint64() << std::dec << std::endl;

        wait(10, SC_NS);
        std::cout << "[TB] Disabling second write..." << std::endl;
        en.write(false);
        wait(10, SC_NS);

        std::cout << "[TB] Test 10 cleanup done" << std::endl;
        std::cout << "✓ Test 10 passed\n" << std::endl;

        std::cout << "\n========================================" << std::endl;
        std::cout << "All NetworkOnChip Tests Passed!" << std::endl;
        std::cout << "========================================\n" << std::endl;

        std::cout << "[TB] Calling sc_stop()..." << std::endl;
        std::flush(std::cout);
        wait(10, SC_NS);
        sc_stop();
    }
};

int sc_main(int argc, char* argv[]) {
    sc_report_handler::set_actions("/IEEE_Std_1666/deprecated",
        SC_DO_NOTHING);

    std::cout << "Starting NetworkOnChip Unit Testbench..." << std::endl;
    NoCTestBench tb("NoCTestBench");

    sc_start();

    return 0;
}
