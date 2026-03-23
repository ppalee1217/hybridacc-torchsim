#pragma once

#include <systemc>

#include <array>
#include <cstdint>
#include <string>

#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

enum class ExecState : uint8_t {
	FETCH_REQ = 0,
	FETCH_CAPTURE = 1,
	EXECUTE = 2,
	LS_WAIT = 3,
	CMD_REQ = 4,
	CMD_RESP = 5,
	WAIT_EVENT = 6,
	HALTED = 7,
	FAULT = 8,
};

inline std::ostream& operator<<(std::ostream& os, ExecState state) {
	switch (state) {
	case ExecState::FETCH_REQ: return os << "FETCH_REQ";
	case ExecState::FETCH_CAPTURE: return os << "FETCH_CAPTURE";
	case ExecState::EXECUTE: return os << "EXECUTE";
	case ExecState::LS_WAIT: return os << "LS_WAIT";
	case ExecState::CMD_REQ: return os << "CMD_REQ";
	case ExecState::CMD_RESP: return os << "CMD_RESP";
	case ExecState::WAIT_EVENT: return os << "WAIT_EVENT";
	case ExecState::HALTED: return os << "HALTED";
	case ExecState::FAULT: return os << "FAULT";
	default: return os << "UNKNOWN_EXEC_STATE";
	}
}

inline void sc_trace(sc_trace_file* tf, const ExecState& state, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(state), name);
}

enum class PendingOp : uint8_t {
	NONE = 0,
	LOAD_WORD = 1,
	LOAD_BYTE = 2,
	STORE_WORD = 3,
	STORE_BYTE = 4,
	PUSH = 5,
	POP = 6,
	CMD_WRITE = 7,
	CMD_READ = 8,
	STREAM = 9,
	WAIT_EVENT = 10,
	WFI = 11,
};

inline std::ostream& operator<<(std::ostream& os, PendingOp op) {
	switch (op) {
	case PendingOp::NONE: return os << "NONE";
	case PendingOp::LOAD_WORD: return os << "LOAD_WORD";
	case PendingOp::LOAD_BYTE: return os << "LOAD_BYTE";
	case PendingOp::STORE_WORD: return os << "STORE_WORD";
	case PendingOp::STORE_BYTE: return os << "STORE_BYTE";
	case PendingOp::PUSH: return os << "PUSH";
	case PendingOp::POP: return os << "POP";
	case PendingOp::CMD_WRITE: return os << "CMD_WRITE";
	case PendingOp::CMD_READ: return os << "CMD_READ";
	case PendingOp::STREAM: return os << "STREAM";
	case PendingOp::WAIT_EVENT: return os << "WAIT_EVENT";
	case PendingOp::WFI: return os << "WFI";
	default: return os << "UNKNOWN_PENDING_OP";
	}
}

inline void sc_trace(sc_trace_file* tf, const PendingOp& op, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(op), name);
}

template <unsigned DATA_SRAM_BYTES = 65536>
SC_MODULE(CoreMcu) {
public:
	static constexpr uint32_t kResetPc = kIsramBase;
	static constexpr uint32_t kResetSp = kDataSramBase + DATA_SRAM_BYTES;

	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_out<bool> if_req_valid_o{"if_req_valid_o"};
	sc_out<sc_uint<32>> if_addr_o{"if_addr_o"};
	sc_in<sc_uint<32>> if_rdata_i{"if_rdata_i"};

	sc_out<bool> ls_req_valid_o{"ls_req_valid_o"};
	sc_out<bool> ls_req_write_o{"ls_req_write_o"};
	sc_out<sc_uint<32>> ls_req_addr_o{"ls_req_addr_o"};
	sc_out<sc_uint<32>> ls_req_wdata_o{"ls_req_wdata_o"};
	sc_out<sc_uint<4>> ls_req_wstrb_o{"ls_req_wstrb_o"};
	sc_in<bool> ls_resp_valid_i{"ls_resp_valid_i"};
	sc_in<sc_uint<32>> ls_resp_rdata_i{"ls_resp_rdata_i"};

	sc_out<bool> cmd_req_valid_o{"cmd_req_valid_o"};
	sc_out<McuCmdReq> cmd_req_o{"cmd_req_o"};
	sc_in<bool> cmd_req_ready_i{"cmd_req_ready_i"};
	sc_in<bool> cmd_resp_valid_i{"cmd_resp_valid_i"};
	sc_in<McuCmdResp> cmd_resp_i{"cmd_resp_i"};

	sc_in<bool> irq_taken_i{"irq_taken_i"};
	sc_in<sc_uint<32>> irq_vector_i{"irq_vector_i"};
	sc_in<sc_uint<32>> irq_pending_lo_i{"irq_pending_lo_i"};
	sc_in<sc_uint<32>> irq_pending_hi_i{"irq_pending_hi_i"};
	sc_in<sc_uint<8>> irq_cause_id_i{"irq_cause_id_i"};
	sc_out<sc_uint<32>> irq_enable_lo_o{"irq_enable_lo_o"};
	sc_out<sc_uint<32>> irq_enable_hi_o{"irq_enable_hi_o"};
	sc_out<sc_uint<32>> irq_ack_lo_o{"irq_ack_lo_o"};
	sc_out<sc_uint<32>> irq_ack_hi_o{"irq_ack_hi_o"};

	sc_vector<sc_signal<sc_uint<32>>> gpr_reg{"gpr_reg", kCoreGprCount};
	sc_signal<sc_uint<32>> pc_reg{"pc_reg"};
	sc_signal<sc_uint<32>> sr_reg{"sr_reg"};
	sc_signal<sc_uint<32>> epc_reg{"epc_reg"};
	sc_signal<sc_uint<32>> esr_reg{"esr_reg"};
	sc_signal<sc_uint<32>> current_instr_reg{"current_instr_reg"};
	sc_signal<sc_uint<32>> fault_code_reg{"fault_code_reg"};
	sc_signal<sc_uint<32>> fault_aux_reg{"fault_aux_reg"};
	sc_signal<sc_uint<64>> cycle_count_reg{"cycle_count_reg"};
	sc_signal<sc_uint<64>> instret_count_reg{"instret_count_reg"};
	sc_signal<ExecState> exec_state_reg{"exec_state_reg"};
	sc_signal<PendingOp> pending_op_reg{"pending_op_reg"};
	sc_signal<sc_uint<4>> pending_rd_reg{"pending_rd_reg"};
	sc_signal<sc_uint<32>> pending_next_pc_reg{"pending_next_pc_reg"};
	sc_signal<sc_uint<32>> pending_addr_reg{"pending_addr_reg"};
	sc_signal<sc_uint<32>> pending_store_data_reg{"pending_store_data_reg"};
	sc_signal<sc_uint<4>> pending_wstrb_reg{"pending_wstrb_reg"};
	sc_signal<sc_uint<2>> pending_byte_lane_reg{"pending_byte_lane_reg"};
	sc_signal<McuCmdReq> pending_cmd_reg{"pending_cmd_reg"};
	sc_signal<sc_uint<32>> stream_addr_reg{"stream_addr_reg"};
	sc_signal<sc_uint<32>> stream_remaining_reg{"stream_remaining_reg"};
	sc_signal<StreamDestination> stream_dst_reg{"stream_dst_reg"};
	sc_signal<bool> stream_first_reg{"stream_first_reg"};
	sc_signal<sc_uint<32>> wait_mask_lo_reg{"wait_mask_lo_reg"};
	sc_signal<sc_uint<32>> wait_mask_hi_reg{"wait_mask_hi_reg"};
	sc_signal<bool> if_req_valid_reg{"if_req_valid_reg"};
	sc_signal<sc_uint<32>> if_addr_reg{"if_addr_reg"};
	sc_signal<bool> ls_req_valid_reg{"ls_req_valid_reg"};
	sc_signal<bool> ls_req_write_reg{"ls_req_write_reg"};
	sc_signal<sc_uint<32>> ls_req_addr_reg{"ls_req_addr_reg"};
	sc_signal<sc_uint<32>> ls_req_wdata_reg{"ls_req_wdata_reg"};
	sc_signal<sc_uint<4>> ls_req_wstrb_reg{"ls_req_wstrb_reg"};
	sc_signal<bool> cmd_req_valid_reg{"cmd_req_valid_reg"};
	sc_signal<McuCmdReq> cmd_req_reg{"cmd_req_reg"};
	sc_signal<sc_uint<32>> irq_enable_lo_reg{"irq_enable_lo_reg"};
	sc_signal<sc_uint<32>> irq_enable_hi_reg{"irq_enable_hi_reg"};
	sc_signal<sc_uint<32>> irq_ack_lo_reg{"irq_ack_lo_reg"};
	sc_signal<sc_uint<32>> irq_ack_hi_reg{"irq_ack_hi_reg"};
	sc_signal<bool> single_step_reg{"single_step_reg"};

	SC_CTOR(CoreMcu) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);

		SC_METHOD(comb_if_process);
		sensitive << if_req_valid_reg << if_addr_reg;

		SC_METHOD(comb_ls_process);
		sensitive << ls_req_valid_reg << ls_req_write_reg << ls_req_addr_reg << ls_req_wdata_reg << ls_req_wstrb_reg;

		SC_METHOD(comb_cmd_process);
		sensitive << cmd_req_valid_reg << cmd_req_reg;

		SC_METHOD(comb_irq_process);
		sensitive << irq_enable_lo_reg << irq_enable_hi_reg << irq_ack_lo_reg << irq_ack_hi_reg;
	}

	uint32_t debug_read_csr(uint32_t csr_id) const {
		return read_csr(csr_id);
	}

	uint32_t debug_read_pc() const {
		return pc_reg.read().to_uint();
	}

	bool debug_is_halted() const {
		return exec_state_reg.read() == ExecState::HALTED;
	}

	uint32_t debug_read_gpr(uint32_t index) const {
		return index < kCoreGprCount ? read_gpr(index) : 0u;
	}

