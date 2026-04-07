#pragma once

#include <cstdint>
#include <systemc>
#include "Utils/utils.hpp"
#include "Utils/FIFO.hpp" // Include FIFO

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace noc {

static const int NUM_PORTS_DEFAULT = 3;

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

template <unsigned NUM_PORTS = NUM_PORTS_DEFAULT, unsigned PORT_WIDTH_BITS = 64>
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
    VRDIF<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> noc_ps_in; // PS (Weights)
    VRDIF<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> noc_pd_in; // PD (Activations)
    VRDIF<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> noc_pli_in; // Write PLI
    VRDIF<noc_addr_req_t> noc_plo_in; // Read PLO (New)
    VRDOF<response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>>> noc_plo_out; // Read Response (Was resp1)

    // ===  NoC MBUS interface ports ===
    // NoC interface ports - using VRDIF/VRDOF
    // Split into PS, PD, PLI, PLO channels
    sc_vector<VRDOF<noc_request_t>> noc_ps_to_bus_req; // PS Write
    sc_vector<VRDOF<noc_request_t>> noc_pd_to_bus_req; // PD Write
    sc_vector<VRDOF<noc_request_t>> noc_pli_to_bus_req; // PLI Write
    sc_vector<VRDOF<noc_addr_req_t>> noc_plo_to_bus_req; // PLO Read Request
    sc_vector<VRDIF<noc_response_t>> bus_to_noc_plo_resp; // PLO Read Response

    // ID, mode, enable scan-chain ports
    sc_out<bool> scan_chain_enable; // broadcast to all ports
    sc_vector<sc_in<ScanChainFormat>> scan_chain_in;
    sc_vector<sc_out<ScanChainFormat>> scan_chain_out;

    size_t num_ports;
    size_t noc_fifo_depth;

    SC_HAS_PROCESS(NoCRouter);
    NoCRouter(sc_module_name name, size_t num_ports, size_t noc_fifo_depth = 4)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          noc_ps_in("noc_ps_in"),
          noc_pd_in("noc_pd_in"),
          noc_pli_in("noc_pli_in"),
          noc_plo_in("noc_plo_in"),
          noc_plo_out("noc_plo_out"),
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
          ps_fifo("ps_fifo", noc_fifo_depth),
          pd_fifo("pd_fifo", noc_fifo_depth),
          pli_fifo("pli_fifo", noc_fifo_depth),
          plo_fifo("plo_fifo", noc_fifo_depth),
          resp_fifo("resp_fifo", noc_fifo_depth),
          noc_fifo_depth(noc_fifo_depth)

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
        ps_fifo.clear(zero_signal);

        pd_fifo.clk(clk);
        pd_fifo.reset_n(reset_n);
        pd_fifo.data_in(pd_fifo_in_sig);
        pd_fifo.push(pd_fifo_push_sig);
        pd_fifo.data_out(pd_fifo_out_sig);
        pd_fifo.pop(pd_fifo_pop_sig);
        pd_fifo.empty(pd_fifo_empty_sig);
        pd_fifo.full(pd_fifo_full_sig);
        pd_fifo.clear(zero_signal);

        pli_fifo.clk(clk);
        pli_fifo.reset_n(reset_n);
        pli_fifo.data_in(pli_fifo_in_sig);
        pli_fifo.push(pli_fifo_push_sig);
        pli_fifo.data_out(pli_fifo_out_sig);
        pli_fifo.pop(pli_fifo_pop_sig);
        pli_fifo.empty(pli_fifo_empty_sig);
        pli_fifo.full(pli_fifo_full_sig);
        pli_fifo.clear(zero_signal);

        plo_fifo.clk(clk);
        plo_fifo.reset_n(reset_n);
        plo_fifo.data_in(plo_fifo_in_sig);
        plo_fifo.push(plo_fifo_push_sig);
        plo_fifo.data_out(plo_fifo_out_sig);
        plo_fifo.pop(plo_fifo_pop_sig);
        plo_fifo.empty(plo_fifo_empty_sig);
        plo_fifo.full(plo_fifo_full_sig);
        plo_fifo.clear(zero_signal);

        resp_fifo.clk(clk);
        resp_fifo.reset_n(reset_n);
        resp_fifo.data_in(resp_fifo_in_sig);
        resp_fifo.push(resp_fifo_push_sig);
        resp_fifo.data_out(resp_fifo_out_sig);
        resp_fifo.pop(resp_fifo_pop_sig);
        resp_fifo.empty(resp_fifo_empty_sig);
        resp_fifo.full(resp_fifo_full_sig);
        resp_fifo.clear(zero_signal);

        // Register sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Request Processing NoC PS (Tx)
        SC_METHOD(process_requests_noc_ps);
        sensitive << ps_fifo_empty_sig << ps_fifo_out_sig
                  << command_mode << command_data;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_ps_to_bus_req[i].ready_in;
        }

        // Request Processing NoC PD (Tx)
        SC_METHOD(process_requests_noc_pd);
        sensitive << pd_fifo_empty_sig << pd_fifo_out_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_pd_to_bus_req[i].ready_in;
        }

        // Request Processing NoC PLI (Tx - Write)
        SC_METHOD(process_requests_noc_pli);
        sensitive << pli_fifo_empty_sig << pli_fifo_out_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_pli_to_bus_req[i].ready_in;
        }

        // Request Processing NoC PLO (Tx - Read)
        SC_METHOD(process_requests_noc_plo);
        sensitive << plo_fifo_empty_sig << plo_fifo_out_sig << rx_stall_sig;
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
        sensitive << noc_ps_in.valid_in << noc_ps_in.data_in << ps_fifo_full_sig
              << noc_pd_in.valid_in << noc_pd_in.data_in << pd_fifo_full_sig
              << noc_pli_in.valid_in << noc_pli_in.data_in << pli_fifo_full_sig
              << noc_plo_in.valid_in << noc_plo_in.data_in << plo_fifo_full_sig
              << noc_plo_out.ready_in << resp_fifo_empty_sig << resp_fifo_out_sig;

        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    // Overloaded constructor with default number of PEs
    NoCRouter(sc_module_name name)
        : NoCRouter(name, NUM_PORTS_DEFAULT) // Default to 3 PEs
    {}

