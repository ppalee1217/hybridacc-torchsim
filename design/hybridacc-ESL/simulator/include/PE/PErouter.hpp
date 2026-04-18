#pragma once

#include "Utils/utils.hpp"
#include <systemc>
#include <cassert>
#include "Utils/FIFO.hpp"
#include "Utils/async_FIFO.hpp"

using namespace sc_core;

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

    // NoC-PS input ports (Control & Push)
    VRDIF<noc_request_t> noc_ps_req_in_if;

    // NoC-PD input ports (Push)
    VRDIF<noc_request_t> noc_pd_req_in_if;

    // NoC-PLI input ports (PLI Write)
    VRDIF<noc_request_t> noc_pli_req_in_if;

    // NoC-PLO input ports (PLO Read)
    VRDIF<noc_addr_req_t> noc_plo_req_in_if;

    // NoC-PLO output ports (PLO Read Response)
    VRDOF<noc_response_t> noc_plo_resp_out_if;

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
    // port dynamic (set pop) - using VRDOF
    VRDOF<uint64_t> pe_pd_set_out_if;

    // port local input - using VRDOF
    VRDOF<uint64_t> pe_pli_out_if;

    // port local output - using VRDIF
    VRDIF<uint64_t> pe_plo_in_if;

    // methods
    SC_HAS_PROCESS(PErouter);
    PErouter(sc_module_name name, size_t pe_fifo_depth = 4)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          enable("enable"),
          route_mode("route_mode"),
          noc_ps_req_in_if("noc_ps_req_in_if"),
          noc_pd_req_in_if("noc_pd_req_in_if"),
          noc_pli_req_in_if("noc_pli_req_in_if"),
          noc_plo_req_in_if("noc_plo_req_in_if"),
          noc_plo_resp_out_if("noc_plo_resp_out_if"),
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
          pe_pd_set_out_if("pe_pd_set_out_if"),
          pe_pli_out_if("pe_pli_out_if"),
          pe_plo_in_if("pe_plo_in_if"),
          ps_fifo("ps_fifo", pe_fifo_depth),
          pd_fifo("pd_fifo", pe_fifo_depth),
          pli_fifo("pli_fifo", pe_fifo_depth),
          plo_fifo("plo_fifo", pe_fifo_depth),
          pe_fifo_depth(pe_fifo_depth)
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
        ps_fifo.clear(fifo_flush);

        pd_fifo.clk(clk);
        pd_fifo.reset_n(reset_n);
        pd_fifo.data_in(pd_fifo_data_in_sig);
        pd_fifo.mask_in(pd_fifo_mask_in_sig);
        pd_fifo.push(pd_fifo_push_sig);
        pd_fifo.data_out(pd_fifo_data_out_sig);
        pd_fifo.pop(pd_fifo_pop_sig);
        pd_fifo.data_out_set(pd_fifo_data_out_set_sig);
        pd_fifo.pop_set(pd_fifo_pop_set_sig);
        pd_fifo.set_valid(pd_fifo_set_valid_sig);
        pd_fifo.empty(pd_fifo_empty_sig);
        pd_fifo.full(pd_fifo_full_sig);
        pd_fifo.clear(fifo_flush);

        pli_fifo.clk(clk);
        pli_fifo.reset_n(reset_n);
        pli_fifo.data_in(pli_fifo_data_in_sig);
        pli_fifo.push(pli_fifo_push_sig);
        pli_fifo.data_out(pli_fifo_data_out_sig);
        pli_fifo.pop(pli_fifo_pop_sig);
        pli_fifo.empty(pli_fifo_empty_sig);
        pli_fifo.clear(fifo_flush);
        pli_fifo.full(pli_fifo_full_sig);

        plo_fifo.clk(clk);
        plo_fifo.reset_n(reset_n);
        plo_fifo.data_in(plo_fifo_data_in_sig);
        plo_fifo.push(plo_fifo_push_sig);
        plo_fifo.data_out(plo_fifo_data_out_sig);
        plo_fifo.pop(plo_fifo_pop_sig);
        plo_fifo.clear(plo_fifo_clear_sig);
        plo_fifo.empty(plo_fifo_empty_sig);
        plo_fifo.full(plo_fifo_full_sig);

        // -----------------------------------------------------------------
        // Combinational processes (split by responsibility / sensitivity)
        // -----------------------------------------------------------------
        SC_METHOD(comb_noc_ps);
        sensitive << reset_n << enable
              << noc_ps_req_in_if.valid_in << noc_ps_req_in_if.data_in
              << ps_fifo_full_sig << ps_fifo_pop_sig << router_running_reg << stop_pending_reg;

        SC_METHOD(comb_noc_pd);
        sensitive << reset_n << enable
              << noc_pd_req_in_if.valid_in << noc_pd_req_in_if.data_in
              << pd_fifo_full_sig << pd_fifo_pop_sig << router_running_reg << stop_pending_reg;

        SC_METHOD(comb_noc_pli);
        sensitive << reset_n << enable << route_mode
                  << noc_pli_req_in_if.valid_in << noc_pli_req_in_if.data_in
                  << ln_pli_in_if.valid_in << ln_pli_in_if.data_in
              << pli_fifo_full_sig << pli_fifo_pop_sig << router_running_reg << stop_pending_reg;

        SC_METHOD(comb_noc_plo_req);
        sensitive << reset_n << enable << route_mode << state_reg
                  << noc_plo_req_in_if.valid_in << noc_plo_req_in_if.data_in
                  << plo_fifo_empty_sig << router_running_reg << stop_pending_reg;

        SC_METHOD(comb_pe_collect);
        sensitive << reset_n << enable
                  << pe_plo_in_if.valid_in << pe_plo_in_if.data_in
                  << plo_fifo_full_sig << router_running_reg;

        SC_METHOD(comb_pe_feed);
        sensitive << reset_n << enable
              << pe_ps_out_if.ready_in << pe_pd_out_if.ready_in << pe_pd_set_out_if.ready_in << pe_pli_out_if.ready_in
              << ps_fifo_empty_sig << ps_fifo_data_out_sig
              << pd_fifo_empty_sig << pd_fifo_data_out_sig << pd_fifo_data_out_set_sig << pd_fifo_set_valid_sig
              << pli_fifo_empty_sig << pli_fifo_data_out_sig << router_running_reg;

        SC_METHOD(comb_output_and_state);
        sensitive << reset_n << enable << route_mode
                  << state_reg << pending_noc_resp_reg << noc_plo_resp_reg
                  << noc_plo_req_in_if.valid_in
                  << plo_fifo_empty_sig << plo_fifo_data_out_sig
                  << noc_plo_resp_out_if.ready_in << ln_plo_out_if.ready_in << router_running_reg;

        // -----------------------------------------------------------------
        // Single sequential process (all regs/pulses updated here)
        // -----------------------------------------------------------------
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

    bool has_pending_noc_response() const {
        return pending_noc_resp_reg.read() || state_reg.read() == PErouterState::WAIT_RESP;
    }

    bool has_pending_quiesce() const {
        return stop_pending_reg.read() || has_pending_noc_response();
    }

    bool any_fifo_nonempty() const {
        return !ps_fifo_empty_sig.read()
            || !pd_fifo_empty_sig.read()
            || !pli_fifo_empty_sig.read()
            || !plo_fifo_empty_sig.read();
    }

    bool is_quiesced() const {
        return !router_running_reg.read()
            && !stop_pending_reg.read()
            && !has_pending_noc_response()
            && !any_fifo_nonempty();
    }

    // ========================= Register definitions =========================

    // State registers
    sc_signal<PErouterState> state_reg;
    sc_signal<PErouterState> state_next;

    // Pending request registers
    sc_signal<bool> pending_noc_resp_reg;
    sc_signal<bool> pending_noc_resp_next;

    sc_signal<noc_response_t> noc_plo_resp_reg;
    sc_signal<noc_response_t> noc_plo_resp_next;

    // Control signal registers
    sc_signal<bool> im_write_en_reg;

    sc_signal<uint16_t> im_write_addr_reg;

    sc_signal<pe_inst_t> im_write_data_reg;

    sc_signal<bool> pe_reset_reg;

    sc_signal<bool> pe_start_reg;

    sc_signal<bool> pe_program_reg;

    sc_signal<bool> stop_pending_reg;

    // One-cycle command pulses (from NoC-PS command messages)
    sc_signal<bool> cmd_reset_pulse;
    sc_signal<bool> cmd_start_pulse;
    sc_signal<bool> cmd_stop_pulse;
    sc_signal<bool> cmd_program_pulse;
    sc_signal<bool> cmd_im_write_pulse;
    sc_signal<uint16_t> cmd_im_addr;
    sc_signal<pe_inst_t> cmd_im_data;

    sc_signal<bool> router_running_reg; // signal to indicate PE router is running

    // Channel-specific data queues (these are not registers in HDL sense)
    size_t pe_fifo_depth = 4;

    sc_signal<bool> fifo_flush;          // clears PS/PD/PLI (ingress)
    sc_signal<bool> plo_fifo_clear_sig;   // clears PLO only (on layer start)

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
    sc_signal<uint64_t> pd_fifo_data_out_set_sig;
    sc_signal<bool> pd_fifo_pop_set_sig;
    sc_signal<bool> pd_fifo_set_valid_sig;
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
    // NoC-PS: command + PS data ingress (direct to FIFO, no extra buffering)
    void comb_noc_ps() {
        noc_ps_req_in_if.ready_out.write(false);

        ps_fifo_push_sig.write(false);
        ps_fifo_data_in_sig.write(0);

        cmd_reset_pulse.write(false);
        cmd_start_pulse.write(false);
        cmd_stop_pulse.write(false);
        cmd_program_pulse.write(false);
        cmd_im_write_pulse.write(false);
        cmd_im_addr.write(0);
        cmd_im_data.write(0);

        if (!reset_n.read()) return;

        const noc_request_t req = noc_ps_req_in_if.data_in.read();
        const bool is_cmd = (req.addr == PE_CMD_ADDRESS);

        // Commands are accepted regardless of FIFO availability.
        // Data is accepted when enabled and PS FIFO is not full.

        // During stop_pending, non-command data is accepted to drain the
        // upstream pipeline but silently discarded (not pushed to FIFO).
        const bool draining = stop_pending_reg.read();
        bool ready;
        if(is_cmd){
            ready = enable.read();
        } else {
            ready = enable.read() && router_running_reg.read()
                 && (draining
                     || (!ps_fifo_full_sig.read() || ps_fifo_pop_sig.read()));
        }

        noc_ps_req_in_if.ready_out.write(ready);

        const bool fire = noc_ps_req_in_if.valid_in.read() && ready;

        if (!fire) return;

        if (is_cmd) {
            DEBUG_MSG("[PErouter] Received command: 0x" << std::hex << req.data << std::dec, DEBUG_LEVEL_PE_TOP);
            const message_command_t cmd = static_cast<message_command_t>(req.data & 0xF);
            switch (cmd) {
                case message_command_t::CMD_RESET:
                    cmd_reset_pulse.write(true);
                    break;
                case message_command_t::CMD_LOAD_PROGRAM: {
                    const uint16_t im_addr = static_cast<uint16_t>((req.data >> PE_ROUTER_IM_ADDR_OFFSET) & PE_ROUTER_IM_ADDR_MASK);
                    const pe_inst_t im_data = static_cast<pe_inst_t>((req.data >> PE_ROUTER_IM_DATA_OFFSET) & PE_ROUTER_IM_DATA_MASK);
                    cmd_im_addr.write(im_addr);
                    cmd_im_data.write(im_data);
                    cmd_im_write_pulse.write(true);
                    cmd_program_pulse.write(true);
                    break;
                }
                case message_command_t::CMD_START_PE:
                    cmd_start_pulse.write(true);
                    break;
                case message_command_t::CMD_STOP_PE:
                    cmd_stop_pulse.write(true);
                    break;
                default:
                    break;
            }
        } else if (!draining) {
            // Data path: push directly into FIFO.
            ps_fifo_data_in_sig.write(req.data);
            ps_fifo_push_sig.write(true);
        }
        // During stop: data fire is accepted but silently discarded.
    }

    // NoC-PD: PD data ingress (direct to FIFO, no extra buffering)
    void comb_noc_pd() {
        noc_pd_req_in_if.ready_out.write(false);
        pd_fifo_push_sig.write(false);
        pd_fifo_data_in_sig.write(0);
        pd_fifo_mask_in_sig.write(0);

        if (!reset_n.read()) return;

        // During stop_pending: accept to drain upstream, but discard.
        const bool draining = stop_pending_reg.read();
        const bool ready = enable.read() && router_running_reg.read()
            && (draining || !pd_fifo_full_sig.read());
        noc_pd_req_in_if.ready_out.write(ready);

        const bool fire = noc_pd_req_in_if.valid_in.read() && ready;
        if (!fire) return;

        if (!draining) {
            const noc_request_t req = noc_pd_req_in_if.data_in.read();
            pd_fifo_data_in_sig.write(req.data);
            pd_fifo_mask_in_sig.write(req.mask);
            pd_fifo_push_sig.write(true);
        }
    }

    // NoC-PLI + LN-PLI: PLI ingress with arbitration (direct to FIFO)
    void comb_noc_pli() {
        noc_pli_req_in_if.ready_out.write(false);
        ln_pli_in_if.ready_out.write(false);

        pli_fifo_push_sig.write(false);
        pli_fifo_data_in_sig.write(0);

        if (!reset_n.read()) return;

        // During stop_pending: accept ingress to drain upstream, but discard.
        const bool draining = stop_pending_reg.read();
        const bool enabled = enable.read() && router_running_reg.read();
        const bool fifo_has_space = draining || (!pli_fifo_full_sig.read() || pli_fifo_pop_sig.read());
        const bool allow_bus = enabled && (draining || can_route_to_bus(NOC_CHANNEL_PLI)) && fifo_has_space;
        const bool allow_ln = enabled && (draining || can_route_from_ln(NOC_CHANNEL_PLI)) && fifo_has_space;

        // Priority: NoC over LN
        const bool noc_ready = allow_bus;
        const bool noc_fire = noc_pli_req_in_if.valid_in.read() && noc_ready;
        noc_pli_req_in_if.ready_out.write(noc_ready);

        const bool ln_ready = allow_ln && !noc_pli_req_in_if.valid_in.read();
        const bool ln_fire = ln_pli_in_if.valid_in.read() && ln_ready;
        ln_pli_in_if.ready_out.write(ln_ready);

        if (!draining) {
            if (noc_fire) {
                const noc_request_t req = noc_pli_req_in_if.data_in.read();
                pli_fifo_data_in_sig.write(req.data);
                pli_fifo_push_sig.write(true);
            } else if (ln_fire) {
                pli_fifo_data_in_sig.write(ln_pli_in_if.data_in.read());
                pli_fifo_push_sig.write(true);
            }
        }
    }

    // NoC-PLO: read request acceptance
    void comb_noc_plo_req() {
        noc_plo_req_in_if.ready_out.write(false);

        if (!reset_n.read()) return;
        if (!enable.read()) return;
        if (!router_running_reg.read()) return;
        // Allow PLO egress during stop_pending so output data can drain
        // to NoC before the router declares itself drained.
        bool stage_valid = (state_reg.read() == PErouterState::IDLE);
        if (!stage_valid) return;
        if (!can_route_to_bus(NOC_CHANNEL_PLO)) return;
        if (plo_fifo_empty_sig.read()) return;

        noc_plo_req_in_if.ready_out.write(true);
    }

    // PE->PLO collect: direct ingress into FIFO
    void comb_pe_collect() {
        pe_plo_in_if.ready_out.write(false);
        plo_fifo_push_sig.write(false);
        plo_fifo_data_in_sig.write(0);

        if (!reset_n.read()) return;
        if (!enable.read()) return;
        if (!router_running_reg.read()) return;

        const bool ready = !plo_fifo_full_sig.read();
        pe_plo_in_if.ready_out.write(ready);

        if (pe_plo_in_if.valid_in.read() && ready) {
            plo_fifo_data_in_sig.write(pe_plo_in_if.data_in.read());
            plo_fifo_push_sig.write(true);
        }
    }

    // FIFO -> PE feed (combinational, handshake-driven)
    void comb_pe_feed() {
        ps_fifo_pop_sig.write(false);
        pd_fifo_pop_sig.write(false);
        pd_fifo_pop_set_sig.write(false);
        pli_fifo_pop_sig.write(false);

        pe_ps_out_if.valid_out.write(false);
        pe_ps_out_if.data_out.write(0);
        pe_pd_out_if.valid_out.write(false);
        pe_pd_out_if.data_out.write(0);
        pe_pd_set_out_if.valid_out.write(false);
        pe_pd_set_out_if.data_out.write(0);
        pe_pli_out_if.valid_out.write(false);
        pe_pli_out_if.data_out.write(0);

        if (!reset_n.read()) return;
        if (!enable.read()) return;
        if (!router_running_reg.read()) return;

        // PS
        if (!ps_fifo_empty_sig.read()) {
            pe_ps_out_if.valid_out.write(true);
            pe_ps_out_if.data_out.write(ps_fifo_data_out_sig.read());
            if (pe_ps_out_if.ready_in.read()) {
                ps_fifo_pop_sig.write(true);
            }
        }

        // PD
        const bool pd_set_valid = pd_fifo_set_valid_sig.read();
        const bool pd_scalar_valid = !pd_fifo_empty_sig.read();

        if (pd_set_valid) {
            pe_pd_set_out_if.valid_out.write(true);
            pe_pd_set_out_if.data_out.write(pd_fifo_data_out_set_sig.read());
        }

        if (pd_scalar_valid) {
            pe_pd_out_if.valid_out.write(true);
            pe_pd_out_if.data_out.write(pd_fifo_data_out_sig.read());
        }

        const bool pd_pop_set = pd_set_valid && pe_pd_set_out_if.ready_in.read();
        const bool pd_pop_scalar = pd_scalar_valid && pe_pd_out_if.ready_in.read() && !pd_pop_set;

        if (pd_pop_set) {
            pd_fifo_pop_set_sig.write(true);
        } else if (pd_pop_scalar) {
            pd_fifo_pop_sig.write(true);
        }

        // PLI
        if (!pli_fifo_empty_sig.read()) {
            pe_pli_out_if.valid_out.write(true);
            pe_pli_out_if.data_out.write(pli_fifo_data_out_sig.read());
            if (pe_pli_out_if.ready_in.read()) {
                pli_fifo_pop_sig.write(true);
            }
        }
    }

    // Output + state machine for PLO response (single writer for these outputs)
    void comb_output_and_state() {
        // Defaults
        plo_fifo_pop_sig.write(false);

        noc_plo_resp_next.write(noc_plo_resp_reg.read());
        pending_noc_resp_next.write(pending_noc_resp_reg.read());
        state_next.write(state_reg.read());

        noc_plo_resp_out_if.valid_out.write(false);
        noc_plo_resp_out_if.data_out.write(noc_response_t());
        ln_plo_out_if.valid_out.write(false);
        ln_plo_out_if.data_out.write(0);

        if (!reset_n.read()) {
            pending_noc_resp_next.write(false);
            state_next.write(PErouterState::IDLE);
            return;
        }
        if (!enable.read() || !router_running_reg.read()) {
            state_next.write(PErouterState::IDLE);
            return;
        }

        const bool has_plo = !plo_fifo_empty_sig.read();
        const PERouterMode mode = route_mode.read();
        const bool plo_to_ln = (mode == PERouterMode::PLI_FROM_LN_PLO_TO_LN) ||
                               (mode == PERouterMode::PLI_FROM_BUS_PLO_TO_LN);

        // Track whether we accepted a new NoC PLO request this cycle and the corresponding response payload.
        bool new_noc_resp_valid = false;
        noc_response_t new_noc_resp;
        new_noc_resp = noc_response_t();

        // NoC read request fire condition (computed locally; avoids delta ordering)
        const bool noc_plo_ready = (state_reg.read() == PErouterState::IDLE) &&
                       enable.read() &&
                       can_route_to_bus(NOC_CHANNEL_PLO) &&
                       has_plo;
        const bool noc_plo_fire = noc_plo_req_in_if.valid_in.read() && noc_plo_ready;

        // 1) If a NoC read request is accepted, capture response and pop FIFO.
        if (noc_plo_fire) {
            noc_response_t resp;
            resp.data = plo_fifo_data_out_sig.read();
            resp.status = NOC_RESPONSE_STATUS::NOC_OK;
            noc_plo_resp_next.write(resp);
            pending_noc_resp_next.write(true);
            plo_fifo_pop_sig.write(true);

            new_noc_resp_valid = true;
            new_noc_resp = resp;
        }

        // 2) LN proactive streaming (only when not servicing a NoC read this delta)
        if (!noc_plo_fire && state_reg.read() == PErouterState::IDLE) {
            if (has_plo && plo_to_ln) {
                ln_plo_out_if.valid_out.write(true);
                ln_plo_out_if.data_out.write(plo_fifo_data_out_sig.read());
                if (ln_plo_out_if.ready_in.read()) {
                    plo_fifo_pop_sig.write(true);
                }
            }
        }

        // 3) NoC response channel FSM
        // IMPORTANT: Never assert valid based on pending_noc_resp_next while driving data from noc_plo_resp_reg;
        // that creates a spurious extra response in the same cycle a new request is accepted.
        const bool have_reg_resp = pending_noc_resp_reg.read();

        if (have_reg_resp) {
            noc_plo_resp_out_if.valid_out.write(true);
            noc_plo_resp_out_if.data_out.write(noc_plo_resp_reg.read());

            if (noc_plo_resp_out_if.ready_in.read()) {
                // Registered response consumed this cycle.
                // If we also accepted a new read this cycle, keep pending for the new response.
                if (!noc_plo_fire) {
                    pending_noc_resp_next.write(false);
                }
                state_next.write(PErouterState::IDLE);
            } else {
                state_next.write(PErouterState::WAIT_RESP);
            }
        } else {
            // No registered response to send this cycle. New responses are stored and sent next cycle.
            if (state_reg.read() == PErouterState::WAIT_RESP) {
                state_next.write(PErouterState::IDLE);
            }
        }
    }

    // ========================= Sequential Logic =========================
    // All registers update here (single clocked process)
    void seq_process() {
        fifo_flush.write(false);
        plo_fifo_clear_sig.write(false);
        // Reset values
        state_reg.write(PErouterState::IDLE);
        pending_noc_resp_reg.write(false);
        noc_plo_resp_reg.write(noc_response_t());

        im_write_en_reg.write(false);
        im_write_addr_reg.write(0);
        im_write_data_reg.write(0);
        pe_reset_reg.write(false);
        pe_start_reg.write(false);
        pe_program_reg.write(false);
        stop_pending_reg.write(false);
        router_running_reg.write(false);

        wait();

        while (true) {
            // ---- State / response regs ----
            state_reg.write(state_next.read());
            pending_noc_resp_reg.write(pending_noc_resp_next.read());
            noc_plo_resp_reg.write(noc_plo_resp_next.read());


            // ---- Command-derived pulses (registered one-cycle) ----
            pe_reset_reg.write(cmd_reset_pulse.read());
            pe_start_reg.write(cmd_start_pulse.read());
            pe_program_reg.write(cmd_program_pulse.read());
            im_write_en_reg.write(cmd_im_write_pulse.read());
            if (cmd_im_write_pulse.read()) {
                im_write_addr_reg.write(cmd_im_addr.read());
                im_write_data_reg.write(cmd_im_data.read());
            }

            bool next_router_running = router_running_reg.read();
            bool next_stop_pending = stop_pending_reg.read();

            if (cmd_start_pulse.read()) {
                next_router_running = true;
                next_stop_pending = false;
                // Flush ALL FIFOs to clear stale data from previous wave.
                fifo_flush.write(true);
                plo_fifo_clear_sig.write(true);
            } else if (next_stop_pending) {
                // During stop: flush only INGRESS FIFOs (PS/PD/PLI) so
                // PE-unconsumed residual data drains immediately.
                // PLO is NOT flushed — it must drain naturally via NoC
                // egress so output data reaches SPM/DRAM.
                fifo_flush.write(true);
                plo_fifo_clear_sig.write(false);
            } else {
                fifo_flush.write(false);
                plo_fifo_clear_sig.write(false);
            }

            if (cmd_reset_pulse.read()) {
                next_router_running = false;
                next_stop_pending = false;
            }

            if (cmd_stop_pulse.read()) {
                next_stop_pending = true;
            }

            const bool router_drained = ps_fifo_empty_sig.read()
                                     && pd_fifo_empty_sig.read()
                                     && pli_fifo_empty_sig.read()
                                     && plo_fifo_empty_sig.read()
                                     && !pending_noc_resp_reg.read()
                                     && state_reg.read() == PErouterState::IDLE;

            if (next_stop_pending && router_drained) {
                next_router_running = false;
                next_stop_pending = false;
            }

            router_running_reg.write(next_router_running);
            stop_pending_reg.write(next_stop_pending);

            // Output registered control signals
            im_write_en.write(im_write_en_reg.read());
            im_write_addr.write(im_write_addr_reg.read());
            im_write_data.write(im_write_data_reg.read());
            pe_reset.write(pe_reset_reg.read());
            pe_start.write(pe_start_reg.read());
            pe_program.write(pe_program_reg.read());

            wait();
        }
    }
};

} // namespace pe
} // namespace hybridacc
