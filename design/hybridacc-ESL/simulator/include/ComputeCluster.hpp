#pragma once

#include <systemc>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "utils.hpp"
#include "AXI4_lite/axi4-lite.hpp"
#include "Cluster/ScratchpadMemory.hpp"
#include "Cluster/HybridDataDeliverUnit.hpp"
#include "NetworkOnChip.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {

template <
	unsigned SPM_NUM_NOC_CHANNEL = 4,
	unsigned SPM_NUM_BANKS_PER_GROUP = 3,
	unsigned SPM_SRAM_BANK_WIDTH_BITS = 64,
	unsigned SPM_SRAM_BANK_DEPTH_WORDS = 8192,
	unsigned SPM_SRAM_BANK_LATENCY = 1,
	unsigned SPM_SRAM_BANK_PIPELINE_DEPTH = 1,
	unsigned SPM_ADDR_WIDTH = 32,
	unsigned NOC_NUM_PORTS = 3,
	unsigned NOC_PORT_WIDTH_BITS = 64,
	unsigned NOC_NUM_PES_PER_PORT = 16>
SC_MODULE(ComputeCluster) {
	static_assert(SPM_NUM_NOC_CHANNEL == 4, "ComputeCluster expects 4 SPM NoC-side channels.");
	static_assert(SPM_SRAM_BANK_WIDTH_BITS == 64, "ComputeCluster DMA slave width is fixed at 64 bits.");
	static_assert(
		NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS == SPM_NUM_BANKS_PER_GROUP * SPM_SRAM_BANK_WIDTH_BITS,
		"HDDU/NoC payload width must match SPM group aggregate width.");

	static constexpr unsigned HDDU_DATA_BITS = NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS;
	static constexpr unsigned HDDU_NOC_TAG_BITS = 6;
	static constexpr unsigned HDDU_NOC_ADDR_BITS = HDDU_NOC_TAG_BITS + 1;

	using spm_req_payload_t = spm_request_t<SPM_ADDR_WIDTH, HDDU_DATA_BITS>;
	using spm_resp_payload_t = spm_response_t<HDDU_DATA_BITS>;

	static constexpr uint32_t kCmdSpmBase = 0x0000;
	static constexpr uint32_t kCmdSpmSize = 0x0100;
	static constexpr uint32_t kCmdHdduBase = 0x1000;
	static constexpr uint32_t kCmdHdduSize = 0x1000;
	static constexpr uint32_t kCmdNocBase = 0x2000;
	static constexpr uint32_t kCmdNocSize = 0x0100;

	static constexpr uint32_t kSpmCfgMapOffset = 0x00;
	static constexpr uint32_t kSpmCfgUpdateOffset = 0x04;
	static constexpr uint32_t kSpmArbPolicyOffset = 0x08;
	static constexpr uint32_t kSpmPmuCtrlOffset = 0x0C;
	static constexpr uint32_t kSpmPmuCycleCntLoOffset = 0x10;
	static constexpr uint32_t kSpmPmuCycleCntHiOffset = 0x14;
	static constexpr uint32_t kSpmPmuArbStallLoOffset = 0x18;
	static constexpr uint32_t kSpmPmuArbStallHiOffset = 0x1C;
	static constexpr uint32_t kSpmPmuCreditStallLoOffset = 0x20;
	static constexpr uint32_t kSpmPmuCreditStallHiOffset = 0x24;
	static constexpr uint32_t kSpmPmuPortTxnBaseOffset = 0x40;
	static constexpr uint32_t kSpmPmuPortTxnStride = 0x08;

	static constexpr uint32_t kNocCmdDataOffset = 0x00;

public:
	sc_in<bool> clk;
	sc_in<bool> reset_n;
	sc_in<bool> power_enable_i;

	sc_out<bool> interrupt_o;

	// AXI4-Lite slave DATA port -> SPM DMA port
	sc_in<bool> s_axi_awvalid_i;
	sc_out<bool> s_axi_awready_o;
	sc_in<sc_uint<SPM_ADDR_WIDTH>> s_axi_awaddr_i;

	sc_in<bool> s_axi_wvalid_i;
	sc_out<bool> s_axi_wready_o;
	sc_in<sc_biguint<SPM_SRAM_BANK_WIDTH_BITS>> s_axi_wdata_i;
	sc_in<sc_uint<SPM_SRAM_BANK_WIDTH_BITS / 8>> s_axi_wstrb_i;

	sc_out<bool> s_axi_bvalid_o;
	sc_in<bool> s_axi_bready_i;
	sc_out<sc_uint<2>> s_axi_bresp_o;

	sc_in<bool> s_axi_arvalid_i;
	sc_out<bool> s_axi_arready_o;
	sc_in<sc_uint<SPM_ADDR_WIDTH>> s_axi_araddr_i;

	sc_out<bool> s_axi_rvalid_o;
	sc_in<bool> s_axi_rready_i;
	sc_out<sc_biguint<SPM_SRAM_BANK_WIDTH_BITS>> s_axi_rdata_o;
	sc_out<sc_uint<2>> s_axi_rresp_o;

	// AHB-Lite Slave Port
	sc_in<bool> hsel_i;
	sc_in<sc_uint<32>> haddr_i;
	sc_in<bool> hwrite_i;
	sc_in<sc_uint<2>> htrans_i;
	sc_in<sc_uint<3>> hsize_i;
	sc_in<sc_uint<3>> hburst_i;
	sc_in<sc_uint<4>> hprot_i;
	sc_in<bool> hready_i;
	sc_in<sc_uint<32>> hwdata_i;

	sc_out<bool> hready_o;
	sc_out<bool> hresp_o;
	sc_out<sc_uint<32>> hrdata_o;

	using noc_req_payload_t = request_t<sc_biguint<HDDU_DATA_BITS>, uint16_t>;
	using noc_resp_payload_t = response_t<sc_biguint<HDDU_DATA_BITS>>;

	cluster::ScratchpadMemory<
		SPM_NUM_NOC_CHANNEL,
		SPM_NUM_BANKS_PER_GROUP,
		SPM_SRAM_BANK_WIDTH_BITS,
		SPM_SRAM_BANK_DEPTH_WORDS,
		SPM_SRAM_BANK_LATENCY,
		SPM_SRAM_BANK_PIPELINE_DEPTH,
		SPM_ADDR_WIDTH> spm;

	cluster::HybridDataDeliverUnit<
		SPM_ADDR_WIDTH,
		HDDU_NOC_TAG_BITS,
		HDDU_DATA_BITS> hddu;

	NetworkOnChip<
		NOC_NUM_PORTS,
		NOC_PORT_WIDTH_BITS,
		NOC_NUM_PES_PER_PORT> noc;

	SC_HAS_PROCESS(ComputeCluster);

	explicit ComputeCluster(sc_module_name name, NetWorkOnChipConfig noc_cfg = NetWorkOnChipConfig())
		: sc_module(name),
		  clk("clk"),
		  reset_n("reset_n"),
		  power_enable_i("power_enable_i"),
		  interrupt_o("interrupt_o"),
		  s_axi_awvalid_i("s_axi_awvalid_i"),
		  s_axi_awready_o("s_axi_awready_o"),
		  s_axi_awaddr_i("s_axi_awaddr_i"),
		  s_axi_wvalid_i("s_axi_wvalid_i"),
		  s_axi_wready_o("s_axi_wready_o"),
		  s_axi_wdata_i("s_axi_wdata_i"),
		  s_axi_wstrb_i("s_axi_wstrb_i"),
		  s_axi_bvalid_o("s_axi_bvalid_o"),
		  s_axi_bready_i("s_axi_bready_i"),
		  s_axi_bresp_o("s_axi_bresp_o"),
		  s_axi_arvalid_i("s_axi_arvalid_i"),
		  s_axi_arready_o("s_axi_arready_o"),
		  s_axi_araddr_i("s_axi_araddr_i"),
		  s_axi_rvalid_o("s_axi_rvalid_o"),
		  s_axi_rready_i("s_axi_rready_i"),
		  s_axi_rdata_o("s_axi_rdata_o"),
		  s_axi_rresp_o("s_axi_rresp_o"),
		  hsel_i("hsel_i"),
		  haddr_i("haddr_i"),
		  hwrite_i("hwrite_i"),
		  htrans_i("htrans_i"),
		  hsize_i("hsize_i"),
		  hburst_i("hburst_i"),
		  hprot_i("hprot_i"),
		  hready_i("hready_i"),
		  hwdata_i("hwdata_i"),
		  hready_o("hready_o"),
		  hresp_o("hresp_o"),
		  hrdata_o("hrdata_o"),
		  spm("spm"),
		  hddu("hddu"),
		  noc("noc", noc_cfg),
		  local_reset_n_sig("local_reset_n_sig"),
		  spm_cfg_map_sig("spm_cfg_map_sig"),
		  spm_cfg_update_sig("spm_cfg_update_sig"),
		  spm_arb_policy_sig("spm_arb_policy_sig"),
		  hddu_spm_req_valid_sig("hddu_spm_req_valid_sig", SPM_NUM_NOC_CHANNEL),
		  hddu_spm_req_ready_sig("hddu_spm_req_ready_sig", SPM_NUM_NOC_CHANNEL),
		  hddu_spm_req_payload_sig("hddu_spm_req_payload_sig", SPM_NUM_NOC_CHANNEL),
		  hddu_spm_resp_valid_sig("hddu_spm_resp_valid_sig", SPM_NUM_NOC_CHANNEL),
		  hddu_spm_resp_ready_sig("hddu_spm_resp_ready_sig", SPM_NUM_NOC_CHANNEL),
		  hddu_spm_resp_payload_sig("hddu_spm_resp_payload_sig", SPM_NUM_NOC_CHANNEL),
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
		  spm_axi_awvalid_sig("spm_axi_awvalid_sig"),
		  spm_axi_awready_sig("spm_axi_awready_sig"),
		  spm_axi_awaddr_sig("spm_axi_awaddr_sig"),
		  spm_axi_wvalid_sig("spm_axi_wvalid_sig"),
		  spm_axi_wready_sig("spm_axi_wready_sig"),
		  spm_axi_wdata_sig("spm_axi_wdata_sig"),
		  spm_axi_wstrb_sig("spm_axi_wstrb_sig"),
		  spm_axi_bvalid_sig("spm_axi_bvalid_sig"),
		  spm_axi_bready_sig("spm_axi_bready_sig"),
		  spm_axi_bresp_sig("spm_axi_bresp_sig"),
		  spm_axi_arvalid_sig("spm_axi_arvalid_sig"),
		  spm_axi_arready_sig("spm_axi_arready_sig"),
		  spm_axi_araddr_sig("spm_axi_araddr_sig"),
		  spm_axi_rvalid_sig("spm_axi_rvalid_sig"),
		  spm_axi_rready_sig("spm_axi_rready_sig"),
		  spm_axi_rdata_sig("spm_axi_rdata_sig"),
		  spm_axi_rresp_sig("spm_axi_rresp_sig"),
		  ahb_active_reg("ahb_active_reg"),
		  ahb_write_reg("ahb_write_reg"),
		  ahb_addr_reg("ahb_addr_reg"),
		  ahb_hsize_reg("ahb_hsize_reg"),
		  ahb_rdata_reg("ahb_rdata_reg")
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
		spm.pmu_rst_i(spm_pmu_rst_sig);
		spm.pmu_cycle_cnt_o(spm_pmu_cycle_cnt_sig);
		spm.pmu_arb_stall_cnt_o(spm_pmu_arb_stall_cnt_sig);
		spm.pmu_credit_stall_cnt_o(spm_pmu_credit_stall_cnt_sig);
		for (size_t i = 0; i < SPM_NUM_NOC_CHANNEL; ++i) {
			spm.pmu_port_txn_cnt_o[i](spm_pmu_port_txn_cnt_sig[i]);
		}

		// SPM<->HDDU data path
		for (size_t i = 0; i < SPM_NUM_NOC_CHANNEL; ++i) {
			hddu.spm_req_valid[i](hddu_spm_req_valid_sig[i]);
			hddu.spm_req_ready[i](hddu_spm_req_ready_sig[i]);
			hddu.spm_req_payload[i](hddu_spm_req_payload_sig[i]);
			hddu.spm_resp_valid[i](hddu_spm_resp_valid_sig[i]);
			hddu.spm_resp_ready[i](hddu_spm_resp_ready_sig[i]);
			hddu.spm_resp_payload[i](hddu_spm_resp_payload_sig[i]);

			spm.spm_req_valid_i[i](hddu_spm_req_valid_sig[i]);
			spm.spm_req_ready_o[i](hddu_spm_req_ready_sig[i]);
			spm.spm_req_i[i](hddu_spm_req_payload_sig[i]);
			spm.spm_resp_valid_o[i](hddu_spm_resp_valid_sig[i]);
			spm.spm_resp_ready_i[i](hddu_spm_resp_ready_sig[i]);
			spm.spm_resp_o[i](hddu_spm_resp_payload_sig[i]);
		}

		// External 64-bit data slave <-> SPM DMA
		spm.s_axi_awvalid_i(spm_axi_awvalid_sig);
		spm.s_axi_awready_o(spm_axi_awready_sig);
		spm.s_axi_awaddr_i(spm_axi_awaddr_sig);
		spm.s_axi_wvalid_i(spm_axi_wvalid_sig);
		spm.s_axi_wready_o(spm_axi_wready_sig);
		spm.s_axi_wdata_i(spm_axi_wdata_sig);
		spm.s_axi_wstrb_i(spm_axi_wstrb_sig);
		spm.s_axi_bvalid_o(spm_axi_bvalid_sig);
		spm.s_axi_bready_i(spm_axi_bready_sig);
		spm.s_axi_bresp_o(spm_axi_bresp_sig);
		spm.s_axi_arvalid_i(spm_axi_arvalid_sig);
		spm.s_axi_arready_o(spm_axi_arready_sig);
		spm.s_axi_araddr_i(spm_axi_araddr_sig);
		spm.s_axi_rvalid_o(spm_axi_rvalid_sig);
		spm.s_axi_rready_i(spm_axi_rready_sig);
		spm.s_axi_rdata_o(spm_axi_rdata_sig);
		spm.s_axi_rresp_o(spm_axi_rresp_sig);

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
		sensitive << s_axi_awvalid_i << s_axi_awaddr_i;
		sensitive << s_axi_wvalid_i << s_axi_wdata_i << s_axi_wstrb_i;
		sensitive << s_axi_bready_i << s_axi_arvalid_i << s_axi_araddr_i << s_axi_rready_i;
		sensitive << spm_axi_awready_sig << spm_axi_wready_sig << spm_axi_bvalid_sig << spm_axi_bresp_sig;
		sensitive << spm_axi_arready_sig << spm_axi_rvalid_sig << spm_axi_rdata_sig << spm_axi_rresp_sig;
		sensitive << hddu_interrupt_sig;
		sensitive << ahb_active_reg << ahb_addr_reg << ahb_write_reg << hwdata_i;
		sensitive << ps_req_sig.ready_sig << pd_req_sig.ready_sig << pli_req_sig.ready_sig;
		sensitive << plo_resp_sig.valid_sig << plo_resp_sig.data_sig;
		sensitive << spm_cfg_map_sig << spm_arb_policy_sig << hddu_mmio_rdata_sig;

		SC_CTHREAD(seq_ahb_ctrl, clk.pos());
		reset_signal_is(local_reset_n_sig, false);

		SC_METHOD(trace_process);
		sensitive << clk.pos();
	}

	void set_trace_context(uint32_t pid, int tid_base) {
		trace_pid = pid;
		trace_id = tid_base;
		trace_init = false;
		last_state_cluster = "IDLE";
		last_state_cmd = "IDLE";
		last_state_dma = "IDLE";
	}

	int get_trace_num() const { return 4; }

	std::pair<uint32_t, uint32_t> enable_perffeto_trace(uint32_t start_pid = 100, uint32_t start_tid = 1000) {
		set_trace_context(start_pid, static_cast<int>(start_tid));

		uint32_t next_pid = start_pid + 1;
		uint32_t next_tid = start_tid + static_cast<uint32_t>(get_trace_num() + 1);

		auto spm_next = spm.enable_perffeto_trace(next_pid, next_tid);
		next_pid = spm_next.first;
		next_tid = spm_next.second;

		auto hddu_next = hddu.enable_perffeto_trace(next_pid, next_tid);
		next_pid = hddu_next.first;
		next_tid = hddu_next.second;

		auto noc_next = noc.enable_perffeto_trace(next_pid, next_tid);
		next_pid = noc_next.first;
		next_tid = noc_next.second;

		return {next_pid, next_tid};
	}

