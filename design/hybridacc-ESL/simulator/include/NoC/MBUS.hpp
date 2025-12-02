#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"

namespace hybridacc {
namespace noc {

static const int NUM_PES_DEFAULT = 16;

SC_MODULE(MBUS) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // === Process Element (PE) interface ports ===
    // Router config port
    sc_vector<sc_out<bool>> router_enable;
    sc_vector<sc_out<PERouterMode>> router_mode;

    // NoC interface ports - using VRDOF/VRDIF with internal signals
    sc_vector<VRDSIG<noc_request_t>> bus_to_pe_req_sig;
    sc_vector<VRDOF<noc_request_t>> bus_to_pe_req;

    sc_vector<VRDSIG<noc_response_t>> pe_to_bus_resp_sig;
    sc_vector<VRDIF<noc_response_t>> pe_to_bus_resp;

    // Control ports
    sc_vector<sc_in<bool>> pe_busy;

    // ===  NoC Router interface ports ===
    // NoC interface ports - using VRDIF/VRDOF
    VRDIF<noc_request_t> noc_to_bus_req;
    VRDOF<noc_response_t> bus_to_noc_resp;

    // ID, mode, enable scan-chain ports
    sc_in<bool> scan_chain_enable;
    sc_in<ScanChainFormat> scan_chain_in;
    sc_out<ScanChainFormat> scan_chain_out;

    // Constructor
    MBUS(sc_core::sc_module_name name, size_t num_pes)
        : sc_core::sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          num_pes(num_pes),
          bus_to_pe_req_sig("bus_to_pe_req_sig", num_pes),
          bus_to_pe_req("bus_to_pe_req", num_pes),
          pe_to_bus_resp_sig("pe_to_bus_resp_sig", num_pes),
          pe_to_bus_resp("pe_to_bus_resp", num_pes),
          router_enable("router_enable", num_pes),
          router_mode("router_mode", num_pes),
          pe_busy("pe_busy", num_pes),
          noc_to_bus_req("noc_to_bus_req"),
          bus_to_noc_resp("bus_to_noc_resp"),
          scan_chain_enable("scan_chain_enable"),
          scan_chain_in("scan_chain_in"),
          scan_chain_out("scan_chain_out"),
          pe_scan_chain_signals_reg("pe_scan_chain_signal_reg", num_pes),
          pe_scan_chain_signals_next("pe_scan_chain_signal_next", num_pes) {

        DEBUG_MSG("[Create] MBUS with " << num_pes << " PEs");

        // Connect internal signals
        for (size_t i = 0; i < num_pes; ++i) {
            connect_vr_signals(bus_to_pe_req[i], bus_to_pe_req_sig[i]);
            connect_vr_signals(pe_to_bus_resp[i], pe_to_bus_resp_sig[i]);
        }

        // Register sequential process (only for scan-chain)
        SC_CTHREAD(seq_scan_chain, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational processes with different sensitivity lists

        // Scan-chain shifting logic
        SC_METHOD(comb_scan_chain_shift);
        sensitive << scan_chain_enable << scan_chain_in;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i];
        }

        // Router configuration output (from scan-chain registers)
        SC_METHOD(comb_router_config);
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i];
        }

        // NoC request routing to PEs
        SC_METHOD(comb_noc_to_pe_routing);
        sensitive << noc_to_bus_req.valid_in << noc_to_bus_req.data_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i]
                      << bus_to_pe_req_sig[i].ready_sig;
        }

        // PE response to NoC (including collision detection)
        SC_METHOD(comb_pe_to_noc_response);
        sensitive << noc_to_bus_req.valid_in << noc_to_bus_req.data_in
                  << bus_to_noc_resp.ready_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_to_bus_resp_sig[i].valid_sig
                      << pe_to_bus_resp_sig[i].data_sig
                      << bus_to_pe_req_sig[i].ready_sig;
        }

        // PE response ready signals
        SC_METHOD(comb_pe_response_ready);
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_to_bus_resp_sig[i].valid_sig;
        }
    }

    // Overloaded constructor with default number of PEs
    MBUS(sc_core::sc_module_name name)
        : MBUS(name, NUM_PES_DEFAULT) // Default to NUM_PES_DEFAULT PEs
    {}

