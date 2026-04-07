#pragma once

#include <systemc>
#include <cstdint>
#include "Core/rv32i_mcu/PipelineTypes.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

SC_MODULE(FetchStage) {
	sc_core::sc_in<bool> clk;
	sc_core::sc_in<bool> rst_n;

	sc_core::sc_out<bool> IM_stall;
	sc_core::sc_in<bool> stall;

	sc_core::sc_in<bool> EXE_bp_miss;
	sc_core::sc_in<bool> EXE_taken;
	sc_core::sc_in<sc_uint<kRvPcBit>> EXE_pc_target;
	sc_core::sc_in<sc_uint<kRvPcBit>> EXE_pc;
	sc_core::sc_in<bool> EXE_update;

	sc_core::sc_out<sc_uint<kRvPcBit>> IF_pc;
	sc_core::sc_out<sc_uint<kRvPcBit>> IF_pc_plus_4;
	sc_core::sc_out<sc_uint<kRvMemDataBitWidth>> IF_inst;
	sc_core::sc_out<bool> IF_inst_valid;

	sc_core::sc_out<bool> im_CEB;
	sc_core::sc_out<bool> im_WEB;
	sc_core::sc_out<sc_uint<4>> im_STRB;
	sc_core::sc_out<sc_uint<kRvMemAddrBitWidth>> im_A;
	sc_core::sc_out<sc_uint<kRvMemDataBitWidth>> im_DI;
	sc_core::sc_in<sc_uint<kRvMemDataBitWidth>> im_DO;
	sc_core::sc_in<bool> im_valid;

	sc_core::sc_signal<bool> memory_state_reg_;
	sc_core::sc_signal<bool> inst_read_done_reg_;
	sc_core::sc_signal<sc_uint<32>> if_inst_reg_;

	sc_uint<32> if_pc_next_ = 0;
	bool memory_state_next_ = false;
	bool inst_read_done_next_ = false;
	sc_uint<32> if_inst_next_ = 0;

	SC_CTOR(FetchStage) {
		SC_METHOD(compute_comb);
		sensitive << stall << EXE_bp_miss << EXE_taken << EXE_pc_target << EXE_pc << EXE_update
				  << IF_pc << memory_state_reg_ << inst_read_done_reg_ << if_inst_reg_
				  << im_DO << im_valid << rst_n;

		SC_METHOD(write_ff);
		sensitive << clk.pos();
		async_reset_signal_is(rst_n, false);
	}

	static uint32_t align_word(uint32_t value) {
		return value & ~0x3u;
	}

	void compute_comb() {
		const uint32_t if_pc = IF_pc.read().to_uint();
		const uint32_t if_pc_plus_4 = if_pc + 4u;
		const bool inst_read_done = inst_read_done_reg_.read();
		const bool im_stall = !(inst_read_done || im_valid.read());
		const bool local_stall = stall.read() || im_stall;

		const uint32_t if_predict_pc = if_pc_plus_4;
		const bool if_taken = false;

		IF_pc_plus_4.write(if_pc_plus_4);
		IF_inst_valid.write(!local_stall && !EXE_bp_miss.read());

		if_pc_next_ = local_stall ? if_pc
								  : (EXE_bp_miss.read() ? EXE_pc_target.read().to_uint()
														: (if_taken ? if_predict_pc : if_pc_plus_4));

		memory_state_next_ = memory_state_reg_.read();
		if (!memory_state_reg_.read()) {
			memory_state_next_ = inst_read_done ? false : true;
		} else {
			memory_state_next_ = im_valid.read() ? false : true;
		}

		inst_read_done_next_ = inst_read_done;
		if (im_valid.read() && stall.read()) {
			inst_read_done_next_ = true;
		} else if (!local_stall) {
			inst_read_done_next_ = false;
		}

		if_inst_next_ = im_valid.read() ? im_DO.read() : if_inst_reg_.read();
		IF_inst.write(if_inst_next_);

		im_CEB.write((!rst_n.read()) || inst_read_done || im_valid.read());
		im_WEB.write(true);
		im_STRB.write(0xFu);
		im_A.write(if_pc);
		im_DI.write(0u);
		IM_stall.write(im_stall);
	}

	void write_ff() {
		if (!rst_n.read()) {
			IF_pc.write(0u);
			memory_state_reg_.write(false);
			if_inst_reg_.write(0u);
			inst_read_done_reg_.write(false);
		} else {
			IF_pc.write(align_word(if_pc_next_));
			memory_state_reg_.write(memory_state_next_);
			if_inst_reg_.write(if_inst_next_);
			inst_read_done_reg_.write(inst_read_done_next_);
		}
	}
};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc
