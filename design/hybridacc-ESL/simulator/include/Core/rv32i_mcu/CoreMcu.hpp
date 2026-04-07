#pragma once

/**
 * @file CoreMcu.hpp
 * @brief cc_core_mcu — 5-stage RV32I_Zmmul_Zicsr pipeline wrapper.
 *
 * Provides clean external interfaces for Isram (instruction fetch),
 * DataSram (load/store), and CmdFabric (MMIO).  Internally bridges
 * the FetchStage / MemoryStage CEB/WEB protocol to these ports.
 */

#include <systemc>
#include <cstdint>
#include <functional>
#include "Core/Types.hpp"
#include "Core/rv32i_mcu/PipelineTypes.hpp"
#include "Core/rv32i_mcu/FetchStage.hpp"
#include "Core/rv32i_mcu/DecodeStage.hpp"
#include "Core/rv32i_mcu/ExecuteStage.hpp"
#include "Core/rv32i_mcu/MemoryStage.hpp"
#include "Core/rv32i_mcu/WriteBackStage.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

SC_MODULE(CoreMcu) {

	// ====================================================================
	// External Ports
	// ====================================================================

	sc_core::sc_in<bool> clk;
	sc_core::sc_in<bool> reset_n;

	// --- Instruction fetch → Isram ---
	sc_core::sc_out<bool>           if_req_valid_o;
	sc_core::sc_out<sc_uint<32>>    if_addr_o;
	sc_core::sc_in<sc_uint<32>>     if_rdata_i;

	// --- Load/Store → DataSram ---
	sc_core::sc_out<bool>           ls_req_valid_o;
	sc_core::sc_out<bool>           ls_req_write_o;
	sc_core::sc_out<sc_uint<32>>    ls_req_addr_o;
	sc_core::sc_out<sc_uint<32>>    ls_req_wdata_o;
	sc_core::sc_out<sc_uint<4>>     ls_req_wstrb_o;
	sc_core::sc_in<sc_uint<32>>     ls_resp_rdata_i;

	// --- MMIO → CmdFabric ---
	sc_core::sc_out<bool>           mmio_req_valid_o;
	sc_core::sc_out<bool>           mmio_req_write_o;
	sc_core::sc_out<sc_uint<32>>    mmio_req_addr_o;
	sc_core::sc_out<sc_uint<32>>    mmio_req_wdata_o;
	sc_core::sc_out<sc_uint<4>>     mmio_req_wstrb_o;
	sc_core::sc_in<bool>            mmio_resp_valid_i;
	sc_core::sc_in<sc_uint<32>>     mmio_resp_rdata_i;

	// --- IRQs ---
	sc_core::sc_in<bool>            irq_meip_i;
	sc_core::sc_in<bool>            irq_msip_i;
	sc_core::sc_in<bool>            irq_mtip_i;

	// --- Boot / control ---
	sc_core::sc_in<sc_uint<32>>     boot_addr_i;
	sc_core::sc_in<bool>            core_enable_i;

	// --- Trace / status ---
	sc_core::sc_out<bool>           retire_valid_o;
	sc_core::sc_out<sc_uint<32>>    retire_pc_o;
	sc_core::sc_out<bool>           halted_o;

	// --- Runtime config ---
	bool core_debug = false;

	/// Install a direct SRAM read callback (bypasses signal delta-cycle)
	void set_sram_read_cb(std::function<uint32_t(uint32_t)> cb) {
		sram_read_cb_ = std::move(cb);
	}

	/// Install a direct ISRAM read callback for data-port reads from IRAM
	void set_isram_read_cb(std::function<uint32_t(uint32_t)> cb) {
		isram_read_cb_ = std::move(cb);
	}

	// ====================================================================
	// Pipeline submodules
	// ====================================================================

	FetchStage if_stage_0_;
	DecodeStage<> id_stage_0_;
	ExecuteStage<> exe_stage_0_;
	MemoryStage<> mem_stage_0_;
	WriteBackStage<> wb_stage_0_;

	// ====================================================================
	// Inter-stage pipeline signals
	// ====================================================================

	sc_core::sc_signal<bool> EXE_taken_sig_;
	sc_core::sc_signal<bool> EXE_update_sig_;
	sc_core::sc_signal<bool> EXE_bp_miss_sig_;
	sc_core::sc_signal<bool> IF_inst_valid_sig_;
	sc_core::sc_signal<bool> ID_inst_valid_sig_;
	sc_core::sc_signal<bool> EXE_inst_valid_sig_;
	sc_core::sc_signal<bool> MEM_inst_valid_sig_;
	sc_core::sc_signal<bool> EXE_mem_R_sig_;
	sc_core::sc_signal<bool> EXE_mem_W_sig_;
	sc_core::sc_signal<bool> EXE_irf_wen_sig_;
	sc_core::sc_signal<bool> EXE_frf_wen_sig_;
	sc_core::sc_signal<bool> MEM_irf_wen_sig_;
	sc_core::sc_signal<bool> MEM_frf_wen_sig_;
	sc_core::sc_signal<bool> WB_irf_wen_sig_;
	sc_core::sc_signal<bool> WB_frf_wen_sig_;

	sc_core::sc_signal<bool> IM_stall_sig_;
	sc_core::sc_signal<bool> DM_stall_sig_;
	sc_core::sc_signal<bool> stall_DH_sig_;
	sc_core::sc_signal<bool> stall_FP_sig_;
	sc_core::sc_signal<bool> stall_M_sig_;
	sc_core::sc_signal<bool> stall_fence_sig_;
	sc_core::sc_signal<bool> stall_wfi_sig_;
	sc_core::sc_signal<bool> IF_stall_sig_;
	sc_core::sc_signal<bool> ID_stall_sig_;
	sc_core::sc_signal<bool> EXE_stall_sig_;
	sc_core::sc_signal<bool> MEM_stall_sig_;
	sc_core::sc_signal<bool> WB_stall_sig_;

	sc_core::sc_signal<sc_uint<5>> EXE_rf_w_index_sig_;
	sc_core::sc_signal<sc_uint<5>> MEM_rf_w_index_sig_;
	sc_core::sc_signal<sc_uint<5>> WB_rf_w_index_sig_;
	sc_core::sc_signal<sc_uint<3>> EXE_funct3_sig_;
	sc_core::sc_signal<sc_uint<3>> MEM_funct3_sig_;
	sc_core::sc_signal<sc_uint<2>> EXE_wb_sel_sig_;
	sc_core::sc_signal<sc_uint<2>> MEM_wb_sel_sig_;

	sc_core::sc_signal<sc_uint<kRvPcBit>> EXE_pc_target_sig_;
	sc_core::sc_signal<sc_uint<kRvPcBit>> IF_pc_sig_;
	sc_core::sc_signal<sc_uint<kRvPcBit>> ID_pc_sig_;
	sc_core::sc_signal<sc_uint<kRvPcBit>> EXE_pc_sig_;
	sc_core::sc_signal<sc_uint<kRvPcBit>> IF_pc_plus_4_sig_;
	sc_core::sc_signal<sc_uint<kRvPcBit>> ID_pc_plus_4_sig_;
	sc_core::sc_signal<sc_uint<kRvPcBit>> EXE_pc_plus_4_sig_;
	sc_core::sc_signal<sc_uint<kRvPcBit>> MEM_pc_plus_4_sig_;

	sc_core::sc_signal<sc_uint<kRvMemDataBitWidth>> IF_inst_sig_;
	sc_core::sc_signal<sc_uint<32>> ID_inst_sig_;
	sc_core::sc_signal<sc_uint<32>> ID_rs1_data_sig_;
	sc_core::sc_signal<sc_uint<32>> ID_rs2_data_sig_;
	sc_core::sc_signal<sc_uint<32>> ID_imm_sig_;
	sc_core::sc_signal<sc_uint<17>> ID_controll_sel_sig_;
	sc_core::sc_signal<sc_uint<32>> EXE_store_data_sig_;
	sc_core::sc_signal<sc_uint<32>> EXE_exe_out_sig_;
	sc_core::sc_signal<sc_uint<32>> MEM_exe_out_sig_;
	sc_core::sc_signal<sc_uint<32>> MEM_rf_w_data_sig_;
	sc_core::sc_signal<sc_uint<32>> MEM_read_data_sig_;
	sc_core::sc_signal<sc_uint<32>> WB_rf_w_data_sig_;

	// ====================================================================
	// Internal bridge signals
	// ====================================================================

	// Internal IM interface (FetchStage ↔ bridge)
	sc_core::sc_signal<bool>           im_CEB_int_{"im_CEB_int"};
	sc_core::sc_signal<bool>           im_WEB_int_{"im_WEB_int"};
	sc_core::sc_signal<sc_uint<4>>     im_STRB_int_{"im_STRB_int"};
	sc_core::sc_signal<sc_uint<32>>    im_A_int_{"im_A_int"};
	sc_core::sc_signal<sc_uint<32>>    im_DI_int_{"im_DI_int"};
	sc_core::sc_signal<sc_uint<32>>    im_DO_int_{"im_DO_int"};
	sc_core::sc_signal<bool>           im_valid_int_{"im_valid_int"};
	sc_core::sc_signal<bool>           im_valid_reg_{"im_valid_reg"};
	sc_core::sc_signal<sc_uint<32>>    if_rdata_reg_{"if_rdata_reg"}; ///< registered IF data

	// Internal DM interface (MemoryStage ↔ bridge)
	sc_core::sc_signal<bool>           dm_CEB_int_{"dm_CEB_int"};
	sc_core::sc_signal<bool>           dm_WEB_int_{"dm_WEB_int"};
	sc_core::sc_signal<sc_uint<4>>     dm_STRB_int_{"dm_STRB_int"};
	sc_core::sc_signal<sc_uint<32>>    dm_A_int_{"dm_A_int"};
	sc_core::sc_signal<sc_uint<32>>    dm_DI_int_{"dm_DI_int"};
	sc_core::sc_signal<sc_uint<32>>    dm_DO_int_{"dm_DO_int"};
	sc_core::sc_signal<bool>           dm_valid_int_{"dm_valid_int"};
	sc_core::sc_signal<bool>           dm_valid_reg_{"dm_valid_reg"};
	sc_core::sc_signal<sc_uint<32>>    dm_rdata_reg_{"dm_rdata_reg"}; ///< registered DM read data

	// Pipeline internal reset (gated by core_enable)
	sc_core::sc_signal<bool>           pipe_reset_n_{"pipe_reset_n"};

	// Halted / retire tracking
	sc_core::sc_signal<bool>           halted_reg_{"halted_reg"};
	sc_core::sc_signal<bool>           retire_valid_reg_{"retire_valid_reg"};
	sc_core::sc_signal<sc_uint<32>>    retire_pc_reg_{"retire_pc_reg"};

	// SRAM read callback
	std::function<uint32_t(uint32_t)>  sram_read_cb_;
	// ISRAM read callback (for data-port reads from instruction RAM)
	std::function<uint32_t(uint32_t)>  isram_read_cb_;

	// ====================================================================
	// Constructor
	// ====================================================================

	SC_HAS_PROCESS(CoreMcu);

	CoreMcu(sc_module_name name)
		: sc_module(name),
		  clk("clk"), reset_n("reset_n"),
		  if_req_valid_o("if_req_valid_o"),
		  if_addr_o("if_addr_o"),
		  if_rdata_i("if_rdata_i"),
		  ls_req_valid_o("ls_req_valid_o"),
		  ls_req_write_o("ls_req_write_o"),
		  ls_req_addr_o("ls_req_addr_o"),
		  ls_req_wdata_o("ls_req_wdata_o"),
		  ls_req_wstrb_o("ls_req_wstrb_o"),
		  ls_resp_rdata_i("ls_resp_rdata_i"),
		  mmio_req_valid_o("mmio_req_valid_o"),
		  mmio_req_write_o("mmio_req_write_o"),
		  mmio_req_addr_o("mmio_req_addr_o"),
		  mmio_req_wdata_o("mmio_req_wdata_o"),
		  mmio_req_wstrb_o("mmio_req_wstrb_o"),
		  mmio_resp_valid_i("mmio_resp_valid_i"),
		  mmio_resp_rdata_i("mmio_resp_rdata_i"),
		  irq_meip_i("irq_meip_i"),
		  irq_msip_i("irq_msip_i"),
		  irq_mtip_i("irq_mtip_i"),
		  boot_addr_i("boot_addr_i"),
		  core_enable_i("core_enable_i"),
		  retire_valid_o("retire_valid_o"),
		  retire_pc_o("retire_pc_o"),
		  halted_o("halted_o"),
		  if_stage_0_("IF_STAGE_0"),
		  id_stage_0_("ID_STAGE_0"),
		  exe_stage_0_("EXE_STAGE_0"),
		  mem_stage_0_("MEM_STAGE_0"),
		  wb_stage_0_("WB_STAGE_0")
	{
		// ================================================================
		// FetchStage wiring (IM goes to internal bridge signals)
		// ================================================================
		if_stage_0_.clk(clk);
		if_stage_0_.rst_n(pipe_reset_n_);
		if_stage_0_.IM_stall(IM_stall_sig_);
		if_stage_0_.stall(IF_stall_sig_);
		if_stage_0_.EXE_bp_miss(EXE_bp_miss_sig_);
		if_stage_0_.EXE_taken(EXE_taken_sig_);
		if_stage_0_.EXE_pc_target(EXE_pc_target_sig_);
		if_stage_0_.EXE_pc(EXE_pc_sig_);
		if_stage_0_.EXE_update(EXE_update_sig_);
		if_stage_0_.IF_pc(IF_pc_sig_);
		if_stage_0_.IF_pc_plus_4(IF_pc_plus_4_sig_);
		if_stage_0_.IF_inst(IF_inst_sig_);
		if_stage_0_.IF_inst_valid(IF_inst_valid_sig_);
		if_stage_0_.im_CEB(im_CEB_int_);
		if_stage_0_.im_WEB(im_WEB_int_);
		if_stage_0_.im_STRB(im_STRB_int_);
		if_stage_0_.im_A(im_A_int_);
		if_stage_0_.im_DI(im_DI_int_);
		if_stage_0_.im_DO(im_DO_int_);
		if_stage_0_.im_valid(im_valid_int_);

		// ================================================================
		// DecodeStage wiring
		// ================================================================
		id_stage_0_.clk(clk);
		id_stage_0_.rst_n(pipe_reset_n_);
		id_stage_0_.stall(ID_stall_sig_);
		id_stage_0_.flush(EXE_bp_miss_sig_);
		id_stage_0_.IF_pc(IF_pc_sig_);
		id_stage_0_.IF_pc_plus_4(IF_pc_plus_4_sig_);
		id_stage_0_.IF_inst(IF_inst_sig_);
		id_stage_0_.IF_inst_valid(IF_inst_valid_sig_);
		id_stage_0_.ID_pc(ID_pc_sig_);
		id_stage_0_.ID_pc_plus_4(ID_pc_plus_4_sig_);
		id_stage_0_.ID_inst(ID_inst_sig_);
		id_stage_0_.ID_rs1_data(ID_rs1_data_sig_);
		id_stage_0_.ID_rs2_data(ID_rs2_data_sig_);
		id_stage_0_.ID_imm(ID_imm_sig_);
		id_stage_0_.ID_controll_sel(ID_controll_sel_sig_);
		id_stage_0_.ID_inst_valid(ID_inst_valid_sig_);
		id_stage_0_.WB_irf_wen(WB_irf_wen_sig_);
		id_stage_0_.WB_frf_wen(WB_frf_wen_sig_);
		id_stage_0_.WB_rf_w_index(WB_rf_w_index_sig_);
		id_stage_0_.WB_rf_w_data(WB_rf_w_data_sig_);

		// ================================================================
		// ExecuteStage wiring (IRQs from bridge signals)
		// ================================================================
		exe_stage_0_.clk(clk);
		exe_stage_0_.rst_n(pipe_reset_n_);
		exe_stage_0_.timer_interrupt(irq_mtip_i);
		exe_stage_0_.external_interrupt(irq_meip_i);
		exe_stage_0_.software_interrupt(irq_msip_i);
		exe_stage_0_.stall_wfi(stall_wfi_sig_);
		exe_stage_0_.stall_DH(stall_DH_sig_);
		exe_stage_0_.stall_FP(stall_FP_sig_);
		exe_stage_0_.stall_M(stall_M_sig_);
		exe_stage_0_.stall_fence(stall_fence_sig_);
		exe_stage_0_.stall(EXE_stall_sig_);
		exe_stage_0_.IF_pc(IF_pc_sig_);
		exe_stage_0_.IF_pc_plus_4(IF_pc_plus_4_sig_);
		exe_stage_0_.EXE_taken(EXE_taken_sig_);
		exe_stage_0_.EXE_update(EXE_update_sig_);
		exe_stage_0_.EXE_pc_target(EXE_pc_target_sig_);
		exe_stage_0_.EXE_bp_miss(EXE_bp_miss_sig_);
		exe_stage_0_.ID_pc(ID_pc_sig_);
		exe_stage_0_.ID_pc_plus_4(ID_pc_plus_4_sig_);
		exe_stage_0_.ID_inst(ID_inst_sig_);
		exe_stage_0_.ID_rs1_data(ID_rs1_data_sig_);
		exe_stage_0_.ID_rs2_data(ID_rs2_data_sig_);
		exe_stage_0_.ID_imm(ID_imm_sig_);
		exe_stage_0_.ID_controll_sel(ID_controll_sel_sig_);
		exe_stage_0_.ID_inst_valid(ID_inst_valid_sig_);
		exe_stage_0_.EXE_pc(EXE_pc_sig_);
		exe_stage_0_.EXE_pc_plus_4(EXE_pc_plus_4_sig_);
		exe_stage_0_.EXE_mem_R(EXE_mem_R_sig_);
		exe_stage_0_.EXE_mem_W(EXE_mem_W_sig_);
		exe_stage_0_.EXE_rf_w_index(EXE_rf_w_index_sig_);
		exe_stage_0_.EXE_irf_wen(EXE_irf_wen_sig_);
		exe_stage_0_.EXE_frf_wen(EXE_frf_wen_sig_);
		exe_stage_0_.EXE_funct3(EXE_funct3_sig_);
		exe_stage_0_.EXE_wb_sel(EXE_wb_sel_sig_);
		exe_stage_0_.EXE_store_data(EXE_store_data_sig_);
		exe_stage_0_.EXE_exe_out(EXE_exe_out_sig_);
		exe_stage_0_.EXE_inst_valid(EXE_inst_valid_sig_);
		exe_stage_0_.MEM_irf_wen(MEM_irf_wen_sig_);
		exe_stage_0_.MEM_frf_wen(MEM_frf_wen_sig_);
		exe_stage_0_.MEM_rf_w_index(MEM_rf_w_index_sig_);
		exe_stage_0_.MEM_rf_w_data(MEM_rf_w_data_sig_);
		exe_stage_0_.dm_valid(dm_valid_int_);
		exe_stage_0_.WB_irf_wen(WB_irf_wen_sig_);
		exe_stage_0_.WB_frf_wen(WB_frf_wen_sig_);
		exe_stage_0_.WB_rf_w_index(WB_rf_w_index_sig_);
		exe_stage_0_.WB_rf_w_data(WB_rf_w_data_sig_);

		// ================================================================
		// MemoryStage wiring (DM goes to internal bridge signals)
		// ================================================================
		mem_stage_0_.clk(clk);
		mem_stage_0_.rst_n(pipe_reset_n_);
		mem_stage_0_.DM_stall(DM_stall_sig_);
		mem_stage_0_.stall(MEM_stall_sig_);
		mem_stage_0_.EXE_pc_plus_4(EXE_pc_plus_4_sig_);
		mem_stage_0_.EXE_mem_R(EXE_mem_R_sig_);
		mem_stage_0_.EXE_mem_W(EXE_mem_W_sig_);
		mem_stage_0_.EXE_rf_w_index(EXE_rf_w_index_sig_);
		mem_stage_0_.EXE_irf_wen(EXE_irf_wen_sig_);
		mem_stage_0_.EXE_frf_wen(EXE_frf_wen_sig_);
		mem_stage_0_.EXE_funct3(EXE_funct3_sig_);
		mem_stage_0_.EXE_wb_sel(EXE_wb_sel_sig_);
		mem_stage_0_.EXE_store_data(EXE_store_data_sig_);
		mem_stage_0_.EXE_exe_out(EXE_exe_out_sig_);
		mem_stage_0_.EXE_inst_valid(EXE_inst_valid_sig_);
		mem_stage_0_.MEM_pc_plus_4(MEM_pc_plus_4_sig_);
		mem_stage_0_.MEM_rf_w_data(MEM_rf_w_data_sig_);
		mem_stage_0_.MEM_exe_out(MEM_exe_out_sig_);
		mem_stage_0_.MEM_rf_w_index(MEM_rf_w_index_sig_);
		mem_stage_0_.MEM_irf_wen(MEM_irf_wen_sig_);
		mem_stage_0_.MEM_frf_wen(MEM_frf_wen_sig_);
		mem_stage_0_.MEM_read_data(MEM_read_data_sig_);
		mem_stage_0_.MEM_funct3(MEM_funct3_sig_);
		mem_stage_0_.MEM_wb_sel(MEM_wb_sel_sig_);
		mem_stage_0_.MEM_inst_valid(MEM_inst_valid_sig_);
		mem_stage_0_.dm_CEB(dm_CEB_int_);
		mem_stage_0_.dm_WEB(dm_WEB_int_);
		mem_stage_0_.dm_STRB(dm_STRB_int_);
		mem_stage_0_.dm_A(dm_A_int_);
		mem_stage_0_.dm_DI(dm_DI_int_);
		mem_stage_0_.dm_DO(dm_DO_int_);
		mem_stage_0_.dm_valid(dm_valid_int_);

		// ================================================================
		// WriteBackStage wiring
		// ================================================================
		wb_stage_0_.clk(clk);
		wb_stage_0_.rst_n(pipe_reset_n_);
		wb_stage_0_.stall(WB_stall_sig_);
		wb_stage_0_.MEM_pc_plus_4(MEM_pc_plus_4_sig_);
		wb_stage_0_.MEM_exe_out(MEM_exe_out_sig_);
		wb_stage_0_.MEM_rf_w_index(MEM_rf_w_index_sig_);
		wb_stage_0_.MEM_irf_wen(MEM_irf_wen_sig_);
		wb_stage_0_.MEM_frf_wen(MEM_frf_wen_sig_);
		wb_stage_0_.MEM_read_data(MEM_read_data_sig_);
		wb_stage_0_.MEM_funct3(MEM_funct3_sig_);
		wb_stage_0_.MEM_wb_sel(MEM_wb_sel_sig_);
		wb_stage_0_.MEM_inst_valid(MEM_inst_valid_sig_);
		wb_stage_0_.WB_irf_wen(WB_irf_wen_sig_);
		wb_stage_0_.WB_frf_wen(WB_frf_wen_sig_);
		wb_stage_0_.WB_rf_w_index(WB_rf_w_index_sig_);
		wb_stage_0_.WB_rf_w_data(WB_rf_w_data_sig_);

		// ================================================================
		// Pipeline stall logic
		// ================================================================
		SC_METHOD(compute_stall_logic);
		sensitive << IM_stall_sig_ << DM_stall_sig_ << stall_DH_sig_
		          << stall_FP_sig_ << stall_wfi_sig_ << stall_M_sig_
		          << stall_fence_sig_;

		// ================================================================
		// Bridge: pipeline reset gated by core_enable
		// ================================================================
		SC_METHOD(comb_pipe_reset);
		sensitive << reset_n << core_enable_i;

		// ================================================================
		// Bridge: IF → Isram external interface
		// ================================================================
		SC_METHOD(comb_if_bridge);
		sensitive << im_CEB_int_ << im_A_int_ << if_rdata_reg_;

		SC_METHOD(comb_if_valid);
		sensitive << im_valid_reg_;

		// ================================================================
		// Bridge: DM → DataSram / MMIO external interface
		// ================================================================
		SC_METHOD(comb_dm_bridge);
		sensitive << dm_CEB_int_ << dm_WEB_int_ << dm_STRB_int_
		          << dm_A_int_ << dm_DI_int_
		          << dm_rdata_reg_ << mmio_resp_rdata_i;

		SC_METHOD(comb_dm_valid);
		sensitive << dm_valid_reg_ << dm_A_int_ << mmio_resp_valid_i;

		// ================================================================
		// Bridge: halted / retire outputs
		// ================================================================
		SC_METHOD(comb_outputs);
		sensitive << halted_reg_ << retire_valid_reg_ << retire_pc_reg_;

		// ================================================================
		// Clocked bridge registers
		// ================================================================
		SC_METHOD(clk_bridge_regs);
		sensitive << clk.pos();
	}

	// ====================================================================
	// Process implementations
	// ====================================================================

	void compute_stall_logic() {
		IF_stall_sig_.write(DM_stall_sig_.read() || stall_DH_sig_.read() || stall_FP_sig_.read() || stall_wfi_sig_.read() || stall_M_sig_.read() || stall_fence_sig_.read());
		ID_stall_sig_.write(IM_stall_sig_.read() || DM_stall_sig_.read() || stall_DH_sig_.read() || stall_FP_sig_.read() || stall_wfi_sig_.read() || stall_M_sig_.read() || stall_fence_sig_.read());
		EXE_stall_sig_.write(IM_stall_sig_.read() || DM_stall_sig_.read() || stall_FP_sig_.read() || stall_wfi_sig_.read() || stall_M_sig_.read());
		MEM_stall_sig_.write(IM_stall_sig_.read() || stall_FP_sig_.read() || stall_wfi_sig_.read() || stall_M_sig_.read());
		WB_stall_sig_.write(IM_stall_sig_.read() || DM_stall_sig_.read() || stall_FP_sig_.read() || stall_wfi_sig_.read() || stall_M_sig_.read());
	}

	/// Pipeline reset is held low until both reset_n and core_enable are high.
	void comb_pipe_reset() {
		pipe_reset_n_.write(reset_n.read() && core_enable_i.read());
	}

	/// Bridge: convert FetchStage CEB/A → Isram valid/addr port;
	///         feed Isram rdata back to FetchStage DO.
	void comb_if_bridge() {
		if_req_valid_o.write(!im_CEB_int_.read());
		if_addr_o.write(im_A_int_.read());
		// Use registered data — Isram output is only valid while request is active
		im_DO_int_.write(if_rdata_reg_.read());
	}

	/// IF valid: registered 1-cycle response (matches SRAM latency=1).
	void comb_if_valid() {
		im_valid_int_.write(im_valid_reg_.read());
	}

	/// Bridge: convert MemoryStage CEB/WEB/A/DI/STRB → DataSram or MMIO;
	///         drive external request ports. dm_DO_int_ uses registered data for DSRAM.
	void comb_dm_bridge() {
		const bool active = !dm_CEB_int_.read();
		const bool is_write = !dm_WEB_int_.read();
		const uint32_t addr = dm_A_int_.read().to_uint();
		const bool is_dsram = is_dsram_range(addr);
		const bool is_isram = is_isram_range(addr);

		// DataSram port
		ls_req_valid_o.write(active && is_dsram);
		ls_req_write_o.write(is_write);
		ls_req_addr_o.write(addr);
		ls_req_wdata_o.write(dm_DI_int_.read());
		ls_req_wstrb_o.write(dm_STRB_int_.read());

		// MMIO port (only if not DSRAM and not ISRAM)
		mmio_req_valid_o.write(active && !is_dsram && !is_isram);
		mmio_req_write_o.write(is_write);
		mmio_req_addr_o.write(addr);
		mmio_req_wdata_o.write(dm_DI_int_.read());
		mmio_req_wstrb_o.write(dm_STRB_int_.read());

		// Read data mux:
		//   DSRAM: use registered data (captured at posedge, same time as dm_valid_reg_)
		//   ISRAM: use registered data (captured at posedge via isram_read_cb_)
		//   MMIO:  use external response (combinational)
		if (is_dsram || is_isram) {
			dm_DO_int_.write(dm_rdata_reg_.read());
		} else {
			dm_DO_int_.write(mmio_resp_rdata_i.read());
		}
	}

	/// DM valid: DSRAM uses registered 1-cycle response;
	///           MMIO uses external response valid.
	void comb_dm_valid() {
		const uint32_t addr = dm_A_int_.read().to_uint();
		if (is_dsram_range(addr) || is_isram_range(addr)) {
			dm_valid_int_.write(dm_valid_reg_.read());
		} else {
			dm_valid_int_.write(mmio_resp_valid_i.read());
		}
	}

	/// Drive external halted / retire ports from registered values.
	void comb_outputs() {
		halted_o.write(halted_reg_.read());
		retire_valid_o.write(retire_valid_reg_.read());
		retire_pc_o.write(retire_pc_reg_.read());
	}

	/// Clocked bridge registers: IF/DM valid, halted detection, retire tracking.
	void clk_bridge_regs() {
		if (!reset_n.read()) {
			im_valid_reg_.write(false);
			dm_valid_reg_.write(false);
			halted_reg_.write(false);
			retire_valid_reg_.write(false);
			retire_pc_reg_.write(0);
		} else if (!core_enable_i.read()) {
			im_valid_reg_.write(false);
			dm_valid_reg_.write(false);
			// halted_reg_ preserved
			retire_valid_reg_.write(false);
		} else {
			// IF: capture data & valid 1 cycle after request
			const bool if_active = !im_CEB_int_.read();
			im_valid_reg_.write(if_active);
			if (if_active) {
				if_rdata_reg_.write(if_rdata_i.read());
			}

			// DM: capture read data and valid 1 cycle after DSRAM/ISRAM request
			const bool dm_active = !dm_CEB_int_.read();
			const uint32_t dm_addr = dm_A_int_.read().to_uint();
			const bool dm_is_dsram = dm_active && is_dsram_range(dm_addr);
			const bool dm_is_isram = dm_active && is_isram_range(dm_addr);
			dm_valid_reg_.write(dm_is_dsram || dm_is_isram);
			if (dm_is_dsram) {
				const uint32_t rd_addr = dm_addr & ~3u;
				if (sram_read_cb_) {
					dm_rdata_reg_.write(sram_read_cb_(rd_addr));
				} else {
					dm_rdata_reg_.write(ls_resp_rdata_i.read());
				}
			} else if (dm_is_isram) {
				const uint32_t rd_addr = dm_addr & ~3u;
				if (isram_read_cb_) {
					dm_rdata_reg_.write(isram_read_cb_(rd_addr));
				}
			}

			// Detect EBREAK (instruction encoding 0x00100073)
			if (!halted_reg_.read()) {
				const uint32_t exe_inst = exe_stage_0_.exe_inst_reg_.read().to_uint();
				const bool exe_valid = exe_stage_0_.exe_inst_valid_reg_.read();
				if (exe_inst == 0x00100073u && exe_valid) {
					halted_reg_.write(true);
				}
			}

			// Retire tracking (from MEM→WB transition)
			retire_valid_reg_.write(MEM_inst_valid_sig_.read() && !WB_stall_sig_.read());
			const uint32_t mem_pc4 = MEM_pc_plus_4_sig_.read().to_uint();
			retire_pc_reg_.write(mem_pc4 >= 4 ? mem_pc4 - 4 : 0);

			// Debug trace
			if (core_debug) {
				const uint32_t cycle = static_cast<uint32_t>(sc_core::sc_time_stamp().to_double() / sc_core::sc_time(1, sc_core::SC_NS).to_double());
				const uint32_t exe_inst_v = exe_stage_0_.exe_inst_reg_.read().to_uint();
				const uint32_t exe_ctrl_v = exe_stage_0_.exe_control_sel_reg_.read().to_uint();
				const bool exe_valid_v = exe_stage_0_.exe_inst_valid_reg_.read();
				std::printf("[%4u] IF_pc=%08x IM_stall=%d | ID_inst=%08x ID_v=%d | EXE_inst=%08x EXE_v=%d ctrl=%05x | "
				            "MEM_v=%d DM_stall=%d dm_val=%d | WB_wen=%d WB_idx=%u WB_data=%08x | "
				            "stall_DH=%d IF_s=%d ID_s=%d EXE_s=%d MEM_s=%d WB_s=%d\n",
				    cycle,
				    IF_pc_sig_.read().to_uint(),
				    (int)IM_stall_sig_.read(),
				    id_stage_0_.id_inst_reg_.read().to_uint(),
				    (int)id_stage_0_.id_inst_valid_reg_.read(),
				    exe_inst_v,
				    (int)exe_valid_v,
				    exe_ctrl_v,
				    (int)MEM_inst_valid_sig_.read(),
				    (int)DM_stall_sig_.read(),
				    (int)dm_valid_int_.read(),
				    (int)WB_irf_wen_sig_.read(),
				    WB_rf_w_index_sig_.read().to_uint(),
				    WB_rf_w_data_sig_.read().to_uint(),
				    (int)stall_DH_sig_.read(),
				    (int)IF_stall_sig_.read(),
				    (int)ID_stall_sig_.read(),
				    (int)EXE_stall_sig_.read(),
				    (int)MEM_stall_sig_.read(),
				    (int)WB_stall_sig_.read());
			}
		}
	}

private:
	/// Check if address falls in Data SRAM range.
	static bool is_dsram_range(uint32_t addr) {
		return addr >= kBaseDataRam && addr <= kEndDataRam;
	}

	/// Check if address falls in Instruction SRAM range (for data-port reads).
	static bool is_isram_range(uint32_t addr) {
		return addr >= kBaseInstRam && addr <= kEndInstRam;
	}
};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc
