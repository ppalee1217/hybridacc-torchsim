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

struct AguCfg {
	uint32_t base_addr = 0;
	uint32_t base_addr_h = 0;
	uint16_t iter0 = 1;
	uint16_t iter1 = 1;
	uint16_t iter2 = 1;
	uint16_t iter3 = 1;
	int32_t stride0 = 0;
	int32_t stride1 = 0;
	int32_t stride2 = 0;
	int32_t stride3 = 0;
	uint32_t lane_cfg = 0;
	uint32_t tag_base = 0;
	uint32_t tag_stride0 = 0;
	uint32_t tag_stride1 = 0;
	uint32_t tag_ctrl = 0;
	uint32_t mask_cfg = 0xF;
	bool ultra = false;
	bool enable = false;
};

inline std::ostream& operator<<(std::ostream& os, const AguCfg& cfg) {
	os << "base_addr=0x" << std::hex << cfg.base_addr << std::dec
	   << " iter=[" << cfg.iter0 << "," << cfg.iter1 << "," << cfg.iter2 << "," << cfg.iter3 << "]"
	   << " stride=[" << cfg.stride0 << "," << cfg.stride1 << "," << cfg.stride2 << "," << cfg.stride3 << "]"
	   << " lane_cfg=0x" << std::hex << cfg.lane_cfg << std::dec
	   << " tag_base=0x" << std::hex << cfg.tag_base << std::dec
	   << " tag_stride=[" << cfg.tag_stride0 << "," << cfg.tag_stride1 << "]"
	   << " tag_ctrl=0x" << std::hex << cfg.tag_ctrl << std::dec
	   << " mask_cfg=0x" << std::hex << cfg.mask_cfg << std::dec
	   << " ultra=" << cfg.ultra;
	return os;
}

struct ClusterPlan {
	AguCfg agu_ps;
	AguCfg agu_pd;
	AguCfg agu_pli;
	AguCfg agu_plo;
	uint32_t global_mask = 0xF;
	bool ultra_mode = false;
	std::string name;
};

struct DriverHooks {
	std::function<void(uint32_t, uint32_t)> mmio_write;
	std::function<uint32_t(uint32_t)> mmio_read;
	std::function<void(uint32_t, uint64_t)> data_write64;
	std::function<void(uint32_t, const std::vector<uint64_t>&)> data_write64_burst;
	std::function<uint64_t(uint32_t)> data_read64;
	std::function<std::vector<uint64_t>(uint32_t, uint32_t)> data_read64_burst;
	std::function<void(bool)> set_power_enable;
	std::function<void(bool)> set_reset_n;
	std::function<void(uint32_t)> wait_cycles;
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
		if (!hooks_.mmio_write || !hooks_.mmio_read || !hooks_.data_write64 || !hooks_.data_read64 ||
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

	void preload_words64(uint32_t base_addr, const std::vector<uint64_t>& words) {
		constexpr uint32_t kWordBytes = 8;
		if (words.empty()) {
			return;
		}
		if (hooks_.data_write64_burst) {
			hooks_.data_write64_burst(base_addr, words);
			return;
		}
		for (size_t i = 0; i < words.size(); ++i) {
			hooks_.data_write64(base_addr + static_cast<uint32_t>(i) * kWordBytes, words[i]);
		}
	}

	std::vector<uint64_t> readback_words64(uint32_t base_addr, uint32_t word_count) const {
		constexpr uint32_t kWordBytes = 8;
		if (word_count == 0) {
			return {};
		}
		if (hooks_.data_read64_burst) {
			return hooks_.data_read64_burst(base_addr, word_count);
		}
		std::vector<uint64_t> out(word_count, 0);
		for (uint32_t i = 0; i < word_count; ++i) {
			out[i] = hooks_.data_read64(base_addr + i * kWordBytes);
		}
		return out;
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
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_START));
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
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_START));
	}

	void stop_all() {
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_STOP));
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

// Testbench backend that instantiates the ComputeCluster and provides hooks for the driver to interact with it
class ComputeClusterTbBackend {
public:
	struct DmaWaveSyncRequest {
		uint32_t wave_id = 0;
		uint32_t latency_cycles = 1;
	};

	int kClockPeriodNs = 10;
	int timeout_cycles = 200;

	sc_core::sc_clock clk;
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

	hybridacc::ComputeCluster<
		SPM_NUM_NOC_CHANNEL,
		SPM_NUM_BANKS_PER_GROUP,
		SPM_SRAM_BANK_WIDTH_BITS,
		SPM_SRAM_BANK_DEPTH_WORDS,
		SPM_SRAM_BANK_LATENCY,
		SPM_SRAM_BANK_PIPELINE_DEPTH,
		SPM_ADDR_WIDTH,
		NOC_NUM_PORTS,
		NOC_PORT_WIDTH_BITS,
		NOC_NUM_PES_PER_PORT> dut;

	ComputeClusterTbBackend(int clock_period_ns = 10, int timeout_cycles = 200)
		: kClockPeriodNs(clock_period_ns),
		  timeout_cycles(timeout_cycles),
		  clk("clk", sc_core::sc_time(clock_period_ns, sc_core::SC_NS)),
		  dut("Cluster", hybridacc::NetWorkOnChipConfig(4, 4)) {
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
		dma_wave_done_.write(false);

		sc_core::sc_spawn(sc_bind(&ComputeClusterTbBackend::dma_wave_process, this), "tb_dma_wave_process");
		tick(1);
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

	void data_write64_burst(uint32_t base_addr, const std::vector<uint64_t>& words) {
		if (!data_write_burst_tx(base_addr, words)) {
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

	std::vector<uint64_t> data_read64_burst(uint32_t base_addr, uint32_t word_count) {
		std::vector<uint64_t> out;
		out.resize(word_count, 0);
		if (!data_read_burst_tx(base_addr, out)) {
			throw std::runtime_error("ComputeCluster burst data read timeout");
		}
		return out;
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

private:
	static bool in_range(uint32_t addr, uint32_t base, uint32_t size) {
		return addr >= base && addr < (base + size);
	}

	void tick(uint32_t n = 1) {
		for (uint32_t i = 0; i < n; ++i) {
			sc_core::sc_start(kClockPeriodNs, sc_core::SC_NS);
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

	bool data_write_burst_tx(uint32_t base_addr, const std::vector<uint64_t>& words) {
		constexpr uint32_t kWordBytes = 8;
		if (words.empty()) {
			return true;
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
			static_cast<uint64_t>(std::max(1, timeout_cycles)) * static_cast<uint64_t>(words.size() + 2);
		for (uint64_t i = 0; i < max_cycles; ++i) {
			if (!aw_active && aw_sent < words.size()) {
				s_axi_awaddr_i.write(base_addr + static_cast<uint32_t>(aw_sent) * kWordBytes);
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
			}
			if (w_active && w_ready) {
				s_axi_wvalid_i.write(false);
				w_active = false;
				++w_sent;
			}

			if (b_valid && s_axi_bready_i.read()) {
				++bresp_done;
			}

			if (aw_sent == words.size() && w_sent == words.size() && !aw_active && !w_active && bresp_done == words.size()) {
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

	bool data_read_burst_tx(uint32_t base_addr, std::vector<uint64_t>& out_words) {
		constexpr uint32_t kWordBytes = 8;
		if (out_words.empty()) {
			return true;
		}

		size_t ar_sent = 0;
		size_t r_done = 0;
		bool ar_active = false;
		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(true);

		const uint64_t max_cycles =
			static_cast<uint64_t>(std::max(1, timeout_cycles)) * static_cast<uint64_t>(out_words.size() + 2);
		for (uint64_t i = 0; i < max_cycles; ++i) {
			if (!ar_active && ar_sent < out_words.size()) {
				s_axi_araddr_i.write(base_addr + static_cast<uint32_t>(ar_sent) * kWordBytes);
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

			if (ar_sent == out_words.size() && !ar_active && r_done == out_words.size()) {
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
	struct SpmSectionAddr {
		uint32_t linear_addr = 0;
		uint32_t parallel_addr = 0;
	};

	struct DmaAddrGen4D {
		bool enabled = false;
		uint32_t base_addr = 0;
		std::array<uint32_t, 4> iter = {1, 1, 1, 1};
		std::array<int32_t, 4> stride = {0, 0, 0, 8};

		friend std::ostream& operator<<(std::ostream& os, const DmaAddrGen4D& gen) {
			os << "AddrGen4D(enabled=" << gen.enabled
			   << ", base_addr=0x" << std::hex << gen.base_addr << std::dec
			   << ", iter=[" << gen.iter[0] << "," << gen.iter[1] << "," << gen.iter[2] << "," << gen.iter[3] << "]"
			   << ", stride=[" << gen.stride[0] << "," << gen.stride[1] << "," << gen.stride[2] << "," << gen.stride[3] << "]"
			   << ")";
			return os;
		}
	};

	struct DmaTransferCfg {
		enum class Direction {
			DramToSpm,
			SpmToDram,
		};

		std::string tensor;
		int group_id = -1;
		std::string section;
		Direction direction = Direction::DramToSpm;
		uint32_t src_dram_addr = 0;
		uint32_t dst_spm_addr = 0;
		uint32_t src_spm_addr = 0;
		uint32_t src_parallel_spm_addr = 0;
		uint32_t dst_dram_addr = 0;
		uint32_t dst_parallel_spm_addr = 0;
		uint32_t size_words64 = 0;
		DmaAddrGen4D src_addr_gen;
		DmaAddrGen4D dst_addr_gen;
	};

	struct DmaWaveCfg {
		uint32_t wave_id = 0;
		int compute_plan_idx = -1;
		std::vector<DmaTransferCfg> transfers;
	};

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

	static std::string format_json(const std::shared_ptr<JsonValue>& v) {
		if (!v || v->is_null()) return "null";
		if (v->is_string()) return v->as_string();
		if (v->is_number()) return std::to_string(v->as_int());
		if (v->is_bool()) return v->as_bool() ? "true" : "false";
		if (v->is_array()) {
			std::stringstream ss;
			ss << "[";
			const auto& arr = v->as_array();
			for (size_t i = 0; i < arr.size(); ++i) {
				if (i > 0) ss << ", ";
				ss << format_json(arr[i]);
			}
			ss << "]";
			return ss.str();
		}
		if (v->is_object()) {
			std::stringstream ss;
			ss << "{";
			size_t count = 0;
			for (const auto& [key, val] : v->as_object()) {
				if (count++ > 0) ss << ", ";
				ss << key << ":" << format_json(val);
			}
			ss << "}";
			return ss.str();
		}
		return "?";
	}

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

	static bool parse_u32_array4(const std::shared_ptr<JsonValue>& node, std::array<uint32_t, 4>& out) {
		if (!node || !node->is_array()) {
			return false;
		}
		const auto& arr = node->as_array();
		if (arr.size() != 4) {
			return false;
		}
		for (size_t i = 0; i < 4; ++i) {
			if (!arr[i]) {
				return false;
			}
			out[i] = static_cast<uint32_t>(arr[i]->as_int64());
		}
		return true;
	}

	static bool parse_i32_array4(const std::shared_ptr<JsonValue>& node, std::array<int32_t, 4>& out) {
		if (!node || !node->is_array()) {
			return false;
		}
		const auto& arr = node->as_array();
		if (arr.size() != 4) {
			return false;
		}
		for (size_t i = 0; i < 4; ++i) {
			if (!arr[i]) {
				return false;
			}
			out[i] = static_cast<int32_t>(arr[i]->as_int64());
		}
		return true;
	}

	static bool parse_addr_gen4d(const std::shared_ptr<JsonValue>& node, DmaAddrGen4D& out) {
		if (!node || !node->is_object()) {
			return false;
		}
		auto iter_v = (*node)["iter"];
		auto stride_v = (*node)["stride"];
		if (!iter_v || !stride_v) {
			return false;
		}

		std::array<uint32_t, 4> iter = {1, 1, 1, 1};
		std::array<int32_t, 4> stride = {0, 0, 0, 8};
		if (!parse_u32_array4(iter_v, iter) || !parse_i32_array4(stride_v, stride)) {
			return false;
		}

		uint32_t base_addr = 0;
		auto base_v = (*node)["base_addr"];
		if (base_v) {
			base_addr = static_cast<uint32_t>(base_v->as_int64());
		}

		out.enabled = true;
		out.base_addr = base_addr;
		out.iter = iter;
		out.stride = stride;
		return true;
	}

	static uint64_t addr_gen_word_count(const DmaAddrGen4D& gen) {
		if (!gen.enabled) {
			return 0;
		}
		uint64_t count = 1;
		for (uint32_t v : gen.iter) {
			count *= static_cast<uint64_t>(v);
		}
		return count;
	}

	static std::vector<uint32_t> build_dma_addr_list(const DmaAddrGen4D& gen,
			uint32_t fallback_base,
			uint32_t word_count) {
		constexpr uint32_t kWordBytes = 8;
		std::vector<uint32_t> out;
		out.reserve(word_count);

		if (!gen.enabled) {
			for (uint32_t i = 0; i < word_count; ++i) {
				out.push_back(fallback_base + i * kWordBytes);
			}
			return out;
		}

		std::cout << "Generating addresses using " << gen << " for " << word_count << " words" << std::endl;

		for (uint32_t i0 = 0; i0 < gen.iter[0] && out.size() < word_count; ++i0) {
			for (uint32_t i1 = 0; i1 < gen.iter[1] && out.size() < word_count; ++i1) {
				for (uint32_t i2 = 0; i2 < gen.iter[2] && out.size() < word_count; ++i2) {
					for (uint32_t i3 = 0; i3 < gen.iter[3] && out.size() < word_count; ++i3) {
						const int64_t addr =
							static_cast<int64_t>(gen.base_addr) +
							static_cast<int64_t>(i0) * static_cast<int64_t>(gen.stride[0]) +
							static_cast<int64_t>(i1) * static_cast<int64_t>(gen.stride[1]) +
							static_cast<int64_t>(i2) * static_cast<int64_t>(gen.stride[2]) +
							static_cast<int64_t>(i3) * static_cast<int64_t>(gen.stride[3]);
						out.push_back(static_cast<uint32_t>(addr));
					}
				}
			}
		}

		if (out.size() < word_count) {
			for (uint32_t i = static_cast<uint32_t>(out.size()); i < word_count; ++i) {
				out.push_back(fallback_base + i * kWordBytes);
			}
		}
		return out;
	}

	static std::unordered_map<std::string, SpmSectionAddr> parse_spm_sections(const std::shared_ptr<JsonValue>& software) {
		std::unordered_map<std::string, SpmSectionAddr> out;
		auto spm = (*software)["spm"];
		if (!spm) return out;

		auto groups = (*spm)["groups"];
		if (groups && groups->is_array()) {
			for (const auto& g : groups->as_array()) {
				if (!g || !g->is_object()) continue;
				auto sections = (*g)["sections"];
				if (!sections || !sections->is_array()) continue;
				for (const auto& s : sections->as_array()) {
					if (!s || !s->is_object() || !(*s)["name"]) continue;
					const std::string name = (*s)["name"]->as_string();
					SpmSectionAddr addr;
					if ((*s)["global_linear_addr"]) addr.linear_addr = static_cast<uint32_t>((*s)["global_linear_addr"]->as_int64());
					if ((*s)["global_parallel_addr"]) addr.parallel_addr = static_cast<uint32_t>((*s)["global_parallel_addr"]->as_int64());
					out[name] = addr;
				}
			}
		}

		auto sections_legacy = (*spm)["sections"];
		if (out.empty() && sections_legacy && sections_legacy->is_array()) {
			for (const auto& s : sections_legacy->as_array()) {
				if (!s || !s->is_object() || !(*s)["name"]) continue;
				const std::string name = (*s)["name"]->as_string();
				SpmSectionAddr addr;
				if ((*s)["spm_addr"]) {
					addr.linear_addr = static_cast<uint32_t>((*s)["spm_addr"]->as_int64());
					addr.parallel_addr = addr.linear_addr;
				}
				out[name] = addr;
			}
		}

		return out;
	}

	static uint32_t get_spm_tensor_addr_or_default(const std::shared_ptr<JsonValue>& software,
			const std::string& tensor_name,
			uint32_t default_addr,
			bool prefer_parallel) {
		auto spm = (*software)["spm"];
		if (!spm) return default_addr;
		auto tensor_mapping = (*spm)["tensor_mapping"];
		if (!tensor_mapping) return default_addr;
		auto t = (*tensor_mapping)[tensor_name];
		if (!t) return default_addr;

		const auto mode_v = (*t)["spm_mode"];
		const std::string spm_mode = (mode_v && mode_v->is_string()) ? mode_v->as_string() : "";

		bool has_linear = false;
		bool has_parallel = false;
		uint32_t linear_addr = 0;
		uint32_t parallel_addr = 0;

		auto linear_v = (*t)["linear_spm_addr"];
		if (linear_v) {
			has_linear = true;
			linear_addr = static_cast<uint32_t>(linear_v->as_int64());
		}

		auto parallel_v = (*t)["parallel_spm_addr"];
		if (parallel_v) {
			has_parallel = true;
			parallel_addr = static_cast<uint32_t>(parallel_v->as_int64());
		}

		auto spm_addr_v = (*t)["spm_addr"];
		if (spm_addr_v) {
			const uint32_t spm_addr = static_cast<uint32_t>(spm_addr_v->as_int64());
			if (spm_mode == "parallel") {
				has_parallel = true;
				parallel_addr = spm_addr;
			} else {
				has_linear = true;
				linear_addr = spm_addr;
			}
		}

		if (prefer_parallel) {
			if (has_parallel) return parallel_addr;
			if (has_linear) return linear_addr;
		} else {
			if (has_linear) return linear_addr;
			if (has_parallel) return parallel_addr;
		}
		return default_addr;
	}

	static std::vector<DmaWaveCfg> parse_dma_waves(const std::shared_ptr<JsonValue>& software,
			const std::unordered_map<std::string, SpmSectionAddr>& section_addr_map) {
		std::vector<DmaWaveCfg> waves;
		auto dma = (*software)["dma"];
		if (!dma) {
			return waves;
		}
		auto wave_arr = (*dma)["waves"];
		if (!wave_arr || !wave_arr->is_array()) {
			return waves;
		}

		for (const auto& w : wave_arr->as_array()) {
			if (!w || !w->is_object()) continue;
			DmaWaveCfg wave;
			if ((*w)["wave_id"]) {
				wave.wave_id = static_cast<uint32_t>((*w)["wave_id"]->as_int());
			}
			auto sync = (*w)["sync"];
			if (sync && (*sync)["compute_plan_idx"]) {
				wave.compute_plan_idx = (*sync)["compute_plan_idx"]->as_int();
			}

			auto transfers = (*w)["transfers"];
			if (transfers && transfers->is_array()) {
				for (const auto& t : transfers->as_array()) {
					if (!t || !t->is_object()) continue;
					DmaTransferCfg cfg;
					bool has_src_dram_addr = false;
					bool has_dst_spm_addr = false;
					bool has_src_spm_addr = false;
					bool has_dst_dram_addr = false;
					if ((*t)["tensor"]) cfg.tensor = (*t)["tensor"]->as_string();
					if ((*t)["group_id"]) cfg.group_id = (*t)["group_id"]->as_int();
					if ((*t)["section"]) cfg.section = (*t)["section"]->as_string();
					if ((*t)["src_dram_addr"]) {
						cfg.src_dram_addr = static_cast<uint32_t>((*t)["src_dram_addr"]->as_int64());
						has_src_dram_addr = true;
					}
					if ((*t)["dst_dram_addr"]) {
						cfg.dst_dram_addr = static_cast<uint32_t>((*t)["dst_dram_addr"]->as_int64());
						has_dst_dram_addr = true;
					}
					if ((*t)["src_parallel_spm_addr"]) cfg.src_parallel_spm_addr = static_cast<uint32_t>((*t)["src_parallel_spm_addr"]->as_int64());
					if ((*t)["dst_parallel_spm_addr"]) cfg.dst_parallel_spm_addr = static_cast<uint32_t>((*t)["dst_parallel_spm_addr"]->as_int64());
					if ((*t)["src_spm_addr"]) {
						cfg.src_spm_addr = static_cast<uint32_t>((*t)["src_spm_addr"]->as_int64());
						has_src_spm_addr = true;
					}
					parse_addr_gen4d((*t)["src_addr_gen"], cfg.src_addr_gen);
					if ((*t)["dst_spm_addr"]) {
						cfg.dst_spm_addr = static_cast<uint32_t>((*t)["dst_spm_addr"]->as_int64());
						has_dst_spm_addr = true;
					} else if (!cfg.section.empty()) {
						auto it = section_addr_map.find(cfg.section);
						if (it != section_addr_map.end()) {
							cfg.dst_spm_addr = it->second.linear_addr;
							has_dst_spm_addr = true;
							if (cfg.dst_parallel_spm_addr == 0) cfg.dst_parallel_spm_addr = it->second.parallel_addr;
							if (cfg.src_spm_addr == 0) {
								cfg.src_spm_addr = it->second.linear_addr;
								has_src_spm_addr = true;
							}
							if (cfg.src_parallel_spm_addr == 0) cfg.src_parallel_spm_addr = it->second.parallel_addr;
						}
					}
					parse_addr_gen4d((*t)["dst_addr_gen"], cfg.dst_addr_gen);

					if ((*t)["direction"] && (*t)["direction"]->is_string()) {
						const std::string direction = (*t)["direction"]->as_string();
						if (direction == "spm_to_dram") {
							cfg.direction = DmaTransferCfg::Direction::SpmToDram;
						} else {
							cfg.direction = DmaTransferCfg::Direction::DramToSpm;
						}
					} else if (has_src_spm_addr && has_dst_dram_addr) {
						cfg.direction = DmaTransferCfg::Direction::SpmToDram;
					} else {
						cfg.direction = DmaTransferCfg::Direction::DramToSpm;
					}

					if ((*t)["size_words64"]) cfg.size_words64 = static_cast<uint32_t>((*t)["size_words64"]->as_int64());
					if (cfg.size_words64 == 0) {
						const uint64_t src_words = addr_gen_word_count(cfg.src_addr_gen);
						const uint64_t dst_words = addr_gen_word_count(cfg.dst_addr_gen);
						const uint64_t gen_words = src_words ? src_words : dst_words;
						if (gen_words > 0) {
							cfg.size_words64 = static_cast<uint32_t>(gen_words);
						}
					}
					const bool valid =
						(cfg.direction == DmaTransferCfg::Direction::DramToSpm)
							? ((has_src_dram_addr || cfg.src_addr_gen.enabled) && (has_dst_spm_addr || cfg.dst_addr_gen.enabled))
							: ((has_src_spm_addr || cfg.src_addr_gen.enabled) && (has_dst_dram_addr || cfg.dst_addr_gen.enabled));
					if (cfg.size_words64 > 0 && valid) {
						wave.transfers.push_back(cfg);
					}
				}
			}
			waves.push_back(std::move(wave));
		}
		return waves;
	}

	static bool json_to_bool(const std::shared_ptr<JsonValue>& node, bool default_value = false) {
		if (!node) {
			return default_value;
		}
		if (node->is_bool()) {
			return node->as_bool();
		}
		return node->as_int64() != 0;
	}

	static bool parse_agu_cfg(const std::shared_ptr<JsonValue>& node, AguCfg& out) {
		if (!node || !node->is_object()) {
			return false;
		}
		auto read_u32 = [&](const std::string& key, uint32_t& dst) {
			auto v = (*node)[key];
			if (v) dst = static_cast<uint32_t>(v->as_int64());
		};
		auto read_u16 = [&](const std::string& key, uint16_t& dst) {
			auto v = (*node)[key];
			if (v) dst = static_cast<uint16_t>(v->as_int64());
		};
		auto read_i32 = [&](const std::string& key, int32_t& dst) {
			auto v = (*node)[key];
			if (v) dst = static_cast<int32_t>(v->as_int64());
		};

		read_u32("base_addr", out.base_addr);
		read_u32("base_addr_h", out.base_addr_h);
		read_u16("iter0", out.iter0);
		read_u16("iter1", out.iter1);
		read_u16("iter2", out.iter2);
		read_u16("iter3", out.iter3);
		read_i32("stride0", out.stride0);
		read_i32("stride1", out.stride1);
		read_i32("stride2", out.stride2);
		read_i32("stride3", out.stride3);
		read_u32("lane_cfg", out.lane_cfg);
		read_u32("tag_base", out.tag_base);
		read_u32("tag_stride0", out.tag_stride0);
		read_u32("tag_stride1", out.tag_stride1);
		read_u32("tag_ctrl", out.tag_ctrl);
		read_u32("mask_cfg", out.mask_cfg);
		out.ultra = json_to_bool((*node)["ultra"], out.ultra);
		out.enable = json_to_bool((*node)["enable"], out.enable);
		return true;
	}

	static std::vector<ClusterPlan> parse_cluster_plans(const std::shared_ptr<JsonValue>& software) {
		std::vector<ClusterPlan> plans;
		auto plan_arr = (*software)["cluster_plans"];
		if (!plan_arr || !plan_arr->is_array()) {
			return plans;
		}

		for (const auto& p : plan_arr->as_array()) {
			if (!p || !p->is_object()) {
				continue;
			}
			ClusterPlan plan;
			auto name = (*p)["name"];
			if (name && name->is_string()) {
				plan.name = name->as_string();
			}
			auto global_mask = (*p)["global_mask"];
			if (global_mask) {
				plan.global_mask = static_cast<uint32_t>(global_mask->as_int64());
			}
			plan.ultra_mode = json_to_bool((*p)["ultra_mode"], false);

			parse_agu_cfg((*p)["agu_ps"], plan.agu_ps);
			parse_agu_cfg((*p)["agu_pd"], plan.agu_pd);
			parse_agu_cfg((*p)["agu_pli"], plan.agu_pli);
			parse_agu_cfg((*p)["agu_plo"], plan.agu_plo);

			plans.push_back(std::move(plan));
		}
		return plans;
	}

	bool run_dma_wave(const DmaWaveCfg& wave, DmaTransferCfg::Direction stage_direction) {
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
					  << " words=" << t.size_words64
					  << (t.src_addr_gen.enabled ? " src4d=1" : "")
					  << (t.dst_addr_gen.enabled ? " dst4d=1" : "")
					  << std::endl;

			auto src_addrs = build_dma_addr_list(
				t.src_addr_gen,
				t.direction == DmaTransferCfg::Direction::DramToSpm ? t.src_dram_addr : t.src_spm_addr,
				t.size_words64);
			auto dst_addrs = build_dma_addr_list(
				t.dst_addr_gen,
				t.direction == DmaTransferCfg::Direction::DramToSpm ? t.dst_spm_addr : t.dst_dram_addr,
				t.size_words64);

			if (t.direction == DmaTransferCfg::Direction::DramToSpm) {
				std::vector<uint64_t> payload;
				payload.reserve(t.size_words64);
				for (uint32_t i = 0; i < t.size_words64; ++i) {
					auto it = dram_shadow_.find(src_addrs[i]);
					payload.push_back(it == dram_shadow_.end() ? 0ULL : it->second);
				}
				if (!t.dst_addr_gen.enabled) {
					driver_.preload_words64(t.dst_spm_addr, payload);
				} else {
					for (uint32_t i = 0; i < t.size_words64; ++i) {
						driver_.write_word64(dst_addrs[i], payload[i]);
					}
				}
			} else {
				std::vector<uint64_t> payload;
				payload.reserve(t.size_words64);
				if (!t.src_addr_gen.enabled) {
					payload = driver_.readback_words64(t.src_spm_addr, t.size_words64);
				} else {
					for (uint32_t i = 0; i < t.size_words64; ++i) {
						payload.push_back(driver_.read_word64(src_addrs[i]));
					}
				}
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
			std::cout << "      " << k << ": " << format_json(v) << std::endl;
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

		const uint32_t addr_act_run = get_spm_tensor_addr_or_default(software, "activation", addr_act, is_ultra);
		const uint32_t addr_wgt_run = get_spm_tensor_addr_or_default(software, "weight", addr_wgt, is_ultra);
		const uint32_t addr_psum_run = get_spm_tensor_addr_or_default(software, "partial_sum", addr_psum, is_ultra);
		const uint32_t addr_out_run = get_spm_tensor_addr_or_default(software, "output", addr_out, is_ultra);
		const auto spm_section_addr_map = parse_spm_sections(software);
		const auto dma_waves = parse_dma_waves(software, spm_section_addr_map);
		const auto plans = parse_cluster_plans(software);

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
			const auto& plan = plans[i];
			std::cout << "[runner] Executing plan " << i << ": " << plan.name << std::endl;
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

			if (plan.agu_ps.enable) driver_.cfg_agu(AGU_PS, plan.agu_ps);
			if (plan.agu_pd.enable) driver_.cfg_agu(AGU_PD, plan.agu_pd);
			if (plan.agu_pli.enable) driver_.cfg_agu(AGU_PLI, plan.agu_pli);
			if (plan.agu_plo.enable) driver_.cfg_agu(AGU_PLO, plan.agu_plo);

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

		auto res_vec = has_output_dma_writeback
			? readback_dram_shadow(addr_out, static_cast<uint32_t>(out_words))
			: driver_.readback_words64(addr_out_run, static_cast<uint32_t>(out_words));
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

		return (stats.mismatches == 0) && all_ok;
	}

	ClusterSimDriver& driver_;
	ComputeClusterTbBackend& backend_;
	std::unordered_map<uint32_t, uint64_t> dram_shadow_;
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

	ComputeClusterTbBackend backend(options.clock_period_ns);
	if (options.enable_trace) {
		backend.dut.enable_perffeto_trace();
		PerfettoTrace::getInstance().open(options.trace_file);
	}
	DriverHooks hooks;

	if (options.verbose) {
		hooks.mmio_write = [&](uint32_t a, uint32_t d) {
			std::cout << "[hook] mmio_write addr=0x" << std::hex << a << " data=0x" << d << std::dec << std::endl;
			backend.mmio_write(a, d);
		};
		hooks.mmio_read = [&](uint32_t a) -> uint32_t {
			const uint32_t v = backend.mmio_read(a);
			std::cout << "[hook] mmio_read addr=0x" << std::hex << a << " -> 0x" << v << std::dec << std::endl;
			return v;
		};
		hooks.data_write64 = [&](uint32_t a, uint64_t d) {
			std::cout << "[hook] data_write64 addr=0x" << std::hex << a << " data=0x" << d << std::dec << std::endl;
			backend.data_write64(a, d);
		};
		hooks.data_write64_burst = [&](uint32_t a, const std::vector<uint64_t>& words) {
			std::cout << "[hook] data_write64_burst base=0x" << std::hex << a << std::dec
					  << " words=" << words.size() << std::endl;
			backend.data_write64_burst(a, words);
		};
		hooks.data_read64 = [&](uint32_t a) -> uint64_t {
			const uint64_t v = backend.data_read64(a);
			std::cout << "[hook] data_read64 addr=0x" << std::hex << a << " -> 0x" << v << std::dec << std::endl;
			return v;
		};
		hooks.data_read64_burst = [&](uint32_t a, uint32_t word_count) -> std::vector<uint64_t> {
			auto out = backend.data_read64_burst(a, word_count);
			std::cout << "[hook] data_read64_burst base=0x" << std::hex << a << std::dec
					  << " words=" << word_count << std::endl;
			return out;
		};
		hooks.set_power_enable = [&](bool on) {
			std::cout << "[hook] set_power_enable on=" << (on ? 1 : 0) << std::endl;
			backend.set_power_enable(on);
		};
		hooks.set_reset_n = [&](bool rst_n) {
			std::cout << "[hook] set_reset_n rst_n=" << (rst_n ? 1 : 0) << std::endl;
			backend.set_reset_n(rst_n);
		};
		hooks.wait_cycles = [&](uint32_t n) {
			std::cout << "[hook] wait_cycles n=" << n << std::endl;
			backend.wait_cycles(n);
		};
	} else {
		hooks.mmio_write = [&](uint32_t a, uint32_t d) { backend.mmio_write(a, d);};
		hooks.mmio_read = [&](uint32_t a) -> uint32_t { return backend.mmio_read(a);};
		hooks.data_write64 = [&](uint32_t a, uint64_t d) { backend.data_write64(a, d); };
		hooks.data_write64_burst = [&](uint32_t a, const std::vector<uint64_t>& words) { backend.data_write64_burst(a, words); };
		hooks.data_read64 = [&](uint32_t a) -> uint64_t { return backend.data_read64(a); };
		hooks.data_read64_burst = [&](uint32_t a, uint32_t word_count) -> std::vector<uint64_t> { return backend.data_read64_burst(a, word_count); };
		hooks.set_power_enable = [&](bool on) { backend.set_power_enable(on); };
		hooks.set_reset_n = [&](bool rst_n) { backend.set_reset_n(rst_n); };
		hooks.wait_cycles = [&](uint32_t n) { backend.wait_cycles(n); };
	}

	ClusterSimDriver driver(std::move(hooks));
	ScenarioRunner runner(driver, backend);

	if (!options.dry_run) {
		std::cout << "[test_cluster_sim] non-dry-run currently uses ComputeCluster TB backend" << std::endl;
	}

	const bool pass = runner.run(options);
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
