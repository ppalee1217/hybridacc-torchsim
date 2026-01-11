#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"
#include "FIFO.hpp" // Include FIFO

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace noc {

static const int NUM_PORTS_DEFAULT = 3;

// Define Router Request/Response types
typedef request_t<sc_biguint<192>, uint16_t> router_req_t;
typedef response_t<sc_biguint<192>> router_resp_t;


// === FSM State & Registers ===
enum class RouterState {
    IDLE,
    SEND_REQ,
    COLLECT_RESP,
    PUSH_RESP
};

inline std::ostream& operator<<(std::ostream& os, RouterState state) {
    switch (state) {
        case RouterState::IDLE: return os << "IDLE";
        case RouterState::SEND_REQ: return os << "SEND_REQ";
        case RouterState::COLLECT_RESP: return os << "COLLECT_RESP";
        case RouterState::PUSH_RESP: return os << "PUSH_RESP";
        default: return os << "UNKNOWN";
    }
}

inline void sc_trace(sc_core::sc_trace_file* tf, const RouterState& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}


SC_MODULE(NoCRouter) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // ===  NoC Router interface ports ===
    // Command and configuration ports
    // For scan-chain programming and PE programming (addr = 0x100)
    sc_in<bool> command_mode;
    sc_in<sc_uint<32>> command_data;

    // New Valid-Ready Interface
    VRDIF<router_req_t> req0_in;
    VRDIF<router_req_t> req1_in; // Write PLI
    VRDIF<noc_addr_req_t> req2_in; // Read PLO (New)
    VRDOF<router_resp_t> resp2_out; // Read Response (Was resp1)

    // ===  NoC MBUS interface ports ===
    // NoC interface ports - using VRDIF/VRDOF
    // Split into NoC-0 (Write only), NoC-1 (Write only), NoC-2 (Read only)
    sc_vector<VRDOF<noc_request_t>> noc0_to_bus_req; // NoC-0: PS, PD, Command
    sc_vector<VRDOF<noc_request_t>> noc1_to_bus_req; // NoC-1: PLI Write
    sc_vector<VRDOF<noc_addr_req_t>> noc2_to_bus_req; // NoC-2: PLO Read Request
    sc_vector<VRDIF<noc_response_t>> bus_to_noc2_resp; // NoC-2 Response

    // ID, mode, enable scan-chain ports
    sc_out<bool> scan_chain_enable; // broadcast to all ports
    sc_vector<sc_in<ScanChainFormat>> scan_chain_in;
    sc_vector<sc_out<ScanChainFormat>> scan_chain_out;

    size_t num_ports;

    SC_HAS_PROCESS(NoCRouter);
    NoCRouter(sc_module_name name, size_t num_ports)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          req0_in("req0_in"),
          req1_in("req1_in"),
          req2_in("req2_in"),
          resp2_out("resp2_out"),
          noc0_to_bus_req("noc0_to_bus_req", num_ports),
          noc1_to_bus_req("noc1_to_bus_req", num_ports),
          noc2_to_bus_req("noc2_to_bus_req", num_ports),
          bus_to_noc2_resp("bus_to_noc2_resp", num_ports),
          scan_chain_enable("scan_chain_enable"),
          scan_chain_in("scan_chain_in", num_ports),
          scan_chain_out("scan_chain_out", num_ports),
          num_ports(num_ports),
          scan_chain_data_reg("scan_chain_data_reg"),
          scan_chain_data_next("scan_chain_data_next"),
          scan_chain_enable_reg("scan_chain_enable_reg"),
          scan_chain_enable_next("scan_chain_enable_next"),
          pending_read_reg("pending_read_reg"),
          pending_read_next("pending_read_next"),
          pending_read_ultra_reg("pending_read_ultra_reg"),
          pending_read_ultra_next("pending_read_ultra_next"),
          rx_stall_sig("rx_stall_sig"),
          req0_fifo("req0_fifo", 4),
          req1_fifo("req1_fifo", 4),
          req2_fifo("req2_fifo", 4),
          resp_fifo("resp_fifo", 4)
    {
        DEBUG_MSG("[Create] NoCRouter with " << num_ports << " ports", DEBUG_LEVEL_NOC_COMPONENTS);

        // Bind FIFOs
        req0_fifo.clk(clk);
        req0_fifo.reset_n(reset_n);
        req0_fifo.data_in(req0_fifo_in_sig);
        req0_fifo.push(req0_fifo_push_sig);
        req0_fifo.data_out(req0_fifo_out_sig);
        req0_fifo.pop(req0_fifo_pop_sig);
        req0_fifo.empty(req0_fifo_empty_sig);
        req0_fifo.full(req0_fifo_full_sig);

        req1_fifo.clk(clk);
        req1_fifo.reset_n(reset_n);
        req1_fifo.data_in(req1_fifo_in_sig);
        req1_fifo.push(req1_fifo_push_sig);
        req1_fifo.data_out(req1_fifo_out_sig);
        req1_fifo.pop(req1_fifo_pop_sig);
        req1_fifo.empty(req1_fifo_empty_sig);
        req1_fifo.full(req1_fifo_full_sig);

        req2_fifo.clk(clk);
        req2_fifo.reset_n(reset_n);
        req2_fifo.data_in(req2_fifo_in_sig);
        req2_fifo.push(req2_fifo_push_sig);
        req2_fifo.data_out(req2_fifo_out_sig);
        req2_fifo.pop(req2_fifo_pop_sig);
        req2_fifo.empty(req2_fifo_empty_sig);
        req2_fifo.full(req2_fifo_full_sig);

        resp_fifo.clk(clk);
        resp_fifo.reset_n(reset_n);
        resp_fifo.data_in(resp_fifo_in_sig);
        resp_fifo.push(resp_fifo_push_sig);
        resp_fifo.data_out(resp_fifo_out_sig);
        resp_fifo.pop(resp_fifo_pop_sig);
        resp_fifo.empty(resp_fifo_empty_sig);
        resp_fifo.full(resp_fifo_full_sig);

        // Register sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Request Processing NoC-0 (Tx)
        SC_METHOD(process_requests_noc0);
        sensitive << req0_fifo_empty_sig << req0_fifo_out_sig
                  << command_mode << command_data;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc0_to_bus_req[i].ready_in;
        }

        // Request Processing NoC-1 (Tx - Write)
        SC_METHOD(process_requests_noc1);
        sensitive << req1_fifo_empty_sig << req1_fifo_out_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc1_to_bus_req[i].ready_in;
        }

        // Request Processing NoC-2 (Tx - Read)
        SC_METHOD(process_requests_noc2);
        sensitive << req2_fifo_empty_sig << req2_fifo_out_sig << rx_stall_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc2_to_bus_req[i].ready_in;
        }

        // Response Processing (Rx - NoC2)
        SC_METHOD(process_responses);
        sensitive << pending_read_reg << pending_read_ultra_reg << resp_fifo_full_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << bus_to_noc2_resp[i].valid_in << bus_to_noc2_resp[i].data_in;
        }

        // Combinational processes
        SC_METHOD(comb_command_process);
        sensitive << command_mode << command_data << scan_chain_enable_reg;

        SC_METHOD(comb_scan_chain_output);
        sensitive << scan_chain_enable_reg << scan_chain_data_reg;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << scan_chain_in[i];
        }

        // Interface logic (connect ports to FIFOs)
        SC_METHOD(comb_interface_logic);
        sensitive << req0_in.valid_in << req0_in.data_in << req0_fifo_full_sig
                  << req1_in.valid_in << req1_in.data_in << req1_fifo_full_sig
                  << req2_in.valid_in << req2_in.data_in << req2_fifo_full_sig
                  << resp2_out.ready_in << resp_fifo_empty_sig << resp_fifo_out_sig;

        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    // Overloaded constructor with default number of PEs
    NoCRouter(sc_module_name name)
        : NoCRouter(name, NUM_PORTS_DEFAULT) // Default to 3 PEs
    {}