private:
    size_t num_pes;

    // === Sequential Elements (Only Scan-chain Registers) ===
    // Scan-chain signals for each PE (these are the ONLY registers)
    sc_vector<sc_signal<ScanChainFormat>> pe_scan_chain_signals_reg;
    sc_vector<sc_signal<ScanChainFormat>> pe_scan_chain_signals_next;

    // === Sequential Process (Scan-chain only) ===
    void seq_scan_chain() {
        // Reset initialization
        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat init_config;
            init_config.ps_id = 0;
            init_config.pd_id = 0;
            init_config.pli_id = 0;
            init_config.plo_id = 0;
            init_config.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
            init_config.enable = false;
            pe_scan_chain_signals_reg[i].write(init_config);
        }

        wait();

        // Sequential logic - only update scan-chain registers
        while (true) {
            for (size_t i = 0; i < num_pes; ++i) {
                pe_scan_chain_signals_reg[i].write(pe_scan_chain_signals_next[i].read());
            }

            wait();
        }
    }

    // === Combinational: Scan-chain Shifting ===
    void comb_scan_chain_shift() {
        if (scan_chain_enable.read()) {
            // Shift scan-chain data every cycle
            ScanChainFormat first_config = scan_chain_in.read();
            pe_scan_chain_signals_next[0].write(first_config);

            for (size_t i = 1; i < num_pes; ++i) {
                ScanChainFormat prev_config = pe_scan_chain_signals_reg[i - 1].read();
                pe_scan_chain_signals_next[i].write(prev_config);
            }

            // Output from last PE
            ScanChainFormat last_config = pe_scan_chain_signals_reg[num_pes - 1].read();
            scan_chain_out.write(last_config);
        } else {
            // Hold current configuration
            for (size_t i = 0; i < num_pes; ++i) {
                pe_scan_chain_signals_next[i].write(pe_scan_chain_signals_reg[i].read());
            }

            // Output default
            ScanChainFormat default_out;
            default_out.ps_id = 0;
            default_out.pd_id = 0;
            default_out.pli_id = 0;
            default_out.plo_id = 0;
            default_out.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
            default_out.enable = false;
            scan_chain_out.write(default_out);
        }
    }

    // === Combinational: Router Configuration Output ===
    void comb_router_config() {
        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat config = pe_scan_chain_signals_reg[i].read();
            router_enable[i].write(config.enable);
            router_mode[i].write(config.route_mode);
        }
    }

    // === Combinational: NoC Request to PE Routing ===
    void comb_noc_to_pe_routing() {
        bool scan_mode = scan_chain_enable.read();
        bool noc_valid = noc_to_bus_req.valid_in.read();
        noc_request_t noc_req = noc_to_bus_req.data_in.read();

        // Calculate target PE mask
        uint64_t target_mask = 0;
        if (noc_valid && !scan_mode) {
            target_mask = calculate_target_pe_mask(noc_req.addr);
        }

        // Check if all target PEs are ready
        bool all_ready = true;
        if (target_mask != 0) {
            for (size_t i = 0; i < num_pes; ++i) {
                if (target_mask & (1ULL << i)) {
                    if (!bus_to_pe_req_sig[i].ready_sig.read()) {
                        all_ready = false;
                        break;
                    }
                }
            }
        }

        // Set NoC ready signal
        // NoC is ready when:
        // 1. Not in scan mode
        // 2. Either no request, or all target PEs are ready
        bool noc_ready = !scan_mode && (!noc_valid || all_ready);
        noc_to_bus_req.ready_out.write(noc_ready);

        // Route request to target PEs
        for (size_t i = 0; i < num_pes; ++i) {
            bool is_target = (target_mask & (1ULL << i)) != 0;
            bool send_to_pe = noc_valid && !scan_mode && is_target && all_ready;

            bus_to_pe_req_sig[i].data_sig.write(noc_req);
            bus_to_pe_req_sig[i].valid_sig.write(send_to_pe);
        }
    }

    // === Combinational: PE Response to NoC (with collision detection) ===
    void comb_pe_to_noc_response() {
        bool scan_mode = scan_chain_enable.read();
        noc_request_t noc_req = noc_to_bus_req.data_in.read();
        bool is_write = noc_req.is_w;
        bool noc_req_valid = noc_to_bus_req.valid_in.read();

        // Check which PEs have valid responses
        uint64_t resp_mask = 0;
        noc_response_t pe_resp;
        pe_resp.data = 0;
        pe_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;

        for (size_t i = 0; i < num_pes; ++i) {
            if (pe_to_bus_resp_sig[i].valid_sig.read()) {
                resp_mask |= (1ULL << i);
                if (resp_mask == (1ULL << i)) { // First response
                    pe_resp = pe_to_bus_resp_sig[i].data_sig.read();
                }
            }
        }

        noc_response_t noc_resp;
        bool noc_resp_valid = false;

        if (scan_mode) {
            // Scan mode: no response
            noc_resp.data = 0;
            noc_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;
            noc_resp_valid = false;
        } else if (noc_req_valid) {
            // Calculate target mask
            uint64_t target_mask = calculate_target_pe_mask(noc_req.addr);

            // Check if all target PEs are ready
            bool all_target_ready = (target_mask != 0);
            for (size_t i = 0; i < num_pes && all_target_ready; ++i) {
                if (target_mask & (1ULL << i)) {
                    if (!bus_to_pe_req_sig[i].ready_sig.read()) {
                        all_target_ready = false;
                    }
                }
            }

            if (is_write && all_target_ready) {
                // Write request: immediate ACK when all PEs are ready
                noc_resp.data = 0;
                noc_resp.status = NOC_RESPONSE_STATUS::NOC_OK;
                noc_resp_valid = true;
            } else if (!is_write && resp_mask != 0) {
                // Read request: wait for PE response
                int resp_count = __builtin_popcountll(resp_mask);

                if (resp_count == 1) {
                    // Exactly one PE responded (correct)
                    noc_resp = pe_resp;
                    noc_resp.status = NOC_RESPONSE_STATUS::NOC_OK;
                } else {
                    // Multiple PEs responded (collision error)
                    noc_resp.data = 0;
                    noc_resp.status = NOC_RESPONSE_STATUS::NOC_ERROR;
                    DEBUG_MSG("[MBUS] ERROR: Response collision from " << resp_count << " PEs");
                }
                noc_resp_valid = true;
            } else {
                // Read request but no response yet
                noc_resp.data = 0;
                noc_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;
                noc_resp_valid = false;
            }
        } else {
            // No request
            noc_resp.data = 0;
            noc_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;
            noc_resp_valid = false;
        }

        bus_to_noc_resp.data_out.write(noc_resp);
        bus_to_noc_resp.valid_out.write(noc_resp_valid);
    }

    // === Combinational: PE Response Ready Signals ===
    void comb_pe_response_ready() {
        // All PE responses are always ready to be consumed
        // (combinational path directly to NoC)
        for (size_t i = 0; i < num_pes; ++i) {
            pe_to_bus_resp_sig[i].ready_sig.write(true);
        }
    }

    // === Helper Functions ===

    // Calculate which PEs should receive this request based on address
    uint64_t calculate_target_pe_mask(uint16_t addr) const {
        uint8_t tag = addr & 0x3F;          // addr[5:0] = PE ID tag
        uint8_t channel = (addr >> 6) & 0x3; // addr[7:6] = channel

        uint64_t mask = 0;

        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat config = pe_scan_chain_signals_reg[i].read();

            if (!config.enable) continue;

            uint8_t pe_channel_id = 0;
            switch (channel) {
                case NOC_CHANNEL_PS:  pe_channel_id = config.ps_id; break;
                case NOC_CHANNEL_PD:  pe_channel_id = config.pd_id; break;
                case NOC_CHANNEL_PLI: pe_channel_id = config.pli_id; break;
                case NOC_CHANNEL_PLO: pe_channel_id = config.plo_id; break;
            }

            if (pe_channel_id == tag) {
                mask |= (1ULL << i);
            }
        }

        return mask;
    }
};

} // namespace noc
} // namespace hybridacc