#pragma once

#include "utils.hpp"
#include <systemc>
#include "VADDU.hpp"
#include "PsumRegFile.hpp"

namespace hybridacc {
namespace pe {
// -----------------------------------------------------------------------------
// PLI-PLO operation state machine
enum class PLI_PLO_State {
    IDLE,
    WAIT_PLI,
    VADDU_RUNNING,
    WAIT_PLO
};

// Add operator<< support for PLI_PLO_State
inline std::ostream& operator<<(std::ostream& os, PLI_PLO_State state) {
    switch (state) {
        case PLI_PLO_State::IDLE: return os << "IDLE";
        case PLI_PLO_State::WAIT_PLI: return os << "WAIT_PLI";
        case PLI_PLO_State::VADDU_RUNNING: return os << "VADDU_RUNNING";
        case PLI_PLO_State::WAIT_PLO: return os << "WAIT_PLO";
        default: return os << "UNKNOWN";
    }
}

// sc_trace for PLI_PLO_State
inline void sc_trace(sc_core::sc_trace_file* tf, const PLI_PLO_State& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}

// -----------------------------------------------------------------------------
SC_MODULE(EXE_A_Stage) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // PE control signals
    sc_in<bool> stage_reset;
    sc_in<bool> pe_running;

    // pipelined inputs
    sc_in<v_fp16_t> vmul_out_in;
    sc_in<pe_decode_signals_t> EXE_M_decode_signals_in;
    sc_in<bool> signal_valid_in;

    // stall control
    sc_in<bool> stall_from_downstream;
    sc_out<bool> stall_adder;
    sc_out<bool> stall_port_io;

    // Status outputs
    sc_out<bool> halted_out;

    // Local Network ports
    sc_in<uint64_t> pli_in_data;
    sc_in<bool> pli_in_valid;
    sc_out<bool> pli_in_ready;

    sc_out<uint64_t> plo_out_data;
    sc_out<bool> plo_out_valid;
    sc_in<bool> plo_out_ready;

    SC_CTOR(EXE_A_Stage)
        : clk("clk"),
          reset_n("reset_n"),
          stage_reset("stage_reset"),
          pe_running("pe_running"),
          vmul_out_in("vmul_out_in"),
          EXE_M_decode_signals_in("EXE_M_decode_signals_in"),
          signal_valid_in("signal_valid_in"),
          stall_from_downstream("stall_from_downstream"),
          stall_adder("stall_adder"),
          stall_port_io("stall_port_io"),
          pli_in_data("pli_in_data"),
          pli_in_valid("pli_in_valid"),
          pli_in_ready("pli_in_ready"),
          plo_out_data("plo_out_data"),
          plo_out_valid("plo_out_valid"),
          plo_out_ready("plo_out_ready"),
          vadd("vadd"),
          PR("PR")
    {
        DEBUG_MSG("[Create] EXE_A_Stage");
        SC_CTHREAD(main_thread, clk.pos());
        reset_signal_is(reset_n, false);

        // === Combinational logic split by dataflow stages ===

        // Stage 1: Stall calculation
        SC_METHOD(stall_calculation_process);
        sensitive << decode_signals_reg << valid_reg << pli_plo_state_reg
                  << signal_valid_in << EXE_M_decode_signals_in
                  << pli_in_valid << plo_out_ready << vadd_done_sig;

        // Stage 2: Pipeline control and next state calculation
        SC_METHOD(pipeline_control_process);
        sensitive << decode_signals_reg << valid_reg << halted_reg << vmul_out_reg
                  << pli_plo_state_reg << pli_data_captured_reg
                  << pe_running << stage_reset << stall_from_downstream
                  << EXE_M_decode_signals_in << signal_valid_in << vmul_out_in
                  << vaddu_stall_sig << pli_stall_sig << plo_stall_sig << stage_stall_sig;

        // Stage 3: Execution datapath (VADDU control and PR control)
        SC_METHOD(execution_datapath_process);
        sensitive << decode_signals_next << valid_next << vmul_out_next
                  << pli_plo_state_next << pli_data_captured_next
                  << pli_in_valid << pli_in_data << plo_out_ready
                  << pr_p_out_sig << pr_vp_out_sig << vadd_result_sig << vadd_done_sig;

        // Stage 4: Output stage (drive output ports)
        SC_METHOD(output_stage_process);
        sensitive << vaddu_stall_sig << pli_stall_sig << plo_stall_sig
                  << pli_in_ready_internal << plo_out_valid_internal << plo_out_data_internal;

        bind();
    }

