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
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>
#include <deque>
#include <set>
#include <unordered_map>

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

using AguCfg = cluster_json::AguCfg;
using ClusterPlan = cluster_json::ClusterPlan;

struct DriverHooks {
	std::function<void(uint32_t, uint32_t)> mmio_write;
	std::function<uint32_t(uint32_t)> mmio_read;
	std::function<void(uint32_t, uint64_t)> data_write64;
	std::function<void(const std::vector<uint32_t>&, const std::vector<uint64_t>&)> data_write64_burst;
	std::function<uint64_t(uint32_t)> data_read64;
	std::function<void(const std::vector<uint32_t>&, std::vector<uint64_t>&)> data_read64_burst;
	std::function<void(bool)> set_power_enable;
	std::function<void(bool)> set_reset_n;
	std::function<void(uint32_t)> wait_cycles;
	std::function<bool(uint32_t)> wait_interrupt;
};

inline uint32_t pack_noc_cmd(message_command_t cmd, uint32_t param) {
	return (param & 0xFFFFFFF0u) | (static_cast<uint32_t>(cmd) & 0x0Fu);
}

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

inline uint32_t pack_load_program(uint16_t im_addr_bytes, uint16_t inst16) {
	uint32_t p = 0;
	p |= (static_cast<uint32_t>(im_addr_bytes) & PE_ROUTER_IM_ADDR_MASK) << PE_ROUTER_IM_ADDR_OFFSET;
	p |= (static_cast<uint32_t>(inst16) & PE_ROUTER_IM_DATA_MASK) << PE_ROUTER_IM_DATA_OFFSET;
	return pack_noc_cmd(CMD_LOAD_PROGRAM, p);
}

class ClusterSimDriver {
public:
	explicit ClusterSimDriver(DriverHooks hooks)
		: hooks_(std::move(hooks)) {
		if (!hooks_.mmio_write || !hooks_.mmio_read || !hooks_.data_write64 || !hooks_.data_write64_burst ||
			!hooks_.data_read64 || !hooks_.data_read64_burst ||
			!hooks_.set_power_enable || !hooks_.set_reset_n || !hooks_.wait_cycles) {
			throw std::invalid_argument("ClusterSimDriver hooks are incomplete");
		}
	}

	void power_on_and_reset(uint32_t reset_low_cycles = 4, uint32_t settle_cycles = 4) {
		hooks_.set_power_enable(true);
		hooks_.set_reset_n(false);
		hooks_.wait_cycles(reset_low_cycles);
		hooks_.set_reset_n(true);
		hooks_.wait_cycles(settle_cycles);
	}

	void power_off(uint32_t settle_cycles = 2) {
		hooks_.set_power_enable(false);
		hooks_.wait_cycles(settle_cycles);
	}

	void config_spm_map(uint8_t map_val) {
		hooks_.mmio_write(CLUSTER_SPM_CFG_MAP, static_cast<uint32_t>(map_val));
		hooks_.mmio_write(CLUSTER_SPM_CFG_UPDATE, 0x1u);
	}

	void dma_write_words64(const std::vector<uint32_t>& addrs, const std::vector<uint64_t>& datas) {
		if (addrs.empty() || datas.empty()) {
			return;
		}
		if (addrs.size() != datas.size()) {
			throw std::invalid_argument("dma_write_words64: addrs size mismatch datas size");
		}
		hooks_.data_write64_burst(addrs, datas);
	}

	void dma_read_words64(const std::vector<uint32_t>& addrs, std::vector<uint64_t>& datas) const {
		if (addrs.empty()) {
			datas.clear();
			return;
		}
		hooks_.data_read64_burst(addrs, datas);
	}

	void write_word64(uint32_t addr, uint64_t data) {
		hooks_.data_write64(addr, data);
	}

	uint64_t read_word64(uint32_t addr) const {
		return hooks_.data_read64(addr);
	}

	void dma_copy_words64(uint32_t src_addr, uint32_t dst_addr, uint32_t word_count) {
		constexpr uint32_t kWordBytes = 8;
		for (uint32_t i = 0; i < word_count; ++i) {
			const uint64_t v = hooks_.data_read64(src_addr + i * kWordBytes);
			hooks_.data_write64(dst_addr + i * kWordBytes, v);
		}
	}

	void cfg_agu(uint32_t bank, const AguCfg& c) {
		const uint32_t b = CLUSTER_HDDU_BASE + bank * AGU_BANK_STRIDE;
		if (!c.enable) {
			std::cout << "AGU bank " << bank << " disabled, skipping config" << std::endl;
            return;
        }
		hooks_.mmio_write(b + REG_BASE_ADDR, c.base_addr);
		hooks_.mmio_write(b + REG_BASE_ADDR_H, c.base_addr_h);
		hooks_.mmio_write(b + REG_ITER01, (static_cast<uint32_t>(c.iter1) << 16) | c.iter0);
		hooks_.mmio_write(b + REG_ITER23, (static_cast<uint32_t>(c.iter3) << 16) | c.iter2);
		hooks_.mmio_write(b + REG_STRIDE0, static_cast<uint32_t>(c.stride0));
		hooks_.mmio_write(b + REG_STRIDE1, static_cast<uint32_t>(c.stride1));
		hooks_.mmio_write(b + REG_STRIDE2, static_cast<uint32_t>(c.stride2));
		hooks_.mmio_write(b + REG_STRIDE3, static_cast<uint32_t>(c.stride3));
		hooks_.mmio_write(b + REG_LANE_CFG, c.lane_cfg);
		hooks_.mmio_write(b + REG_TAG_BASE, c.tag_base);
		hooks_.mmio_write(b + REG_TAG_STRIDE0, c.tag_stride0);
		hooks_.mmio_write(b + REG_TAG_STRIDE1, c.tag_stride1);
		hooks_.mmio_write(b + REG_TAG_CTRL, c.tag_ctrl);
		hooks_.mmio_write(b + REG_MASK_CFG, c.mask_cfg);
		hooks_.mmio_write(b + REG_CTRL, c.ultra ? (1u << (int)hybridacc::cluster::AguCtrlBit::CTRL_ULTRA) : 0u);
	}