private:
    // === FIFOs ===
    hybridacc::FIFO<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> ps_fifo;
    hybridacc::FIFO<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> pd_fifo;
    hybridacc::FIFO<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> pli_fifo;
    hybridacc::FIFO<noc_addr_req_t> plo_fifo;
    hybridacc::FIFO<response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>>> resp_fifo;

    // FIFO Signals - PS
    sc_signal<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> ps_fifo_in_sig;
    sc_signal<bool> ps_fifo_push_sig;
    sc_signal<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> ps_fifo_out_sig;
    sc_signal<bool> ps_fifo_pop_sig;
    sc_signal<bool> ps_fifo_empty_sig;
    sc_signal<bool> ps_fifo_full_sig;

    // FIFO Signals - PD
    sc_signal<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> pd_fifo_in_sig;
    sc_signal<bool> pd_fifo_push_sig;
    sc_signal<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> pd_fifo_out_sig;
    sc_signal<bool> pd_fifo_pop_sig;
    sc_signal<bool> pd_fifo_empty_sig;
    sc_signal<bool> pd_fifo_full_sig;

    // FIFO Signals - NoC1
    sc_signal<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> pli_fifo_in_sig;
    sc_signal<bool> pli_fifo_push_sig;
    sc_signal<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> pli_fifo_out_sig;
    sc_signal<bool> pli_fifo_pop_sig;
    sc_signal<bool> pli_fifo_empty_sig;
    sc_signal<bool> pli_fifo_full_sig;

    // FIFO Signals - NoC2 (Read Req)
    sc_signal<noc_addr_req_t> plo_fifo_in_sig;
    sc_signal<bool> plo_fifo_push_sig;
    sc_signal<noc_addr_req_t> plo_fifo_out_sig;
    sc_signal<bool> plo_fifo_pop_sig;
    sc_signal<bool> plo_fifo_empty_sig;
    sc_signal<bool> plo_fifo_full_sig;

    sc_signal<response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>>> resp_fifo_in_sig;
    sc_signal<bool> resp_fifo_push_sig;
    sc_signal<response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>>> resp_fifo_out_sig;
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

    sc_signal<bool> zero_signal;

    // === Sequential Process ===
    void seq_process() {
        // Reset initialization
        zero_signal.write(false);
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
        if (noc_ps_in.valid_in.read() && (!ps_fifo_full_sig.read())) {
            noc_ps_in.ready_out.write(true);
            ps_fifo_in_sig.write(noc_ps_in.data_in.read());
            ps_fifo_push_sig.write(true);
        } else {
            noc_ps_in.ready_out.write(false);
            ps_fifo_push_sig.write(false);
        }

        // Input: Req PD Port -> PD FIFO
        if (noc_pd_in.valid_in.read() && (!pd_fifo_full_sig.read())) {
            noc_pd_in.ready_out.write(true);
            pd_fifo_in_sig.write(noc_pd_in.data_in.read());
            pd_fifo_push_sig.write(true);
        } else {
            noc_pd_in.ready_out.write(false);
            pd_fifo_push_sig.write(false);
        }

        // Input: Req PLI Port -> Req1 FIFO (Write Only)
        if (noc_pli_in.valid_in.read() && (!pli_fifo_full_sig.read())) {
            noc_pli_in.ready_out.write(true);
            pli_fifo_in_sig.write(noc_pli_in.data_in.read());
            pli_fifo_push_sig.write(true);
        } else {
            noc_pli_in.ready_out.write(false);
            pli_fifo_push_sig.write(false);
        }

        // Input: Req PLO Port -> Req2 FIFO (Read Only)
        if (noc_plo_in.valid_in.read() && (!plo_fifo_full_sig.read())) {
            noc_plo_in.ready_out.write(true);
            plo_fifo_in_sig.write(noc_plo_in.data_in.read());
            plo_fifo_push_sig.write(true);
        } else {
            noc_plo_in.ready_out.write(false);
            plo_fifo_push_sig.write(false);
        }

        // Output: Resp FIFO -> Resp PLO Port
        if (!resp_fifo_empty_sig.read()) {
            noc_plo_out.valid_out.write(true);
            noc_plo_out.data_out.write(resp_fifo_out_sig.read());
            if (noc_plo_out.ready_in.read()) {
                resp_fifo_pop_sig.write(true);
            } else {
                resp_fifo_pop_sig.write(false);
            }
        } else {
            noc_plo_out.valid_out.write(false);
            resp_fifo_pop_sig.write(false);
        }
    }

    // === Request Processing NoC PS (Tx) ===
    void process_requests_noc_ps() {
        // Default outputs
        ps_fifo_pop_sig.write(false);
        for (size_t i = 0; i < num_ports; ++i) {
            noc_ps_to_bus_req[i].valid_out.write(false);
            noc_ps_to_bus_req[i].data_out.write(noc_request_t());
        }

        // 1. Sideband Command (High Priority) -> NoC PS
        bool cmd_active = command_mode.read();
        sc_uint<32> cmd_val = command_data.read();
        message_command_t cmd_type = static_cast<message_command_t>(cmd_val.range(3, 0).to_uint());
        bool is_pe_cmd = cmd_active && (cmd_type != message_command_t::CMD_NOC_SCAN_CHAIN);

        if (is_pe_cmd) {
            noc_request_t cmd_req;
            cmd_req.addr = 0x40; // cmd bit at addr[6]
            cmd_req.data = cmd_val;
            // cmd_req.is_w = true; // Implicit Write

            for (size_t i = 0; i < num_ports; ++i) {
                noc_ps_to_bus_req[i].valid_out.write(true);
                noc_ps_to_bus_req[i].data_out.write(cmd_req);
            }
            return; // Block FIFO requests
        }

        // 2. FIFO Request (NoC PS)
        if (!ps_fifo_empty_sig.read()) {
            request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t> req = ps_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 6) & 0x1;
            sc_uint<8> base_addr = (req.addr & 0x80 ? 0x40 : 0x00) | (req.addr & 0x3F);
            // bool is_write = req.is_w; // Implicit Write
            size_t mask = req.mask;

            // Broadcast / SIMD
            for (size_t i = 0; i < num_ports; ++i) {
                noc_request_t r;
                if (is_ultra) {
                    assert(num_ports <= NUM_PORTS_DEFAULT); // Max 4 ports for ultra mode (since NUM_PORTS*PORT_WIDTH_BITS/64=3)
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

    // === Request Processing NoC PD (Tx) ===
    void process_requests_noc_pd() {
        // Default outputs
        pd_fifo_pop_sig.write(false);
        for (size_t i = 0; i < num_ports; ++i) {
            noc_pd_to_bus_req[i].valid_out.write(false);
            noc_pd_to_bus_req[i].data_out.write(noc_request_t());
        }

        // FIFO Request (NoC PD)
        if (!pd_fifo_empty_sig.read()) {
            request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t> req = pd_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 6) & 0x1;
            sc_uint<8> base_addr = (req.addr & 0x80 ? 0x40 : 0x00) | (req.addr & 0x3F);
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
        pli_fifo_pop_sig.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            noc_pli_to_bus_req[i].valid_out.write(false);
            noc_pli_to_bus_req[i].data_out.write(noc_request_t());
        }

        // FIFO Request (NoC-PLI)
        if (!pli_fifo_empty_sig.read()) {
            request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t> req = pli_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 6) & 0x1;
            sc_uint<8> base_addr = (req.addr & 0x80 ? 0x40 : 0x00) | (req.addr & 0x3F);
            // bool is_write = req.is_w; // Always Write for NoC-PLI
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
                pli_fifo_pop_sig.write(true);
                DEBUG_MSG("[NoCRouter] NoC-PLI request accepted for transfer - " << r << (is_ultra ? " (ULTRA)" : " (BROADCAST)"), DEBUG_LEVEL_NOC_COMPONENTS);
            }
            else{
                DEBUG_MSG(" [NoCRouter] NoC-PLI Request not all ready, stalling - " << r << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }
        }
    }

    // === Request Processing NoC PLO (Tx) - Read Only ===
    void process_requests_noc_plo() {
        // Default outputs
        plo_fifo_pop_sig.write(false);
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

        // FIFO Request (NoC-PLO)
        if (!plo_fifo_empty_sig.read()) {
            noc_addr_req_t req = plo_fifo_out_sig.read();

            // Decode
            bool is_ultra = (req.addr >> 6) & 0x1;

            if (is_ultra) {
                DEBUG_MSG("[NoCRouter] Received NoC-PLO ULTRA read req addr=0x" << std::hex << req.addr << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }

            // Broadcast / SIMD Logic (Address based)
            noc_addr_req_t r;
            r.addr = (req.addr & 0x80 ? 0x40 : 0x00) | (req.addr & 0x3F);

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
                plo_fifo_pop_sig.write(true);

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
        resp_fifo_in_sig.write(response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>>());
        rx_stall_sig.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            bus_to_noc_plo_resp[i].ready_out.write(false);
        }

        bool is_pending_read = pending_read_reg.read();
        bool is_ultra = pending_read_ultra_reg.read();

        if (!is_pending_read) return;

        // Check if all expected responses are valid
        bool all_valid = true;
        sc_biguint<NUM_PORTS*PORT_WIDTH_BITS> collected_data = 0;
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
                response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>> final_resp;
                final_resp.data = collected_data;
                // Note: response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>> doesn't have status field in current definition,
                // assuming data is enough or error handling is done elsewhere/ignored for now.

                resp_fifo_push_sig.write(true);
                resp_fifo_in_sig.write(final_resp);

                DEBUG_MSG("[NoCRouter] Collected NoC-PLO Response - Data: 0x"
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
    uint32_t trace_pid = static_cast<uint32_t>(TRACE_PID::NOC_ROUTER);
    std::string last_state_noc_ps = "IDLE";
    std::string last_state_noc_pd = "IDLE";
    std::string last_state_noc_pli = "IDLE";
    std::string last_state_noc_plo = "IDLE";
    std::string last_state_noc_ps_fifo = "IDLE";
    std::string last_state_noc_pd_fifo = "IDLE";
    std::string last_state_noc_pli_fifo = "IDLE";
    std::string last_state_noc_plo_fifo = "IDLE";
    std::string last_state_noc_resp_fifo = "IDLE";
    std::string last_state_noc_ps_in = "IDLE";
    std::string last_state_noc_pd_in = "IDLE";
    std::string last_state_noc_pli_in = "IDLE";
    std::string last_state_noc_plo_in = "IDLE";
    std::string last_state_noc_ps_tx = "IDLE";
    std::string last_state_noc_pd_tx = "IDLE";
    std::string last_state_noc_pli_tx = "IDLE";
    std::string last_state_noc_plo_tx = "IDLE";
    std::string last_state_noc_plo_rx = "IDLE";
    std::string last_state_noc_plo_out = "IDLE";
    std::string last_args_noc_ps_tx = "{}";
    std::string last_args_noc_pd_tx = "{}";
    std::string last_args_noc_pli_tx = "{}";
    std::string last_args_noc_plo_tx = "{}";
    std::string last_args_noc_plo_rx = "{}";
    std::string last_args_noc_plo_out = "{}";
    bool trace_init = false;

public:
    void set_trace_id(int id) { trace_id = id; }
    void set_trace_context(uint32_t pid, int tid_base) {
        trace_pid = pid;
        trace_id = tid_base;
        trace_init = false;
        last_state_noc_ps = "IDLE";
        last_state_noc_pd = "IDLE";
        last_state_noc_pli = "IDLE";
        last_state_noc_plo = "IDLE";
        last_state_noc_ps_fifo = "IDLE";
        last_state_noc_pd_fifo = "IDLE";
        last_state_noc_pli_fifo = "IDLE";
        last_state_noc_plo_fifo = "IDLE";
        last_state_noc_resp_fifo = "IDLE";
        last_state_noc_ps_in = "IDLE";
        last_state_noc_pd_in = "IDLE";
        last_state_noc_pli_in = "IDLE";
        last_state_noc_plo_in = "IDLE";
        last_state_noc_ps_tx = "IDLE";
        last_state_noc_pd_tx = "IDLE";
        last_state_noc_pli_tx = "IDLE";
        last_state_noc_plo_tx = "IDLE";
        last_state_noc_plo_rx = "IDLE";
        last_state_noc_plo_out = "IDLE";
        last_args_noc_ps_tx = "{}";
        last_args_noc_pd_tx = "{}";
        last_args_noc_pli_tx = "{}";
        last_args_noc_plo_tx = "{}";
        last_args_noc_plo_rx = "{}";
        last_args_noc_plo_out = "{}";
    }
    int get_trace_num() { return 40; }

private:
    void trace_process() {
        uint32_t tid_noc_ps = trace_id + 1;
        uint32_t tid_noc_pd = trace_id + 2;
        uint32_t tid_noc_pli = trace_id + 3;
        uint32_t tid_noc_plo = trace_id + 4;
        uint32_t tid_noc_ps_fifo = trace_id + 11;
        uint32_t tid_noc_pd_fifo = trace_id + 12;
        uint32_t tid_noc_pli_fifo = trace_id + 13;
        uint32_t tid_noc_plo_fifo = trace_id + 14;
        uint32_t tid_noc_resp_fifo = trace_id + 15;
        uint32_t tid_noc_ps_in = trace_id + 21;
        uint32_t tid_noc_pd_in = trace_id + 22;
        uint32_t tid_noc_pli_in = trace_id + 23;
        uint32_t tid_noc_plo_in = trace_id + 24;
        uint32_t tid_noc_ps_tx = trace_id + 31;
        uint32_t tid_noc_pd_tx = trace_id + 32;
        uint32_t tid_noc_pli_tx = trace_id + 33;
        uint32_t tid_noc_plo_tx = trace_id + 34;
        uint32_t tid_noc_plo_rx = trace_id + 35;
        uint32_t tid_noc_plo_out = trace_id + 36;

        auto bool_to_json = [](bool v) { return v ? "true" : "false"; };
        auto u64_hex_json = [](uint64_t v) {
            return std::string("\"") + sc_uint<64>(v).to_string(SC_HEX) + "\"";
        };
        auto trace_state = [&](std::string& last, const std::string& current,
                               const std::string& cat, uint32_t tid,
                               const std::string& args) {
            if (current != last) {
                TRACE_EVENT(last, cat, TRACE_END, trace_pid, tid, "{}");
                TRACE_EVENT(current, cat, TRACE_BEGIN, trace_pid, tid, args);
                last = current;
            }
        };
        auto trace_state_with_args = [&](std::string& last_state, std::string& last_args,
                                         const std::string& current,
                                         const std::string& cat, uint32_t tid,
                                         const std::string& args) {
            if (current != last_state || args != last_args) {
                TRACE_EVENT(last_state, cat, TRACE_END, trace_pid, tid, "{}");
                TRACE_EVENT(current, cat, TRACE_BEGIN, trace_pid, tid, args);
                last_state = current;
                last_args = args;
            }
        };
        auto fifo_state = [&](bool empty, bool full, bool push, bool pop) {
            if (push && pop) return std::string("PUSH_POP");
            if (push) return std::string("PUSH");
            if (pop) return std::string("POP");
            if (full) return std::string("FULL");
            if (empty) return std::string("EMPTY");
            return std::string("HAS_DATA");
        };
        auto fifo_args = [&](bool empty, bool full, bool push, bool pop) {
            return std::string("{\"empty\": ") + bool_to_json(empty)
                + ", \"full\": " + bool_to_json(full)
                + ", \"push\": " + bool_to_json(push)
                + ", \"pop\": " + bool_to_json(pop) + "}";
        };

        if (!trace_init) {
            TRACE_THREAD_NAME(trace_pid, tid_noc_ps, "NoC-PS (Tx Write)");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pd, "NoC-PD (Tx Write)");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pli, "NoC-PLI (Tx Write)");
            TRACE_THREAD_NAME(trace_pid, tid_noc_plo, "NoC-PLO (Rx/Tx Read)");
            TRACE_THREAD_NAME(trace_pid, tid_noc_ps_fifo, "NoC-PS FIFO");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pd_fifo, "NoC-PD FIFO");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pli_fifo, "NoC-PLI FIFO");
            TRACE_THREAD_NAME(trace_pid, tid_noc_plo_fifo, "NoC-PLO FIFO");
            TRACE_THREAD_NAME(trace_pid, tid_noc_resp_fifo, "NoC-RESP FIFO");
            TRACE_THREAD_NAME(trace_pid, tid_noc_ps_in, "NoC-PS In");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pd_in, "NoC-PD In");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pli_in, "NoC-PLI In");
            TRACE_THREAD_NAME(trace_pid, tid_noc_plo_in, "NoC-PLO In");
            TRACE_THREAD_NAME(trace_pid, tid_noc_ps_tx, "NoC-PS Tx");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pd_tx, "NoC-PD Tx");
            TRACE_THREAD_NAME(trace_pid, tid_noc_pli_tx, "NoC-PLI Tx");
            TRACE_THREAD_NAME(trace_pid, tid_noc_plo_tx, "NoC-PLO Tx");
            TRACE_THREAD_NAME(trace_pid, tid_noc_plo_rx, "NoC-PLO Rx");
            TRACE_THREAD_NAME(trace_pid, tid_noc_plo_out, "NoC-PLO Out");

            TRACE_EVENT(last_state_noc_ps, "NoC_PS_State", TRACE_BEGIN, trace_pid, tid_noc_ps, "{}");
            TRACE_EVENT(last_state_noc_pd, "NoC_PD_State", TRACE_BEGIN, trace_pid, tid_noc_pd, "{}");
            TRACE_EVENT(last_state_noc_pli, "NoC_PLI_State", TRACE_BEGIN, trace_pid, tid_noc_pli, "{}");
            TRACE_EVENT(last_state_noc_plo, "NoC_PLO_State", TRACE_BEGIN, trace_pid, tid_noc_plo, "{}");
            TRACE_EVENT(last_state_noc_ps_fifo, "NoC_PS_FIFO", TRACE_BEGIN, trace_pid, tid_noc_ps_fifo, "{}");
            TRACE_EVENT(last_state_noc_pd_fifo, "NoC_PD_FIFO", TRACE_BEGIN, trace_pid, tid_noc_pd_fifo, "{}");
            TRACE_EVENT(last_state_noc_pli_fifo, "NoC_PLI_FIFO", TRACE_BEGIN, trace_pid, tid_noc_pli_fifo, "{}");
            TRACE_EVENT(last_state_noc_plo_fifo, "NoC_PLO_FIFO", TRACE_BEGIN, trace_pid, tid_noc_plo_fifo, "{}");
            TRACE_EVENT(last_state_noc_resp_fifo, "NoC_RESP_FIFO", TRACE_BEGIN, trace_pid, tid_noc_resp_fifo, "{}");
            TRACE_EVENT(last_state_noc_ps_in, "NoC_PS_IN", TRACE_BEGIN, trace_pid, tid_noc_ps_in, "{}");
            TRACE_EVENT(last_state_noc_pd_in, "NoC_PD_IN", TRACE_BEGIN, trace_pid, tid_noc_pd_in, "{}");
            TRACE_EVENT(last_state_noc_pli_in, "NoC_PLI_IN", TRACE_BEGIN, trace_pid, tid_noc_pli_in, "{}");
            TRACE_EVENT(last_state_noc_plo_in, "NoC_PLO_IN", TRACE_BEGIN, trace_pid, tid_noc_plo_in, "{}");
            TRACE_EVENT(last_state_noc_ps_tx, "NoC_PS_TX", TRACE_BEGIN, trace_pid, tid_noc_ps_tx, "{}");
            TRACE_EVENT(last_state_noc_pd_tx, "NoC_PD_TX", TRACE_BEGIN, trace_pid, tid_noc_pd_tx, "{}");
            TRACE_EVENT(last_state_noc_pli_tx, "NoC_PLI_TX", TRACE_BEGIN, trace_pid, tid_noc_pli_tx, "{}");
            TRACE_EVENT(last_state_noc_plo_tx, "NoC_PLO_TX", TRACE_BEGIN, trace_pid, tid_noc_plo_tx, "{}");
            TRACE_EVENT(last_state_noc_plo_rx, "NoC_PLO_RX", TRACE_BEGIN, trace_pid, tid_noc_plo_rx, "{}");
            TRACE_EVENT(last_state_noc_plo_out, "NoC_PLO_OUT", TRACE_BEGIN, trace_pid, tid_noc_plo_out, "{}");
            trace_init = true;
        }

        // NoC-PS State
        std::string current_state_noc_ps = ps_fifo_empty_sig.read() ? "IDLE" : "PROCESSING";
        if (current_state_noc_ps != last_state_noc_ps) {
            TRACE_EVENT(last_state_noc_ps, "NoC_PS_State", TRACE_END, trace_pid, tid_noc_ps, "{}");
            TRACE_EVENT(current_state_noc_ps, "NoC_PS_State", TRACE_BEGIN, trace_pid, tid_noc_ps, "{}");
            last_state_noc_ps = current_state_noc_ps;
        }

        // NoC-PD State
        std::string current_state_noc_pd = pd_fifo_empty_sig.read() ? "IDLE" : "PROCESSING";
        if (current_state_noc_pd != last_state_noc_pd) {
            TRACE_EVENT(last_state_noc_pd, "NoC_PD_State", TRACE_END, trace_pid, tid_noc_pd, "{}");
            TRACE_EVENT(current_state_noc_pd, "NoC_PD_State", TRACE_BEGIN, trace_pid, tid_noc_pd, "{}");
            last_state_noc_pd = current_state_noc_pd;
        }

        // NoC-PLI State
        std::string current_state_noc_pli = pli_fifo_empty_sig.read() ? "IDLE" : "PROCESSING";
        if (current_state_noc_pli != last_state_noc_pli) {
            TRACE_EVENT(last_state_noc_pli, "NoC_PLI_State", TRACE_END, trace_pid, tid_noc_pli, "{}");
            TRACE_EVENT(current_state_noc_pli, "NoC_PLI_State", TRACE_BEGIN, trace_pid, tid_noc_pli, "{}");
            last_state_noc_pli = current_state_noc_pli;
        }

        // NoC-PLO State
        std::string current_state_noc_plo;
        if (pending_read_reg.read()) {
            if (rx_stall_sig.read() && resp_fifo_full_sig.read()) {
                current_state_noc_plo = "STALL_RESP_FIFO_FULL";
            } else if (rx_stall_sig.read()) {
                current_state_noc_plo = "WAITING_RESP";
            } else {
                current_state_noc_plo = "COLLECT_RESP";
            }
        } else if (!plo_fifo_empty_sig.read()) {
            current_state_noc_plo = "PROCESSING";
        } else {
            current_state_noc_plo = "IDLE";
        }

        if (current_state_noc_plo != last_state_noc_plo) {
            TRACE_EVENT(last_state_noc_plo, "NoC_PLO_State", TRACE_END, trace_pid, tid_noc_plo, "{}");
            TRACE_EVENT(current_state_noc_plo, "NoC_PLO_State", TRACE_BEGIN, trace_pid, tid_noc_plo, "{}");
            last_state_noc_plo = current_state_noc_plo;
        }

        // FIFO States (Issue/Response)
        bool ps_empty = ps_fifo_empty_sig.read();
        bool ps_full = ps_fifo_full_sig.read();
        bool ps_push = ps_fifo_push_sig.read();
        bool ps_pop = ps_fifo_pop_sig.read();
        trace_state(last_state_noc_ps_fifo,
                    fifo_state(ps_empty, ps_full, ps_push, ps_pop),
                    "NoC_PS_FIFO", tid_noc_ps_fifo,
                    fifo_args(ps_empty, ps_full, ps_push, ps_pop));

        bool pd_empty = pd_fifo_empty_sig.read();
        bool pd_full = pd_fifo_full_sig.read();
        bool pd_push = pd_fifo_push_sig.read();
        bool pd_pop = pd_fifo_pop_sig.read();
        trace_state(last_state_noc_pd_fifo,
                    fifo_state(pd_empty, pd_full, pd_push, pd_pop),
                    "NoC_PD_FIFO", tid_noc_pd_fifo,
                    fifo_args(pd_empty, pd_full, pd_push, pd_pop));

        bool pli_empty = pli_fifo_empty_sig.read();
        bool pli_full = pli_fifo_full_sig.read();
        bool pli_push = pli_fifo_push_sig.read();
        bool pli_pop = pli_fifo_pop_sig.read();

        if (pli_push) {
            request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t> req_in = pli_fifo_in_sig.read();
            bool is_ultra_in = (req_in.addr >> 6) & 0x1;
            sc_uint<8> base_addr_in = (req_in.addr & 0x80 ? 0x40 : 0x00) | (req_in.addr & 0x3F);

            noc_request_t enq;
            if (is_ultra_in) {
                const size_t lane = (num_ports > 0) ? (num_ports - 1) : 0;
                enq.data = req_in.data.range(64*lane + 63, 64*lane).to_uint64();
            } else {
                enq.data = req_in.data.range(63, 0).to_uint64();
            }
            enq.addr = base_addr_in;
            enq.mask = req_in.mask;

            DEBUG_MSG("[NoCRouter] Enqueued NoC-PLI req" << enq
                      << (is_ultra_in ? " (ULTRA/IN)" : " (BROADCAST/IN)"),
                      DEBUG_LEVEL_NOC_COMPONENTS);
        }

        if (pli_pop && !pli_empty) {
            request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t> req = pli_fifo_out_sig.read();

            bool is_ultra = (req.addr >> 6) & 0x1;
            sc_uint<8> base_addr = (req.addr & 0x80 ? 0x40 : 0x00) | (req.addr & 0x3F);
            size_t mask = req.mask;

            noc_request_t fired;
            if (is_ultra) {
                const size_t lane = (num_ports > 0) ? (num_ports - 1) : 0;
                fired.data = req.data.range(64*lane + 63, 64*lane).to_uint64();
            } else {
                fired.data = req.data.range(63, 0).to_uint64();
            }
            fired.addr = base_addr;
            fired.mask = mask;

            DEBUG_MSG("[NoCRouter] Sent NoC-PLI write req" << fired
                      << (is_ultra ? " (ULTRA/FIRE)" : " (BROADCAST/FIRE)"),
                      DEBUG_LEVEL_NOC_COMPONENTS);
        }

        trace_state(last_state_noc_pli_fifo,
                    fifo_state(pli_empty, pli_full, pli_push, pli_pop),
                    "NoC_PLI_FIFO", tid_noc_pli_fifo,
                    fifo_args(pli_empty, pli_full, pli_push, pli_pop));

        bool plo_empty = plo_fifo_empty_sig.read();
        bool plo_full = plo_fifo_full_sig.read();
        bool plo_push = plo_fifo_push_sig.read();
        bool plo_pop = plo_fifo_pop_sig.read();
        trace_state(last_state_noc_plo_fifo,
                    fifo_state(plo_empty, plo_full, plo_push, plo_pop),
                    "NoC_PLO_FIFO", tid_noc_plo_fifo,
                    fifo_args(plo_empty, plo_full, plo_push, plo_pop));

        bool resp_empty = resp_fifo_empty_sig.read();
        bool resp_full = resp_fifo_full_sig.read();
        bool resp_push = resp_fifo_push_sig.read();
        bool resp_pop = resp_fifo_pop_sig.read();
        trace_state(last_state_noc_resp_fifo,
                    fifo_state(resp_empty, resp_full, resp_push, resp_pop),
                    "NoC_RESP_FIFO", tid_noc_resp_fifo,
                    fifo_args(resp_empty, resp_full, resp_push, resp_pop));

        // Input transfer (Issue to FIFO)
        bool ps_in_valid = noc_ps_in.valid_in.read();
        std::string ps_in_state = ps_in_valid ? (ps_full ? "IN_STALL" : "IN_XFER") : "IN_IDLE";
        trace_state(last_state_noc_ps_in, ps_in_state, "NoC_PS_IN", tid_noc_ps_in,
                    std::string("{\"valid\": ") + bool_to_json(ps_in_valid)
                    + ", \"full\": " + bool_to_json(ps_full)
                    + ", \"push\": " + bool_to_json(ps_push) + "}");

        bool pd_in_valid = noc_pd_in.valid_in.read();
        std::string pd_in_state = pd_in_valid ? (pd_full ? "IN_STALL" : "IN_XFER") : "IN_IDLE";
        trace_state(last_state_noc_pd_in, pd_in_state, "NoC_PD_IN", tid_noc_pd_in,
                    std::string("{\"valid\": ") + bool_to_json(pd_in_valid)
                    + ", \"full\": " + bool_to_json(pd_full)
                    + ", \"push\": " + bool_to_json(pd_push) + "}");

        bool pli_in_valid = noc_pli_in.valid_in.read();
        std::string pli_in_state = pli_in_valid ? (pli_full ? "IN_STALL" : "IN_XFER") : "IN_IDLE";
        trace_state(last_state_noc_pli_in, pli_in_state, "NoC_PLI_IN", tid_noc_pli_in,
                    std::string("{\"valid\": ") + bool_to_json(pli_in_valid)
                    + ", \"full\": " + bool_to_json(pli_full)
                    + ", \"push\": " + bool_to_json(pli_push) + "}");

        bool plo_in_valid = noc_plo_in.valid_in.read();
        std::string plo_in_state = plo_in_valid ? (plo_full ? "IN_STALL" : "IN_XFER") : "IN_IDLE";
        trace_state(last_state_noc_plo_in, plo_in_state, "NoC_PLO_IN", tid_noc_plo_in,
                    std::string("{\"valid\": ") + bool_to_json(plo_in_valid)
                    + ", \"full\": " + bool_to_json(plo_full)
                    + ", \"push\": " + bool_to_json(plo_push) + "}");

        // Output transfer (FIFO to MBUS)
        bool ps_tx_any_valid = false;
        bool ps_tx_all_ready = true;
        uint64_t ps_tx_addr = 0;
        uint64_t ps_tx_data = 0;
        for (size_t i = 0; i < num_ports; ++i) {
            if (noc_ps_to_bus_req[i].valid_out.read()) {
                ps_tx_any_valid = true;
                ps_tx_addr = noc_ps_to_bus_req[i].data_out.read().addr;
                ps_tx_data = noc_ps_to_bus_req[i].data_out.read().data;
            }
            if (!noc_ps_to_bus_req[i].ready_in.read()) ps_tx_all_ready = false;
        }
        std::string ps_tx_state = ps_tx_any_valid ? (ps_tx_all_ready ? "TX_XFER" : "TX_WAIT_READY") : "TX_IDLE";
        std::string ps_tx_args = std::string("{\"any_valid\": ") + bool_to_json(ps_tx_any_valid)
                       + ", \"all_ready\": " + bool_to_json(ps_tx_all_ready)
                       + ", \"addr\": " + u64_hex_json(ps_tx_addr)
                       + ", \"data\": " + u64_hex_json(ps_tx_data) + "}";
        trace_state_with_args(last_state_noc_ps_tx, last_args_noc_ps_tx, ps_tx_state,
                      "NoC_PS_TX", tid_noc_ps_tx, ps_tx_args);

        bool pd_tx_any_valid = false;
        bool pd_tx_all_ready = true;
        uint64_t pd_tx_addr = 0;
        uint64_t pd_tx_data = 0;
        for (size_t i = 0; i < num_ports; ++i) {
            if (noc_pd_to_bus_req[i].valid_out.read()) {
                pd_tx_any_valid = true;
                pd_tx_addr = noc_pd_to_bus_req[i].data_out.read().addr;
                pd_tx_data = noc_pd_to_bus_req[i].data_out.read().data;
            }
            if (!noc_pd_to_bus_req[i].ready_in.read()) pd_tx_all_ready = false;
        }
        std::string pd_tx_state = pd_tx_any_valid ? (pd_tx_all_ready ? "TX_XFER" : "TX_WAIT_READY") : "TX_IDLE";
        std::string pd_tx_args = std::string("{\"any_valid\": ") + bool_to_json(pd_tx_any_valid)
                       + ", \"all_ready\": " + bool_to_json(pd_tx_all_ready)
                       + ", \"addr\": " + u64_hex_json(pd_tx_addr)
                       + ", \"data\": " + u64_hex_json(pd_tx_data) + "}";
        trace_state_with_args(last_state_noc_pd_tx, last_args_noc_pd_tx, pd_tx_state,
                      "NoC_PD_TX", tid_noc_pd_tx, pd_tx_args);

        bool pli_tx_any_valid = false;
        bool pli_tx_all_ready = true;
        uint64_t pli_tx_addr = 0;
        uint64_t pli_tx_data = 0;
        for (size_t i = 0; i < num_ports; ++i) {
            if (noc_pli_to_bus_req[i].valid_out.read()) {
                pli_tx_any_valid = true;
                pli_tx_addr = noc_pli_to_bus_req[i].data_out.read().addr;
                pli_tx_data = noc_pli_to_bus_req[i].data_out.read().data;
            }
            if (!noc_pli_to_bus_req[i].ready_in.read()) pli_tx_all_ready = false;
        }
        std::string pli_tx_state = pli_tx_any_valid ? (pli_tx_all_ready ? "TX_XFER" : "TX_WAIT_READY") : "TX_IDLE";
        std::string pli_tx_args = std::string("{\"any_valid\": ") + bool_to_json(pli_tx_any_valid)
                    + ", \"all_ready\": " + bool_to_json(pli_tx_all_ready)
                    + ", \"addr\": " + u64_hex_json(pli_tx_addr)
                    + ", \"data\": " + u64_hex_json(pli_tx_data) + "}";
        trace_state_with_args(last_state_noc_pli_tx, last_args_noc_pli_tx, pli_tx_state,
                      "NoC_PLI_TX", tid_noc_pli_tx, pli_tx_args);

        bool plo_tx_any_valid = false;
        bool plo_tx_all_ready = true;
        uint64_t plo_tx_addr = 0;
        for (size_t i = 0; i < num_ports; ++i) {
            if (noc_plo_to_bus_req[i].valid_out.read()) {
                plo_tx_any_valid = true;
                plo_tx_addr = noc_plo_to_bus_req[i].data_out.read().addr;
            }
            if (!noc_plo_to_bus_req[i].ready_in.read()) plo_tx_all_ready = false;
        }
        std::string plo_tx_state = plo_tx_any_valid ? (plo_tx_all_ready ? "TX_XFER" : "TX_WAIT_READY") : "TX_IDLE";
        std::string plo_tx_args = std::string("{\"any_valid\": ") + bool_to_json(plo_tx_any_valid)
                    + ", \"all_ready\": " + bool_to_json(plo_tx_all_ready)
                    + ", \"addr\": " + u64_hex_json(plo_tx_addr) + "}";
        trace_state_with_args(last_state_noc_plo_tx, last_args_noc_plo_tx, plo_tx_state,
                      "NoC_PLO_TX", tid_noc_plo_tx, plo_tx_args);

        // Response transfer (MBUS to FIFO)
        bool plo_rx_any_valid = false;
        uint64_t plo_rx_valid_mask = 0;
        for (size_t i = 0; i < num_ports; ++i) {
            if (bus_to_noc_plo_resp[i].valid_in.read()) {
                plo_rx_any_valid = true;
                plo_rx_valid_mask |= (1ULL << i);
            }
        }
        bool plo_pending = pending_read_reg.read();
        bool plo_ultra = pending_read_ultra_reg.read();

        std::string plo_rx_data_json = "null";
        if (plo_pending && plo_rx_any_valid) {
            if (plo_ultra) {
                std::string lane_data = "[";
                bool first = true;
                for (size_t i = 0; i < num_ports; ++i) {
                    if (bus_to_noc_plo_resp[i].valid_in.read()) {
                        if (!first) lane_data += ", ";
                        lane_data += u64_hex_json(bus_to_noc_plo_resp[i].data_in.read().data);
                        first = false;
                    }
                }
                lane_data += "]";
                plo_rx_data_json = lane_data;
            } else {
                for (size_t i = 0; i < num_ports; ++i) {
                    if (bus_to_noc_plo_resp[i].valid_in.read()) {
                        plo_rx_data_json = u64_hex_json(bus_to_noc_plo_resp[i].data_in.read().data);
                        break;
                    }
                }
            }
        }
        std::string plo_rx_state;
        if (!plo_pending) {
            plo_rx_state = "RX_IDLE";
        } else if (rx_stall_sig.read() && resp_fifo_full_sig.read()) {
            plo_rx_state = "RX_STALL_FIFO_FULL";
        } else if (plo_rx_any_valid) {
            plo_rx_state = "RX_COLLECT";
        } else {
            plo_rx_state = "RX_WAIT";
        }
        std::string plo_rx_args = std::string("{\"pending\": ") + bool_to_json(plo_pending)
                    + ", \"any_valid\": " + bool_to_json(plo_rx_any_valid)
                    + ", \"valid_mask\": " + std::to_string(plo_rx_valid_mask)
                    + ", \"resp_fifo_full\": " + bool_to_json(resp_fifo_full_sig.read())
                    + ", \"data\": " + plo_rx_data_json + "}";
        trace_state_with_args(last_state_noc_plo_rx, last_args_noc_plo_rx, plo_rx_state,
                      "NoC_PLO_RX", tid_noc_plo_rx, plo_rx_args);

        // Response output (FIFO to PLO Out)
        bool plo_out_valid = noc_plo_out.valid_out.read();
        bool plo_out_ready = noc_plo_out.ready_in.read();
        std::string plo_out_state = plo_out_valid ? (plo_out_ready ? "OUT_XFER" : "OUT_WAIT_READY") : "OUT_IDLE";
        std::string plo_out_args = std::string("{\"valid\": ") + bool_to_json(plo_out_valid)
                                 + ", \"ready\": " + bool_to_json(plo_out_ready)
                                 + ", \"resp_fifo_empty\": " + bool_to_json(resp_empty) + "}";
        trace_state_with_args(last_state_noc_plo_out, last_args_noc_plo_out, plo_out_state,
                              "NoC_PLO_OUT", tid_noc_plo_out, plo_out_args);
    }
};

} // namespace noc
} // namespace hybridacc
