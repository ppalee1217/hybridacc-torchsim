#pragma once

#include <queue>
#include "utils.hpp"
#include <systemc>
#include <cassert>
#include "FIFO.hpp"
#include "async_FIFO.hpp"

using namespace sc_core;

// PE Router Command and ID field definitions
#define PE_CMD_ADDRESS 0x100

#define PE_CMD_OFFSET 0
#define PE_CMD_BITS 4

// Command - Load Program
#define PE_ROUTER_IM_ADDR_OFFSET 4
#define PE_ROUTER_IM_DATA_OFFSET 16

#define PE_ROUTER_IM_ADDR_MASK 0xFFF // 12 bits
#define PE_ROUTER_IM_DATA_MASK 0xFFFF // 16 bits

namespace hybridacc {
namespace pe {

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

    // NoC-0 input ports (Control & Push)
    VRDIF<noc_request_t> noc0_req_in_if;

    // NoC-1 input ports (PLI Write)
    VRDIF<noc_request_t> noc1_req_in_if;

    // NoC-2 input ports (PLO Read)
    VRDIF<noc_addr_req_t> noc2_req_in_if;

    // NoC-2 output ports (PLO Read Response)
    VRDOF<noc_response_t> noc2_resp_out_if;

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
          noc0_req_in_if("noc0_req_in_if"),
          noc1_req_in_if("noc1_req_in_if"),
          noc2_req_in_if("noc2_req_in_if"),
          noc2_resp_out_if("noc2_resp_out_if"),
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
        DEBUG_MSG("[Create] PErouter", DEBUG_LEVEL_PE_TOP);

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
        pd_fifo.mask_in(pd_fifo_mask_in_sig);
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

        SC_METHOD(noc0_input_handling_process);
        sensitive << reset_n << noc0_req_in_if.valid_in << noc0_req_in_if.data_in << enable
                  << ps_fifo_full_sig << pd_fifo_full_sig
                  << im_write_addr_reg << im_write_data_reg;

        SC_METHOD(noc1_input_handling_process);
        sensitive << reset_n << noc1_req_in_if.valid_in << noc1_req_in_if.data_in << enable << route_mode
                  << ln_pli_in_if.valid_in << ln_pli_in_if.data_in
                  << pli_fifo_full_sig;

        SC_METHOD(noc2_input_handling_process);
        sensitive << reset_n << state_reg << noc2_req_in_if.valid_in << noc2_req_in_if.data_in << enable << route_mode
                  << plo_fifo_empty_sig << noc2_resp_reg;

        SC_METHOD(pe_feed_process);
        sensitive << reset_n << enable
                  << pe_ps_out_if.ready_in << pe_pd_out_if.ready_in << pe_pli_out_if.ready_in
                  << ps_fifo_empty_sig << ps_fifo_data_out_sig
                  << pd_fifo_empty_sig << pd_fifo_data_out_sig
                  << pli_fifo_empty_sig << pli_fifo_data_out_sig;

        SC_METHOD(pe_collect_process);
        sensitive << reset_n << enable
                  << pe_plo_in_if.valid_in << pe_plo_in_if.data_in
                  << plo_fifo_full_sig;

        SC_METHOD(output_and_state_process);
        sensitive << reset_n << state_reg << pending_noc_resp_reg << noc2_resp_reg
                  << plo_fifo_empty_sig << plo_fifo_data_out_sig
                  << noc2_resp_out_if.ready_in << ln_plo_out_if.ready_in
                  << route_mode << enable << internal_noc_read_req_accepted;

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

    // ========================= Register definitions =========================
    // Internal signal for communication between processes
    sc_signal<bool> internal_noc_read_req_accepted;

    // State registers
    sc_signal<PErouterState> state_reg;
    sc_signal<PErouterState> state_next;

    // Pending request registers
    sc_signal<bool> pending_noc_resp_reg;
    sc_signal<bool> pending_noc_resp_next;

    sc_signal<noc_response_t> noc2_resp_reg;
    sc_signal<noc_response_t> noc2_resp_next;

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
    asyncFIFO<uint64_t, uint16_t> pd_fifo;   // PD channel: NoC -> PE (activations)
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
    sc_signal<uint64_t> pd_fifo_data_in_sig;
    sc_signal<size_t> pd_fifo_mask_in_sig;
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
    void noc0_input_handling_process() {
        ps_fifo_push_sig.write(false);
        pd_fifo_push_sig.write(false);
        pd_fifo_mask_in_sig.write(0);

        im_write_en_next.write(false);
        im_write_addr_next.write(im_write_addr_reg.read());
        im_write_data_next.write(im_write_data_reg.read());

        pe_reset_next.write(false);
        pe_start_next.write(false);
        pe_program_next.write(false);

        noc0_req_in_if.ready_out.write(false);

        if (!reset_n.read()) return;

        bool enabled = enable.read();
        noc_request_t req = noc0_req_in_if.data_in.read();
        bool noc_req_ready = false;

        if (req.addr == PE_CMD_ADDRESS) {
            noc_req_ready = true;
        } else if (enabled) {
            NOC_CHANNELS channel = get_noc_channel(req.addr);
            switch (channel) {
                case NOC_CHANNEL_PS:  noc_req_ready = can_accept_ps(); break;
                case NOC_CHANNEL_PD:  noc_req_ready = can_accept_pd(); break;
                default: noc_req_ready = false; break;
            }
        }

        noc0_req_in_if.ready_out.write(noc_req_ready);

        if (noc0_req_in_if.valid_in.read() && noc_req_ready) {
            if (req.addr == PE_CMD_ADDRESS) {
                DEBUG_MSG("[PErouter] Received command: 0x" << std::hex << req.data << std::dec, DEBUG_LEVEL_PE_TOP);
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
            } else {
                NOC_CHANNELS channel = get_noc_channel(req.addr);
                switch (channel) {
                    case NOC_CHANNEL_PS:
                        ps_fifo_data_in_sig.write(req.data);
                        ps_fifo_push_sig.write(true);
                        break;
                    case NOC_CHANNEL_PD:
                        pd_fifo_data_in_sig.write(req.data);
                        pd_fifo_mask_in_sig.write(req.mask);
                        pd_fifo_push_sig.write(true);
                        break;
                    default:
                        break;
                }
            }
        }
    }

    void noc1_input_handling_process() {
        pli_fifo_push_sig.write(false);
        noc1_req_in_if.ready_out.write(false);
        ln_pli_in_if.ready_out.write(false);

        if (!reset_n.read()) return;

        bool enabled = enable.read();

        // PLI Write (NoC-1)
        noc_request_t req = noc1_req_in_if.data_in.read();
        bool noc_req_ready = false;

        NOC_CHANNELS channel = get_noc_channel(req.addr);
        if (enabled && channel == NOC_CHANNEL_PLI) {
             noc_req_ready = can_route_to_bus(channel) && can_accept_pli();
        }

        noc1_req_in_if.ready_out.write(noc_req_ready);

        bool noc_req_active = noc1_req_in_if.valid_in.read() && noc_req_ready;
        bool ln_pli_ready = enabled && can_route_from_ln(NOC_CHANNEL_PLI) && can_accept_pli() && !noc_req_active;
        ln_pli_in_if.ready_out.write(ln_pli_ready);

        if (noc_req_active) {
             DEBUG_MSG("[PErouter] Receiving PLI data from NoC1: 0x" << std::hex << req.data << std::dec, DEBUG_LEVEL_PE_TOP);
             pli_fifo_data_in_sig.write(req.data);
             pli_fifo_push_sig.write(true);
        } else if (ln_pli_ready && ln_pli_in_if.valid_in.read()) {
              DEBUG_MSG("[PErouter] Receiving PLI data from LN: 0x" << std::hex << ln_pli_in_if.data_in.read() << std::dec, DEBUG_LEVEL_PE_TOP);
              pli_fifo_data_in_sig.write(ln_pli_in_if.data_in.read());
              pli_fifo_push_sig.write(true);
        }
    }