	void cfg_hddu_global(uint32_t plane_en, uint32_t plane_mode) {
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN, plane_en);
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE, plane_mode);
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrlBit::START));
	}

	void noc_cmd_write(uint32_t packed_cmd) {
		hooks_.mmio_write(CLUSTER_NOC_CMD, packed_cmd);
	}

	void noc_init(uint32_t init_param) {
		noc_cmd_write(pack_noc_cmd(CMD_RESET, 0));
		noc_cmd_write(pack_noc_cmd(CMD_INIT, init_param));
	}

	void send_scan_chain_words_reverse(const std::vector<uint32_t>& words) {
		for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
			noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, words[static_cast<size_t>(i)]));
		}
	}

	void load_pe_program(const std::vector<uint16_t>& inst16) {
		for (uint32_t pc = 0; pc < inst16.size(); ++pc) {
			noc_cmd_write(pack_load_program(static_cast<uint16_t>(pc * 2), inst16[pc]));
		}
	}

	void start_all() {
		noc_cmd_write(pack_noc_cmd(CMD_START_PE, 0));
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrlBit::START));
	}

	void stop_all() {
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrlBit::STOP));
		noc_cmd_write(pack_noc_cmd(CMD_STOP_PE, 0));
	}

	void print_hddu_error_info() {
		uint32_t err_code = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_CTRL) & 0xFFu;
		uint32_t err_info0 = hooks_.mmio_read(CLUSTER_HDDU_BASE + 0x80);
		uint32_t err_info1 = hooks_.mmio_read(CLUSTER_HDDU_BASE + 0x84);
		std::cerr << "HDDU error detected! Error code: " << err_code
				  << ", info0: 0x" << std::hex << err_info0
				  << ", info1: 0x" << std::hex << err_info1 << std::dec << std::endl;
	}

	bool wait_hddu_done(uint32_t timeout_cycles, uint32_t poll_step = 1) {
		if (hooks_.wait_interrupt) {
			const bool irq_asserted = hooks_.wait_interrupt(timeout_cycles);
			if (!irq_asserted) {
				std::cerr << "HDDU wait timeout: interrupt not asserted within " << timeout_cycles << " cycles" << std::endl;
				return false;
			}

			const uint32_t st = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::ERROR)) {
				std::cerr << "HDDU error detected after interrupt, status=0x" << std::hex << st << std::dec << std::endl;
				print_hddu_error_info();
				return false;
			}
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::DONE)) {
				std::cout << "HDDU done detected by interrupt and confirmed via MMIO status" << std::endl;
				return true;
			}

			std::cerr << "Interrupt asserted but HDDU DONE is not set yet, fallback to MMIO polling" << std::endl;
		}

		for (uint32_t waited = 0; waited < timeout_cycles; waited += poll_step) {
			const uint32_t st = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::ERROR)) {
				std::cerr << "HDDU error detected, status=0x" << std::hex << st << std::dec << std::endl;
				print_hddu_error_info();
				return false;
			}
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::DONE)) {
				std::cout << "HDDU done detected after waiting " << waited << " cycles" << std::endl;
				return true;
			}
			hooks_.wait_cycles(poll_step);
		}
		return false;
	}

	HdduCounters read_hddu_counters() const {
		HdduCounters c;
		c.tx_pkt = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_TX_PKT);
		c.tx_byte = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_TX_BYTE);
		c.rx_byte = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_RX_BYTE);
		c.stall = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_STALL);
		return c;
	}

private:
	DriverHooks hooks_;
};

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

// Testbench backend that drives DUT via SystemC IO ports.
class ComputeClusterTbBackend : public sc_core::sc_module {
public:
	SC_HAS_PROCESS(ComputeClusterTbBackend);

	struct DmaWaveSyncRequest {
		uint32_t wave_id = 0;
		uint32_t latency_cycles = 1;
	};

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

	DutType& dut;

	ComputeClusterTbBackend(
		sc_core::sc_module_name name,
		DutType& dut_ref,
		int clock_period_ns = 10,
		int timeout_cycles = 200)
		: sc_core::sc_module(name),
		  dut(dut_ref),
		  kClockPeriodNs(clock_period_ns),
		  timeout_cycles(timeout_cycles),
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

		SC_THREAD(dma_wave_process);
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

	void mmio_write(uint32_t addr, uint32_t data) {
		if (addr == CLUSTER_NOC_CMD) {
			noc_cmd_log_.push_back(data);
		}
		if (!ahb_write_tx(addr, data)) {
			throw std::runtime_error("ComputeCluster MMIO write timeout");
		}
	}

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

	void data_write64(uint32_t addr, uint64_t data) {
		if (!data_write_tx(addr, data)) {
			throw std::runtime_error("ComputeCluster data write timeout");
		}
	}

