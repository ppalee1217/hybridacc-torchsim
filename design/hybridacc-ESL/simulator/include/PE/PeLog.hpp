#pragma once
/**
 * @file PeLog.hpp
 * @brief Optional per-PE observation log (M21 WP-J; agent_run/261021-m21-decomposition/decide-counterfactual/decision.md).
 *
 * Enabled by `--pe-log FILE`. Each ProcessElement appends key=value lines; times are sc_time_stamp() in ps at the
 * rising edge, and every signal is read as that edge's sequential processes see it (the values the hardware acts
 * on at that edge), the same convention as --wave-log. b= is the PE's bus (NoC port), r= its row on that bus.
 *
 *   H b= r= p=ps|pd|pli|plo_req|plo_resp|ln_in|ln_out d= a= m= s= t=
 *       a handshake the PE took part in (valid and ready at this edge): p names the port; d is the 64-bit
 *       payload (hex), a the request address (hex), m the request lane mask, s the response status; fields a
 *       port does not carry are '-'. PE commands (START, RESET, LOAD_PROGRAM, STOP) arrive on ps with the
 *       command address, so they are H lines too.
 *   Q b= r= run= en= mode= ifv= mrdy= mv= mar= madv= dl= ps= pd= as= ardy= halt= t=
 *       the PE's observable state, written when any field changes (run-length): run PE running; en, mode the
 *       router enable and route mode; ifv IF_ID has an instruction for EXE_M; mrdy EXE_M accepts it; mv EXE_M
 *       holds an instruction; mar it is an arithmetic one (VADDU-using: VMAC, VMUL, VPSUM); madv it moves to
 *       EXE_A at this edge; dl/ps/pd EXE_M's stall flags; as the EXE_A state (EXE_A_State as an integer);
 *       ardy EXE_A accepts from EXE_M; halt all three stages report halted.
 *   W cmd=configure|clear h= w= rows= loop_pc= loop_end_pc= t=
 *       the simulator's spatial-tail override (our fork, afbdf1e) wrote every PE's IF_ID input-window loop
 *       (NetworkOnChip); rows is the active PE rows per bus. Rows at or beyond it still run the program, and the
 *       MBUS drains their PLO responses locally, so only active rows' responses reach the HDDU.
 *
 * Classification (arithmetic, waiting on input, waiting on output, control, idle) is left to the analysis, which
 * reads these raw observables; the hooks only read signals and write the file; they change no simulated state.
 */
#include <cstdint>
#include <cstdio>

namespace hacc {
inline std::FILE*& pe_log() {
    static std::FILE* file = nullptr;
    return file;
}
}  // namespace hacc
