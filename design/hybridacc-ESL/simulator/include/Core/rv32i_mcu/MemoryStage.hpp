#pragma once

#include <systemc>
#include <cstdint>
#include "Core/rv32i_mcu/PipelineTypes.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

template <unsigned BITWIDTH = 32>
SC_MODULE(MemoryStage) {
	sc_core::sc_in<bool> clk;
	sc_core::sc_in<bool> rst_n;

	sc_core::sc_out<bool> DM_stall;
	sc_core::sc_in<bool> stall;

	sc_core::sc_in<sc_uint<BITWIDTH>> EXE_pc_plus_4;
	sc_core::sc_in<bool> EXE_mem_R;
	sc_core::sc_in<bool> EXE_mem_W;
	sc_core::sc_in<sc_uint<5>> EXE_rf_w_index;
	sc_core::sc_in<bool> EXE_irf_wen;
	sc_core::sc_in<bool> EXE_frf_wen;
	sc_core::sc_in<sc_uint<3>> EXE_funct3;
	sc_core::sc_in<sc_uint<2>> EXE_wb_sel;
	sc_core::sc_in<sc_uint<BITWIDTH>> EXE_store_data;
	sc_core::sc_in<sc_uint<BITWIDTH>> EXE_exe_out;
	sc_core::sc_in<bool> EXE_inst_valid;

	sc_core::sc_out<sc_uint<BITWIDTH>> MEM_pc_plus_4;
	sc_core::sc_out<sc_uint<BITWIDTH>> MEM_rf_w_data;
	sc_core::sc_out<sc_uint<BITWIDTH>> MEM_exe_out;
	sc_core::sc_out<sc_uint<5>> MEM_rf_w_index;
	sc_core::sc_out<bool> MEM_irf_wen;
	sc_core::sc_out<bool> MEM_frf_wen;
	sc_core::sc_out<sc_uint<BITWIDTH>> MEM_read_data;
	sc_core::sc_out<sc_uint<3>> MEM_funct3;
	sc_core::sc_out<sc_uint<2>> MEM_wb_sel;
	sc_core::sc_out<bool> MEM_inst_valid;

	sc_core::sc_out<bool> dm_CEB;
	sc_core::sc_out<bool> dm_WEB;
	sc_core::sc_out<sc_uint<4>> dm_STRB;
	sc_core::sc_out<sc_uint<kRvMemAddrBitWidth>> dm_A;
	sc_core::sc_out<sc_uint<kRvMemDataBitWidth>> dm_DI;
	sc_core::sc_in<sc_uint<kRvMemDataBitWidth>> dm_DO;
	sc_core::sc_in<bool> dm_valid;

	sc_core::sc_signal<bool> memory_state_reg_;
	sc_core::sc_signal<bool> mem_mem_r_reg_;
	sc_core::sc_signal<bool> mem_mem_w_reg_;
	sc_core::sc_signal<sc_uint<BITWIDTH>> mem_pc_plus_4_reg_;
	sc_core::sc_signal<sc_uint<BITWIDTH>> mem_store_data_reg_;
	sc_core::sc_signal<sc_uint<BITWIDTH>> mem_exe_out_reg_;
	sc_core::sc_signal<sc_uint<BITWIDTH>> mem_read_data_reg_;
	sc_core::sc_signal<sc_uint<5>> mem_rf_w_index_reg_;
	sc_core::sc_signal<bool> mem_irf_wen_reg_;
	sc_core::sc_signal<bool> mem_frf_wen_reg_;
	sc_core::sc_signal<sc_uint<3>> mem_funct3_reg_;
	sc_core::sc_signal<sc_uint<2>> mem_wb_sel_reg_;
	sc_core::sc_signal<bool> mem_inst_valid_reg_;
	sc_core::sc_signal<bool> mem_access_done_reg_;

	bool memory_state_next_ = false;
	bool mem_access_done_next_ = false;
	sc_uint<BITWIDTH> mem_read_data_next_ = 0;

	SC_CTOR(MemoryStage) {
		SC_METHOD(compute_comb);
		sensitive << stall << EXE_pc_plus_4 << EXE_mem_R << EXE_mem_W << EXE_rf_w_index << EXE_irf_wen << EXE_frf_wen
				  << EXE_funct3 << EXE_wb_sel << EXE_store_data << EXE_exe_out << EXE_inst_valid
				  << memory_state_reg_ << mem_mem_r_reg_ << mem_mem_w_reg_ << mem_pc_plus_4_reg_ << mem_store_data_reg_
				  << mem_exe_out_reg_ << mem_read_data_reg_ << mem_rf_w_index_reg_ << mem_irf_wen_reg_ << mem_frf_wen_reg_
				  << mem_funct3_reg_ << mem_wb_sel_reg_ << mem_inst_valid_reg_ << mem_access_done_reg_
				  << dm_DO << dm_valid;

		SC_METHOD(write_ff);
		sensitive << clk.pos();
		async_reset_signal_is(rst_n, false);
	}

	void compute_comb() {
		// ── Compute DM_stall FIRST so that stall_local uses the
		//    freshly-evaluated value instead of the stale port value.
		//    (Reading DM_stall.read() would return the previous-delta
		//     value, causing MEM_inst_valid to stay false for one
		//     extra delta after dm_valid arrives.)
		bool dm_stall = false;
		if (!memory_state_reg_.read()) {
			dm_stall = (mem_mem_r_reg_.read() || mem_mem_w_reg_.read()) && !(mem_access_done_reg_.read() || dm_valid.read()) && mem_inst_valid_reg_.read();
		} else {
			dm_stall = !(mem_access_done_reg_.read() || dm_valid.read());
		}
		DM_stall.write(dm_stall);

		const bool stall_local = stall.read() || dm_stall;
		MEM_inst_valid.write(mem_inst_valid_reg_.read() && !stall_local);

		MEM_pc_plus_4.write(mem_pc_plus_4_reg_.read());
		MEM_exe_out.write(mem_exe_out_reg_.read());
		MEM_rf_w_index.write(mem_rf_w_index_reg_.read());
		MEM_irf_wen.write(mem_irf_wen_reg_.read());
		MEM_frf_wen.write(mem_frf_wen_reg_.read());
		MEM_funct3.write(mem_funct3_reg_.read());
		MEM_wb_sel.write(mem_wb_sel_reg_.read());
		MEM_rf_w_data.write(mem_wb_sel_reg_.read() == static_cast<uint32_t>(WriteBackSel::PC_PLUS_4)
			? mem_pc_plus_4_reg_.read()
			: mem_exe_out_reg_.read());

		memory_state_next_ = memory_state_reg_.read();
		if (!memory_state_reg_.read()) {
			memory_state_next_ = ((mem_mem_r_reg_.read() || mem_mem_w_reg_.read()) && !mem_access_done_reg_.read() && mem_inst_valid_reg_.read())
				? true
				: false;
		} else {
			memory_state_next_ = dm_valid.read() ? false : true;
		}

		if (dm_valid.read() && stall.read()) {
			mem_access_done_next_ = true;
		} else if (!stall_local) {
			mem_access_done_next_ = false;
		} else {
			mem_access_done_next_ = mem_access_done_reg_.read();
		}

		uint32_t dm_do_aligned = dm_DO.read().to_uint();
		switch (mem_funct3_reg_.read().to_uint()) {
			case kMemWidthByte:
				dm_do_aligned = (mem_exe_out_reg_.read().range(1, 0) == 0) ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(dm_DO.read().range(7, 0).to_uint())))
					: (mem_exe_out_reg_.read().range(1, 0) == 1) ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(dm_DO.read().range(15, 8).to_uint())))
					: (mem_exe_out_reg_.read().range(1, 0) == 2) ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(dm_DO.read().range(23, 16).to_uint())))
					: static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(dm_DO.read().range(31, 24).to_uint())));
				break;
			case kMemWidthHalf:
				dm_do_aligned = mem_exe_out_reg_.read()[1]
					? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(dm_DO.read().range(31, 16).to_uint())))
					: static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(dm_DO.read().range(15, 0).to_uint())));
				break;
			case kMemWidthWord:
				dm_do_aligned = dm_DO.read().to_uint();
				break;
			case kMemWidthUByte:
				dm_do_aligned = (mem_exe_out_reg_.read().range(1, 0) == 0) ? dm_DO.read().range(7, 0).to_uint()
					: (mem_exe_out_reg_.read().range(1, 0) == 1) ? dm_DO.read().range(15, 8).to_uint()
					: (mem_exe_out_reg_.read().range(1, 0) == 2) ? dm_DO.read().range(23, 16).to_uint()
					: dm_DO.read().range(31, 24).to_uint();
				break;
			case kMemWidthUHalf:
				dm_do_aligned = mem_exe_out_reg_.read()[1] ? dm_DO.read().range(31, 16).to_uint() : dm_DO.read().range(15, 0).to_uint();
				break;
			default:
				dm_do_aligned = dm_DO.read().to_uint();
				break;
		}

		mem_read_data_next_ = dm_valid.read() ? sc_uint<BITWIDTH>(dm_do_aligned) : mem_read_data_reg_.read();
		MEM_read_data.write(mem_read_data_next_);

		dm_CEB.write(!((mem_mem_r_reg_.read() || mem_mem_w_reg_.read()) && !(mem_access_done_reg_.read() || dm_valid.read()) && mem_inst_valid_reg_.read()));
		dm_WEB.write(!mem_mem_w_reg_.read() && !(mem_access_done_reg_.read() || dm_valid.read()));
		dm_A.write(mem_exe_out_reg_.read());
		dm_DI.write(mem_store_data_reg_.read());

		switch (mem_funct3_reg_.read().to_uint()) {
			case kMemWidthByte:
				dm_STRB.write(0x1u);
				break;
			case kMemWidthHalf:
				dm_STRB.write(0x3u);
				break;
			case kMemWidthWord:
				dm_STRB.write(0xFu);
				break;
			default:
				dm_STRB.write(0u);
				break;
		}
	}

	void write_ff() {
		if (!rst_n.read()) {
			memory_state_reg_.write(false);
			mem_inst_valid_reg_.write(false);
			mem_pc_plus_4_reg_.write(0u);
			mem_mem_r_reg_.write(false);
			mem_mem_w_reg_.write(false);
			mem_rf_w_index_reg_.write(0u);
			mem_irf_wen_reg_.write(false);
			mem_frf_wen_reg_.write(false);
			mem_funct3_reg_.write(0u);
			mem_wb_sel_reg_.write(0u);
			mem_store_data_reg_.write(0u);
			mem_exe_out_reg_.write(0u);
			mem_access_done_reg_.write(false);
			mem_read_data_reg_.write(0u);
		} else {
			const bool stall_local = stall.read() || DM_stall.read();
			memory_state_reg_.write(memory_state_next_);
			mem_inst_valid_reg_.write(stall_local ? mem_inst_valid_reg_.read() : EXE_inst_valid.read());
			mem_pc_plus_4_reg_.write(stall_local ? mem_pc_plus_4_reg_.read() : EXE_pc_plus_4.read());
			mem_mem_r_reg_.write(stall_local ? mem_mem_r_reg_.read() : EXE_mem_R.read());
			mem_mem_w_reg_.write(stall_local ? mem_mem_w_reg_.read() : EXE_mem_W.read());
			mem_rf_w_index_reg_.write(stall_local ? mem_rf_w_index_reg_.read() : EXE_rf_w_index.read());
			mem_irf_wen_reg_.write(stall_local ? mem_irf_wen_reg_.read() : EXE_irf_wen.read());
			mem_frf_wen_reg_.write(stall_local ? mem_frf_wen_reg_.read() : EXE_frf_wen.read());
			mem_funct3_reg_.write(stall_local ? mem_funct3_reg_.read() : EXE_funct3.read());
			mem_wb_sel_reg_.write(stall_local ? mem_wb_sel_reg_.read() : EXE_wb_sel.read());
			mem_store_data_reg_.write(stall_local ? mem_store_data_reg_.read() : EXE_store_data.read());
			mem_exe_out_reg_.write(stall_local ? mem_exe_out_reg_.read() : EXE_exe_out.read());
			mem_access_done_reg_.write(mem_access_done_next_);
			mem_read_data_reg_.write(mem_read_data_next_);
		}
	}
};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc
