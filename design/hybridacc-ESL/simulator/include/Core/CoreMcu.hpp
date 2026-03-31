#pragma once

#include <systemc>

#include <array>
#include <cstdint>

#include "Core/DecodeStage.hpp"
#include "Core/ExecuteStage.hpp"
#include "Core/FetchStage.hpp"
#include "Core/MemoryStage.hpp"
#include "Core/PipelineTypes.hpp"
#include "Core/Types.hpp"
#include "Core/WritebackStage.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

template <unsigned DATA_SRAM_BYTES = 65536>
SC_MODULE(CoreMcu) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};
	sc_in<bool> enable_i{"enable_i"};
	sc_in<sc_uint<32>> boot_addr_i{"boot_addr_i"};
	sc_in<sc_uint<32>> trap_vector_i{"trap_vector_i"};

	sc_out<bool> if_req_valid_o{"if_req_valid_o"};
	sc_out<sc_uint<32>> if_addr_o{"if_addr_o"};
	sc_in<bool> if_resp_valid_i{"if_resp_valid_i"};
	sc_in<sc_uint<32>> if_rdata_i{"if_rdata_i"};

	sc_out<bool> ls_req_valid_o{"ls_req_valid_o"};
	sc_out<bool> ls_req_write_o{"ls_req_write_o"};
	sc_out<sc_uint<32>> ls_req_addr_o{"ls_req_addr_o"};
	sc_out<sc_uint<32>> ls_req_wdata_o{"ls_req_wdata_o"};
	sc_out<sc_uint<4>> ls_req_wstrb_o{"ls_req_wstrb_o"};
	sc_in<bool> ls_resp_valid_i{"ls_resp_valid_i"};
	sc_in<sc_uint<32>> ls_resp_rdata_i{"ls_resp_rdata_i"};

	sc_out<bool> mmio_req_valid_o{"mmio_req_valid_o"};
	sc_out<MmioRequest> mmio_req_o{"mmio_req_o"};
	sc_in<bool> mmio_resp_valid_i{"mmio_resp_valid_i"};
	sc_in<MmioResponse> mmio_resp_i{"mmio_resp_i"};

	sc_in<bool> irq_meip_i{"irq_meip_i"};
	sc_in<bool> irq_mtip_i{"irq_mtip_i"};

	SC_CTOR(CoreMcu) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	uint32_t debug_read_pc() const { return pc_reg_; }
	uint32_t debug_read_gpr(uint32_t index) const { return index < kCoreGprCount ? gpr_[index] : 0u; }
	bool debug_is_halted() const { return halted_; }
	uint32_t debug_read_csr(uint32_t csr_id) const { return read_csr(csr_id); }
	MmioRequest debug_last_mmio_request() const { return last_mmio_request_; }

private:
	std::array<uint32_t, kCoreGprCount> gpr_{};
	uint32_t pc_reg_ = kIsramBase;
	uint32_t fetch_addr_reg_ = kIsramBase;
	uint32_t mstatus_reg_ = 0u;
	uint32_t misa_reg_ = 0x40001100u;  // RV32I + Zmmul (bit 12 = M) + Zicsr
	uint32_t mie_reg_ = 0u;
	uint32_t mtvec_reg_ = kIsramBase;
	uint32_t mscratch_reg_ = 0u;
	uint32_t mepc_reg_ = 0u;
	uint32_t mcause_reg_ = 0u;
	uint32_t mtval_reg_ = 0u;
	uint32_t mip_reg_ = 0u;
	uint64_t mcycle_reg_ = 0u;
	uint64_t minstret_reg_ = 0u;
	bool halted_ = false;
	bool faulted_ = false;
	bool in_trap_ = false;
	uint32_t last_fault_code_ = 0u;
	MmioRequest last_mmio_request_{};

	IfIdLatch ifid_{};
	IdExLatch idex_{};
	ExMemLatch exmem_{};
	MemWbLatch memwb_{};
	MemoryTransaction mem_txn_{};
	uint32_t if_inflight_addr_ = kIsramBase;
	bool if_req_was_sent_ = false;
	bool drop_next_if_resp_ = false;

	void reset_state() {
		gpr_.fill(0u);
		pc_reg_ = boot_addr_i.read().to_uint();
		fetch_addr_reg_ = boot_addr_i.read().to_uint();
		mstatus_reg_ = 0u;
		misa_reg_ = 0x40001100u;
		mie_reg_ = 0u;
		mtvec_reg_ = trap_vector_i.read().to_uint();
		mscratch_reg_ = 0u;
		mepc_reg_ = 0u;
		mcause_reg_ = 0u;
		mtval_reg_ = 0u;
		mip_reg_ = 0u;
		mcycle_reg_ = 0u;
		minstret_reg_ = 0u;
		halted_ = false;
		faulted_ = false;
		in_trap_ = false;
		last_fault_code_ = 0u;
		last_mmio_request_ = {};
		ifid_ = {};
		idex_ = {};
		exmem_ = {};
		memwb_ = {};
		mem_txn_ = {};
		if_inflight_addr_ = boot_addr_i.read().to_uint();
		if_req_was_sent_ = false;
		drop_next_if_resp_ = false;
	}

	uint32_t read_csr(uint32_t csr_id) const {
		switch (csr_id) {
		case 0x300u: return mstatus_reg_;
		case 0x301u: return misa_reg_;
		case 0x304u: return mie_reg_;
		case 0x305u: return mtvec_reg_;
		case 0x340u: return mscratch_reg_;
		case 0x341u: return mepc_reg_;
		case 0x342u: return mcause_reg_;
		case 0x343u: return mtval_reg_;
		case 0x344u: return mip_reg_;
		case 0xB00u: return static_cast<uint32_t>(mcycle_reg_);
		case 0xB80u: return static_cast<uint32_t>(mcycle_reg_ >> 32);
		case 0xB02u: return static_cast<uint32_t>(minstret_reg_);
		case 0xB82u: return static_cast<uint32_t>(minstret_reg_ >> 32);
		default: return 0u;
		}
	}

	void write_csr(uint32_t csr_id, uint32_t value) {
		switch (csr_id) {
		case 0x300u: mstatus_reg_ = value; break;
		case 0x304u: mie_reg_ = value; break;
		case 0x305u: mtvec_reg_ = value; break;
		case 0x340u: mscratch_reg_ = value; break;
		case 0x341u: mepc_reg_ = value; break;
		case 0x342u: mcause_reg_ = value; break;
		case 0x343u: mtval_reg_ = value; break;
		case 0xB00u: mcycle_reg_ = (mcycle_reg_ & 0xFFFFFFFF00000000ull) | value; break;
		case 0xB80u: mcycle_reg_ = (static_cast<uint64_t>(value) << 32) | static_cast<uint32_t>(mcycle_reg_); break;
		case 0xB02u: minstret_reg_ = (minstret_reg_ & 0xFFFFFFFF00000000ull) | value; break;
		case 0xB82u: minstret_reg_ = (static_cast<uint64_t>(value) << 32) | static_cast<uint32_t>(minstret_reg_); break;
		default: break;
		}
	}

	uint32_t apply_csr_op(const DecodedInstruction& decoded, uint32_t rs1_value, uint32_t& old_value) const {
		old_value = read_csr(decoded.csr);
		const uint32_t source = (decoded.csr_op == CsrOp::CSRRWI || decoded.csr_op == CsrOp::CSRRSI || decoded.csr_op == CsrOp::CSRRCI)
			? decoded.imm
			: rs1_value;
		switch (decoded.csr_op) {
		case CsrOp::CSRRW:
		case CsrOp::CSRRWI:
			return source;
		case CsrOp::CSRRS:
		case CsrOp::CSRRSI:
			return old_value | source;
		case CsrOp::CSRRC:
		case CsrOp::CSRRCI:
			return old_value & ~source;
		default:
			return old_value;
		}
	}

	uint32_t forward_value(uint8_t reg, uint32_t base_value) const {
		if (reg == 0u) {
			return 0u;
		}
		if (exmem_.valid && exmem_.decoded.reg_write && !exmem_.decoded.mem_read && exmem_.decoded.rd == reg) {
			if (exmem_.decoded.csr_op != CsrOp::NONE) {
				return exmem_.csr_old_value;
			}
			if (exmem_.decoded.branch_op == BranchOp::JAL || exmem_.decoded.branch_op == BranchOp::JALR) {
				return exmem_.decoded.pc + 4u;
			}
			return exmem_.alu_result;
		}
		if (memwb_.valid && memwb_.reg_write && memwb_.rd == reg) {
			return memwb_.write_value;
		}
		return base_value;
	}

	bool has_load_use_hazard() const {
		if (!ifid_.valid || !idex_.valid || !idex_.decoded.mem_read || idex_.decoded.rd == 0u) {
			return false;
		}
		const auto next_decoded = DecodeStage::decode(ifid_);
		return (next_decoded.use_rs1 && next_decoded.rs1 == idex_.decoded.rd) || (next_decoded.use_rs2 && next_decoded.rs2 == idex_.decoded.rd);
	}

	bool interrupt_pending() const {
		const bool global_enable = (mstatus_reg_ & (1u << 3)) != 0u;
		const bool ext_enable = (mie_reg_ & (1u << 11)) != 0u;
		const bool timer_enable = (mie_reg_ & (1u << 7)) != 0u;
		return global_enable && ((irq_meip_i.read() && ext_enable) || (irq_mtip_i.read() && timer_enable));
	}

	void enter_trap(uint32_t cause, uint32_t tval) {
		mepc_reg_ = pc_reg_;
		mcause_reg_ = cause;
		mtval_reg_ = tval;
		in_trap_ = true;
		pc_reg_ = mtvec_reg_;
		fetch_addr_reg_ = mtvec_reg_;
		ifid_ = {};
		idex_ = {};
		exmem_ = {};
		memwb_ = {};
		mem_txn_ = {};
		if (if_req_was_sent_) {
			drop_next_if_resp_ = true;
		}
	}

	void drive_idle_outputs() {
		if_req_valid_o.write(false);
		if_addr_o.write(fetch_addr_reg_);
		ls_req_valid_o.write(false);
		ls_req_write_o.write(false);
		ls_req_addr_o.write(0u);
		ls_req_wdata_o.write(0u);
		ls_req_wstrb_o.write(0u);
		mmio_req_valid_o.write(false);
		mmio_req_o.write(MmioRequest{});
	}

	void issue_memory_request(const ExMemLatch& exmem) {
		const uint32_t addr = exmem.alu_result;
		if (is_data_sram_addr(addr, DATA_SRAM_BYTES)) {
			ls_req_valid_o.write(true);
			ls_req_write_o.write(exmem.decoded.mem_write);
			ls_req_addr_o.write(addr);
			ls_req_wdata_o.write(MemoryStage::store_data(exmem.decoded.mem_op, exmem.rs2_value, addr));
			ls_req_wstrb_o.write(MemoryStage::write_strobe(exmem.decoded.mem_op, addr));
		} else {
			MmioRequest request{};
			request.write = exmem.decoded.mem_write;
			request.addr = addr;
			request.wdata = MemoryStage::store_data(exmem.decoded.mem_op, exmem.rs2_value, addr);
			request.wstrb = MemoryStage::write_strobe(exmem.decoded.mem_op, addr);
			last_mmio_request_ = request;
			mmio_req_valid_o.write(true);
			mmio_req_o.write(request);
		}
	}

	void seq_process() {
		reset_state();
		drive_idle_outputs();
		wait();

		while (true) {
			mcycle_reg_ += 1u;
			mip_reg_ = (irq_meip_i.read() ? (1u << 11) : 0u) | (irq_mtip_i.read() ? (1u << 7) : 0u);
			drive_idle_outputs();

			if (!enable_i.read()) {
				reset_state();
				wait();
				continue;
			}

			if (interrupt_pending() && !halted_ && !mem_txn_.active) {
				enter_trap((1u << 31) | (irq_meip_i.read() ? 11u : 7u), 0u);
				wait();
				continue;
			}

			WritebackStage::commit_gpr(gpr_, memwb_);
			if (memwb_.valid && memwb_.csr_write) {
				write_csr(memwb_.csr, memwb_.csr_value);
			}
			if (memwb_.valid) {
				minstret_reg_ += 1u;
			}
			if (memwb_.halt) {
				halted_ = true;
			}
			if (memwb_.fault) {
				faulted_ = true;
				last_fault_code_ = memwb_.fault_code;
				halted_ = true;
			}

			MemWbLatch next_memwb{};
			ExMemLatch next_exmem = exmem_;
			IdExLatch next_idex = idex_;
			IfIdLatch next_ifid = ifid_;
			bool stall_pipeline = halted_;
			bool hold_fetch = false;
			bool flush_decode = false;
			bool flush_fetch = false;
			uint32_t next_fetch_addr = fetch_addr_reg_;

			// === Consume IF response (from previous cycle's request) ===
			const bool if_resp_valid = if_resp_valid_i.read();
			bool if_resp_accepted = false;
			IfIdLatch fetched_ifid{};
			if (if_resp_valid) {
				if (drop_next_if_resp_) {
					// Stale response from before redirect — discard
					drop_next_if_resp_ = false;
				} else {
					fetched_ifid = FetchStage::latch(if_inflight_addr_, if_rdata_i.read().to_uint());
					if_resp_accepted = true;
				}
				if_req_was_sent_ = false;
			}

			if (mem_txn_.active) {
				if (!mem_txn_.request_issued) {
					issue_memory_request(mem_txn_.exmem);
					mem_txn_.request_issued = true;
					stall_pipeline = true;
				} else {
					const bool response_valid = mem_txn_.uses_mmio ? mmio_resp_valid_i.read() : ls_resp_valid_i.read();
					if (response_valid) {
						const uint32_t raw = mem_txn_.uses_mmio ? mmio_resp_i.read().rdata : ls_resp_rdata_i.read().to_uint();
						next_memwb.valid = true;
						next_memwb.rd = mem_txn_.exmem.decoded.rd;
						next_memwb.reg_write = mem_txn_.exmem.decoded.reg_write;
						next_memwb.write_value = mem_txn_.exmem.decoded.mem_read ? MemoryStage::load_data(mem_txn_.exmem.decoded.mem_op, raw, mem_txn_.exmem.alu_result) : 0u;
						next_memwb.pc = mem_txn_.exmem.decoded.pc;
						mem_txn_ = {};
						next_exmem = {};
					} else {
						stall_pipeline = true;
					}
				}
			} else if (exmem_.valid) {
				if (exmem_.fault || exmem_.decoded.trap) {
					next_memwb.valid = true;
					next_memwb.fault = true;
					next_memwb.fault_code = exmem_.fault ? exmem_.fault_code : 2u;
					next_exmem = {};
				} else if (exmem_.decoded.mem_read || exmem_.decoded.mem_write) {
					mem_txn_.active = true;
					mem_txn_.request_issued = false;
					mem_txn_.uses_mmio = !is_data_sram_addr(exmem_.alu_result, DATA_SRAM_BYTES);
					mem_txn_.exmem = exmem_;
					stall_pipeline = true;
				} else {
					next_memwb.valid = true;
					next_memwb.rd = exmem_.decoded.rd;
					next_memwb.reg_write = exmem_.decoded.reg_write;
					next_memwb.write_value = exmem_.decoded.csr_op != CsrOp::NONE ? exmem_.csr_old_value :
						((exmem_.decoded.branch_op == BranchOp::JAL || exmem_.decoded.branch_op == BranchOp::JALR) ? exmem_.decoded.pc + 4u : exmem_.alu_result);
					next_memwb.csr_write = exmem_.decoded.csr_op != CsrOp::NONE;
					next_memwb.csr = exmem_.decoded.csr;
					next_memwb.csr_value = exmem_.csr_new_value;
					next_memwb.halt = exmem_.decoded.halt;
					next_memwb.pc = exmem_.decoded.pc;
					next_exmem = {};
				}
			}

			if (!stall_pipeline) {
				if (idex_.valid) {
					const auto& decoded = idex_.decoded;
					ExMemLatch produced{};
					produced.valid = decoded.valid;
					produced.decoded = decoded;
					const uint32_t rs1_value = forward_value(decoded.rs1, idex_.rs1_value);
					const uint32_t rs2_value = forward_value(decoded.rs2, idex_.rs2_value);
					produced.rs2_value = rs2_value;
					uint32_t rhs_value = decoded.use_rs2 ? rs2_value : decoded.imm;
					if ((decoded.instruction & 0x7Fu) == 0x17u) {
						produced.alu_result = decoded.pc + decoded.imm;
					} else if (decoded.mem_read || decoded.mem_write || decoded.branch_op == BranchOp::JALR) {
						produced.alu_result = rs1_value + decoded.imm;
					} else {
						produced.alu_result = ExecuteStage::alu(decoded.alu_op, rs1_value, rhs_value);
					}
					if (decoded.csr_op != CsrOp::NONE) {
						produced.csr_new_value = apply_csr_op(decoded, rs1_value, produced.csr_old_value);
					}
					if (decoded.branch_op == BranchOp::JAL) {
						produced.branch_taken = true;
						produced.branch_target = decoded.pc + decoded.imm;
					} else if (decoded.branch_op == BranchOp::JALR) {
						produced.branch_taken = true;
						produced.branch_target = (rs1_value + decoded.imm) & ~1u;
					} else if (decoded.branch_op != BranchOp::NONE) {
						produced.branch_taken = ExecuteStage::branch_taken(decoded.branch_op, rs1_value, rs2_value);
						produced.branch_target = decoded.pc + decoded.imm;
					}
					if (decoded.trap) {
						produced.fault = true;
						produced.fault_code = 2u;
					}
					next_exmem = produced;
					if (produced.branch_taken) {
						pc_reg_ = produced.branch_target;
						next_fetch_addr = produced.branch_target;
						flush_decode = true;
						flush_fetch = true;
						if (if_req_was_sent_) {
							drop_next_if_resp_ = true;
						}
					}
				} else {
					next_exmem = {};
				}

				if (!flush_decode) {
					if (has_load_use_hazard()) {
						next_idex = {};
						next_ifid = ifid_;
						hold_fetch = true;
					} else if (ifid_.valid) {
						const auto decoded = DecodeStage::decode(ifid_);
						next_idex.valid = decoded.valid;
						next_idex.decoded = decoded;
						next_idex.rs1_value = gpr_[decoded.rs1];
						next_idex.rs2_value = gpr_[decoded.rs2];
					} else {
						next_idex = {};
					}
				} else {
					next_idex = {};
				}

				// === IF stage: accept response or hold ===
				if (flush_fetch) {
					next_ifid = {};
				} else if (hold_fetch) {
					// Keep current ifid; do not advance fetch
				} else if (if_resp_accepted) {
					next_ifid = fetched_ifid;
					pc_reg_ = if_inflight_addr_;
					next_fetch_addr = if_inflight_addr_ + 4u;
				} else {
					// No response yet — insert bubble
					next_ifid = {};
				}
			} else {
				// Pipeline stalled: absorb the response if it arrived, but don't advance
				// (fetched_ifid is discarded; we'll re-request when stall clears)
				next_idex = idex_;
				next_ifid = ifid_;
				next_fetch_addr = fetch_addr_reg_;
			}

			// === Drive IF request for next cycle ===
			const bool send_if_req = !halted_ && !stall_pipeline && !hold_fetch && enable_i.read() && !if_req_was_sent_;
			if_req_valid_o.write(send_if_req);
			if_addr_o.write(next_fetch_addr);
			if (send_if_req) {
				if_inflight_addr_ = next_fetch_addr;
				if_req_was_sent_ = true;
			}

			memwb_ = next_memwb;
			exmem_ = next_exmem;
			idex_ = next_idex;
			ifid_ = next_ifid;
			fetch_addr_reg_ = next_fetch_addr;
			gpr_[0] = 0u;

			wait();
		}
	}
};

} // namespace hybridacc::core