private:
	int trace_id = -1;
	uint32_t trace_pid = 0;
	bool trace_init = false;
	std::string last_state_cluster = "IDLE";
	std::string last_state_cmd = "IDLE";
	std::string last_state_dma = "IDLE";

	void trace_process() {
		if (trace_id < 0) return;

		const uint32_t tid_cluster = static_cast<uint32_t>(trace_id + 1);
		const uint32_t tid_cmd = static_cast<uint32_t>(trace_id + 2);
		const uint32_t tid_dma = static_cast<uint32_t>(trace_id + 3);

		auto bool_to_json = [](bool v) { return v ? "true" : "false"; };
		auto trace_state = [&](std::string& last, const std::string& current,
							   const std::string& cat, uint32_t tid,
							   const std::string& args) {
			if (current != last) {
				TRACE_EVENT(last, cat, TRACE_END, trace_pid, tid, "{}");
				TRACE_EVENT(current, cat, TRACE_BEGIN, trace_pid, tid, args);
				last = current;
			}
		};

		if (!trace_init) {
			TRACE_THREAD_NAME(trace_pid, tid_cluster, std::string(name()) + " Cluster");
			TRACE_THREAD_NAME(trace_pid, tid_cmd, std::string(name()) + " Cmd");
			TRACE_THREAD_NAME(trace_pid, tid_dma, std::string(name()) + " DMA");

			TRACE_EVENT(last_state_cluster, "Cluster_State", TRACE_BEGIN, trace_pid, tid_cluster, "{}");
			TRACE_EVENT(last_state_cmd, "Cluster_AHB", TRACE_BEGIN, trace_pid, tid_cmd, "{}");
			TRACE_EVENT(last_state_dma, "Cluster_DMA", TRACE_BEGIN, trace_pid, tid_dma, "{}");
			trace_init = true;
		}

		const bool power_on = power_enable_i.read();
		const bool rst_n = reset_n.read();
		const std::string cluster_state = !power_on ? "POWER_OFF" : (rst_n ? "RUN" : "RESET");
		trace_state(last_state_cluster, cluster_state, "Cluster_State", tid_cluster,
					std::string("{\"power\": ") + bool_to_json(power_on)
					+ ", \"reset_n\": " + bool_to_json(rst_n)
					+ ", \"interrupt\": " + bool_to_json(interrupt_o.read()) + "}");

		const bool ahb_active = ahb_active_reg.read();
		const bool ahb_write = ahb_write_reg.read();
		const std::string ahb_state = ahb_active ? (ahb_write ? "WRITE" : "READ") : "IDLE";
		trace_state(last_state_cmd, ahb_state, "Cluster_AHB", tid_cmd,
					std::string("{\"addr\": ") + std::to_string(ahb_addr_reg.read().to_uint())
					+ ", \"write\": " + bool_to_json(ahb_write) + "}");

		const bool dma_req = (s_axi_awvalid_i.read() && s_axi_awready_o.read())
			|| (s_axi_wvalid_i.read() && s_axi_wready_o.read())
			|| (s_axi_arvalid_i.read() && s_axi_arready_o.read());
		const bool dma_done = (s_axi_bvalid_o.read() && s_axi_bready_i.read())
			|| (s_axi_rvalid_o.read() && s_axi_rready_i.read());
		const std::string dma_state = dma_req ? (dma_done ? "XFER" : "REQ") : (dma_done ? "RESP" : "IDLE");
		trace_state(last_state_dma, dma_state, "Cluster_DMA", tid_dma,
					std::string("{\"req\": ") + bool_to_json(dma_req)
					+ ", \"done\": " + bool_to_json(dma_done) + "}");
	}

	sc_signal<bool> local_reset_n_sig;

	// SPM config/control
	sc_signal<sc_uint<8>> spm_cfg_map_sig;
	sc_signal<bool> spm_cfg_update_sig;
	sc_signal<bool> spm_arb_policy_sig;
	sc_signal<bool> spm_pmu_rst_sig;
	sc_signal<sc_uint<64>> spm_pmu_cycle_cnt_sig;
	sc_vector<sc_signal<sc_uint<64>>> spm_pmu_port_txn_cnt_sig{"spm_pmu_port_txn_cnt_sig", SPM_NUM_NOC_CHANNEL};
	sc_signal<sc_uint<64>> spm_pmu_arb_stall_cnt_sig;
	sc_signal<sc_uint<64>> spm_pmu_credit_stall_cnt_sig;

	// HDDU <-> SPM
	sc_vector<sc_signal<bool>> hddu_spm_req_valid_sig;
	sc_vector<sc_signal<bool>> hddu_spm_req_ready_sig;
	sc_vector<sc_signal<spm_req_payload_t>> hddu_spm_req_payload_sig;
	sc_vector<sc_signal<bool>> hddu_spm_resp_valid_sig;
	sc_vector<sc_signal<bool>> hddu_spm_resp_ready_sig;
	sc_vector<sc_signal<spm_resp_payload_t>> hddu_spm_resp_payload_sig;

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

	// SPM AXI-Lite bridge
	sc_signal<bool> spm_axi_awvalid_sig;
	sc_signal<bool> spm_axi_awready_sig;
	sc_signal<sc_uint<SPM_ADDR_WIDTH>> spm_axi_awaddr_sig;
	sc_signal<bool> spm_axi_wvalid_sig;
	sc_signal<bool> spm_axi_wready_sig;
	sc_signal<sc_biguint<SPM_SRAM_BANK_WIDTH_BITS>> spm_axi_wdata_sig;
	sc_signal<sc_uint<SPM_SRAM_BANK_WIDTH_BITS / 8>> spm_axi_wstrb_sig;
	sc_signal<bool> spm_axi_bvalid_sig;
	sc_signal<bool> spm_axi_bready_sig;
	sc_signal<sc_uint<2>> spm_axi_bresp_sig;
	sc_signal<bool> spm_axi_arvalid_sig;
	sc_signal<bool> spm_axi_arready_sig;
	sc_signal<sc_uint<SPM_ADDR_WIDTH>> spm_axi_araddr_sig;
	sc_signal<bool> spm_axi_rvalid_sig;
	sc_signal<bool> spm_axi_rready_sig;
	sc_signal<sc_biguint<SPM_SRAM_BANK_WIDTH_BITS>> spm_axi_rdata_sig;
	sc_signal<sc_uint<2>> spm_axi_rresp_sig;

	sc_signal<bool> ahb_active_reg;
	sc_signal<bool> ahb_write_reg;
	sc_signal<sc_uint<32>> ahb_addr_reg;
	sc_signal<sc_uint<3>> ahb_hsize_reg;
	sc_signal<sc_uint<32>> ahb_rdata_reg;

	sc_uint<32> noc_last_cmd_reg{};

	static bool in_range(uint32_t addr, uint32_t base, uint32_t size) {
		return addr >= base && addr < (base + size);
	}

	void comb_power_and_wiring() {
		const bool power_on = power_enable_i.read();
		local_reset_n_sig.write(reset_n.read() && power_on);

		// External AXI4 data slave gating
		spm_axi_awvalid_sig.write(power_on && s_axi_awvalid_i.read());
		spm_axi_awaddr_sig.write(s_axi_awaddr_i.read());
		s_axi_awready_o.write(power_on && spm_axi_awready_sig.read());

		spm_axi_wvalid_sig.write(power_on && s_axi_wvalid_i.read());
		spm_axi_wdata_sig.write(s_axi_wdata_i.read());
		spm_axi_wstrb_sig.write(s_axi_wstrb_i.read());
		s_axi_wready_o.write(power_on && spm_axi_wready_sig.read());

		spm_axi_bready_sig.write(power_on && s_axi_bready_i.read());
		s_axi_bvalid_o.write(power_on && spm_axi_bvalid_sig.read());
		s_axi_bresp_o.write(power_on ? spm_axi_bresp_sig.read() : sc_uint<2>(axi4lite::AXI_RESP_DECERR));

		spm_axi_arvalid_sig.write(power_on && s_axi_arvalid_i.read());
		spm_axi_araddr_sig.write(s_axi_araddr_i.read());
		s_axi_arready_o.write(power_on && spm_axi_arready_sig.read());

		spm_axi_rready_sig.write(power_on && s_axi_rready_i.read());
		s_axi_rvalid_o.write(power_on && spm_axi_rvalid_sig.read());
		s_axi_rdata_o.write(power_on ? spm_axi_rdata_sig.read() : sc_biguint<SPM_SRAM_BANK_WIDTH_BITS>(0));
		s_axi_rresp_o.write(power_on ? spm_axi_rresp_sig.read() : sc_uint<2>(axi4lite::AXI_RESP_DECERR));

		// AHB Output Logic
		hready_o.write(power_on);
		hresp_o.write(false); // OKAY

		// Top-level interrupt
		interrupt_o.write(power_on && hddu_interrupt_sig.read());

		// AHB read data is sequence-driven to avoid delta-cycle races.
		hrdata_o.write(power_on ? ahb_rdata_reg.read() : sc_uint<32>(0));

	}

	void seq_ahb_ctrl() {
		spm_cfg_map_sig.write(0);
		spm_cfg_update_sig.write(false);
		spm_arb_policy_sig.write(false);
		spm_pmu_rst_sig.write(false);

		noc_command_mode_sig.write(false);
		noc_command_data_sig.write(0);
		noc_last_cmd_reg = 0;

		ahb_active_reg.write(false);
		ahb_write_reg.write(false);
		ahb_addr_reg.write(0);
		ahb_rdata_reg.write(0);

		hddu_mmio_addr_sig.write(0);
		hddu_mmio_wdata_sig.write(0);
		hddu_mmio_write_sig.write(false);

		wait();

		while (true) {
			spm_cfg_update_sig.write(false);
			noc_command_mode_sig.write(false);
			hddu_mmio_write_sig.write(false);
			spm_pmu_rst_sig.write(false);

			if (!power_enable_i.read()) {
				ahb_active_reg.write(false);
				wait();
				continue;
			}

			// Pipeline Control: Capture Address Phase for Next Cycle Data Phase
			const bool sel = hsel_i.read();
			const bool ready = hready_i.read();
			const sc_uint<2> trans = htrans_i.read();
			// HTRANS: 2=NONSEQ, 3=SEQ implies valid transfer
			const bool is_trans = sel && ready && (trans[1] != 0);

			if (is_trans) {
				ahb_addr_reg.write(haddr_i.read());
				ahb_write_reg.write(hwrite_i.read());
				ahb_hsize_reg.write(hsize_i.read());
				ahb_active_reg.write(true);
			} else {
				ahb_active_reg.write(false);
			}

			// Data Phase Execution (using registers latched in previous cycle)
			if (ahb_active_reg.read()) {
				const uint32_t addr = ahb_addr_reg.read().to_uint();
				const bool is_write = ahb_write_reg.read();
				const uint32_t wdata = hwdata_i.read().to_uint(); // HWDATA valid now
				sc_uint<32> read_value = 0;

				if (is_write) {
					if (in_range(addr, kCmdSpmBase, kCmdSpmSize)) {
						const uint32_t off = addr - kCmdSpmBase;
						if (off == kSpmCfgMapOffset) {
							spm_cfg_map_sig.write(static_cast<sc_uint<8>>(wdata & 0xFF));
						} else if (off == kSpmCfgUpdateOffset) {
							if ((wdata & 0x1U) != 0U) {
								spm_cfg_update_sig.write(true);
							}
						} else if (off == kSpmArbPolicyOffset) {
							spm_arb_policy_sig.write((wdata & 0x1U) != 0U);
						} else if (off == kSpmPmuCtrlOffset) {
							if ((wdata & 0x1U) != 0U) {
								spm_pmu_rst_sig.write(true);
							}
						}
					} else if (in_range(addr, kCmdNocBase, kCmdNocSize)) {
						const uint32_t off = addr - kCmdNocBase;
						if (off == kNocCmdDataOffset) {
							noc_last_cmd_reg = wdata;
							noc_command_data_sig.write(static_cast<sc_uint<32>>(wdata));
							noc_command_mode_sig.write(true);
						}
					} else if (in_range(addr, kCmdHdduBase, kCmdHdduSize)) {
						hddu_mmio_addr_sig.write(static_cast<sc_uint<32>>(addr - kCmdHdduBase));
						hddu_mmio_wdata_sig.write(static_cast<sc_uint<32>>(wdata));
						hddu_mmio_write_sig.write(true);
					}
				} else {
					if (in_range(addr, kCmdSpmBase, kCmdSpmSize)) {
						const uint32_t off = addr - kCmdSpmBase;
						if (off == kSpmCfgMapOffset) {
							read_value = spm_cfg_map_sig.read();
						} else if (off == kSpmCfgUpdateOffset) {
							read_value = 0;
						} else if (off == kSpmArbPolicyOffset) {
							read_value = spm_arb_policy_sig.read() ? 1U : 0U;
						} else if (off == kSpmPmuCtrlOffset) {
							read_value = 0;
						} else if (off == kSpmPmuCycleCntLoOffset) {
							read_value = static_cast<uint32_t>(spm_pmu_cycle_cnt_sig.read().to_uint64() & 0xFFFFFFFFULL);
						} else if (off == kSpmPmuCycleCntHiOffset) {
							read_value = static_cast<uint32_t>((spm_pmu_cycle_cnt_sig.read().to_uint64() >> 32) & 0xFFFFFFFFULL);
						} else if (off == kSpmPmuArbStallLoOffset) {
							read_value = static_cast<uint32_t>(spm_pmu_arb_stall_cnt_sig.read().to_uint64() & 0xFFFFFFFFULL);
						} else if (off == kSpmPmuArbStallHiOffset) {
							read_value = static_cast<uint32_t>((spm_pmu_arb_stall_cnt_sig.read().to_uint64() >> 32) & 0xFFFFFFFFULL);
						} else if (off == kSpmPmuCreditStallLoOffset) {
							read_value = static_cast<uint32_t>(spm_pmu_credit_stall_cnt_sig.read().to_uint64() & 0xFFFFFFFFULL);
						} else if (off == kSpmPmuCreditStallHiOffset) {
							read_value = static_cast<uint32_t>((spm_pmu_credit_stall_cnt_sig.read().to_uint64() >> 32) & 0xFFFFFFFFULL);
						} else if (off >= kSpmPmuPortTxnBaseOffset) {
							const uint32_t rel = off - kSpmPmuPortTxnBaseOffset;
							const uint32_t idx = rel / kSpmPmuPortTxnStride;
							const bool high = (rel % kSpmPmuPortTxnStride) == 0x4;
							if (idx < SPM_NUM_NOC_CHANNEL && ((rel % kSpmPmuPortTxnStride) == 0x0 || high)) {
								const uint64_t v = spm_pmu_port_txn_cnt_sig[idx].read().to_uint64();
								read_value = static_cast<uint32_t>(high ? (v >> 32) : (v & 0xFFFFFFFFULL));
							}
						}
					} else if (in_range(addr, kCmdHdduBase, kCmdHdduSize)) {
						hddu_mmio_addr_sig.write(static_cast<sc_uint<32>>(addr - kCmdHdduBase));
						read_value = hddu_mmio_rdata_sig.read();
					} else if (in_range(addr, kCmdNocBase, kCmdNocSize)) {
						const uint32_t off = addr - kCmdNocBase;
						if (off == kNocCmdDataOffset) {
							read_value = noc_last_cmd_reg;
						}
					}
					ahb_rdata_reg.write(read_value);
				}
			}

			wait();
		}
	}
};

} // namespace hybridacc