private:
    // === FIFOs ===
    hybridacc::pe::FIFO<router_req_t> req0_fifo;
    hybridacc::pe::FIFO<router_req_t> req1_fifo;
    hybridacc::pe::FIFO<noc_addr_req_t> req2_fifo;
    hybridacc::pe::FIFO<router_resp_t> resp_fifo;

    // FIFO Signals - NoC0
    sc_signal<router_req_t> req0_fifo_in_sig;
    sc_signal<bool> req0_fifo_push_sig;
    sc_signal<router_req_t> req0_fifo_out_sig;
    sc_signal<bool> req0_fifo_pop_sig;
    sc_signal<bool> req0_fifo_empty_sig;
    sc_signal<bool> req0_fifo_full_sig;

    // FIFO Signals - NoC1
    sc_signal<router_req_t> req1_fifo_in_sig;
    sc_signal<bool> req1_fifo_push_sig;
    sc_signal<router_req_t> req1_fifo_out_sig;
    sc_signal<bool> req1_fifo_pop_sig;
    sc_signal<bool> req1_fifo_empty_sig;
    sc_signal<bool> req1_fifo_full_sig;

    // FIFO Signals - NoC2 (Read Req)
    sc_signal<noc_addr_req_t> req2_fifo_in_sig;
    sc_signal<bool> req2_fifo_push_sig;
    sc_signal<noc_addr_req_t> req2_fifo_out_sig;
    sc_signal<bool> req2_fifo_pop_sig;
    sc_signal<bool> req2_fifo_empty_sig;
    sc_signal<bool> req2_fifo_full_sig;

    sc_signal<router_resp_t> resp_fifo_in_sig;
    sc_signal<bool> resp_fifo_push_sig;
    sc_signal<router_resp_t> resp_fifo_out_sig;
    sc_signal<bool> resp_fifo_pop_sig;
    sc_signal<bool> resp_fifo_empty_sig;
    sc_signal<bool> resp_fifo_full_sig;

    // === Sequential Elements ===
    sc_signal<ScanChainFormat> scan_chain_data_reg;
    sc_signal<ScanChainFormat> scan_chain_data_next;
    sc_signal<bool> scan_chain_enable_reg;
    sc_signal<bool> scan_chain_enable_next;

    sc_signal<bool> pending_read_reg;
    sc_signal<bool> pending_read_next;
    sc_signal<bool> pending_read_ultra_reg;
    sc_signal<bool> pending_read_ultra_next;

    // Internal signal for Rx stall
    sc_signal<bool> rx_stall_sig;

    // === Sequential Process ===
    void seq_process() {
        // Reset initialization
        ScanChainFormat init_config;
        init_config.ps_id = 0;
        init_config.pd_id = 0;
        init_config.pli_id = 0;
        init_config.plo_id = 0;
        init_config.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
        init_config.enable = false;
        scan_chain_data_reg.write(init_config);
        scan_chain_enable_reg.write(false);

        pending_read_reg.write(false);
        pending_read_ultra_reg.write(false);

        wait();

        while (true) {
            scan_chain_data_reg.write(scan_chain_data_next.read());
            scan_chain_enable_reg.write(scan_chain_enable_next.read());
            pending_read_reg.write(pending_read_next.read());
            pending_read_ultra_reg.write(pending_read_ultra_next.read());
            wait();
        }
    }


    // === Interface Logic (Combinational) ===
    void comb_interface_logic() {
        // Input: Req0 Port -> Req0 FIFO
        if (req0_in.valid_in.read() && !req0_fifo_full_sig.read()) {
            req0_in.ready_out.write(true);
            req0_fifo_in_sig.write(req0_in.data_in.read());
            req0_fifo_push_sig.write(true);
        } else {
            req0_in.ready_out.write(false);
            req0_fifo_push_sig.write(false);
        }

        // Input: Req1 Port -> Req1 FIFO (Write Only)
        if (req1_in.valid_in.read() && !req1_fifo_full_sig.read()) {
            req1_in.ready_out.write(true);
            req1_fifo_in_sig.write(req1_in.data_in.read());
            req1_fifo_push_sig.write(true);
        } else {
            req1_in.ready_out.write(false);
            req1_fifo_push_sig.write(false);
        }

        // Input: Req2 Port -> Req2 FIFO (Read Only)
        if (req2_in.valid_in.read() && !req2_fifo_full_sig.read()) {
            req2_in.ready_out.write(true);
            req2_fifo_in_sig.write(req2_in.data_in.read());
            req2_fifo_push_sig.write(true);
        } else {
            req2_in.ready_out.write(false);
            req2_fifo_push_sig.write(false);
        }

        // Output: Resp FIFO -> Resp2 Port
        if (!resp_fifo_empty_sig.read()) {
            resp2_out.valid_out.write(true);
            resp2_out.data_out.write(resp_fifo_out_sig.read());
            if (resp2_out.ready_in.read()) {
                resp_fifo_pop_sig.write(true);
            } else {
                resp_fifo_pop_sig.write(false);
            }
        } else {
            resp2_out.valid_out.write(false);
            resp_fifo_pop_sig.write(false);
        }
    }

    // === Request Processing NoC-0 (Tx) ===
    void process_requests_noc0() {
        // Default outputs
        req0_fifo_pop_sig.write(false);
        for (size_t i = 0; i < num_ports; ++i) {
            noc0_to_bus_req[i].valid_out.write(false);
            noc0_to_bus_req[i].data_out.write(noc_request_t());
        }

        // 1. Sideband Command (High Priority) -> NoC-0
        bool cmd_active = command_mode.read();
        sc_uint<32> cmd_val = command_data.read();
        message_command_t cmd_type = static_cast<message_command_t>(cmd_val.range(3, 0).to_uint());
        bool is_pe_cmd = cmd_active && (cmd_type != message_command_t::CMD_NOC_SCAN_CHAIN);

        if (is_pe_cmd) {
            noc_request_t cmd_req;
            cmd_req.addr = 0x100;
            cmd_req.data = cmd_val;
            // cmd_req.is_w = true; // Implicit Write

            for (size_t i = 0; i < num_ports; ++i) {
                noc0_to_bus_req[i].valid_out.write(true);
                noc0_to_bus_req[i].data_out.write(cmd_req);
            }
            return; // Block FIFO requests
        }

        // 2. FIFO Request (NoC-0)
        if (!req0_fifo_empty_sig.read()) {
            router_req_t req = req0_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 8) & 0x1;
            sc_uint<8> base_addr = req.addr & 0xFF;
            // bool is_write = req.is_w; // Implicit Write
            size_t mask = req.mask;

            // Broadcast / SIMD
            for (size_t i = 0; i < num_ports; ++i) {
                noc_request_t r;
                if (is_ultra) {
                    assert(num_ports <= NUM_PORTS_DEFAULT); // Max 4 ports for ultra mode (since 192/64=3)
                    r.data = req.data.range(64*i + 63, 64*i).to_uint64();
                } else { // Broadcast
                    r.data = req.data.range(63, 0).to_uint64();
                }
                r.addr = base_addr;
                // r.is_w = is_write;
                r.mask = mask;

                noc0_to_bus_req[i].data_out.write(r);
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc0_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                for (size_t i = 0; i < num_ports; ++i) {
                    noc0_to_bus_req[i].valid_out.write(true);
                }
                req0_fifo_pop_sig.write(true);
            }
        }
    }

    // === Request Processing NoC-1 (Tx) - Write Only ===
    void process_requests_noc1() {
        // Default outputs
        req1_fifo_pop_sig.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            noc1_to_bus_req[i].valid_out.write(false);
            noc1_to_bus_req[i].data_out.write(noc_request_t());
        }

        // FIFO Request (NoC-1)
        if (!req1_fifo_empty_sig.read()) {
            router_req_t req = req1_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 8) & 0x1;
            sc_uint<8> base_addr = req.addr & 0xFF;
            // bool is_write = req.is_w; // Always Write for NoC-1
            size_t mask = req.mask;
            noc_request_t r;

            // Broadcast / SIMD
            for (size_t i = 0; i < num_ports; ++i) {

                if (is_ultra) {
                    assert(num_ports <= NUM_PORTS_DEFAULT);
                    r.data = req.data.range(64*i + 63, 64*i).to_uint64();
                } else { // Broadcast
                    r.data = req.data.range(63, 0).to_uint64();
                }
                r.addr = base_addr;
                r.mask = mask;

                noc1_to_bus_req[i].data_out.write(r);
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc1_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                for (size_t i = 0; i < num_ports; ++i) {
                    noc1_to_bus_req[i].valid_out.write(true);
                }
                req1_fifo_pop_sig.write(true);
            }
            else{
                DEBUG_MSG(" [NoCRouter] NoC-1 Request not all ready, stalling - " << r << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }
        }
    }

    // === Request Processing NoC-2 (Tx) - Read Only ===
    void process_requests_noc2() {
        // Default outputs
        req2_fifo_pop_sig.write(false);
        pending_read_next.write(false);
        pending_read_ultra_next.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            noc2_to_bus_req[i].valid_out.write(false);
            noc2_to_bus_req[i].data_out.write(noc_addr_req_t());
        }

        // Check Rx Stall (Waiting for Response)
        if (rx_stall_sig.read()) {
            // Stall Tx: Hold pending read mask
            pending_read_next.write(pending_read_reg.read());
            pending_read_ultra_next.write(pending_read_ultra_reg.read());
            return;
        }

        // FIFO Request (NoC-2)
        if (!req2_fifo_empty_sig.read()) {
            noc_addr_req_t req = req2_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 8) & 0x1;

            if (is_ultra) {
                DEBUG_MSG("[NoCRouter] Received NoC-2 ULTRA read req addr=0x" << std::hex << req.addr << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }

            // Broadcast / SIMD Logic (Address based)
            noc_addr_req_t r;
            r.addr = req.addr;

            for (size_t i = 0; i < num_ports; ++i) {
                noc2_to_bus_req[i].data_out.write(r);
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc2_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                for (size_t i = 0; i < num_ports; ++i) {
                    noc2_to_bus_req[i].valid_out.write(true);
                }
                req2_fifo_pop_sig.write(true);

                // Set pending read state
                pending_read_next.write(true);
                pending_read_ultra_next.write(is_ultra);
            }
            else{
                DEBUG_MSG(" [NoCRouter] NoC-2 Request not all ready, stalling" << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }
        }
    }

    // === Response Processing (Rx) ===
    void process_responses() {
        // Default outputs
        resp_fifo_push_sig.write(false);
        resp_fifo_in_sig.write(router_resp_t());
        rx_stall_sig.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            bus_to_noc2_resp[i].ready_out.write(false);
        }

        bool is_pending_read = pending_read_reg.read();
        bool is_ultra = pending_read_ultra_reg.read();

        if (!is_pending_read) return;

        // Check if all expected responses are valid
        bool all_valid = true;
        sc_biguint<192> collected_data = 0;
        uint64_t valid_rx = 0;
        bool error_flag = false;

        for (size_t i = 0; i < num_ports; ++i) {
            if(bus_to_noc2_resp[i].valid_in.read()) {
                valid_rx |= (1ULL << i);
                if ( bus_to_noc2_resp[i].data_in.read().status == NOC_RESPONSE_STATUS::NOC_ERROR) {
                    error_flag = true;
                }
            }
        }

        if (is_ultra) {
            // SIMD mode: expect response from all ports
            for (size_t i = 0; i < num_ports; ++i) {
                if (!(valid_rx & (1ULL << i))) {
                    all_valid = false;
                    break;
                }
                collected_data.range(64*i + 63, 64*i) = bus_to_noc2_resp[i].data_in.read().data;
            }
        } else {
            // Broadcast mode: expect response from only one of ports
            bool resp_received = false;
            for (size_t i = 0; i < num_ports; ++i) {
                if (valid_rx & (1ULL << i)) {
                    if (resp_received) {
                        // More than one response received -> error
                        error_flag = true;
                    }
                    resp_received = true;
                    collected_data.range(63, 0) = bus_to_noc2_resp[i].data_in.read().data;
                }
            }
            if (!resp_received) {
                all_valid = false;
            }
        }

        for (size_t i = 0; i < num_ports; ++i) {
            DEBUG_MSG("[NoCRouter] Checking response from port " << i
                      << ": valid=" << bus_to_noc2_resp[i].valid_in.read()
                      << ", data=0x" << std::hex << bus_to_noc2_resp[i].data_in.read().data
                      << ", status=" << static_cast<int>(bus_to_noc2_resp[i].data_in.read().status)
                      << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
        }

        if (all_valid) {
            if (!resp_fifo_full_sig.read()) {
                // Accept responses
                for (size_t i = 0; i < num_ports; ++i) {
                    if (valid_rx & (1ULL << i)) {
                        bus_to_noc2_resp[i].ready_out.write(true);
                    }
                }

                // Push to FIFO
                router_resp_t final_resp;
                final_resp.data = collected_data;
                // Note: router_resp_t doesn't have status field in current definition,
                // assuming data is enough or error handling is done elsewhere/ignored for now.

                resp_fifo_push_sig.write(true);
                resp_fifo_in_sig.write(final_resp);

                DEBUG_MSG("[NoCRouter] Collected NoC-2 Response - Data: 0x"
                          << std::hex << collected_data << ", Valid: 0x" << std::hex << valid_rx << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);

                // rx_stall_sig remains false (default)
            } else {
                // FIFO full, cannot accept -> Stall
                rx_stall_sig.write(true);
            }
        } else {
            // Waiting for responses -> Stall
            rx_stall_sig.write(true);

            // Note: We keep ready_out low, so MBUS holds the data.
        }
    }

    // === Combinational: Command Processing ===
    void comb_command_process() {
        bool cmd_mode = command_mode.read();
        sc_uint<32> cmd_data = command_data.read();

        if (cmd_mode) {
            message_command_t cmd_type = static_cast<message_command_t>(cmd_data.range(3, 0).to_uint());

            if (cmd_type == message_command_t::CMD_NOC_SCAN_CHAIN) {
                // Parse scan-chain data from command_data
                ScanChainFormat sc_format = parse_scan_chain_data(cmd_data.to_uint());
                scan_chain_data_next.write(sc_format);
                scan_chain_enable_next.write(true);

                DEBUG_MSG("[NoCRouter] Scan-chain command received: "  << sc_format, DEBUG_LEVEL_NOC_COMPONENTS);

            } else {
                // PE command: keep scan-chain state
                scan_chain_data_next.write(scan_chain_data_reg.read());
                scan_chain_enable_next.write(false);
            }
        } else {
            // Normal mode: keep scan-chain state, disable scan-chain
            scan_chain_data_next.write(scan_chain_data_reg.read());
            scan_chain_enable_next.write(false);
        }
    }

    // === Combinational: Scan-chain Output ===
    void comb_scan_chain_output() {
        bool scan_en = scan_chain_enable_reg.read();
        ScanChainFormat sc_data = scan_chain_data_reg.read();

        scan_chain_enable.write(scan_en);

        if (scan_en) {
            // First port gets the data from register
            scan_chain_out[0].write(sc_data);

            // Chain the scan data through all ports
            for (size_t i = 1; i < num_ports; ++i) {
                scan_chain_out[i].write(scan_chain_in[i-1].read());
            }
        } else {
            // Not in scan mode: output default
            ScanChainFormat default_out;
            default_out.ps_id = 0;
            default_out.pd_id = 0;
            default_out.pli_id = 0;
            default_out.plo_id = 0;
            default_out.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
            default_out.enable = false;

            for (size_t i = 0; i < num_ports; ++i) {
                scan_chain_out[i].write(default_out);
            }
        }
    }

    // Trace support
    int trace_id = 0;
    std::string last_state_noc0 = "IDLE";
    std::string last_state_noc1 = "IDLE";
    std::string last_state_noc2 = "IDLE";
    bool trace_init = false;