    // Sub-modules (public)
    VADDU vadd;
    PsumRegFile PR;

    // === Sequential Elements (Registers) ===
    sc_signal<pe_decode_signals_t> decode_signals_reg;
    sc_signal<pe_decode_signals_t> decode_signals_next;

    sc_signal<bool> valid_reg;
    sc_signal<bool> valid_next;

    sc_signal<v_fp16_t> vmul_out_reg;
    sc_signal<v_fp16_t> vmul_out_next;

    sc_signal<bool> halted_reg;
    sc_signal<bool> halted_next;

    // PLI-PLO state machine registers
    sc_signal<PLI_PLO_State> pli_plo_state_reg;
    sc_signal<PLI_PLO_State> pli_plo_state_next;

    sc_signal<v_fp16_t> pli_data_captured_reg;
    sc_signal<v_fp16_t> pli_data_captured_next;

    // === Internal signals for submodules ===
    // VADDU signals
    sc_signal<v_fp16_t> vadd_op1_sig;
    sc_signal<v_fp16_t> vadd_op2_sig;
    sc_signal<v_fp16_t> vadd_result_sig;
    sc_signal<fp16_t> vadd_acc_in_sig;
    sc_signal<fp16_t> vadd_acc_out_sig;
    sc_signal<bool> vadd_enable_sig;
    sc_signal<VADDU_Mode> vadd_mode_sig;
    sc_signal<bool> vadd_done_sig;

    // PsumRegFile signals
    sc_signal<bool> pr_enable_sig;
    sc_signal<int> pr_pid_sig;
    sc_signal<fp16_t> pr_p_in_sig;
    sc_signal<v_fp16_t> pr_vp_in_sig;
    sc_signal<bool> pr_vpid_write_en_sig;
    sc_signal<int> pr_mode_sig;
    sc_signal<fp16_t> pr_p_out_sig;
    sc_signal<v_fp16_t> pr_vp_out_sig;
    sc_signal<bool> pr_clear_regs_sig;
    sc_signal<bool> pr_use_pcounter_sig;
    sc_signal<bool> pr_set_pcounter_sig;
    sc_signal<bool> pr_clear_pcounter_sig;
    sc_signal<bool> pr_incr_pcounter_sig;

    // Combinational outputs
    sc_signal<bool> vaddu_stall_next;
    sc_signal<bool> pli_stall_next;
    sc_signal<bool> plo_stall_next;
    sc_signal<bool> pli_in_ready_next;
    sc_signal<bool> plo_out_valid_next;
    sc_signal<uint64_t> plo_out_data_next;

    // === Internal combinational signals ===
    sc_signal<bool> vaddu_stall_sig;
    sc_signal<bool> pli_stall_sig;
    sc_signal<bool> plo_stall_sig;
    sc_signal<bool> stage_stall_sig;

    sc_signal<bool> pli_in_ready_internal;
    sc_signal<bool> plo_out_valid_internal;
    sc_signal<uint64_t> plo_out_data_internal;

    void bind() {
        // clock and reset
        vadd.clk(clk);
        vadd.reset_n(reset_n);
        PR.clk(clk);
        PR.reset_n(reset_n);

        // VADDU connections
        vadd.op1(vadd_op1_sig);
        vadd.op2(vadd_op2_sig);
        vadd.result(vadd_result_sig);
        vadd.acc_in(vadd_acc_in_sig);
        vadd.acc_out(vadd_acc_out_sig);
        vadd.enable(vadd_enable_sig);
        vadd.mode(vadd_mode_sig);
        vadd.done(vadd_done_sig);

        // PsumRegFile connections
        PR.enable(pr_enable_sig);
        PR.pid(pr_pid_sig);
        PR.p_in(pr_p_in_sig);
        PR.vp_in(pr_vp_in_sig);
        PR.vpid_write_en(pr_vpid_write_en_sig);
        PR.mode(pr_mode_sig);
        PR.p_out(pr_p_out_sig);
        PR.vp_out(pr_vp_out_sig);
        PR.clear_regs(pr_clear_regs_sig);
        PR.use_pcounter(pr_use_pcounter_sig);
        PR.set_pcounter(pr_set_pcounter_sig);
        PR.clear_pcounter(pr_clear_pcounter_sig);
        PR.incr_pcounter(pr_incr_pcounter_sig);
    }

    // === Combinational Logic (Split by dataflow stages) ===

    // Stage 1: Calculate all stall conditions
    void stall_calculation_process() {
        bool vaddu_stall = false;
        bool pli_stall = false;
        bool plo_stall = false;

        pe_decode_signals_t decode_current = decode_signals_reg.read();
        bool valid_current = valid_reg.read();
        PLI_PLO_State pli_plo_state_current = pli_plo_state_reg.read();

        if (valid_current) {
            bool pli_plo_operation = decode_current.pli_plo_operation;

            if (pli_plo_operation) {
                // PLI-PLO stalls
                switch (pli_plo_state_current) {
                    case PLI_PLO_State::IDLE:
                    case PLI_PLO_State::WAIT_PLI:
                        if (!pli_in_valid.read()) {
                            pli_stall = true;
                        }
                        break;
                    case PLI_PLO_State::VADDU_RUNNING:
                        if (!vadd_done_sig.read()) {
                            vaddu_stall = true;
                        }
                        break;
                    case PLI_PLO_State::WAIT_PLO:
                        if (!plo_out_ready.read()) {
                            plo_stall = true;
                        }
                        break;
                }
            } else {
                // PR operations stall
                if (decode_current.vaddu_en && !vadd_done_sig.read()) {
                    vaddu_stall = true;
                }
            }
        }

        // Check incoming instruction
        if (!valid_current && signal_valid_in.read()) {
            pe_decode_signals_t next_signals = EXE_M_decode_signals_in.read();
            if (next_signals.pli_plo_operation && !pli_in_valid.read()) {
                pli_stall = true;
            }
        }

        // Write stall signals
        vaddu_stall_sig.write(vaddu_stall);
        pli_stall_sig.write(pli_stall);
        plo_stall_sig.write(plo_stall);
        stage_stall_sig.write(vaddu_stall || pli_stall || plo_stall || stall_from_downstream.read());
    }

    // Stage 2: Pipeline control and next state calculation
    void pipeline_control_process() {
        pe_decode_signals_t decode_current = decode_signals_reg.read();
        bool valid_current = valid_reg.read();
        v_fp16_t vmul_current = vmul_out_reg.read();
        bool halted_current = halted_reg.read();
        PLI_PLO_State pli_plo_state_current = pli_plo_state_reg.read();
        v_fp16_t pli_data_captured_current = pli_data_captured_reg.read();

        pe_decode_signals_t decode_n = decode_current;
        bool valid_n = valid_current;
        v_fp16_t vmul_n = vmul_current;
        bool halted_n = halted_current;
        PLI_PLO_State pli_plo_state_n = pli_plo_state_current;
        v_fp16_t pli_data_captured_n = pli_data_captured_current;

        // Reset logic
        if (stage_reset.read()) {
            decode_n = pe_decode_signals_t();
            valid_n = false;
            vmul_n = v_fp16_t();
            halted_n = false;
            pli_plo_state_n = PLI_PLO_State::IDLE;
            pli_data_captured_n = v_fp16_t();
        }
        // PE not running or halted
        else if (!pe_running.read() || halted_current) {
            // Keep current state
        }
        // Normal operation
        else {
            bool stage_stall = stage_stall_sig.read();

            // Pipeline advance
            if (!stage_stall) {
                decode_n = EXE_M_decode_signals_in.read();
                valid_n = signal_valid_in.read();
                vmul_n = vmul_out_in.read();

                if (valid_n && decode_n.halt) {
                    halted_n = true;
                    DEBUG_MSG("[EXE_A_Stage] HALT instruction detected");
                }
            }

            // PLI-PLO state machine transitions
            if (valid_current && decode_current.pli_plo_operation) {
                pli_plo_state_n = calculate_pli_plo_next_state(
                    pli_plo_state_current,
                    pli_in_valid.read(),
                    vadd_done_sig.read(),
                    plo_out_ready.read()
                );

                // Capture PLI data when available
                if ((pli_plo_state_current == PLI_PLO_State::IDLE ||
                     pli_plo_state_current == PLI_PLO_State::WAIT_PLI) &&
                    pli_in_valid.read()) {
                    pli_data_captured_n.fromUint64(pli_in_data.read());
                }
            }
        }

        // Write next values
        decode_signals_next.write(decode_n);
        valid_next.write(valid_n);
        vmul_out_next.write(vmul_n);
        halted_next.write(halted_n);
        pli_plo_state_next.write(pli_plo_state_n);
        pli_data_captured_next.write(pli_data_captured_n);
    }

    // Stage 3: Execution datapath (VADDU and PR control)
    void execution_datapath_process() {
        pe_decode_signals_t decode_n = decode_signals_next.read();
        bool valid_n = valid_next.read();
        v_fp16_t vmul_n = vmul_out_next.read();
        PLI_PLO_State pli_plo_state_n = pli_plo_state_next.read();
        v_fp16_t pli_data_captured_n = pli_data_captured_next.read();

        // Default: disable everything
        vadd_enable_sig.write(false);
        bool pli_ready = false;
        bool plo_valid = false;
        uint64_t plo_data = 0;

        // PR control signals
        update_pr_control_signals(decode_n, valid_n);

        // VADDU datapath control
        if (valid_n) {
            bool pli_plo_operation = decode_n.pli_plo_operation;

            if (pli_plo_operation) {
                // PLI-PLO operation datapath
                setup_pli_plo_datapath(pli_plo_state_n, pli_data_captured_n,
                                      pli_ready, plo_valid, plo_data);
            } else if (decode_n.vaddu_en) {
                // PR operation datapath
                setup_pr_vaddu_datapath(decode_n, vmul_n);
            }
        }

        // VADDU and PR interconnect
        vadd_acc_in_sig.write(pr_p_out_sig.read());
        pr_p_in_sig.write(vadd_acc_out_sig.read());
        pr_vp_in_sig.write(vadd_result_sig.read());

        // Write internal output signals
        pli_in_ready_internal.write(pli_ready);
        plo_out_valid_internal.write(plo_valid);
        plo_out_data_internal.write(plo_data);
    }

    // Stage 4: Output stage - drive output ports
    void output_stage_process() {
        stall_adder.write(vaddu_stall_sig.read());
        stall_port_io.write(pli_stall_sig.read() || plo_stall_sig.read());
        pli_in_ready.write(pli_in_ready_internal.read());
        plo_out_valid.write(plo_out_valid_internal.read());
        plo_out_data.write(plo_out_data_internal.read());
        halted_out.write(halted_reg.read());
    }

    // === Helper functions ===

    PLI_PLO_State calculate_pli_plo_next_state(PLI_PLO_State current_state,
                                                bool pli_valid,
                                                bool vadd_done,
                                                bool plo_ready) {
        switch (current_state) {
            case PLI_PLO_State::IDLE:
                return pli_valid ? PLI_PLO_State::VADDU_RUNNING : PLI_PLO_State::WAIT_PLI;

            case PLI_PLO_State::WAIT_PLI:
                return pli_valid ? PLI_PLO_State::VADDU_RUNNING : PLI_PLO_State::WAIT_PLI;

            case PLI_PLO_State::VADDU_RUNNING:
                if (!vadd_done) return PLI_PLO_State::VADDU_RUNNING;
                return plo_ready ? PLI_PLO_State::IDLE : PLI_PLO_State::WAIT_PLO;

            case PLI_PLO_State::WAIT_PLO:
                return plo_ready ? PLI_PLO_State::IDLE : PLI_PLO_State::WAIT_PLO;

            default:
                return PLI_PLO_State::IDLE;
        }
    }

    void setup_pli_plo_datapath(PLI_PLO_State state,
                                const v_fp16_t& pli_data_captured,
                                bool& pli_ready,
                                bool& plo_valid,
                                uint64_t& plo_data) {
        v_fp16_t pr_data = pr_vp_out_sig.read();

        switch (state) {
            case PLI_PLO_State::IDLE:
            case PLI_PLO_State::WAIT_PLI:
                pli_ready = true;
                if (pli_in_valid.read()) {
                    vadd_op1_sig.write(pli_data_captured);
                    vadd_op2_sig.write(pr_data);
                    vadd_enable_sig.write(true);
                    vadd_mode_sig.write(VADDU_Mode::ADD);
                }
                break;

            case PLI_PLO_State::VADDU_RUNNING:
                vadd_op1_sig.write(pli_data_captured);
                vadd_op2_sig.write(pr_data);

                if (vadd_done_sig.read()) {
                    v_fp16_t result = vadd_result_sig.read();
                    plo_data = result.toUint64();
                    plo_valid = true;
                }
                break;

            case PLI_PLO_State::WAIT_PLO:
                v_fp16_t result = vadd_result_sig.read();
                plo_data = result.toUint64();
                plo_valid = true;
                break;
        }
    }

    void setup_pr_vaddu_datapath(const pe_decode_signals_t& decode,
                                 const v_fp16_t& vmul_data) {
        vadd_op1_sig.write(vmul_data);

        v_fp16_t pr_data = pr_vp_out_sig.read();
        vadd_op2_sig.write(pr_data);

        vadd_enable_sig.write(true);

        vadd_mode_sig.write((decode.vaddu_mode == 0) ? VADDU_Mode::ADD : VADDU_Mode::ACCUMULATE);
    }

    void update_pr_control_signals(const pe_decode_signals_t& signals, bool valid) {
        if (!valid) {
            pr_enable_sig.write(0);
            pr_vpid_write_en_sig.write(false);
            pr_clear_regs_sig.write(false);
            pr_use_pcounter_sig.write(false);
            pr_set_pcounter_sig.write(false);
            pr_clear_pcounter_sig.write(false);
            pr_incr_pcounter_sig.write(false);
            return;
        }

        pr_enable_sig.write(signals.pr_en);
        pr_pid_sig.write(signals.rid5);
        pr_mode_sig.write(signals.vaddu_mode);
        pr_vpid_write_en_sig.write(signals.pr_write);
        pr_clear_regs_sig.write(signals.pr_clear_regs);
        pr_use_pcounter_sig.write(signals.pr_use_vcounter);
        pr_set_pcounter_sig.write(signals.pr_set_vcounter);
        pr_clear_pcounter_sig.write(signals.pr_clear_vcounter);
        pr_incr_pcounter_sig.write(signals.pr_incr_vcounter);
    }

    // === Sequential Logic (Register Updates) ===
    void main_thread() {
        // Reset initialization
        decode_signals_reg.write(pe_decode_signals_t());
        valid_reg.write(false);
        vmul_out_reg.write(v_fp16_t());
        halted_reg.write(false);
        pli_plo_state_reg.write(PLI_PLO_State::IDLE);
        pli_data_captured_reg.write(v_fp16_t());

        wait(); // Wait for first clock edge

        while (true) {
            // On each clock edge, update registers with next values
            decode_signals_reg.write(decode_signals_next.read());
            valid_reg.write(valid_next.read());
            vmul_out_reg.write(vmul_out_next.read());
            halted_reg.write(halted_next.read());
            pli_plo_state_reg.write(pli_plo_state_next.read());
            pli_data_captured_reg.write(pli_data_captured_next.read());

            DEBUG_MSG("[EXE_A_Stage] Clocked: Valid=" << valid_reg.read()
                      << " Halted=" << halted_reg.read()
                      << " Inst=0x" << std::hex << decode_signals_reg.read().inst << std::dec
                      << " PLI_PLO_State=" << pli_plo_state_reg.read()
                      << " Stall=" << stage_stall_sig.read());

            wait(); // Wait for next clock edge
        }
    }
};

} // namespace pe
} // namespace hybridacc