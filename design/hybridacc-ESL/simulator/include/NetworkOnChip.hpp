#pragma once

#include <cstdint>
#include <utility>
#include <systemc>
#include "Utils/utils.hpp"
#include "NoC/MBUS.hpp"
#include "NoC/NoCRouter.hpp"
#include "ProcessElement.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {

struct NetWorkOnChipConfig {
    size_t noc_fifo_depth = 4;
    size_t pe_fifo_depth = 4;

    NetWorkOnChipConfig() = default;
    NetWorkOnChipConfig(size_t noc_fifo_depth = 4, size_t pe_fifo_depth = 4)
        : noc_fifo_depth(noc_fifo_depth),
          pe_fifo_depth(pe_fifo_depth)
    {}
};

template <unsigned NUM_PORTS, unsigned PORT_WIDTH_BITS, unsigned NUM_PES_PER_PORT>
SC_MODULE(NetworkOnChip) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // NoC Router interface ports
    sc_in<bool> command_mode;
    sc_in<sc_uint<32>> command_data;

    // New Valid-Ready Interface (Dual Plane + Split)
    VRDIF<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> noc_ps_in;
    VRDIF<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> noc_pd_in;
    VRDIF<request_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>, uint16_t>> noc_pli_in;
    VRDIF<noc_addr_req_t> noc_plo_in;
    VRDOF<response_t<sc_biguint<NUM_PORTS*PORT_WIDTH_BITS>>> noc_plo_out;

    // ---------------------------------------------
    // parameters
    NetWorkOnChipConfig config;
    size_t num_pes() const { return NUM_PORTS * NUM_PES_PER_PORT; }

    // Internal modules
    noc::NoCRouter<NUM_PORTS, PORT_WIDTH_BITS> router;
    sc_vector<noc::MBUS> mbus;
    sc_vector<sc_vector<pe::ProcessElement>> pes;

    // Internal signals - NoC/MBus
    sc_vector<VRDSIG<noc_request_t>> noc_ps_to_bus_req;
    sc_vector<VRDSIG<noc_request_t>> noc_pd_to_bus_req;
    sc_vector<VRDSIG<noc_request_t>> noc_pli_to_bus_req;
    sc_vector<VRDSIG<noc_addr_req_t>> noc_plo_to_bus_req;
    sc_vector<VRDSIG<noc_response_t>> bus_to_noc_plo_resp;

    sc_signal<bool> scan_chain_enable;
    sc_signal<sc_uint<16>> spatial_active_rows;

    // Scan chain signals
    sc_vector<sc_signal<ScanChainFormat>> router_scan_chain_out;
    sc_vector<sc_signal<ScanChainFormat>> mbus_scan_chain_out;

    //  Internal signals - MBus/PE
    sc_vector<sc_vector<sc_signal<bool>>> router_enable;
    sc_vector<sc_vector<sc_signal<PERouterMode>>> router_mode;
    sc_vector<sc_vector<VRDSIG<noc_request_t>>> bus_to_pe_ps_req;
    sc_vector<sc_vector<VRDSIG<noc_request_t>>> bus_to_pe_pd_req;
    sc_vector<sc_vector<VRDSIG<noc_request_t>>> bus_to_pe_pli_req;
    sc_vector<sc_vector<VRDSIG<noc_addr_req_t>>> bus_to_pe_plo_req;
    sc_vector<sc_vector<VRDSIG<noc_response_t>>> pe_to_bus_plo_resp;
    sc_vector<sc_vector<sc_signal<bool>>> pe_busy;
    sc_vector<sc_vector<sc_signal<bool>>> pe_halted;

    //  Internal signals - PE/PE
    sc_vector<sc_vector<VRDSIG<uint64_t>>> ln_pli_plo;

    SC_HAS_PROCESS(NetworkOnChip);
    NetworkOnChip(sc_module_name name, NetWorkOnChipConfig config)
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
          config(config),
          router("NoC_Router", NUM_PORTS, config.noc_fifo_depth),
          mbus("mbus"),
          pes("pes"),
          noc_ps_to_bus_req("noc_ps_to_bus_req", NUM_PORTS),
          noc_pd_to_bus_req("noc_pd_to_bus_req", NUM_PORTS),
          noc_pli_to_bus_req("noc_pli_to_bus_req", NUM_PORTS),
          noc_plo_to_bus_req("noc_plo_to_bus_req", NUM_PORTS),
          bus_to_noc_plo_resp("bus_to_noc_plo_resp", NUM_PORTS),
          scan_chain_enable("scan_chain_enable"),
          spatial_active_rows("spatial_active_rows"),
          router_scan_chain_out("router_scan_chain_out", NUM_PORTS),
          mbus_scan_chain_out("mbus_scan_chain_out", NUM_PORTS),
          router_enable("router_enable"),
          router_mode("router_mode"),
          bus_to_pe_ps_req("bus_to_pe_ps_req"),
          bus_to_pe_pd_req("bus_to_pe_pd_req"),
          bus_to_pe_pli_req("bus_to_pe_pli_req"),
          bus_to_pe_plo_req("bus_to_pe_plo_req"),
          pe_to_bus_plo_resp("pe_to_bus_plo_resp"),
          pe_busy("pe_busy"),
          pe_halted("pe_halted"),
          ln_pli_plo("ln_pli_plo")
    {
        spatial_active_rows.write(NUM_PES_PER_PORT);

        DEBUG_MSG("[Create] NetworkOnChip with " << NUM_PORTS << " ports, "
                  << NUM_PES_PER_PORT << " PEs per port", DEBUG_LEVEL_NOC_TOP);

        std::cout << "NetworkOnChip: Initializing with "
                  << NUM_PORTS << " ports, "
                  << NUM_PES_PER_PORT << " PEs per port" << std::endl;

        // 初始化 MBUS 向量
        mbus.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new noc::MBUS(n, NUM_PES_PER_PORT);
        });

        // 初始化 PE 二維向量
        pes.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<pe::ProcessElement>(n, NUM_PES_PER_PORT, [config](const char* m, size_t j) {
                return new pe::ProcessElement(m, config.pe_fifo_depth);
            });
        });

        // 初始化內部信號二維向量
        router_enable.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<sc_signal<bool>>(n, NUM_PES_PER_PORT);
        });

        router_mode.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<sc_signal<PERouterMode>>(n, NUM_PES_PER_PORT);
        });

        bus_to_pe_ps_req.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_request_t>>(n, NUM_PES_PER_PORT);
        });

        bus_to_pe_pd_req.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_request_t>>(n, NUM_PES_PER_PORT);
        });

        bus_to_pe_pli_req.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_request_t>>(n, NUM_PES_PER_PORT);
        });

        bus_to_pe_plo_req.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_addr_req_t>>(n, NUM_PES_PER_PORT);
        });

        pe_to_bus_plo_resp.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_response_t>>(n, NUM_PES_PER_PORT);
        });

        pe_busy.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<sc_signal<bool>>(n, NUM_PES_PER_PORT);
        });

        pe_halted.init(NUM_PORTS, [this, config](const char* n, size_t i) {
            return new sc_vector<sc_signal<bool>>(n, NUM_PES_PER_PORT);
        });

        ln_pli_plo.init(NUM_PORTS+1, [this, config](const char* n, size_t i) {
            return new sc_vector<VRDSIG<uint64_t>>(n, NUM_PES_PER_PORT);
        });

        // Bind internal modules
        bind();
    }

    NetworkOnChip(sc_module_name name)
        : NetworkOnChip(name, NetWorkOnChipConfig()) // Delegate to main constructor with default config
    {}

    // Debug: Dump internal state
    void dump_state() {
        std::cout << "\n[NoC] Dumping NetworkOnChip State:" << std::endl;
        std::cout << "--- MBUS ---" << std::endl;
        for(auto& mbus_inst : mbus) {
            mbus_inst.dump_state();
        }
        std::cout << "[NoC] End of NetworkOnChip State Dump\n" << std::endl;
    }

private:
    bool spatial_window_bound_ = false;
    uint16_t spatial_full_rows_ = 0u;
    uint16_t spatial_full_width_ = 0u;
    uint16_t spatial_window_loop_pc_ = 0u;
    uint16_t spatial_window_loop_end_pc_ = 0u;

    static bool is_loop_in(pe_inst_t instruction) {
        return pe::getOpcode(instruction) == 2
            && pe::getFunct2(instruction) == 0
            && pe::getFunc1(instruction) == 0;
    }

    static bool find_matching_loop_end(const std::vector<pe_inst_t>& program,
                                       size_t loop_index,
                                       uint16_t& loop_end_pc) {
        unsigned depth = 1u;
        for (size_t i = loop_index + 1u; i < program.size(); ++i) {
            if (is_loop_in(program[i])) {
                ++depth;
            }
            if ((program[i] & 0x1u) != 0u && --depth == 0u) {
                loop_end_pc = static_cast<uint16_t>(i * sizeof(pe_inst_t));
                return true;
            }
        }
        return false;
    }

    bool has_conv_spatial_program_signature() const {
        const std::vector<pe_inst_t>& program = pes[0][0].if_id_stage.IM.mem;
        return program.size() > 35u
            && program[0] == 0x004Cu
            && is_loop_in(program[1])
            && is_loop_in(program[10])
            && is_loop_in(program[15])
            && is_loop_in(program[17])
            && is_loop_in(program[26])
            && program[34] == 0x0015u
            && program[35] == 0x001Cu;
    }

    bool detect_chained_conv_topology(uint16_t& full_rows) const {
        size_t active_buses = 0u;
        while (active_buses < NUM_PORTS
               && mbus[active_buses].scan_config(0u).enable) {
            ++active_buses;
        }
        if (active_buses < 2u) {
            return false;
        }

        size_t rows = 0u;
        while (rows < NUM_PES_PER_PORT && mbus[0].scan_config(rows).enable) {
            ++rows;
        }
        if (rows == 0u) {
            return false;
        }

        const ScanChainFormat row0 = mbus[0].scan_config(0u);
        const ScanChainFormat next = rows > 1u
            ? mbus[0].scan_config(1u)
            : mbus[1].scan_config(0u);
        if (next.pd_id <= row0.pd_id) {
            return false;
        }
        const unsigned stride = next.pd_id - row0.pd_id;

        for (size_t bus = 0u; bus < NUM_PORTS; ++bus) {
            for (size_t row = 0u; row < NUM_PES_PER_PORT; ++row) {
                const ScanChainFormat config = mbus[bus].scan_config(row);
                const bool expected_enable = bus < active_buses && row < rows;
                if (config.enable != expected_enable) {
                    return false;
                }
                if (!expected_enable) {
                    continue;
                }

                const PERouterMode expected_mode = bus == 0u
                    ? PERouterMode::PLI_FROM_BUS_PLO_TO_LN
                    : (bus + 1u == active_buses
                        ? PERouterMode::PLI_FROM_LN_PLO_TO_BUS
                        : PERouterMode::PLI_FROM_LN_PLO_TO_LN);
                const unsigned expected_pd = (bus + row) * stride;
                if (expected_pd > 63u
                    || config.route_mode != expected_mode
                    || config.ps_id != bus
                    || config.pd_id != expected_pd
                    || config.pli_id != (bus == 0u ? row : 63u)
                    || config.plo_id != (bus + 1u == active_buses ? row : 63u)) {
                    return false;
                }
            }
        }

        full_rows = static_cast<uint16_t>(rows);
        return true;
    }

    bool detect_bus_attached_conv_topology(uint16_t& full_rows) const {
        size_t active_buses = 0u;
        while (active_buses < NUM_PORTS
               && mbus[active_buses].scan_config(0u).enable) {
            ++active_buses;
        }
        if (active_buses == 0u) {
            return false;
        }

        size_t rows = 0u;
        while (rows < NUM_PES_PER_PORT && mbus[0].scan_config(rows).enable) {
            ++rows;
        }
        if (rows == 0u) {
            return false;
        }

        unsigned stride = 1u;
        if (rows > 1u) {
            const ScanChainFormat row0 = mbus[0].scan_config(0u);
            const ScanChainFormat row1 = mbus[0].scan_config(1u);
            if (row1.pd_id <= row0.pd_id) {
                return false;
            }
            stride = row1.pd_id - row0.pd_id;
        }

        for (size_t bus = 0u; bus < NUM_PORTS; ++bus) {
            for (size_t row = 0u; row < NUM_PES_PER_PORT; ++row) {
                const ScanChainFormat config = mbus[bus].scan_config(row);
                const bool expected_enable = bus < active_buses && row < rows;
                if (config.enable != expected_enable) {
                    return false;
                }
                if (!expected_enable) {
                    continue;
                }
                const unsigned expected_pd = row * stride;
                if (expected_pd > 63u
                    || config.route_mode != PERouterMode::PLI_FROM_BUS_PLO_TO_BUS
                    || config.ps_id != 0u
                    || config.pd_id != expected_pd
                    || config.pli_id != row
                    || config.plo_id != row) {
                    return false;
                }
            }
        }

        full_rows = static_cast<uint16_t>(rows);
        return true;
    }

    bool detect_spatial_conv_topology(uint16_t& full_rows) const {
        return has_conv_spatial_program_signature()
            && (detect_chained_conv_topology(full_rows)
                || detect_bus_attached_conv_topology(full_rows));
    }

    bool locate_spatial_window_loop(uint16_t full_width,
                                    uint16_t& loop_pc,
                                    uint16_t& loop_end_pc) const {
        if (full_width < 2u) {
            return false;
        }
        const std::vector<pe_inst_t>& program = pes[0][0].if_id_stage.IM.mem;
        const unsigned encoded_count = full_width - 2u;
        bool found = false;
        for (size_t i = 0u; i < program.size(); ++i) {
            if (!is_loop_in(program[i])
                || static_cast<unsigned>(pe::getPayload(program[i])) != encoded_count) {
                continue;
            }
            uint16_t candidate_end_pc = 0u;
            if (found || !find_matching_loop_end(program, i, candidate_end_pc)) {
                return false;
            }
            found = true;
            loop_pc = static_cast<uint16_t>(i * sizeof(pe_inst_t));
            loop_end_pc = candidate_end_pc;
        }
        return found;
    }

    bool cached_spatial_window_is_valid(uint16_t full_rows) const {
        if (!spatial_window_bound_ || spatial_full_rows_ != full_rows) {
            return false;
        }
        const std::vector<pe_inst_t>& program = pes[0][0].if_id_stage.IM.mem;
        const size_t loop_index = spatial_window_loop_pc_ / sizeof(pe_inst_t);
        uint16_t loop_end_pc = 0u;
        return spatial_full_width_ >= 2u
            && loop_index < program.size()
            && is_loop_in(program[loop_index])
            && static_cast<uint16_t>(pe::getPayload(program[loop_index]))
                == static_cast<uint16_t>(spatial_full_width_ - 2u)
            && find_matching_loop_end(program, loop_index, loop_end_pc)
            && loop_end_pc == spatial_window_loop_end_pc_;
    }

    void clear_spatial_wave() {
        spatial_window_bound_ = false;
        spatial_active_rows.write(NUM_PES_PER_PORT);
        for (size_t bus = 0u; bus < NUM_PORTS; ++bus) {
            for (size_t row = 0u; row < NUM_PES_PER_PORT; ++row) {
                pes[bus][row].if_id_stage.clear_spatial_window_loop();
            }
        }
    }

    void bind() {
        // NoC Router bindings (Dual Plane Integrated)
        router.clk(clk);
        router.reset_n(reset_n);
        router.command_mode(command_mode);
        router.command_data(command_data);

        // Bind external ports to router
        bind_vr_interface(router.noc_ps_in, noc_ps_in);
        bind_vr_interface(router.noc_pd_in, noc_pd_in);
        bind_vr_interface(router.noc_pli_in, noc_pli_in);
        bind_vr_interface(router.noc_plo_in, noc_plo_in);
        bind_vr_interface(noc_plo_out, router.noc_plo_out);

        for (size_t i = 0; i < NUM_PORTS; ++i) {
            // Connect NoC to MBUS request signals
            connect_vr_signals(router.noc_ps_to_bus_req[i], noc_ps_to_bus_req[i]);
            connect_vr_signals(router.noc_pd_to_bus_req[i], noc_pd_to_bus_req[i]);
            connect_vr_signals(router.noc_pli_to_bus_req[i], noc_pli_to_bus_req[i]);
            connect_vr_signals(router.noc_plo_to_bus_req[i], noc_plo_to_bus_req[i]);
            connect_vr_signals(router.bus_to_noc_plo_resp[i], bus_to_noc_plo_resp[i]);

            // Scan chain: router -> mbus
            router.scan_chain_in[i](mbus_scan_chain_out[i]);
            router.scan_chain_out[i](router_scan_chain_out[i]);
        }
        router.scan_chain_enable(scan_chain_enable);

        // MBUS bindings
        for (size_t i = 0; i < NUM_PORTS; ++i) {
            noc::MBUS& mbus_inst = mbus[i];

            mbus_inst.clk(clk);
            mbus_inst.reset_n(reset_n);
            mbus_inst.active_pe_count(spatial_active_rows);

            // Connect NoC to MBUS request signals
            connect_vr_signals(mbus_inst.noc_ps_to_bus_req, noc_ps_to_bus_req[i]);
            connect_vr_signals(mbus_inst.noc_pd_to_bus_req, noc_pd_to_bus_req[i]);
            connect_vr_signals(mbus_inst.noc_pli_to_bus_req, noc_pli_to_bus_req[i]);
            connect_vr_signals(mbus_inst.noc_plo_to_bus_req, noc_plo_to_bus_req[i]);
            connect_vr_signals(mbus_inst.bus_to_noc_plo_resp, bus_to_noc_plo_resp[i]);

            // Scan chain: router -> mbus
            mbus_inst.scan_chain_enable(scan_chain_enable);
            mbus_inst.scan_chain_in(router_scan_chain_out[i]);
            mbus_inst.scan_chain_out(mbus_scan_chain_out[i]);

            // Connect PE interface signals
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                mbus_inst.router_enable[j](router_enable[i][j]);
                mbus_inst.router_mode[j](router_mode[i][j]);

                connect_vr_signals(mbus_inst.bus_to_pe_ps_req[j], bus_to_pe_ps_req[i][j]);
                connect_vr_signals(mbus_inst.bus_to_pe_pd_req[j], bus_to_pe_pd_req[i][j]);
                connect_vr_signals(mbus_inst.bus_to_pe_pli_req[j], bus_to_pe_pli_req[i][j]);
                connect_vr_signals(mbus_inst.bus_to_pe_plo_req[j], bus_to_pe_plo_req[i][j]);
                connect_vr_signals(mbus_inst.pe_to_bus_plo_resp[j], pe_to_bus_plo_resp[i][j]);

                mbus_inst.pe_busy[j](pe_busy[i][j]);
            }
        }

        // PEs bindings
        for (size_t i = 0; i < NUM_PORTS; ++i) {
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                pe::ProcessElement& pe_inst = pes[i][j];

                // Connect clock and reset
                pe_inst.clk(clk);
                pe_inst.reset_n(reset_n);

                // Router config ports
                pe_inst.router_enable(router_enable[i][j]);
                pe_inst.router_mode(router_mode[i][j]);

                // NoC interface
                connect_vr_signals(pe_inst.noc_ps_in, bus_to_pe_ps_req[i][j]);
                connect_vr_signals(pe_inst.noc_pd_in, bus_to_pe_pd_req[i][j]);
                connect_vr_signals(pe_inst.noc_pli_in, bus_to_pe_pli_req[i][j]);
                connect_vr_signals(pe_inst.noc_plo_in, bus_to_pe_plo_req[i][j]);
                connect_vr_signals(pe_inst.noc_plo_out, pe_to_bus_plo_resp[i][j]);

                // PE busy signal
                pe_inst.pe_busy(pe_busy[i][j]);
                pe_inst.pe_halted(pe_halted[i][j]);

                // Connect Local Network signals (PE-to-PE communication)
                connect_vr_signals(pe_inst.ln_pli, ln_pli_plo[i][j]);
                connect_vr_signals(pe_inst.ln_plo, ln_pli_plo[i+1][j]);

            }
        }
    }