	void data_write64_burst(const std::vector<uint32_t>& addrs, const std::vector<uint64_t>& words) {
		if (!data_write_burst_tx(addrs, words)) {
			throw std::runtime_error("ComputeCluster burst data write timeout");
		}
	}

	uint64_t data_read64(uint32_t addr) const {
		uint64_t out = 0;
		if (!const_cast<ComputeClusterTbBackend*>(this)->data_read_tx(addr, out)) {
			throw std::runtime_error("ComputeCluster data read timeout");
		}
		return out;
	}

	void data_read64_burst(const std::vector<uint32_t>& addrs, std::vector<uint64_t>& out) {
		out.assign(addrs.size(), 0);
		if (!data_read_burst_tx(addrs, out)) {
			throw std::runtime_error("ComputeCluster burst data read timeout");
		}
	}

	void set_power_enable(bool on) {
		power_enable_i.write(on);
	}

	void set_reset_n(bool rst_n) {
		reset_n.write(rst_n);
	}

	void wait_cycles(uint32_t n) {
		tick(n);
		cycle_ += n;
	}

	uint64_t cycle() const {
		return cycle_;
	}

	const std::vector<uint32_t>& noc_cmd_log() const {
		return noc_cmd_log_;
	}

	void request_dma_wave_sync(uint32_t wave_id, uint32_t latency_cycles = 1) {
		dma_requests_.push_back(DmaWaveSyncRequest{
			wave_id,
			std::max<uint32_t>(1, latency_cycles)
		});
		dma_req_event_.notify(sc_core::SC_ZERO_TIME);
	}

	bool wait_dma_wave_done(uint32_t wave_id, uint32_t timeout) {
		for (uint32_t i = 0; i < timeout; ++i) {
			if (dma_completed_waves_.count(wave_id) > 0) {
				return true;
			}
			tick(1);
			cycle_ += 1;
		}
		return false;
	}

	bool wait_interrupt_asserted(uint32_t timeout_cycles) {
		if (interrupt_o.read()) {
			return true;
		}
		for (uint32_t i = 0; i < timeout_cycles; ++i) {
			tick(1);
			cycle_ += 1;
			if (interrupt_o.read()) {
				return true;
			}
		}
		return false;
	}

private:
	static bool in_range(uint32_t addr, uint32_t base, uint32_t size) {
		return addr >= base && addr < (base + size);
	}

	void tick(uint32_t n = 1) {
		for (uint32_t i = 0; i < n; ++i) {
			wait(clk.negedge_event());
		}
	}

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
		tick(1);

		hsel_i.write(false);
		htrans_i.write(0); // IDLE
		hwdata_i.write(data);

		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (hready_o.read()) {
				hwdata_i.write(0);
				return true;
			}
		}

		hwdata_i.write(0);
		return false;
	}

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
		tick(1);

		hsel_i.write(false);
		htrans_i.write(0); // IDLE

		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (hready_o.read()) {
				out = hrdata_o.read().to_uint();
				return true;
			}
		}

		return false;
	}

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
				tick(1);
				s_axi_bready_i.write(false);
				return true;
			}
			tick(1);
			s_axi_awvalid_i.write(aw_done ? false : true);
			s_axi_wvalid_i.write(w_done ? false : true);

		}

		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_bready_i.write(false);
		tick(1);
		return false;
	}

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
				tick(1);
				return true;
			}
			tick(1);
			s_axi_arvalid_i.write(ar_done ? false : true);
		}

		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(false);
		tick(1);
		return false;
	}

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

			bool aw_ready = s_axi_awready_o.read();
			bool w_ready = s_axi_wready_o.read();
			bool b_valid = s_axi_bvalid_o.read();

			tick(1);

			if (aw_active && aw_ready) {
				s_axi_awvalid_i.write(false);
				aw_active = false;
				++aw_sent;
				// std::cout << sc_time_stamp() << " [TB] Burst write: AW sent " << aw_sent << "/" << addrs.size() << std::endl;
			}
			if (w_active && w_ready) {
				s_axi_wvalid_i.write(false);
				w_active = false;
				++w_sent;
				// std::cout << sc_time_stamp() << " [TB] Burst write: W sent " << w_sent << "/" << words.size() << std::endl;
			}

			if (b_valid && s_axi_bready_i.read()) {
				++bresp_done;
				// std::cout << sc_time_stamp() << " [TB] Burst write: B resp received " << bresp_done << "/" << words.size() << std::endl;
			}

			if (aw_sent == addrs.size() && w_sent == words.size() && !aw_active && !w_active && bresp_done == words.size()) {
				s_axi_awvalid_i.write(false);
				s_axi_wvalid_i.write(false);
				s_axi_bready_i.write(false);
				tick(1);
				return true;
			}
		}

		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_bready_i.write(false);
		tick(1);
		return false;
	}

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

			bool ar_ready = s_axi_arready_o.read();
			bool r_valid = s_axi_rvalid_o.read();
			uint64_t r_data = s_axi_rdata_o.read().to_uint64();

			tick(1);

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
				tick(1);
				return true;
			}
		}

		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(false);
		tick(1);
		return false;
	}

	void dma_wave_process() {
		while (true) {
			if (dma_requests_.empty()) {
				wait(dma_req_event_);
			}
			while (!dma_requests_.empty()) {
				auto req = dma_requests_.front();
				dma_requests_.pop_front();

				dma_wave_done_.write(false);
				dma_active_wave_id_ = req.wave_id;
				for (uint32_t c = 0; c < req.latency_cycles; ++c) {
					wait(clk.posedge_event());
				}
				dma_completed_waves_.insert(req.wave_id);
				dma_wave_done_.write(true);
				wait(clk.posedge_event());
				dma_wave_done_.write(false);
			}
		}
	}

	uint64_t cycle_ = 0;
	std::vector<uint32_t> noc_cmd_log_;

	sc_core::sc_signal<bool> dma_wave_done_;
	uint32_t dma_active_wave_id_ = 0;
	std::deque<DmaWaveSyncRequest> dma_requests_;
	std::set<uint32_t> dma_completed_waves_;
	sc_core::sc_event dma_req_event_;
};

