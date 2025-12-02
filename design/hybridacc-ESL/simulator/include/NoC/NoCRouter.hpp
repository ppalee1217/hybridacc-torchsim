#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"

namespace hybridacc {
namespace noc {

static const int NUM_PORTS_DEFAULT = 4;

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

    sc_in<bool> en;
    sc_in<bool> wen;
    sc_in<sc_uint<10>> addr; // [9] cmd, [8] SIMD, [7:6] channel, [5:0] tag
    sc_in<sc_uint<256>> data_in;
    sc_out<sc_uint<256>> data_out;

    // ===  NoC MBUS interface ports ===
    // NoC interface ports - using VRDIF/VRDOF
    sc_vector<VRDOF<noc_request_t>> noc_to_bus_req; // .addr([8] cmd, [7:6] channel, [5:0] tag)
    sc_vector<VRDIF<noc_response_t>> bus_to_noc_resp;

    // ID, mode, enable scan-chain ports
    sc_out<bool> scan_chain_enable; // broadcast to all ports
    sc_vector<sc_in<ScanChainFormat>> scan_chain_in;
    sc_vector<sc_out<ScanChainFormat>> scan_chain_out;

    size_t num_ports;

    NoCRouter(sc_core::sc_module_name name, size_t num_ports)
        : sc_core::sc_module(name),
            clk("clk"),
            reset_n("reset_n"),
            command_mode("command_mode"),
            command_data("command_data"),
            en("en"),
            wen("wen"),
            addr("addr"),
            data_in("data_in"),
            data_out("data_out"),
            noc_to_bus_req("noc_to_bus_req", num_ports),
            bus_to_noc_resp("bus_to_noc_resp", num_ports),
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
            is_simd_read_reg("is_simd_read_reg"),
            is_simd_read_next("is_simd_read_next"),
            collected_data_reg("collected_data_reg"),
            collected_data_next("collected_data_next"),
            response_mask_reg("response_mask_reg"),
            response_mask_next("response_mask_next") {

        DEBUG_MSG("[Create] NoCRouter with " << num_ports << " ports");

        // Register sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational processes with different sensitivity lists

        // Command processing
        SC_METHOD(comb_command_process);
        sensitive << command_mode << command_data << scan_chain_enable_reg;

        // Normal read/write request generation
        SC_METHOD(comb_noc_request);
        sensitive << en << wen << addr << data_in << command_mode << scan_chain_enable_reg;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_to_bus_req[i].ready_in;
        }

        // Response collection and error detection
        SC_METHOD(comb_noc_response);
        sensitive << addr << wen << command_mode << scan_chain_enable_reg;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << bus_to_noc_resp[i].valid_in << bus_to_noc_resp[i].data_in;
        }

        // Scan-chain output
        SC_METHOD(comb_scan_chain_output);
        sensitive << scan_chain_enable_reg << scan_chain_data_reg;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << scan_chain_in[i];
        }
    }

    // Overloaded constructor with default number of PEs
    NoCRouter(sc_core::sc_module_name name)
        : NoCRouter(name, NUM_PORTS_DEFAULT) // Default to 4 PEs
    {}

