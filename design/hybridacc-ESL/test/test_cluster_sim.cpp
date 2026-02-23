#include <systemc>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cluster_sim {

enum message_command_t : uint32_t {
	CMD_RESET = 0,
	CMD_INIT = 1,
	CMD_LOAD_PROGRAM = 2,
	CMD_STOP_PE = 3,
	CMD_START_PE = 4,
	CMD_NOC_SCAN_CHAIN = 8,
};

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
static constexpr uint32_t HDDU_MAX_OUTSTANDING = 0x818;
static constexpr uint32_t HDDU_COUNTER_TX_PKT = 0x82C;
static constexpr uint32_t HDDU_COUNTER_TX_BYTE = 0x830;
static constexpr uint32_t HDDU_COUNTER_RX_BYTE = 0x834;
static constexpr uint32_t HDDU_COUNTER_STALL = 0x838;

static constexpr uint32_t PE_ROUTER_IM_ADDR_OFFSET = 4;
static constexpr uint32_t PE_ROUTER_IM_DATA_OFFSET = 16;
static constexpr uint32_t PE_ROUTER_IM_ADDR_MASK = 0xFFF;
static constexpr uint32_t PE_ROUTER_IM_DATA_MASK = 0xFFFF;

struct AguCfg {
	uint32_t base_addr = 0;
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
};

struct HdduCounters {
	uint32_t tx_pkt = 0;
	uint32_t tx_byte = 0;
	uint32_t rx_byte = 0;
	uint32_t stall = 0;
};

struct DriverHooks {
	std::function<void(uint32_t, uint32_t)> mmio_write;
	std::function<uint32_t(uint32_t)> mmio_read;
	std::function<void(uint32_t, uint64_t)> data_write64;
	std::function<uint64_t(uint32_t)> data_read64;
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
		for (size_t i = 0; i < words.size(); ++i) {
			hooks_.data_write64(base_addr + static_cast<uint32_t>(i), words[i]);
		}
	}

	std::vector<uint64_t> readback_words64(uint32_t base_addr, uint32_t word_count) const {
		std::vector<uint64_t> out(word_count, 0);
		for (uint32_t i = 0; i < word_count; ++i) {
			out[i] = hooks_.data_read64(base_addr + i);
		}
		return out;
	}

	void cfg_agu(uint32_t bank, const AguCfg& c) {
		const uint32_t b = CLUSTER_HDDU_BASE + bank * AGU_BANK_STRIDE;
		hooks_.mmio_write(b + REG_BASE_ADDR, c.base_addr);
		hooks_.mmio_write(b + REG_BASE_ADDR_H, 0);
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
		hooks_.mmio_write(b + REG_CTRL, c.ultra ? (1u << 3) : 0u);
	}

	void cfg_hddu_global(uint32_t plane_en, uint32_t plane_mode, uint32_t max_outstanding) {
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << 1));
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN, plane_en);
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE, plane_mode);
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_MAX_OUTSTANDING, max_outstanding);
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
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << 2));
	}

	void stop_all() {
		hooks_.mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << 3));
		noc_cmd_write(pack_noc_cmd(CMD_STOP_PE, 0));
	}

	bool wait_hddu_done(uint32_t timeout_cycles, uint32_t poll_step = 1) {
		for (uint32_t waited = 0; waited < timeout_cycles; waited += poll_step) {
			const uint32_t st = hooks_.mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
			if (st & (1u << 2)) {
				return false;
			}
			if (st & (1u << 1)) {
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

struct RunnerOptions {
	ScenarioKind scenario = ScenarioKind::Both;
	bool dry_run = true;
};

class MockBackend {
public:
	void mmio_write(uint32_t addr, uint32_t data) {
		if (!power_enable_) {
			return;
		}
		if (addr == CLUSTER_NOC_CMD) {
			noc_cmd_log_.push_back(data);
		}

		if (addr == CLUSTER_HDDU_BASE + HDDU_CTRL) {
			if (data & (1u << 1)) {
				status_reg_ = 0;
				err_reg_ = false;
			}
			if (data & (1u << 2)) {
				running_ = true;
				done_cycle_ = cycle_ + 20;
				status_reg_ = 0;
			}
			if (data & (1u << 3)) {
				running_ = false;
				status_reg_ = 0;
			}
		}

		if (addr == CLUSTER_SPM_CFG_UPDATE) {
			mmio_regs_[addr] = data & 0x1u;
			return;
		}

		mmio_regs_[addr] = data;
	}

	uint32_t mmio_read(uint32_t addr) {
		if (!power_enable_) {
			return 0;
		}

		refresh_status();

		if (addr == CLUSTER_HDDU_BASE + HDDU_STATUS) {
			return status_reg_;
		}
		if (addr == CLUSTER_HDDU_BASE + HDDU_COUNTER_TX_PKT) {
			return counter_tx_pkt_;
		}
		if (addr == CLUSTER_HDDU_BASE + HDDU_COUNTER_TX_BYTE) {
			return counter_tx_byte_;
		}
		if (addr == CLUSTER_HDDU_BASE + HDDU_COUNTER_RX_BYTE) {
			return counter_rx_byte_;
		}
		if (addr == CLUSTER_HDDU_BASE + HDDU_COUNTER_STALL) {
			return counter_stall_;
		}

		auto it = mmio_regs_.find(addr);
		if (it == mmio_regs_.end()) {
			return 0;
		}
		return it->second;
	}

	void data_write64(uint32_t addr, uint64_t data) {
		if (!power_enable_) {
			return;
		}
		data_mem_[addr] = data;
	}

	uint64_t data_read64(uint32_t addr) const {
		if (!power_enable_) {
			return 0;
		}
		auto it = data_mem_.find(addr);
		if (it == data_mem_.end()) {
			return 0;
		}
		return it->second;
	}

	void set_power_enable(bool on) {
		power_enable_ = on;
		if (!on) {
			running_ = false;
			status_reg_ = 0;
		}
	}

	void set_reset_n(bool rst_n) {
		reset_n_ = rst_n;
		if (!reset_n_) {
			reset_internal();
		}
	}

	void wait_cycles(uint32_t n) {
		cycle_ += n;
		refresh_status();
	}

	uint64_t cycle() const {
		return cycle_;
	}

	const std::vector<uint32_t>& noc_cmd_log() const {
		return noc_cmd_log_;
	}

private:
	void reset_internal() {
		mmio_regs_.clear();
		data_mem_.clear();
		noc_cmd_log_.clear();
		running_ = false;
		status_reg_ = 0;
		err_reg_ = false;
		counter_tx_pkt_ = 0;
		counter_tx_byte_ = 0;
		counter_rx_byte_ = 0;
		counter_stall_ = 0;
		mmio_regs_[CLUSTER_HDDU_BASE + HDDU_PLANE_EN] = 0xF;
		mmio_regs_[CLUSTER_HDDU_BASE + HDDU_MAX_OUTSTANDING] = 16;
	}

	void refresh_status() {
		if (running_ && cycle_ >= done_cycle_) {
			running_ = false;
			status_reg_ = err_reg_ ? (1u << 2) : (1u << 1);
			counter_tx_pkt_ += 64;
			counter_tx_byte_ += 64 * 24;
			counter_rx_byte_ += 16 * 24;
			counter_stall_ += 3;
		}
	}

	bool power_enable_ = false;
	bool reset_n_ = false;
	bool running_ = false;
	bool err_reg_ = false;
	uint64_t cycle_ = 0;
	uint64_t done_cycle_ = 0;

	uint32_t status_reg_ = 0;
	uint32_t counter_tx_pkt_ = 0;
	uint32_t counter_tx_byte_ = 0;
	uint32_t counter_rx_byte_ = 0;
	uint32_t counter_stall_ = 0;

	std::unordered_map<uint32_t, uint32_t> mmio_regs_;
	std::unordered_map<uint32_t, uint64_t> data_mem_;
	std::vector<uint32_t> noc_cmd_log_;
};

class ScenarioRunner {
public:
	explicit ScenarioRunner(ClusterSimDriver& driver)
		: driver_(driver) {}

	bool run(ScenarioKind kind) {
		if (kind == ScenarioKind::Conv2D) {
			return run_conv2d();
		}
		if (kind == ScenarioKind::GEMM) {
			return run_gemm();
		}
		const bool c = run_conv2d();
		const bool g = run_gemm();
		return c && g;
	}

private:
	AguCfg make_cfg(uint32_t base, uint16_t i0, uint16_t i1, int32_t s0, uint32_t tag_base) {
		AguCfg c;
		c.base_addr = base;
		c.iter0 = i0;
		c.iter1 = i1;
		c.iter2 = 1;
		c.iter3 = 1;
		c.stride0 = s0;
		c.stride1 = 0;
		c.stride2 = 0;
		c.stride3 = 0;
		c.tag_base = tag_base;
		c.mask_cfg = 0xF;
		return c;
	}

	bool run_conv2d() {
		std::cout << "[runner] scenario=conv2d" << std::endl;
		driver_.power_on_and_reset();
		driver_.config_spm_map(0xE4);

		const uint32_t weight_base = 0x000;
		const uint32_t ifmap_base = 0x100;
		const uint32_t pli_base = 0x200;
		const uint32_t ofmap_base = 0x300;

		driver_.preload_words64(weight_base, {0x11, 0x22, 0x33, 0x44});
		driver_.preload_words64(ifmap_base, {0x101, 0x202, 0x303, 0x404});
		driver_.preload_words64(pli_base, {0, 0, 0, 0});

		driver_.cfg_agu(AGU_PS, make_cfg(weight_base, 4, 1, 1, 0x10));
		driver_.cfg_agu(AGU_PD, make_cfg(ifmap_base, 4, 1, 1, 0x20));
		driver_.cfg_agu(AGU_PLI, make_cfg(pli_base, 4, 1, 1, 0x30));
		driver_.cfg_agu(AGU_PLO, make_cfg(ofmap_base, 4, 1, 1, 0x40));

		driver_.cfg_hddu_global(0xF, 0x1, 16);
		driver_.noc_init(0x12340000);
		driver_.send_scan_chain_words_reverse({
			pack_scan_chain(0, 1, 2, 3, 1, true),
			pack_scan_chain(4, 5, 6, 7, 1, true),
		});
		driver_.load_pe_program({0x1001, 0x2002, 0x3003, 0xF000});
		driver_.start_all();

		const bool done = driver_.wait_hddu_done(200, 2);
		const auto out = driver_.readback_words64(ofmap_base, 4);
		const auto cnt = driver_.read_hddu_counters();
		const bool ok = done && out.size() == 4 && cnt.tx_pkt > 0;

		std::cout << "[runner] conv2d done=" << (done ? "1" : "0")
				  << " tx_pkt=" << cnt.tx_pkt
				  << " tx_byte=" << cnt.tx_byte
				  << " rx_byte=" << cnt.rx_byte
				  << " stall=" << cnt.stall << std::endl;

		return ok;
	}

	bool run_gemm() {
		std::cout << "[runner] scenario=gemm" << std::endl;
		driver_.power_on_and_reset();
		driver_.config_spm_map(0xE4);

		const uint32_t a_base = 0x400;
		const uint32_t b_base = 0x500;
		const uint32_t c_base = 0x600;

		driver_.preload_words64(a_base, {0x1, 0x2, 0x3, 0x4, 0x5});
		driver_.preload_words64(b_base, {0x10, 0x20, 0x30, 0x40, 0x50});
		driver_.preload_words64(c_base, {0, 0, 0, 0, 0});

		driver_.cfg_agu(AGU_PS, make_cfg(a_base, 5, 1, 1, 0x11));
		driver_.cfg_agu(AGU_PD, make_cfg(b_base, 5, 1, 1, 0x22));
		driver_.cfg_agu(AGU_PLI, make_cfg(0x0, 1, 1, 0, 0x00));
		driver_.cfg_agu(AGU_PLO, make_cfg(c_base, 5, 1, 1, 0x33));

		driver_.cfg_hddu_global(0xB, 0x2, 16);
		driver_.noc_init(0x56780000);
		driver_.send_scan_chain_words_reverse({
			pack_scan_chain(0, 1, 2, 3, 0, true),
			pack_scan_chain(8, 9, 10, 11, 0, true),
		});
		driver_.load_pe_program({0xA001, 0xB002, 0xC003, 0xF000});
		driver_.start_all();

		const bool done = driver_.wait_hddu_done(200, 2);
		const auto out = driver_.readback_words64(c_base, 5);
		const auto cnt = driver_.read_hddu_counters();
		const bool ok = done && out.size() == 5 && cnt.tx_byte > 0;

		std::cout << "[runner] gemm done=" << (done ? "1" : "0")
				  << " tx_pkt=" << cnt.tx_pkt
				  << " tx_byte=" << cnt.tx_byte
				  << " rx_byte=" << cnt.rx_byte
				  << " stall=" << cnt.stall << std::endl;

		return ok;
	}

	ClusterSimDriver& driver_;
};

bool parse_args(int argc, char* argv[], RunnerOptions& opt) {
	for (int i = 1; i < argc; ++i) {
		const std::string a(argv[i]);
		if (a == "--case") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for --case" << std::endl;
				return false;
			}
			const std::string v(argv[++i]);
			if (v == "conv2d") {
				opt.scenario = ScenarioKind::Conv2D;
			} else if (v == "gemm") {
				opt.scenario = ScenarioKind::GEMM;
			} else if (v == "both") {
				opt.scenario = ScenarioKind::Both;
			} else {
				std::cerr << "unsupported case: " << v << std::endl;
				return false;
			}
		} else if (a == "--dry-run") {
			opt.dry_run = true;
		} else if (a == "--help" || a == "-h") {
			std::cout << "Usage: test_cluster_sim [--case conv2d|gemm|both] [--dry-run]" << std::endl;
			return false;
		} else {
			std::cerr << "unknown arg: " << a << std::endl;
			return false;
		}
	}
	return true;
}

} // namespace cluster_sim

