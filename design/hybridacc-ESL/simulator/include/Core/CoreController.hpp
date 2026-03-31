#pragma once

#include <systemc>

#include <cstdint>

#include "Core/BootHostIf.hpp"
#include "Core/CmdFabric.hpp"
#include "Core/CoreMcu.hpp"
#include "Core/DataSram.hpp"
#include "Core/DmaEngine.hpp"
#include "Core/Isram.hpp"
#include "Core/Plic.hpp"
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
	sc_out<bool> cluster_req_valid_o{"cluster_req_valid_o"};
	sc_out<core::ClusterMmioRequest> cluster_req_o{"cluster_req_o"};
	sc_in<bool> cluster_req_ready_i{"cluster_req_ready_i"};
	sc_out<bool> nlu_req_valid_o{"nlu_req_valid_o"};
	sc_out<core::NluMmioRequest> nlu_req_o{"nlu_req_o"};
	sc_in<bool> nlu_req_ready_i{"nlu_req_ready_i"};
	sc_out<bool> controller_irq_o{"controller_irq_o"};

	core::BootHostIf boot_host_if{"boot_host_if"};
	core::SectionLoader section_loader{"section_loader"};
	core::Isram<ISRAM_BYTES> isram{"isram"};
	core::DataSram<DATA_SRAM_BYTES> data_sram{"data_sram"};
	core::CoreMcu<DATA_SRAM_BYTES> core_mcu{"core_mcu"};
	core::CmdFabric cmd_fabric{"cmd_fabric"};
	core::Plic<NUM_CLUSTERS, NUM_NLU> plic{"plic"};
	core::DmaEngine dma_engine{"dma_engine"};

	SC_CTOR(CoreController) {
		bind_submodules();

		SC_METHOD(comb_output_process);
		sensitive << dma_req_valid_sig_ << dma_req_sig_ << cluster_req_valid_sig_ << cluster_req_sig_ << nlu_req_valid_sig_ << nlu_req_sig_;
	}

	void load_instruction(uint32_t byte_addr, uint32_t value) { isram.load_instruction(byte_addr, value); }
	void load_data_word(uint32_t byte_addr, uint32_t value) { data_sram.load_word(byte_addr, value); }
	uint32_t read_data_word(uint32_t byte_addr) const { return data_sram.read_word(byte_addr); }
	void host_push_manifest(const core::ManifestPacket& packet) { boot_host_if.host_push_manifest(packet); }
	void host_set_boot_addr(uint32_t addr) { boot_host_if.set_core_boot_addr(addr); }
	void host_set_trap_vector(uint32_t addr) { boot_host_if.set_core_trap_vector(addr); }
	void host_set_core_enable(bool enable) { boot_host_if.set_core_enable(enable); }
	uint32_t debug_read_pc() const { return core_mcu.debug_read_pc(); }
	uint32_t debug_read_gpr(uint32_t index) const { return core_mcu.debug_read_gpr(index); }
	bool debug_is_halted() const { return core_mcu.debug_is_halted(); }
	core::MmioRequest debug_last_mmio_request() const { return core_mcu.debug_last_mmio_request(); }
	core::ClusterMmioRequest debug_last_cluster_request() const { return cmd_fabric.last_cluster_request(); }
	uint32_t debug_cluster_mask_lo() const { return cmd_fabric.cluster_mask_lo(); }

