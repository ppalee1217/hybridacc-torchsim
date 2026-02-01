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
    VRDIF<router_req_t> req_ps_in; // PS (Weights)
    VRDIF<router_req_t> req_pd_in; // PD (Activations)
    VRDIF<router_req_t> req_pli_in; // Write PLI
    VRDIF<noc_addr_req_t> req_plo_in; // Read PLO (New)
    VRDOF<router_resp_t> resp_plo_out; // Read Response (Was resp1)

    // ===  NoC MBUS interface ports ===
    // NoC interface ports - using VRDIF/VRDOF
    // Split into NoC-0 (Write only/Split), NoC-1 (Write only), NoC-2 (Read only)
    sc_vector<VRDOF<noc_request_t>> noc_ps_to_bus_req; // PS
    sc_vector<VRDOF<noc_request_t>> noc_pd_to_bus_req; // PD
    sc_vector<VRDOF<noc_request_t>> noc_pli_to_bus_req; // NoC-1: PLI Write
    sc_vector<VRDOF<noc_addr_req_t>> noc_plo_to_bus_req; // NoC-2: PLO Read Request
    sc_vector<VRDIF<noc_response_t>> bus_to_noc_plo_resp; // NoC-2 Response

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
          req_ps_in("req_ps_in"),
          req_pd_in("req_pd_in"),
          req_pli_in("req_pli_in"),
          req_plo_in("req_plo_in"),
          resp_plo_out("resp_plo_out"),
          noc_ps_to_bus_req("noc_ps_to_bus_req", num_ports),
          noc_pd_to_bus_req("noc_pd_to_bus_req", num_ports),
          noc_pli_to_bus_req("noc_pli_to_bus_req", num_ports),
          noc_plo_to_bus_req("noc_plo_to_bus_req", num_ports),
          bus_to_noc_plo_resp("bus_to_noc_plo_resp", num_ports),
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
          ps_fifo("ps_fifo", 4),
          pd_fifo("pd_fifo", 4),
          req1_fifo("req1_fifo", 4),
          req2_fifo("req2_fifo", 4),
          resp_fifo("resp_fifo", 4)
    {
        DEBUG_MSG("[Create] NoCRouter with " << num_ports << " ports", DEBUG_LEVEL_NOC_COMPONENTS);

        // Bind FIFOs
        ps_fifo.clk(clk);
        ps_fifo.reset_n(reset_n);
        ps_fifo.data_in(ps_fifo_in_sig);
        ps_fifo.push(ps_fifo_push_sig);
        ps_fifo.data_out(ps_fifo_out_sig);
        ps_fifo.pop(ps_fifo_pop_sig);
        ps_fifo.empty(ps_fifo_empty_sig);
        ps_fifo.full(ps_fifo_full_sig);

        pd_fifo.clk(clk);
        pd_fifo.reset_n(reset_n);
        pd_fifo.data_in(pd_fifo_in_sig);
        pd_fifo.push(pd_fifo_push_sig);
        pd_fifo.data_out(pd_fifo_out_sig);
        pd_fifo.pop(pd_fifo_pop_sig);
        pd_fifo.empty(pd_fifo_empty_sig);
        pd_fifo.full(pd_fifo_full_sig);

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

        // Request Processing NoC-0 PS (Tx)
        SC_METHOD(process_requests_noc0_ps);
        sensitive << ps_fifo_empty_sig << ps_fifo_out_sig
                  << command_mode << command_data;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_ps_to_bus_req[i].ready_in;
        }

        // Request Processing NoC-0 PD (Tx)
        SC_METHOD(process_requests_noc0_pd);
        sensitive << pd_fifo_empty_sig << pd_fifo_out_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_pd_to_bus_req[i].ready_in;
        }

        // Request Processing NoC PLI (Tx - Write)
        SC_METHOD(process_requests_noc_pli);
        sensitive << req1_fifo_empty_sig << req1_fifo_out_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_pli_to_bus_req[i].ready_in;
        }

        // Request Processing NoC PLO (Tx - Read)
        SC_METHOD(process_requests_noc_plo);
        sensitive << req2_fifo_empty_sig << req2_fifo_out_sig << rx_stall_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_plo_to_bus_req[i].ready_in;
        }

        // Response Processing (Rx - PLO)
        SC_METHOD(process_responses_plo);
        sensitive << pending_read_reg << pending_read_ultra_reg << resp_fifo_full_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << bus_to_noc_plo_resp[i].valid_in << bus_to_noc_plo_resp[i].data_in;
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
        sensitive << req_ps_in.valid_in << req_ps_in.data_in << ps_fifo_full_sig
                  << req_pd_in.valid_in << req_pd_in.data_in << pd_fifo_full_sig
                  << req_pli_in.valid_in << req_pli_in.data_in << req1_fifo_full_sig
                  << req_plo_in.valid_in << req_plo_in.data_in << req2_fifo_full_sig
                  << resp_plo_out.ready_in << resp_fifo_empty_sig << resp_fifo_out_sig;

        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    // Overloaded constructor with default number of PEs
    NoCRouter(sc_module_name name)
        : NoCRouter(name, NUM_PORTS_DEFAULT) // Default to 3 PEs
    {}

