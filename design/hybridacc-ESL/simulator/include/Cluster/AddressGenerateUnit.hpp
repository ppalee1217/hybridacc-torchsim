#pragma once

#include <systemc>
#include <array>
#include <cstdint>
#include <string>

#include "Utils/utils.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

enum class AguCtrlBit : int {
	CTRL_GEN_START = 0,
	CTRL_GEN_STOP = 1,
	CTRL_ULTRA = 3,
};

enum class AguStatusBit : int {
	STATUS_BUSY = 0,
	STATUS_DONE = 1,
	STATUS_ERROR = 2,
	STATUS_STALL = 3,
};

// -----------------------------------------------------------------------------
// AddressGenerateUnit (AGU)
// -----------------------------------------------------------------------------
// Role:
// 1) Through MMIO write configuration (base / iter / stride / tag rules)
// 2) In RUN state, continuously output a "address + tag + ultra + mask" descriptor
// 3) Only advance the loop counter when gen_valid && gen_ready handshake succeeds
//
// Note:
// - This is skeleton, outputting "descriptors", not directly accessing SRAM or packet data.
// - 4-level loop with idx_reg[0] as innermost, idx_reg[3] as outermost.
// - tag only retains low 6 bits (aligned with NoC addr[5:0]).
// -----------------------------------------------------------------------------
SC_MODULE(AddressGenerateUnit) {
public:

	enum AguRegOffset : uint32_t {
		REG_BASE_ADDR   = 0x00, // 32-bit base address (group-local word64), actual = base + idx0*stride0 + idx1*stride1 + ...
		REG_BASE_ADDR_H = 0x04, // 32-bit base address high，for future >32-bit address expansion，currently not enabled (must write 0).
		REG_ITER01      = 0x08, // iter0=bit15:0 iter1=bit31:16
		REG_ITER23      = 0x0C, // iter2=bit15:0 iter3=bit31:16
		REG_STRIDE0     = 0x10, // 32-bit stride for idx0 (unit: word64)
		REG_STRIDE1     = 0x14, // 32-bit stride for idx1 (unit: word64)
		REG_STRIDE2     = 0x18, // 32-bit stride for idx2 (unit: word64)
		REG_STRIDE3     = 0x1C, // 32-bit stride for idx3 (unit: word64)
		REG_CTRL        = 0x20, // bit0=gen start, bit1=gen stop, bit3=ultra
		REG_STATUS      = 0x24, // bit0=busy, bit1=done, bit2=error, bit3=stalled
		REG_LANE_CFG    = 0x28, // bit0=idx0 to addr, bit1=idx1 to addr, ...
								// bit8=idx0 to tag, bit9=idx1 to tag, ...
		REG_TAG_BASE    = 0x40, // 32-bit tag base (only low 6 bit used)
		REG_TAG_STRIDE0 = 0x44, // 32-bit tag stride for idx0
		REG_TAG_STRIDE1 = 0x48, // 32-bit tag stride for idx1
		REG_TAG_CTRL    = 0x4C, // bit0=tag gen enable, bit1=tag idx from idx1, bit2=tag idx from idx0, ...
		REG_MASK_CFG    = 0x54, // bit0~15: mask for idx0~idx3 (1=enable)
		REG_ERR_CODE    = 0x58, // 32-bit error code for last gen failure (0 if no error)
		REG_DBG_TAG     = 0x5C, // 32-bit debug register to record last generated tag (for error reporting)
		REG_DBG_ADDR    = 0x60, // 32-bit debug register to record last generated addr (for error reporting)
	};

    // --- Clock / Reset ---
	sc_in<bool> clk;
	sc_in<bool> reset_n;

    // --- MMIO config interface ---
    // cfg_addr: AGU bank internal offset (8-bit sufficient to cover 0x00~0xFF)
	sc_in<bool> cfg_write;
	sc_in<sc_uint<8>> cfg_addr;
	sc_in<sc_uint<32>> cfg_wdata;
	sc_out<sc_uint<32>> cfg_rdata;

    // --- External control (can coexist with MMIO CTRL)---
	sc_in<bool> start;
	sc_in<bool> stop;

    // --- Generated descriptor interface ---
	sc_out<bool> gen_valid;
	sc_in<bool> gen_ready;
	sc_out<sc_uint<32>> gen_addr;
	sc_out<sc_uint<16>> gen_tag;
	sc_out<bool> gen_ultra;
	sc_out<sc_uint<16>> gen_mask;

    // --- State output ---
	sc_out<bool> busy;
	sc_out<bool> done;
	sc_out<sc_uint<2>> fsm_state;

	// MMIO register offset（bank-local）
	SC_CTOR(AddressGenerateUnit)
		: clk("clk"),
		  reset_n("reset_n"),
		  cfg_write("cfg_write"),
		  cfg_addr("cfg_addr"),
		  cfg_wdata("cfg_wdata"),
		  cfg_rdata("cfg_rdata"),
		  start("start"),
		  stop("stop"),
		  gen_valid("gen_valid"),
		  gen_ready("gen_ready"),
		  gen_addr("gen_addr"),
		  gen_tag("gen_tag"),
		  gen_ultra("gen_ultra"),
		  gen_mask("gen_mask"),
		  busy("busy"),
		  done("done"),
		  fsm_state("fsm_state") {
		SC_METHOD(comb_read);
		sensitive << cfg_addr
				  << base_addr_reg
				  << base_addr_h_reg
				  << iter_reg[0] << iter_reg[1] << iter_reg[2] << iter_reg[3]
				  << stride_reg[0] << stride_reg[1] << stride_reg[2] << stride_reg[3]
				  << ctrl_reg << lane_cfg_reg
				  << tag_base_reg << tag_stride0_reg << tag_stride1_reg << tag_ctrl_reg
				  << mask_cfg_reg << err_code_reg << dbg_last_tag_reg << dbg_last_addr_reg
				  << busy_reg << done_reg << error_reg << stalled_reg;

		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);

		SC_METHOD(trace_process);
		sensitive << clk.pos();
	}

	void set_trace_context(uint32_t pid, int tid_base) {
		trace_pid = pid;
		trace_id = tid_base;
		trace_init = false;
		last_state = "IDLE";
	}

	int get_trace_num() const { return 1; }

