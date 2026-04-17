#pragma once
#include <cstdint>
#include <vector>
#include <systemc>
#include "Utils/utils.hpp"
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
    VRDIF<noc_request_t> noc_ps_in;
    VRDIF<noc_request_t> noc_pd_in;
    VRDIF<noc_request_t> noc_pli_in;
    VRDIF<noc_addr_req_t> noc_plo_in;
    VRDOF<noc_response_t> noc_plo_out;

    // Control ports
    sc_out<bool> pe_busy;
    sc_out<bool> pe_halted;

    // Local Network ports - using VRDIF/VRDOF
    VRDIF<uint64_t> ln_pli;
    VRDOF<uint64_t> ln_plo;

    // Constructor
    SC_HAS_PROCESS(ProcessElement);
    ProcessElement(sc_module_name name, size_t pe_fifo_depth = 4)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          router_enable("router_enable"),
          router_mode("router_mode"),
          noc_ps_in("noc_ps_in"),
          noc_pd_in("noc_pd_in"),
          noc_pli_in("noc_pli_in"),
          noc_plo_in("noc_plo_in"),
          noc_plo_out("noc_plo_out"),
          pe_busy("pe_busy"),
          pe_halted("pe_halted"),
          ln_pli("ln_pli"),
          ln_plo("ln_plo"),
          router("PE_Router", pe_fifo_depth),
          if_id_stage("IF_ID_Stage"),
          exe_m_stage("EXE_M_Stage"),
          exe_a_stage("EXE_A_Stage"),
          router_pe_ps_sig("router_pe_ps_sig"),
          router_pe_pd_sig("router_pe_pd_sig"),
          router_pe_pd_set_sig("router_pe_pd_set_sig"),
          router_pe_pli_sig("router_pe_pli_sig"),
          router_pe_plo_sig("router_pe_plo_sig")
    {
        DEBUG_MSG("[Create] ProcessElement", DEBUG_LEVEL_PE_TOP);

        // Single Sequential Process
        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational Processes

        // 1. Control Logic (Next State Calculation)
        SC_METHOD(comb_control_next);
        sensitive << pe_running_reg << cycles_reg << instr_count_reg << stage_reset_reg
                  << startup_guard_reg
                  << router_pe_reset << router_pe_start << router_pe_program
                  << if_id_halted_sig << exe_m_halted_sig << exe_a_halted_sig
                  << exe_m_to_exe_a_valid << exe_a_to_exe_m_ready << exe_a_stall_adder << exe_a_stall_port_pli << exe_a_stall_port_plo;

        // 2. Output Logic
        SC_METHOD(comb_outputs);
        sensitive << pe_running_reg << stage_reset_reg
              << if_id_halted_sig << exe_m_halted_sig << exe_a_halted_sig;

        // 3. Status/Performance Monitoring
        SC_METHOD(comb_monitoring);
        sensitive << exe_m_to_if_id_ready << exe_a_to_exe_m_ready
                  << exe_m_stall_dl << exe_m_stall_ps << exe_m_stall_pd
                  << exe_a_stall_adder << exe_a_stall_port_pli << exe_a_stall_port_plo;

        SC_METHOD(trace_process);
        sensitive << clk.pos();

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

    // Suppress auto-halt for a few cycles after START to let stage_reset
    // clear registered halted flags across all pipeline stages.
    sc_signal<sc_dt::sc_uint<3>> startup_guard_reg;
    sc_signal<sc_dt::sc_uint<3>> startup_guard_next;

    // Pipeline interconnect signals
    sc_signal<pe_decode_signals_t> if_id_to_exe_m_signals;
    sc_signal<bool> if_id_to_exe_m_valid;
    sc_signal<bool> exe_m_to_if_id_ready; // New: Ready path from EXE_M to IF_ID

    sc_signal<v_fp16_t> exe_m_to_exe_a_vmul;
    sc_signal<pe_decode_signals_t> exe_m_to_exe_a_signals;
    sc_signal<bool> exe_m_to_exe_a_valid;
    sc_signal<bool> exe_a_to_exe_m_ready; // New: Ready path from EXE_A to EXE_M

    // Stage status signals
    sc_signal<uint16_t> if_id_pc_sig;
    sc_signal<bool> if_id_halted_sig;
    sc_signal<bool> exe_m_halted_sig;
    sc_signal<bool> exe_a_halted_sig;

    // Stall signals (monitoring only now)
    sc_signal<bool> exe_m_stall_dl;
    sc_signal<bool> exe_m_stall_ps;
    sc_signal<bool> exe_m_stall_pd;

    sc_signal<bool> exe_a_stall_adder;
    sc_signal<bool> exe_a_stall_port_pli;
    sc_signal<bool> exe_a_stall_port_plo;

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
    VRDSIG<uint64_t> router_pe_pd_set_sig;
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
        bind_vr_interface(router.noc_ps_req_in_if, noc_ps_in);
        bind_vr_interface(router.noc_pd_req_in_if, noc_pd_in);
        bind_vr_interface(router.noc_pli_req_in_if, noc_pli_in);
        bind_vr_interface(router.noc_plo_req_in_if, noc_plo_in);
        bind_vr_interface(noc_plo_out, router.noc_plo_resp_out_if);

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
        if_id_stage.valid_out(if_id_to_exe_m_valid);

        // Status outputs
        if_id_stage.pc_out(if_id_pc_sig);
        if_id_stage.halted_out(if_id_halted_sig);

        // Instruction Memory programming
        if_id_stage.im_write_en(router_im_write_en_sig);
        if_id_stage.im_write_addr(router_im_write_addr_sig);
        if_id_stage.im_write_data(router_im_write_data_sig);

        // Flow control input (Backpressure from EXE_M)
        if_id_stage.ready_in(exe_m_to_if_id_ready);

        // === EXE_M Stage Connections ===
        exe_m_stage.clk(clk);
        exe_m_stage.reset_n(reset_n);

        // PE control signals
        exe_m_stage.stage_reset(stage_reset_signal);
        exe_m_stage.pe_running(pe_running_signal);

        // Pipeline inputs
        exe_m_stage.ID_decode_signals_in(if_id_to_exe_m_signals);
        exe_m_stage.valid_in(if_id_to_exe_m_valid);

        // Pipeline outputs
        exe_m_stage.vmul_out_out(exe_m_to_exe_a_vmul);
        exe_m_stage.EXE_A_decode_signals_out(exe_m_to_exe_a_signals);
        exe_m_stage.valid_out(exe_m_to_exe_a_valid);

        // Flow control Flow
        exe_m_stage.ready_out(exe_m_to_if_id_ready); // To IF_ID
        exe_m_stage.ready_in(exe_a_to_exe_m_ready);   // From EXE_A

        // Status outputs
        exe_m_stage.halted_out(exe_m_halted_sig);

        // Stall outputs (for monitoring)
        exe_m_stage.stall_DL(exe_m_stall_dl);
        exe_m_stage.stall_PS(exe_m_stall_ps);
        exe_m_stage.stall_PD(exe_m_stall_pd);

        // PS port (Port Static)
        connect_vr_signals(exe_m_stage.ps_data, router_pe_ps_sig);
        connect_vr_signals(router.pe_ps_out_if, router_pe_ps_sig);

        // PD port (Port Dynamic)
        connect_vr_signals(exe_m_stage.pd_data, router_pe_pd_sig);
        connect_vr_signals(router.pe_pd_out_if, router_pe_pd_sig);

        // PD port (Port Dynamic, set pop)
        connect_vr_signals(exe_m_stage.pd_data_set, router_pe_pd_set_sig);
        connect_vr_signals(router.pe_pd_set_out_if, router_pe_pd_set_sig);

        // === EXE_A Stage Connections ===
        exe_a_stage.clk(clk);
        exe_a_stage.reset_n(reset_n);

        // PE control signals
        exe_a_stage.stage_reset(stage_reset_signal);
        exe_a_stage.pe_running(pe_running_signal);

        // Pipeline inputs
        exe_a_stage.vmul_out_in(exe_m_to_exe_a_vmul);
        exe_a_stage.EXE_M_decode_signals_in(exe_m_to_exe_a_signals);
        exe_a_stage.valid_in(exe_m_to_exe_a_valid);

        // Flow control Flow
        exe_a_stage.ready_out(exe_a_to_exe_m_ready); // To EXE_M

        // Status outputs
        exe_a_stage.halted_out(exe_a_halted_sig);

        // Stall outputs (debug)
        // exe_a_stage.stall_adder(exe_a_stall_adder); // Removed from port list
        exe_a_stage.stall_port_pli(exe_a_stall_port_pli);
        exe_a_stage.stall_port_plo(exe_a_stall_port_plo);

        // PLI port (Port Local Input)
        connect_vr_signals(exe_a_stage.pli, router_pe_pli_sig);
        connect_vr_signals(router.pe_pli_out_if, router_pe_pli_sig);

        // PLO port (Port Local Output)
        connect_vr_signals(router.pe_plo_in_if, router_pe_plo_sig);
        connect_vr_signals(exe_a_stage.plo, router_pe_plo_sig);
    }

    // === Combinational Logic ===

    // 1. Control Logic (Next State)
    void comb_control_next() {
        bool pe_running_current = pe_running_reg.read();
        uint64_t cycles_current = cycles_reg.read();
        uint64_t instr_count_current = instr_count_reg.read();
        bool stage_reset_current = stage_reset_reg.read();
        sc_dt::sc_uint<3> startup_guard_current = startup_guard_reg.read();

        // Default: hold current values
        bool pe_running_n = pe_running_current;
        uint64_t cycles_n = cycles_current;
        uint64_t instr_count_n = instr_count_current;
        bool stage_reset_n = false;
        sc_dt::sc_uint<3> startup_guard_n = startup_guard_current;

        if (startup_guard_current > 0) {
            startup_guard_n = startup_guard_current - 1;
            stage_reset_n = true;
        }

        // Check router control signals
        if (router_pe_reset.read()) {
            // Reset all stages, keep pe_running state
            DEBUG_MSG("[ProcessElement] RESET signal detected", DEBUG_LEVEL_PE_TOP);
            stage_reset_n = true;
            startup_guard_n = 4;
        }

        if (router_pe_start.read()) {
            // Start PE: always reset + set running, even if already running.
            // This handles the case where the PE was stuck waiting for
            // in-flight NoC data that was dropped by spm_drop_noc_resp.
            DEBUG_MSG("[ProcessElement] START signal detected", DEBUG_LEVEL_PE_TOP);
            stage_reset_n = true;
            pe_running_n = true;
            cycles_n = 0;
            instr_count_n = 0;
            startup_guard_n = 4;
        }

        if (router_pe_program.read() && pe_running_current) {
            // Stop PE manually
            DEBUG_MSG("[ProcessElement] PROGRAM signal detected", DEBUG_LEVEL_PE_TOP);
            pe_running_n = false;
        }

        // Check for Auto Halting (All stages reported halted)
         // Only halt if actually running AND not in stage_reset cycle
         // (stage_reset clears halted flags, but they're registered —
         //  checking on the same cycle would see stale True from prev wave)
        if (pe_running_current && !stage_reset_current && !stage_reset_n && startup_guard_current == 0) {
            bool all_stages_halted = if_id_halted_sig.read() &&
                                     exe_m_halted_sig.read() &&
                                     exe_a_halted_sig.read();
            if (all_stages_halted) {
                DEBUG_MSG("[ProcessElement] All stages halted, stopping PE", DEBUG_LEVEL_PE_TOP);
                pe_running_n = false;
            }
        }

        // Performance Counters
        if (pe_running_current) {
            // Count instructions completed (Valid output from M to A, and A is accepting it)
             // Essentially, instruction "issued" to execution units.
             // Or maybe better: instructions leaving pipeline (exe_a completed).
             // Let's stick to M->A transition as "issued to execution".
             /*
                Correction: We should count completed instructions.
                If EXE_A is valid and ready (no backpressure to M), then M is feeding A.
             */
            bool instr_issued = exe_m_to_exe_a_valid.read() && exe_a_to_exe_m_ready.read();

            if (instr_issued) {
                instr_count_n = instr_count_current + 1;
                 DEBUG_MSG("[ProcessElement] Instruction issued: " << instr_count_n, DEBUG_LEVEL_PE_TOP);
            }

            // Count cycles
            cycles_n = cycles_current + 1;
            if (cycles_n % 100 == 0) {
                DEBUG_MSG("[ProcessElement] Running: cycle=" << cycles_n
                          << ", instr_count=" << instr_count_n, DEBUG_LEVEL_PE_TOP);
            }
        }

        // Write next values
        pe_running_next.write(pe_running_n);
        cycles_next.write(cycles_n);
        instr_count_next.write(instr_count_n);
        stage_reset_next.write(stage_reset_n);
        startup_guard_next.write(startup_guard_n);
    }

    // 2. Output Logic
    void comb_outputs() {
        pe_running_signal.write(pe_running_reg.read());
        stage_reset_signal.write(stage_reset_reg.read());
        pe_busy.write(pe_running_reg.read());
        pe_halted.write(!pe_running_reg.read()
                        && if_id_halted_sig.read()
                        && exe_m_halted_sig.read()
                        && exe_a_halted_sig.read());
    }

    // 3. Status/Performance Monitoring logic
    void comb_monitoring() {
        // Just used for debug prints or driving non-functional outputs if needed
        bool pipeline_stalled = !exe_m_to_if_id_ready.read() || !exe_a_to_exe_m_ready.read();

        // Derive legacy stall signals from ready signals where possible
        // If EXE_A is not ready, and it's not waiting for Port IO, we assume it's busy with computation (Adder/VMAC)
        bool a_not_ready = !exe_a_to_exe_m_ready.read();
        bool a_port_pli_stall = exe_a_stall_port_pli.read();
        bool a_port_plo_stall = exe_a_stall_port_plo.read();
        exe_a_stall_adder.write(a_not_ready && !a_port_pli_stall && !a_port_plo_stall);

        if (pipeline_stalled && pe_running_reg.read()) {
             // We can use the debug flags to see WHY it is stalled (internal flags)
             // Internal stalls are monitored via bound signals directly.
        }
    }

    // Stall propagation calculation (Removed - replaced by Valid/Ready wiring in bind)
    // Control logic (split above)

    // === Sequential Logic (Register Updates) ===
    void main_thread() {
        // Reset initialization
        pe_running_reg.write(false);
        cycles_reg.write(0);
        instr_count_reg.write(0);
        stage_reset_reg.write(false);
        startup_guard_reg.write(0);
        pc_init_value.write(0);

        wait(); // Wait for first clock edge

        while (true) {
            // On each clock edge, update registers with next values
            pe_running_reg.write(pe_running_next.read());
            cycles_reg.write(cycles_next.read());
            instr_count_reg.write(instr_count_next.read());
            stage_reset_reg.write(stage_reset_next.read());
            startup_guard_reg.write(startup_guard_next.read());

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
        bool if_id_stalled = !exe_m_to_if_id_ready.read();
        std::cout << "[IF_ID Stage]" << std::endl;
        std::cout << "  PC: " << if_id_pc_sig.read() << std::endl;
        std::cout << "  Halted: " << (if_id_halted_sig.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PE Running: " << (pe_running_signal.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Valid Out: " << (if_id_to_exe_m_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Stalled (Not Ready): " << (if_id_stalled ? "Yes" : "No") << std::endl;
        std::cout << std::endl;

        // === EXE_M Stage Status ===
        bool exe_m_total_stall = !exe_a_to_exe_m_ready.read();
        std::cout << "[EXE_M Stage]" << std::endl;
        std::cout << "  Valid In: " << (if_id_to_exe_m_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Valid Out: " << (exe_m_to_exe_a_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Halted: " << (exe_m_halted_sig.read() ? "Yes" : "No") << std::endl;
        std::cout << "  DL Stall: " << (exe_m_stall_dl.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PS Stall: " << (exe_m_stall_ps.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PD Stall: " << (exe_m_stall_pd.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Backpressure (Not Ready): " << (exe_m_total_stall ? "Yes" : "No") << std::endl;
        std::cout << std::endl;

        // === EXE_A Stage Status ===
        std::cout << "[EXE_A Stage]" << std::endl;
        std::cout << "  Valid In: " << (exe_m_to_exe_a_valid.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Halted: " << (exe_a_halted_sig.read() ? "Yes" : "No") << std::endl;
        std::cout << "  VADDU Stall: " << (exe_a_stall_adder.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Port PLI Stall: " << (exe_a_stall_port_pli.read() ? "Yes" : "No") << std::endl;
        std::cout << "  Port PLO Stall: " << (exe_a_stall_port_plo.read() ? "Yes" : "No") << std::endl;
        std::cout << std::endl;

        // === Overall Status ===
        std::cout << "[Overall Status]" << std::endl;
        std::cout << "  PE Running: " << (pe_running_reg.read() ? "Yes" : "No") << std::endl;
        std::cout << "  PE Busy: " << (pe_busy.read() ? "Yes" : "No") << std::endl;
        std::cout << "  All Stages Halted: " << (is_halted() ? "Yes" : "No") << std::endl;
        std::cout << "  Instructions Executed: " << instr_count_reg.read() << std::endl;
        std::cout << "  Pipeline Backpressure: " << ((if_id_stalled || exe_m_total_stall) ? "Yes" : "No") << std::endl;
        std::cout << "========================================" << std::endl;
    }

    // Trace support
    int trace_id = -1;
    uint32_t trace_pid = static_cast<uint32_t>(TRACE_PID::PE);
    std::string last_state = "IDLE";
    bool trace_init = false;

    void set_trace_id(int id) { trace_id = id;}
    void set_trace_context(uint32_t pid, int tid_base) {
        trace_pid = pid;
        trace_id = tid_base;
        trace_init = false;
        last_state = "IDLE";
    }
    int get_trace_num() const { return 1;}

    void trace_process() {
        if (trace_id == -1) return;

        if (!trace_init) {
            TRACE_THREAD_NAME(trace_pid, trace_id, "PE " + std::to_string(trace_id));
            TRACE_EVENT(last_state, "PE_State", TRACE_BEGIN, trace_pid, trace_id, "{}");
            trace_init = true;
        }

        std::string current_state;
        if (!router_enable.read()) {
            current_state = "DISABLED";
        } else if (is_halted()) {
            current_state = "HALTED";
        } else if (!pe_running_reg.read()) {
            current_state = "IDLE";
        } else if (exe_m_stall_dl.read()) {
            current_state = "STALL_M_DL";
        } else if (exe_m_stall_ps.read()) {
            current_state = "STALL_M_PS";
        } else if (exe_m_stall_pd.read()) {
            current_state = "STALL_M_PD";
        } else if (exe_a_stall_adder.read()) {
            current_state = "STALL_A_ADDER";
        } else if (exe_a_stall_port_pli.read()) {
            current_state = "STALL_A_PORT_PLI";
        } else if (exe_a_stall_port_plo.read()) {
            current_state = "STALL_A_PORT_PLO";
        } else if (!exe_m_to_if_id_ready.read()) {
            current_state = "STALL_EXE_M";
        } else if (!exe_a_to_exe_m_ready.read()) {
            current_state = "STALL_EXE_A";
        } else {
            current_state = "RUNNING";
        }

        if (current_state != last_state) {
            // End previous state
            TRACE_EVENT(last_state, "PE_State", TRACE_END, trace_pid, trace_id, "{}");
            // Begin new state
            TRACE_EVENT(current_state, "PE_State", TRACE_BEGIN, trace_pid, trace_id, "{}");
            last_state = current_state;
        }
    }
};

} // namespace pe
} // namespace hybridacc