#define SC_INCLUDE_DYNAMIC_PROCESSES
#include <systemc>

#include "ComputeCluster.hpp"
#include "tb_utils.hpp"

#include <cstdint>
#include <cstdlib>
#include <array>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <iostream>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <limits>

namespace fs = std::filesystem;

namespace cluster_sim {

enum message_command_t : uint32_t {
	CMD_RESET = 0,
	CMD_INIT = 1,
	CMD_LOAD_PROGRAM = 2,
	CMD_STOP_PE = 3,
	CMD_START_PE = 4,
	CMD_NOC_SCAN_CHAIN = 8,
};

static constexpr uint32_t WAVE_TIMEOUT_CYCLES = 10000000;
static constexpr uint32_t POLL_INTERVAL_CYCLES = 1000;

static constexpr uint32_t CLUSTER_SPM_CFG_MAP = 0x0000;
static constexpr uint32_t CLUSTER_SPM_CFG_UPDATE = 0x0004;
static constexpr uint32_t CLUSTER_HDDU_BASE = 0x1000;
static constexpr uint32_t CLUSTER_NOC_CMD = 0x2000;

static constexpr uint32_t AGU_BANK_STRIDE = 0x100;
static constexpr uint32_t AGU_PS = 0;
static constexpr uint32_t AGU_PD = 1;
static constexpr uint32_t AGU_PLI = 2;
static constexpr uint32_t AGU_PLO = 3;

static constexpr uint32_t REG_BASE_ADDR = 0x00;
static constexpr uint32_t REG_BASE_ADDR_H = 0x04;
static constexpr uint32_t REG_ITER01 = 0x08;
static constexpr uint32_t REG_ITER23 = 0x0C;
static constexpr uint32_t REG_STRIDE0 = 0x10;
static constexpr uint32_t REG_STRIDE1 = 0x14;
static constexpr uint32_t REG_STRIDE2 = 0x18;
static constexpr uint32_t REG_STRIDE3 = 0x1C;
static constexpr uint32_t REG_CTRL = 0x20;
static constexpr uint32_t REG_LANE_CFG = 0x28;
static constexpr uint32_t REG_TAG_BASE = 0x40;
static constexpr uint32_t REG_TAG_STRIDE0 = 0x44;
static constexpr uint32_t REG_TAG_STRIDE1 = 0x48;
static constexpr uint32_t REG_TAG_CTRL = 0x4C;
static constexpr uint32_t REG_MASK_CFG = 0x54;

static constexpr uint32_t HDDU_CTRL = 0x800;
static constexpr uint32_t HDDU_STATUS = 0x804;
static constexpr uint32_t HDDU_PLANE_EN = 0x808;
static constexpr uint32_t HDDU_PLANE_MODE = 0x80C;
static constexpr uint32_t HDDU_COUNTER_TX_PKT = 0x828;
static constexpr uint32_t HDDU_COUNTER_TX_BYTE = 0x82c;
static constexpr uint32_t HDDU_COUNTER_RX_BYTE = 0x830;
static constexpr uint32_t HDDU_COUNTER_STALL = 0x834;


struct HdduCounters {
	uint32_t tx_pkt = 0;
	uint32_t tx_byte = 0;
	uint32_t rx_byte = 0;
	uint32_t stall = 0;
};

struct PerformanceSummary {
	uint64_t dram_read_bytes = 0;
	uint64_t dram_write_bytes = 0;
	uint64_t total_macs = 0;
	uint64_t total_flops = 0;
	double sim_seconds = 0.0;
	double dram_throughput_bps = 0.0;
	double dram_throughput_Bps = 0.0;
	double macs_per_sec = 0.0;
	double flops_per_sec = 0.0;
	double arithmetic_intensity = 0.0; // MACs / byte
};

struct ScaledMetric {
	double value = 0.0;
	const char* prefix = "";
};

inline ScaledMetric scale_metric(double v) {
	const double av = std::fabs(v);
	if (av >= 1e9) return {v / 1e9, "G"};
	if (av >= 1e6) return {v / 1e6, "M"};
	if (av >= 1e3) return {v / 1e3, "K"};
	return {v, ""};
}

inline std::string format_scaled(double v, const char* unit, int precision = 3) {
	const ScaledMetric s = scale_metric(v);
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(precision) << s.value << " " << s.prefix << unit;
	return oss.str();
}

using AguCfg = cluster_json::AguCfg;
using ClusterPlan = cluster_json::ClusterPlan;

/**
 * @brief Pack NOC command code into low nibble with command parameter payload.
 * @param cmd Command opcode.
 * @param param Command payload.
 * @return Packed 32-bit NOC command word.
 */
inline uint32_t pack_noc_cmd(message_command_t cmd, uint32_t param) {
	return (param & 0xFFFFFFF0u) | (static_cast<uint32_t>(cmd) & 0x0Fu);
}

/**
 * @brief Build packed scan-chain command payload.
 * @param ps_id Source PE id.
 * @param pd_id Destination PE id.
 * @param pli_id Input PE lane id.
 * @param plo_id Output PE lane id.
 * @param route_mode NOC routing mode.
 * @param enable Enable/disable flag.
 * @return Packed scan-chain command word.
 */
inline uint32_t pack_scan_chain(uint8_t ps_id, uint8_t pd_id, uint8_t pli_id, uint8_t plo_id,
							   uint8_t route_mode, bool enable) {
	uint32_t v = 0;
	v |= (static_cast<uint32_t>(ps_id) & 0x3Fu) << 4;
	v |= (static_cast<uint32_t>(pd_id) & 0x3Fu) << 10;
	v |= (static_cast<uint32_t>(pli_id) & 0x3Fu) << 16;
	v |= (static_cast<uint32_t>(plo_id) & 0x3Fu) << 22;
	v |= (static_cast<uint32_t>(route_mode) & 0x03u) << 28;
	v |= (enable ? 1u : 0u) << 30;
	return pack_noc_cmd(CMD_NOC_SCAN_CHAIN, v);
}

/**
 * @brief Build packed load-program command payload for PE instruction memory.
 * @param im_addr_bytes IM byte address.
 * @param inst16 16-bit instruction value.
 * @return Packed load-program command word.
 */
inline uint32_t pack_load_program(uint16_t im_addr_bytes, uint16_t inst16) {
	uint32_t p = 0;
	p |= (static_cast<uint32_t>(im_addr_bytes) & PE_ROUTER_IM_ADDR_MASK) << PE_ROUTER_IM_ADDR_OFFSET;
	p |= (static_cast<uint32_t>(inst16) & PE_ROUTER_IM_DATA_MASK) << PE_ROUTER_IM_DATA_OFFSET;
	return pack_noc_cmd(CMD_LOAD_PROGRAM, p);
}

enum class ScenarioKind {
	Conv2D,
	GEMM,
	Both,
};

struct SimRunnerOptions {
    ScenarioKind scenario = ScenarioKind::Both;
    bool dry_run = true;
	bool verbose = false;
	bool enable_trace = false;
	int clock_period_ns = 10;
	int timeout_cycles = 200;
    std::string test_path;
	std::string trace_file;
};

// Unified testbench that owns cluster control + DMA processes
class ComputeClusterTestBench : public sc_core::sc_module {
public:
	SC_HAS_PROCESS(ComputeClusterTestBench);

	using DmaTransferCfg = cluster_json::DmaTransferCfg;
	using DmaWaveCfg = cluster_json::DmaWaveCfg;

	struct DmaWaveSyncRequest {
		uint32_t wave_id = 0;
		DmaWaveCfg wave;
		DmaTransferCfg::Direction stage_direction = DmaTransferCfg::Direction::DramToSpm;
		uint32_t latency_cycles = 1;
	};

	struct TimingStats {
		sc_core::sc_time dma_wait_time = sc_core::SC_ZERO_TIME;
		sc_core::sc_time hddu_wait_time = sc_core::SC_ZERO_TIME;
		sc_core::sc_time dma_busy_time = sc_core::SC_ZERO_TIME;
		uint32_t dma_wave_count = 0;
	};

	static uint64_t make_wave_stage_key(uint32_t wave_id, DmaTransferCfg::Direction stage_direction) {
		return (static_cast<uint64_t>(wave_id) << 32) |
				   static_cast<uint32_t>(stage_direction);
	}

	int kClockPeriodNs = 10;
	int timeout_cycles = 200;

	sc_core::sc_in<bool> clk;
	sc_core::sc_out<bool> reset_n;
	sc_core::sc_out<bool> power_enable_i;
	sc_core::sc_in<bool> interrupt_o;

	sc_core::sc_out<bool> s_axi_awvalid_i;
	sc_core::sc_in<bool> s_axi_awready_o;
	sc_core::sc_out<sc_dt::sc_uint<32>> s_axi_awaddr_i;
	sc_core::sc_out<bool> s_axi_wvalid_i;
	sc_core::sc_in<bool> s_axi_wready_o;
	sc_core::sc_out<sc_dt::sc_biguint<64>> s_axi_wdata_i;
	sc_core::sc_out<sc_dt::sc_uint<8>> s_axi_wstrb_i;
	sc_core::sc_in<bool> s_axi_bvalid_o;
	sc_core::sc_out<bool> s_axi_bready_i;
	sc_core::sc_in<sc_dt::sc_uint<2>> s_axi_bresp_o;
	sc_core::sc_out<bool> s_axi_arvalid_i;
	sc_core::sc_in<bool> s_axi_arready_o;
	sc_core::sc_out<sc_dt::sc_uint<32>> s_axi_araddr_i;
	sc_core::sc_in<bool> s_axi_rvalid_o;
	sc_core::sc_out<bool> s_axi_rready_i;
	sc_core::sc_in<sc_dt::sc_biguint<64>> s_axi_rdata_o;
	sc_core::sc_in<sc_dt::sc_uint<2>> s_axi_rresp_o;

	sc_core::sc_out<bool> hsel_i;
	sc_core::sc_out<sc_dt::sc_uint<32>> haddr_i;
	sc_core::sc_out<bool> hwrite_i;
	sc_core::sc_out<sc_dt::sc_uint<2>> htrans_i;
	sc_core::sc_out<sc_dt::sc_uint<3>> hsize_i;
	sc_core::sc_out<sc_dt::sc_uint<3>> hburst_i;
	sc_core::sc_out<sc_dt::sc_uint<4>> hprot_i;
	sc_core::sc_out<bool> hready_i;
	sc_core::sc_out<sc_dt::sc_uint<32>> hwdata_i;
	sc_core::sc_in<bool> hready_o;
	sc_core::sc_in<bool> hresp_o;
	sc_core::sc_in<sc_dt::sc_uint<32>> hrdata_o;

	// consts
	static constexpr unsigned SPM_NUM_NOC_CHANNEL = 4;
	static constexpr unsigned SPM_NUM_BANKS_PER_GROUP = 3;
	static constexpr unsigned SPM_SRAM_BANK_WIDTH_BITS = 64;
	static constexpr unsigned SPM_SRAM_BANK_DEPTH_WORDS = 8192;
	static constexpr unsigned SPM_SRAM_BANK_LATENCY = 1;
	static constexpr unsigned SPM_SRAM_BANK_PIPELINE_DEPTH = 3;
	static constexpr unsigned SPM_ADDR_WIDTH = 32;
	static constexpr unsigned NOC_NUM_PORTS = 3;
	static constexpr unsigned NOC_PORT_WIDTH_BITS = 64;
	static constexpr unsigned NOC_NUM_PES_PER_PORT = 16;

	using DutType = hybridacc::ComputeCluster<
		SPM_NUM_NOC_CHANNEL,
		SPM_NUM_BANKS_PER_GROUP,
		SPM_SRAM_BANK_WIDTH_BITS,
		SPM_SRAM_BANK_DEPTH_WORDS,
		SPM_SRAM_BANK_LATENCY,
		SPM_SRAM_BANK_PIPELINE_DEPTH,
		SPM_ADDR_WIDTH,
		NOC_NUM_PORTS,
		NOC_PORT_WIDTH_BITS,
		NOC_NUM_PES_PER_PORT>;

	ComputeClusterTestBench(
		sc_core::sc_module_name name,
		const SimRunnerOptions& options)
		: sc_core::sc_module(name),
		  options_(options),
		  kClockPeriodNs(options.clock_period_ns),
		  timeout_cycles(options.timeout_cycles),
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
		  hrdata_o("hrdata_o") {
		dma_wave_done_.write(false);

		SC_THREAD(dma_process);
		SC_THREAD(cluster_control_process);
	}

	void end_of_elaboration() override {
		reset_n.write(false);
		power_enable_i.write(false);
		s_axi_awvalid_i.write(false);
		s_axi_awaddr_i.write(0);
		s_axi_wvalid_i.write(false);
		s_axi_wdata_i.write(0);
		s_axi_wstrb_i.write(0xFF);
		s_axi_bready_i.write(false);
		s_axi_arvalid_i.write(false);
		s_axi_araddr_i.write(0);
		s_axi_rready_i.write(false);
		hsel_i.write(false);
		haddr_i.write(0);
		hwrite_i.write(false);
		htrans_i.write(0);
		hsize_i.write(2);
		hburst_i.write(0);
		hprot_i.write(0);
		hready_i.write(true);
		hwdata_i.write(0);
	}

	/** @brief Mark testbench as started so control process can begin execution. */
	void init() {
		started_ = true;
	}

	/**
	 * @brief Print verification stats and return pass/fail state.
	 * @return True when testcase execution and comparison pass threshold.
	 */
	bool verify() {
		if (!run_finished_) {
			std::cerr << "[test_cluster_sim] simulation ended before testbench finished" << std::endl;
			return false;
		}
		if (!verify_done_) {
			verify_done_ = true;
		}
		std::cout << "[runner] Verify results: " << std::endl << verify_stats_ << std::endl;
		return pass_;
	}

	/** @brief Query final pass status after simulation completes. */
	bool passed() const {
		return pass_;
	}

	/**
	 * @brief Power on DUT and apply active-low reset sequence.
	 * @param reset_low_cycles Reset asserted cycles.
	 * @param settle_cycles Additional settle cycles after reset release.
	 */
	void power_on_and_reset(uint32_t reset_low_cycles = 4, uint32_t settle_cycles = 4) {
		set_power_enable(true);
		set_reset_n(false);
		wait_cycles(reset_low_cycles);
		set_reset_n(true);
		wait_cycles(settle_cycles);
	}

	/**
	 * @brief Program SPM map and trigger update pulse.
	 * @param map_val Packed SPM map selector value.
	 */
	void config_spm_map(uint8_t map_val) {
		mmio_write(CLUSTER_SPM_CFG_MAP, static_cast<uint32_t>(map_val));
		mmio_write(CLUSTER_SPM_CFG_UPDATE, 0x1u);
	}

	/**
	 * @brief Burst-write 64-bit words into memory-mapped data path.
	 * @param addrs Address list.
	 * @param datas Word payload list.
	 */
	void dma_write_words64(const std::vector<uint32_t>& addrs, const std::vector<uint64_t>& datas) {
		if (addrs.empty() || datas.empty()) {
			return;
		}
		if (addrs.size() != datas.size()) {
			throw std::invalid_argument("dma_write_words64: addrs size mismatch datas size");
		}
		data_write64_burst(addrs, datas);
	}

	/**
	 * @brief Burst-read 64-bit words from memory-mapped data path.
	 * @param addrs Address list.
	 * @param datas Output word payload list.
	 */
	void dma_read_words64(const std::vector<uint32_t>& addrs, std::vector<uint64_t>& datas) {
		if (addrs.empty()) {
			datas.clear();
			return;
		}
		data_read64_burst(addrs, datas);
	}

	/**
	 * @brief Configure one AGU bank using compiler-generated settings.
	 * @param bank AGU bank index.
	 * @param c AGU configuration object.
	 */
	void cfg_agu(uint32_t bank, const AguCfg& c) {
		const uint32_t b = CLUSTER_HDDU_BASE + bank * AGU_BANK_STRIDE;
		if (!c.enable) {
			std::cout << "AGU bank " << bank << " disabled, skipping config" << std::endl;
			return;
		}
		mmio_write(b + REG_BASE_ADDR, c.base_addr);
		mmio_write(b + REG_BASE_ADDR_H, c.base_addr_h);
		mmio_write(b + REG_ITER01, (static_cast<uint32_t>(c.iter1) << 16) | c.iter0);
		mmio_write(b + REG_ITER23, (static_cast<uint32_t>(c.iter3) << 16) | c.iter2);
		mmio_write(b + REG_STRIDE0, static_cast<uint32_t>(c.stride0));
		mmio_write(b + REG_STRIDE1, static_cast<uint32_t>(c.stride1));
		mmio_write(b + REG_STRIDE2, static_cast<uint32_t>(c.stride2));
		mmio_write(b + REG_STRIDE3, static_cast<uint32_t>(c.stride3));
		mmio_write(b + REG_LANE_CFG, c.lane_cfg);
		mmio_write(b + REG_TAG_BASE, c.tag_base);
		mmio_write(b + REG_TAG_STRIDE0, c.tag_stride0);
		mmio_write(b + REG_TAG_STRIDE1, c.tag_stride1);
		mmio_write(b + REG_TAG_CTRL, c.tag_ctrl);
		mmio_write(b + REG_MASK_CFG, c.mask_cfg);
		mmio_write(b + REG_CTRL, c.ultra ? (1u << (int)hybridacc::cluster::AguCtrlBit::CTRL_ULTRA) : 0u);
	}

	/**
	 * @brief Configure global HDDU plane controls and issue START.
	 * @param plane_en Plane enable mask.
	 * @param plane_mode Plane mode value.
	 */
	void cfg_hddu_global(uint32_t plane_en, uint32_t plane_mode) {
		mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN, plane_en);
		mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE, plane_mode);
		mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_START));
	}

	/**
	 * @brief Send one packed NOC command via MMIO.
	 * @param packed_cmd Packed command word.
	 */
	void noc_cmd_write(uint32_t packed_cmd) {
		mmio_write(CLUSTER_NOC_CMD, packed_cmd);
	}

	/**
	 * @brief Send scan-chain words in reverse order.
	 * @param words Scan-chain payload words.
	 */
	void send_scan_chain_words_reverse(const std::vector<uint32_t>& words) {
		for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
			noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, words[static_cast<size_t>(i)]));
		}
	}

	/**
	 * @brief Load PE instruction program into IM over NOC command path.
	 * @param inst16 Instruction vector.
	 */
	void load_pe_program(const std::vector<uint16_t>& inst16) {
		for (uint32_t pc = 0; pc < inst16.size(); ++pc) {
			noc_cmd_write(pack_load_program(static_cast<uint16_t>(pc * 2), inst16[pc]));
		}
	}

	/** @brief Start PE execution and HDDU engine. */
	void start_all() {
		noc_cmd_write(pack_noc_cmd(CMD_START_PE, 0));
		mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_START));
	}

	/** @brief Stop HDDU engine and PE execution. */
	void stop_all() {
		mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_STOP));
		noc_cmd_write(pack_noc_cmd(CMD_STOP_PE, 0));
	}

	/** @brief Dump HDDU error code and diagnostic registers. */
	void print_hddu_error_info() {
		uint32_t err_code = mmio_read(CLUSTER_HDDU_BASE + HDDU_CTRL) & 0xFFu;
		uint32_t err_info0 = mmio_read(CLUSTER_HDDU_BASE + 0x80);
		uint32_t err_info1 = mmio_read(CLUSTER_HDDU_BASE + 0x84);
		std::cerr << "HDDU error detected! Error code: " << err_code
				  << ", info0: 0x" << std::hex << err_info0
				  << ", info1: 0x" << std::hex << err_info1 << std::dec << std::endl;
	}

	/**
	 * @brief Wait HDDU DONE with interrupt-first then polling fallback.
	 * @param timeout_cycles Timeout in cycles.
	 * @param poll_step Polling interval cycles.
	 * @return True if DONE is observed without ERROR.
	 */
	bool wait_hddu_done(uint32_t timeout_cycles, uint32_t poll_step = 1) {
		const sc_core::sc_time wait_t0 = sc_core::sc_time_stamp();
		std::cout << sc_core::sc_time_stamp() << " Waiting for HDDU done with timeout " << timeout_cycles << " cycles..." << std::endl;
		bool interrupt_received = wait_interrupt_asserted(timeout_cycles);
		std::cout << sc_core::sc_time_stamp() << " Interrupt wait " << (interrupt_received ? "succeeded" : "timed out") << std::endl;

		mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS); // dummy read to ensure we see the latest status after interrupt

		if (interrupt_received) {
			const uint32_t st = mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::ERROR)) {
				timing_stats_.hddu_wait_time += (sc_core::sc_time_stamp() - wait_t0);
				std::cerr << "HDDU error detected after interrupt, status=0x" << std::hex << st << std::dec << std::endl;
				print_hddu_error_info();
				return false;
			}
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::DONE)) {
				timing_stats_.hddu_wait_time += (sc_core::sc_time_stamp() - wait_t0);
				std::cout << sc_core::sc_time_stamp() << " HDDU done detected by interrupt and confirmed via MMIO status" << std::endl;
				return true;
			}

			std::cout << sc_core::sc_time_stamp() << " HDDU interrupt, status=0x" << std::hex << st << std::dec << ", but DONE bit not set, falling back to polling..." << std::endl;
		}

		for (uint32_t waited = 0; waited < timeout_cycles; waited += poll_step) {
			const uint32_t st = mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::ERROR)) {
				timing_stats_.hddu_wait_time += (sc_core::sc_time_stamp() - wait_t0);
				std::cerr << sc_core::sc_time_stamp() << " HDDU error detected, status=0x" << std::hex << st << std::dec << std::endl;
				print_hddu_error_info();
				return false;
			}
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::DONE)) {
				timing_stats_.hddu_wait_time += (sc_core::sc_time_stamp() - wait_t0);
				std::cout << sc_core::sc_time_stamp() << " HDDU done detected after waiting " << waited << " cycles" << std::endl;
				return true;
			}
			wait_cycles(poll_step);
		}
		timing_stats_.hddu_wait_time += (sc_core::sc_time_stamp() - wait_t0);
		return false;
	}

	/**
	 * @brief Write one 32-bit MMIO value through AHB.
	 * @param addr MMIO byte address.
	 * @param data 32-bit payload.
	 */
	void mmio_write(uint32_t addr, uint32_t data) {
		if (addr == CLUSTER_NOC_CMD) {
			noc_cmd_log_.push_back(data);
		}
		if (!ahb_write_tx(addr, data)) {
			throw std::runtime_error("ComputeCluster MMIO write timeout");
		}
	}

	/**
	 * @brief Read one 32-bit MMIO value through AHB.
	 * @param addr MMIO byte address.
	 * @return 32-bit read data.
	 */
	uint32_t mmio_read(uint32_t addr) {
		uint32_t out = 0;
		if (!ahb_read_tx(addr, out)) {
			throw std::runtime_error("ComputeCluster MMIO read timeout");
		}
		if (in_range(addr, CLUSTER_HDDU_BASE, 0x1000)) {
			uint32_t stable = 0;
			if (!ahb_read_tx(addr, stable)) {
				throw std::runtime_error("ComputeCluster HDDU MMIO read timeout");
			}
			out = stable;
		}
		return out;
	}

	/**
	 * @brief Write one 64-bit data word via AXI data path.
	 * @param addr Byte address.
	 * @param data 64-bit word value.
	 */
	void data_write64(uint32_t addr, uint64_t data) {
		if (!data_write_tx(addr, data)) {
			throw std::runtime_error("ComputeCluster data write timeout");
		}
	}

	/**
	 * @brief Burst-write multiple 64-bit words via AXI data path.
	 * @param addrs Address list.
	 * @param words Data list.
	 */
	void data_write64_burst(const std::vector<uint32_t>& addrs, const std::vector<uint64_t>& words) {
		if (!data_write_burst_tx(addrs, words)) {
			throw std::runtime_error("ComputeCluster burst data write timeout");
		}
	}

	/**
	 * @brief Read one 64-bit data word via AXI data path.
	 * @param addr Byte address.
	 * @return 64-bit word value.
	 */
	uint64_t data_read64(uint32_t addr) const {
		uint64_t out = 0;
		if (!const_cast<ComputeClusterTestBench*>(this)->data_read_tx(addr, out)) {
			throw std::runtime_error("ComputeCluster data read timeout");
		}
		return out;
	}

	/**
	 * @brief Burst-read multiple 64-bit words via AXI data path.
	 * @param addrs Address list.
	 * @param out Output data list.
	 */
	void data_read64_burst(const std::vector<uint32_t>& addrs, std::vector<uint64_t>& out) {
		out.assign(addrs.size(), 0);
		if (!data_read_burst_tx(addrs, out)) {
			throw std::runtime_error("ComputeCluster burst data read timeout");
		}
	}

	/** @brief Drive DUT power-enable input signal. */
	void set_power_enable(bool on) {
		power_enable_i.write(on);
	}

	/** @brief Drive DUT reset_n input signal. */
	void set_reset_n(bool rst_n) {
		reset_n.write(rst_n);
	}

	/**
	 * @brief Wait clock cycles and accumulate cycle counter.
	 * @param n Number of cycles.
	 */
	void wait_cycles(uint32_t n) {
		tick(n);
	}

	/** @brief Get elapsed simulation cycles derived from global simulation timestamp. */
	uint64_t cycle() const {
		return static_cast<uint64_t>(
			sc_core::sc_time_stamp() / sc_core::sc_time(kClockPeriodNs, sc_core::SC_NS));
	}

	/** @brief Access recorded NOC command stream for summary/debug. */
	const std::vector<uint32_t>& noc_cmd_log() const {
		return noc_cmd_log_;
	}

	/** @brief Access performance summary generated from the current run. */
	const PerformanceSummary& perf_summary() const {
		return perf_summary_;
	}

	/**
	 * @brief Queue one synthetic DMA wave request for DMA process.
	 * @param wave_id Wave identifier.
	 * @param latency_cycles Synthetic latency cycles.
	 */
	void request_dma_wave_sync(uint32_t wave_id, uint32_t latency_cycles = 1) {
		dma_requests_.push_back(DmaWaveSyncRequest{
			wave_id,
			{},
			DmaTransferCfg::Direction::DramToSpm,
			std::max<uint32_t>(1, latency_cycles)
		});
		dma_req_event_.notify(sc_core::SC_ZERO_TIME);
	}

	/**
	 * @brief Queue one DMA wave work item for dma_process to execute.
	 * @param wave Wave configuration.
	 * @param stage_direction Stage direction.
	 * @param latency_cycles Synthetic latency cycles.
	 */
	void request_dma_wave_work(
		const DmaWaveCfg& wave,
		DmaTransferCfg::Direction stage_direction,
		uint32_t latency_cycles = 1) {
		dma_requests_.push_back(DmaWaveSyncRequest{
			wave.wave_id,
			wave,
			stage_direction,
			std::max<uint32_t>(1, latency_cycles)
		});
		dma_req_event_.notify(sc_core::SC_ZERO_TIME);
	}

	/**
	 * @brief Wait for DMA wave completion event with timeout.
	 * @param wave_id Wave identifier.
	 * @param timeout Timeout in cycles.
	 * @return True if wave completion is observed.
	 */
	bool wait_dma_wave_done(
		uint32_t wave_id,
		DmaTransferCfg::Direction stage_direction,
		uint32_t timeout) {
		const uint64_t wave_stage_key = make_wave_stage_key(wave_id, stage_direction);
		if (dma_completed_waves_.count(wave_stage_key) > 0) {
			return true;
		}
		const sc_core::sc_time wait_t0 = sc_core::sc_time_stamp();
		const sc_core::sc_time deadline =
			wait_t0 + sc_core::sc_time(static_cast<double>(timeout) * kClockPeriodNs, sc_core::SC_NS);
		std::cout << sc_core::sc_time_stamp()
				  << " [dma] wait completion wave_id=" << wave_id
				  << " stage="
				  << (stage_direction == DmaTransferCfg::Direction::DramToSpm ? "dram_to_spm" : "spm_to_dram")
				  << " timeout_cycles=" << timeout << std::endl;

		while (dma_completed_waves_.count(wave_stage_key) == 0) {
			const sc_core::sc_time now = sc_core::sc_time_stamp();
			if (now >= deadline) {
				break;
			}
			wait(deadline - now, dma_done_event_);
		}

		timing_stats_.dma_wait_time += (sc_core::sc_time_stamp() - wait_t0);
		const bool completed = dma_completed_waves_.count(wave_stage_key) > 0;
		std::cout << sc_core::sc_time_stamp()
				  << " [dma] wait done wave_id=" << wave_id
				  << " stage="
				  << (stage_direction == DmaTransferCfg::Direction::DramToSpm ? "dram_to_spm" : "spm_to_dram")
				  << " completed=" << (completed ? "yes" : "no")
				  << std::endl;
		return completed;
	}

	/**
	 * @brief Wait interrupt assertion with cycle timeout.
	 * @param timeout_cycles Timeout in cycles.
	 * @return True if interrupt asserted before timeout.
	 */
	bool wait_interrupt_asserted(uint32_t timeout_cycles) {
		if (interrupt_o.read()) {
			return true;
		}
		for (uint32_t i = 0; i < timeout_cycles; ++i) {
			wait_cycles(1);
			if (interrupt_o.read()) {
				return true;
			}
		}
		return false;
	}