private:
	void comb_if_process() {
		if_req_valid_o.write(if_req_valid_reg.read());
		if_addr_o.write(if_addr_reg.read());
	}

	void comb_ls_process() {
		ls_req_valid_o.write(ls_req_valid_reg.read());
		ls_req_write_o.write(ls_req_write_reg.read());
		ls_req_addr_o.write(ls_req_addr_reg.read());
		ls_req_wdata_o.write(ls_req_wdata_reg.read());
		ls_req_wstrb_o.write(ls_req_wstrb_reg.read());
	}

	void comb_cmd_process() {
		cmd_req_valid_o.write(cmd_req_valid_reg.read());
		cmd_req_o.write(cmd_req_reg.read());
	}

	void comb_irq_process() {
		irq_enable_lo_o.write(irq_enable_lo_reg.read());
		irq_enable_hi_o.write(irq_enable_hi_reg.read());
		irq_ack_lo_o.write(irq_ack_lo_reg.read());
		irq_ack_hi_o.write(irq_ack_hi_reg.read());
	}

	uint32_t read_gpr(unsigned index) const {
		return index == 0u ? 0u : gpr_reg[index].read().to_uint();
	}

	uint32_t reg_field_25_22(uint32_t instruction) const {
		return (instruction >> 22) & 0x0Fu;
	}

	uint32_t reg_field_21_18(uint32_t instruction) const {
		return (instruction >> 18) & 0x0Fu;
	}

	void write_gpr(unsigned index, uint32_t value) {
		if (index != 0u && index < kCoreGprCount) {
			gpr_reg[index].write(value);
		}
	}

	bool sr_bit(uint32_t bit_index) const {
		return ((sr_reg.read().to_uint() >> bit_index) & 0x1u) != 0u;
	}

	void write_sr_bit(uint32_t bit_index, bool value) {
		uint32_t sr_value = sr_reg.read().to_uint();
		if (value) {
			sr_value |= (1u << bit_index);
		} else {
			sr_value &= ~(1u << bit_index);
		}
		sr_reg.write(sr_value);
	}

	uint32_t update_sr_bit(uint32_t sr_value, uint32_t bit_index, bool value) const {
		if (value) {
			return sr_value | (1u << bit_index);
		}
		return sr_value & ~(1u << bit_index);
	}

	uint32_t nz_flags_value(uint32_t sr_value, uint32_t value) const {
		sr_value = update_sr_bit(sr_value, kSrZBit, value == 0u);
		sr_value = update_sr_bit(sr_value, kSrNBit, (value & 0x80000000u) != 0u);
		return sr_value;
	}

	uint32_t add_flags_value(uint32_t sr_value, uint32_t lhs, uint32_t rhs, uint32_t result) const {
		const uint64_t wide_sum = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs);
		sr_value = nz_flags_value(sr_value, result);
		sr_value = update_sr_bit(sr_value, kSrCBit, (wide_sum >> 32) != 0u);
		const bool overflow = ((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) != 0u;
		return update_sr_bit(sr_value, kSrVBit, overflow);
	}

	uint32_t sub_flags_value(uint32_t sr_value, uint32_t lhs, uint32_t rhs, uint32_t result) const {
		sr_value = nz_flags_value(sr_value, result);
		sr_value = update_sr_bit(sr_value, kSrCBit, lhs >= rhs);
		const bool overflow = (((lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) != 0u;
		return update_sr_bit(sr_value, kSrVBit, overflow);
	}

	uint32_t core_status_value() const {
		uint32_t value = 0u;
		const ExecState state = exec_state_reg.read();
		if (state != ExecState::HALTED && state != ExecState::FAULT) {
			value |= kCoreStatusRunningBit;
		}
		if (state == ExecState::HALTED) {
			value |= kCoreStatusHaltedBit;
		}
		if (state == ExecState::FAULT || sr_bit(kSrFaultBit)) {
			value |= kCoreStatusFaultBit;
		}
		if (sr_bit(kSrInIsrBit)) {
			value |= kCoreStatusInIsrBit;
		}
		return value;
	}

	uint32_t read_csr(uint32_t csr_id) const {
		switch (csr_id) {
		case kCsrCoreCtrl:
			return (single_step_reg.read() ? kCoreCtrlSingleStepBit : 0u);
		case kCsrCoreStatus:
			return core_status_value();
		case kCsrIrqPendingLo:
			return irq_pending_lo_i.read().to_uint();
		case kCsrIrqPendingHi:
			return irq_pending_hi_i.read().to_uint();
		case kCsrIrqEnableLo:
			return irq_enable_lo_reg.read().to_uint();
		case kCsrIrqEnableHi:
			return irq_enable_hi_reg.read().to_uint();
		case kCsrIrqCauseId:
			return irq_cause_id_i.read().to_uint();
		case kCsrEpc:
			return epc_reg.read().to_uint();
		case kCsrEsr:
			return esr_reg.read().to_uint();
		case kCsrFaultCode:
			return fault_code_reg.read().to_uint();
		case kCsrFaultAux:
			return fault_aux_reg.read().to_uint();
		case kCsrCycleCntLo:
			return static_cast<uint32_t>(cycle_count_reg.read().to_uint64() & 0xFFFFFFFFull);
		case kCsrCycleCntHi:
			return static_cast<uint32_t>((cycle_count_reg.read().to_uint64() >> 32) & 0xFFFFFFFFull);
		case kCsrInstretCntLo:
			return static_cast<uint32_t>(instret_count_reg.read().to_uint64() & 0xFFFFFFFFull);
		case kCsrInstretCntHi:
			return static_cast<uint32_t>((instret_count_reg.read().to_uint64() >> 32) & 0xFFFFFFFFull);
		default:
			return 0u;
		}
	}

	void apply_core_ctrl(uint32_t value) {
		if ((value & kCoreCtrlClrFaultBit) != 0u) {
			fault_code_reg.write(static_cast<uint32_t>(ErrorCode::NONE));
			fault_aux_reg.write(0u);
			write_sr_bit(kSrFaultBit, false);
			if (exec_state_reg.read() == ExecState::FAULT) {
				exec_state_reg.write(ExecState::FETCH_REQ);
			}
		}
		single_step_reg.write((value & kCoreCtrlSingleStepBit) != 0u);
		if ((value & kCoreCtrlHaltReqBit) != 0u) {
			exec_state_reg.write(ExecState::HALTED);
		}
		if ((value & kCoreCtrlRunBit) != 0u && exec_state_reg.read() == ExecState::HALTED && !sr_bit(kSrFaultBit)) {
			exec_state_reg.write(ExecState::FETCH_REQ);
		}
	}

	void write_csr(uint32_t csr_id, uint32_t value) {
		switch (csr_id) {
		case kCsrCoreCtrl:
			apply_core_ctrl(value);
			break;
		case kCsrIrqEnableLo:
			irq_enable_lo_reg.write(value);
			break;
		case kCsrIrqEnableHi:
			irq_enable_hi_reg.write(value);
			break;
		case kCsrIrqAckLo:
			irq_ack_lo_reg.write(value);
			break;
		case kCsrIrqAckHi:
			irq_ack_hi_reg.write(value);
			break;
		case kCsrEpc:
			epc_reg.write(value);
			break;
		case kCsrEsr:
			esr_reg.write(value);
			break;
		default:
			break;
		}
	}

	void set_fault(ErrorCode code, uint32_t aux) {
		fault_code_reg.write(static_cast<uint32_t>(code));
		fault_aux_reg.write(aux);
		write_sr_bit(kSrFaultBit, true);
		exec_state_reg.write(ExecState::FAULT);
		if_req_valid_reg.write(false);
		ls_req_valid_reg.write(false);
		cmd_req_valid_reg.write(false);
	}

	void retire_to(uint32_t next_pc) {
		pc_reg.write(next_pc);
		instret_count_reg.write(instret_count_reg.read().to_uint64() + 1ull);
		pending_op_reg.write(PendingOp::NONE);
		if (single_step_reg.read()) {
			exec_state_reg.write(ExecState::HALTED);
		} else {
			exec_state_reg.write(ExecState::FETCH_REQ);
		}
	}

	bool take_interrupt_if_possible() {
		if (!irq_taken_i.read() || !sr_bit(kSrIrqEnableBit) || sr_bit(kSrInIsrBit)) {
			return false;
		}
		epc_reg.write(pc_reg.read());
		esr_reg.write(sr_reg.read());
		uint32_t sr_value = sr_reg.read().to_uint();
		sr_value &= ~(1u << kSrIrqEnableBit);
		sr_value |= (1u << kSrInIsrBit);
		sr_reg.write(sr_value);
		pc_reg.write(irq_vector_i.read());
		exec_state_reg.write(ExecState::FETCH_REQ);
		if_req_valid_reg.write(false);
		ls_req_valid_reg.write(false);
		cmd_req_valid_reg.write(false);
		return true;
	}

	void start_load(PendingOp op, uint32_t addr, uint32_t wdata, uint32_t wstrb, uint32_t next_pc, uint8_t rd, uint8_t lane) {
		pending_op_reg.write(op);
		pending_addr_reg.write(addr);
		pending_store_data_reg.write(wdata);
		pending_wstrb_reg.write(wstrb & 0xFu);
		pending_next_pc_reg.write(next_pc);
		pending_rd_reg.write(rd & 0xFu);
		pending_byte_lane_reg.write(lane & 0x3u);
		ls_req_addr_reg.write(addr);
		ls_req_wdata_reg.write(wdata);
		ls_req_wstrb_reg.write(wstrb & 0xFu);
		ls_req_write_reg.write(op == PendingOp::STORE_WORD || op == PendingOp::STORE_BYTE || op == PendingOp::PUSH);
		ls_req_valid_reg.write(true);
		exec_state_reg.write(ExecState::LS_WAIT);
	}

	void start_command(PendingOp op, const McuCmdReq& req, uint32_t next_pc, uint8_t rd) {
		pending_op_reg.write(op);
		pending_cmd_reg.write(req);
		pending_next_pc_reg.write(next_pc);
		pending_rd_reg.write(rd & 0xFu);
		cmd_req_reg.write(req);
		cmd_req_valid_reg.write(true);
		exec_state_reg.write(ExecState::CMD_REQ);
	}

	void handle_ls_response() {
		const PendingOp pending_op = pending_op_reg.read();
		const uint32_t next_pc = pending_next_pc_reg.read().to_uint();
		ls_req_valid_reg.write(false);
		switch (pending_op) {
		case PendingOp::LOAD_WORD:
		case PendingOp::POP:
			write_gpr(pending_rd_reg.read().to_uint(), ls_resp_rdata_i.read().to_uint());
			if (pending_op == PendingOp::POP) {
				write_gpr(13u, pending_addr_reg.read().to_uint() + 4u);
			}
			retire_to(next_pc);
			break;
		case PendingOp::LOAD_BYTE: {
			const uint32_t shift = pending_byte_lane_reg.read().to_uint() * 8u;
			const uint32_t value = (ls_resp_rdata_i.read().to_uint() >> shift) & 0xFFu;
			write_gpr(pending_rd_reg.read().to_uint(), value);
			retire_to(next_pc);
			break;
		}
		case PendingOp::STORE_WORD:
		case PendingOp::STORE_BYTE:
		case PendingOp::PUSH:
			if (pending_op == PendingOp::PUSH) {
				write_gpr(13u, pending_addr_reg.read().to_uint());
			}
			retire_to(next_pc);
			break;
		case PendingOp::STREAM: {
			McuCmdReq req{};
			req.kind = CommandKind::STREAM_WORD;
			req.stream_dst = stream_dst_reg.read();
			req.stream_flags = 0u;
			req.target_mask = 0u;
			req.addr = 0u;
			req.data = ls_resp_rdata_i.read().to_uint();
			req.word_count = 1u;
			const uint32_t remaining = stream_remaining_reg.read().to_uint();
			if (stream_first_reg.read()) {
				req.stream_flags |= 0x2u;
			}
			if (remaining == 1u) {
				req.stream_flags |= 0x1u | 0x4u;
			}
			start_command(PendingOp::STREAM, req, next_pc, 0u);
			break;
		}
		default:
			set_fault(ErrorCode::LOCAL_MEMORY_BOUNDS_FAULT, pending_addr_reg.read().to_uint());
			break;
		}
	}

	void handle_cmd_response() {
		const McuCmdResp resp = cmd_resp_i.read();
		const PendingOp pending_op = pending_op_reg.read();
		const uint32_t next_pc = pending_next_pc_reg.read().to_uint();
		cmd_req_valid_reg.write(false);
		if (resp.error) {
			set_fault(resp.error_target_id != 0u ? ErrorCode::BROADCAST_TARGET_FAULT : ErrorCode::COMMAND_ERROR,
				(static_cast<uint32_t>(resp.error_target_id) << 16) | (resp.aux & 0xFFFFu));
			return;
		}
		switch (pending_op) {
		case PendingOp::CMD_READ:
			write_gpr(pending_rd_reg.read().to_uint(), resp.rdata);
			retire_to(next_pc);
			break;
		case PendingOp::CMD_WRITE:
			retire_to(next_pc);
			break;
		case PendingOp::STREAM: {
			const uint32_t remaining = stream_remaining_reg.read().to_uint();
			if (remaining <= 1u) {
				retire_to(next_pc);
			} else {
				stream_remaining_reg.write(remaining - 1u);
				stream_addr_reg.write(stream_addr_reg.read().to_uint() + 4u);
				stream_first_reg.write(false);
				start_load(PendingOp::STREAM, stream_addr_reg.read().to_uint() + 4u, 0u, 0u, next_pc, 0u, 0u);
			}
			break;
		}
		default:
			retire_to(next_pc);
			break;
		}
	}

	uint32_t imm_high_value(int32_t imm18) const {
		return (static_cast<uint32_t>(imm18) & 0x3FFFFu) << 14;
	}

	bool check_word_alignment(uint32_t addr) {
		if ((addr & 0x3u) != 0u) {
			set_fault(ErrorCode::UNALIGNED_WORD_ACCESS, addr);
			return false;
		}
		return true;
	}

	void seq_process() {
		for (unsigned index = 0; index < kCoreGprCount; ++index) {
			gpr_reg[index].write(0u);
		}
		pc_reg.write(kResetPc);
		sr_reg.write(0u);
		epc_reg.write(0u);
		esr_reg.write(0u);
		current_instr_reg.write(0u);
		fault_code_reg.write(static_cast<uint32_t>(ErrorCode::NONE));
		fault_aux_reg.write(0u);
		cycle_count_reg.write(0ull);
		instret_count_reg.write(0ull);
		exec_state_reg.write(ExecState::FETCH_REQ);
		pending_op_reg.write(PendingOp::NONE);
		pending_rd_reg.write(0u);
		pending_next_pc_reg.write(0u);
		pending_addr_reg.write(0u);
		pending_store_data_reg.write(0u);
		pending_wstrb_reg.write(0u);
		pending_byte_lane_reg.write(0u);
		pending_cmd_reg.write(McuCmdReq{});
		stream_addr_reg.write(0u);
		stream_remaining_reg.write(0u);
		stream_dst_reg.write(StreamDestination::DMA);
		stream_first_reg.write(false);
		wait_mask_lo_reg.write(0u);
		wait_mask_hi_reg.write(0u);
		if_req_valid_reg.write(false);
		if_addr_reg.write(kResetPc);
		ls_req_valid_reg.write(false);
		ls_req_write_reg.write(false);
		ls_req_addr_reg.write(0u);
		ls_req_wdata_reg.write(0u);
		ls_req_wstrb_reg.write(0u);
		cmd_req_valid_reg.write(false);
		cmd_req_reg.write(McuCmdReq{});
		irq_enable_lo_reg.write(0u);
		irq_enable_hi_reg.write(0u);
		irq_ack_lo_reg.write(0u);
		irq_ack_hi_reg.write(0u);
		single_step_reg.write(false);
		write_gpr(13u, kResetSp);
		wait();

		while (true) {
			cycle_count_reg.write(cycle_count_reg.read().to_uint64() + 1ull);
			irq_ack_lo_reg.write(0u);
			irq_ack_hi_reg.write(0u);

			switch (exec_state_reg.read()) {
			case ExecState::FETCH_REQ:
				if (take_interrupt_if_possible()) {
					break;
				}
				if_req_valid_reg.write(true);
				if_addr_reg.write(pc_reg.read());
				exec_state_reg.write(ExecState::FETCH_CAPTURE);
				break;

			case ExecState::FETCH_CAPTURE:
				if_req_valid_reg.write(false);
				current_instr_reg.write(if_rdata_i.read());
				exec_state_reg.write(ExecState::EXECUTE);
				break;

			case ExecState::EXECUTE: {
				const uint32_t instruction = current_instr_reg.read().to_uint();
				const uint8_t opcode = opcode_of(instruction);
				const uint8_t rd = rd_of(instruction);
				const uint8_t rs1 = rs1_of(instruction);
				const uint8_t rs2 = rs2_of(instruction);
				const uint8_t format_s_rs2 = static_cast<uint8_t>(reg_field_25_22(instruction));
				const uint8_t format_s_rs1 = static_cast<uint8_t>(reg_field_21_18(instruction));
				const uint8_t format_b_rs1 = static_cast<uint8_t>(reg_field_25_22(instruction));
				const uint8_t format_b_rs2 = static_cast<uint8_t>(reg_field_21_18(instruction));
				const int32_t imm18 = imm18_of(instruction);
				const uint32_t pc = pc_reg.read().to_uint();
				const uint32_t next_pc = pc + 4u;
				const uint32_t rs1_value = read_gpr(rs1);
				const uint32_t rs2_value = read_gpr(rs2);
				const uint32_t format_s_rs1_value = read_gpr(format_s_rs1);
				const uint32_t format_s_rs2_value = read_gpr(format_s_rs2);
				const uint32_t format_b_rs1_value = read_gpr(format_b_rs1);
				const uint32_t format_b_rs2_value = read_gpr(format_b_rs2);
				const uint32_t sr_value = sr_reg.read().to_uint();

				switch (static_cast<Opcode>(opcode)) {
				case Opcode::NOP:
					retire_to(next_pc);
					break;
				case Opcode::MOVI:
					write_gpr(rd, static_cast<uint32_t>(imm18));
					sr_reg.write(nz_flags_value(sr_value, static_cast<uint32_t>(imm18)));
					retire_to(next_pc);
					break;
				case Opcode::MOVHI:
					write_gpr(rd, imm_high_value(imm18));
					sr_reg.write(nz_flags_value(sr_value, imm_high_value(imm18)));
					retire_to(next_pc);
					break;
				case Opcode::MOV:
					write_gpr(rd, rs1_value);
					sr_reg.write(nz_flags_value(sr_value, rs1_value));
					retire_to(next_pc);
					break;
				case Opcode::ADD: {
					const uint32_t result = rs1_value + rs2_value;
					write_gpr(rd, result);
					sr_reg.write(add_flags_value(sr_value, rs1_value, rs2_value, result));
					retire_to(next_pc);
					break;
				}
				case Opcode::ADDI: {
					const uint32_t rhs = static_cast<uint32_t>(imm18);
					const uint32_t result = rs1_value + rhs;
					write_gpr(rd, result);
					sr_reg.write(add_flags_value(sr_value, rs1_value, rhs, result));
					retire_to(next_pc);
					break;
				}
				case Opcode::SUB: {
					const uint32_t result = rs1_value - rs2_value;
					write_gpr(rd, result);
					sr_reg.write(sub_flags_value(sr_value, rs1_value, rs2_value, result));
					retire_to(next_pc);
					break;
				}
				case Opcode::AND:
					write_gpr(rd, rs1_value & rs2_value);
					sr_reg.write(nz_flags_value(sr_value, rs1_value & rs2_value));
					retire_to(next_pc);
					break;
				case Opcode::OR:
					write_gpr(rd, rs1_value | rs2_value);
					sr_reg.write(nz_flags_value(sr_value, rs1_value | rs2_value));
					retire_to(next_pc);
					break;
				case Opcode::XOR:
					write_gpr(rd, rs1_value ^ rs2_value);
					sr_reg.write(nz_flags_value(sr_value, rs1_value ^ rs2_value));
					retire_to(next_pc);
					break;
				case Opcode::SHL: {
					const uint32_t result = rs1_value << (imm18 & 0x1F);
					write_gpr(rd, result);
					sr_reg.write(nz_flags_value(sr_value, result));
					retire_to(next_pc);
					break;
				}
				case Opcode::SHR: {
					const uint32_t result = rs1_value >> (imm18 & 0x1F);
					write_gpr(rd, result);
					sr_reg.write(nz_flags_value(sr_value, result));
					retire_to(next_pc);
					break;
				}
				case Opcode::CMP: {
					const uint32_t result = rs1_value - rs2_value;
					sr_reg.write(sub_flags_value(sr_value, rs1_value, rs2_value, result));
					retire_to(next_pc);
					break;
				}
				case Opcode::CMPI: {
					const uint32_t rhs = static_cast<uint32_t>(imm18);
					const uint32_t result = rs1_value - rhs;
					sr_reg.write(sub_flags_value(sr_value, rs1_value, rhs, result));
					retire_to(next_pc);
					break;
				}
				case Opcode::B:
					retire_to(pc + branch_offset_bytes26(instruction));
					break;
				case Opcode::BEQ:
					retire_to(format_b_rs1_value == format_b_rs2_value ? pc + branch_offset_bytes18(instruction) : next_pc);
					break;
				case Opcode::BNE:
					retire_to(format_b_rs1_value != format_b_rs2_value ? pc + branch_offset_bytes18(instruction) : next_pc);
					break;
				case Opcode::BLT:
					retire_to(static_cast<int32_t>(format_b_rs1_value) < static_cast<int32_t>(format_b_rs2_value) ? pc + branch_offset_bytes18(instruction) : next_pc);
					break;
				case Opcode::BGE:
					retire_to(static_cast<int32_t>(format_b_rs1_value) >= static_cast<int32_t>(format_b_rs2_value) ? pc + branch_offset_bytes18(instruction) : next_pc);
					break;
				case Opcode::CALL:
					write_gpr(14u, next_pc);
					retire_to(pc + branch_offset_bytes26(instruction));
					break;
				case Opcode::CALLR:
					write_gpr(14u, next_pc);
					retire_to(rs1_value);
					break;
				case Opcode::RET:
					retire_to(read_gpr(14u));
					break;
				case Opcode::HLT:
					exec_state_reg.write(ExecState::HALTED);
					instret_count_reg.write(instret_count_reg.read().to_uint64() + 1ull);
					break;
				case Opcode::LDW: {
					const uint32_t addr = rs1_value + static_cast<uint32_t>(imm18);
					if (check_word_alignment(addr)) {
						start_load(PendingOp::LOAD_WORD, addr, 0u, 0u, next_pc, rd, 0u);
					}
					break;
				}
				case Opcode::STW: {
					const uint32_t addr = format_s_rs1_value + static_cast<uint32_t>(imm18);
					if (check_word_alignment(addr)) {
						start_load(PendingOp::STORE_WORD, addr, format_s_rs2_value, 0xFu, next_pc, 0u, 0u);
					}
					break;
				}
				case Opcode::LDB: {
					const uint32_t addr = rs1_value + static_cast<uint32_t>(imm18);
					start_load(PendingOp::LOAD_BYTE, addr & ~0x3u, 0u, 0u, next_pc, rd, static_cast<uint8_t>(addr & 0x3u));
					break;
				}
				case Opcode::STB: {
					const uint32_t addr = format_s_rs1_value + static_cast<uint32_t>(imm18);
					const uint32_t shift = (addr & 0x3u) * 8u;
					start_load(PendingOp::STORE_BYTE, addr & ~0x3u, (format_s_rs2_value & 0xFFu) << shift, 1u << (addr & 0x3u), next_pc, 0u, static_cast<uint8_t>(addr & 0x3u));
					break;
				}
				case Opcode::PUSH: {
					const uint32_t addr = read_gpr(13u) - 4u;
					if (check_word_alignment(addr)) {
						start_load(PendingOp::PUSH, addr, rs1_value, 0xFu, next_pc, 0u, 0u);
					}
					break;
				}
				case Opcode::POP: {
					const uint32_t addr = read_gpr(13u);
					if (check_word_alignment(addr)) {
						start_load(PendingOp::POP, addr, 0u, 0u, next_pc, rd, 0u);
					}
					break;
				}
				case Opcode::CSRRD:
					write_gpr(rd, read_csr(static_cast<uint32_t>(imm18)));
					retire_to(next_pc);
					break;
				case Opcode::CSRWR:
					write_csr(static_cast<uint32_t>(imm18), rs1_value);
					retire_to(next_pc);
					break;
				case Opcode::CSRSI: {
					const uint32_t csr_id = func14_of(instruction) & 0x3FFFu;
					write_csr(csr_id, read_csr(csr_id) | rs1_value);
					retire_to(next_pc);
					break;
				}
				case Opcode::CSRCL: {
					const uint32_t csr_id = func14_of(instruction) & 0x3FFFu;
					write_csr(csr_id, read_csr(csr_id) & ~rs1_value);
					retire_to(next_pc);
					break;
				}
				case Opcode::MMIOW: {
					McuCmdReq req{};
					req.kind = CommandKind::MMIO_WRITE;
					req.addr = rs1_value + static_cast<uint32_t>(imm18);
					req.data = read_gpr(rd);
					start_command(PendingOp::CMD_WRITE, req, next_pc, 0u);
					break;
				}
				case Opcode::MMIOR: {
					McuCmdReq req{};
					req.kind = CommandKind::MMIO_READ;
					req.addr = rs1_value + static_cast<uint32_t>(imm18);
					start_command(PendingOp::CMD_READ, req, next_pc, rd);
					break;
				}
				case Opcode::MMIOWB: {
					McuCmdReq req{};
					req.kind = CommandKind::MMIO_WRITE_BROADCAST;
					req.addr = rs1_value + (func14_of(instruction) & 0x3FFFu);
					req.data = read_gpr(rs2);
					req.target_mask = read_gpr(rd);
					start_command(PendingOp::CMD_WRITE, req, next_pc, 0u);
					break;
				}
				case Opcode::MMIORD: {
					McuCmdReq req{};
					req.kind = CommandKind::MMIO_READ_BROADCAST;
					req.addr = rs1_value + (func14_of(instruction) & 0x3FFFu);
					req.target_mask = read_gpr(rd);
					start_command(PendingOp::CMD_READ, req, next_pc, rs2);
					break;
				}
				case Opcode::STRM: {
					const uint32_t count = read_gpr(rs2);
					if (count == 0u) {
						retire_to(next_pc);
					} else {
						stream_addr_reg.write(rs1_value);
						stream_remaining_reg.write(count);
						stream_dst_reg.write(static_cast<StreamDestination>(rd & 0x3u));
						stream_first_reg.write(true);
						start_load(PendingOp::STREAM, rs1_value, 0u, 0u, next_pc, 0u, 0u);
					}
					break;
				}
				case Opcode::STRMI: {
					McuCmdReq req{};
					req.kind = CommandKind::STREAM_WORD;
					req.stream_dst = static_cast<StreamDestination>(rd & 0x3u);
					req.data = rs1_value;
					req.word_count = 1u;
					start_command(PendingOp::CMD_WRITE, req, next_pc, 0u);
					break;
				}
				case Opcode::STRMC: {
					McuCmdReq req{};
					req.kind = CommandKind::STREAM_CTRL;
					req.stream_dst = static_cast<StreamDestination>(rd & 0x3u);
					req.stream_flags = static_cast<uint8_t>(func14_of(instruction) & 0x3Fu);
					start_command(PendingOp::CMD_WRITE, req, next_pc, 0u);
					break;
				}
				case Opcode::WFI:
					pending_op_reg.write(PendingOp::WFI);
					pending_next_pc_reg.write(next_pc);
					wait_mask_lo_reg.write(rs1_value);
					wait_mask_hi_reg.write(0u);
					exec_state_reg.write(ExecState::WAIT_EVENT);
					break;
				case Opcode::WAIT:
					pending_op_reg.write(PendingOp::WAIT_EVENT);
					pending_next_pc_reg.write(next_pc);
					pending_rd_reg.write(rd);
					wait_mask_lo_reg.write(rs1_value);
					wait_mask_hi_reg.write(0u);
					exec_state_reg.write(ExecState::WAIT_EVENT);
					break;
				case Opcode::ACKIRQ:
					irq_ack_lo_reg.write(rs1_value);
					retire_to(next_pc);
					break;
				case Opcode::EI:
					write_sr_bit(kSrIrqEnableBit, true);
					retire_to(next_pc);
					break;
				case Opcode::DI:
					write_sr_bit(kSrIrqEnableBit, false);
					retire_to(next_pc);
					break;
				case Opcode::IRET:
					if (!sr_bit(kSrInIsrBit)) {
						set_fault(ErrorCode::IRET_OUTSIDE_ISR, instruction);
					} else {
						sr_reg.write(esr_reg.read());
						retire_to(epc_reg.read().to_uint());
					}
					break;
				default:
					set_fault(ErrorCode::ILLEGAL_OPCODE, instruction);
					break;
				}
				break;
			}

			case ExecState::LS_WAIT:
				if (ls_resp_valid_i.read()) {
					handle_ls_response();
				}
				break;

			case ExecState::CMD_REQ:
				if (cmd_req_ready_i.read()) {
					cmd_req_valid_reg.write(false);
					exec_state_reg.write(ExecState::CMD_RESP);
				}
				break;

			case ExecState::CMD_RESP:
				if (cmd_resp_valid_i.read()) {
					handle_cmd_response();
				}
				break;

			case ExecState::WAIT_EVENT: {
				const uint32_t pending_lo = irq_pending_lo_i.read().to_uint();
				const uint32_t pending_hi = irq_pending_hi_i.read().to_uint();
				const uint32_t mask_lo = wait_mask_lo_reg.read().to_uint();
				const uint32_t mask_hi = wait_mask_hi_reg.read().to_uint();
				const bool any_event = ((mask_lo == 0u || (pending_lo & mask_lo) != 0u) && (mask_hi == 0u || (pending_hi & mask_hi) != 0u)) || irq_taken_i.read();
				if (pending_op_reg.read() == PendingOp::WFI) {
					if (take_interrupt_if_possible()) {
						break;
					}
					if (any_event) {
						retire_to(pending_next_pc_reg.read().to_uint());
					}
				} else if (pending_op_reg.read() == PendingOp::WAIT_EVENT && any_event) {
					write_gpr(pending_rd_reg.read().to_uint(), 1u);
					retire_to(pending_next_pc_reg.read().to_uint());
				}
				break;
			}

			case ExecState::HALTED:
				if (irq_taken_i.read() && sr_bit(kSrIrqEnableBit)) {
					take_interrupt_if_possible();
				}
				break;

			case ExecState::FAULT:
				break;
			}

			gpr_reg[0].write(0u);
			wait();
		}
	}
};

} // namespace hybridacc::core