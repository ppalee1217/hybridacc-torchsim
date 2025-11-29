#pragma once

#include <queue>
#include "utils.hpp"
#include <systemc>
#include <cassert>
#include "FIFO.hpp"

// PE Router Command and ID field definitions
#define PE_CMD_ADDRESS 0x100

#define PE_CMD_OFFSET 0
#define PE_CMD_BITS 4

// Command - Initialize
#define PE_ROUTER_PS_ID_OFFSET 4
#define PE_ROUTER_PD_ID_OFFSET 10
#define PE_ROUTER_PLI_ID_OFFSET 16
#define PE_ROUTER_PLO_ID_OFFSET 22
#define PE_ROUTER_MODE_ID_OFFSET 28
#define PE_ROUTER_EN_ID_OFFSET 30

#define PE_ROUTER_PS_ID_MASK 0x3F // 6 bits
#define PE_ROUTER_PD_ID_MASK 0x3F // 6 bits
#define PE_ROUTER_PLI_ID_MASK 0x3F // 6 bits
#define PE_ROUTER_PLO_ID_MASK 0x3F // 6 bits
#define PE_ROUTER_MODE_ID_MASK 0x03 // 2 bits
#define PE_ROUTER_EN_ID_MASK 0x01 // 6 bits

// Command - Load Program
#define PE_ROUTER_IM_ADDR_OFFSET 4
#define PE_ROUTER_IM_DATA_OFFSET 20

#define PE_ROUTER_IM_ADDR_MASK 0xFFFF // 16 bits
#define PE_ROUTER_IM_DATA_MASK 0xFFFF // 16 bits

namespace hybridacc {
namespace pe {

// -----------------------------------------------------------------------------
enum class PERouterMode {
    PLI_FROM_LN_PLO_TO_LN = 0b00,  // PLI from LN, PLO to LN
    PLI_FROM_BUS_PLO_TO_LN = 0b01, // PLI from bus, PLO to LN
    PLI_FROM_LN_PLO_TO_BUS = 0b10, // PLI from LN, PLO to Bus
    PLI_FROM_BUS_PLO_TO_BUS = 0b11  // PLI from bus, PLO to Bus
};

// Add operator<< support for PERouterMode
inline std::ostream& operator<<(std::ostream& os, PERouterMode mode) {
    switch (mode) {
        case PERouterMode::PLI_FROM_LN_PLO_TO_LN: return os << "PLI_FROM_LN_PLO_TO_LN";
        case PERouterMode::PLI_FROM_BUS_PLO_TO_LN: return os << "PLI_FROM_BUS_PLO_TO_LN";
        case PERouterMode::PLI_FROM_LN_PLO_TO_BUS: return os << "PLI_FROM_LN_PLO_TO_BUS";
        case PERouterMode::PLI_FROM_BUS_PLO_TO_BUS: return os << "PLI_FROM_BUS_PLO_TO_BUS";
        default: return os << "UNKNOWN";
    }
}

// sc_trace support for PERouterMode
inline void sc_trace(sc_core::sc_trace_file* tf, const PERouterMode& mode, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(mode), name);
}

// -----------------------------------------------------------------------------
enum class message_command_t {
    CMD_RESET = 0, // clear reg
    CMD_INIT = 1, // setting ids, mode, enable
    CMD_LOAD_PROGRAM = 2, // load program to IM
    CMD_STOP_PE = 3, // stop PE operation
    CMD_START_PE = 4, // start PE operation
};

// Add operator<< support for message_command_t
inline std::ostream& operator<<(std::ostream& os, message_command_t command) {
    switch (command) {
        case message_command_t::CMD_RESET: return os << "CMD_RESET";
        case message_command_t::CMD_INIT: return os << "CMD_INIT";
        case message_command_t::CMD_LOAD_PROGRAM: return os << "CMD_LOAD_PROGRAM";
        case message_command_t::CMD_STOP_PE: return os << "CMD_STOP_PE";
        case message_command_t::CMD_START_PE: return os << "CMD_START_PE";
        default: return os << "UNKNOWN";
    }
}

// sc_trace support for message_command_t
inline void sc_trace(sc_core::sc_trace_file* tf, const message_command_t& command, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(command), name);
}

// -----------------------------------------------------------------------------
// state enumeration - redesigned for hardware implementation
enum class PErouterState {
    IDLE,      // Handle operations, accept new requests
    WAIT_RESP, // Waiting for response to be acknowledged
};

// Add operator<< support for PErouterState
inline std::ostream& operator<<(std::ostream& os, PErouterState state) {
    switch (state) {
        case PErouterState::IDLE: return os << "IDLE";
        case PErouterState::WAIT_RESP: return os << "WAIT_RESP";
        default: return os << "UNKNOWN";
    }
}

// sc_trace support for PErouterState
inline void sc_trace(sc_core::sc_trace_file* tf, const PErouterState& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}


// -----------------------------------------------------------------------------
SC_MODULE(PErouter) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    //======= NoC Ports =======//
    // NoC enable ports
    sc_in<bool> enable;
    sc_in<PERouterMode> route_mode;

    // NoC input ports
    VRDIF<noc_request_t> noc_req_in_if;

    // NoC output ports
    VRDOF<noc_response_t> noc_resp_out_if;

    //======= LN Ports =======//
    // LN input ports - using VRDIF
    VRDIF<uint64_t> ln_pli_in_if;

    // LN output ports - using VRDOF
    VRDOF<uint64_t> ln_plo_out_if;

    //======= Internal Ports =======//
    // Internal port - top controller
    sc_out<bool> pe_reset;
    sc_out<bool> pe_start;
    sc_out<bool> pe_program; // signal to indicate instruction programming (Mux switch)

    // Internal port - IM writer
    sc_out<bool> im_write_en;
    sc_out<uint16_t> im_write_addr;
    sc_out<pe_inst_t> im_write_data;

    // port static - using VRDOF
    VRDOF<uint64_t> pe_ps_out_if;

    // port dynamic - using VRDOF
    VRDOF<uint16_t> pe_pd_out_if;

    // port local input - using VRDOF
    VRDOF<uint64_t> pe_pli_out_if;

    // port local output - using VRDIF
    VRDIF<uint64_t> pe_plo_in_if;

    // methods
    SC_CTOR(PErouter)
        : clk("clk"),
          reset_n("reset_n"),
          enable("enable"),
          route_mode("route_mode"),
          noc_req_in_if("noc_req_in_if"),
          noc_resp_out_if("noc_resp_out_if"),
          ln_pli_in_if("ln_pli_in_if"),
          ln_plo_out_if("ln_plo_out_if"),
          pe_reset("pe_reset"),
          pe_start("pe_start"),
          pe_program("pe_program"),
          im_write_en("im_write_en"),
          im_write_addr("im_write_addr"),
          im_write_data("im_write_data"),
          pe_ps_out_if("pe_ps_out_if"),
          pe_pd_out_if("pe_pd_out_if"),
          pe_pli_out_if("pe_pli_out_if"),
          pe_plo_in_if("pe_plo_in_if"),
          ps_fifo("ps_fifo", max_queue_size),
          pd_fifo("pd_fifo", max_queue_size),
          pli_fifo("pli_fifo", max_queue_size),
          plo_fifo("plo_fifo", max_queue_size)
    {
        DEBUG_MSG("[Create] PErouter");

        //  綁定 FIFO 的時鐘和 reset 信號
        ps_fifo.clk(clk);
        ps_fifo.reset_n(reset_n);
        ps_fifo.data_in(ps_fifo_data_in_sig);
        ps_fifo.push(ps_fifo_push_sig);
        ps_fifo.data_out(ps_fifo_data_out_sig);
        ps_fifo.pop(ps_fifo_pop_sig);
        ps_fifo.empty(ps_fifo_empty_sig);
        ps_fifo.full(ps_fifo_full_sig);

        pd_fifo.clk(clk);
        pd_fifo.reset_n(reset_n);
        pd_fifo.data_in(pd_fifo_data_in_sig);
        pd_fifo.push(pd_fifo_push_sig);
        pd_fifo.data_out(pd_fifo_data_out_sig);
        pd_fifo.pop(pd_fifo_pop_sig);
        pd_fifo.empty(pd_fifo_empty_sig);
        pd_fifo.full(pd_fifo_full_sig);

        pli_fifo.clk(clk);
        pli_fifo.reset_n(reset_n);
        pli_fifo.data_in(pli_fifo_data_in_sig);
        pli_fifo.push(pli_fifo_push_sig);
        pli_fifo.data_out(pli_fifo_data_out_sig);
        pli_fifo.pop(pli_fifo_pop_sig);
        pli_fifo.empty(pli_fifo_empty_sig);
        pli_fifo.full(pli_fifo_full_sig);

        plo_fifo.clk(clk);
        plo_fifo.reset_n(reset_n);
        plo_fifo.data_in(plo_fifo_data_in_sig);
        plo_fifo.push(plo_fifo_push_sig);
        plo_fifo.data_out(plo_fifo_data_out_sig);
        plo_fifo.pop(plo_fifo_pop_sig);
        plo_fifo.empty(plo_fifo_empty_sig);
        plo_fifo.full(plo_fifo_full_sig);

        SC_METHOD(combinational_process);
        sensitive << reset_n << state_reg << noc_req_in_if.valid_in << noc_req_in_if.data_in << enable << route_mode
                  << noc_resp_out_if.ready_in << ln_pli_in_if.valid_in << ln_pli_in_if.data_in << ln_plo_out_if.ready_in
                  << pe_ps_out_if.ready_in << pe_pd_out_if.ready_in << pe_pli_out_if.ready_in << pe_plo_in_if.valid_in << pe_plo_in_if.data_in
                  << pending_noc_req_reg << current_noc_req_reg
                  << ps_fifo_empty_sig << ps_fifo_full_sig << ps_fifo_data_out_sig
                  << pd_fifo_empty_sig << pd_fifo_full_sig << pd_fifo_data_out_sig
                  << pli_fifo_empty_sig << pli_fifo_full_sig << pli_fifo_data_out_sig
                  << plo_fifo_empty_sig << plo_fifo_full_sig << plo_fifo_data_out_sig;
        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

    // ========================= Register definitions =========================
    // State registers
    sc_signal<PErouterState> state_reg;
    sc_signal<PErouterState> state_next;

    // Pending request registers
    sc_signal<bool> pending_noc_req_reg;
    sc_signal<bool> pending_noc_req_next;

    sc_signal<noc_request_t> current_noc_req_reg;
    sc_signal<noc_request_t> current_noc_req_next;

    // Control signal registers
    sc_signal<bool> im_write_en_reg;
    sc_signal<bool> im_write_en_next;

    sc_signal<uint16_t> im_write_addr_reg;
    sc_signal<uint16_t> im_write_addr_next;

    sc_signal<pe_inst_t> im_write_data_reg;
    sc_signal<pe_inst_t> im_write_data_next;

    sc_signal<bool> pe_reset_reg;
    sc_signal<bool> pe_reset_next;

    sc_signal<bool> pe_start_reg;
    sc_signal<bool> pe_start_next;

    sc_signal<bool> pe_program_reg;
    sc_signal<bool> pe_program_next;

    // Channel-specific data queues (these are not registers in HDL sense)
    const int max_queue_size = 4;

    FIFO<uint64_t> ps_fifo;   // PS channel: NoC -> PE (weights)
    FIFO<uint16_t> pd_fifo;   // PD channel: NoC -> PE (activations)
    FIFO<uint64_t> pli_fifo;  // PLI channel: NoC/LN -> PE (partial inputs)
    FIFO<uint64_t> plo_fifo;  // PLO channel: PE -> NoC/LN (partial outputs)

    // PS FIFO signals
    sc_signal<uint64_t> ps_fifo_data_in_sig;
    sc_signal<bool> ps_fifo_push_sig;
    sc_signal<uint64_t> ps_fifo_data_out_sig;
    sc_signal<bool> ps_fifo_pop_sig;
    sc_signal<bool> ps_fifo_empty_sig;
    sc_signal<bool> ps_fifo_full_sig;

    // PD FIFO signals
    sc_signal<uint16_t> pd_fifo_data_in_sig;
    sc_signal<bool> pd_fifo_push_sig;
    sc_signal<uint16_t> pd_fifo_data_out_sig;
    sc_signal<bool> pd_fifo_pop_sig;
    sc_signal<bool> pd_fifo_empty_sig;
    sc_signal<bool> pd_fifo_full_sig;

    // PLI FIFO signals
    sc_signal<uint64_t> pli_fifo_data_in_sig;
    sc_signal<bool> pli_fifo_push_sig;
    sc_signal<uint64_t> pli_fifo_data_out_sig;
    sc_signal<bool> pli_fifo_pop_sig;
    sc_signal<bool> pli_fifo_empty_sig;
    sc_signal<bool> pli_fifo_full_sig;

    // PLO FIFO signals
    sc_signal<uint64_t> plo_fifo_data_in_sig;
    sc_signal<bool> plo_fifo_push_sig;
    sc_signal<uint64_t> plo_fifo_data_out_sig;
    sc_signal<bool> plo_fifo_pop_sig;
    sc_signal<bool> plo_fifo_empty_sig;
    sc_signal<bool> plo_fifo_full_sig;

    // ========================= Helper functions =========================

    bool can_accept_ps() { return !ps_fifo_full_sig.read(); }
    bool can_accept_pd() { return !pd_fifo_full_sig.read(); }
    bool can_accept_pli() { return !pli_fifo_full_sig.read(); }
    bool can_accept_plo() { return !plo_fifo_full_sig.read(); }

    bool has_ps_data() { return !ps_fifo_empty_sig.read(); }
    bool has_pd_data() { return !pd_fifo_empty_sig.read(); }
    bool has_pli_data() { return !pli_fifo_empty_sig.read(); }
    bool has_plo_data() { return !plo_fifo_empty_sig.read(); }

    NOC_CHANNELS get_noc_channel(uint16_t addr) {
        int ch = (addr >> 6) & 0x3;
        switch (ch) {
            case 0: return NOC_CHANNEL_PS;
            case 1: return NOC_CHANNEL_PD;
            case 2: return NOC_CHANNEL_PLI;
            case 3: return NOC_CHANNEL_PLO;
            default: return NOC_CHANNEL_PS;
        }
    }

    bool can_route_to_bus(NOC_CHANNELS channel) {
        PERouterMode mode = route_mode.read();
        switch (channel) {
            case NOC_CHANNEL_PLI:
                return (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_LN) ||
                       (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);
            case NOC_CHANNEL_PLO:
                return (mode == PERouterMode::PLI_FROM_LN_PLO_TO_BUS) ||
                       (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);
            default:
                return true;
        }
    }

    bool can_route_from_ln(NOC_CHANNELS channel) {
        PERouterMode mode = route_mode.read();
        switch (channel) {
            case NOC_CHANNEL_PLI:
                return (mode == PERouterMode::PLI_FROM_LN_PLO_TO_LN) ||
                       (mode == PERouterMode::PLI_FROM_LN_PLO_TO_BUS);
            case NOC_CHANNEL_PLO:
                return (mode == PERouterMode::PLI_FROM_LN_PLO_TO_LN) ||
                       (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_LN);
            default:
                return false;
        }
    }

    // ========================= Combinational Logic =========================
    void combinational_process() {
        ps_fifo_pop_sig.write(false);
        pd_fifo_pop_sig.write(false);
        pli_fifo_pop_sig.write(false);
        plo_fifo_pop_sig.write(false);

        ps_fifo_push_sig.write(false);
        pd_fifo_push_sig.write(false);
        pli_fifo_push_sig.write(false);
        plo_fifo_push_sig.write(false);

        // Default: hold current values (reg -> next)
        state_next.write(state_reg.read());
        pending_noc_req_next.write(pending_noc_req_reg.read());
        current_noc_req_next.write(current_noc_req_reg.read());

        im_write_en_next.write(false);  // Single-cycle pulse
        im_write_addr_next.write(im_write_addr_reg.read());
        im_write_data_next.write(im_write_data_reg.read());

        pe_reset_next.write(false);     // Single-cycle pulse
        pe_start_next.write(false);     // Single-cycle pulse
        pe_program_next.write(false);   // Single-cycle pulse

        // Default outputs
        noc_req_in_if.ready_out.write(false);
        noc_resp_out_if.valid_out.write(false);
        noc_resp_out_if.data_out.write(noc_response_t());

        ln_pli_in_if.ready_out.write(false);
        ln_plo_out_if.valid_out.write(false);
        ln_plo_out_if.data_out.write(0);

        pe_ps_out_if.valid_out.write(false);
        pe_ps_out_if.data_out.write(0);
        pe_pd_out_if.valid_out.write(false);
        pe_pd_out_if.data_out.write(0);
        pe_pli_out_if.valid_out.write(false);
        pe_pli_out_if.data_out.write(0);
        pe_plo_in_if.ready_out.write(false);

        if (!reset_n.read()) {
            return;
        }

        PErouterState current_state = state_reg.read();
        PERouterMode mode = route_mode.read();
        bool enabled = enable.read();

        // ==================== State Machine Logic ====================
        switch (current_state) {
            case PErouterState::IDLE:
            {
                // Handle incoming NoC requests
                if (noc_req_in_if.valid_in.read()) {
                    noc_request_t req = noc_req_in_if.data_in.read();

                    // Command requests - always accepted
                    if (req.addr == PE_CMD_ADDRESS && req.is_w) {
                        noc_req_in_if.ready_out.write(true);

                        // Process command
                        message_command_t cmd = static_cast<message_command_t>(req.data & 0xF);
                        switch (cmd) {
                            case message_command_t::CMD_RESET:
                                pe_reset_next.write(true);
                                break;
                            case message_command_t::CMD_LOAD_PROGRAM:
                                im_write_addr_next.write((req.data >> PE_ROUTER_IM_ADDR_OFFSET) & PE_ROUTER_IM_ADDR_MASK);
                                im_write_data_next.write((req.data >> PE_ROUTER_IM_DATA_OFFSET) & PE_ROUTER_IM_DATA_MASK);
                                im_write_en_next.write(true);
                                pe_program_next.write(true);
                                break;
                            case message_command_t::CMD_START_PE:
                                pe_start_next.write(true);
                                break;
                            default:
                                break;
                        }
                        state_next.write(PErouterState::IDLE);
                    }
                    // Data write requests - single cycle if queue has space
                    else if (enabled && req.is_w) {
                        NOC_CHANNELS channel = get_noc_channel(req.addr);
                        bool can_accept = false;

                        switch (channel) {
                            case NOC_CHANNEL_PS:
                                can_accept = can_accept_ps();
                                if (can_accept) {
                                    ps_fifo_data_in_sig.write(req.data);
                                    ps_fifo_push_sig.write(true);
                                }
                                break;
                            case NOC_CHANNEL_PD:
                                can_accept = can_accept_pd();
                                if (can_accept) {
                                    pd_fifo_data_in_sig.write(static_cast<uint16_t>(req.data));
                                    pd_fifo_push_sig.write(true);
                                }
                                break;
                            case NOC_CHANNEL_PLI:
                                can_accept = can_route_to_bus(channel) && can_accept_pli();
                                if (can_accept) {
                                    pli_fifo_data_in_sig.write(req.data);
                                    pli_fifo_push_sig.write(true);
                                }
                                break;
                            default:
                                break;
                        }

                        noc_req_in_if.ready_out.write(can_accept);
                        state_next.write(PErouterState::IDLE);
                    }
                    // Read requests - accept request, prepare for next cycle response
                    else if (enabled && !req.is_w) {
                        NOC_CHANNELS channel = get_noc_channel(req.addr);
                        if (channel == NOC_CHANNEL_PLO) {
                            // Accept request if data is available
                            if (has_plo_data()) {
                                noc_req_in_if.ready_out.write(true);
                                current_noc_req_next.write(req);
                                pending_noc_req_next.write(true);
                                state_next.write(PErouterState::WAIT_RESP);
                            } else {
                                // Data not ready, reject request
                                noc_req_in_if.ready_out.write(false);
                                state_next.write(PErouterState::IDLE);
                            }
                        }
                    }
                }
                // Handle LN PLI input
                else if (enabled && ln_pli_in_if.valid_in.read() && can_route_from_ln(NOC_CHANNEL_PLI) && can_accept_pli()) {
                    ln_pli_in_if.ready_out.write(true);
                    pli_fifo_data_in_sig.write(ln_pli_in_if.data_in.read());
                    pli_fifo_push_sig.write(true);
                    state_next.write(PErouterState::IDLE);
                }

                //  Data output to PE - 提供數據並在握手成功時 pop FIFO
                if (enabled) {
                    // PS channel: NoC -> PE
                    if (has_ps_data()) {
                        pe_ps_out_if.valid_out.write(true);
                        pe_ps_out_if.data_out.write(ps_fifo_data_out_sig.read());
                        //  如果握手成功，設定 pop 信號
                        if (pe_ps_out_if.ready_in.read()) {
                            ps_fifo_pop_sig.write(true);
                        }
                    }

                    // PD channel: NoC -> PE
                    if (has_pd_data()) {
                        pe_pd_out_if.valid_out.write(true);
                        pe_pd_out_if.data_out.write(pd_fifo_data_out_sig.read());
                        if (pe_pd_out_if.ready_in.read()) {
                            pd_fifo_pop_sig.write(true);
                        }
                    }

                    // PLI channel: NoC/LN -> PE
                    if (has_pli_data()) {
                        pe_pli_out_if.valid_out.write(true);
                        pe_pli_out_if.data_out.write(pli_fifo_data_out_sig.read());
                        if (pe_pli_out_if.ready_in.read()) {
                            pli_fifo_pop_sig.write(true);
                        }
                    }

                    // PLO channel: PE -> internal FIFO
                    if (pe_plo_in_if.valid_in.read() && can_accept_plo()) {
                        pe_plo_in_if.ready_out.write(true);
                        plo_fifo_data_in_sig.write(pe_plo_in_if.data_in.read());
                        plo_fifo_push_sig.write(true);
                    }

                    // PLO output
                    if (has_plo_data()) {
                        bool plo_to_ln = (mode == PERouterMode::PLI_FROM_LN_PLO_TO_LN) ||
                                         (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_LN);
                        bool plo_to_noc = (mode == PERouterMode::PLI_FROM_LN_PLO_TO_BUS) ||
                                          (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);

                        if (plo_to_ln && ln_plo_out_if.ready_in.read()) {
                            ln_plo_out_if.valid_out.write(true);
                            ln_plo_out_if.data_out.write(plo_fifo_data_out_sig.read());
                            plo_fifo_pop_sig.write(true);
                        } else if (plo_to_noc && noc_resp_out_if.ready_in.read()) {
                            noc_resp_out_if.valid_out.write(true);
                            noc_response_t resp;
                            resp.data = plo_fifo_data_out_sig.read();
                            noc_resp_out_if.data_out.write(resp);
                            plo_fifo_pop_sig.write(true);
                        }
                    }
                }
                break;
            }

            case PErouterState::WAIT_RESP:
            {
                if (pending_noc_req_reg.read()) {
                    // Send response
                    noc_resp_out_if.valid_out.write(true);
                    if (has_plo_data()) {
                        noc_response_t resp;
                        resp.data = plo_fifo_data_out_sig.read();
                        noc_resp_out_if.data_out.write(resp);

                        //  如果握手成功，設定 pop 信號
                        if (noc_resp_out_if.ready_in.read()) {
                            plo_fifo_pop_sig.write(true);
                            pending_noc_req_next.write(false);
                            state_next.write(PErouterState::IDLE);
                        } else {
                            state_next.write(PErouterState::WAIT_RESP);
                        }
                    }
                } else {
                    state_next.write(PErouterState::IDLE);
                }
                break;
            }

            default:
                state_next.write(PErouterState::IDLE);
                break;
        }
    }

    // ========================= Sequential Logic =========================
    void sequential_process() {
        // Reset
        state_reg.write(PErouterState::IDLE);
        pending_noc_req_reg.write(false);
        current_noc_req_reg.write(noc_request_t());

        im_write_en_reg.write(false);
        im_write_addr_reg.write(0);
        im_write_data_reg.write(0);
        pe_reset_reg.write(false);
        pe_start_reg.write(false);
        pe_program_reg.write(false);

        wait();

        while (true) {
            // ========== Register updates: next -> reg ==========
            state_reg.write(state_next.read());
            pending_noc_req_reg.write(pending_noc_req_next.read());
            current_noc_req_reg.write(current_noc_req_next.read());

            im_write_en_reg.write(im_write_en_next.read());
            im_write_addr_reg.write(im_write_addr_next.read());
            im_write_data_reg.write(im_write_data_next.read());
            pe_reset_reg.write(pe_reset_next.read());
            pe_start_reg.write(pe_start_next.read());
            pe_program_reg.write(pe_program_next.read());

            // Output control signals from registers
            im_write_en.write(im_write_en_reg.read());
            im_write_addr.write(im_write_addr_reg.read());
            im_write_data.write(im_write_data_reg.read());
            pe_reset.write(pe_reset_reg.read());
            pe_start.write(pe_start_reg.read());
            pe_program.write(pe_program_reg.read());

            //  時序邏輯只負責更新暫存器，不再處理 FIFO 操作
            // FIFO 的 push/pop 信號已經在組合邏輯中設定

            DEBUG_MSG("[PErouter] State: " << state_reg.read()
                      << " PS_empty=" << ps_fifo_empty_sig.read()
                      << " PS_full=" << ps_fifo_full_sig.read()
                      << " ps_pop=" << ps_fifo_pop_sig.read()
                      << " PD_empty=" << pd_fifo_empty_sig.read()
                      << " PD_full=" << pd_fifo_full_sig.read()
                      << " pd_pop=" << pd_fifo_pop_sig.read()
                      << " PLI_empty=" << pli_fifo_empty_sig.read()
                      << " PLI_full=" << pli_fifo_full_sig.read()
                      << " pli_pop=" << pli_fifo_pop_sig.read()
                      << " PLO_empty=" << plo_fifo_empty_sig.read()
                      << " PLO_full=" << plo_fifo_full_sig.read()
                      << " plo_pop=" << plo_fifo_pop_sig.read());
            wait();
        }
    }
};

} // namespace pe
} // namespace hybridacc