private:
	/**
	 * @brief Check whether address lies in [base, base + size).
	 */
	static bool in_range(uint32_t addr, uint32_t base, uint32_t size) {
		return addr >= base && addr < (base + size);
	}

	/**
	 * @brief Wait @p n positive edges of local testbench clock.
	 */
	void tick(uint32_t n = 1) {
		for (uint32_t i = 0; i < n; ++i) {
			wait(clk.negedge_event());
		}
	}

	/**
	 * @brief Execute one AHB write transaction.
	 * @return True when transaction handshake completes.
	 */
	bool ahb_write_tx(uint32_t addr, uint32_t data) {
		if (!hready_o.read()) {
			return false;
		}

		hsel_i.write(true);
		haddr_i.write(addr);
		hwrite_i.write(true);
		htrans_i.write(2); // NONSEQ
		hsize_i.write(2);  // 32-bit
		hburst_i.write(0); // SINGLE
		hprot_i.write(0);
		hready_i.write(true);
		wait_cycles(1);

		hsel_i.write(false);
		htrans_i.write(0); // IDLE
		hwdata_i.write(data);

		for (int i = 0; i < timeout_cycles; ++i) {
			wait_cycles(1);
			if (hready_o.read()) {
				hwdata_i.write(0);
				return true;
			}
		}

		hwdata_i.write(0);
		return false;
	}

	/**
	 * @brief Execute one AHB read transaction.
	 * @return True when transaction handshake completes.
	 */
	bool ahb_read_tx(uint32_t addr, uint32_t& out) {
		if (!hready_o.read()) {
			return false;
		}

		hsel_i.write(true);
		haddr_i.write(addr);
		hwrite_i.write(false);
		htrans_i.write(2); // NONSEQ
		hsize_i.write(2);  // 32-bit
		hburst_i.write(0); // SINGLE
		hprot_i.write(0);
		hready_i.write(true);
		wait_cycles(1);

		hsel_i.write(false);
		htrans_i.write(0); // IDLE

		for (int i = 0; i < timeout_cycles; ++i) {
			wait_cycles(1);
			if (hready_o.read()) {
				out = hrdata_o.read().to_uint();
				return true;
			}
		}

		return false;
	}

	/**
	 * @brief Execute one AXI write transaction for a single 64-bit word.
	 * @return True when AW/W/B handshake completes.
	 */
	bool data_write_tx(uint32_t addr, uint64_t data) {
		s_axi_awaddr_i.write(addr);
		s_axi_awvalid_i.write(true);
		s_axi_wdata_i.write(sc_dt::sc_biguint<64>(data));
		s_axi_wstrb_i.write(0xFF);
		s_axi_wvalid_i.write(true);
		s_axi_bready_i.write(true);

		bool aw_done = false;
		bool w_done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			if (!aw_done && s_axi_awready_o.read()) {
				aw_done = true;
			}
			if (!w_done && s_axi_wready_o.read()) {
				w_done = true;
			}
			if (aw_done && w_done && s_axi_bvalid_o.read()) {
				wait_cycles(1);
				s_axi_bready_i.write(false);
				return true;
			}
			wait_cycles(1);
			s_axi_awvalid_i.write(aw_done ? false : true);
			s_axi_wvalid_i.write(w_done ? false : true);

		}

		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_bready_i.write(false);
		wait_cycles(1);
		return false;
	}

	/**
	 * @brief Execute one AXI read transaction for a single 64-bit word.
	 * @return True when AR/R handshake completes.
	 */
	bool data_read_tx(uint32_t addr, uint64_t& out) {
		s_axi_araddr_i.write(addr);
		s_axi_arvalid_i.write(true);
		s_axi_rready_i.write(true);

		bool ar_done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			if (!ar_done && s_axi_arready_o.read()) {
				ar_done = true;
			}
			if (ar_done && s_axi_rvalid_o.read()) {
				out = s_axi_rdata_o.read().to_uint64();
				s_axi_rready_i.write(false);
				wait_cycles(1);
				return true;
			}
			wait_cycles(1);
			s_axi_arvalid_i.write(ar_done ? false : true);
		}

		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(false);
		wait_cycles(1);
		return false;
	}

	/**
	 * @brief Execute address-list based AXI burst write sequence.
	 * @return True when all requests and responses complete.
	 */
	bool data_write_burst_tx(const std::vector<uint32_t>& addrs, const std::vector<uint64_t>& words) {
		if (addrs.empty() || words.empty()) {
			return true;
		}
		if (addrs.size() != words.size()) {
			return false;
		}

		size_t aw_sent = 0;
		size_t w_sent = 0;
		size_t bresp_done = 0;
		bool aw_active = false;
		bool w_active = false;
		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_wstrb_i.write(0xFF);
		s_axi_bready_i.write(true);

		const uint64_t max_cycles =
			static_cast<uint64_t>(std::max(1, timeout_cycles)) * static_cast<uint64_t>(addrs.size() + 2);

		for (uint64_t i = 0; i < max_cycles; ++i) {
			if (!aw_active && aw_sent < addrs.size()) {
				s_axi_awaddr_i.write(addrs[aw_sent]);
				s_axi_awvalid_i.write(true);
				aw_active = true;

			}
			if (!w_active && w_sent < words.size()) {
				s_axi_wdata_i.write(sc_dt::sc_biguint<64>(words[w_sent]));
				s_axi_wvalid_i.write(true);
				w_active = true;
			}

			// wait for next cycle to sample ready/valid signals
			// wait(clk.negedge_event());

			bool aw_ready = s_axi_awready_o.read();
			bool w_ready = s_axi_wready_o.read();
			bool b_valid = s_axi_bvalid_o.read();

			wait_cycles(1);

			if (aw_active && aw_ready) {
				s_axi_awvalid_i.write(false);
				aw_active = false;
				++aw_sent;
			}
			if (w_active && w_ready) {
				s_axi_wvalid_i.write(false);
				w_active = false;
				++w_sent;
			}

			if (b_valid && s_axi_bready_i.read()) {
				++bresp_done;
			}

			if (aw_sent == addrs.size() && w_sent == words.size() && !aw_active && !w_active && bresp_done == words.size()) {
				s_axi_awvalid_i.write(false);
				s_axi_wvalid_i.write(false);
				s_axi_bready_i.write(false);
				wait_cycles(1);
				return true;
			}
		}

		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_bready_i.write(false);
		wait_cycles(1);
		return false;
	}

	/**
	 * @brief Execute address-list based AXI burst read sequence.
	 * @return True when all requests and responses complete.
	 */
	bool data_read_burst_tx(const std::vector<uint32_t>& addrs, std::vector<uint64_t>& out_words) {
		if (addrs.empty()) {
			out_words.clear();
			return true;
		}
		out_words.assign(addrs.size(), 0);

		size_t ar_sent = 0;
		size_t r_done = 0;
		bool ar_active = false;
		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(true);


		const uint64_t max_cycles =
			static_cast<uint64_t>(std::max(1, timeout_cycles)) * static_cast<uint64_t>(addrs.size() + 2);

		for (uint64_t i = 0; i < max_cycles; ++i) {
			if (!ar_active && ar_sent < addrs.size()) {
				s_axi_araddr_i.write(addrs[ar_sent]);
				s_axi_arvalid_i.write(true);
				ar_active = true;
			}

			// wait for next cycle to sample ready/valid signals
			// wait(clk.negedge_event());

			bool ar_ready = s_axi_arready_o.read();
			bool r_valid = s_axi_rvalid_o.read();
			uint64_t r_data = s_axi_rdata_o.read().to_uint64();

			wait_cycles(1);

			if (ar_active && ar_ready) {
				s_axi_arvalid_i.write(false);
				ar_active = false;
				++ar_sent;
			}

			if (r_done < out_words.size() && r_valid && s_axi_rready_i.read()) {
				out_words[r_done] = r_data;
				++r_done;
			}

			if (ar_sent == addrs.size() && !ar_active && r_done == out_words.size()) {
				s_axi_arvalid_i.write(false);
				s_axi_rready_i.write(false);
				wait_cycles(1);
				return true;
			}
		}

		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(false);
		wait_cycles(1);
		return false;
	}

	/**
	 * @brief DMA worker process that consumes queued wave requests.
	 */
	void dma_process() {
		while (true) {
			if (dma_requests_.empty()) {
				wait(dma_req_event_);
			}
			while (!dma_requests_.empty()) {
				auto req = dma_requests_.front();
				dma_requests_.pop_front();
				const sc_core::sc_time dma_t0 = sc_core::sc_time_stamp();
				std::cout << sc_core::sc_time_stamp()
						  << " [dma] start wave_id=" << req.wave_id
						  << " stage="
						  << (req.stage_direction == DmaTransferCfg::Direction::DramToSpm ? "dram_to_spm" : "spm_to_dram")
						  << " transfers=" << req.wave.transfers.size()
						  << " latency_cycles=" << req.latency_cycles
						  << std::endl;

				dma_wave_done_.write(false);
				dma_active_wave_id_ = req.wave_id;
				bool work_ok = true;
				if (!req.wave.transfers.empty()) {
					work_ok = execute_dma_wave_transfers(req.wave, req.stage_direction);
				}
				for (uint32_t c = 0; c < req.latency_cycles; ++c) {
					wait_cycles(1);
				}
				const uint64_t wave_stage_key = make_wave_stage_key(req.wave_id, req.stage_direction);
				dma_wave_result_[wave_stage_key] = work_ok;
				dma_completed_waves_.insert(wave_stage_key);
				dma_done_event_.notify(sc_core::SC_ZERO_TIME);
				timing_stats_.dma_busy_time += (sc_core::sc_time_stamp() - dma_t0);
				++timing_stats_.dma_wave_count;
				std::cout << sc_core::sc_time_stamp()
						  << " [dma] complete wave_id=" << req.wave_id
						  << " ok=" << (work_ok ? "1" : "0")
						  << std::endl;
				dma_wave_done_.write(true);
				wait_cycles(1);
				dma_wave_done_.write(false);
			}
		}
	}

	/**
	 * @brief Perform actual memory movement for one DMA wave stage.
	 * @return True if all DMA transfers in stage succeed.
	 */
	bool execute_dma_wave_transfers(const DmaWaveCfg& wave, DmaTransferCfg::Direction stage_direction) {
		auto make_linear_addr_gen4d = [](uint32_t base_addr, uint32_t word_count) {
			cluster_json::DmaAddrGen4D gen;
			gen.enabled = true;
			gen.base_addr = base_addr;
			gen.iter = {1, 1, word_count, 1};
			gen.stride = {
				static_cast<int32_t>(word_count * 8),
				static_cast<int32_t>(word_count * 8),
				8,
				8,
			};
			return gen;
		};

		for (const auto& t : wave.transfers) {
			if (t.direction != stage_direction) {
				continue;
			}

			const uint64_t transfer_bytes = static_cast<uint64_t>(t.size_words64) * 8ull;

			auto src_gen = t.src_addr_gen;
			auto dst_gen = t.dst_addr_gen;
			if (!src_gen.enabled) {
				src_gen = make_linear_addr_gen4d(
					t.direction == DmaTransferCfg::Direction::DramToSpm ? t.src_dram_addr : t.src_spm_addr,
					t.size_words64);
			}
			if (!dst_gen.enabled) {
				dst_gen = make_linear_addr_gen4d(
					t.direction == DmaTransferCfg::Direction::DramToSpm ? t.dst_spm_addr : t.dst_dram_addr,
					t.size_words64);
			}

			auto src_addrs = cluster_json::build_dma_addr_list(
				src_gen,
				t.direction == DmaTransferCfg::Direction::DramToSpm ? t.src_dram_addr : t.src_spm_addr,
				t.size_words64);
			auto dst_addrs = cluster_json::build_dma_addr_list(
				dst_gen,
				t.direction == DmaTransferCfg::Direction::DramToSpm ? t.dst_spm_addr : t.dst_dram_addr,
				t.size_words64);

			if (t.direction == DmaTransferCfg::Direction::DramToSpm) {
				std::vector<uint64_t> payload;
				payload.reserve(t.size_words64);
				for (uint32_t i = 0; i < t.size_words64; ++i) {
					auto it = dram_shadow_.find(src_addrs[i]);
					payload.push_back(it == dram_shadow_.end() ? 0ULL : it->second);
				}
				dma_write_words64(dst_addrs, payload);
				perf_summary_.dram_read_bytes += transfer_bytes;
			} else {
				std::vector<uint64_t> payload;
				dma_read_words64(src_addrs, payload);
				for (uint32_t i = 0; i < t.size_words64; ++i) {
					dram_shadow_[dst_addrs[i]] = payload[i];
				}
				perf_summary_.dram_write_bytes += transfer_bytes;
			}
		}

		return true;
	}

	/**
	 * @brief Main cluster control process for testcase execution.
	 */
	void cluster_control_process() {
		wait(sc_core::SC_ZERO_TIME);
		while (!started_) {
			wait_cycles(1);
		}

		if (options_.test_path.empty()) {
			std::cerr << "No test path specified" << std::endl;
			pass_ = false;
			run_finished_ = true;
			sc_core::sc_stop();
			return;
		}

		pass_ = run_from_config(options_.test_path);
		run_finished_ = true;
		sc_core::sc_stop();
	}

	/**
	 * @brief Store prepared DRAM data into shadow map.
	 */
	void preload_dram_shadow(uint32_t base_addr, const std::vector<uint64_t>& words) {
		constexpr uint32_t kWordBytes = 8;
		for (uint32_t i = 0; i < words.size(); ++i) {
			dram_shadow_[base_addr + i * kWordBytes] = words[i];
		}
	}

	/**
	 * @brief Read DRAM shadow values as 64-bit words.
	 */
	std::vector<uint64_t> readback_dram_shadow(uint32_t base_addr, uint32_t word_count) const {
		constexpr uint32_t kWordBytes = 8;
		std::vector<uint64_t> out(word_count, 0);
		for (uint32_t i = 0; i < word_count; ++i) {
			auto it = dram_shadow_.find(base_addr + i * kWordBytes);
			out[i] = (it == dram_shadow_.end()) ? 0ULL : it->second;
		}
		return out;
	}

	/**
	 * @brief Pack FP16 vector into little-endian 64-bit words.
	 */
	static std::vector<uint64_t> pack_fp16_to_words64(const std::vector<uint16_t>& in) {
		std::vector<uint64_t> out;
		out.reserve((in.size() + 3) / 4);
		for (size_t i = 0; i < in.size(); i += 4) {
			uint64_t v = 0;
			v |= static_cast<uint64_t>(in[i]);
			if (i + 1 < in.size()) v |= (static_cast<uint64_t>(in[i + 1]) << 16);
			if (i + 2 < in.size()) v |= (static_cast<uint64_t>(in[i + 2]) << 32);
			if (i + 3 < in.size()) v |= (static_cast<uint64_t>(in[i + 3]) << 48);
			out.push_back(v);
		}
		return out;
	}

	/**
	 * @brief Prepare activation/weight/psum binary data into DRAM shadow.
	 */
	void prepare_binary_data_into_dram(
		uint32_t addr_act,
		uint32_t addr_wgt,
		uint32_t addr_psum,
		const std::vector<uint16_t>& act,
		const std::vector<uint16_t>& wgt,
		const std::vector<uint16_t>& psum) {
		std::cout << "[runner] Preload activation into DRAM shadow" << std::endl;
		preload_dram_shadow(addr_act, pack_fp16_to_words64(act));

		std::cout << "[runner] Preload weight into DRAM shadow" << std::endl;
		preload_dram_shadow(addr_wgt, pack_fp16_to_words64(wgt));

		if (!psum.empty()) {
			std::cout << "[runner] Preload partial sum into DRAM shadow" << std::endl;
			preload_dram_shadow(addr_psum, pack_fp16_to_words64(psum));
		}
	}

	/**
	 * @brief Validate testcase directory and required files.
	 * @return True when testcase is usable.
	 */
	bool load_testcase(const std::string& path, fs::path& testcase_dir) {
		testcase_dir = fs::path(path);
		if (!fs::exists(testcase_dir / "config.json")) {
			std::cerr << "Missing config.json under testcase dir: " << testcase_dir << std::endl;
			return false;
		}
		return true;
	}

	/**
	 * @brief Execute one DMA wave stage (dram_to_spm or spm_to_dram).
	 * @return True when all transfers and wave sync complete.
	 */
	bool run_dma_wave(const DmaWaveCfg& wave, DmaTransferCfg::Direction stage_direction) {
		if (!launch_dma_wave_async(wave, stage_direction)) {
			return false;
		}
		return wait_dma_wave_result(wave, stage_direction);
	}

	/**
	 * @brief Return true when wave contains transfer(s) for target DMA stage.
	 */
	bool wave_has_stage(const DmaWaveCfg& wave, DmaTransferCfg::Direction stage_direction) const {
		for (const auto& t : wave.transfers) {
			if (t.direction == stage_direction) {
				return true;
			}
		}
		return false;
	}

	/**
	 * @brief Build linear 4D address generator for contiguous 64-bit words.
	 */
	static cluster_json::DmaAddrGen4D make_linear_addr_gen4d(uint32_t base_addr, uint32_t word_count) {
		cluster_json::DmaAddrGen4D gen;
		gen.enabled = true;
		gen.base_addr = base_addr;
		gen.iter = {1, 1, word_count, 1};
		gen.stride = {
			static_cast<int32_t>(word_count * 8),
			static_cast<int32_t>(word_count * 8),
			8,
			8,
		};
		return gen;
	}

	/**
	 * @brief Build SPM address list for one transfer endpoint.
	 */
	static std::vector<uint32_t> build_transfer_spm_addr_list(
		const DmaTransferCfg& t,
		bool use_src_endpoint) {
		if (t.size_words64 == 0) {
			return {};
		}

		const uint32_t spm_base = use_src_endpoint ? t.src_spm_addr : t.dst_spm_addr;
		const auto& addr_gen_ref = use_src_endpoint ? t.src_addr_gen : t.dst_addr_gen;
		auto addr_gen = addr_gen_ref;
		if (!addr_gen.enabled) {
			addr_gen = make_linear_addr_gen4d(spm_base, t.size_words64);
		}
		return cluster_json::build_dma_addr_list(addr_gen, spm_base, t.size_words64);
	}

	/**
	 * @brief Collect current-wave active SPM footprint (compute inputs and pending writeback source).
	 */
	static std::vector<uint32_t> collect_current_wave_active_spm_addrs(const DmaWaveCfg& wave) {
		std::vector<uint32_t> addrs;
		for (const auto& t : wave.transfers) {
			if (t.direction == DmaTransferCfg::Direction::DramToSpm) {
				auto v = build_transfer_spm_addr_list(t, false);
				addrs.insert(addrs.end(), v.begin(), v.end());
			} else {
				auto v = build_transfer_spm_addr_list(t, true);
				addrs.insert(addrs.end(), v.begin(), v.end());
			}
		}
		std::sort(addrs.begin(), addrs.end());
		addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
		return addrs;
	}

	/**
	 * @brief Collect next-wave D2S destination SPM footprint for prefetch.
	 */
	static std::vector<uint32_t> collect_next_wave_d2s_dst_spm_addrs(const DmaWaveCfg& wave) {
		std::vector<uint32_t> addrs;
		for (const auto& t : wave.transfers) {
			if (t.direction != DmaTransferCfg::Direction::DramToSpm) {
				continue;
			}
			auto v = build_transfer_spm_addr_list(t, false);
			addrs.insert(addrs.end(), v.begin(), v.end());
		}
		std::sort(addrs.begin(), addrs.end());
		addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
		return addrs;
	}

	struct SpmRegionInfo {
		std::string label;
		std::vector<uint32_t> addrs;
	};

	/**
	 * @brief Build transfer label for overlap diagnostics.
	 */
	static std::string build_transfer_region_label(
		uint32_t wave_id,
		const DmaTransferCfg& t,
		bool use_src_endpoint) {
		std::ostringstream oss;
		oss << "wave=" << wave_id
			<< " transfer(tensor=" << t.tensor
			<< ",dir=" << (t.direction == DmaTransferCfg::Direction::DramToSpm ? "d2s" : "s2d")
			<< ",endpoint=" << (use_src_endpoint ? "src_spm" : "dst_spm")
			<< ",section=" << t.section << ")";
		return oss.str();
	}

	/**
	 * @brief Build AGU label for overlap diagnostics.
	 */
	static std::string build_agu_region_label(uint32_t wave_id, const char* agu_name) {
		std::ostringstream oss;
		oss << "wave=" << wave_id << " plan(" << agu_name << ")";
		return oss.str();
	}

	/**
	 * @brief Collect per-region SPM footprint for current wave (DMA + AGU plan).
	 * @return False when AGU footprint is too large to analyze safely.
	 */
	static bool collect_current_wave_region_infos(
		const ClusterPlan& plan,
		uint8_t current_spm_map,
		const DmaWaveCfg& current_wave,
		std::vector<SpmRegionInfo>& regions,
		std::string& fail_reason) {
		regions.clear();
		fail_reason.clear();

		for (const auto& t : current_wave.transfers) {
			const bool use_src = (t.direction == DmaTransferCfg::Direction::SpmToDram);
			auto addrs = build_transfer_spm_addr_list(t, use_src);
			if (addrs.empty()) {
				continue;
			}
			std::sort(addrs.begin(), addrs.end());
			addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
			regions.push_back(SpmRegionInfo{
				build_transfer_region_label(current_wave.wave_id, t, use_src),
				std::move(addrs),
			});
		}

		auto map_port_to_group = [&](uint8_t map_val, uint32_t port_id) -> uint32_t {
			return (static_cast<uint32_t>(map_val) >> (port_id * 2)) & 0x3u;
		};

		auto convert_agu_local_word_to_global_byte = [&](uint8_t map_val, uint32_t port_id, uint32_t local_word_addr) {
			const uint32_t group = map_port_to_group(map_val, port_id);
			const uint32_t global_word_addr = group * SPM_SRAM_BANK_DEPTH_WORDS * (SPM_NUM_BANKS_PER_GROUP + 1) + local_word_addr;
			return global_word_addr * 8u;
		};

		auto add_agu_region = [&](const AguCfg& agu, const char* name, uint32_t port_id) -> bool {
			std::vector<uint32_t> addrs;
			if (!append_agu_footprint_addrs(agu, addrs)) {
				fail_reason = std::string("agu_footprint_too_large:") + name;
				return false;
			}
			if (addrs.empty()) {
				return true;
			}

			for (auto& addr : addrs) {
				addr = convert_agu_local_word_to_global_byte(current_spm_map, port_id, addr);
			}

			std::sort(addrs.begin(), addrs.end());
			addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
			regions.push_back(SpmRegionInfo{build_agu_region_label(current_wave.wave_id, name), std::move(addrs)});
			return true;
		};

		if (!add_agu_region(plan.agu_ps, "agu_ps", AGU_PS)) return false;
		if (!add_agu_region(plan.agu_pd, "agu_pd", AGU_PD)) return false;
		if (!add_agu_region(plan.agu_pli, "agu_pli", AGU_PLI)) return false;
		if (!add_agu_region(plan.agu_plo, "agu_plo", AGU_PLO)) return false;
		return true;
	}

	/**
	 * @brief Collect per-transfer destination SPM footprints for next-wave D2S prefetch.
	 */
	static void collect_next_wave_prefetch_region_infos(
		const DmaWaveCfg& next_wave,
		std::vector<SpmRegionInfo>& regions) {
		regions.clear();
		for (const auto& t : next_wave.transfers) {
			if (t.direction != DmaTransferCfg::Direction::DramToSpm) {
				continue;
			}
			auto addrs = build_transfer_spm_addr_list(t, false);
			if (addrs.empty()) {
				continue;
			}
			std::sort(addrs.begin(), addrs.end());
			addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
			regions.push_back(SpmRegionInfo{
				build_transfer_region_label(next_wave.wave_id, t, false),
				std::move(addrs),
			});
		}
	}

	/**
	 * @brief Find overlap between two sorted unique address lists.
	 */
	static bool find_overlap_span(
		const std::vector<uint32_t>& a,
		const std::vector<uint32_t>& b,
		uint32_t& overlap_first,
		uint32_t& overlap_last,
		uint32_t& overlap_count) {
		overlap_first = 0;
		overlap_last = 0;
		overlap_count = 0;
		size_t i = 0;
		size_t j = 0;
		while (i < a.size() && j < b.size()) {
			if (a[i] == b[j]) {
				if (overlap_count == 0) {
					overlap_first = a[i];
				}
				overlap_last = a[i];
				++overlap_count;
				++i;
				++j;
			} else if (a[i] < b[j]) {
				++i;
			} else {
				++j;
			}
		}
		return overlap_count > 0;
	}

	/**
	 * @brief Append AGU-generated address footprint for one AGU config.
	 * @return False when footprint grows beyond guard limit (conservative fallback).
	 */
	static bool append_agu_footprint_addrs(const AguCfg& agu, std::vector<uint32_t>& addrs) {
		static constexpr size_t kMaxFootprintEntries = 1u << 20;
		if (!agu.enable) {
			return true;
		}

		const uint32_t i0 = std::max<uint32_t>(1, agu.iter0);
		const uint32_t i1 = std::max<uint32_t>(1, agu.iter1);
		const uint32_t i2 = std::max<uint32_t>(1, agu.iter2);
		const uint32_t i3 = std::max<uint32_t>(1, agu.iter3);
		const int64_t base = (static_cast<int64_t>(static_cast<uint64_t>(agu.base_addr_h) << 32) |
			static_cast<int64_t>(agu.base_addr));

		for (uint32_t a3 = 0; a3 < i3; ++a3) {
			for (uint32_t a2 = 0; a2 < i2; ++a2) {
				for (uint32_t a1 = 0; a1 < i1; ++a1) {
					for (uint32_t a0 = 0; a0 < i0; ++a0) {
						const int64_t off =
							static_cast<int64_t>(a0) * static_cast<int64_t>(agu.stride0) +
							static_cast<int64_t>(a1) * static_cast<int64_t>(agu.stride1) +
							static_cast<int64_t>(a2) * static_cast<int64_t>(agu.stride2) +
							static_cast<int64_t>(a3) * static_cast<int64_t>(agu.stride3);
						const int64_t addr = base + off;
						if (addr >= 0 && addr <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
							addrs.push_back(static_cast<uint32_t>(addr));
							if (addrs.size() > kMaxFootprintEntries) {
								return false;
							}
						}
					}
				}
			}
		}
		return true;
	}

	/**
	 * @brief Collect current compute plan AGU read/write SPM footprint.
	 */
	static bool collect_current_plan_active_spm_addrs(const ClusterPlan& plan, std::vector<uint32_t>& addrs) {
		addrs.clear();
		if (!append_agu_footprint_addrs(plan.agu_ps, addrs)) return false;
		if (!append_agu_footprint_addrs(plan.agu_pd, addrs)) return false;
		if (!append_agu_footprint_addrs(plan.agu_pli, addrs)) return false;
		if (!append_agu_footprint_addrs(plan.agu_plo, addrs)) return false;
		std::sort(addrs.begin(), addrs.end());
		addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
		return true;
	}

	/**
	 * @brief Check whether next-wave D2S prefetch is safe against current-wave active SPM footprint.
	 */
	static bool is_safe_prefetch_spm_nonoverlap(
		const ClusterPlan& current_plan,
		uint8_t current_spm_map,
		const DmaWaveCfg& current_wave,
		const DmaWaveCfg& next_wave,
		uint32_t& hazard_addr,
		std::string& conflict_current_region,
		std::string& conflict_next_region,
		uint32_t& overlap_first,
		uint32_t& overlap_last,
		uint32_t& overlap_count,
		std::string& fail_reason) {
		hazard_addr = 0;
		overlap_first = 0;
		overlap_last = 0;
		overlap_count = 0;
		conflict_current_region.clear();
		conflict_next_region.clear();
		fail_reason.clear();

		// Fast-path guard: if current wave and next-wave prefetch target exactly
		// the same section, prefetch is unsafe by construction.
		for (const auto& curr_t : current_wave.transfers) {
			if (curr_t.section.empty()) {
				continue;
			}
			for (const auto& next_t : next_wave.transfers) {
				if (next_t.direction != DmaTransferCfg::Direction::DramToSpm) {
					continue;
				}
				if (next_t.section.empty()) {
					continue;
				}
				if (curr_t.section == next_t.section) {
					hazard_addr = 0;
					const bool curr_use_src = (curr_t.direction == DmaTransferCfg::Direction::SpmToDram);
					conflict_current_region = build_transfer_region_label(current_wave.wave_id, curr_t, curr_use_src);
					conflict_next_region = build_transfer_region_label(next_wave.wave_id, next_t, false);
					overlap_first = 0;
					overlap_last = 0;
					overlap_count = 0;
					fail_reason = "section_overlap_fastpath";
					return false;
				}
			}
		}

		std::vector<SpmRegionInfo> current_regions;
		if (!collect_current_wave_region_infos(current_plan, current_spm_map, current_wave, current_regions, fail_reason)) {
			return false;
		}

		std::vector<SpmRegionInfo> next_regions;
		collect_next_wave_prefetch_region_infos(next_wave, next_regions);
		if (current_regions.empty() || next_regions.empty()) {
			return true;
		}

		for (const auto& curr : current_regions) {
			for (const auto& next : next_regions) {
				uint32_t first = 0;
				uint32_t last = 0;
				uint32_t cnt = 0;
				if (find_overlap_span(curr.addrs, next.addrs, first, last, cnt)) {
					hazard_addr = first;
					conflict_current_region = curr.label;
					conflict_next_region = next.label;
					overlap_first = first;
					overlap_last = last;
					overlap_count = cnt;
					fail_reason = "spm_overlap";
					return false;
				}
			}
		}
		return true;
	}

	/**
	 * @brief Launch one DMA wave stage asynchronously.
	 * @return True when launch succeeds or stage is empty.
	 */
	bool launch_dma_wave_async(const DmaWaveCfg& wave, DmaTransferCfg::Direction stage_direction) {
		uint32_t total_words = 0;
		for (const auto& t : wave.transfers) {
			if (t.direction != stage_direction) {
				continue;
			}
			total_words += t.size_words64;
		}

		if (total_words == 0) {
			return true;
		}

		const uint64_t wave_stage_key = make_wave_stage_key(wave.wave_id, stage_direction);
		dma_completed_waves_.erase(wave_stage_key);
		dma_wave_result_.erase(wave_stage_key);
		const uint32_t dma_latency_cycles = std::max<uint32_t>(1, total_words / 64 + 1);
		std::cout << sc_core::sc_time_stamp()
				  << " [dma] launch async wave_id=" << wave.wave_id
				  << " stage="
				  << (stage_direction == DmaTransferCfg::Direction::DramToSpm ? "dram_to_spm" : "spm_to_dram")
				  << " total_words64=" << total_words
				  << " latency_cycles=" << dma_latency_cycles
				  << std::endl;
		request_dma_wave_work(wave, stage_direction, dma_latency_cycles);
		return true;
	}

	/**
	 * @brief Wait for DMA wave stage completion and return stage result.
	 * @return True when stage is successful or empty.
	 */
	bool wait_dma_wave_result(const DmaWaveCfg& wave, DmaTransferCfg::Direction stage_direction) {
		if (!wave_has_stage(wave, stage_direction)) {
			return true;
		}
		if (!wait_dma_wave_done(wave.wave_id, stage_direction, 200000)) {
			return false;
		}
		const uint64_t wave_stage_key = make_wave_stage_key(wave.wave_id, stage_direction);
		auto it = dma_wave_result_.find(wave_stage_key);
		return it != dma_wave_result_.end() && it->second;
	}

	/**
	 * @brief Load testcase config/binaries, execute plans, and collect verify stats.
	 * @return True when execution and numerical criteria pass.
	 */
	bool run_from_config(const std::string& path) {
		std::cout << "[runner] Loading test from: " << path << std::endl;
		fs::path p;
		if (!load_testcase(path, p)) {
			return false;
		}

		auto root = JsonParser::parse_file((p / "config.json").string());
		if (!root || !root->is_object()) {
			std::cerr << "Failed to parse config.json" << std::endl;
			return false;
		}

		auto meta = (*root)["meta"];
		auto software = (*root)["software"];
		if (!meta || !software) {
			std::cerr << "Missing meta or software section" << std::endl;
			return false;
		}

		bool is_ultra = false;
		if ((*meta)["ultra_mode"]) is_ultra = (*meta)["ultra_mode"]->as_bool();

		auto files = (*software)["files"];
		auto act = read_binary_file<uint16_t>((p / (*files)["activation"]->as_string()).string());
		auto wgt = read_binary_file<uint16_t>((p / (*files)["weight"]->as_string()).string());
		std::vector<uint16_t> psum;
		if ((*files)["partial_sum"]) {
			psum = read_binary_file<uint16_t>((p / (*files)["partial_sum"]->as_string()).string());
		}
		gold_ = {};
		if ((*files)["output"]) {
			gold_ = read_binary_file<uint16_t>((p / (*files)["output"]->as_string()).string());
		}
		if (gold_.empty() && (*files)["output_gold"]) {
			gold_ = read_binary_file<uint16_t>((p / (*files)["output_gold"]->as_string()).string());
		}

		auto pe_prog_path = p / (*files)["pe_program"]->as_string();
		std::vector<uint16_t> pe_prog;
		if (fs::exists(pe_prog_path)) {
			pe_prog = read_binary_file<uint16_t>(pe_prog_path.string());
		}

		std::vector<uint32_t> scan_chain_data;
		if (fs::exists(p / "scan_chain.bin")) {
			scan_chain_data = read_binary_file<uint32_t>((p / "scan_chain.bin").string());
		}

		uint32_t addr_act = 0x00000000;
		uint32_t addr_wgt = 0x10000000;
		uint32_t addr_psum = 0x20000000;
		uint32_t addr_out = 0x30000000;
		auto dram_map = (*software)["dram_mapping"];
		if (dram_map) {
			if ((*dram_map)["activation"]) addr_act = (*dram_map)["activation"]->as_int64();
			if ((*dram_map)["weight"]) addr_wgt = (*dram_map)["weight"]->as_int64();
			if ((*dram_map)["partial_sum"]) addr_psum = (*dram_map)["partial_sum"]->as_int64();
			if ((*dram_map)["output"]) addr_out = (*dram_map)["output"]->as_int64();
		}

		const uint32_t addr_out_run = cluster_json::get_spm_tensor_addr_or_default(software, "output", addr_out, is_ultra);
		const auto spm_section_addr_map = cluster_json::parse_spm_sections(software);
		const auto dma_waves = cluster_json::parse_dma_waves(software, spm_section_addr_map);
		const auto plans = cluster_json::parse_cluster_plans(software);

		auto parse_u64_shape = [](const std::shared_ptr<JsonValue>& n) {
			std::vector<uint64_t> shape;
			if (!n || !n->is_array()) {
				return shape;
			}
			for (const auto& v : n->as_array()) {
				if (!v) {
					return std::vector<uint64_t>{};
				}
				shape.push_back(static_cast<uint64_t>(v->as_int64()));
			}
			return shape;
		};

		auto shape_prod = [](const std::vector<uint64_t>& shape) {
			if (shape.empty()) {
				return uint64_t{0};
			}
			long double p = 1.0;
			for (uint64_t d : shape) {
				p *= static_cast<long double>(d);
			}
			if (p < 0.0L) {
				return uint64_t{0};
			}
			return static_cast<uint64_t>(p);
		};

		auto read_u64_field = [](const std::shared_ptr<JsonValue>& obj, const char* key, uint64_t& out) {
			if (!obj || !obj->is_object()) {
				return false;
			}
			auto v = (*obj)[key];
			if (!v) {
				return false;
			}
			out = static_cast<uint64_t>(v->as_int64());
			return true;
		};

		uint64_t estimated_macs = 0;
		std::string mode;
		if ((*meta)["mode"] && (*meta)["mode"]->is_string()) {
			mode = (*meta)["mode"]->as_string();
		} else if ((*root)["mode"] && (*root)["mode"]->is_string()) {
			mode = (*root)["mode"]->as_string();
		}

		if (mode == "conv2d") {
			auto tensor_shapes = (*meta)["tensor_shapes"];
			if (tensor_shapes && tensor_shapes->is_object()) {
				const auto out_shape = parse_u64_shape((*tensor_shapes)["output"]);
				const auto wgt_shape = parse_u64_shape((*tensor_shapes)["weight"]);
				if (out_shape.size() == 4 && wgt_shape.size() == 4) {
					const uint64_t out_elems = shape_prod(out_shape);
					const uint64_t kh = wgt_shape[1];
					const uint64_t kw = wgt_shape[2];
					const uint64_t ic = wgt_shape[3];
					estimated_macs = out_elems * kh * kw * ic;
				}
			}
		} else if (mode == "gemm") {
			uint64_t m = 0;
			uint64_t n = 0;
			uint64_t k = 0;
			if (!read_u64_field(root, "M", m)) {
				read_u64_field(meta, "M", m);
			}
			if (!read_u64_field(root, "N", n)) {
				read_u64_field(meta, "N", n);
			}
			if (!read_u64_field(root, "K", k)) {
				read_u64_field(meta, "K", k);
			}
			if (m > 0 && n > 0 && k > 0) {
				estimated_macs = m * n * k;
			}
		}

		perf_summary_.total_macs = estimated_macs;
		perf_summary_.total_flops = estimated_macs * 2ull;

		power_on_and_reset();
		config_spm_map(0xE4);
		prepare_binary_data_into_dram(addr_act, addr_wgt, addr_psum, act, wgt, psum);

		if (!scan_chain_data.empty()) {
			send_scan_chain_words_reverse(scan_chain_data);
			wait_cycles(5);
		}
		load_pe_program(pe_prog);

		bool all_ok = true;
		std::vector<bool> prefetched_d2s(plans.size(), false);
		std::vector<bool> inflight_d2s(plans.size(), false);

		auto wave_spm_map = [&](size_t idx) -> uint8_t {
			if (idx < dma_waves.size() && dma_waves[idx].has_spm_map) {
				return dma_waves[idx].spm_map_val;
			}
			return 0xE4;
		};

		for (size_t i = 0; i < plans.size(); ++i) {
			auto plan = plans[i];
			const uint8_t curr_spm_map = wave_spm_map(i);

			if (i < dma_waves.size()) {
				// Prefetch current wave D2S if not already done by previous wave async launch.
				if (inflight_d2s[i]) {
					std::cout << sc_core::sc_time_stamp()
							  << " [dma] wait current_wave D2S wave_id=" << dma_waves[i].wave_id
							  << std::endl;
					if (!wait_dma_wave_result(dma_waves[i], DmaTransferCfg::Direction::DramToSpm)) {
						all_ok = false;
						break;
					}
					inflight_d2s[i] = false;
					prefetched_d2s[i] = true;
				}
				// If not prefetched yet, do it now before AGU config and wave start to maximize overlap.
				if (!prefetched_d2s[i]) {
					config_spm_map(curr_spm_map);
					std::cout << sc_core::sc_time_stamp()
							  << " [dma] prefetch current_wave D2S wave_id=" << dma_waves[i].wave_id
							  << std::endl;
					if (!run_dma_wave(dma_waves[i], DmaTransferCfg::Direction::DramToSpm)) {
						all_ok = false;
						break;
					}
					prefetched_d2s[i] = true;
				}
			}

			config_spm_map(curr_spm_map);
			cfg_agu(AGU_PS, plan.agu_ps);
			cfg_agu(AGU_PD, plan.agu_pd);
			cfg_agu(AGU_PLI, plan.agu_pli);
			cfg_agu(AGU_PLO, plan.agu_plo);
			cfg_hddu_global(plan.global_mask, is_ultra ? 0x2 : 0x1);
			start_all();

			// Overlap: prefetch next wave input while current wave computes.
			const size_t next_idx = i + 1;
			if (next_idx < plans.size() && i < dma_waves.size() && next_idx < dma_waves.size() &&
				!prefetched_d2s[next_idx] && !inflight_d2s[next_idx]) {
				uint32_t hazard_addr = 0;
				std::string conflict_current_region;
				std::string conflict_next_region;
				uint32_t overlap_first = 0;
				uint32_t overlap_last = 0;
				uint32_t overlap_count = 0;
				std::string fail_reason;
				if (!is_safe_prefetch_spm_nonoverlap(
						plan,
						curr_spm_map,
						dma_waves[i],
						dma_waves[next_idx],
						hazard_addr,
						conflict_current_region,
						conflict_next_region,
						overlap_first,
						overlap_last,
						overlap_count,
						fail_reason)) {
					std::cout << sc_core::sc_time_stamp()
							  << " [dma] skip prefetch next_wave=" << next_idx
							  << " reason=" << (fail_reason.empty() ? "spm_overlap_or_unknown" : fail_reason)
							  << " hazard_addr=0x" << std::hex << hazard_addr << std::dec
							  << " current_region=\"" << conflict_current_region << "\""
							  << " next_region=\"" << conflict_next_region << "\""
							  << " overlap_first=0x" << std::hex << overlap_first << std::dec
							  << " overlap_last=0x" << std::hex << overlap_last << std::dec
							  << " overlap_count=" << overlap_count
							  << std::endl;
				} else {
					if (!launch_dma_wave_async(dma_waves[next_idx], DmaTransferCfg::Direction::DramToSpm)) {
						all_ok = false;
						break;
					}
					if (wave_has_stage(dma_waves[next_idx], DmaTransferCfg::Direction::DramToSpm)) {
						inflight_d2s[next_idx] = true;
					}
				}
			}

			std::cout << sc_core::sc_time_stamp()
					  << " [runner] before wait_hddu_done"
					  << " wave_idx=" << i
					  << " inflight_next_d2s="
					  << ((next_idx < inflight_d2s.size() && inflight_d2s[next_idx]) ? "1" : "0")
					  << " dma_active_wave_id=" << dma_active_wave_id_
					  << " dma_queue_depth=" << dma_requests_.size()
					  << std::endl;

			if (!wait_hddu_done(WAVE_TIMEOUT_CYCLES, POLL_INTERVAL_CYCLES)) {
				all_ok = false;
				break;
			}
			stop_all();
			if (i < dma_waves.size() && !run_dma_wave(dma_waves[i], DmaTransferCfg::Direction::SpmToDram)) {
				all_ok = false;
				break;
			}
		}

		size_t out_words = gold_.size() / 4;
		if (gold_.size() % 4 != 0) out_words++;

		bool has_output_dma_writeback = false;
		for (const auto& w : dma_waves) {
			for (const auto& t : w.transfers) {
				if (t.tensor == "output" && t.direction == DmaTransferCfg::Direction::SpmToDram) {
					has_output_dma_writeback = true;
					break;
				}
			}
			if (has_output_dma_writeback) {
				break;
			}
		}

		std::vector<uint64_t> res_vec;
		if (has_output_dma_writeback) {
			res_vec = readback_dram_shadow(addr_out, static_cast<uint32_t>(out_words));
		} else {
			std::vector<uint32_t> out_addrs;
			out_addrs.reserve(out_words);
			for (size_t i = 0; i < out_words; ++i) {
				out_addrs.push_back(addr_out_run + static_cast<uint32_t>(i) * 8u);
			}
			dma_read_words64(out_addrs, res_vec);
		}

		std::vector<uint16_t> res_fp16;
		for (uint64_t w : res_vec) {
			res_fp16.push_back(w & 0xFFFF);
			res_fp16.push_back((w >> 16) & 0xFFFF);
			res_fp16.push_back((w >> 32) & 0xFFFF);
			res_fp16.push_back((w >> 48) & 0xFFFF);
		}
		if (res_fp16.size() > gold_.size()) res_fp16.resize(gold_.size());
		verify_stats_ = verify_fp16_vectors(gold_, res_fp16, 2e-2f, true);

		const sc_core::sc_time sim_elapsed = sc_core::sc_time_stamp();
		const double sim_ns = sim_elapsed / sc_core::sc_time(1, sc_core::SC_NS);
		const double sim_seconds = sim_elapsed.to_seconds();
		const double dma_wait_ns = timing_stats_.dma_wait_time / sc_core::sc_time(1, sc_core::SC_NS);
		const double hddu_wait_ns = timing_stats_.hddu_wait_time / sc_core::sc_time(1, sc_core::SC_NS);
		const double dma_busy_ns = timing_stats_.dma_busy_time / sc_core::sc_time(1, sc_core::SC_NS);
		std::cout << "[timing] sim_ns=" << sim_ns
				  << " dma_wait_ns=" << dma_wait_ns
				  << " dma_busy_ns=" << dma_busy_ns
				  << " hddu_wait_ns=" << hddu_wait_ns
				  << " dma_wave_count=" << timing_stats_.dma_wave_count
				  << std::endl;

		const uint64_t dram_total_bytes = perf_summary_.dram_read_bytes + perf_summary_.dram_write_bytes;
		perf_summary_.sim_seconds = sim_seconds;
		if (sim_seconds > 0.0) {
			perf_summary_.dram_throughput_Bps = static_cast<double>(dram_total_bytes) / sim_seconds;
			perf_summary_.dram_throughput_bps = perf_summary_.dram_throughput_Bps * 8.0;
			perf_summary_.macs_per_sec = static_cast<double>(perf_summary_.total_macs) / sim_seconds;
			perf_summary_.flops_per_sec = static_cast<double>(perf_summary_.total_flops) / sim_seconds;
		}
		if (dram_total_bytes > 0) {
			perf_summary_.arithmetic_intensity =
				static_cast<double>(perf_summary_.total_macs) / static_cast<double>(dram_total_bytes);
		}

		return (verify_stats_.cosine_similarity >= 0.99f) && all_ok;
	}

	SimRunnerOptions options_;
	bool started_ = false;
	bool run_finished_ = false;
	bool verify_done_ = false;
	bool pass_ = false;
	VerifyStats verify_stats_;
	std::vector<uint16_t> gold_;
	std::unordered_map<uint32_t, uint64_t> dram_shadow_;
	std::vector<uint32_t> noc_cmd_log_;

	sc_core::sc_signal<bool> dma_wave_done_;
	uint32_t dma_active_wave_id_ = 0;
	std::deque<DmaWaveSyncRequest> dma_requests_;
	std::set<uint64_t> dma_completed_waves_;
	std::unordered_map<uint64_t, bool> dma_wave_result_;
	sc_core::sc_event dma_req_event_;
	sc_core::sc_event dma_done_event_;
	TimingStats timing_stats_;
	PerformanceSummary perf_summary_;
};

} // namespace cluster_sim

int sc_main(int argc, char* argv[]) {
	using namespace cluster_sim;
	SimRunnerOptions options;

	// inline parse_simulation_args logic to avoid weird linking/compiler issues
	using namespace std;
	for (int i = 1; i < argc; ++i) {
		const std::string a(argv[i]);
		if (a == "-d" || a == "--dir") {
			if (i + 1 >= argc) {
				cerr << "missing value for -d/--dir" << endl;
				return 1;
			}
			options.test_path = argv[++i];
		} else if (a == "--case") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for --case" << std::endl;
				return 1;
			}
			const std::string v(argv[++i]);
			if (v == "conv2d") {
				options.scenario = ScenarioKind::Conv2D;
			} else if (v == "gemm") {
				options.scenario = ScenarioKind::GEMM;
			} else if (v == "both") {
				options.scenario = ScenarioKind::Both;
			} else {
				std::cerr << "unsupported case: " << v << std::endl;
				return 1;
			}
		} else if (a == "--clock-period") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for --clock-period" << std::endl;
				return 1;
			}
			options.clock_period_ns = std::stoi(argv[++i]);
			std::cout << "Clock period set to " << options.clock_period_ns << " ns" << std::endl;
		} else if (a == "--verbose" || a == "-v") {
			options.verbose = true;
		} else if (a == "--dry-run") {
			options.dry_run = true;
		} else if (a == "--no-dry-run") {
			options.dry_run = false;
		} else if (a == "--timeout-cycles") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for --timeout-cycles" << std::endl;
				return 1;
			}
			options.timeout_cycles = std::stoi(argv[++i]);
			std::cout << "Timeout cycles set to " << options.timeout_cycles << std::endl;
		} else if (a == "-f" || a == "--trace-file") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for -f/--trace-file" << std::endl;
				return 1;
			}
			options.trace_file = argv[++i];
			options.enable_trace = true;
		} else if (a == "--help" || a == "-h") {
			std::cout << "Usage: test_cluster_sim [-d <dir>] [--case conv2d|gemm|both] [--clock-period <ns>] [--timeout-cycles <cycles>] [--dry-run] [--verbose|-v] [-f <trace_file>]" << std::endl;
			return 1;
		} else {
			std::cerr << "unknown arg: " << a << std::endl;
			return 1;
		}
	}

	if (options.verbose) {
		std::cout << "[test_cluster_sim] verbose logging enabled" << std::endl;
	}

	const uint32_t cmd = pack_noc_cmd(CMD_START_PE, 0xABCDEF00u);
	if ((cmd & 0xFu) != static_cast<uint32_t>(CMD_START_PE)) {
		std::cerr << "pack_noc_cmd self-check failed" << std::endl;
		return 1;
	}

	const uint32_t load = pack_load_program(0x20, 0x55AA);
	if ((load & 0xFu) != static_cast<uint32_t>(CMD_LOAD_PROGRAM)) {
		std::cerr << "pack_load_program self-check failed" << std::endl;
		return 1;
	}

	const uint32_t scan = pack_scan_chain(1, 2, 3, 4, 1, true);
	if ((scan & 0xFu) != static_cast<uint32_t>(CMD_NOC_SCAN_CHAIN)) {
		std::cerr << "pack_scan_chain self-check failed" << std::endl;
		return 1;
	}

	sc_core::sc_clock clk("clk", sc_core::sc_time(options.clock_period_ns, sc_core::SC_NS));
	sc_core::sc_signal<bool> reset_n;
	sc_core::sc_signal<bool> power_enable_i;
	sc_core::sc_signal<bool> interrupt_o;

	sc_core::sc_signal<bool> s_axi_awvalid_i;
	sc_core::sc_signal<bool> s_axi_awready_o;
	sc_core::sc_signal<sc_dt::sc_uint<32>> s_axi_awaddr_i;
	sc_core::sc_signal<bool> s_axi_wvalid_i;
	sc_core::sc_signal<bool> s_axi_wready_o;
	sc_core::sc_signal<sc_dt::sc_biguint<64>> s_axi_wdata_i;
	sc_core::sc_signal<sc_dt::sc_uint<8>> s_axi_wstrb_i;
	sc_core::sc_signal<bool> s_axi_bvalid_o;
	sc_core::sc_signal<bool> s_axi_bready_i;
	sc_core::sc_signal<sc_dt::sc_uint<2>> s_axi_bresp_o;
	sc_core::sc_signal<bool> s_axi_arvalid_i;
	sc_core::sc_signal<bool> s_axi_arready_o;
	sc_core::sc_signal<sc_dt::sc_uint<32>> s_axi_araddr_i;
	sc_core::sc_signal<bool> s_axi_rvalid_o;
	sc_core::sc_signal<bool> s_axi_rready_i;
	sc_core::sc_signal<sc_dt::sc_biguint<64>> s_axi_rdata_o;
	sc_core::sc_signal<sc_dt::sc_uint<2>> s_axi_rresp_o;

	sc_core::sc_signal<bool> hsel_i;
	sc_core::sc_signal<sc_dt::sc_uint<32>> haddr_i;
	sc_core::sc_signal<bool> hwrite_i;
	sc_core::sc_signal<sc_dt::sc_uint<2>> htrans_i;
	sc_core::sc_signal<sc_dt::sc_uint<3>> hsize_i;
	sc_core::sc_signal<sc_dt::sc_uint<3>> hburst_i;
	sc_core::sc_signal<sc_dt::sc_uint<4>> hprot_i;
	sc_core::sc_signal<bool> hready_i;
	sc_core::sc_signal<sc_dt::sc_uint<32>> hwdata_i;
	sc_core::sc_signal<bool> hready_o;
	sc_core::sc_signal<bool> hresp_o;
	sc_core::sc_signal<sc_dt::sc_uint<32>> hrdata_o;

	ComputeClusterTestBench::DutType dut("Cluster", hybridacc::NetWorkOnChipConfig(4, 4));
	dut.clk(clk);
	dut.reset_n(reset_n);
	dut.power_enable_i(power_enable_i);
	dut.interrupt_o(interrupt_o);

	dut.s_axi_awvalid_i(s_axi_awvalid_i);
	dut.s_axi_awready_o(s_axi_awready_o);
	dut.s_axi_awaddr_i(s_axi_awaddr_i);
	dut.s_axi_wvalid_i(s_axi_wvalid_i);
	dut.s_axi_wready_o(s_axi_wready_o);
	dut.s_axi_wdata_i(s_axi_wdata_i);
	dut.s_axi_wstrb_i(s_axi_wstrb_i);
	dut.s_axi_bvalid_o(s_axi_bvalid_o);
	dut.s_axi_bready_i(s_axi_bready_i);
	dut.s_axi_bresp_o(s_axi_bresp_o);
	dut.s_axi_arvalid_i(s_axi_arvalid_i);
	dut.s_axi_arready_o(s_axi_arready_o);
	dut.s_axi_araddr_i(s_axi_araddr_i);
	dut.s_axi_rvalid_o(s_axi_rvalid_o);
	dut.s_axi_rready_i(s_axi_rready_i);
	dut.s_axi_rdata_o(s_axi_rdata_o);
	dut.s_axi_rresp_o(s_axi_rresp_o);

	dut.hsel_i(hsel_i);
	dut.haddr_i(haddr_i);
	dut.hwrite_i(hwrite_i);
	dut.htrans_i(htrans_i);
	dut.hsize_i(hsize_i);
	dut.hburst_i(hburst_i);
	dut.hprot_i(hprot_i);
	dut.hready_i(hready_i);
	dut.hwdata_i(hwdata_i);
	dut.hready_o(hready_o);
	dut.hresp_o(hresp_o);
	dut.hrdata_o(hrdata_o);

	ComputeClusterTestBench tb("cluster_tb", options);
	tb.clk(clk);
	tb.reset_n(reset_n);
	tb.power_enable_i(power_enable_i);
	tb.interrupt_o(interrupt_o);
	tb.s_axi_awvalid_i(s_axi_awvalid_i);
	tb.s_axi_awready_o(s_axi_awready_o);
	tb.s_axi_awaddr_i(s_axi_awaddr_i);
	tb.s_axi_wvalid_i(s_axi_wvalid_i);
	tb.s_axi_wready_o(s_axi_wready_o);
	tb.s_axi_wdata_i(s_axi_wdata_i);
	tb.s_axi_wstrb_i(s_axi_wstrb_i);
	tb.s_axi_bvalid_o(s_axi_bvalid_o);
	tb.s_axi_bready_i(s_axi_bready_i);
	tb.s_axi_bresp_o(s_axi_bresp_o);
	tb.s_axi_arvalid_i(s_axi_arvalid_i);
	tb.s_axi_arready_o(s_axi_arready_o);
	tb.s_axi_araddr_i(s_axi_araddr_i);
	tb.s_axi_rvalid_o(s_axi_rvalid_o);
	tb.s_axi_rready_i(s_axi_rready_i);
	tb.s_axi_rdata_o(s_axi_rdata_o);
	tb.s_axi_rresp_o(s_axi_rresp_o);
	tb.hsel_i(hsel_i);
	tb.haddr_i(haddr_i);
	tb.hwrite_i(hwrite_i);
	tb.htrans_i(htrans_i);
	tb.hsize_i(hsize_i);
	tb.hburst_i(hburst_i);
	tb.hprot_i(hprot_i);
	tb.hready_i(hready_i);
	tb.hwdata_i(hwdata_i);
	tb.hready_o(hready_o);
	tb.hresp_o(hresp_o);
	tb.hrdata_o(hrdata_o);
	if (options.enable_trace) {
		dut.enable_perffeto_trace();
		PerfettoTrace::getInstance().open(options.trace_file);
	}
	tb.init();
	sc_core::sc_start();
	if (options.enable_trace) {
		PerfettoTrace::getInstance().close();
	}
	const bool pass = tb.verify();
	const std::string case_name =
		(options.scenario == ScenarioKind::Conv2D)
			? "conv2d"
			: (options.scenario == ScenarioKind::GEMM ? "gemm" : "both");
	const auto& perf = tb.perf_summary();

	auto print_metric = [](const std::string& label, const std::string& value) {
		std::cout << "[test_cluster_sim] "
				  << std::left << std::setw(28) << label
				  << ": " << value << std::endl;
	};

	std::cout << "[test_cluster_sim] ===============================================" << std::endl;
	print_metric("Runner case", case_name);
	print_metric("Dry run", options.dry_run ? "1" : "0");
	print_metric("Simulation cycles", std::to_string(tb.cycle()));
	print_metric("NoC cmd count", std::to_string(tb.noc_cmd_log().size()));
	print_metric(
		"DRAM throughput",
		format_scaled(perf.dram_throughput_bps, "bps") + ", " +
		format_scaled(perf.dram_throughput_Bps, "B/s"));
	print_metric(
		"Total MACs / FLOPs",
		format_scaled(static_cast<double>(perf.total_macs), "MAC") + " / " +
		format_scaled(static_cast<double>(perf.total_flops), "FLOP"));
	print_metric(
		"Performance",
		format_scaled(perf.macs_per_sec, "MAC/s") + ", " +
		format_scaled(perf.flops_per_sec, "FLOP/s"));
	{
		std::ostringstream oss;
		oss << std::fixed << std::setprecision(6) << perf.arithmetic_intensity << " MAC/Byte";
		print_metric("Arithmetic intensity", oss.str());
	}
	std::cout << "[test_cluster_sim] ===============================================" << std::endl;

	return pass ? 0 : 1;
}
