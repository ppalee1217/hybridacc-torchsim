#pragma once
#include <cstdint>
#include <vector>
#include <systemc>
#include "utils.hpp"
#include "PE/PErouter.hpp"
#include "PE/IF_ID_stage.hpp"
#include "PE/EXE_M_stage.hpp"
#include "PE/EXE_A_stage.hpp"

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

SC_MODULE(ProcessElement) {

public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Router config port
    sc_in<bool> router_enable;
    sc_in<PERouterMode> router_mode;

    // NoC interface ports - using VRDIF/VRDOF
    VRDIF<noc_request_t> noc_req;
    VRDOF<noc_response_t> noc_resp;

    // Control ports
    sc_out<bool> pe_busy;

    // Local Network ports - using VRDIF/VRDOF
    VRDIF<uint64_t> ln_pli;
    VRDOF<uint64_t> ln_plo;

    // Constructor
    SC_CTOR(ProcessElement)
        : clk("clk"),
          reset_n("reset_n"),
          router_enable("router_enable"),
          router_mode("router_mode"),
          noc_req("noc_req"),
          noc_resp("noc_resp"),
          pe_busy("pe_busy"),
          ln_pli("ln_pli"),
          ln_plo("ln_plo"),
          router("PE_Router"),
          if_id_stage("IF_ID_Stage"),
          exe_m_stage("EXE_M_Stage"),
          exe_a_stage("EXE_A_Stage"),
          router_pe_ps_sig("router_pe_ps_sig"),
          router_pe_pd_sig("router_pe_pd_sig"),
          router_pe_pli_sig("router_pe_pli_sig"),
          router_pe_plo_sig("router_pe_plo_sig")
    {
        DEBUG_MSG("[Create] ProcessElement", DEBUG_LEVEL_PE_TOP);

        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational logic
        SC_METHOD(stall_propagation_process);
        sensitive << exe_a_stall_adder << exe_a_stall_port_io
                  << exe_m_stall_dl << exe_m_stall_ps << exe_m_stall_pd
                  << downstream_stall;

        SC_METHOD(control_logic_process);
        sensitive << pe_running_reg << router_pe_reset << router_pe_start
                  << router_pe_program << if_id_halted_sig
                  << exe_m_halted_sig << exe_a_halted_sig
                  << exe_m_to_exe_a_valid << exe_a_stall_adder
                  << exe_a_stall_port_io << downstream_stall
                  << cycles_reg << instr_count_reg;  // Add these to update counters

        bind();
    }

    // Stage modules (public)
    hybridacc::pe::PErouter router;
    hybridacc::pe::IF_ID_Stage if_id_stage;
    hybridacc::pe::EXE_M_Stage exe_m_stage;
    hybridacc::pe::EXE_A_Stage exe_a_stage;

    // === Sequential Elements (Registers) ===
    sc_signal<bool> pe_running_reg;
    sc_signal<bool> pe_running_next;

    sc_signal<uint64_t> cycles_reg;
    sc_signal<uint64_t> cycles_next;

    sc_signal<uint64_t> instr_count_reg;
    sc_signal<uint64_t> instr_count_next;

    sc_signal<bool> stage_reset_reg;
    sc_signal<bool> stage_reset_next;

    // Pipeline interconnect signals
    sc_signal<pe_decode_signals_t> if_id_to_exe_m_signals;
    sc_signal<bool> if_id_to_exe_m_valid;

    sc_signal<v_fp16_t> exe_m_to_exe_a_vmul;
    sc_signal<pe_decode_signals_t> exe_m_to_exe_a_signals;
    sc_signal<bool> exe_m_to_exe_a_valid;

    // Stage status signals
    sc_signal<uint16_t> if_id_pc_sig;
    sc_signal<bool> if_id_halted_sig;
    sc_signal<bool> exe_m_halted_sig;
    sc_signal<bool> exe_a_halted_sig;

    // Stall signals
    sc_signal<bool> exe_m_stall_dl;
    sc_signal<bool> exe_m_stall_ps;
    sc_signal<bool> exe_m_stall_pd;

    sc_signal<bool> exe_a_stall_adder;
    sc_signal<bool> exe_a_stall_port_io;

    sc_signal<bool> downstream_stall;
    sc_signal<bool> exe_a_stage_stall;
    sc_signal<bool> exe_m_stage_stall;

    // PE control signals
    sc_signal<uint16_t> pc_init_value;
    sc_signal<bool> stage_reset_signal;
    sc_signal<bool> pe_running_signal;

    // Router internal control signals
    sc_signal<bool> router_pe_reset;
    sc_signal<bool> router_pe_start;
    sc_signal<bool> router_pe_program;

    // Router IM (Instruction Memory) signals
    sc_signal<bool> router_im_write_en_sig;
    sc_signal<uint16_t> router_im_write_addr_sig;
    sc_signal<pe_inst_t> router_im_write_data_sig;

    // PE pipeline data signals - Router <-> EXE stages
    VRDSIG<uint64_t> router_pe_ps_sig;
    VRDSIG<uint16_t> router_pe_pd_sig;
    VRDSIG<uint64_t> router_pe_pli_sig;
    VRDSIG<uint64_t> router_pe_plo_sig;

    void bind() {
        // === Router Connections ===
        router.clk(clk);
        router.reset_n(reset_n);

        // Router config ports
        router.enable(router_enable);
        router.route_mode(router_mode);

        // NoC interface - using bind_vr_interface
        bind_vr_interface(router.noc_req_in_if, noc_req);
        bind_vr_interface(noc_resp, router.noc_resp_out_if);

        // Local Network interface - using bind_vr_interface
        bind_vr_interface(router.ln_pli_in_if, ln_pli);
        bind_vr_interface(ln_plo, router.ln_plo_out_if);

        router.pe_reset(router_pe_reset);
        router.pe_start(router_pe_start);
        router.pe_program(router_pe_program);

        // Router IM (Instruction Memory) signals
        router.im_write_en(router_im_write_en_sig);
        router.im_write_addr(router_im_write_addr_sig);
        router.im_write_data(router_im_write_data_sig);

        // === IF_ID Stage Connections ===
        if_id_stage.clk(clk);
        if_id_stage.reset_n(reset_n);

        // PE control signals
        if_id_stage.stage_reset(stage_reset_signal);
        if_id_stage.pe_running(pe_running_signal);
        if_id_stage.pc_init_value(pc_init_value);

        // Pipeline outputs
        if_id_stage.ID_decode_signals_out(if_id_to_exe_m_signals);
        if_id_stage.signal_valid_out(if_id_to_exe_m_valid);

        // Status outputs
        if_id_stage.pc_out(if_id_pc_sig);
        if_id_stage.halted_out(if_id_halted_sig);

        // Instruction Memory programming
        if_id_stage.im_write_en(router_im_write_en_sig);
        if_id_stage.im_write_addr(router_im_write_addr_sig);
        if_id_stage.im_write_data(router_im_write_data_sig);

        // Stall input
        if_id_stage.stall_from_downstream(exe_m_stage_stall);

        // === EXE_M Stage Connections ===
        exe_m_stage.clk(clk);
        exe_m_stage.reset_n(reset_n);

        // PE control signals
        exe_m_stage.stage_reset(stage_reset_signal);
        exe_m_stage.pe_running(pe_running_signal);

        // Pipeline inputs
        exe_m_stage.ID_decode_signals_in(if_id_to_exe_m_signals);
        exe_m_stage.signal_valid_in(if_id_to_exe_m_valid);

        // Pipeline outputs
        exe_m_stage.vmul_out_out(exe_m_to_exe_a_vmul);
        exe_m_stage.EXE_A_decode_signals_out(exe_m_to_exe_a_signals);
        exe_m_stage.signal_valid_out(exe_m_to_exe_a_valid);

        // Status outputs
        exe_m_stage.halted_out(exe_m_halted_sig);

        // Stall outputs
        exe_m_stage.stall_DL(exe_m_stall_dl);
        exe_m_stage.stall_PS(exe_m_stall_ps);
        exe_m_stage.stall_PD(exe_m_stall_pd);

        // Stall input
        exe_m_stage.stall_from_downstream(exe_a_stage_stall);

        // PS port (Port Static) - using connect_vr_signals
        connect_vr_signals(exe_m_stage.ps_data, router_pe_ps_sig);
        connect_vr_signals(router.pe_ps_out_if, router_pe_ps_sig);

        // PD port (Port Dynamic) - using connect_vr_signals
        connect_vr_signals(exe_m_stage.pd_data, router_pe_pd_sig);
        connect_vr_signals(router.pe_pd_out_if, router_pe_pd_sig);

        // === EXE_A Stage Connections ===
        exe_a_stage.clk(clk);
        exe_a_stage.reset_n(reset_n);

        // PE control signals
        exe_a_stage.stage_reset(stage_reset_signal);
        exe_a_stage.pe_running(pe_running_signal);

        // Pipeline inputs
        exe_a_stage.vmul_out_in(exe_m_to_exe_a_vmul);
        exe_a_stage.EXE_M_decode_signals_in(exe_m_to_exe_a_signals);
        exe_a_stage.signal_valid_in(exe_m_to_exe_a_valid);

        // Status outputs
        exe_a_stage.halted_out(exe_a_halted_sig);

        // Stall outputs
        exe_a_stage.stall_adder(exe_a_stall_adder);
        exe_a_stage.stall_port_io(exe_a_stall_port_io);

        // Stall input
        exe_a_stage.stall_from_downstream(downstream_stall);

        // PLI port (Port Local Input) - using connect_vr_signals
        connect_vr_signals(exe_a_stage.pli, router_pe_pli_sig);
        connect_vr_signals(router.pe_pli_out_if, router_pe_pli_sig);

        // PLO port (Port Local Output) - using connect_vr_signals
        connect_vr_signals(router.pe_plo_in_if, router_pe_plo_sig);
        connect_vr_signals(exe_a_stage.plo, router_pe_plo_sig);
    }

    // === Combinational Logic ===

    // Stall propagation calculation
    void stall_propagation_process() {
        // Downstream stall (from external system)
        bool external_stall = false;
        downstream_stall.write(external_stall);

        // EXE_A stage stall (internal + downstream)
        bool exe_a_internal_stall = exe_a_stall_adder.read() || exe_a_stall_port_io.read();
        bool exe_a_total_stall = exe_a_internal_stall || external_stall;
        exe_a_stage_stall.write(exe_a_total_stall);

        // EXE_M stage stall (internal + from EXE_A)
        bool exe_m_internal_stall = exe_m_stall_dl.read() ||
                                    exe_m_stall_ps.read() ||
                                    exe_m_stall_pd.read();
        bool exe_m_total_stall = exe_m_internal_stall || exe_a_total_stall;
        exe_m_stage_stall.write(exe_m_total_stall);

        // Monitor stalls for debugging
        monitor_stalls();
    }

    // Control logic and state calculation
    void control_logic_process() {
        bool pe_running_current = pe_running_reg.read();
        uint64_t cycles_current = cycles_reg.read();
        uint64_t instr_count_current = instr_count_reg.read();
        bool stage_reset_current = stage_reset_reg.read();

        // Default: hold current values
        bool pe_running_n = pe_running_current;
        uint64_t cycles_n = cycles_current;
        uint64_t instr_count_n = instr_count_current;
        bool stage_reset_n = false;  // stage_reset is a pulse signal
        bool pe_busy_n = false;

        // Clear stage_reset after one cycle pulse
        if (stage_reset_current) {
            stage_reset_n = false;
        }

        // Check router control signals
        if (router_pe_reset.read()) {
            // Reset all stages, keep pe_running state
            DEBUG_MSG("[ProcessElement] RESET signal detected", DEBUG_LEVEL_PE_TOP);
            stage_reset_n = true;
        }

        if (router_pe_start.read() && !pe_running_current) {
            // Start PE: reset + set running
            DEBUG_MSG("[ProcessElement] START signal detected", DEBUG_LEVEL_PE_TOP);
            stage_reset_n = true;
            pe_running_n = true;
            cycles_n = 0;
            instr_count_n = 0;
        }

        if (router_pe_program.read() && pe_running_current) {
            // Stop PE
            DEBUG_MSG("[ProcessElement] PROGRAM signal detected", DEBUG_LEVEL_PE_TOP);
            pe_running_n = false;
        }

        // Check if all stages are halted
        bool all_stages_halted = if_id_halted_sig.read() &&
                                 exe_m_halted_sig.read() &&
                                 exe_a_halted_sig.read();

        if (all_stages_halted && pe_running_current) {
            DEBUG_MSG("[ProcessElement] All stages halted, stopping PE", DEBUG_LEVEL_PE_TOP);
            pe_running_n = false;
        }

        // Count instructions
        bool exe_a_has_valid = exe_m_to_exe_a_valid.read();
        bool exe_a_stalled = exe_a_stall_adder.read() || exe_a_stall_port_io.read();
        bool exe_a_downstream_stalled = downstream_stall.read();

        if (exe_a_has_valid && !exe_a_stalled && !exe_a_downstream_stalled && pe_running_current) {
            instr_count_n = instr_count_current + 1;
            DEBUG_MSG("[ProcessElement] Instruction completed: " << instr_count_n
                << " Inst: 0x" << std::hex << exe_m_to_exe_a_signals.read().inst << std::dec, DEBUG_LEVEL_PE_TOP);
        }

        // Update cycle count
        if (pe_running_current) {
            cycles_n = cycles_current + 1;
            if (cycles_n % 10 == 0) {
                DEBUG_MSG("[ProcessElement] Running: cycle=" << cycles_n
                          << ", instr_count=" << instr_count_n, DEBUG_LEVEL_PE_TOP);
            }
        }

        // Calculate pe_busy
        pe_busy_n = pe_running_n;

        // Write next values
        pe_running_next.write(pe_running_n);
        cycles_next.write(cycles_n);
        instr_count_next.write(instr_count_n);
        stage_reset_next.write(stage_reset_n);

        // Update control signals (combinational outputs)
        pe_running_signal.write(pe_running_n);
        stage_reset_signal.write(stage_reset_n);
        pe_busy.write(pe_busy_n);
    }

    void monitor_stalls() {
        bool exe_m_has_stall = exe_m_stall_dl.read() ||
                               exe_m_stall_ps.read() ||
                               exe_m_stall_pd.read();
        bool exe_a_has_stall = exe_a_stall_adder.read() ||
                               exe_a_stall_port_io.read();
        bool pipeline_stalled = exe_m_has_stall || exe_a_has_stall;

        if (pipeline_stalled && pe_running_reg.read()) {
            if (exe_m_stall_dl.read()) {
                DEBUG_MSG("[ProcessElement] Stall: DataLoader busy", DEBUG_LEVEL_PE_TOP);
            }
            if (exe_m_stall_ps.read()) {
                DEBUG_MSG("[ProcessElement] Stall: Port Static waiting", DEBUG_LEVEL_PE_TOP);
            }
            if (exe_m_stall_pd.read()) {
                DEBUG_MSG("[ProcessElement] Stall: Port Dynamic blocked", DEBUG_LEVEL_PE_TOP);
            }
            if (exe_a_stall_adder.read()) {
                DEBUG_MSG("[ProcessElement] Stall: VADDU in progress", DEBUG_LEVEL_PE_TOP);
            }
            if (exe_a_stall_port_io.read()) {
                DEBUG_MSG("[ProcessElement] Stall: Local Network PLI/PLO blocked", DEBUG_LEVEL_PE_TOP);
            }
        }
    }

    // === Sequential Logic (Register Updates) ===
    void main_thread() {
        // Reset initialization
        pe_running_reg.write(false);
        cycles_reg.write(0);
        instr_count_reg.write(0);
        stage_reset_reg.write(false);
        pc_init_value.write(0);

        wait(); // Wait for first clock edge

        while (true) {
            // On each clock edge, update registers with next values
            pe_running_reg.write(pe_running_next.read());
            cycles_reg.write(cycles_next.read());
            instr_count_reg.write(instr_count_next.read());
            stage_reset_reg.write(stage_reset_next.read());

            wait(); // Wait for next clock edge
        }
    }

public:
    // Performance monitoring interface
    uint64_t get_cycle_count() const { return cycles_reg.read(); }
    uint64_t get_instruction_count() const { return instr_count_reg.read(); }
    bool is_running() const { return pe_running_reg.read(); }
    bool is_halted() const {
        return if_id_halted_sig.read() &&
               exe_m_halted_sig.read() &&
               exe_a_halted_sig.read();
    }

    // Helper function to display pipeline stage status
    void print_stage_status() const {
        std::cout << "=== PE Pipeline Status (Cycle " << cycles_reg.read() << ") ===" << std::endl;
        std::cout << std::endl;

        // === IF_ID Stage Status ===
        std::cout << "[IF_ID Stage]" << std::endl;
        std::cout << "  PC: " << if_id_pc_sig.read() << std::endl;
        std::cout << "  Halted: " << (if_id_halted_sig.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PE Running: " << (pe_running_signal.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Valid Out: " << (if_id_to_exe_m_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Stalled: " << (exe_m_stage_stall.read() ? "Yes" : "No") << std::endl;
        std::cout << std::endl;

        // === EXE_M Stage Status ===
        std::cout << "[EXE_M Stage]" << std::endl;
        std::cout << "  Valid In: " << (if_id_to_exe_m_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Valid Out: " << (exe_m_to_exe_a_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Halted: " << (exe_m_halted_sig.read() ? "Yes" : "No") << std::endl;
        std::cout << "  DL Stall: " << (exe_m_stall_dl.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PS Stall: " << (exe_m_stall_ps.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PD Stall: " << (exe_m_stall_pd.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Total Stall: " << (exe_a_stage_stall.read() ? "Yes" : "No") << std::endl;
        std::cout << std::endl;

        // === EXE_A Stage Status ===
        std::cout << "[EXE_A Stage]" << std::endl;
        std::cout << "  Valid In: " << (exe_m_to_exe_a_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Halted: " << (exe_a_halted_sig.read() ? "Yes" : "No") << std::endl;
        std::cout << "  VADDU Stall: " << (exe_a_stall_adder.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Port IO Stall: " << (exe_a_stall_port_io.read() ? "Yes" : "No") << std::endl;
        std::cout << std::endl;

        // === Overall Status ===
        std::cout << "[Overall Status]" << std::endl;
        std::cout << "  PE Running: " << (pe_running_reg.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PE Busy: " << (pe_busy.read() ? "Yes" : "No") << std::endl;
        std::cout << "  All Stages Halted: " << (is_halted() ? "Yes" : "No") << std::endl;
        std::cout << "  Instructions Executed: " << instr_count_reg.read() << std::endl;
        std::cout << "  Pipeline Stalled: " << ((exe_m_stall_dl.read() || exe_m_stall_ps.read() ||
                                                   exe_m_stall_pd.read() || exe_a_stall_adder.read() ||
                                                   exe_a_stall_port_io.read()) ? "Yes" : "No") << std::endl;
        std::cout << "========================================" << std::endl;
    }
};

} // namespace pe
} // namespace hybridacc