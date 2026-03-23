#pragma once

#include <systemc>

#include <cstdint>

#include "Core/BootHostIf.hpp"
#include "Core/CmdFabric.hpp"
#include "Core/ClusterDataFabric.hpp"
#include "Core/CoreMcu.hpp"
#include "Core/DataSram.hpp"
#include "Core/DmaEngine.hpp"
#include "Core/IrqRouter.hpp"
#include "Core/Isram.hpp"
#include "Core/SectionLoader.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {

template <unsigned NUM_CLUSTERS = 4, unsigned NUM_NLU = 1, unsigned ISRAM_BYTES = 16384, unsigned DATA_SRAM_BYTES = 65536>
SC_MODULE(CoreController) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};
	sc_in<sc_uint<NUM_CLUSTERS>> cluster_irq_i{"cluster_irq_i"};
	sc_in<sc_uint<NUM_NLU>> nlu_irq_i{"nlu_irq_i"};

	sc_out<bool> dma_req_valid_o{"dma_req_valid_o"};
	sc_out<core::DmaRequest> dma_req_o{"dma_req_o"};
	sc_in<bool> dma_req_ready_i{"dma_req_ready_i"};
	sc_out<bool> cluster_cmd_valid_o{"cluster_cmd_valid_o"};
	sc_out<core::ClusterCommand> cluster_cmd_o{"cluster_cmd_o"};
	sc_in<bool> cluster_cmd_ready_i{"cluster_cmd_ready_i"};
	sc_out<bool> nlu_cmd_valid_o{"nlu_cmd_valid_o"};
	sc_out<core::NluCommand> nlu_cmd_o{"nlu_cmd_o"};
	sc_in<bool> nlu_cmd_ready_i{"nlu_cmd_ready_i"};
	sc_out<bool> controller_irq_o{"controller_irq_o"};

	core::BootHostIf boot_host_if{"boot_host_if"};
	core::SectionLoader section_loader{"section_loader"};
	core::Isram<ISRAM_BYTES> isram{"isram"};
	core::DataSram<DATA_SRAM_BYTES> data_sram{"data_sram"};
	core::CoreMcu<DATA_SRAM_BYTES> core_mcu{"core_mcu"};
	core::CmdFabric cmd_fabric{"cmd_fabric"};
	core::IrqRouter<NUM_CLUSTERS, NUM_NLU> irq_router{"irq_router"};
	core::DmaEngine dma_engine{"dma_engine"};
	core::ClusterDataFabric cluster_data_fabric{"cluster_data_fabric"};

	SC_CTOR(CoreController) {
		bind_submodules();

		SC_METHOD(comb_core_reset_process);
		sensitive << reset_n << core_start_sig_ << manual_core_enable_sig_;

		SC_METHOD(comb_runtime_error_process);
		sensitive << runtime_error_sig_;

		SC_CTHREAD(seq_status_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	void host_push_manifest(const core::ManifestPacket& packet) {
		boot_host_if.host_push_manifest(packet);
	}

	void host_write_run_config(uint32_t job_base, uint32_t range_begin, uint32_t range_count) {
		boot_host_if.host_write_run_config(job_base, range_begin, range_count);
	}

	void load_instruction(uint32_t byte_addr, uint32_t value) {
		manual_core_enable_sig_.write(true);
		isram.load_instruction(byte_addr, value);
	}

	void load_job_desc(uint32_t index, const core::HaccJobDesc& job) {
		manual_core_enable_sig_.write(true);
		const uint32_t base = core::kDataSramBase + index * 64u;
		data_sram.load_word(base + 0u, job.version);
		data_sram.load_word(base + 4u, job.flags);
		data_sram.load_word(base + 8u, job.block_desc_base);
		data_sram.load_word(base + 12u, job.block_desc_count);
		data_sram.load_word(base + 16u, job.profile_table_base);
		data_sram.load_word(base + 20u, job.profile_table_count);
		data_sram.load_word(base + 24u, job.dma_rule_base);
		data_sram.load_word(base + 28u, job.dma_rule_count);
		data_sram.load_word(base + 32u, job.agu_rule_base);
		data_sram.load_word(base + 36u, job.agu_rule_count);
		data_sram.load_word(base + 40u, job.patch_base);
		data_sram.load_word(base + 44u, job.patch_count);
		data_sram.load_word(base + 48u, job.pe_program_base);
		data_sram.load_word(base + 52u, job.scan_chain_base);
		data_sram.load_word(base + 56u, job.required_cluster_mask);
		data_sram.load_word(base + 60u, job.required_caps);
	}

	void load_block_desc(uint32_t index, const core::HaccBlockDesc& block) {
		manual_core_enable_sig_.write(true);
		const uint32_t base = core::kDataSramBase + 0x100u + index * 32u;
		data_sram.load_word(base + 0u, block.block_id);
		data_sram.load_word(base + 4u, static_cast<uint32_t>(block.profile_id) | (static_cast<uint32_t>(block.pe_program_id) << 16));
		data_sram.load_word(base + 8u, static_cast<uint32_t>(block.loop_rank) | (static_cast<uint32_t>(block.flags) << 16));
		data_sram.load_word(base + 12u, block.repeat_count);
		data_sram.load_word(base + 16u, block.cluster_mask);
		data_sram.load_word(base + 20u, block.section_map_seed);
		data_sram.load_word(base + 24u, 0u);
		data_sram.load_word(base + 28u, 0u);
	}

	bool loader_done() const { return section_loader_done_sig_.read(); }
	uint32_t read_core_csr(uint32_t csr_id) const { return read_csr(csr_id); }
	uint32_t read_csr(uint32_t csr_id) const {
		switch (csr_id) {
		case core::kCsrHaccStatus: return hacc_status_reg_.read().to_uint();
		case core::kCsrHaccErrCode: return hacc_err_code_reg_.read().to_uint();
		case core::kCsrHaccLastBlockId: return hacc_last_block_id_reg_.read().to_uint();
		case core::kCsrHaccPerfDmaCnt: return hacc_perf_dma_cnt_reg_.read().to_uint();
		case core::kCsrHaccPerfMmioCnt: return hacc_perf_mmio_cnt_reg_.read().to_uint();
		default: return core_mcu.debug_read_csr(csr_id);
		}
	}
	uint32_t read_pc() const { return core_mcu.debug_read_pc(); }
	bool is_halted() const { return core_mcu.debug_is_halted(); }
	uint32_t read_gpr(uint32_t index) const { return core_mcu.debug_read_gpr(index); }

private:
	sc_signal<bool> core_reset_n_sig_{"core_reset_n_sig"};
	sc_signal<bool> core_start_sig_{"core_start_sig"};
	sc_signal<bool> runtime_error_sig_{"runtime_error_sig"};
	sc_signal<core::ManifestHeader> manifest_header_sig_{"manifest_header_sig"};
	sc_signal<bool> manifest_header_valid_sig_{"manifest_header_valid_sig"};
	sc_signal<bool> manifest_header_ready_sig_{"manifest_header_ready_sig"};
	sc_signal<core::ManifestPayloadBeat> manifest_payload_sig_{"manifest_payload_sig"};
	sc_signal<bool> manifest_payload_valid_sig_{"manifest_payload_valid_sig"};
	sc_signal<bool> manifest_payload_ready_sig_{"manifest_payload_ready_sig"};
	sc_signal<bool> loader_isram_wr_valid_sig_{"loader_isram_wr_valid_sig"};
	sc_signal<sc_uint<32>> loader_isram_wr_addr_sig_{"loader_isram_wr_addr_sig"};
	sc_signal<sc_uint<32>> loader_isram_wr_data_sig_{"loader_isram_wr_data_sig"};
	sc_signal<sc_uint<4>> loader_isram_wr_strb_sig_{"loader_isram_wr_strb_sig"};
	sc_signal<bool> loader_data_wr_valid_sig_{"loader_data_wr_valid_sig"};
	sc_signal<bool> loader_data_wr_write_sig_{"loader_data_wr_write_sig"};
	sc_signal<sc_uint<32>> loader_data_wr_addr_sig_{"loader_data_wr_addr_sig"};
	sc_signal<sc_uint<32>> loader_data_wr_data_sig_{"loader_data_wr_data_sig"};
	sc_signal<sc_uint<4>> loader_data_wr_strb_sig_{"loader_data_wr_strb_sig"};
	sc_signal<bool> loader_busy_sig_{"loader_busy_sig"};
	sc_signal<bool> section_loader_done_sig_{"section_loader_done_sig"};
	sc_signal<bool> loader_error_sig_{"loader_error_sig"};
	sc_signal<bool> if_req_valid_sig_{"if_req_valid_sig"};
	sc_signal<sc_uint<32>> if_addr_sig_{"if_addr_sig"};
	sc_signal<sc_uint<32>> if_rdata_sig_{"if_rdata_sig"};
	sc_signal<bool> ls_req_valid_sig_{"ls_req_valid_sig"};
	sc_signal<bool> ls_req_write_sig_{"ls_req_write_sig"};
	sc_signal<sc_uint<32>> ls_req_addr_sig_{"ls_req_addr_sig"};
	sc_signal<sc_uint<32>> ls_req_wdata_sig_{"ls_req_wdata_sig"};
	sc_signal<sc_uint<4>> ls_req_wstrb_sig_{"ls_req_wstrb_sig"};
	sc_signal<bool> ls_resp_valid_sig_{"ls_resp_valid_sig"};
	sc_signal<sc_uint<32>> ls_resp_rdata_sig_{"ls_resp_rdata_sig"};
	sc_signal<bool> mcu_cmd_req_valid_sig_{"mcu_cmd_req_valid_sig"};
	sc_signal<core::McuCmdReq> mcu_cmd_req_sig_{"mcu_cmd_req_sig"};
	sc_signal<bool> mcu_cmd_req_ready_sig_{"mcu_cmd_req_ready_sig"};
	sc_signal<bool> mcu_cmd_resp_valid_sig_{"mcu_cmd_resp_valid_sig"};
	sc_signal<core::McuCmdResp> mcu_cmd_resp_sig_{"mcu_cmd_resp_sig"};
	sc_signal<sc_uint<32>> irq_pending_lo_sig_{"irq_pending_lo_sig"};
	sc_signal<sc_uint<32>> irq_pending_hi_sig_{"irq_pending_hi_sig"};
	sc_signal<sc_uint<8>> irq_cause_id_sig_{"irq_cause_id_sig"};
	sc_signal<bool> irq_req_sig_{"irq_req_sig"};
	sc_signal<sc_uint<32>> irq_vector_sig_{"irq_vector_sig"};
	sc_signal<sc_uint<32>> irq_enable_lo_sig_{"irq_enable_lo_sig"};
	sc_signal<sc_uint<32>> irq_enable_hi_sig_{"irq_enable_hi_sig"};
	sc_signal<sc_uint<32>> irq_ack_lo_sig_{"irq_ack_lo_sig"};
	sc_signal<sc_uint<32>> irq_ack_hi_sig_{"irq_ack_hi_sig"};
	sc_signal<bool> dma_mmio_req_valid_sig_{"dma_mmio_req_valid_sig"};
	sc_signal<core::DmaMmioReq> dma_mmio_req_sig_{"dma_mmio_req_sig"};
	sc_signal<bool> dma_mmio_req_ready_sig_{"dma_mmio_req_ready_sig"};
	sc_signal<bool> dma_mmio_resp_valid_sig_{"dma_mmio_resp_valid_sig"};
	sc_signal<core::McuCmdResp> dma_mmio_resp_sig_{"dma_mmio_resp_sig"};
	sc_signal<bool> dma_stream_valid_sig_{"dma_stream_valid_sig"};
	sc_signal<sc_uint<32>> dma_stream_data_sig_{"dma_stream_data_sig"};
	sc_signal<sc_uint<8>> dma_stream_flags_sig_{"dma_stream_flags_sig"};
	sc_signal<bool> dma_stream_ready_sig_{"dma_stream_ready_sig"};
	sc_signal<bool> dma_irq_sig_{"dma_irq_sig"};
	sc_signal<bool> manual_core_enable_sig_{"manual_core_enable_sig"};
	sc_signal<sc_uint<32>> hacc_status_reg_{"hacc_status_reg"};
	sc_signal<sc_uint<32>> hacc_err_code_reg_{"hacc_err_code_reg"};
	sc_signal<sc_uint<32>> hacc_last_block_id_reg_{"hacc_last_block_id_reg"};
	sc_signal<sc_uint<32>> hacc_perf_dma_cnt_reg_{"hacc_perf_dma_cnt_reg"};
	sc_signal<sc_uint<32>> hacc_perf_mmio_cnt_reg_{"hacc_perf_mmio_cnt_reg"};

	void bind_submodules() {
		boot_host_if.clk(clk);
		boot_host_if.reset_n(reset_n);
		boot_host_if.manifest_header_o(manifest_header_sig_);
		boot_host_if.manifest_header_valid_o(manifest_header_valid_sig_);
		boot_host_if.manifest_header_ready_i(manifest_header_ready_sig_);
		boot_host_if.manifest_payload_o(manifest_payload_sig_);
		boot_host_if.manifest_payload_valid_o(manifest_payload_valid_sig_);
		boot_host_if.manifest_payload_ready_i(manifest_payload_ready_sig_);
		boot_host_if.loader_busy_i(loader_busy_sig_);
		boot_host_if.loader_done_i(section_loader_done_sig_);
		boot_host_if.loader_error_i(loader_error_sig_);
		boot_host_if.runtime_error_i(runtime_error_sig_);
		boot_host_if.core_start_o(core_start_sig_);
		boot_host_if.controller_irq_o(controller_irq_o);

		section_loader.clk(clk);
		section_loader.reset_n(reset_n);
		section_loader.manifest_header_i(manifest_header_sig_);
		section_loader.manifest_header_valid_i(manifest_header_valid_sig_);
		section_loader.manifest_header_ready_o(manifest_header_ready_sig_);
		section_loader.manifest_payload_i(manifest_payload_sig_);
		section_loader.manifest_payload_valid_i(manifest_payload_valid_sig_);
		section_loader.manifest_payload_ready_o(manifest_payload_ready_sig_);
		section_loader.isram_wr_valid_o(loader_isram_wr_valid_sig_);
		section_loader.isram_wr_addr_o(loader_isram_wr_addr_sig_);
		section_loader.isram_wr_data_o(loader_isram_wr_data_sig_);
		section_loader.isram_wr_strb_o(loader_isram_wr_strb_sig_);
		section_loader.data_wr_valid_o(loader_data_wr_valid_sig_);
		section_loader.data_wr_addr_o(loader_data_wr_addr_sig_);
		section_loader.data_wr_data_o(loader_data_wr_data_sig_);
		section_loader.data_wr_strb_o(loader_data_wr_strb_sig_);
		section_loader.loader_busy_o(loader_busy_sig_);
		section_loader.loader_done_o(section_loader_done_sig_);
		section_loader.loader_error_o(loader_error_sig_);

		isram.clk(clk);
		isram.reset_n(reset_n);
		isram.core_if_req_valid_i(if_req_valid_sig_);
		isram.core_if_addr_i(if_addr_sig_);
		isram.core_if_rdata_o(if_rdata_sig_);
		isram.loader_wr_valid_i(loader_isram_wr_valid_sig_);
		isram.loader_wr_addr_i(loader_isram_wr_addr_sig_);
		isram.loader_wr_data_i(loader_isram_wr_data_sig_);
		isram.loader_wr_strb_i(loader_isram_wr_strb_sig_);

		data_sram.clk(clk);
		data_sram.reset_n(reset_n);
		data_sram.loader_req_valid_i(loader_data_wr_valid_sig_);
		data_sram.loader_req_write_i(loader_data_wr_write_sig_);
		data_sram.loader_req_addr_i(loader_data_wr_addr_sig_);
		data_sram.loader_req_wdata_i(loader_data_wr_data_sig_);
		data_sram.loader_req_wstrb_i(loader_data_wr_strb_sig_);
		data_sram.mcu_req_valid_i(ls_req_valid_sig_);
		data_sram.mcu_req_write_i(ls_req_write_sig_);
		data_sram.mcu_req_addr_i(ls_req_addr_sig_);
		data_sram.mcu_req_wdata_i(ls_req_wdata_sig_);
		data_sram.mcu_req_wstrb_i(ls_req_wstrb_sig_);
		data_sram.mcu_resp_valid_o(ls_resp_valid_sig_);
		data_sram.mcu_resp_rdata_o(ls_resp_rdata_sig_);

		core_mcu.clk(clk);
		core_mcu.reset_n(core_reset_n_sig_);
		core_mcu.if_req_valid_o(if_req_valid_sig_);
		core_mcu.if_addr_o(if_addr_sig_);
		core_mcu.if_rdata_i(if_rdata_sig_);
		core_mcu.ls_req_valid_o(ls_req_valid_sig_);
		core_mcu.ls_req_write_o(ls_req_write_sig_);
		core_mcu.ls_req_addr_o(ls_req_addr_sig_);
		core_mcu.ls_req_wdata_o(ls_req_wdata_sig_);
		core_mcu.ls_req_wstrb_o(ls_req_wstrb_sig_);
		core_mcu.ls_resp_valid_i(ls_resp_valid_sig_);
		core_mcu.ls_resp_rdata_i(ls_resp_rdata_sig_);
		core_mcu.cmd_req_valid_o(mcu_cmd_req_valid_sig_);
		core_mcu.cmd_req_o(mcu_cmd_req_sig_);
		core_mcu.cmd_req_ready_i(mcu_cmd_req_ready_sig_);
		core_mcu.cmd_resp_valid_i(mcu_cmd_resp_valid_sig_);
		core_mcu.cmd_resp_i(mcu_cmd_resp_sig_);
		core_mcu.irq_taken_i(irq_req_sig_);
		core_mcu.irq_vector_i(irq_vector_sig_);
		core_mcu.irq_pending_lo_i(irq_pending_lo_sig_);
		core_mcu.irq_pending_hi_i(irq_pending_hi_sig_);
		core_mcu.irq_cause_id_i(irq_cause_id_sig_);
		core_mcu.irq_enable_lo_o(irq_enable_lo_sig_);
		core_mcu.irq_enable_hi_o(irq_enable_hi_sig_);
		core_mcu.irq_ack_lo_o(irq_ack_lo_sig_);
		core_mcu.irq_ack_hi_o(irq_ack_hi_sig_);

		cmd_fabric.clk(clk);
		cmd_fabric.reset_n(reset_n);
		cmd_fabric.mcu_cmd_req_valid_i(mcu_cmd_req_valid_sig_);
		cmd_fabric.mcu_cmd_req_i(mcu_cmd_req_sig_);
		cmd_fabric.mcu_cmd_req_ready_o(mcu_cmd_req_ready_sig_);
		cmd_fabric.mcu_cmd_resp_valid_o(mcu_cmd_resp_valid_sig_);
		cmd_fabric.mcu_cmd_resp_o(mcu_cmd_resp_sig_);
		cmd_fabric.dma_mmio_req_valid_o(dma_mmio_req_valid_sig_);
		cmd_fabric.dma_mmio_req_o(dma_mmio_req_sig_);
		cmd_fabric.dma_mmio_req_ready_i(dma_mmio_req_ready_sig_);
		cmd_fabric.dma_mmio_resp_valid_i(dma_mmio_resp_valid_sig_);
		cmd_fabric.dma_mmio_resp_i(dma_mmio_resp_sig_);
		cmd_fabric.dma_stream_valid_o(dma_stream_valid_sig_);
		cmd_fabric.dma_stream_data_o(dma_stream_data_sig_);
		cmd_fabric.dma_stream_flags_o(dma_stream_flags_sig_);
		cmd_fabric.dma_stream_ready_i(dma_stream_ready_sig_);
		cmd_fabric.cluster_cmd_valid_o(cluster_cmd_valid_o);
		cmd_fabric.cluster_cmd_o(cluster_cmd_o);
		cmd_fabric.cluster_cmd_ready_i(cluster_cmd_ready_i);
		cmd_fabric.nlu_cmd_valid_o(nlu_cmd_valid_o);
		cmd_fabric.nlu_cmd_o(nlu_cmd_o);
		cmd_fabric.nlu_cmd_ready_i(nlu_cmd_ready_i);

		irq_router.clk(clk);
		irq_router.reset_n(reset_n);
		irq_router.cluster_irq_i(cluster_irq_i);
		irq_router.nlu_irq_i(nlu_irq_i);
		irq_router.dma_irq_i(dma_irq_sig_);
		irq_router.irq_enable_lo_i(irq_enable_lo_sig_);
		irq_router.irq_enable_hi_i(irq_enable_hi_sig_);
		irq_router.irq_ack_lo_i(irq_ack_lo_sig_);
		irq_router.irq_ack_hi_i(irq_ack_hi_sig_);
		irq_router.irq_pending_lo_o(irq_pending_lo_sig_);
		irq_router.irq_pending_hi_o(irq_pending_hi_sig_);
		irq_router.irq_cause_id_o(irq_cause_id_sig_);
		irq_router.irq_req_o(irq_req_sig_);
		irq_router.irq_vector_o(irq_vector_sig_);

		dma_engine.clk(clk);
		dma_engine.reset_n(reset_n);
		dma_engine.mmio_req_valid_i(dma_mmio_req_valid_sig_);
		dma_engine.mmio_req_i(dma_mmio_req_sig_);
		dma_engine.mmio_req_ready_o(dma_mmio_req_ready_sig_);
		dma_engine.mmio_resp_valid_o(dma_mmio_resp_valid_sig_);
		dma_engine.mmio_resp_o(dma_mmio_resp_sig_);
		dma_engine.stream_valid_i(dma_stream_valid_sig_);
		dma_engine.stream_data_i(dma_stream_data_sig_);
		dma_engine.stream_flags_i(dma_stream_flags_sig_);
		dma_engine.stream_ready_o(dma_stream_ready_sig_);
		dma_engine.dma_req_valid_o(dma_req_valid_o);
		dma_engine.dma_req_o(dma_req_o);
		dma_engine.dma_req_ready_i(dma_req_ready_i);
		dma_engine.dma_irq_o(dma_irq_sig_);
	}

	void comb_core_reset_process() {
		core_reset_n_sig_.write(reset_n.read() && (manual_core_enable_sig_.read() || core_start_sig_.read()));
	}

	void comb_runtime_error_process() {
		runtime_error_sig_.write(false);
		loader_data_wr_write_sig_.write(true);
	}

	void seq_status_process() {
		hacc_status_reg_.write(0u);
		hacc_err_code_reg_.write(0u);
		hacc_last_block_id_reg_.write(0u);
		hacc_perf_dma_cnt_reg_.write(0u);
		hacc_perf_mmio_cnt_reg_.write(0u);
		wait();

		while (true) {
			uint32_t status = 0u;
			if (loader_busy_sig_.read() || !core_mcu.debug_is_halted()) {
				status |= core::kStatusBusyBit;
			}
			if (loader_error_sig_.read() || runtime_error_sig_.read() || (core_mcu.sr_reg.read().to_uint() & (1u << core::kSrFaultBit)) != 0u) {
				status |= core::kStatusErrorBit;
				hacc_err_code_reg_.write(core_mcu.fault_code_reg.read());
			}
			if (core_mcu.debug_is_halted() && (status & core::kStatusErrorBit) == 0u && manual_core_enable_sig_.read()) {
				status |= core::kStatusDoneBit;
			}
			if (dma_req_valid_o.read() && dma_req_ready_i.read()) {
				hacc_perf_dma_cnt_reg_.write(hacc_perf_dma_cnt_reg_.read().to_uint() + 1u);
			}
			if (cluster_cmd_valid_o.read() && cluster_cmd_ready_i.read()) {
				hacc_perf_mmio_cnt_reg_.write(hacc_perf_mmio_cnt_reg_.read().to_uint() + 1u);
			}
			if (data_sram.read_word(core::kDataSramBase + 0x100u) != 0u) {
				hacc_last_block_id_reg_.write(data_sram.read_word(core::kDataSramBase + 0x100u));
			}
			hacc_status_reg_.write(status);
			wait();
		}
	}
};

template <unsigned NUM_CLUSTERS = 4, unsigned NUM_NLU = 1, unsigned ISRAM_BYTES = 16384, unsigned DATA_SRAM_BYTES = 65536>
using Core = CoreController<NUM_CLUSTERS, NUM_NLU, ISRAM_BYTES, DATA_SRAM_BYTES>;

} // namespace hybridacc