private:
	static constexpr uint64_t DBG_REPORT_PERIOD = 256;

	int trace_id = -1;
	uint32_t trace_pid = 0;
	bool trace_init = false;
	std::string last_state = "IDLE";
	uint32_t last_addr = 0;
	uint32_t last_tag = 0;

	void trace_process() {
		if (trace_id < 0) return;

		auto make_args = [&]() {
			return std::string("{\"addr\": ")
				+ std::to_string(gen_addr.read().to_uint())
				+ ", \"tag\": "
				+ std::to_string(gen_tag.read().to_uint())
				+ "}";
		};

		if (!trace_init) {
			TRACE_THREAD_NAME(trace_pid, static_cast<uint32_t>(trace_id), std::string(name()) + " AGU");
			TRACE_EVENT(last_state, "AGU_State", TRACE_BEGIN, trace_pid, static_cast<uint32_t>(trace_id), make_args());
			trace_init = true;
		}

		std::string current_state;
		const bool stall = gen_valid.read() && !gen_ready.read();
		if (state_reg == AguState::DONE || done_reg.read()) {
			current_state = "DONE";
		} else if (state_reg == AguState::RUN) {
			current_state = stall ? "STALL" : "RUN";
		} else if (busy_reg.read()) {
			current_state = "BUSY";
		} else {
			current_state = "IDLE";
		}

		const bool update = (current_state != last_state) || (gen_addr.read().to_uint() != last_addr) || (gen_tag.read().to_uint() != last_tag);
		if (update) {
			TRACE_EVENT(last_state, "AGU_State", TRACE_END, trace_pid, static_cast<uint32_t>(trace_id), "{}");
			TRACE_EVENT(current_state, "AGU_State", TRACE_BEGIN, trace_pid, static_cast<uint32_t>(trace_id), make_args());
			last_state = current_state;
			last_addr = gen_addr.read().to_uint();
			last_tag = gen_tag.read().to_uint();
		}
	}

	// -------------------------------
	// Config / runtime registers
	// -------------------------------
	// base_addr_h_reg currently reserved for future >32-bit address expansion.
	sc_signal<sc_uint<32>> base_addr_reg;
	sc_signal<sc_uint<32>> base_addr_h_reg;
	std::array<sc_signal<sc_uint<16>>, 4> iter_reg;
	std::array<sc_signal<sc_uint<32>>, 4> stride_reg;

	sc_signal<sc_uint<32>> ctrl_reg;
	sc_signal<sc_uint<32>> lane_cfg_reg;
	sc_signal<sc_uint<32>> tag_base_reg;
	sc_signal<sc_uint<32>> tag_stride0_reg;
	sc_signal<sc_uint<32>> tag_stride1_reg;
	sc_signal<sc_uint<32>> tag_ctrl_reg;
	sc_signal<sc_uint<32>> mask_cfg_reg;

	sc_signal<sc_uint<32>> err_code_reg;
	sc_signal<sc_uint<32>> dbg_last_tag_reg;
	sc_signal<sc_uint<32>> dbg_last_addr_reg;

	sc_signal<bool> busy_reg;
	sc_signal<bool> done_reg;
	sc_signal<bool> error_reg;
	sc_signal<bool> stalled_reg;

	// 4-level loop counter：idx_reg[0] is innermost, idx_reg[3] is outermost.
	std::array<sc_uint<16>, 4> idx_reg{};
	std::array<sc_uint<16>, 4> idx_next_reg{};
	bool idx_update_pending = false;

	enum class AguState : uint8_t {
		IDLE = 0,
		RUN = 1,
		DONE = 2,
	};
	AguState state_reg = AguState::IDLE;

	struct Stage0Reg {
		bool valid = false;
		std::array<sc_uint<16>, 4> idx{};
		sc_uint<32> base_addr = 0;
		std::array<sc_uint<32>, 4> stride{};
		sc_uint<6> tag_base = 0;
		sc_uint<2> tag_level = 0;
		sc_uint<8> tag_stride0 = 1;
		sc_uint<8> tag_stride1 = 1;
		sc_uint<16> mask = 0;
		bool ultra = false;
		bool last = false;
	};

	struct Stage1Reg {
		bool valid = false;
		std::array<uint64_t, 4> prod{};
		sc_uint<32> base_addr = 0;
		sc_uint<6> tag_base = 0;
		uint32_t tag_mul = 0;
		sc_uint<16> mask = 0;
		bool ultra = false;
		bool last = false;
	};

	struct N0Stage0Reg {
		bool valid = false;
		uint64_t s01 = 0;
		uint64_t s23 = 0;
		sc_uint<32> base_addr = 0;
		sc_uint<6> tag_base = 0;
		uint32_t tag_mul = 0;
		sc_uint<16> mask = 0;
		bool ultra = false;
		bool last = false;
	};

	struct N0Stage1Reg {
		bool valid = false;
		uint64_t total = 0;
		sc_uint<6> tag_base = 0;
		uint32_t tag_mul = 0;
		sc_uint<16> mask = 0;
		bool ultra = false;
		bool last = false;
	};

	struct Stage2Reg {
		bool valid = false;
		sc_uint<32> addr = 0;
		sc_uint<16> tag = 0;
		sc_uint<16> mask = 0;
		bool ultra = false;
		bool last = false;
	};

	Stage0Reg s0_reg{};
	Stage1Reg s1_reg{};
	N0Stage0Reg n0_s0_reg{};
	N0Stage1Reg n0_s1_reg{};
	Stage2Reg s2_reg{};
	bool run_last_issued = false;

	// Clear loop counters.
	void clear_counters() {
		idx_reg[0] = 0;
		idx_reg[1] = 0;
		idx_reg[2] = 0;
		idx_reg[3] = 0;
		idx_next_reg = idx_reg;
		idx_update_pending = false;
	}

	// iter=0 is normalized to 1 to avoid disabled-loop ambiguity.
	static sc_uint<16> normalize_iter(sc_uint<16> v) {
		return (v == 0) ? sc_uint<16>(1) : v;
	}

	// Compute next loop state from current idx (combinational), update idx on next cycle by register.
	bool compute_next_loop(const std::array<sc_uint<16>, 4>& current,
					   std::array<sc_uint<16>, 4>& next) const {
		const sc_uint<16> i0 = iter_reg[0].read();
		const sc_uint<16> i1 = iter_reg[1].read();
		const sc_uint<16> i2 = iter_reg[2].read();
		const sc_uint<16> i3 = iter_reg[3].read();

		next = current;
		next[0] = next[0] + 1;
		if (next[0] < i0) return false;
		next[0] = 0;

		next[1] = next[1] + 1;
		if (next[1] < i1) return false;
		next[1] = 0;

		next[2] = next[2] + 1;
		if (next[2] < i2) return false;
		next[2] = 0;

		next[3] = next[3] + 1;
		if (next[3] < i3) return false;
		next[3] = 0;

		return true;
	}

	// n0 three-stage address pipeline helpers.
	static uint64_t calc_addr_n0_s01(const std::array<uint64_t, 4>& p) {
		return p[0] + p[1];
	}

	static uint64_t calc_addr_n0_s23(const std::array<uint64_t, 4>& p) {
		return p[2] + p[3];
	}

	static uint64_t calc_addr_n0_total(sc_uint<32> base_addr, uint64_t s01, uint64_t s23) {
		return static_cast<uint64_t>(base_addr) + s01 + s23;
	}

	static sc_uint<32> calc_addr_n0_out(uint64_t total) {
		return static_cast<sc_uint<32>>(total & 0xFFFFFFFFu);
	}

	static sc_uint<16> calc_tag(sc_uint<6> tag_base, uint32_t tag_mul) {
		const uint32_t tag = static_cast<uint32_t>(tag_base) + tag_mul;
		return static_cast<sc_uint<16>>(tag & 0x3F);
	}

	void clear_pipeline() {
		s0_reg.valid = false;
		s1_reg.valid = false;
		n0_s0_reg.valid = false;
		n0_s1_reg.valid = false;
		s2_reg.valid = false;
		run_last_issued = false;
	}

	// MMIO write decode：
	// - Only effective when cfg_write=1
	// - Minimal side effects, not pushing FSM directly (FSM managed in seq_process)
	void apply_mmio_write() {
		if (!cfg_write.read()) {
			return;
		}

		const sc_uint<8> off = cfg_addr.read();
		const sc_uint<32> val = cfg_wdata.read();
		switch (off.to_uint()) {
			case REG_BASE_ADDR:   base_addr_reg.write(val); break;
			case REG_BASE_ADDR_H: base_addr_h_reg.write(val); break;
			case REG_ITER01:
				iter_reg[0].write(normalize_iter(static_cast<sc_uint<16>>(val.range(15, 0))));
				iter_reg[1].write(normalize_iter(static_cast<sc_uint<16>>(val.range(31, 16))));
				break;
			case REG_ITER23:
				iter_reg[2].write(normalize_iter(static_cast<sc_uint<16>>(val.range(15, 0))));
				iter_reg[3].write(normalize_iter(static_cast<sc_uint<16>>(val.range(31, 16))));
				break;
			case REG_STRIDE0: stride_reg[0].write(val); break;
			case REG_STRIDE1: stride_reg[1].write(val); break;
			case REG_STRIDE2: stride_reg[2].write(val); break;
			case REG_STRIDE3: stride_reg[3].write(val); break;
			case REG_CTRL: ctrl_reg.write(val); break;
			case REG_LANE_CFG: lane_cfg_reg.write(val); break;
			case REG_TAG_BASE: tag_base_reg.write(val); break;
			case REG_TAG_STRIDE0: tag_stride0_reg.write(val); break;
			case REG_TAG_STRIDE1: tag_stride1_reg.write(val); break;
			case REG_TAG_CTRL: tag_ctrl_reg.write(val); break;
			case REG_MASK_CFG: mask_cfg_reg.write(val); break;
			case REG_ERR_CODE: err_code_reg.write(val); break;
			default: break;
		}
	}

	// MMIO read mux：combinational logic to read current register contents.
	void comb_read() {
		sc_uint<32> r = 0;
		switch (cfg_addr.read().to_uint()) {
			case REG_BASE_ADDR: r = base_addr_reg.read(); break;
			case REG_BASE_ADDR_H: r = base_addr_h_reg.read(); break;
			case REG_ITER01:
				r.range(15, 0) = iter_reg[0].read();
				r.range(31, 16) = iter_reg[1].read();
				break;
			case REG_ITER23:
				r.range(15, 0) = iter_reg[2].read();
				r.range(31, 16) = iter_reg[3].read();
				break;
			case REG_STRIDE0: r = stride_reg[0].read(); break;
			case REG_STRIDE1: r = stride_reg[1].read(); break;
			case REG_STRIDE2: r = stride_reg[2].read(); break;
			case REG_STRIDE3: r = stride_reg[3].read(); break;
			case REG_CTRL: r = ctrl_reg.read(); break;
			case REG_STATUS:
				r[0] = busy_reg.read();
				r[1] = done_reg.read();
				r[2] = error_reg.read();
				r[3] = stalled_reg.read();
				break;
			case REG_LANE_CFG: r = lane_cfg_reg.read(); break;
			case REG_TAG_BASE: r = tag_base_reg.read(); break;
			case REG_TAG_STRIDE0: r = tag_stride0_reg.read(); break;
			case REG_TAG_STRIDE1: r = tag_stride1_reg.read(); break;
			case REG_TAG_CTRL: r = tag_ctrl_reg.read(); break;
			case REG_MASK_CFG: r = mask_cfg_reg.read(); break;
			case REG_ERR_CODE: r = err_code_reg.read(); break;
			case REG_DBG_TAG: r = dbg_last_tag_reg.read(); break;
			case REG_DBG_ADDR: r = dbg_last_addr_reg.read(); break;
			default: r = 0; break;
		}
		cfg_rdata.write(r);
	}

	// Main sequence flow:
	// 1) reset initialization
	// 2) handle MMIO write / start / stop / soft_reset
	// 3) RUN state outputs descriptor, advances loop counter on handshake
	// 4) loop completion transitions to DONE, next cycle returns to IDLE
	void seq_process() {
		base_addr_reg.write(0);
		base_addr_h_reg.write(0);
		for (int i = 0; i < 4; ++i) {
			iter_reg[i].write(1);
			stride_reg[i].write(0);
			idx_reg[i] = 0;
			idx_next_reg[i] = 0;
		}
		ctrl_reg.write(0);
		lane_cfg_reg.write(0);
		tag_base_reg.write(0);
		tag_stride0_reg.write(1);
		tag_stride1_reg.write(1);
		tag_ctrl_reg.write(0);
		mask_cfg_reg.write(0xF);

		err_code_reg.write(0);
		dbg_last_tag_reg.write(0);
		dbg_last_addr_reg.write(0);

		busy_reg.write(false);
		done_reg.write(false);
		error_reg.write(false);
		stalled_reg.write(false);

		gen_valid.write(false);
		gen_addr.write(0);
		gen_tag.write(0);
		gen_ultra.write(false);
		gen_mask.write(0);
		busy.write(false);
		done.write(false);
		fsm_state.write(0);
		clear_pipeline();

		state_reg = AguState::IDLE;
		wait();

		while (true) {
			// done is only a pulse, so clear it each cycle, then set high on completion.
			done_reg.write(false);
			// stalled means this cycle valid but downstream not ready.
			stalled_reg.write(false);

			apply_mmio_write();

			if (idx_update_pending) {
				idx_reg = idx_next_reg;
				idx_update_pending = false;
			}

			// CTRL.bit2 = soft reset：retain config, only clear FSM/counter.
			const bool soft_reset = ctrl_reg.read()[2];
			if (soft_reset) {
				clear_counters();
				clear_pipeline();
				state_reg = AguState::IDLE;
				busy_reg.write(false);
				gen_valid.write(false);
			}

			const bool stop_req = stop.read() || ctrl_reg.read()[1];
			const bool start_req = start.read() || ctrl_reg.read()[0];

			if (stop_req) {
				state_reg = AguState::IDLE;
				clear_pipeline();
				busy_reg.write(false);
				gen_valid.write(false);
				if (ctrl_reg.read()[1]) {
					sc_uint<32> ctrl_v = ctrl_reg.read();
					ctrl_v[1] = 0;
					ctrl_reg.write(ctrl_v);
				}
			}

			if (!stop_req && start_req && state_reg == AguState::IDLE) {
				clear_counters();
				clear_pipeline();
				help_print_info();
				state_reg = AguState::RUN;
				busy_reg.write(true);
				sc_uint<32> ctrl_v = ctrl_reg.read();
				ctrl_v[0] = 0;
				ctrl_reg.write(ctrl_v);
			}

			if (state_reg == AguState::RUN && ctrl_reg.read()[0]) {
				sc_uint<32> ctrl_v = ctrl_reg.read();
				ctrl_v[0] = 0;
				ctrl_reg.write(ctrl_v);
			}

			if (state_reg == AguState::RUN) {
				const bool out_fire = s2_reg.valid && gen_ready.read();
				const bool backpressure = s2_reg.valid && !gen_ready.read();
				const bool s2_ready = (!s2_reg.valid) || gen_ready.read();
				const bool n0_s1_ready = (!n0_s1_reg.valid) || s2_ready;
				const bool n0_s0_ready = (!n0_s0_reg.valid) || n0_s1_ready;
				const bool s1_ready = (!s1_reg.valid) || n0_s0_ready;
				const bool s0_ready = (!s0_reg.valid) || s1_ready;

				const bool move_n0_s1_to_s2 = n0_s1_reg.valid && s2_ready;
				const bool move_n0_s0_to_n0_s1 = n0_s0_reg.valid && n0_s1_ready;
				const bool move_s1_to_n0_s0 = s1_reg.valid && n0_s0_ready;
				const bool move_s0_to_s1 = s0_reg.valid && s1_ready;
				const bool bypass_s1_to_s2 = move_s1_to_n0_s0 && !n0_s0_reg.valid && !n0_s1_reg.valid && s2_ready;
				const bool move_s1_to_n0_s0_eff = move_s1_to_n0_s0 && !bypass_s1_to_s2;

				const bool can_issue = !run_last_issued;
				const bool issue_new = can_issue && s0_ready;

				Stage0Reg issue_payload{};
				bool issue_payload_valid = false;
				std::array<sc_uint<16>, 4> issue_next_idx{};
				bool issue_has_next_idx = false;
				bool issue_all_done = false;

				if (out_fire && s2_reg.last) {
					state_reg = AguState::DONE;
					busy_reg.write(false);
					done_reg.write(true);
				}

				if (out_fire) {
					help_print_state();
				}

				if (backpressure) {
					stalled_reg.write(true);
				}

				if (move_n0_s1_to_s2) {
					s2_reg.valid = true;
					s2_reg.addr = calc_addr_n0_out(n0_s1_reg.total);
					s2_reg.tag = calc_tag(n0_s1_reg.tag_base, n0_s1_reg.tag_mul);
					s2_reg.mask = n0_s1_reg.mask;
					s2_reg.ultra = n0_s1_reg.ultra;
					s2_reg.last = n0_s1_reg.last;
				} else if (bypass_s1_to_s2) {
					s2_reg.valid = true;
					const uint64_t s01 = calc_addr_n0_s01(s1_reg.prod);
					const uint64_t s23 = calc_addr_n0_s23(s1_reg.prod);
					const uint64_t total = calc_addr_n0_total(s1_reg.base_addr, s01, s23);
					s2_reg.addr = calc_addr_n0_out(total);
					s2_reg.tag = calc_tag(s1_reg.tag_base, s1_reg.tag_mul);
					s2_reg.mask = s1_reg.mask;
					s2_reg.ultra = s1_reg.ultra;
					s2_reg.last = s1_reg.last;
				} else if (s2_ready) {
					s2_reg.valid = false;
				}

				if (move_n0_s0_to_n0_s1) {
					n0_s1_reg.valid = true;
					n0_s1_reg.total = calc_addr_n0_total(n0_s0_reg.base_addr, n0_s0_reg.s01, n0_s0_reg.s23);
					n0_s1_reg.tag_base = n0_s0_reg.tag_base;
					n0_s1_reg.tag_mul = n0_s0_reg.tag_mul;
					n0_s1_reg.mask = n0_s0_reg.mask;
					n0_s1_reg.ultra = n0_s0_reg.ultra;
					n0_s1_reg.last = n0_s0_reg.last;
				} else if (n0_s1_ready) {
					n0_s1_reg.valid = false;
				}

				if (move_s1_to_n0_s0_eff) {
					n0_s0_reg.valid = true;
					n0_s0_reg.s01 = calc_addr_n0_s01(s1_reg.prod);
					n0_s0_reg.s23 = calc_addr_n0_s23(s1_reg.prod);
					n0_s0_reg.base_addr = s1_reg.base_addr;
					n0_s0_reg.tag_base = s1_reg.tag_base;
					n0_s0_reg.tag_mul = s1_reg.tag_mul;
					n0_s0_reg.mask = s1_reg.mask;
					n0_s0_reg.ultra = s1_reg.ultra;
					n0_s0_reg.last = s1_reg.last;
				} else if (n0_s0_ready) {
					n0_s0_reg.valid = false;
				}

				if (issue_new) {
					std::array<sc_uint<16>, 4> next_idx{};
					const bool all_done = compute_next_loop(idx_reg, next_idx);

					issue_payload.valid = true;
					issue_payload.idx = idx_reg;
					issue_payload.base_addr = base_addr_reg.read();
					issue_payload.stride[0] = stride_reg[0].read();
					issue_payload.stride[1] = stride_reg[1].read();
					issue_payload.stride[2] = stride_reg[2].read();
					issue_payload.stride[3] = stride_reg[3].read();
					issue_payload.tag_base = static_cast<sc_uint<6>>(tag_base_reg.read() & 0x3F);
					issue_payload.tag_level = static_cast<sc_uint<2>>(tag_ctrl_reg.read() & 0x3);
					issue_payload.tag_stride0 = static_cast<sc_uint<8>>(tag_stride0_reg.read() & 0xFF);
					issue_payload.tag_stride1 = static_cast<sc_uint<8>>(tag_stride1_reg.read() & 0xFF);
					issue_payload.mask = static_cast<sc_uint<16>>(mask_cfg_reg.read() & 0xFFFF);
					issue_payload.ultra = ctrl_reg.read()[3];
					issue_payload.last = all_done;
					issue_payload_valid = true;
					issue_next_idx = next_idx;
					issue_has_next_idx = true;
					issue_all_done = all_done;
				}

				if (s1_ready) {
					const Stage0Reg* src = nullptr;
					if (move_s0_to_s1) {
						src = &s0_reg;
					} else if (issue_payload_valid) {
						src = &issue_payload;
					}

					if (src != nullptr) {
						uint32_t tag_index = 0;
						uint32_t tag_stride = 1;
						if (src->tag_level == 0) {
							tag_index = src->idx[0].to_uint();
							tag_stride = static_cast<uint32_t>(src->tag_stride0);
						} else if (src->tag_level == 1) {
							tag_index = src->idx[1].to_uint();
							tag_stride = static_cast<uint32_t>(src->tag_stride1);
						} else if (src->tag_level == 2) {
							tag_index = src->idx[2].to_uint();
							tag_stride = static_cast<uint32_t>(src->tag_stride1);
						} else {
							tag_index = src->idx[3].to_uint();
							tag_stride = static_cast<uint32_t>(src->tag_stride1);
						}

						s1_reg.valid = true;
						s1_reg.base_addr = src->base_addr;
						s1_reg.prod[0] = static_cast<uint64_t>(src->idx[0]) * static_cast<uint64_t>(src->stride[0]);
						s1_reg.prod[1] = static_cast<uint64_t>(src->idx[1]) * static_cast<uint64_t>(src->stride[1]);
						s1_reg.prod[2] = static_cast<uint64_t>(src->idx[2]) * static_cast<uint64_t>(src->stride[2]);
						s1_reg.prod[3] = static_cast<uint64_t>(src->idx[3]) * static_cast<uint64_t>(src->stride[3]);

						s1_reg.tag_base = src->tag_base;
						s1_reg.tag_mul = tag_index * tag_stride;

						s1_reg.mask = src->mask;
						s1_reg.ultra = src->ultra;
						s1_reg.last = src->last;
					} else {
						s1_reg.valid = false;
					}
				}

				if (s0_ready) {
					const bool issue_consumed_by_s1 = issue_payload_valid && !move_s0_to_s1 && s1_ready;
					if (issue_payload_valid && !issue_consumed_by_s1) {
						s0_reg = issue_payload;
						s0_reg.valid = true;
					} else {
						s0_reg.valid = false;
					}

					const bool issue_committed = issue_payload_valid && (issue_consumed_by_s1 || s0_reg.valid);
					if (issue_committed && issue_has_next_idx) {
						idx_next_reg = issue_next_idx;
						idx_update_pending = true;
						if (issue_all_done) {
							run_last_issued = true;
						}
					}
				}

				gen_valid.write(s2_reg.valid);
				gen_addr.write(s2_reg.addr);
				gen_tag.write(s2_reg.tag);
				gen_ultra.write(s2_reg.ultra);
				gen_mask.write(s2_reg.mask);

				if (s2_reg.valid) {
					dbg_last_addr_reg.write(s2_reg.addr);
					dbg_last_tag_reg.write(s2_reg.tag);
				}
			} else if (state_reg == AguState::DONE) {
				// DONE state maintains one cycle, next cycle returns to IDLE.
				state_reg = AguState::IDLE;
				clear_pipeline();
				gen_valid.write(false);
			} else {
				clear_pipeline();
				gen_valid.write(false);
			}

			busy.write(busy_reg.read());
			done.write(done_reg.read());

			switch (state_reg) {
				case AguState::IDLE: fsm_state.write(0); break;
				case AguState::RUN: fsm_state.write(1); break;
				case AguState::DONE: fsm_state.write(2); break;
				default: fsm_state.write(0); break;
			}

			wait();
		}
	}

	// Helper to print current config / state for debugging.
	void help_print_info() {
		DEBUG_MSG("AGU Config: base=0x" << std::hex << base_addr_reg.read() << " iter=[" << std::dec << iter_reg[0].read() << "," << iter_reg[1].read() << "," << iter_reg[2].read() << "," << iter_reg[3].read() << "] stride=[" << stride_reg[0].read() << "," << stride_reg[1].read() << "," << stride_reg[2].read() << "," << stride_reg[3].read() << "] ctrl=0x" << std::hex << ctrl_reg.read() << " lane_cfg=0x" << lane_cfg_reg.read() << " tag_base=0x" << tag_base_reg.read() << " tag_stride0=0x" << tag_stride0_reg.read() << " tag_stride1=0x" << tag_stride1_reg.read() << " tag_ctrl=0x" << tag_ctrl_reg.read() << " mask_cfg=0x" << mask_cfg_reg.read(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
	}

	void help_print_state() {
		DEBUG_MSG("AGU State: last_addr=0x" << std::hex << dbg_last_addr_reg.read() << " last_tag=0x" << dbg_last_tag_reg.read() << " busy=" << busy_reg.read() << " done=" << done_reg.read() << " error=" << error_reg.read() << " stalled=" << stalled_reg.read() << " iter=[" << std::dec << s0_reg.idx[0] << "," << s0_reg.idx[1] << "," << s0_reg.idx[2] << "," << s0_reg.idx[3] << "]", DEBUG_LEVEL_CLUSTER_COMPONENTS);
	}
};

} // namespace cluster
} // namespace hybridacc