class ScenarioRunner {
public:
	using DmaTransferCfg = cluster_json::DmaTransferCfg;
	using DmaWaveCfg = cluster_json::DmaWaveCfg;

	explicit ScenarioRunner(ClusterSimDriver& driver, ComputeClusterTbBackend& backend)
		: driver_(driver), backend_(backend) {}

	bool run(const SimRunnerOptions& opt) {
		if (opt.test_path.empty()) {
			 std::cerr << "No test path specified" << std::endl;
			return false;
		}
		return run_from_config(opt.test_path);
	}

private:

	void preload_dram_shadow(uint32_t base_addr, const std::vector<uint64_t>& words) {
		constexpr uint32_t kWordBytes = 8;
		for (uint32_t i = 0; i < words.size(); ++i) {
			dram_shadow_[base_addr + i * kWordBytes] = words[i];
		}
	}

	std::vector<uint64_t> readback_dram_shadow(uint32_t base_addr, uint32_t word_count) const {
		constexpr uint32_t kWordBytes = 8;
		std::vector<uint64_t> out(word_count, 0);
		for (uint32_t i = 0; i < word_count; ++i) {
			auto it = dram_shadow_.find(base_addr + i * kWordBytes);
			out[i] = (it == dram_shadow_.end()) ? 0ULL : it->second;
		}
		return out;
	}

	bool run_dma_wave(const DmaWaveCfg& wave, DmaTransferCfg::Direction stage_direction) {
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

		std::cout << "[runner] DMA wave " << wave.wave_id
				  << ": stage="
				  << (stage_direction == DmaTransferCfg::Direction::DramToSpm ? "dram_to_spm" : "spm_to_dram")
				  << std::endl;
		uint32_t total_words = 0;
		for (const auto& t : wave.transfers) {
			if (t.direction != stage_direction) {
				continue;
			}

			std::cout << "  [dma] tensor=" << t.tensor
					  << " dir=" << (t.direction == DmaTransferCfg::Direction::DramToSpm ? "dram_to_spm" : "spm_to_dram")
					  << " group=" << t.group_id
					  << " section=" << t.section
					  << " src=0x" << std::hex
					  << (t.direction == DmaTransferCfg::Direction::DramToSpm ? t.src_dram_addr : t.src_spm_addr)
					  << " dst=0x"
					  << (t.direction == DmaTransferCfg::Direction::DramToSpm ? t.dst_spm_addr : t.dst_dram_addr)
					  << std::dec
					  << " words=" << t.size_words64;

			if(t.src_addr_gen.enabled) std::cout << "\n    src4d=" << t.src_addr_gen;
			if(t.dst_addr_gen.enabled) std::cout << "\n    dst4d=" << t.dst_addr_gen;
			std::cout << std::endl;

			auto src_gen = t.src_addr_gen;
			auto dst_gen = t.dst_addr_gen;
			if (!src_gen.enabled) {
				std::cerr << "[runner][warn] Legacy DMA src description detected for tensor=" << t.tensor
						  << ", converting to gen4D linear format" << std::endl;
				src_gen = make_linear_addr_gen4d(
					t.direction == DmaTransferCfg::Direction::DramToSpm ? t.src_dram_addr : t.src_spm_addr,
					t.size_words64);
			}
			if (!dst_gen.enabled) {
				std::cerr << "[runner][warn] Legacy DMA dst description detected for tensor=" << t.tensor
						  << ", converting to gen4D linear format" << std::endl;
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
				driver_.dma_write_words64(dst_addrs, payload);
			} else {
				std::vector<uint64_t> payload;
				driver_.dma_read_words64(src_addrs, payload);
				for (uint32_t i = 0; i < t.size_words64; ++i) {
					dram_shadow_[dst_addrs[i]] = payload[i];
				}
			}
			total_words += t.size_words64;
		}

		if (total_words == 0) {
			return true;
		}

		const uint32_t dma_latency_cycles = std::max<uint32_t>(1, total_words / 64 + 1);
		backend_.request_dma_wave_sync(wave.wave_id, dma_latency_cycles);
		const bool done = backend_.wait_dma_wave_done(wave.wave_id, 200000);
		if (!done) {
			std::cerr << "[runner] DMA wave " << wave.wave_id << " TIMEOUT" << std::endl;
			return false;
		}
		return true;
	}