    void noc2_input_handling_process() {
        noc2_req_in_if.ready_out.write(false);
        internal_noc_read_req_accepted.write(false);

        if (!reset_n.read()) return;

        PErouterState current_state = state_reg.read();
        bool enabled = enable.read();

        if (current_state == PErouterState::IDLE) {
            noc_addr_req_t req = noc2_req_in_if.data_in.read();
            bool noc_req_ready = false;

            if (enabled) {
                 NOC_CHANNELS channel = get_noc_channel(req.addr);
                 if (channel == NOC_CHANNEL_PLO) {
                      noc_req_ready = can_route_to_bus(channel) && has_plo_data();
                 }
            }

            noc2_req_in_if.ready_out.write(noc_req_ready);

            if (noc2_req_in_if.valid_in.read() && noc_req_ready) {
                DEBUG_MSG(" [PErouter] Receiving PLO read request from NoC2: " << req << std::dec, DEBUG_LEVEL_PE_TOP);
                internal_noc_read_req_accepted.write(true);
           }
        }
    }

    void pe_feed_process() {
        ps_fifo_pop_sig.write(false);
        pd_fifo_pop_sig.write(false);
        pli_fifo_pop_sig.write(false);

        pe_ps_out_if.valid_out.write(false);
        pe_ps_out_if.data_out.write(0);
        pe_pd_out_if.valid_out.write(false);
        pe_pd_out_if.data_out.write(0);
        pe_pli_out_if.valid_out.write(false);
        pe_pli_out_if.data_out.write(0);

        if (!reset_n.read()) return;
        if (!enable.read()) return;

        if (has_ps_data()) {
            pe_ps_out_if.valid_out.write(true);
            pe_ps_out_if.data_out.write(ps_fifo_data_out_sig.read());
            if (pe_ps_out_if.ready_in.read()) {
                ps_fifo_pop_sig.write(true);
            }
        }

        if (has_pd_data()) {
            pe_pd_out_if.valid_out.write(true);
            pe_pd_out_if.data_out.write(pd_fifo_data_out_sig.read());
            if (pe_pd_out_if.ready_in.read()) {
                pd_fifo_pop_sig.write(true);
            }
        }

        if (has_pli_data()) {
            pe_pli_out_if.valid_out.write(true);
            pe_pli_out_if.data_out.write(pli_fifo_data_out_sig.read());
            if (pe_pli_out_if.ready_in.read()) {
                pli_fifo_pop_sig.write(true);
            }
        }
    }

    void pe_collect_process() {
        plo_fifo_push_sig.write(false);
        pe_plo_in_if.ready_out.write(false);

        if (!reset_n.read()) return;
        if (!enable.read()) return;

        if (pe_plo_in_if.valid_in.read() && can_accept_plo()) {
            pe_plo_in_if.ready_out.write(true);
            plo_fifo_data_in_sig.write(pe_plo_in_if.data_in.read());
            plo_fifo_push_sig.write(true);
        }
    }

    void output_and_state_process() {
        plo_fifo_pop_sig.write(false);
        noc2_resp_next.write(noc2_resp_reg.read());
        state_next.write(state_reg.read());
        noc2_resp_out_if.valid_out.write(false);
        noc2_resp_out_if.data_out.write(noc_response_t());
        ln_plo_out_if.valid_out.write(false);
        ln_plo_out_if.data_out.write(0);

        if (!reset_n.read()) { // reset logic
            pending_noc_resp_next.write(false);
            return;
        }

        // Logic for pending_noc_resp_next
        bool next_pending = pending_noc_resp_reg.read();
        PErouterState current_state = state_reg.read();
        PERouterMode mode = route_mode.read();
        bool enabled = enable.read();

        if (internal_noc_read_req_accepted.read()) {
            // Send PLO data as response
            noc_response_t resp;
            resp.data = plo_fifo_data_out_sig.read();
            resp.status = NOC_RESPONSE_STATUS::NOC_OK;
            noc2_resp_next.write(resp);

            // Pop PLO FIFO
            plo_fifo_pop_sig.write(true);

            next_pending = true;
        }

        if (current_state == PErouterState::IDLE) {
            if (enabled && has_plo_data()) { // 主動 (non-pending) 發送 PLO 資料到 LN
                bool plo_to_ln = (mode == PERouterMode::PLI_FROM_LN_PLO_TO_LN) ||
                                 (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_LN);

                DEBUG_MSG("[PErouter] has_plo_data, mode: " << mode << ", plo_to_ln: " << plo_to_ln << ", data: 0x" << std::hex << plo_fifo_data_out_sig.read() << std::dec, DEBUG_LEVEL_PE_TOP);

                if (plo_to_ln) {
                    ln_plo_out_if.valid_out.write(true);
                    ln_plo_out_if.data_out.write(plo_fifo_data_out_sig.read());

                    if (ln_plo_out_if.ready_in.read()) {
                        DEBUG_MSG("[PErouter] Sending PLO data to LN: 0x" <<  std::hex << plo_fifo_data_out_sig.read() << std::dec, DEBUG_LEVEL_PE_TOP);
                        plo_fifo_pop_sig.write(true);
                    }
                }
            }

            if (enabled && pending_noc_resp_reg.read()) {
                noc2_resp_out_if.valid_out.write(true);
                noc2_resp_out_if.data_out.write(noc2_resp_reg.read());

                if (noc2_resp_out_if.ready_in.read()) {
                    DEBUG_MSG("[PErouter] IDLE: Sending PLO data to NoC: 0x" <<  std::hex << plo_fifo_data_out_sig.read()  << std::dec, DEBUG_LEVEL_PE_TOP);

                    if (!internal_noc_read_req_accepted.read()) {
                        next_pending = false;
                    }

                    state_next.write(PErouterState::IDLE);
                } else {
                    state_next.write(PErouterState::WAIT_RESP);
                }
            }
        }
        else if (current_state == PErouterState::WAIT_RESP) {
            if (pending_noc_resp_reg.read()) {
                if (has_plo_data()) {
                    noc2_resp_out_if.valid_out.write(true);
                    noc2_resp_out_if.data_out.write(noc2_resp_reg.read());

                    if (noc2_resp_out_if.ready_in.read()) {
                        DEBUG_MSG("[PErouter] WAIT_RESP: Sending PLO data to NoC: 0x" <<  std::hex << plo_fifo_data_out_sig.read() << std::dec, DEBUG_LEVEL_PE_TOP);
                        next_pending = false;
                        state_next.write(PErouterState::IDLE);
                    } else {
                        state_next.write(PErouterState::WAIT_RESP);
                    }
                }
            } else {
                state_next.write(PErouterState::IDLE);
            }
        }
        else {
            state_next.write(PErouterState::IDLE);
        }

        pending_noc_resp_next.write(next_pending);
    }

    // ========================= Sequential Logic =========================
    void sequential_process() {
        // Reset
        state_reg.write(PErouterState::IDLE);
        pending_noc_resp_reg.write(false);
        noc2_resp_reg.write(noc_response_t());

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
            pending_noc_resp_reg.write(pending_noc_resp_next.read());
            noc2_resp_reg.write(noc2_resp_next.read());

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
                      << " plo_pop=" << plo_fifo_pop_sig.read(), DEBUG_LEVEL_PE_COMPONENTS);
            wait();
        }
    }
};

} // namespace pe
} // namespace hybridacc


