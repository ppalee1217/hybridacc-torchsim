#pragma once

#include <systemc>
#include <array>
#include <cstdint>

#include "utils.hpp"
#include "Cluster/ScratchpadMemory.hpp"
#include "Cluster/HybridDataDeliverUnit.hpp"
#include "NetworkOnChip.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {

template <
	unsigned SPM_NUM_NOC_PORT = 4,
	unsigned SPM_BANKS_PER_GROUP = 3,
	unsigned SPM_BANK_WIDTH_BITS = 64,
	unsigned SPM_ADDR_WIDTH = 32,
	unsigned NOC_NUM_PORTS = 3,
	unsigned NOC_PORT_WIDTH_BITS = 64,
	unsigned NOC_NUM_PES_PER_PORT = 16>
SC_MODULE(ComputeCluster) {
	static_assert(SPM_NUM_NOC_PORT == 4, "ComputeCluster expects 4 SPM NoC-side ports.");
	static_assert(SPM_BANK_WIDTH_BITS == 64, "ComputeCluster DMA slave width is fixed at 64 bits.");
	static_assert(
		NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS == SPM_BANKS_PER_GROUP * SPM_BANK_WIDTH_BITS,
		"HDDU/NoC payload width must match SPM group aggregate width.");

	static constexpr unsigned HDDU_DATA_BITS = NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS;
	static constexpr unsigned HDDU_NOC_TAG_BITS = 6;
	static constexpr unsigned HDDU_NOC_ADDR_BITS = HDDU_NOC_TAG_BITS + 1;

	static constexpr uint32_t kCmdSpmBase = 0x0000;
	static constexpr uint32_t kCmdSpmSize = 0x0100;
	static constexpr uint32_t kCmdHdduBase = 0x1000;
	static constexpr uint32_t kCmdHdduSize = 0x1000;
	static constexpr uint32_t kCmdNocBase = 0x2000;
	static constexpr uint32_t kCmdNocSize = 0x0100;

	static constexpr uint32_t kSpmCfgMapOffset = 0x00;
	static constexpr uint32_t kSpmCfgUpdateOffset = 0x04;
	static constexpr uint32_t kSpmArbPolicyOffset = 0x08;

	static constexpr uint32_t kNocCmdDataOffset = 0x00;

public:
	sc_in<bool> clk;
	sc_in<bool> reset_n;
	sc_in<bool> power_enable_i;

	sc_out<bool> interrupt_o;

	// 64-bit data slave port -> SPM DMA port
	sc_in<bool> data_req_vld_i;
	sc_out<bool> data_req_rdy_o;
	sc_in<sc_uint<SPM_ADDR_WIDTH>> data_addr_i;
	sc_in<bool> data_write_i;
	sc_in<sc_biguint<SPM_BANK_WIDTH_BITS>> data_wdata_i;
	sc_out<sc_biguint<SPM_BANK_WIDTH_BITS>> data_rdata_o;
	sc_out<bool> data_done_o;

	// 32-bit MMIO/command slave port
	sc_in<bool> cmd_req_vld_i;
	sc_out<bool> cmd_req_rdy_o;
	sc_in<sc_uint<32>> cmd_addr_i;
	sc_in<bool> cmd_write_i;
	sc_in<sc_uint<32>> cmd_wdata_i;
	sc_out<sc_uint<32>> cmd_rdata_o;
	sc_out<bool> cmd_done_o;

	using noc_req_payload_t = request_t<sc_biguint<HDDU_DATA_BITS>, uint16_t>;
	using noc_resp_payload_t = response_t<sc_biguint<HDDU_DATA_BITS>>;

	cluster::ScratchpadMemory<SPM_NUM_NOC_PORT, SPM_BANKS_PER_GROUP, SPM_BANK_WIDTH_BITS, SPM_ADDR_WIDTH> spm;
	cluster::HybridDataDeliverUnit<SPM_ADDR_WIDTH, HDDU_NOC_TAG_BITS, HDDU_DATA_BITS> hddu;
	NetworkOnChip<NOC_NUM_PORTS, NOC_PORT_WIDTH_BITS, NOC_NUM_PES_PER_PORT> noc;

	SC_HAS_PROCESS(ComputeCluster);

	explicit ComputeCluster(sc_module_name name, NetWorkOnChipConfig noc_cfg = NetWorkOnChipConfig())
		: sc_module(name),
		  clk("clk"),
		  reset_n("reset_n"),
		  power_enable_i("power_enable_i"),
		  interrupt_o("interrupt_o"),
		  data_req_vld_i("data_req_vld_i"),
		  data_req_rdy_o("data_req_rdy_o"),
		  data_addr_i("data_addr_i"),
		  data_write_i("data_write_i"),
		  data_wdata_i("data_wdata_i"),
		  data_rdata_o("data_rdata_o"),
		  data_done_o("data_done_o"),
		  cmd_req_vld_i("cmd_req_vld_i"),
		  cmd_req_rdy_o("cmd_req_rdy_o"),
		  cmd_addr_i("cmd_addr_i"),
		  cmd_write_i("cmd_write_i"),
		  cmd_wdata_i("cmd_wdata_i"),
		  cmd_rdata_o("cmd_rdata_o"),
		  cmd_done_o("cmd_done_o"),
		  spm("spm"),
		  hddu("hddu"),
		  noc("noc", noc_cfg),
		  local_reset_n_sig("local_reset_n_sig"),
		  spm_cfg_map_sig("spm_cfg_map_sig"),
		  spm_cfg_update_sig("spm_cfg_update_sig"),
		  spm_arb_policy_sig("spm_arb_policy_sig"),
		  spm_noc_mode_sig("spm_noc_mode_sig", SPM_NUM_NOC_PORT),
		  hddu_spm_addr_sig("hddu_spm_addr_sig", SPM_NUM_NOC_PORT),
		  hddu_spm_req_sig("hddu_spm_req_sig", SPM_NUM_NOC_PORT),
		  hddu_spm_we_sig("hddu_spm_we_sig", SPM_NUM_NOC_PORT),
		  hddu_spm_wdata_sig("hddu_spm_wdata_sig", SPM_NUM_NOC_PORT),
		  hddu_spm_rdata_sig("hddu_spm_rdata_sig", SPM_NUM_NOC_PORT),
		  hddu_spm_ready_sig("hddu_spm_ready_sig", SPM_NUM_NOC_PORT),
		  hddu_mmio_addr_sig("hddu_mmio_addr_sig"),
		  hddu_mmio_write_sig("hddu_mmio_write_sig"),
		  hddu_mmio_wdata_sig("hddu_mmio_wdata_sig"),
		  hddu_mmio_rdata_sig("hddu_mmio_rdata_sig"),
		  hddu_interrupt_sig("hddu_interrupt_sig"),
		  noc_command_mode_sig("noc_command_mode_sig"),
		  noc_command_data_sig("noc_command_data_sig"),
		  ps_req_sig("ps_req_sig"),
		  pd_req_sig("pd_req_sig"),
		  pli_req_sig("pli_req_sig"),
		  plo_req_sig("plo_req_sig"),
		  plo_resp_sig("plo_resp_sig"),
		  spm_dma_req_vld_sig("spm_dma_req_vld_sig"),
		  spm_dma_req_rdy_sig("spm_dma_req_rdy_sig"),
		  spm_dma_rdata_sig("spm_dma_rdata_sig"),
		    spm_dma_done_sig("spm_dma_done_sig"),
		    cmd_rdata_reg_sig("cmd_rdata_reg_sig")
	{
		// Common clock/reset with power-gated local reset
		spm.clk(clk);
		spm.reset_n(local_reset_n_sig);
		hddu.clk(clk);
		hddu.reset_n(local_reset_n_sig);
		noc.clk(clk);
		noc.reset_n(local_reset_n_sig);

		// SPM config path
		spm.config_map_i(spm_cfg_map_sig);
		spm.config_update_i(spm_cfg_update_sig);
		spm.arb_policy_i(spm_arb_policy_sig);

		// SPM<->HDDU data path
		for (size_t i = 0; i < SPM_NUM_NOC_PORT; ++i) {
			hddu.spm_addr[i](hddu_spm_addr_sig[i]);
			hddu.spm_req[i](hddu_spm_req_sig[i]);
			hddu.spm_we[i](hddu_spm_we_sig[i]);
			hddu.spm_wdata[i](hddu_spm_wdata_sig[i]);
			hddu.spm_rdata[i](hddu_spm_rdata_sig[i]);
			hddu.spm_ready[i](hddu_spm_ready_sig[i]);

			spm.noc_req_vld_i[i](hddu_spm_req_sig[i]);
			spm.noc_req_rdy_o[i](hddu_spm_ready_sig[i]);
			spm.noc_addr_i[i](hddu_spm_addr_sig[i]);
			spm.noc_mode_i[i](spm_noc_mode_sig[i]);
			spm.noc_rdata_o[i](hddu_spm_rdata_sig[i]);
			spm.noc_wdata_i[i](hddu_spm_wdata_sig[i]);
			spm.noc_resp_vld_o[i](open_noc_resp_vld_sig[i]);
		}

		// External 64-bit data slave <-> SPM DMA
		spm.dma_req_vld_i(spm_dma_req_vld_sig);
		spm.dma_req_rdy_o(spm_dma_req_rdy_sig);
		spm.dma_addr_i(data_addr_i);
		spm.dma_rw_i(data_write_i);
		spm.dma_wdata_i(data_wdata_i);
		spm.dma_rdata_o(spm_dma_rdata_sig);
		spm.dma_done_o(spm_dma_done_sig);

		// HDDU MMIO
		hddu.mmio_addr(hddu_mmio_addr_sig);
		hddu.mmio_write(hddu_mmio_write_sig);
		hddu.mmio_wdata(hddu_mmio_wdata_sig);
		hddu.mmio_rdata(hddu_mmio_rdata_sig);
		hddu.interrupt(hddu_interrupt_sig);

		// NoC command sideband
		noc.command_mode(noc_command_mode_sig);
		noc.command_data(noc_command_data_sig);

		// NoC valid/ready interfaces
		connect_vr_signals(hddu.noc_ps_out, ps_req_sig);
		connect_vr_signals(hddu.noc_pd_out, pd_req_sig);
		connect_vr_signals(hddu.noc_pli_out, pli_req_sig);
		connect_vr_signals(hddu.noc_plo_out, plo_req_sig);
		connect_vr_signals(hddu.noc_plo_in, plo_resp_sig);

		connect_vr_signals(noc.noc_ps_in, ps_req_sig);
		connect_vr_signals(noc.noc_pd_in, pd_req_sig);
		connect_vr_signals(noc.noc_pli_in, pli_req_sig);
		connect_vr_signals(noc.noc_plo_in, plo_req_sig);
		connect_vr_signals(noc.noc_plo_out, plo_resp_sig);

		SC_METHOD(comb_power_and_wiring);
		sensitive << reset_n << power_enable_i;
		sensitive << data_req_vld_i << spm_dma_req_rdy_sig << spm_dma_rdata_sig << spm_dma_done_sig;
		sensitive << hddu_interrupt_sig;
		sensitive << cmd_req_vld_i << cmd_addr_i << cmd_wdata_i;
		sensitive << cmd_rdata_reg_sig;
		sensitive << ps_req_sig.ready_sig << pd_req_sig.ready_sig << pli_req_sig.ready_sig;
		sensitive << plo_resp_sig.valid_sig << plo_resp_sig.data_sig;

		SC_CTHREAD(ctrl_process, clk.pos());
		reset_signal_is(local_reset_n_sig, false);
	}

private:
	sc_signal<bool> local_reset_n_sig;

	// SPM config/control
	sc_signal<sc_uint<8>> spm_cfg_map_sig;
	sc_signal<bool> spm_cfg_update_sig;
	sc_signal<bool> spm_arb_policy_sig;

	// Fixed SPM mode (all HDDU accesses use parallel layout)
	sc_vector<sc_signal<bool>> spm_noc_mode_sig;

	// HDDU <-> SPM
	sc_vector<sc_signal<sc_uint<SPM_ADDR_WIDTH>>> hddu_spm_addr_sig;
	sc_vector<sc_signal<bool>> hddu_spm_req_sig;
	sc_vector<sc_signal<bool>> hddu_spm_we_sig;
	sc_vector<sc_signal<sc_biguint<HDDU_DATA_BITS>>> hddu_spm_wdata_sig;
	sc_vector<sc_signal<sc_biguint<HDDU_DATA_BITS>>> hddu_spm_rdata_sig;
	sc_vector<sc_signal<bool>> hddu_spm_ready_sig;
	std::array<sc_signal<bool>, SPM_NUM_NOC_PORT> open_noc_resp_vld_sig;

	// HDDU MMIO
	sc_signal<sc_uint<32>> hddu_mmio_addr_sig;
	sc_signal<bool> hddu_mmio_write_sig;
	sc_signal<sc_uint<32>> hddu_mmio_wdata_sig;
	sc_signal<sc_uint<32>> hddu_mmio_rdata_sig;
	sc_signal<bool> hddu_interrupt_sig;

	// NoC command and request path
	sc_signal<bool> noc_command_mode_sig;
	sc_signal<sc_uint<32>> noc_command_data_sig;

	// NoC interfaces as signals
	VRDSIG<noc_req_payload_t> ps_req_sig;
	VRDSIG<noc_req_payload_t> pd_req_sig;
	VRDSIG<noc_req_payload_t> pli_req_sig;
	VRDSIG<noc_addr_req_t> plo_req_sig;
	VRDSIG<noc_resp_payload_t> plo_resp_sig;

	// SPM DMA bridge
	sc_signal<bool> spm_dma_req_vld_sig;
	sc_signal<bool> spm_dma_req_rdy_sig;
	sc_signal<sc_biguint<SPM_BANK_WIDTH_BITS>> spm_dma_rdata_sig;
	sc_signal<bool> spm_dma_done_sig;

	sc_signal<sc_uint<32>> cmd_rdata_reg_sig;
	sc_uint<32> noc_last_cmd_reg{};

	static bool in_range(uint32_t addr, uint32_t base, uint32_t size) {
		return addr >= base && addr < (base + size);
	}

	void comb_power_and_wiring() {
		const bool power_on = power_enable_i.read();
		local_reset_n_sig.write(reset_n.read() && power_on);

		// External data slave gating
		spm_dma_req_vld_sig.write(power_on && data_req_vld_i.read());
		data_req_rdy_o.write(power_on && spm_dma_req_rdy_sig.read());
		data_rdata_o.write(power_on ? spm_dma_rdata_sig.read() : sc_biguint<SPM_BANK_WIDTH_BITS>(0));
		data_done_o.write(power_on && spm_dma_done_sig.read());

		// External command slave default view
		cmd_req_rdy_o.write(power_on);
		cmd_rdata_o.write(cmd_rdata_reg_sig.read());

		// Top-level interrupt
		interrupt_o.write(power_on && hddu_interrupt_sig.read());

		// HDDU MMIO pre-routing (read path and write payload)
		if (power_on && cmd_req_vld_i.read() && in_range(cmd_addr_i.read().to_uint(), kCmdHdduBase, kCmdHdduSize)) {
			hddu_mmio_addr_sig.write(cmd_addr_i.read() - kCmdHdduBase);
			hddu_mmio_wdata_sig.write(cmd_wdata_i.read());
		} else {
			hddu_mmio_addr_sig.write(0);
			hddu_mmio_wdata_sig.write(0);
		}

		// SPM NoC mode fixed to parallel for HDDU-side accesses
		for (size_t i = 0; i < SPM_NUM_NOC_PORT; ++i) {
			spm_noc_mode_sig[i].write(true);
		}

	}

	void ctrl_process() {
		spm_cfg_map_sig.write(0);
		spm_cfg_update_sig.write(false);
		spm_arb_policy_sig.write(false);

		hddu_mmio_write_sig.write(false);

		noc_command_mode_sig.write(false);
		noc_command_data_sig.write(0);

		cmd_done_o.write(false);
		cmd_rdata_reg_sig.write(0);
		noc_last_cmd_reg = 0;

		wait();

		while (true) {
			spm_cfg_update_sig.write(false);
			hddu_mmio_write_sig.write(false);
			noc_command_mode_sig.write(false);
			cmd_done_o.write(false);

			if (!power_enable_i.read()) {
				cmd_rdata_reg_sig.write(0);
				wait();
				continue;
			}

			if (cmd_req_vld_i.read() && cmd_req_rdy_o.read()) {
				const uint32_t addr = cmd_addr_i.read().to_uint();
				const bool is_write = cmd_write_i.read();
				const uint32_t wdata = cmd_wdata_i.read().to_uint();
				sc_uint<32> read_value = 0;

				if (in_range(addr, kCmdSpmBase, kCmdSpmSize)) {
					const uint32_t off = addr - kCmdSpmBase;
					if (is_write) {
						if (off == kSpmCfgMapOffset) {
							spm_cfg_map_sig.write(static_cast<sc_uint<8>>(wdata & 0xFF));
						} else if (off == kSpmCfgUpdateOffset) {
							if ((wdata & 0x1U) != 0U) {
								spm_cfg_update_sig.write(true);
							}
						} else if (off == kSpmArbPolicyOffset) {
							spm_arb_policy_sig.write((wdata & 0x1U) != 0U);
						}
					} else {
						if (off == kSpmCfgMapOffset) {
							read_value = spm_cfg_map_sig.read();
						} else if (off == kSpmCfgUpdateOffset) {
							read_value = 0;
						} else if (off == kSpmArbPolicyOffset) {
							read_value = spm_arb_policy_sig.read() ? 1U : 0U;
						}
					}
				} else if (in_range(addr, kCmdHdduBase, kCmdHdduSize)) {
					if (is_write) {
						hddu_mmio_write_sig.write(true);
					} else {
						read_value = hddu_mmio_rdata_sig.read();
					}
				} else if (in_range(addr, kCmdNocBase, kCmdNocSize)) {
					const uint32_t off = addr - kCmdNocBase;
					if (is_write) {
						if (off == kNocCmdDataOffset) {
							noc_last_cmd_reg = wdata;
							noc_command_data_sig.write(static_cast<sc_uint<32>>(wdata));
							noc_command_mode_sig.write(true);
						}
					} else {
						if (off == kNocCmdDataOffset) {
							read_value = noc_last_cmd_reg;
						}
					}
				}

				cmd_rdata_reg_sig.write(read_value);
				cmd_done_o.write(true);
			}

			wait();
		}
	}
};

} // namespace hybridacc