private:
	sc_signal<core::ManifestHeader> manifest_header_sig_{"manifest_header_sig"};
	sc_signal<bool> manifest_header_valid_sig_{"manifest_header_valid_sig"};
	sc_signal<bool> manifest_header_ready_sig_{"manifest_header_ready_sig"};
	sc_signal<core::ManifestPayloadBeat> manifest_payload_sig_{"manifest_payload_sig"};
	sc_signal<bool> manifest_payload_valid_sig_{"manifest_payload_valid_sig"};
	sc_signal<bool> manifest_payload_ready_sig_{"manifest_payload_ready_sig"};
	sc_signal<bool> loader_busy_sig_{"loader_busy_sig"};
	sc_signal<bool> loader_done_sig_{"loader_done_sig"};
	sc_signal<bool> loader_error_sig_{"loader_error_sig"};
	sc_signal<bool> core_enable_sig_{"core_enable_sig"};
	sc_signal<sc_uint<32>> core_boot_addr_sig_{"core_boot_addr_sig"};
	sc_signal<sc_uint<32>> core_trap_vector_sig_{"core_trap_vector_sig"};

	sc_signal<bool> if_req_valid_sig_{"if_req_valid_sig"};
	sc_signal<sc_uint<32>> if_addr_sig_{"if_addr_sig"};
	sc_signal<bool> if_resp_valid_sig_{"if_resp_valid_sig"};
	sc_signal<sc_uint<32>> if_rdata_sig_{"if_rdata_sig"};
	sc_signal<bool> ls_req_valid_sig_{"ls_req_valid_sig"};
	sc_signal<bool> ls_req_write_sig_{"ls_req_write_sig"};
	sc_signal<sc_uint<32>> ls_req_addr_sig_{"ls_req_addr_sig"};
	sc_signal<sc_uint<32>> ls_req_wdata_sig_{"ls_req_wdata_sig"};
	sc_signal<sc_uint<4>> ls_req_wstrb_sig_{"ls_req_wstrb_sig"};
	sc_signal<bool> ls_resp_valid_sig_{"ls_resp_valid_sig"};
	sc_signal<sc_uint<32>> ls_resp_rdata_sig_{"ls_resp_rdata_sig"};
	sc_signal<bool> mmio_req_valid_sig_{"mmio_req_valid_sig"};
	sc_signal<core::MmioRequest> mmio_req_sig_{"mmio_req_sig"};
	sc_signal<bool> mmio_resp_valid_sig_{"mmio_resp_valid_sig"};
	sc_signal<core::MmioResponse> mmio_resp_sig_{"mmio_resp_sig"};

	sc_signal<bool> plic_resp_valid_sig_{"plic_resp_valid_sig"};
	sc_signal<core::MmioResponse> plic_resp_sig_{"plic_resp_sig"};
	sc_signal<bool> plic_req_valid_sig_{"plic_req_valid_sig"};
	sc_signal<core::MmioRequest> plic_req_sig_{"plic_req_sig"};
	sc_signal<bool> dma_mmio_req_valid_sig_{"dma_mmio_req_valid_sig"};
	sc_signal<core::DmaMmioRequest> dma_mmio_req_sig_{"dma_mmio_req_sig"};
	sc_signal<bool> dma_mmio_resp_valid_sig_{"dma_mmio_resp_valid_sig"};
	sc_signal<core::MmioResponse> dma_mmio_resp_sig_{"dma_mmio_resp_sig"};
	sc_signal<bool> dma_stream_valid_sig_{"dma_stream_valid_sig"};
	sc_signal<sc_uint<32>> dma_stream_data_sig_{"dma_stream_data_sig"};
	sc_signal<bool> dma_stream_ready_sig_{"dma_stream_ready_sig"};
	sc_signal<bool> dma_irq_sig_{"dma_irq_sig"};
	sc_signal<bool> plic_meip_sig_{"plic_meip_sig"};
	sc_signal<bool> mtip_sig_{"mtip_sig"};
	sc_signal<sc_uint<32>> plic_pending_lo_sig_{"plic_pending_lo_sig"};
	sc_signal<bool> dma_req_valid_sig_{"dma_req_valid_sig"};
	sc_signal<core::DmaRequest> dma_req_sig_{"dma_req_sig"};
	sc_signal<bool> cluster_req_valid_sig_{"cluster_req_valid_sig"};
	sc_signal<core::ClusterMmioRequest> cluster_req_sig_{"cluster_req_sig"};
	sc_signal<bool> nlu_req_valid_sig_{"nlu_req_valid_sig"};
	sc_signal<core::NluMmioRequest> nlu_req_sig_{"nlu_req_sig"};

	sc_signal<bool> isram_loader_wr_valid_sig_{"isram_loader_wr_valid_sig"};
	sc_signal<sc_uint<32>> isram_loader_wr_addr_sig_{"isram_loader_wr_addr_sig"};
	sc_signal<sc_uint<32>> isram_loader_wr_data_sig_{"isram_loader_wr_data_sig"};
	sc_signal<sc_uint<4>> isram_loader_wr_strb_sig_{"isram_loader_wr_strb_sig"};
	sc_signal<bool> data_loader_wr_valid_sig_{"data_loader_wr_valid_sig"};
	sc_signal<sc_uint<32>> data_loader_wr_addr_sig_{"data_loader_wr_addr_sig"};
	sc_signal<sc_uint<32>> data_loader_wr_data_sig_{"data_loader_wr_data_sig"};
	sc_signal<sc_uint<4>> data_loader_wr_strb_sig_{"data_loader_wr_strb_sig"};

	void comb_output_process() {
		dma_req_valid_o.write(dma_req_valid_sig_.read());
		dma_req_o.write(dma_req_sig_.read());
		cluster_req_valid_o.write(cluster_req_valid_sig_.read());
		cluster_req_o.write(cluster_req_sig_.read());
		nlu_req_valid_o.write(nlu_req_valid_sig_.read());
		nlu_req_o.write(nlu_req_sig_.read());
	}

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
		boot_host_if.loader_done_i(loader_done_sig_);
		boot_host_if.loader_error_i(loader_error_sig_);
		boot_host_if.runtime_error_i(dma_irq_sig_);
		boot_host_if.core_enable_o(core_enable_sig_);
		boot_host_if.core_boot_addr_o(core_boot_addr_sig_);
		boot_host_if.core_trap_vector_o(core_trap_vector_sig_);
		boot_host_if.controller_irq_o(controller_irq_o);

		section_loader.clk(clk);
		section_loader.reset_n(reset_n);
		section_loader.manifest_header_i(manifest_header_sig_);
		section_loader.manifest_header_valid_i(manifest_header_valid_sig_);
		section_loader.manifest_header_ready_o(manifest_header_ready_sig_);
		section_loader.manifest_payload_i(manifest_payload_sig_);
		section_loader.manifest_payload_valid_i(manifest_payload_valid_sig_);
		section_loader.manifest_payload_ready_o(manifest_payload_ready_sig_);
		section_loader.isram_wr_valid_o(isram_loader_wr_valid_sig_);
		section_loader.isram_wr_addr_o(isram_loader_wr_addr_sig_);
		section_loader.isram_wr_data_o(isram_loader_wr_data_sig_);
		section_loader.isram_wr_strb_o(isram_loader_wr_strb_sig_);
		section_loader.data_wr_valid_o(data_loader_wr_valid_sig_);
		section_loader.data_wr_addr_o(data_loader_wr_addr_sig_);
		section_loader.data_wr_data_o(data_loader_wr_data_sig_);
		section_loader.data_wr_strb_o(data_loader_wr_strb_sig_);
		section_loader.loader_busy_o(loader_busy_sig_);
		section_loader.loader_done_o(loader_done_sig_);
		section_loader.loader_error_o(loader_error_sig_);

		isram.clk(clk);
		isram.reset_n(reset_n);
		isram.core_if_req_valid_i(if_req_valid_sig_);
		isram.core_if_addr_i(if_addr_sig_);
		isram.core_if_resp_valid_o(if_resp_valid_sig_);
		isram.core_if_rdata_o(if_rdata_sig_);
		isram.loader_wr_valid_i(isram_loader_wr_valid_sig_);
		isram.loader_wr_addr_i(isram_loader_wr_addr_sig_);
		isram.loader_wr_data_i(isram_loader_wr_data_sig_);
		isram.loader_wr_strb_i(isram_loader_wr_strb_sig_);

		data_sram.clk(clk);
		data_sram.reset_n(reset_n);
		data_sram.loader_req_valid_i(data_loader_wr_valid_sig_);
		data_sram.loader_req_addr_i(data_loader_wr_addr_sig_);
		data_sram.loader_req_wdata_i(data_loader_wr_data_sig_);
		data_sram.loader_req_wstrb_i(data_loader_wr_strb_sig_);
		data_sram.mcu_req_valid_i(ls_req_valid_sig_);
		data_sram.mcu_req_write_i(ls_req_write_sig_);
		data_sram.mcu_req_addr_i(ls_req_addr_sig_);
		data_sram.mcu_req_wdata_i(ls_req_wdata_sig_);
		data_sram.mcu_req_wstrb_i(ls_req_wstrb_sig_);
		data_sram.mcu_resp_valid_o(ls_resp_valid_sig_);
		data_sram.mcu_resp_rdata_o(ls_resp_rdata_sig_);

		core_mcu.clk(clk);
		core_mcu.reset_n(reset_n);
		core_mcu.enable_i(core_enable_sig_);
		core_mcu.boot_addr_i(core_boot_addr_sig_);
		core_mcu.trap_vector_i(core_trap_vector_sig_);
		core_mcu.if_req_valid_o(if_req_valid_sig_);
		core_mcu.if_addr_o(if_addr_sig_);
		core_mcu.if_resp_valid_i(if_resp_valid_sig_);
		core_mcu.if_rdata_i(if_rdata_sig_);
		core_mcu.ls_req_valid_o(ls_req_valid_sig_);
		core_mcu.ls_req_write_o(ls_req_write_sig_);
		core_mcu.ls_req_addr_o(ls_req_addr_sig_);
		core_mcu.ls_req_wdata_o(ls_req_wdata_sig_);
		core_mcu.ls_req_wstrb_o(ls_req_wstrb_sig_);
		core_mcu.ls_resp_valid_i(ls_resp_valid_sig_);
		core_mcu.ls_resp_rdata_i(ls_resp_rdata_sig_);
		core_mcu.mmio_req_valid_o(mmio_req_valid_sig_);
		core_mcu.mmio_req_o(mmio_req_sig_);
		core_mcu.mmio_resp_valid_i(mmio_resp_valid_sig_);
		core_mcu.mmio_resp_i(mmio_resp_sig_);
		core_mcu.irq_meip_i(plic_meip_sig_);
		core_mcu.irq_mtip_i(mtip_sig_);

		cmd_fabric.clk(clk);
		cmd_fabric.reset_n(reset_n);
		cmd_fabric.core_mmio_req_valid_i(mmio_req_valid_sig_);
		cmd_fabric.core_mmio_req_i(mmio_req_sig_);
		cmd_fabric.core_mmio_resp_valid_o(mmio_resp_valid_sig_);
		cmd_fabric.core_mmio_resp_o(mmio_resp_sig_);
		cmd_fabric.dma_mmio_req_valid_o(dma_mmio_req_valid_sig_);
		cmd_fabric.dma_mmio_req_o(dma_mmio_req_sig_);
		cmd_fabric.dma_mmio_resp_valid_i(dma_mmio_resp_valid_sig_);
		cmd_fabric.dma_mmio_resp_i(dma_mmio_resp_sig_);
		cmd_fabric.dma_stream_valid_o(dma_stream_valid_sig_);
		cmd_fabric.dma_stream_data_o(dma_stream_data_sig_);
		cmd_fabric.cluster_req_valid_o(cluster_req_valid_sig_);
		cmd_fabric.cluster_req_o(cluster_req_sig_);
		cmd_fabric.cluster_req_ready_i(cluster_req_ready_i);
		cmd_fabric.nlu_req_valid_o(nlu_req_valid_sig_);
		cmd_fabric.nlu_req_o(nlu_req_sig_);
		cmd_fabric.nlu_req_ready_i(nlu_req_ready_i);
		cmd_fabric.plic_mmio_req_valid_o(plic_req_valid_sig_);
		cmd_fabric.plic_mmio_req_o(plic_req_sig_);
		cmd_fabric.plic_mmio_resp_valid_i(plic_resp_valid_sig_);
		cmd_fabric.plic_mmio_resp_i(plic_resp_sig_);

		plic.clk(clk);
		plic.reset_n(reset_n);
		plic.cluster_irq_i(cluster_irq_i);
		plic.nlu_irq_i(nlu_irq_i);
		plic.dma_irq_i(dma_irq_sig_);
		plic.mmio_req_valid_i(plic_req_valid_sig_);
		plic.mmio_req_i(plic_req_sig_);
		plic.mmio_resp_valid_o(plic_resp_valid_sig_);
		plic.mmio_resp_o(plic_resp_sig_);
		plic.meip_o(plic_meip_sig_);
		plic.pending_lo_o(plic_pending_lo_sig_);

		dma_engine.clk(clk);
		dma_engine.reset_n(reset_n);
		dma_engine.mmio_req_valid_i(dma_mmio_req_valid_sig_);
		dma_engine.mmio_req_i(dma_mmio_req_sig_);
		dma_engine.mmio_resp_valid_o(dma_mmio_resp_valid_sig_);
		dma_engine.mmio_resp_o(dma_mmio_resp_sig_);
		dma_engine.stream_valid_i(dma_stream_valid_sig_);
		dma_engine.stream_data_i(dma_stream_data_sig_);
		dma_engine.stream_ready_o(dma_stream_ready_sig_);
		dma_engine.dma_req_valid_o(dma_req_valid_sig_);
		dma_engine.dma_req_o(dma_req_sig_);
		dma_engine.dma_req_ready_i(dma_req_ready_i);
		dma_engine.dma_irq_o(dma_irq_sig_);

		mtip_sig_.write(false);
	}
};

} // namespace hybridacc