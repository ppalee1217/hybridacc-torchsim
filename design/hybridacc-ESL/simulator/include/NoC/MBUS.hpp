#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"
#include <vector>

using namespace sc_core;

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

    // PE interface ports (Strict)
    sc_vector<VRDOF<noc_request_t>> bus_to_pe_ps_req;   // PS (Weights)
    sc_vector<VRDOF<noc_request_t>> bus_to_pe_pd_req;   // PD (Activations)
    sc_vector<VRDOF<noc_request_t>> bus_to_pe_pli_req;  // PLI (Partial Sum In)
    sc_vector<VRDOF<noc_addr_req_t>> bus_to_pe_plo_req; // PLO (Partial Sum Out - Read Request)
    sc_vector<VRDIF<noc_response_t>> pe_to_bus_plo_resp;// PLO (Partial Sum Out - Read Response)

    // Control ports
    sc_vector<sc_in<bool>> pe_busy;

    // ===  NoC Router interface ports (Strict) ===
    VRDIF<noc_request_t> noc_ps_to_bus_req;   // PS
    VRDIF<noc_request_t> noc_pd_to_bus_req;   // PD
    VRDIF<noc_request_t> noc_pli_to_bus_req;  // PLI
    VRDIF<noc_addr_req_t> noc_plo_to_bus_req; // PLO (Read Req)
    VRDOF<noc_response_t> bus_to_noc_plo_resp;// PLO (Read Resp)

    // ID, mode, enable scan-chain ports
    sc_in<bool> scan_chain_enable;
    sc_in<ScanChainFormat> scan_chain_in;
    sc_out<ScanChainFormat> scan_chain_out;

    // Interior Signals
    // ... defined in private

    // Constructor
    SC_HAS_PROCESS(MBUS);
    MBUS(sc_module_name name, size_t num_pes)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          num_pes(num_pes),
          bus_to_pe_ps_req("bus_to_pe_ps_req", num_pes),
          bus_to_pe_pd_req("bus_to_pe_pd_req", num_pes),
          bus_to_pe_pli_req("bus_to_pe_pli_req", num_pes),
          bus_to_pe_plo_req("bus_to_pe_plo_req", num_pes),
          pe_to_bus_plo_resp("pe_to_bus_plo_resp", num_pes),
          router_enable("router_enable", num_pes),
          router_mode("router_mode", num_pes),
          pe_busy("pe_busy", num_pes),
          noc_ps_to_bus_req("noc_ps_to_bus_req"),
          noc_pd_to_bus_req("noc_pd_to_bus_req"),
          noc_pli_to_bus_req("noc_pli_to_bus_req"),
          noc_plo_to_bus_req("noc_plo_to_bus_req"),
          bus_to_noc_plo_resp("bus_to_noc_plo_resp"),
          scan_chain_enable("scan_chain_enable"),
          scan_chain_in("scan_chain_in"),
          scan_chain_out("scan_chain_out"),
          pe_scan_chain_signals_reg("pe_scan_chain_signal_reg", num_pes),
          pe_scan_chain_signals_next("pe_scan_chain_signal_next", num_pes) {

        DEBUG_MSG("[Create] MBUS with " << num_pes << " PEs (Split Channels)", DEBUG_LEVEL_NOC_COMPONENTS);

        // Register sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational processes

        // Scan-chain shifting logic
        SC_METHOD(comb_scan_chain_shift);
        sensitive << scan_chain_enable << scan_chain_in;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i];
        }

        // Router configuration output
        SC_METHOD(comb_router_config);
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i];
        }

        // PS Channel Routing
        SC_METHOD(comb_ps_routing);
        sensitive << noc_ps_to_bus_req.valid_in << noc_ps_to_bus_req.data_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) sensitive << pe_scan_chain_signals_reg[i] << bus_to_pe_ps_req[i].ready_in;

        // PD Channel Routing
        SC_METHOD(comb_pd_routing);
        sensitive << noc_pd_to_bus_req.valid_in << noc_pd_to_bus_req.data_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) sensitive << pe_scan_chain_signals_reg[i] << bus_to_pe_pd_req[i].ready_in;

        // PLI Channel Routing
        SC_METHOD(comb_pli_routing);
        sensitive << noc_pli_to_bus_req.valid_in << noc_pli_to_bus_req.data_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) sensitive << pe_scan_chain_signals_reg[i] << bus_to_pe_pli_req[i].ready_in;

        // PLO Channel Routing (Read Request)
        SC_METHOD(comb_plo_routing);
        sensitive << noc_plo_to_bus_req.valid_in << noc_plo_to_bus_req.data_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) sensitive << pe_scan_chain_signals_reg[i] << bus_to_pe_plo_req[i].ready_in;

        // PLO Response Handling
        SC_METHOD(comb_pe_to_noc_plo_response);
        sensitive << scan_chain_enable << rx_mask_reg;
        for (size_t i = 0; i < num_pes; ++i) sensitive << pe_to_bus_plo_resp[i].valid_in << pe_to_bus_plo_resp[i].data_in;

        // Response Ready Handling
        SC_METHOD(comb_pe_response_ready);
        sensitive << bus_to_noc_plo_resp.ready_in << rx_mask_reg;

        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    MBUS(sc_module_name name) : MBUS(name, NUM_PES_DEFAULT) {}

    void dump_state() {
        // ... (unchanged)
    }