/*

這份 `PErouter.hpp` 定義了 **PErouter** 模組，它是單個 Process Element (PE) 內部的通訊控制中心。

它的主要職責是管理 PE 核心 (Core) 與外部世界（NoC 系統總線 和 Local Network 鄰居）之間的數據流動與控制信號。

以下是 `PErouter` 的詳細行為說明：

### 1. 角色與定位
`PErouter` 位於 PE 的最前端，扮演 "守門員" 與 "分發者" 的角色：
*   **對上 (NoC)**: 接收來自 `MBUS` 的指令與數據，並回傳運算結果。
*   **對旁 (Local Network, LN)**: 處理與相鄰 PE 的直接數據傳輸 (Systolic Array 行為)。
*   **對內 (PE Core)**: 將數據緩衝後餵給運算單元，並接收運算單元的輸出。

### 2. 內部架構：四通道 FIFO
為了隔離外部傳輸速度與內部運算速度的差異，`PErouter` 內部維護了四個獨立的 FIFO (佇列)，對應四種數據通道：
1.  **PS (Partial Sum / Weight)**: 權重或部分和通道 (NoC -> PE)。
2.  **PD (Pixel Data)**: 輸入激活值通道 (NoC -> PE)。
3.  **PLI (Partial Loop Input)**: 脈動陣列輸入通道 (NoC/LN -> PE)。
4.  **PLO (Partial Loop Output)**: 脈動陣列輸出通道 (PE -> NoC/LN)。

### 3. 狀態機行為 (State Machine)
`PErouter` 使用一個簡單的狀態機來管理 NoC 的請求：

*   **IDLE (閒置/處理)**:
    *   這是預設狀態。
    *   **接收寫入 (Write)**: 如果 NoC 送來寫入請求 (`is_w`)，Router 會根據地址將數據推入對應的 FIFO (PS, PD, PLI)。
    *   **接收命令 (Command)**: 如果地址是 `0x100`，則解析並執行控制命令 (Reset, Start, Load Program)。
    *   **接收讀取 (Read)**: 如果 NoC 送來讀取請求 (`!is_w`)，Router 會鎖定請求並跳轉到 `WAIT_RESP` 狀態。
    *   **數據轉發**: 同時持續將 FIFO 內的數據餵給 PE Core，或將 PE Core 的輸出存入 PLO FIFO。

*   **WAIT_RESP (等待回應)**:
    *   專門處理 **NoC 讀取請求**。
    *   它會等待 `PLO FIFO` 中有數據。
    *   一旦有數據，就將其取出並封裝成 `noc_response_t` 回傳給 NoC。
    *   完成後回到 `IDLE`。

### 4. 詳細數據流路徑

#### A. NoC 寫入路徑 (NoC -> FIFO)
當 NoC 發送寫入請求時，Router 根據地址的 **Channel ID** (Bit 7-6) 決定去向：
*   **Channel 0 (PS)** -> 寫入 `ps_fifo`。
*   **Channel 1 (PD)** -> 寫入 `pd_fifo`。
*   **Channel 2 (PLI)** -> 寫入 `pli_fifo` (需檢查路由模式是否允許)。
*   **流控**: 如果目標 FIFO 滿了 (`full`)，Router 會拉低 `ready_out`，暫停 NoC 的傳輸。

#### B. NoC 讀取路徑 (FIFO -> NoC)
*   NoC 只能讀取 **PLO Channel**。
*   這是一個 **Polling (輪詢)** 機制：PE 不會主動把數據推給 NoC，而是將數據存在 `plo_fifo` 中，等待 NoC 發送讀取請求來 "取貨"。

#### C. Local Network (LN) 路徑
這是為了支援 Systolic Array (脈動陣列) 的數據流動：
*   **PLI 輸入**: 如果路由模式設定為從 LN 接收 (`PLI_FROM_LN...`)，Router 會忽略 NoC 對 PLI 的寫入，轉而從 `ln_pli_in_if` (鄰居) 接收數據放入 `pli_fifo`。
*   **PLO 輸出**: 如果路由模式設定為輸出到 LN (`...PLO_TO_LN`)，`plo_fifo` 的數據會自動被 pop 出來並發送給 `ln_plo_out_if` (鄰居)。

#### D. 控制命令路徑 (Command)
當寫入地址為 `0x100` 時，數據被視為指令：
*   **CMD_RESET**: 觸發 `pe_reset` 信號，重置 PE Core。
*   **CMD_LOAD_PROGRAM**: 觸發 `im_write_en`，將數據寫入 PE 的指令記憶體 (Instruction Memory)。這允許透過 NoC 更新 PE 的程式碼。
*   **CMD_START_PE**: 觸發 `pe_start`，啟動 PE 運算。

### 5. 路由模式 (Route Mode)
`PErouter` 的行為高度依賴 `route_mode` 輸入，這決定了數據流的拓撲結構：
*   **PLI 來源**: 決定 `pli_fifo` 的數據是來自 NoC (Bus) 還是左邊鄰居 (LN)。
*   **PLO 去向**: 決定 `plo_fifo` 的數據是送往 NoC (Bus) 還是右邊鄰居 (LN)。

### 6. 總結
`PErouter` 是一個 **具有緩衝能力的交叉開關 (Buffered Crossbar)**。它將不同來源 (NoC, LN) 的數據流解耦，透過 FIFO 緩衝，確保 PE Core 可以穩定地獲取數據，同時處理複雜的控制信號與 NoC 通訊協定。

*/