	bool run_from_config(const std::string& path) {
		// ---------------------------------------
		// step 1: load config and binaries
		// ---------------------------------------
		std::cout << "[runner] Loading test from: " << path << std::endl;
		fs::path p(path);

		auto root = JsonParser::parse_file((p / "config.json").string());
		if (!root || !root->is_object()) {
			std::cerr << "Failed to parse config.json" << std::endl;
			return false;
		}

		auto meta = (*root)["meta"];
		// auto hardware = (*root)["hardware"];
		// auto firmware = (*root)["firmware"];
		auto software = (*root)["software"];

		if (!meta || !software) {
			 std::cerr << "Missing meta or software section" << std::endl;
			 return false;
		}

		bool is_ultra = false;
		if ((*meta)["ultra_mode"]) is_ultra = (*meta)["ultra_mode"]->as_bool();

		// Print loaded config for debugging
		std::cout << "  > Config loaded:" << std::endl;
		std::cout << "    meta: " << std::endl;
		for (const auto& [k, v] : meta->as_object()) {
			std::cout << "      " << k << ": " << cluster_json::format_json(v) << std::endl;
		}

		auto files = (*software)["files"];

		// Load binaries
		auto act = read_binary_file<uint16_t>((p / (*files)["activation"]->as_string()).string());
		auto wgt = read_binary_file<uint16_t>((p / (*files)["weight"]->as_string()).string());

		std::vector<uint16_t> psum;
		if ((*files)["partial_sum"]) {
		    psum = read_binary_file<uint16_t>((p / (*files)["partial_sum"]->as_string()).string());
		}

		std::vector<uint16_t> gold;
		if ((*files)["output"]) {
			gold = read_binary_file<uint16_t>((p / (*files)["output"]->as_string()).string());
		}
		if (gold.empty() && (*files)["output_gold"]) {
		    gold = read_binary_file<uint16_t>((p / (*files)["output_gold"]->as_string()).string());
		}

		auto pe_prog_path = p / (*files)["pe_program"]->as_string();
		std::vector<uint16_t> pe_prog;
		if (fs::exists(pe_prog_path)) {
			pe_prog = read_binary_file<uint16_t>(pe_prog_path.string());
		} else {
			std::cerr << "Warning: pe_program.bin not found at " << pe_prog_path << ". Using empty program." << std::endl;
			throw std::runtime_error("PE program binary not found");
			 // pe_prog remains empty
		}

		std::vector<uint32_t> scan_chain_data;
		// Try to find scan chain file if mentioned, or default.
		// Older code assumed scan_chain.bin.
		if (fs::exists(p / "scan_chain.bin")) {
			scan_chain_data = read_binary_file<uint32_t>((p / "scan_chain.bin").string());
		}

		// Base addresses (Software Convention)
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

		const uint32_t addr_act_run = cluster_json::get_spm_tensor_addr_or_default(software, "activation", addr_act, is_ultra);
		const uint32_t addr_wgt_run = cluster_json::get_spm_tensor_addr_or_default(software, "weight", addr_wgt, is_ultra);
		const uint32_t addr_psum_run = cluster_json::get_spm_tensor_addr_or_default(software, "partial_sum", addr_psum, is_ultra);
		const uint32_t addr_out_run = cluster_json::get_spm_tensor_addr_or_default(software, "output", addr_out, is_ultra);
		const auto spm_section_addr_map = cluster_json::parse_spm_sections(software);
		const auto dma_waves = cluster_json::parse_dma_waves(software, spm_section_addr_map);
		const auto plans = cluster_json::parse_cluster_plans(software);

		// Print loaded binary info and addresses for debugging
		std::cout << "  > Binaries loaded:" << std::endl;
		std::cout << "    Activation: " << act.size() << " elements, mapped to 0x" << std::hex << addr_act << std::dec << std::endl;
		std::cout << "    Weight: " << wgt.size() << " elements, mapped to 0x" << std::hex << addr_wgt << std::dec << std::endl;
		std::cout << "    Runtime Activation(SPM): 0x" << std::hex << addr_act_run << std::dec << std::endl;
		std::cout << "    Runtime Weight(SPM): 0x" << std::hex << addr_wgt_run << std::dec << std::endl;
		if (!psum.empty()) {
		    std::cout << "    Partial Sum: " << psum.size() << " elements, mapped to 0x" << std::hex << addr_psum << std::dec << std::endl;
			std::cout << "    Runtime Partial Sum(SPM): 0x" << std::hex << addr_psum_run << std::dec << std::endl;
		}
		if (!gold.empty()) {
		    std::cout << "    Output Gold: " << gold.size() << " elements" << std::endl;
		}
		std::cout << "    Runtime Output(SPM): 0x" << std::hex << addr_out_run << std::dec << std::endl;
		std::cout << "    PE Program: " << pe_prog.size() << " instructions" << std::endl;
		std::cout << "    Scan Chain: " << scan_chain_data.size() << " words" << std::endl;
		std::cout << "    SPM Sections: " << spm_section_addr_map.size() << std::endl;
		for (const auto& [name, addr] : spm_section_addr_map) {
			std::cout << "      Section: " << name << ", Linear Addr: 0x" << std::hex << addr.linear_addr << ", Parallel Addr: 0x" << std::hex << addr.parallel_addr << std::dec << std::endl;
		}
		std::cout << "    DMA Waves: " << dma_waves.size() << std::endl;
		for (const auto& w : dma_waves) {
			std::cout << "      Wave ID: " << w.wave_id << ", Compute Plan Sync: " << w.compute_plan_idx << ", Transfers: " << w.transfers.size() << std::endl;
			for (const auto& t : w.transfers) {
				const bool d2s = t.direction == DmaTransferCfg::Direction::DramToSpm;
				std::cout << "        Tensor: " << t.tensor << ", Group: " << t.group_id << ", Section: " << t.section
						<< ", Direction: " << (d2s ? "dram_to_spm" : "spm_to_dram")
						<< ", Src: 0x" << std::hex << (d2s ? t.src_dram_addr : t.src_spm_addr)
						<< ", Dst: 0x" << (d2s ? t.dst_spm_addr : t.dst_dram_addr)
						<< ", Dst Parallel SPM: 0x" << t.dst_parallel_spm_addr
						<< ", Size (64-bit words): " << std::dec << t.size_words64 << std::endl;
			}
		}
		std::cout << "    Cluster Plans: " << plans.size() << std::endl;
		std::cout << "   Ultra Mode: " << (is_ultra ? "Enabled" : "Disabled") << std::endl;
		// ---------------------------------------
		// step 2: power on, preload data, program cluster
		// ---------------------------------------
		driver_.power_on_and_reset();
		driver_.config_spm_map(0xE4); // {0b11, 0b10, 0b01, 0b00}

		auto pack_to_64 = [](const std::vector<uint16_t>& in) {
			std::vector<uint64_t> out;
			out.reserve((in.size() + 3) / 4);
			for (size_t i = 0; i < in.size(); i += 4) {
				uint64_t v = 0;
				v |= (uint64_t)in[i];
				if (i+1 < in.size()) v |= ((uint64_t)in[i+1] << 16);
				if (i+2 < in.size()) v |= ((uint64_t)in[i+2] << 32);
				if (i+3 < in.size()) v |= ((uint64_t)in[i+3] << 48);
				out.push_back(v);
			}
			return out;
		};

		// Remove the old double preload calls logic
		std::cout << "[runner] Preload activation into DRAM shadow" << std::endl;
		preload_dram_shadow(addr_act, pack_to_64(act));

		std::cout << "[runner] Preload weight into DRAM shadow" << std::endl;
		preload_dram_shadow(addr_wgt, pack_to_64(wgt));

		if (!psum.empty()) {
			std::cout << "[runner] Preload partial sum into DRAM shadow" << std::endl;
			preload_dram_shadow(addr_psum, pack_to_64(psum));
		}

		// Scan Chain & Program
		if (!scan_chain_data.empty()) {
			std::cout << "[runner] Loading scan chain data into cluster" << std::endl;
    		driver_.send_scan_chain_words_reverse(scan_chain_data);
			backend_.wait_cycles(5);
			backend_.dut.noc.dump_state();
		}

		std::cout << "[runner] Loading PE program into cluster" << std::endl;
		driver_.load_pe_program(pe_prog);

		// ---------------------------------------
		// step 3: execute precomputed cluster plans
		// ---------------------------------------
		if (plans.empty()) {
			std::cerr << "No software.cluster_plans found in config.json" << std::endl;
			return false;
		}

		std::cout << "[runner] Generated " << plans.size() << " plans." << std::endl;

		bool all_ok = true;
		for (size_t i = 0; i < plans.size(); ++i) {
			auto plan = plans[i];
			std::cout << "[runner] Executing plan " << i << ": " << plan.name << std::endl;

			uint8_t spm_map = 0xE4;
			if (i < dma_waves.size() && dma_waves[i].has_spm_map) {
				spm_map = dma_waves[i].spm_map_val;
			}

			// decode spm_map for debugging
			std::cout << "  > Configuring SPM map: 0x" << std::hex << (int)spm_map << std::dec << " (";
			for (int p = 0; p < 4; ++p) {
				int group_map = (spm_map >> (p * 2)) & 0x3;
				std::cout << "P" << p << "->G" << group_map << " ";
			}
			std::cout << ")" << std::endl;

			driver_.config_spm_map(spm_map);

			if (i < dma_waves.size()) {
				std::cout << "[runner] Plan " << i << " has DMA wave sync with wave ID " << dma_waves[i].wave_id << std::endl;
				if (!run_dma_wave(dma_waves[i], DmaTransferCfg::Direction::DramToSpm)) {
					std::cerr << "[runner] DMA wave " << dma_waves[i].wave_id << " failed, aborting plan execution." << std::endl;
					all_ok = false;
					break;
				}
			}

			std::cout << "  > Configuring cluster for plan " << i << std::endl;
			std::cout << "    > AGU Configurations:" << std::endl;
			std::cout << "      PS: " << plan.agu_ps << std::endl;
			std::cout << "      PD: " << plan.agu_pd << std::endl;
			std::cout << "      PLI: " << plan.agu_pli << std::endl;
			std::cout << "      PLO: " << plan.agu_plo << std::endl;

			driver_.cfg_agu(AGU_PS, plan.agu_ps);
			driver_.cfg_agu(AGU_PD, plan.agu_pd);
			driver_.cfg_agu(AGU_PLI, plan.agu_pli);
			driver_.cfg_agu(AGU_PLO, plan.agu_plo);

			// Global HDDU?
			// The compiler doesn't output global HDDU config yet?
			// Use default suitable for tests
			driver_.cfg_hddu_global(plan.global_mask, is_ultra ? 0x2 : 0x1);

			driver_.start_all();
			bool done = driver_.wait_hddu_done(WAVE_TIMEOUT_CYCLES, POLL_INTERVAL_CYCLES);
			if (!done) {
				std::cerr << "[runner] Plan " << i << " TIMEOUT" << std::endl;
				all_ok = false;
				break;
			}
			driver_.stop_all();

			if (i < dma_waves.size()) {
				if (!run_dma_wave(dma_waves[i], DmaTransferCfg::Direction::SpmToDram)) {
					std::cerr << "[runner] DMA wave " << dma_waves[i].wave_id << " writeback failed." << std::endl;
					all_ok = false;
					break;
				}
			}
		}

		// ---------------------------------------
		// step 4: read back results and verify
		// ---------------------------------------
		std::cout << "[runner] Reading back results..." << std::endl;
		size_t out_words = gold.size() / 4;
		if (gold.size() % 4 != 0) out_words++;

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
			driver_.dma_read_words64(out_addrs, res_vec);
		}
		std::vector<uint16_t> res_fp16;
		for(uint64_t w : res_vec) {
			res_fp16.push_back(w & 0xFFFF);
			res_fp16.push_back((w >> 16) & 0xFFFF);
			res_fp16.push_back((w >> 32) & 0xFFFF);
			res_fp16.push_back((w >> 48) & 0xFFFF);
		}
		// trim to gold size
		if (res_fp16.size() > gold.size()) res_fp16.resize(gold.size());

		auto stats = verify_fp16_vectors(gold, res_fp16, 2e-2f, true);
		std::cout << "[runner] Verify results: " << std::endl << stats << std::endl;

		return (stats.cosine_similarity >= 0.99f) && all_ok;
	}

	ClusterSimDriver& driver_;
	ComputeClusterTbBackend& backend_;
	std::unordered_map<uint32_t, uint64_t> dram_shadow_;
};

class ClusterSimTestBench : public sc_core::sc_module {
public:
	SC_HAS_PROCESS(ClusterSimTestBench);

	ClusterSimTestBench(
		sc_core::sc_module_name name,
		const SimRunnerOptions& options,
		ComputeClusterTbBackend& backend)
		: sc_core::sc_module(name),
		  options_(options),
		  backend_(backend) {
		SC_THREAD(control_process);
	}

	bool verify() const {
		if (!run_finished_) {
			std::cerr << "[test_cluster_sim] simulation ended before testbench finished" << std::endl;
			return false;
		}
		return pass_;
	}

private:
	void control_process() {
		DriverHooks hooks;
		if (options_.verbose) {
			hooks.mmio_write = [&](uint32_t a, uint32_t d) {
				std::cout << "[hook] mmio_write addr=0x" << std::hex << a << " data=0x" << d << std::dec << std::endl;
				backend_.mmio_write(a, d);
			};
			hooks.mmio_read = [&](uint32_t a) -> uint32_t {
				const uint32_t v = backend_.mmio_read(a);
				std::cout << "[hook] mmio_read addr=0x" << std::hex << a << " -> 0x" << v << std::dec << std::endl;
				return v;
			};
			hooks.data_write64 = [&](uint32_t a, uint64_t d) {
				std::cout << "[hook] data_write64 addr=0x" << std::hex << a << " data=0x" << d << std::dec << std::endl;
				backend_.data_write64(a, d);
			};
			hooks.data_write64_burst = [&](const std::vector<uint32_t>& addrs, const std::vector<uint64_t>& words) {
				const uint32_t first = addrs.empty() ? 0 : addrs.front();
				std::cout << "[hook] data_write64_burst first_addr=0x" << std::hex << first << std::dec
						  << " words=" << words.size() << std::endl;
				backend_.data_write64_burst(addrs, words);
			};
			hooks.data_read64 = [&](uint32_t a) -> uint64_t {
				const uint64_t v = backend_.data_read64(a);
				std::cout << "[hook] data_read64 addr=0x" << std::hex << a << " -> 0x" << v << std::dec << std::endl;
				return v;
			};
			hooks.data_read64_burst = [&](const std::vector<uint32_t>& addrs, std::vector<uint64_t>& out) {
				const uint32_t first = addrs.empty() ? 0 : addrs.front();
				backend_.data_read64_burst(addrs, out);
				std::cout << "[hook] data_read64_burst first_addr=0x" << std::hex << first << std::dec
						  << " words=" << out.size() << std::endl;
			};
			hooks.set_power_enable = [&](bool on) {
				std::cout << "[hook] set_power_enable on=" << (on ? 1 : 0) << std::endl;
				backend_.set_power_enable(on);
			};
			hooks.set_reset_n = [&](bool rst_n) {
				std::cout << "[hook] set_reset_n rst_n=" << (rst_n ? 1 : 0) << std::endl;
				backend_.set_reset_n(rst_n);
			};
			hooks.wait_cycles = [&](uint32_t n) {
				std::cout << "[hook] wait_cycles n=" << n << std::endl;
				backend_.wait_cycles(n);
			};
			hooks.wait_interrupt = [&](uint32_t timeout) -> bool {
				std::cout << "[hook] wait_interrupt timeout_cycles=" << timeout << std::endl;
				const bool hit = backend_.wait_interrupt_asserted(timeout);
				std::cout << "[hook] wait_interrupt result=" << (hit ? 1 : 0) << std::endl;
				return hit;
			};
		} else {
			hooks.mmio_write = [&](uint32_t a, uint32_t d) { backend_.mmio_write(a, d); };
			hooks.mmio_read = [&](uint32_t a) -> uint32_t { return backend_.mmio_read(a); };
			hooks.data_write64 = [&](uint32_t a, uint64_t d) { backend_.data_write64(a, d); };
			hooks.data_write64_burst = [&](const std::vector<uint32_t>& addrs, const std::vector<uint64_t>& words) { backend_.data_write64_burst(addrs, words); };
			hooks.data_read64 = [&](uint32_t a) -> uint64_t { return backend_.data_read64(a); };
			hooks.data_read64_burst = [&](const std::vector<uint32_t>& addrs, std::vector<uint64_t>& out) { backend_.data_read64_burst(addrs, out); };
			hooks.set_power_enable = [&](bool on) { backend_.set_power_enable(on); };
			hooks.set_reset_n = [&](bool rst_n) { backend_.set_reset_n(rst_n); };
			hooks.wait_cycles = [&](uint32_t n) { backend_.wait_cycles(n); };
			hooks.wait_interrupt = [&](uint32_t timeout) -> bool { return backend_.wait_interrupt_asserted(timeout); };
		}

		ClusterSimDriver driver(std::move(hooks));
		ScenarioRunner runner(driver, backend_);
		if (!options_.dry_run) {
			std::cout << "[test_cluster_sim] non-dry-run currently uses ComputeCluster TB backend" << std::endl;
		}

		pass_ = runner.run(options_);
		run_finished_ = true;
		sc_core::sc_stop();
	}

	SimRunnerOptions options_;
	ComputeClusterTbBackend& backend_;
	bool pass_ = false;
	bool run_finished_ = false;
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
	sc_core::sc_signal<bool> cmd_req_valid_i;
	sc_core::sc_signal<bool> cmd_req_write_i;
	sc_core::sc_signal<sc_dt::sc_uint<32>> cmd_req_addr_i;
	sc_core::sc_signal<sc_dt::sc_uint<32>> cmd_req_wdata_i;
	sc_core::sc_signal<sc_dt::sc_uint<4>> cmd_req_wstrb_i;
	sc_core::sc_signal<bool> cmd_req_ready_o;
	sc_core::sc_signal<bool> cmd_resp_valid_o;
	sc_core::sc_signal<sc_dt::sc_uint<32>> cmd_resp_rdata_o;
	sc_core::sc_signal<bool> cmd_resp_err_o;

	ComputeClusterTbBackend::DutType dut("Cluster", hybridacc::NetWorkOnChipConfig(4, 4));
	dut.clk(clk);
	dut.reset_n(reset_n);
	dut.power_enable_i(power_enable_i);
	dut.interrupt_o(interrupt_o);
	dut.cmd_req_valid_i(cmd_req_valid_i);
	dut.cmd_req_write_i(cmd_req_write_i);
	dut.cmd_req_addr_i(cmd_req_addr_i);
	dut.cmd_req_wdata_i(cmd_req_wdata_i);
	dut.cmd_req_wstrb_i(cmd_req_wstrb_i);
	dut.cmd_req_ready_o(cmd_req_ready_o);
	dut.cmd_resp_valid_o(cmd_resp_valid_o);
	dut.cmd_resp_rdata_o(cmd_resp_rdata_o);
	dut.cmd_resp_err_o(cmd_resp_err_o);

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