int sc_main(int argc, char* argv[]) {
	using namespace cluster_sim;
	RunnerOptions options;
	if (!parse_args(argc, argv, options)) {
		return 1;
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

	MockBackend backend;
	DriverHooks hooks;
	hooks.mmio_write = [&](uint32_t a, uint32_t d) { backend.mmio_write(a, d); };
	hooks.mmio_read = [&](uint32_t a) -> uint32_t { return backend.mmio_read(a); };
	hooks.data_write64 = [&](uint32_t a, uint64_t d) { backend.data_write64(a, d); };
	hooks.data_read64 = [&](uint32_t a) -> uint64_t { return backend.data_read64(a); };
	hooks.set_power_enable = [&](bool on) { backend.set_power_enable(on); };
	hooks.set_reset_n = [&](bool rst_n) { backend.set_reset_n(rst_n); };
	hooks.wait_cycles = [&](uint32_t n) { backend.wait_cycles(n); };

	ClusterSimDriver driver(std::move(hooks));
	ScenarioRunner runner(driver);

	if (!options.dry_run) {
		std::cout << "[test_cluster_sim] non-dry-run currently maps to mock backend execution" << std::endl;
	}

	const bool pass = runner.run(options.scenario);
	const std::string case_name =
		(options.scenario == ScenarioKind::Conv2D)
			? "conv2d"
			: (options.scenario == ScenarioKind::GEMM ? "gemm" : "both");

	std::cout << "[test_cluster_sim] runner case=" << case_name
			  << " dry_run=" << (options.dry_run ? "1" : "0")
			  << " sim_cycles=" << backend.cycle()
			  << " noc_cmd_count=" << backend.noc_cmd_log().size()
			  << std::endl;

	return pass ? 0 : 1;
}
