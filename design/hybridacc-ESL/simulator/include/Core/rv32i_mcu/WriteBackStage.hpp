#pragma once

#include <systemc>
#include <cstdint>
#include "Core/rv32i_mcu/PipelineTypes.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

template <unsigned BITWIDTH = 32>
SC_MODULE(WriteBackStage) {
	sc_core::sc_in<bool> clk;
	sc_core::sc_in<bool> rst_n;

	sc_core::sc_in<bool> stall;

	sc_core::sc_in<sc_uint<BITWIDTH>> MEM_pc_plus_4;
	sc_core::sc_in<sc_uint<BITWIDTH>> MEM_exe_out;
	sc_core::sc_in<sc_uint<5>> MEM_rf_w_index;
	sc_core::sc_in<bool> MEM_irf_wen;
	sc_core::sc_in<bool> MEM_frf_wen;
	sc_core::sc_in<sc_uint<BITWIDTH>> MEM_read_data;
	sc_core::sc_in<sc_uint<3>> MEM_funct3;
	sc_core::sc_in<sc_uint<2>> MEM_wb_sel;
	sc_core::sc_in<bool> MEM_inst_valid;

	sc_core::sc_out<bool> WB_irf_wen;
	sc_core::sc_out<bool> WB_frf_wen;
	sc_core::sc_out<sc_uint<5>> WB_rf_w_index;
	sc_core::sc_out<sc_uint<BITWIDTH>> WB_rf_w_data;

	sc_core::sc_signal<sc_uint<BITWIDTH>> wb_pc_plus_4_reg_;
	sc_core::sc_signal<sc_uint<BITWIDTH>> wb_exe_out_reg_;
	sc_core::sc_signal<sc_uint<BITWIDTH>> wb_read_data_reg_;
	sc_core::sc_signal<sc_uint<3>> wb_funct3_reg_;
	sc_core::sc_signal<sc_uint<2>> wb_wb_sel_reg_;
	sc_core::sc_signal<sc_uint<5>> wb_rf_w_index_reg_;
	sc_core::sc_signal<bool> wb_irf_wen_reg_;
	sc_core::sc_signal<bool> wb_frf_wen_reg_;
	sc_core::sc_signal<bool> wb_inst_valid_reg_;
	sc_core::sc_signal<bool> first_cycle_reg_;

	SC_CTOR(WriteBackStage) {
		SC_METHOD(compute_comb);
		sensitive << stall << MEM_read_data << MEM_inst_valid
				  << wb_pc_plus_4_reg_ << wb_exe_out_reg_ << wb_read_data_reg_ << wb_funct3_reg_
				  << wb_wb_sel_reg_ << wb_rf_w_index_reg_ << wb_irf_wen_reg_ << wb_frf_wen_reg_ << wb_inst_valid_reg_ << first_cycle_reg_;

		SC_METHOD(write_ff);
		sensitive << clk.pos();
		async_reset_signal_is(rst_n, false);
	}

	void compute_comb() {
		const sc_uint<BITWIDTH> wb_read_data = (first_cycle_reg_.read() && MEM_inst_valid.read()) ? MEM_read_data.read() : wb_read_data_reg_.read();
		sc_uint<BITWIDTH> wb_read_data_masked = 0;

		WB_rf_w_index.write(wb_rf_w_index_reg_.read());
		WB_irf_wen.write(wb_irf_wen_reg_.read() && wb_inst_valid_reg_.read());
		WB_frf_wen.write(wb_frf_wen_reg_.read() && wb_inst_valid_reg_.read());

		switch (wb_funct3_reg_.read().to_uint()) {
			case kMemWidthByte:
				wb_read_data_masked = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(wb_read_data.range(7, 0).to_uint())));
				break;
			case kMemWidthHalf:
				wb_read_data_masked = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(wb_read_data.range(15, 0).to_uint())));
				break;
			case kMemWidthWord:
				wb_read_data_masked = wb_read_data;
				break;
			case kMemWidthUByte:
				wb_read_data_masked = wb_read_data.range(7, 0);
				break;
			case kMemWidthUHalf:
				wb_read_data_masked = wb_read_data.range(15, 0);
				break;
			default:
				wb_read_data_masked = 0u;
				break;
		}

		switch (wb_wb_sel_reg_.read().to_uint()) {
			case static_cast<uint32_t>(WriteBackSel::PC_PLUS_4):
				WB_rf_w_data.write(wb_pc_plus_4_reg_.read());
				break;
			case static_cast<uint32_t>(WriteBackSel::ALUOUT):
				WB_rf_w_data.write(wb_exe_out_reg_.read());
				break;
			case static_cast<uint32_t>(WriteBackSel::LD_DATA):
				WB_rf_w_data.write(wb_read_data_masked);
				break;
			default:
				WB_rf_w_data.write(0u);
				break;
		}
	}

	void write_ff() {
		if (!rst_n.read()) {
			wb_pc_plus_4_reg_.write(0u);
			wb_exe_out_reg_.write(0u);
			wb_rf_w_index_reg_.write(0u);
			wb_irf_wen_reg_.write(false);
			wb_frf_wen_reg_.write(false);
			wb_read_data_reg_.write(0u);
			wb_funct3_reg_.write(0u);
			wb_inst_valid_reg_.write(false);
			wb_wb_sel_reg_.write(0u);
			first_cycle_reg_.write(false);
		} else {
			wb_pc_plus_4_reg_.write(stall.read() ? wb_pc_plus_4_reg_.read() : MEM_pc_plus_4.read());
			wb_exe_out_reg_.write(stall.read() ? wb_exe_out_reg_.read() : MEM_exe_out.read());
			wb_rf_w_index_reg_.write(stall.read() ? wb_rf_w_index_reg_.read() : MEM_rf_w_index.read());
			wb_irf_wen_reg_.write(stall.read() ? wb_irf_wen_reg_.read() : MEM_irf_wen.read());
			wb_frf_wen_reg_.write(stall.read() ? wb_frf_wen_reg_.read() : MEM_frf_wen.read());
			wb_read_data_reg_.write(stall.read() ? wb_read_data_reg_.read() : MEM_read_data.read());
			wb_funct3_reg_.write(stall.read() ? wb_funct3_reg_.read() : MEM_funct3.read());
			wb_inst_valid_reg_.write(stall.read() ? wb_inst_valid_reg_.read() : MEM_inst_valid.read());
			wb_wb_sel_reg_.write(stall.read() ? wb_wb_sel_reg_.read() : MEM_wb_sel.read());
			first_cycle_reg_.write(!stall.read());
		}
	}
};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc
