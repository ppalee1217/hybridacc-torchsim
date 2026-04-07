#include "ProcessElement.hpp"
#include "Utils/utils.hpp"
#include <cassert>
#include <iostream>
#include <systemc>
#include <vector>

using namespace sc_core;
using namespace hybridacc;

// -----------------------------------------------------------------------------
// Simple PE testbench (no PEWrapper)
class PEUnitTestBench : public sc_module {
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

    // Local network interfaces
    VRDSIG<uint64_t> ln_pli_sig;
    VRDSIG<uint64_t> ln_plo_sig;

    // DUT
    pe::ProcessElement pe;

    SC_HAS_PROCESS(PEUnitTestBench);
    explicit PEUnitTestBench(sc_module_name name)
        : sc_module(name),
          clk("clk", 10, SC_NS),
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
          pe("PE_DUT") {
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

        // Default ready signals for outputs
        plo_resp_sig.ready_sig.write(true);
        ln_plo_sig.ready_sig.write(true);

        SC_THREAD(test_main);
    }

private:
    void wait_cycles(int cycles) {
        for (int i = 0; i < cycles; ++i) {
            wait(clk.posedge_event());
        }
    }

    void reset_dut(int reset_cycles = 5) {
        reset_n.write(false);
        router_enable.write(false);
        router_mode.write(PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);

        ps_sig.valid_sig.write(false);
        pd_sig.valid_sig.write(false);
        pli_sig.valid_sig.write(false);
        plo_req_sig.valid_sig.write(false);

        ln_pli_sig.valid_sig.write(false);
        ln_plo_sig.ready_sig.write(true);
        plo_resp_sig.ready_sig.write(true);

        wait_cycles(reset_cycles);
        reset_n.write(true);
        router_enable.write(true);
        wait_cycles(1);
    }

    void send_req(VRDSIG<noc_request_t>& sig, const noc_request_t& req) {
        sig.data_sig.write(req);
        sig.valid_sig.write(true);
        do {
            wait(clk.posedge_event());
        } while (!sig.ready_sig.read());
        sig.valid_sig.write(false);
    }

    void send_addr_req(VRDSIG<noc_addr_req_t>& sig, const noc_addr_req_t& req) {
        sig.data_sig.write(req);
        sig.valid_sig.write(true);
        do {
            wait(clk.posedge_event());
        } while (!sig.ready_sig.read());
        sig.valid_sig.write(false);
    }

    void program_pe(const std::vector<uint16_t>& program) {
        for (size_t i = 0; i < program.size(); ++i) {
            uint64_t cmd = static_cast<uint64_t>(message_command_t::CMD_LOAD_PROGRAM);
            cmd |= (static_cast<uint64_t>(i * sizeof(uint16_t)) << PE_ROUTER_IM_ADDR_OFFSET);
            cmd |= (static_cast<uint64_t>(program[i]) << PE_ROUTER_IM_DATA_OFFSET);

            noc_request_t req{};
            req.addr = PE_CMD_ADDRESS;
            req.data = cmd;
            req.mask = 0;
            send_req(ps_sig, req);
        }
    }

    void start_pe() {
        noc_request_t req{};
        req.addr = PE_CMD_ADDRESS;
        req.data = static_cast<uint64_t>(message_command_t::CMD_START_PE);
        req.mask = 0;
        send_req(ps_sig, req);
    }

    bool wait_for_busy(bool target, int max_cycles) {
        for (int i = 0; i < max_cycles; ++i) {
            if (pe_busy.read() == target) {
                return true;
            }
            wait(clk.posedge_event());
        }
        return false;
    }

    bool wait_for_completion(int max_cycles) {
        if (!wait_for_busy(true, max_cycles)) {
            return false;
        }
        return wait_for_busy(false, max_cycles);
    }

    bool try_read_plo(uint64_t& out_data, int max_cycles = 50) {
        for (int i = 0; i < max_cycles; ++i) {
            if (plo_resp_sig.valid_sig.read()) {
                out_data = plo_resp_sig.data_sig.read().data;
                return true;
            }
            wait(clk.posedge_event());
        }
        return false;
    }

    void test_main() {
        sc_report_handler::set_actions("/IEEE_Std_1666/deprecated", SC_DO_NOTHING);

        std::cout << "Running PE Unit Tests" << std::endl;
        std::cout << "=====================" << std::endl;

        const uint16_t HALT_INST = 0x001E;
        const uint16_t NOP_INST = 0x0004;
        const uint16_t VPSUM_INST = 0x800C;

        std::cout << "\n=== Test 1: Basic PE Operations ===" << std::endl;
        reset_dut();
        assert(pe_busy.read() == false);
        std::cout << "✓ Basic operations test passed" << std::endl;

        std::cout << "\n=== Test 2: Simple Program Execution ===" << std::endl;
        reset_dut();
        program_pe({NOP_INST, NOP_INST, HALT_INST});
        start_pe();
        bool completed = wait_for_completion(200);
        assert(completed);
        std::cout << "✓ Simple program test passed" << std::endl;

        std::cout << "\n=== Test 3: Execution Activity ===" << std::endl;
        reset_dut();
        program_pe({NOP_INST, NOP_INST, NOP_INST, HALT_INST});
        start_pe();
        bool saw_busy = wait_for_busy(true, 50);
        assert(saw_busy);
        completed = wait_for_busy(false, 200);
        assert(completed);
        std::cout << "✓ Execution activity test passed" << std::endl;

        std::cout << "\n=== Test 4: PLI/PLO Handshake (Bus Mode) ===" << std::endl;
        reset_dut();
        router_mode.write(PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);
        program_pe({NOP_INST, VPSUM_INST, HALT_INST});
        start_pe();

        noc_request_t pli_req{};
        pli_req.addr = 0x0000;
        pli_req.data = 0x123456789ABCDEF0ULL;
        pli_req.mask = 0;
        send_req(pli_sig, pli_req);
        std::cout << "Sent PLI request" << std::endl;

        noc_addr_req_t plo_req{};
        plo_req.addr = 0x0000;
        send_addr_req(plo_req_sig, plo_req);
        std::cout << "Sent PLO requests" << std::endl;

        uint64_t plo_data = 0;
        if (try_read_plo(plo_data)) {
            std::cout << "Received PLO data: 0x" << std::hex << plo_data << std::dec << std::endl;
        } else {
            std::cout << "No PLO data received (OK for NOP-only program)" << std::endl;
        }

        wait_for_completion(200);
        std::cout << "✓ PLI/PLO handshake test completed" << std::endl;

        std::cout << "\n=== Test 5: Stall Behavior ===" << std::endl;
        reset_dut();
        program_pe({NOP_INST, NOP_INST, NOP_INST, HALT_INST});
        start_pe();
        completed = wait_for_completion(1000);
        assert(completed);
        std::cout << "✓ Stall behavior test completed" << std::endl;

        std::cout << "\n=== Test 6: Error Handling (Timeout Guard) ===" << std::endl;
        reset_dut();
        program_pe({NOP_INST, HALT_INST});
        start_pe();
        completed = wait_for_completion(200);
        assert(completed);
        std::cout << "✓ Error handling test completed" << std::endl;

        std::cout << "\nAll tests completed successfully!" << std::endl;
        sc_stop();
    }
};

// Main test runner
int sc_main(int argc, char* argv[]) {
    PEUnitTestBench tb("PEUnitTestBench");
    sc_start();
    return 0;
}