public:
    void set_trace_id(int id) { trace_id = id; }

private:
    void trace_process() {
        uint32_t tid_noc0 = trace_id + 1;
        uint32_t tid_noc1 = trace_id + 2;
        uint32_t tid_noc2 = trace_id + 3;

        if (!trace_init) {
            TRACE_THREAD_NAME(TRACE_PID::NOC_ROUTER, tid_noc0, "NoC-0 (Tx)");
            TRACE_THREAD_NAME(TRACE_PID::NOC_ROUTER, tid_noc1, "NoC-1 (Tx Write)");
            TRACE_THREAD_NAME(TRACE_PID::NOC_ROUTER, tid_noc2, "NoC-2 (Rx/Tx Read)");

            TRACE_EVENT(last_state_noc0, "NoC0_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, tid_noc0, "{}");
            TRACE_EVENT(last_state_noc1, "NoC1_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, tid_noc1, "{}");
            TRACE_EVENT(last_state_noc2, "NoC2_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, tid_noc2, "{}");
            trace_init = true;
        }

        // NoC-0 State
        std::string current_state_noc0 = req0_fifo_empty_sig.read() ? "IDLE" : "PROCESSING";
        if (current_state_noc0 != last_state_noc0) {
            TRACE_EVENT(last_state_noc0, "NoC0_State", TRACE_END, TRACE_PID::NOC_ROUTER, tid_noc0, "{}");
            TRACE_EVENT(current_state_noc0, "NoC0_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, tid_noc0, "{}");
            last_state_noc0 = current_state_noc0;
        }

        // NoC-1 State
        std::string current_state_noc1 = req1_fifo_empty_sig.read() ? "IDLE" : "PROCESSING";
        if (current_state_noc1 != last_state_noc1) {
            TRACE_EVENT(last_state_noc1, "NoC1_State", TRACE_END, TRACE_PID::NOC_ROUTER, tid_noc1, "{}");
            TRACE_EVENT(current_state_noc1, "NoC1_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, tid_noc1, "{}");
            last_state_noc1 = current_state_noc1;
        }

        // NoC-2 State
        std::string current_state_noc2;
        if (pending_read_reg.read()) {
            current_state_noc2 = "WAITING_RESP";
        } else if (!req2_fifo_empty_sig.read()) {
            current_state_noc2 = "PROCESSING";
        } else {
            current_state_noc2 = "IDLE";
        }

        if (current_state_noc2 != last_state_noc2) {
            TRACE_EVENT(last_state_noc2, "NoC2_State", TRACE_END, TRACE_PID::NOC_ROUTER, tid_noc2, "{}");
            TRACE_EVENT(current_state_noc2, "NoC2_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, tid_noc2, "{}");
            last_state_noc2 = current_state_noc2;
        }
    }
};

} // namespace noc
} // namespace hybridacc

/*

新版 `NoCRouter` 的詳細行為解釋：

### 1. 核心架構 (Core Architecture)

`NoCRouter` 是 NoC 的大腦，負責協調上層控制器與下層 MBUS/PE 之間的通訊。它現在採用 **混合式處理架構 (Hybrid Processing Architecture)**：

*   **數據路徑 (Data Path)**：透過 FIFO 緩衝，支援全雙工流控 (Full-Duplex Flow Control)，保證數據不丟失。
*   **控制路徑 (Control Path)**：透過 Sideband (旁路) 介面，支援高優先級的廣播命令，繞過 FIFO 直接執行。

### 2. 介面定義 (Interfaces)

*   **數據介面 (Data Interface)**：
    *   `req0_in` (Valid/Ready)：接收來自上層的寫入/控制請求 (NoC-0)。
    *   `req1_in` (Valid/Ready)：接收來自上層的讀寫請求 (NoC-1)。
    *   `resp1_out` (Valid/Ready)：回傳讀取結果 (NoC-1)。
*   **控制介面 (Control Interface)**：
    *   `command_mode` & `command_data`：用於發送全局控制命令 (如 Start/Stop PE) 或配置 Scan Chain。
*   **下層介面 (Downstream Interface)**：
    *   `noc_to_bus_req` & `bus_to_noc_resp`：連接到多個 MBUS，使用標準握手協定。

### 3. 詳細行為流程 (Life of a Request)

`process_thread` 是核心執行緒，它在每個時脈週期都會檢查並執行以下邏輯：

#### A. 優先處理 Sideband Command (High Priority)
這是最新的變更點。執行緒會**優先**檢查 `command_mode` 是否為高：

1.  **檢測**：若 `command_mode=1` 且指令類型**不是** `CMD_NOC_SCAN_CHAIN`（例如 `CMD_START_PE`, `CMD_LOAD_PROGRAM`）。
2.  **構建請求**：立即構建一個廣播寫入請求 (`addr=0x100`, `is_w=true`)。
3.  **廣播 (Broadcast)**：
    *   將請求同時送往所有 `MBUS` 端口。
    *   **同步等待**：Router 會暫停並等待，直到**所有** MBUS 都回傳 Ready，確保命令被所有 PE 接收。
4.  **完成**：命令發送完畢後，結束該週期的處理。這意味著 Sideband Command 會暫時阻塞 FIFO 中的數據處理，確保控制命令的即時性。

#### B. 處理 FIFO 數據請求 (Normal Priority)
如果沒有 Sideband Command，Router 才會檢查 `req_fifo`：

1.  **取出請求 (Pop)**：從 FIFO 取出一個 `router_req_t`。
2.  **解碼 (Decode)**：
    *   **SIMD (addr[8]=1)**：將 192-bit 數據切分為 3x64-bit，分別送給 Port 0-2。
    *   **Broadcast (addr[8]=0)**：將低 64-bit 數據複製送給所有端口。
    *   **Command (addr[9]=1)**：視為廣播寫入。
3.  **發送 (Dispatch)**：
    *   將請求送往目標 MBUS 端口。
    *   同樣執行**同步等待**，直到所有目標 MBUS 都接收 (Ready=1)。
4.  **回應收集 (Response Collection)**：
    *   **寫入**：立即生成 `NOC_OK`。
    *   **讀取**：進入收集迴圈，等待所有目標 MBUS 回傳數據，並將其聚合 (Aggregation) 成一個 192-bit 的回應包。
5.  **回傳**：將結果推入 `resp_fifo`。

#### C. Scan Chain 配置 (Configuration)
*   `CMD_NOC_SCAN_CHAIN` 指令由獨立的組合邏輯 (`comb_command_process`) 處理。
*   它不經過 `process_thread`，也不會阻塞數據流，專門用於設定 Router 內部的路由表與 ID。

### 4. 設計特點總結

1.  **雙軌制 (Dual-Track)**：
    *   **數據流**走 FIFO，保證吞吐量與順序。
    *   **控制流**走 Sideband，保證低延遲與高優先級。
2.  **絕對同步 (Strict Synchronization)**：
    *   無論是廣播數據還是命令，Router 都會強制等待所有目標 Ready。這雖然可能被最慢的 PE 拖慢，但保證了系統狀態的一致性 (Lock-step behavior)。
3.  **無阻塞 (Non-Blocking I/O)**：
    *   對上層而言，只要 FIFO 沒滿，就可以一直送數據，不用管下層 PE 是否忙碌。Router 會自動處理內部的背壓 (Backpressure)。

*/