	ComputeClusterTbBackend backend("cluster_backend", dut, options.clock_period_ns, options.timeout_cycles);
	backend.clk(clk);
	backend.reset_n(reset_n);
	backend.power_enable_i(power_enable_i);
	backend.interrupt_o(interrupt_o);
	backend.s_axi_awvalid_i(s_axi_awvalid_i);
	backend.s_axi_awready_o(s_axi_awready_o);
	backend.s_axi_awaddr_i(s_axi_awaddr_i);
	backend.s_axi_wvalid_i(s_axi_wvalid_i);
	backend.s_axi_wready_o(s_axi_wready_o);
	backend.s_axi_wdata_i(s_axi_wdata_i);
	backend.s_axi_wstrb_i(s_axi_wstrb_i);
	backend.s_axi_bvalid_o(s_axi_bvalid_o);
	backend.s_axi_bready_i(s_axi_bready_i);
	backend.s_axi_bresp_o(s_axi_bresp_o);
	backend.s_axi_arvalid_i(s_axi_arvalid_i);
	backend.s_axi_arready_o(s_axi_arready_o);
	backend.s_axi_araddr_i(s_axi_araddr_i);
	backend.s_axi_rvalid_o(s_axi_rvalid_o);
	backend.s_axi_rready_i(s_axi_rready_i);
	backend.s_axi_rdata_o(s_axi_rdata_o);
	backend.s_axi_rresp_o(s_axi_rresp_o);
	backend.hsel_i(hsel_i);
	backend.haddr_i(haddr_i);
	backend.hwrite_i(hwrite_i);
	backend.htrans_i(htrans_i);
	backend.hsize_i(hsize_i);
	backend.hburst_i(hburst_i);
	backend.hprot_i(hprot_i);
	backend.hready_i(hready_i);
	backend.hwdata_i(hwdata_i);
	backend.hready_o(hready_o);
	backend.hresp_o(hresp_o);
	backend.hrdata_o(hrdata_o);

	ClusterSimTestBench tb("cluster_tb", options, backend);
	if (options.enable_trace) {
		dut.enable_perffeto_trace();
		PerfettoTrace::getInstance().open(options.trace_file);
	}

	sc_core::sc_start();
	const bool pass = tb.verify();
	const std::string case_name =
		(options.scenario == ScenarioKind::Conv2D)
			? "conv2d"
			: (options.scenario == ScenarioKind::GEMM ? "gemm" : "both");

	std::cout << "[test_cluster_sim] runner case=" << case_name
			  << " dry_run=" << (options.dry_run ? "1" : "0")
			  << " sim_cycles=" << backend.cycle()
			  << " noc_cmd_count=" << backend.noc_cmd_log().size()
			  << std::endl;

	if (options.enable_trace) {
		PerfettoTrace::getInstance().close();
	}

	return pass ? 0 : 1;
}
