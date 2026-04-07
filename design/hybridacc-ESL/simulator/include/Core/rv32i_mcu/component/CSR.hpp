#pragma once

#include <systemc>
#include <cstdint>
#include "Core/Types.hpp"
#include "Core/rv32i_mcu/PipelineTypes.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

SC_MODULE(CSR) {
	sc_core::sc_in<bool> clk;
	sc_core::sc_in<bool> rst_n;

	sc_core::sc_in<bool> inst_cnt;
	sc_core::sc_in<CsrOp> exe_funct3;
	sc_core::sc_in<sc_uint<32>> exe_imm;
	sc_core::sc_in<sc_uint<32>> exe_rs1;
	sc_core::sc_in<sc_uint<12>> csr_imm;
	sc_core::sc_in<bool> rs1_idx_valid;
	sc_core::sc_out<sc_uint<32>> csr_out;

	sc_core::sc_in<bool> timer_interrupt;
	sc_core::sc_in<bool> external_interrupt;
	sc_core::sc_in<bool> software_interrupt;
	sc_core::sc_out<bool> interrupt;
	sc_core::sc_out<sc_uint<32>> mtvec_out;
	sc_core::sc_out<sc_uint<32>> mepc_out;
	sc_core::sc_in<sc_uint<32>> pc;
	sc_core::sc_in<sc_uint<32>> pc_plus_4;
	sc_core::sc_in<bool> wfi;
	sc_core::sc_in<bool> ecall;
	sc_core::sc_in<bool> ebreak;
	sc_core::sc_in<bool> mret;
	sc_core::sc_in<bool> stall;

	sc_core::sc_signal<sc_uint<64>> cycle_reg_;
	sc_core::sc_signal<sc_uint<64>> instret_reg_;
	sc_core::sc_signal<sc_uint<32>> mstatus_reg_;
	sc_core::sc_signal<sc_uint<32>> mie_reg_;
	sc_core::sc_signal<sc_uint<32>> mtvec_reg_;
	sc_core::sc_signal<sc_uint<32>> mepc_reg_;
	sc_core::sc_signal<sc_uint<32>> mip_reg_;
	sc_core::sc_signal<sc_uint<32>> mcause_reg_;
	sc_core::sc_signal<sc_uint<32>> mscratch_reg_;

	sc_uint<64> cycle_next_ = 0;
	sc_uint<64> instret_next_ = 0;
	sc_uint<32> mstatus_next_ = 0;
	sc_uint<32> mie_next_ = 0;
	sc_uint<32> mtvec_next_ = 0;
	sc_uint<32> mepc_next_ = 0;
	sc_uint<32> mip_next_ = 0;
	sc_uint<32> mcause_next_ = 0;
	sc_uint<32> mscratch_next_ = 0;

	static constexpr uint32_t kMstatusMieBit = 3;
	static constexpr uint32_t kMstatusMpieBit = 7;
	static constexpr uint32_t kMstatusMppShift = 11;
	static constexpr uint32_t kMachineMpp = 0x3;
	static constexpr uint32_t kMstatusMask =
		(1u << kMstatusMieBit) |
		(1u << kMstatusMpieBit) |
		(0x3u << kMstatusMppShift);

	static constexpr uint32_t kMieMsieBit = 3;
	static constexpr uint32_t kMieMtieBit = 7;
	static constexpr uint32_t kMieMeieBit = 11;
	static constexpr uint32_t kMieMask =
		(1u << kMieMsieBit) |
		(1u << kMieMtieBit) |
		(1u << kMieMeieBit);
	static constexpr uint32_t kMipMask = kMieMask;

	static constexpr uint32_t kMtvecModeDirect = 0;
	static constexpr uint32_t kMtvecModeVectored = 1;
	static constexpr uint32_t kMcauseInterruptBit = 31;
	static constexpr uint32_t kTimerInterruptCause = 7;
	static constexpr uint32_t kExternalInterruptCause = 11;
	static constexpr uint32_t kEbreakCause = 3;
	static constexpr uint32_t kSoftwareInterruptCause = 3;
	static constexpr uint32_t kEcallMachineCause = 11;
	static constexpr uint32_t kCsrMcycleh = 0xB80;
	static constexpr uint32_t kCsrMinstreth = 0xB82;

	SC_CTOR(CSR) {
		SC_METHOD(compute_comb);
		sensitive << inst_cnt << exe_funct3 << exe_imm << exe_rs1 << csr_imm << rs1_idx_valid
				  << timer_interrupt << external_interrupt << software_interrupt << pc << pc_plus_4
				  << wfi << ecall << ebreak << mret << stall
				  << cycle_reg_ << instret_reg_ << mstatus_reg_ << mie_reg_
			  << mtvec_reg_ << mepc_reg_ << mip_reg_ << mcause_reg_
			  << mscratch_reg_;
		SC_METHOD(write_ff);
		sensitive << clk.pos();
		async_reset_signal_is(rst_n, false);
	}

	void compute_comb() {
		const uint64_t cycle = cycle_reg_.read().to_uint64();
		const uint64_t instret = instret_reg_.read().to_uint64();
		const uint32_t mstatus = mstatus_reg_.read().to_uint();
		const uint32_t mie = mie_reg_.read().to_uint();
		const uint32_t mtvec = mtvec_reg_.read().to_uint();
		const uint32_t mepc = mepc_reg_.read().to_uint();
		const uint32_t mip = mip_reg_.read().to_uint();
		const uint32_t mcause = mcause_reg_.read().to_uint();
		const uint32_t mscratch = mscratch_reg_.read().to_uint();

		const bool timer_irq_masked = timer_interrupt.read() && (((mie >> kMieMtieBit) & 0x1u) != 0u);
		const bool ext_irq_masked = external_interrupt.read() && (((mie >> kMieMeieBit) & 0x1u) != 0u);
		const bool sw_irq_masked = software_interrupt.read() && (((mie >> kMieMsieBit) & 0x1u) != 0u);
		const bool irq_masked = (timer_irq_masked || ext_irq_masked || sw_irq_masked) && (((mstatus >> kMstatusMieBit) & 0x1u) != 0u);
		const bool trap_pending = irq_masked || ecall.read() || ebreak.read();

		const uint32_t csr_addr = csr_imm.read().to_uint();
		uint32_t csr_rdata = 0;
		switch (csr_addr) {
			case kCsrMstatus:  csr_rdata = mstatus; break;
			case kCsrMisa:     csr_rdata = 0x40001100u; break;
			case kCsrMie:      csr_rdata = mie; break;
			case kCsrMtvec:    csr_rdata = mtvec; break;
			case kCsrMscratch: csr_rdata = mscratch; break;
			case kCsrMepc:     csr_rdata = mepc; break;
			case kCsrMip:      csr_rdata = mip; break;
			case kCsrMcause:   csr_rdata = mcause; break;
			case kCsrMcycle:   csr_rdata = static_cast<uint32_t>(cycle & 0xFFFFFFFFu); break;
			case kCsrMcycleh:  csr_rdata = static_cast<uint32_t>((cycle >> 32) & 0xFFFFFFFFu); break;
			case kCsrMinstret: csr_rdata = static_cast<uint32_t>(instret & 0xFFFFFFFFu); break;
			case kCsrMinstreth: csr_rdata = static_cast<uint32_t>((instret >> 32) & 0xFFFFFFFFu); break;
			default:           csr_rdata = 0; break;
		}
		csr_out.write(csr_rdata);

		bool csr_wb_en = false;
		uint32_t csr_wb_data = mstatus;
		switch (exe_funct3.read()) {
			case CsrOp::CSRRW:
				csr_wb_en = rs1_idx_valid.read();
				csr_wb_data = exe_rs1.read().to_uint();
				break;
			case CsrOp::CSRRS:
				csr_wb_en = rs1_idx_valid.read();
				csr_wb_data = csr_rdata | exe_rs1.read().to_uint();
				break;
			case CsrOp::CSRRC:
				csr_wb_en = rs1_idx_valid.read();
				csr_wb_data = csr_rdata & ~exe_rs1.read().to_uint();
				break;
			case CsrOp::CSRRWI:
				csr_wb_en = rs1_idx_valid.read();
				csr_wb_data = exe_imm.read().to_uint();
				break;
			case CsrOp::CSRRSI:
				csr_wb_en = rs1_idx_valid.read();
				csr_wb_data = csr_rdata | exe_imm.read().to_uint();
				break;
			case CsrOp::CSRRCI:
				csr_wb_en = rs1_idx_valid.read();
				csr_wb_data = csr_rdata & ~exe_imm.read().to_uint();
				break;
			case CsrOp::NONE:
			default:
				csr_wb_en = false;
				csr_wb_data = mstatus;
				break;
		}

		cycle_next_ = cycle + 1u;
		instret_next_ = instret + (inst_cnt.read() ? 1u : 0u);
		mstatus_next_ = mstatus;
		mie_next_ = mie;
		mtvec_next_ = mtvec;
		mepc_next_ = mepc;
		mip_next_ = mip;
		mcause_next_ = mcause;
		mscratch_next_ = mscratch;

		if (trap_pending) {
			const bool old_mie = ((mstatus >> kMstatusMieBit) & 0x1u) != 0u;
			uint32_t next_mstatus = mstatus & ~kMstatusMask;
			next_mstatus |= (kMachineMpp << kMstatusMppShift);
			if (old_mie) next_mstatus |= (1u << kMstatusMpieBit);
			mstatus_next_ = next_mstatus;

			mepc_next_ = wfi.read() ? pc_plus_4.read() : pc.read();

			uint32_t next_mip = mip & ~kMipMask;
			if (timer_irq_masked) next_mip |= (1u << kMieMtieBit);
			if (ext_irq_masked) next_mip |= (1u << kMieMeieBit);
			if (sw_irq_masked) next_mip |= (1u << kMieMsieBit);
			mip_next_ = next_mip;

			if (timer_irq_masked) {
				mcause_next_ = (1u << kMcauseInterruptBit) | kTimerInterruptCause;
			} else if (ext_irq_masked) {
				mcause_next_ = (1u << kMcauseInterruptBit) | kExternalInterruptCause;
			} else if (sw_irq_masked) {
				mcause_next_ = (1u << kMcauseInterruptBit) | kSoftwareInterruptCause;
			} else if (ecall.read()) {
				mcause_next_ = kEcallMachineCause;
			} else if (ebreak.read()) {
				mcause_next_ = kEbreakCause;
			}
		} else if (mret.read()) {
			const bool old_mpie = ((mstatus >> kMstatusMpieBit) & 0x1u) != 0u;
			uint32_t next_mstatus = mstatus & ~kMstatusMask;
			next_mstatus |= (kMachineMpp << kMstatusMppShift);
			next_mstatus |= (1u << kMstatusMpieBit);
			if (old_mpie) next_mstatus |= (1u << kMstatusMieBit);
			mstatus_next_ = next_mstatus;
			mip_next_ = 0;
		} else if (csr_wb_en) {
			const uint32_t mstatus_masked = csr_wb_data & kMstatusMask;
			const uint32_t mie_masked = csr_wb_data & kMieMask;

			if (csr_addr == kCsrMstatus) mstatus_next_ = mstatus_masked;
			if (csr_addr == kCsrMie)     mie_next_ = mie_masked;
			if (csr_addr == kCsrMtvec)   mtvec_next_ = csr_wb_data;
			if (csr_addr == kCsrMepc)    mepc_next_ = csr_wb_data;
			if (csr_addr == kCsrMscratch) mscratch_next_ = csr_wb_data;
			if (csr_addr == kCsrMcycle) {
				uint64_t v = static_cast<uint64_t>(cycle_next_);
				cycle_next_ = (v & 0xFFFFFFFF00000000ull) | csr_wb_data;
			}
			if (csr_addr == kCsrMcycleh) {
				uint64_t v = static_cast<uint64_t>(cycle_next_);
				cycle_next_ = (v & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(csr_wb_data) << 32);
			}
			if (csr_addr == kCsrMinstret) {
				uint64_t v = static_cast<uint64_t>(instret_next_);
				instret_next_ = (v & 0xFFFFFFFF00000000ull) | csr_wb_data;
			}
			if (csr_addr == kCsrMinstreth) {
				uint64_t v = static_cast<uint64_t>(instret_next_);
				instret_next_ = (v & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(csr_wb_data) << 32);
			}
		}

		mepc_out.write(mepc);
		interrupt.write(trap_pending);

		const uint32_t mtvec_base = mtvec & ~0x3u;
		const uint32_t mtvec_mode = mtvec & 0x3u;
		const bool is_sync_exception = ecall.read() || ebreak.read();
		if (!is_sync_exception && mtvec_mode == kMtvecModeVectored) {
			mtvec_out.write(mtvec_base + ((mcause_next_ & ~(1u << kMcauseInterruptBit)) << 2));
		} else {
			mtvec_out.write(mtvec_base);
		}
	}

	void write_ff() {
		if (!rst_n.read()) {
			cycle_reg_.write(0);
			instret_reg_.write(0);
			mstatus_reg_.write(0);
			mie_reg_.write(0);
			mtvec_reg_.write(0);
			mepc_reg_.write(0);
			mip_reg_.write(0);
			mcause_reg_.write(0);
			mscratch_reg_.write(0);
		} else if (!stall.read()) {
			cycle_reg_.write(cycle_next_);
			instret_reg_.write(instret_next_);
			mstatus_reg_.write(mstatus_next_);
			mie_reg_.write(mie_next_);
			mtvec_reg_.write(mtvec_next_);
			mepc_reg_.write(mepc_next_);
			mip_reg_.write(mip_next_);
			mcause_reg_.write(mcause_next_);
			mscratch_reg_.write(mscratch_next_);
		}
	}

};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc