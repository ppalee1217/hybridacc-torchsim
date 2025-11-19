#pragma once

#include <queue>
#include "utils.hpp"
#include <systemc>
#include <cassert>

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
    sc_in<noc_request_t> req_in;
    sc_in<bool> req_in_valid;
    sc_out<bool> req_in_ready;

    // NoC output ports
    sc_out<noc_response_t> resp_out;
    sc_out<bool> resp_out_valid;
    sc_in<bool> resp_out_ready;

    //======= LN Ports =======//
    // LN input ports
    sc_in<uint64_t> ln_pli_in_data;
    sc_in<bool> ln_pli_in_valid;
    sc_out<bool> ln_pli_in_ready;

    // LN output ports
    sc_out<uint64_t> ln_plo_out_data;
    sc_out<bool> ln_plo_out_valid;
    sc_in<bool> ln_plo_out_ready;

    //======= Internal Ports =======//
    // Internal port - top controller
    sc_out<bool> pe_reset;
    sc_out<bool> pe_start;
    sc_out<bool> pe_program; // signal to indicate instruction programming (Mux switch)

    // Internal port - IM writer
    sc_out<bool> im_write_en;
    sc_out<uint16_t> im_write_addr;
    sc_out<pe_inst_t> im_write_data;

    // port static
    sc_out<uint64_t> pe_ps;
    sc_out<bool> pe_ps_valid;
    sc_in<bool> pe_ps_ready;

    // port dynamic
    sc_out<uint16_t> pe_pd;
    sc_out<bool> pe_pd_valid;
    sc_in<bool> pe_pd_ready;

    // port local input
    sc_out<uint64_t> pe_pli;
    sc_out<bool> pe_pli_valid;
    sc_in<bool> pe_pli_ready;

    // port local output
    sc_in<uint64_t> pe_plo;
    sc_in<bool> pe_plo_valid;
    sc_out<bool> pe_plo_ready;

    // methods
    SC_CTOR(PErouter)
        : clk("clk"),
          reset_n("reset_n"),
          enable("enable"),
          route_mode("route_mode"),
          req_in("req_in"),
          req_in_valid("req_in_valid"),
          req_in_ready("req_in_ready"),
          resp_out("resp_out"),
          resp_out_valid("resp_out_valid"),
          resp_out_ready("resp_out_ready"),
          ln_pli_in_data("ln_pli_in_data"),
          ln_pli_in_valid("ln_pli_in_valid"),
          ln_pli_in_ready("ln_pli_in_ready"),
          ln_plo_out_data("ln_plo_out_data"),
          ln_plo_out_valid("ln_plo_out_valid"),
          ln_plo_out_ready("ln_plo_out_ready"),
          pe_reset("pe_reset"),
          pe_start("pe_start"),
          pe_program("pe_program"),
          im_write_en("im_write_en"),
          im_write_addr("im_write_addr"),
          im_write_data("im_write_data"),
          pe_ps("pe_ps"),
          pe_ps_valid("pe_ps_valid"),
          pe_ps_ready("pe_ps_ready"),
          pe_pd("pe_pd"),
          pe_pd_valid("pe_pd_valid"),
          pe_pd_ready("pe_pd_ready"),
          pe_pli("pe_pli"),
          pe_pli_valid("pe_pli_valid"),
          pe_pli_ready("pe_pli_ready"),
          pe_plo("pe_plo"),
          pe_plo_valid("pe_plo_valid"),
          pe_plo_ready("pe_plo_ready")
    {
        DEBUG_MSG("[Create] PErouter");
        SC_METHOD(combinational_process);
        sensitive << reset_n << state_reg << req_in_valid << req_in << enable << route_mode
                  << resp_out_ready << ln_pli_in_valid << ln_pli_in_data << ln_plo_out_ready
                  << pe_ps_ready << pe_pd_ready << pe_pli_ready << pe_plo_valid << pe_plo
                  << pending_noc_req_reg << current_noc_req_reg
                  << ps_queue_full_reg << pd_queue_full_reg << pli_queue_full_reg << plo_queue_full_reg;
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

    // Queue status registers
    sc_signal<bool> ps_queue_full_reg;
    sc_signal<bool> ps_queue_full_next;

    sc_signal<bool> pd_queue_full_reg;
    sc_signal<bool> pd_queue_full_next;

    sc_signal<bool> pli_queue_full_reg;
    sc_signal<bool> pli_queue_full_next;

    sc_signal<bool> plo_queue_full_reg;
    sc_signal<bool> plo_queue_full_next;

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
    std::queue<uint64_t> ps_queue;   // PS channel: NoC -> PE (weights)
    std::queue<uint16_t> pd_queue;   // PD channel: NoC -> PE (activations)
    std::queue<uint64_t> pli_queue;  // PLI channel: NoC/LN -> PE (partial inputs)
    std::queue<uint64_t> plo_queue;  // PLO channel: PE -> NoC/LN (partial outputs)

    // ========================= Helper functions =========================
    void clear_router() {
        // Clear all channel queues
        ps_queue = std::queue<uint64_t>();
        pd_queue = std::queue<uint16_t>();
        pli_queue = std::queue<uint64_t>();
        plo_queue = std::queue<uint64_t>();
    }

    void update_queue_status() {
        ps_queue_full_next.write(ps_queue.size() >= max_queue_size);
        pd_queue_full_next.write(pd_queue.size() >= max_queue_size);
        pli_queue_full_next.write(pli_queue.size() >= max_queue_size);
        plo_queue_full_next.write(plo_queue.size() >= max_queue_size);
    }

    bool can_accept_ps() { return ps_queue.size() < max_queue_size; }
    bool can_accept_pd() { return pd_queue.size() < max_queue_size; }
    bool can_accept_pli() { return pli_queue.size() < max_queue_size; }
    bool can_accept_plo() { return plo_queue.size() < max_queue_size; }

    bool has_ps_data() { return !ps_queue.empty(); }
    bool has_pd_data() { return !pd_queue.empty(); }
    bool has_pli_data() { return !pli_queue.empty(); }
    bool has_plo_data() { return !plo_queue.empty(); }

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
        req_in_ready.write(false);
        resp_out_valid.write(false);
        resp_out.write(noc_response_t());

        ln_pli_in_ready.write(false);
        ln_plo_out_valid.write(false);
        ln_plo_out_data.write(0);

        pe_ps_valid.write(false);
        pe_ps.write(0);
        pe_pd_valid.write(false);
        pe_pd.write(0);
        pe_pli_valid.write(false);
        pe_pli.write(0);
        pe_plo_ready.write(false);

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
                if (req_in_valid.read()) {
                    noc_request_t req = req_in.read();

                    // Command requests - always accepted
                    if (req.addr == PE_CMD_ADDRESS && req.is_w) {
                        req_in_ready.write(true);

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
                                break;
                            case NOC_CHANNEL_PD:
                                can_accept = can_accept_pd();
                                break;
                            case NOC_CHANNEL_PLI:
                                can_accept = can_route_to_bus(channel) && can_accept_pli();
                                break;
                            default:
                                break;
                        }

                        if (can_accept) {
                            req_in_ready.write(true);
                            state_next.write(PErouterState::IDLE);
                        } else {
                            // Queue full, stay in IDLE and reject
                            req_in_ready.write(false);
                            state_next.write(PErouterState::IDLE);
                        }
                    }
                    // Read requests - accept request, prepare for next cycle response
                    else if (enabled && !req.is_w) {
                        NOC_CHANNELS channel = get_noc_channel(req.addr);
                        if (channel == NOC_CHANNEL_PLO) {
                            // Accept request if data is available
                            if (has_plo_data()) {
                                req_in_ready.write(true);
                                current_noc_req_next.write(req);
                                pending_noc_req_next.write(true);
                                state_next.write(PErouterState::WAIT_RESP);
                            } else {
                                // Data not ready, reject request
                                req_in_ready.write(false);
                                state_next.write(PErouterState::IDLE);
                            }
                        }
                    }
                }
                // Handle LN PLI input
                else if (enabled && ln_pli_in_valid.read() && can_route_from_ln(NOC_CHANNEL_PLI) && can_accept_pli()) {
                    ln_pli_in_ready.write(true);
                    state_next.write(PErouterState::IDLE);
                }

                // Data output to PE - provide data if available and PE ready
                if (enabled) {
                    if (has_ps_data() && pe_ps_ready.read()) {
                        pe_ps_valid.write(true);
                        pe_ps.write(ps_queue.front());
                    }
                    if (has_pd_data() && pe_pd_ready.read()) {
                        pe_pd_valid.write(true);
                        pe_pd.write(pd_queue.front());
                    }
                    if (has_pli_data() && pe_pli_ready.read()) {
                        pe_pli_valid.write(true);
                        pe_pli.write(pli_queue.front());
                    }

                    // Accept PLO from PE
                    if (pe_plo_valid.read() && can_accept_plo()) {
                        pe_plo_ready.write(true);
                    }

                    // Output PLO data
                    if (has_plo_data()) {
                        bool plo_to_ln = (mode == PERouterMode::PLI_FROM_LN_PLO_TO_LN) ||
                                         (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_LN);
                        bool plo_to_noc = (mode == PERouterMode::PLI_FROM_LN_PLO_TO_BUS) ||
                                          (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_BUS);

                        if (plo_to_ln && ln_plo_out_ready.read()) {
                            ln_plo_out_valid.write(true);
                            ln_plo_out_data.write(plo_queue.front());
                        } else if (plo_to_noc && resp_out_ready.read()) {
                            resp_out_valid.write(true);
                            noc_response_t resp;
                            resp.data = plo_queue.front();
                            resp_out.write(resp);
                        }
                    }
                }
                break;
            }

            case PErouterState::WAIT_RESP:
            {
                if (pending_noc_req_reg.read()) {
                    // Send response
                    resp_out_valid.write(true);
                    if (has_plo_data()) {
                        noc_response_t resp;
                        resp.data = plo_queue.front();
                        resp_out.write(resp);
                    }

                    // Check if acknowledged - state transition only, pop in sequential
                    if (resp_out_ready.read()) {
                        pending_noc_req_next.write(false);
                        state_next.write(PErouterState::IDLE);
                    } else {
                        state_next.write(PErouterState::WAIT_RESP);
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

        // Update queue status flags
        update_queue_status();
    }

    // ========================= Sequential Logic =========================
    void sequential_process() {
        // Reset
        clear_router();
        state_reg.write(PErouterState::IDLE);
        pending_noc_req_reg.write(false);
        current_noc_req_reg.write(noc_request_t());

        ps_queue_full_reg.write(false);
        pd_queue_full_reg.write(false);
        pli_queue_full_reg.write(false);
        plo_queue_full_reg.write(false);

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

            ps_queue_full_reg.write(ps_queue_full_next.read());
            pd_queue_full_reg.write(pd_queue_full_next.read());
            pli_queue_full_reg.write(pli_queue_full_next.read());
            plo_queue_full_reg.write(plo_queue_full_next.read());

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

            // ========== Queue operations (based on handshakes) ==========
            // Handle command reset
            if (pe_reset_reg.read()) {
                clear_router();
            }

            // Enqueue operations (write to queues)
            if (req_in_valid.read() && req_in_ready.read()) {
                noc_request_t req = req_in.read();
                if (req.is_w && req.addr != PE_CMD_ADDRESS && enable.read()) {
                    NOC_CHANNELS channel = get_noc_channel(req.addr);
                    DEBUG_MSG("[PErouter] Enqueueing data to channel " << channel << ": 0x" << std::hex << req.data << std::dec);
                    switch (channel) {
                        case NOC_CHANNEL_PS:
                            if (can_accept_ps()) ps_queue.push(req.data);
                            break;
                        case NOC_CHANNEL_PD:
                            if (can_accept_pd()) pd_queue.push(static_cast<uint16_t>(req.data));
                            break;
                        case NOC_CHANNEL_PLI:
                            if (can_route_to_bus(channel) && can_accept_pli()) pli_queue.push(req.data);
                            break;
                        default:
                            break;
                    }
                }
            }

            // LN PLI input
            if (ln_pli_in_valid.read() && ln_pli_in_ready.read()) {
                if (can_accept_pli()) pli_queue.push(ln_pli_in_data.read());
            }

            // Dequeue operations (pop from queues when data is consumed)
            if (pe_ps_valid.read() && pe_ps_ready.read() && has_ps_data()) {
                ps_queue.pop();
            }
            if (pe_pd_valid.read() && pe_pd_ready.read() && has_pd_data()) {
                pd_queue.pop();
            }
            if (pe_pli_valid.read() && pe_pli_ready.read() && has_pli_data()) {
                pli_queue.pop();
            }

            // Enqueue PLO from PE
            if (pe_plo_valid.read() && pe_plo_ready.read() && can_accept_plo()) {
                plo_queue.push(pe_plo.read());
            }

            // Dequeue PLO to LN
            if (ln_plo_out_valid.read() && ln_plo_out_ready.read() && has_plo_data()) {
                plo_queue.pop();
            }

            // Dequeue PLO for NoC read (in WAIT_RESP state)
            if (state_reg.read() == PErouterState::WAIT_RESP &&
                resp_out_valid.read() && resp_out_ready.read() && has_plo_data()) {
                plo_queue.pop();
            }

            DEBUG_MSG("[PErouter] State: " << state_reg.read());
            wait();
        }
    }
};

} // namespace pe
} // namespace hybridacc