private:
    // === FIFOs ===
    hybridacc::pe::FIFO<router_req_t> ps_fifo;
    hybridacc::pe::FIFO<router_req_t> pd_fifo;
    hybridacc::pe::FIFO<router_req_t> req1_fifo;
    hybridacc::pe::FIFO<noc_addr_req_t> req2_fifo;
    hybridacc::pe::FIFO<router_resp_t> resp_fifo;

    // FIFO Signals - PS
    sc_signal<router_req_t> ps_fifo_in_sig;
    sc_signal<bool> ps_fifo_push_sig;
    sc_signal<router_req_t> ps_fifo_out_sig;
    sc_signal<bool> ps_fifo_pop_sig;
    sc_signal<bool> ps_fifo_empty_sig;
    sc_signal<bool> ps_fifo_full_sig;

    // FIFO Signals - PD
    sc_signal<router_req_t> pd_fifo_in_sig;
    sc_signal<bool> pd_fifo_push_sig;
    sc_signal<router_req_t> pd_fifo_out_sig;
    sc_signal<bool> pd_fifo_pop_sig;
    sc_signal<bool> pd_fifo_empty_sig;
    sc_signal<bool> pd_fifo_full_sig;

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
        // Input: Req PS Port -> PS FIFO
        if (req_ps_in.valid_in.read() && !ps_fifo_full_sig.read()) {
            req_ps_in.ready_out.write(true);
            ps_fifo_in_sig.write(req_ps_in.data_in.read());
            ps_fifo_push_sig.write(true);
        } else {
            req_ps_in.ready_out.write(false);
            ps_fifo_push_sig.write(false);
        }

        // Input: Req PD Port -> PD FIFO
        if (req_pd_in.valid_in.read() && !pd_fifo_full_sig.read()) {
            req_pd_in.ready_out.write(true);
            pd_fifo_in_sig.write(req_pd_in.data_in.read());
            pd_fifo_push_sig.write(true);
        } else {
            req_pd_in.ready_out.write(false);
            pd_fifo_push_sig.write(false);
        }

        // Input: Req PLI Port -> Req1 FIFO (Write Only)
        if (req_pli_in.valid_in.read() && !req1_fifo_full_sig.read()) {
            req_pli_in.ready_out.write(true);
            req1_fifo_in_sig.write(req_pli_in.data_in.read());
            req1_fifo_push_sig.write(true);
        } else {
            req_pli_in.ready_out.write(false);
            req1_fifo_push_sig.write(false);
        }

        // Input: Req PLO Port -> Req2 FIFO (Read Only)
        if (req_plo_in.valid_in.read() && !req2_fifo_full_sig.read()) {
            req_plo_in.ready_out.write(true);
            req2_fifo_in_sig.write(req_plo_in.data_in.read());
            req2_fifo_push_sig.write(true);
        } else {
            req_plo_in.ready_out.write(false);
            req2_fifo_push_sig.write(false);
        }

        // Output: Resp FIFO -> Resp PLO Port
        if (!resp_fifo_empty_sig.read()) {
            resp_plo_out.valid_out.write(true);
            resp_plo_out.data_out.write(resp_fifo_out_sig.read());
            if (resp_plo_out.ready_in.read()) {
                resp_fifo_pop_sig.write(true);
            } else {
                resp_fifo_pop_sig.write(false);
            }
        } else {
            resp_plo_out.valid_out.write(false);
            resp_fifo_pop_sig.write(false);
        }
    }

    // === Request Processing NoC-0 PS (Tx) ===
    void process_requests_noc0_ps() {
        // Default outputs
        ps_fifo_pop_sig.write(false);
        for (size_t i = 0; i < num_ports; ++i) {
            noc_ps_to_bus_req[i].valid_out.write(false);
            noc_ps_to_bus_req[i].data_out.write(noc_request_t());
        }

        // 1. Sideband Command (High Priority) -> NoC-0 PS
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
                noc_ps_to_bus_req[i].valid_out.write(true);
                noc_ps_to_bus_req[i].data_out.write(cmd_req);
            }
            return; // Block FIFO requests
        }

        // 2. FIFO Request (NoC-0 PS)
        if (!ps_fifo_empty_sig.read()) {
            router_req_t req = ps_fifo_out_sig.read();

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

                noc_ps_to_bus_req[i].data_out.write(r);
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc_ps_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                for (size_t i = 0; i < num_ports; ++i) {
                    noc_ps_to_bus_req[i].valid_out.write(true);
                }
                ps_fifo_pop_sig.write(true);
            }
        }
    }

    // === Request Processing NoC-0 PD (Tx) ===
    void process_requests_noc0_pd() {
        // Default outputs
        pd_fifo_pop_sig.write(false);
        for (size_t i = 0; i < num_ports; ++i) {
            noc_pd_to_bus_req[i].valid_out.write(false);
            noc_pd_to_bus_req[i].data_out.write(noc_request_t());
        }

        // FIFO Request (NoC-0 PD)
        if (!pd_fifo_empty_sig.read()) {
            router_req_t req = pd_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 8) & 0x1;
            sc_uint<8> base_addr = req.addr & 0xFF;
            size_t mask = req.mask;

            // Broadcast / SIMD
            for (size_t i = 0; i < num_ports; ++i) {
                noc_request_t r;
                if (is_ultra) {
                    assert(num_ports <= NUM_PORTS_DEFAULT);
                    r.data = req.data.range(64*i + 63, 64*i).to_uint64();
                } else { // Broadcast
                    r.data = req.data.range(63, 0).to_uint64();
                }
                r.addr = base_addr;
                r.mask = mask;

                noc_pd_to_bus_req[i].data_out.write(r);
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc_pd_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                for (size_t i = 0; i < num_ports; ++i) {
                    noc_pd_to_bus_req[i].valid_out.write(true);
                }
                pd_fifo_pop_sig.write(true);
            }
        }
    }

    // === Request Processing NoC PLI (Tx) - Write Only ===
    void process_requests_noc_pli() {
        // Default outputs
        req1_fifo_pop_sig.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            noc_pli_to_bus_req[i].valid_out.write(false);
            noc_pli_to_bus_req[i].data_out.write(noc_request_t());
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

                noc_pli_to_bus_req[i].data_out.write(r);
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc_pli_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                for (size_t i = 0; i < num_ports; ++i) {
                    noc_pli_to_bus_req[i].valid_out.write(true);
                }
                req1_fifo_pop_sig.write(true);
            }
            else{
                DEBUG_MSG(" [NoCRouter] NoC-PLI Request not all ready, stalling - " << r << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }
        }
    }

    // === Request Processing NoC PLO (Tx) - Read Only ===
    void process_requests_noc_plo() {
        // Default outputs
        req2_fifo_pop_sig.write(false);
        pending_read_next.write(false);
        pending_read_ultra_next.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            noc_plo_to_bus_req[i].valid_out.write(false);
            noc_plo_to_bus_req[i].data_out.write(noc_addr_req_t());
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
                DEBUG_MSG("[NoCRouter] Received NoC-PLO ULTRA read req addr=0x" << std::hex << req.addr << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }

            // Broadcast / SIMD Logic (Address based)
            noc_addr_req_t r;
            r.addr = req.addr;

            for (size_t i = 0; i < num_ports; ++i) {
                noc_plo_to_bus_req[i].data_out.write(r);
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc_plo_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                for (size_t i = 0; i < num_ports; ++i) {
                    noc_plo_to_bus_req[i].valid_out.write(true);
                }
                req2_fifo_pop_sig.write(true);

                // Set pending read state
                pending_read_next.write(true);
                pending_read_ultra_next.write(is_ultra);
            }
            else{
                DEBUG_MSG(" [NoCRouter] NoC-PLO Request not all ready, stalling" << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }
        }
    }

    // === Response Processing (Rx - PLO) ===
    void process_responses_plo() {
        // Default outputs
        resp_fifo_push_sig.write(false);
        resp_fifo_in_sig.write(router_resp_t());
        rx_stall_sig.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            bus_to_noc_plo_resp[i].ready_out.write(false);
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
            if(bus_to_noc_plo_resp[i].valid_in.read()) {
                valid_rx |= (1ULL << i);
                if ( bus_to_noc_plo_resp[i].data_in.read().status == NOC_RESPONSE_STATUS::NOC_ERROR) {
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
                collected_data.range(64*i + 63, 64*i) = bus_to_noc_plo_resp[i].data_in.read().data;
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
                    collected_data.range(63, 0) = bus_to_noc_plo_resp[i].data_in.read().data;
                }
            }
            if (!resp_received) {
                all_valid = false;
            }
        }

        for (size_t i = 0; i < num_ports; ++i) {
            DEBUG_MSG("[NoCRouter] Checking response from port " << i
                      << ": valid=" << bus_to_noc_plo_resp[i].valid_in.read()
                      << ", data=0x" << std::hex << bus_to_noc_plo_resp[i].data_in.read().data
                      << ", status=" << static_cast<int>(bus_to_noc_plo_resp[i].data_in.read().status)
                      << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
        }

        if (all_valid) {
            if (!resp_fifo_full_sig.read()) {
                // Accept responses
                for (size_t i = 0; i < num_ports; ++i) {
                    if (valid_rx & (1ULL << i)) {
                        bus_to_noc_plo_resp[i].ready_out.write(true);
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
        std::string current_state_noc0 = (ps_fifo_empty_sig.read() && pd_fifo_empty_sig.read()) ? "IDLE" : "PROCESSING";
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