private:
    size_t num_pes;

    sc_vector<sc_signal<ScanChainFormat>> pe_scan_chain_signals_reg;
    sc_vector<sc_signal<ScanChainFormat>> pe_scan_chain_signals_next;
    sc_signal<uint64_t> rx_mask_reg;
    sc_signal<uint64_t> rx_mask_next;
    sc_signal<uint64_t> tx_mask_wire;

    void seq_process() {
        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat init_config;
            init_config.enable = false;
            pe_scan_chain_signals_reg[i].write(init_config);
        }
        rx_mask_reg.write(0);
        wait();
        while (true) {
            for (size_t i = 0; i < num_pes; ++i) pe_scan_chain_signals_reg[i].write(pe_scan_chain_signals_next[i].read());
            rx_mask_reg.write(rx_mask_next.read());
            wait();
        }
    }

    void comb_scan_chain_shift() {
        if (scan_chain_enable.read()) {
            pe_scan_chain_signals_next[0].write(scan_chain_in.read());
            for (size_t i = 1; i < num_pes; ++i) pe_scan_chain_signals_next[i].write(pe_scan_chain_signals_reg[i - 1].read());
            scan_chain_out.write(pe_scan_chain_signals_reg[num_pes - 1].read());
        } else {
            for (size_t i = 0; i < num_pes; ++i) pe_scan_chain_signals_next[i].write(pe_scan_chain_signals_reg[i].read());

            ScanChainFormat default_out; default_out.enable = false;
            scan_chain_out.write(default_out);
        }
    }

    void comb_router_config() {
        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat config = pe_scan_chain_signals_reg[i].read();
            router_enable[i].write(config.enable);
            router_mode[i].write(config.route_mode);
        }
    }

    // Helper: Calculate Target PE Mask
    uint64_t calculate_target_pe_mask(uint16_t addr, NOC_CHANNELS expected_channel) const {
        // NoC router Level: addr[7] - command, addr[6] - ultra_mode, addr[5:0] - tag
        // MBUS/PE level: addr[6] - command, addr[5:0] - tag
        // We assume MBUS receives addresses formatted for PE level? No, from Router.
        // If MBUS connects to Router, it receives Router Level addresses?
        // User said: "NoC router Level: ... MBUS/PE level: ..."
        // This implies transformation occurs or MBUS expects PE-level?
        // Let's assume MBUS sees what Router sends. If Router strips bits, then MBUS sees PE level.
        // But Router usually just routes.
        // Assuming MBUS sees address where addr[5:0] is Tag.

        bool command = (addr & 0x40); // addr[6] = 1 for command
        uint8_t tag = addr & 0x3F;    // addr[5:0] = PE ID tag

        // Note: Command might broadcast or be specific.
        // If command, maybe to specific ID? Or all?
        // Usually command is specific.

        uint64_t mask = 0;
        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat config = pe_scan_chain_signals_reg[i].read();
            if (!config.enable) continue;

            uint8_t pe_channel_id = 0;
            switch (expected_channel) {
                case NOC_CHANNEL_PS:  pe_channel_id = config.ps_id; break;
                case NOC_CHANNEL_PD:  pe_channel_id = config.pd_id; break;
                case NOC_CHANNEL_PLI: pe_channel_id = config.pli_id; break;
                case NOC_CHANNEL_PLO: pe_channel_id = config.plo_id; break;
            }

            // Command or Tag match
            // If command bit is set, does it override tag matching?
            // "addr[6] - command". Maybe Command means "Configuration" or "Start"?
            // If command, we check tag? Or ignore tag?
            // User: "addr[5:0] - tag".
            // So we match tag, and addr[6] is just a flag for the PE controller.

            if (pe_channel_id == tag) {
                mask |= (1ULL << i);
            }
        }
        return mask;
    }

    void comb_ps_routing() {
        bool scan = scan_chain_enable.read();
        bool valid = noc_ps_to_bus_req.valid_in.read();
        noc_request_t req = noc_ps_to_bus_req.data_in.read();
        uint64_t mask = (!scan && valid) ? calculate_target_pe_mask(req.addr, NOC_CHANNEL_PS) : 0;

        bool all_ready = true;
        for(size_t i=0; i<num_pes; ++i) if(mask & (1ULL<<i)) if(!bus_to_pe_ps_req[i].ready_in.read()) all_ready=false;

        bool ready = !scan && all_ready;
        noc_ps_to_bus_req.ready_out.write(ready);

        for(size_t i=0; i<num_pes; ++i) {
            bus_to_pe_ps_req[i].data_out.write(req);
            bus_to_pe_ps_req[i].valid_out.write(valid && ready && (mask & (1ULL<<i)));
        }
    }

    void comb_pd_routing() {
        bool scan = scan_chain_enable.read();
        bool valid = noc_pd_to_bus_req.valid_in.read();
        noc_request_t req = noc_pd_to_bus_req.data_in.read();
        uint64_t mask = (!scan && valid) ? calculate_target_pe_mask(req.addr, NOC_CHANNEL_PD) : 0;

        bool all_ready = true;
        for(size_t i=0; i<num_pes; ++i) if(mask & (1ULL<<i)) if(!bus_to_pe_pd_req[i].ready_in.read()) all_ready=false;

        bool ready = !scan && all_ready;
        noc_pd_to_bus_req.ready_out.write(ready);

        for(size_t i=0; i<num_pes; ++i) {
            bus_to_pe_pd_req[i].data_out.write(req);
            bus_to_pe_pd_req[i].valid_out.write(valid && ready && (mask & (1ULL<<i)));
        }
    }

    void comb_pli_routing() {
        bool scan = scan_chain_enable.read();
        bool valid = noc_pli_to_bus_req.valid_in.read();
        noc_request_t req = noc_pli_to_bus_req.data_in.read();
        uint64_t mask = (!scan && valid) ? calculate_target_pe_mask(req.addr, NOC_CHANNEL_PLI) : 0;

        bool all_ready = true;
        for(size_t i=0; i<num_pes; ++i) if(mask & (1ULL<<i)) if(!bus_to_pe_pli_req[i].ready_in.read()) all_ready=false;

        bool ready = !scan && all_ready;
        noc_pli_to_bus_req.ready_out.write(ready);

        for(size_t i=0; i<num_pes; ++i) {
            bus_to_pe_pli_req[i].data_out.write(req);
            bus_to_pe_pli_req[i].valid_out.write(valid && ready && (mask & (1ULL<<i)));
        }
    }

    void comb_plo_routing() {
        bool scan = scan_chain_enable.read();
        bool valid = noc_plo_to_bus_req.valid_in.read();
        noc_addr_req_t req = noc_plo_to_bus_req.data_in.read();
        uint64_t mask = (!scan && valid) ? calculate_target_pe_mask(req.addr, NOC_CHANNEL_PLO) : 0;

        // Update RX Mask for response cycle
        // Note: Logic simplified; implies atomic read-req then response wait?
        // Original logic updated rx_mask.
        if(!scan && valid) rx_mask_next.write(mask);
        else rx_mask_next.write(0);

        bool all_ready = true;
        for(size_t i=0; i<num_pes; ++i) if(mask & (1ULL<<i)) if(!bus_to_pe_plo_req[i].ready_in.read()) all_ready=false;

        bool ready = !scan && all_ready;
        noc_plo_to_bus_req.ready_out.write(ready);

        for(size_t i=0; i<num_pes; ++i) {
            bus_to_pe_plo_req[i].data_out.write(req);
            bus_to_pe_plo_req[i].valid_out.write(valid && ready && (mask & (1ULL<<i)));
        }
    }

    void comb_pe_to_noc_plo_response() {
        uint64_t mask = rx_mask_reg.read();
        bool active = false;
        noc_response_t resp; resp.data = 0; resp.status = NOC_RESPONSE_STATUS::NOC_NOP;

        // Simple priority arbiter or check collision
        int count = 0;
        for(size_t i=0; i<num_pes; ++i) {
            if((mask & (1ULL<<i)) && pe_to_bus_plo_resp[i].valid_in.read()){
                count++;
                resp = pe_to_bus_plo_resp[i].data_in.read();
                active = true;
            }
        }

        if(count > 1) { resp.status = NOC_RESPONSE_STATUS::NOC_ERROR; active = true; }

        bus_to_noc_plo_resp.data_out.write(resp);
        bus_to_noc_plo_resp.valid_out.write(active);
    }

    void comb_pe_response_ready() {
        bool ready = bus_to_noc_plo_resp.ready_in.read();
        uint64_t mask = rx_mask_reg.read();
        for(size_t i=0; i<num_pes; ++i) {
            pe_to_bus_plo_resp[i].ready_out.write(ready && (mask & (1ULL<<i)));
        }
    }

    // Trace support placeholder
public:
    int trace_id = -1;
    void set_trace_id(int id) { trace_id = id; }
    void trace_process() {}
};

} // namespace noc
} // namespace hybridacc