public:
    uint16_t configure_spatial_wave(uint16_t valid_height, uint16_t valid_width) {
        uint16_t full_rows = 0u;
        if (valid_height == 0u || valid_width == 0u
            || !detect_spatial_conv_topology(full_rows)) {
            clear_spatial_wave();
            return 0u;
        }

        const uint16_t active_rows = valid_height < full_rows ? valid_height : full_rows;
        spatial_active_rows.write(active_rows);

        if (!cached_spatial_window_is_valid(full_rows)) {
            spatial_window_bound_ = locate_spatial_window_loop(
                valid_width, spatial_window_loop_pc_, spatial_window_loop_end_pc_);
            if (spatial_window_bound_) {
                spatial_full_rows_ = full_rows;
                spatial_full_width_ = valid_width;
            }
        }

        const bool width_is_valid = spatial_window_bound_
            && valid_width <= spatial_full_width_;
        for (size_t bus = 0u; bus < NUM_PORTS; ++bus) {
            for (size_t row = 0u; row < NUM_PES_PER_PORT; ++row) {
                if (width_is_valid) {
                    pes[bus][row].if_id_stage.configure_spatial_window_loop(
                        spatial_window_loop_pc_, spatial_window_loop_end_pc_, valid_width);
                } else {
                    pes[bus][row].if_id_stage.clear_spatial_window_loop();
                }
            }
        }
        return width_is_valid ? spatial_full_width_ : 0u;
    }

    bool any_pe_busy() const {
        for (size_t i = 0; i < NUM_PORTS; ++i) {
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                if (pe_busy[i][j].read()) {
                    return true;
                }
            }
        }
        return false;
    }

    bool all_active_pes_halted() const {
        for (size_t i = 0; i < NUM_PORTS; ++i) {
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                if (!router_enable[i][j].read()) {
                    continue;
                }
                if (!pe_halted[i][j].read()) {
                    return false;
                }
            }
        }
        return true;
    }

    bool any_router_pending_resp() const {
        for (size_t i = 0; i < NUM_PORTS; ++i) {
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                if (!router_enable[i][j].read()) {
                    continue;
                }
                if (pes[i][j].router.has_pending_quiesce()) {
                    return true;
                }
            }
        }
        return false;
    }

    bool any_router_fifo_nonempty() const {
        for (size_t i = 0; i < NUM_PORTS; ++i) {
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                if (!router_enable[i][j].read()) {
                    continue;
                }
                if (pes[i][j].router.any_fifo_nonempty()) {
                    return true;
                }
            }
        }
        return false;
    }

    uint32_t status_word() const {
        constexpr uint32_t kAnyPeBusyBit = 0u;
        constexpr uint32_t kAllActivePesHaltedBit = 1u;
        constexpr uint32_t kAnyRouterPendingRespBit = 2u;
        constexpr uint32_t kAnyRouterFifoNonemptyBit = 3u;

        uint32_t status = 0u;
        if (any_pe_busy()) {
            status |= (1u << kAnyPeBusyBit);
        }
        if (all_active_pes_halted()) {
            status |= (1u << kAllActivePesHaltedBit);
        }
        if (any_router_pending_resp()) {
            status |= (1u << kAnyRouterPendingRespBit);
        }
        if (any_router_fifo_nonempty()) {
            status |= (1u << kAnyRouterFifoNonemptyBit);
        }
        return status;
    }

    /// NoC is quiesced when all PEs halted, all PE routers quiesced, and central router quiesced.
    bool is_quiesced() const {
        return all_active_pes_halted()
            && !any_router_pending_resp()
            && !any_router_fifo_nonempty()
            && router.is_quiesced();
    }

    std::pair<uint32_t, uint32_t> enable_perffeto_trace(uint32_t start_pid, uint32_t start_tid) {
        uint32_t next_pid = start_pid;
        uint32_t next_tid = start_tid;

        router.set_trace_context(next_pid, static_cast<int>(next_tid));
        next_pid += 1;
        next_tid += static_cast<uint32_t>(router.get_trace_num() + 1);

        for (size_t i = 0; i < NUM_PORTS; ++i) {
            mbus[i].set_trace_context(next_pid, static_cast<int>(next_tid));
            next_tid += static_cast<uint32_t>(mbus[i].get_trace_num() + 1);
        }
        next_pid += 1;


        for (size_t i = 0; i < NUM_PORTS; ++i) {
            for (size_t j = 0; j < NUM_PES_PER_PORT; ++j) {
                pes[i][j].set_trace_context(next_pid, static_cast<int>(next_tid));
                next_tid += static_cast<uint32_t>(pes[i][j].get_trace_num() + 1);
            }
        }
        next_pid += 1;

        return {next_pid, next_tid};
    }

    void enable_perffeto_trace(){
        (void)enable_perffeto_trace(100, 1000);
    }
};

} // namespace hybridacc