private:
    // === Sequential Elements ===
    sc_signal<ScanChainFormat> scan_chain_data_reg;
    sc_signal<ScanChainFormat> scan_chain_data_next;
    sc_signal<bool> scan_chain_enable_reg;
    sc_signal<bool> scan_chain_enable_next;

    // Response collection state
    sc_signal<bool> pending_read_reg;
    sc_signal<bool> pending_read_next;
    sc_signal<bool> is_simd_read_reg;
    sc_signal<bool> is_simd_read_next;
    sc_signal<sc_uint<256>> collected_data_reg;
    sc_signal<sc_uint<256>> collected_data_next;
    sc_signal<uint8_t> response_mask_reg;  // Track which ports have responded
    sc_signal<uint8_t> response_mask_next;

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
        is_simd_read_reg.write(false);
        collected_data_reg.write(0);
        response_mask_reg.write(0);

        wait();

        // Sequential logic
        while (true) {
            scan_chain_data_reg.write(scan_chain_data_next.read());
            scan_chain_enable_reg.write(scan_chain_enable_next.read());
            pending_read_reg.write(pending_read_next.read());
            is_simd_read_reg.write(is_simd_read_next.read());
            collected_data_reg.write(collected_data_next.read());
            response_mask_reg.write(response_mask_next.read());
            wait();
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

                DEBUG_MSG("[NoCRouter] Scan-chain command received: "
                         << "ps_id=" << (int)sc_format.ps_id
                         << ", pd_id=" << (int)sc_format.pd_id
                         << ", pli_id=" << (int)sc_format.pli_id
                         << ", plo_id=" << (int)sc_format.plo_id
                         << ", mode=" << sc_format.route_mode
                         << ", enable=" << sc_format.enable);
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

    // === Combinational: NoC Request Generation ===
    void comb_noc_request() {
        bool cmd_mode = command_mode.read();
        sc_uint<32> cmd_data = command_data.read();
        bool enable = en.read();
        bool write_en = wen.read();
        sc_uint<10> address = addr.read();
        sc_uint<256> data_256 = data_in.read();
        bool scan_en = scan_chain_enable_reg.read();

        if (cmd_mode) {
            message_command_t cmd_type = static_cast<message_command_t>(cmd_data.range(3, 0).to_uint());

            if (cmd_type == message_command_t::CMD_NOC_SCAN_CHAIN) {
                // NoC command: no request to MBUS
                for (size_t i = 0; i < num_ports; ++i) {
                    noc_to_bus_req[i].valid_out.write(false);
                    noc_to_bus_req[i].data_out.write(noc_request_t{0, 0, false});
                }
            } else {
                // PE command: broadcast to all MBUS with addr=0x100
                bool all_ready = true;
                for (size_t i = 0; i < num_ports; ++i) {
                    if (!noc_to_bus_req[i].ready_in.read()) {
                        all_ready = false;
                        break;
                    }
                }

                noc_request_t cmd_req;
                cmd_req.addr = 0x100; // PE command address
                cmd_req.data = cmd_data.to_uint64();
                cmd_req.is_w = true;

                for (size_t i = 0; i < num_ports; ++i) {
                    noc_to_bus_req[i].data_out.write(cmd_req);
                    noc_to_bus_req[i].valid_out.write(all_ready);
                }

                DEBUG_MSG("[NoCRouter] PE command broadcast: cmd=" << cmd_type
                         << ", data=0x" << std::hex << cmd_data.to_uint());
            }
        } else if (enable && !scan_en) {
            // Normal read/write mode
            bool is_simd = address.bit(8); // addr[8]
            bool is_cmd = address.bit(9);  // addr[9]
            sc_uint<8> base_addr = address.range(7, 0);

            // Check if all ports are ready
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (is_simd) {
                // SIMD mode: 0x100~0x1ff, split 256bits into 4x64bits
                for (size_t i = 0; i < num_ports && i < 4; ++i) {
                    noc_request_t req;
                    req.addr = base_addr.to_uint();
                    req.data = data_256.range(64*i + 63, 64*i).to_uint64();
                    req.is_w = write_en;

                    noc_to_bus_req[i].data_out.write(req);
                    noc_to_bus_req[i].valid_out.write(all_ready);
                }
                // Additional ports beyond 4 get no request
                for (size_t i = 4; i < num_ports; ++i) {
                    noc_to_bus_req[i].valid_out.write(false);
                    noc_to_bus_req[i].data_out.write(noc_request_t{0, 0, false});
                }
            } else {
                // Broadcast mode: 0x0~0xff, use low 64bits
                noc_request_t req;
                req.addr = base_addr.to_uint();
                req.data = data_256.range(63, 0).to_uint64();
                req.is_w = write_en;

                for (size_t i = 0; i < num_ports; ++i) {
                    noc_to_bus_req[i].data_out.write(req);
                    noc_to_bus_req[i].valid_out.write(all_ready);
                }
            }
        } else {
            // Disabled or scan mode: no request
            for (size_t i = 0; i < num_ports; ++i) {
                noc_to_bus_req[i].valid_out.write(false);
                noc_to_bus_req[i].data_out.write(noc_request_t{0, 0, false});
            }
        }
    }

    // === Combinational: NoC Response Collection ===
    void comb_noc_response() {
        sc_uint<10> address = addr.read();
        bool write_en = wen.read();
        bool enable = en.read();
        bool cmd_mode = command_mode.read();
        bool scan_en = scan_chain_enable_reg.read();

        bool pending = pending_read_reg.read();
        bool is_simd = is_simd_read_reg.read();
        sc_uint<256> collected = collected_data_reg.read();
        uint8_t resp_mask = response_mask_reg.read();

        // Default: keep state
        pending_read_next.write(pending);
        is_simd_read_next.write(is_simd);
        collected_data_next.write(collected);
        response_mask_next.write(resp_mask);

        if (cmd_mode || scan_en) {
            // Command or scan mode: clear pending state, no response
            data_out.write(0);
            pending_read_next.write(false);
            response_mask_next.write(0);
            collected_data_next.write(0);

        } else if (enable && !write_en && !pending) {
            // New read request initiated
            bool new_is_simd = address.bit(8);
            pending_read_next.write(true);
            is_simd_read_next.write(new_is_simd);
            collected_data_next.write(0);
            response_mask_next.write(0);
            data_out.write(0);

        } else if (pending) {
            // Pending read: collect responses
            sc_uint<256> new_collected = collected;
            uint8_t new_resp_mask = resp_mask;
            bool error_occurred = false;

            // Collect responses from all ports
            for (size_t i = 0; i < num_ports; ++i) {
                if (bus_to_noc_resp[i].valid_in.read()) {
                    noc_response_t resp = bus_to_noc_resp[i].data_in.read();

                    if (resp.status == NOC_RESPONSE_STATUS::NOC_OK) {
                        // Mark this port as responded
                        new_resp_mask |= (1 << i);

                        if (is_simd) {
                            // SIMD mode: assign data to corresponding 64-bit segment
                            if (i < 4) {
                                new_collected.range(64*i + 63, 64*i) = resp.data;
                            }
                        } else {
                            // Broadcast mode: collect data (check for collision later)
                            new_collected.range(63, 0) = resp.data;
                        }
                    } else if (resp.status == NOC_RESPONSE_STATUS::NOC_ERROR) {
                        error_occurred = true;
                    }
                }

                // Accept responses by asserting ready
                bus_to_noc_resp[i].ready_out.write(true);
            }

            // Check if collection is complete
            bool collection_complete = false;

            if (is_simd) {
                // SIMD mode: need responses from all 4 ports
                uint8_t expected_mask = 0x0F; // bits 0-3
                if (num_ports < 4) {
                    expected_mask = (1 << num_ports) - 1;
                }
                collection_complete = (new_resp_mask == expected_mask);

            } else {
                // Broadcast mode: need exactly one response
                int resp_count = __builtin_popcount(new_resp_mask);

                if (resp_count == 1) {
                    collection_complete = true;
                } else if (resp_count > 1) {
                    // Multiple responses: ID conflict error
                    error_occurred = true;
                    collection_complete = true;
                    DEBUG_MSG("[NoCRouter] Broadcast read ERROR: multiple responses ("
                             << resp_count << ") - ID scan-chain misconfiguration");
                }
            }

            if (error_occurred) {
                // Error: output zero and clear state
                data_out.write(0);
                pending_read_next.write(false);
                response_mask_next.write(0);
                collected_data_next.write(0);
                DEBUG_MSG("[NoCRouter] Read ERROR occurred");

            } else if (collection_complete) {
                // All expected responses collected: output result
                data_out.write(new_collected);
                pending_read_next.write(false);
                response_mask_next.write(0);
                collected_data_next.write(0);
                DEBUG_MSG("[NoCRouter] Read completed: data=0x"
                         << std::hex << new_collected.to_uint64());

            } else {
                // Still waiting for more responses
                data_out.write(0);
                response_mask_next.write(new_resp_mask);
                collected_data_next.write(new_collected);
            }

        } else {
            // Write mode or disabled: no data output
            data_out.write(0);

            // Clear ready signals when not collecting
            for (size_t i = 0; i < num_ports; ++i) {
                bus_to_noc_resp[i].ready_out.write(false);
            }
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
};

} // namespace noc
} // namespace hybridacc