#define SC_INCLUDE_DYNAMIC_PROCESSES
#include <systemc>

#include "ComputeCluster.hpp"
#include "tb_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace cluster_sim {

// Command encoding for NoC interface
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
static constexpr uint32_t REG_STATUS = 0x24;
static constexpr uint32_t REG_LANE_CFG = 0x28;
static constexpr uint32_t REG_TAG_BASE = 0x40;
static constexpr uint32_t REG_TAG_STRIDE0 = 0x44;
static constexpr uint32_t REG_TAG_STRIDE1 = 0x48;
static constexpr uint32_t REG_TAG_CTRL = 0x4C;
static constexpr uint32_t REG_MASK_CFG = 0x54;
static constexpr uint32_t REG_ERR_CODE = 0x58;
static constexpr uint32_t REG_DBG_TAG = 0x5C;
static constexpr uint32_t REG_DBG_ADDR = 0x60;

static constexpr uint32_t HDDU_CTRL = 0x800;
static constexpr uint32_t HDDU_STATUS = 0x804;
static constexpr uint32_t HDDU_PLANE_EN = 0x808;
static constexpr uint32_t HDDU_PLANE_MODE = 0x80C;

struct SimOptions {
	bool verbose = false;
	bool enable_trace = false;
	bool dry_run = true;
	int clock_period_ns = 10;
	int timeout_cycles = 200;
	std::string test_path;
	std::string trace_file;
};

// Helper function to pack NoC command and parameter into a single 32-bit word
inline uint32_t pack_noc_cmd(message_command_t cmd, uint32_t param) {
	return (param & 0xFFFFFFF0u) | (static_cast<uint32_t>(cmd) & 0x0Fu);
}

// Helper function to pack a LOAD_PROGRAM command with instruction memory address and 16-bit instruction data
inline uint32_t pack_load_program(uint16_t im_addr_bytes, uint16_t inst16) {
	uint32_t p = 0;
	p |= (static_cast<uint32_t>(im_addr_bytes) & PE_ROUTER_IM_ADDR_MASK) << PE_ROUTER_IM_ADDR_OFFSET;
	p |= (static_cast<uint32_t>(inst16) & PE_ROUTER_IM_DATA_MASK) << PE_ROUTER_IM_DATA_OFFSET;
	return pack_noc_cmd(CMD_LOAD_PROGRAM, p);
}

// Testbench module for the ComputeCluster
class Cluster_tb : public sc_core::sc_module {
public:
	SC_HAS_PROCESS(Cluster_tb);

	struct DmaTimingModel {
		uint32_t bandwidth_words64_per_cycle = 8;
		uint32_t startup_latency_cycles = 4;
		uint32_t burst_size_words64 = 16;

		uint32_t estimate_latency(uint32_t word_count) const {
			if (word_count == 0) return 0;
			const uint32_t bursts = (word_count + burst_size_words64 - 1) / burst_size_words64;
			return startup_latency_cycles + bursts * burst_size_words64 / std::max<uint32_t>(1, bandwidth_words64_per_cycle);
		}
	};

	struct DmaRequest {
		uint32_t wave_id = 0;
		cluster_json::DmaWaveCfg wave;
		cluster_json::DmaTransferCfg::Direction dir = cluster_json::DmaTransferCfg::Direction::DramToSpm;
		uint32_t latency_cycles = 1;
	};

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

	SimOptions opt_;
	bool pass_ = false;
	uint64_t cycle_ = 0;

	// Signals
	sc_core::sc_clock clk;
	sc_core::sc_signal<bool> reset_n;
	sc_core::sc_signal<bool> power_enable_i;
	sc_core::sc_signal<bool> interrupt_o;

	// AXI4-Lite signals for DMA control and SPM access
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

	// AHB-Lite signals for MMIO access
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

	// DUT instance
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

	Cluster_tb(sc_core::sc_module_name name, const SimOptions& options)
		: sc_core::sc_module(name),
		  opt_(options),
		  clk("clk", sc_core::sc_time(options.clock_period_ns, sc_core::SC_NS)),
		  dut("Cluster", hybridacc::NetWorkOnChipConfig(4, 4)) {

		// Bind DUT ports to testbench signals
		bind_ports();

		// Initialize signals
		if (opt_.enable_trace) {
			dut.enable_perffeto_trace();
			PerfettoTrace::getInstance().open(opt_.trace_file);
		}

		SC_THREAD(main_process);
		SC_THREAD(dma_process);
	}

	~Cluster_tb() override {
		if (opt_.enable_trace) {
			PerfettoTrace::getInstance().close();
		}
	}

private:
	DmaTimingModel dma_timing_;
	std::unordered_map<uint32_t, uint64_t> dram_shadow_;
	std::deque<DmaRequest> dma_queue_;
	std::set<uint32_t> dma_done_waves_;
	sc_core::sc_event dma_req_evt_;
	sc_core::sc_event dma_done_evt_;

	// Helper function to bind DUT ports to testbench signals
	void bind_ports() {
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
	}

	// Helper function to initialize all signals to default values
	void init_signals() {
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

	// Helper function to wait for a certain number of clock cycles
	void tick(uint32_t n = 1) {
		for (uint32_t i = 0; i < n; ++i) {
			wait(clk.posedge_event());
			cycle_ += 1;
		}
	}

	// Helper function to wait for a certain number of clock cycles (alternative name)
	void wait_cycles(uint32_t n) { tick(n); }

	// Wait until interrupt_o is asserted or timeout.
	bool wait_interrupt_asserted(uint32_t timeout_cycles) {
		if (interrupt_o.read()) {
			return true;
		}
		for (uint32_t i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (interrupt_o.read()) {
				return true;
			}
		}
		return false;
	}

	// Helper functions for AHB-Lite transactions
	bool ahb_write_tx(uint32_t addr, uint32_t data) {
		if (!hready_o.read()) return false;
		hsel_i.write(true);
		haddr_i.write(addr);
		hwrite_i.write(true);
		htrans_i.write(2);
		hsize_i.write(2);
		hburst_i.write(0);
		hprot_i.write(0);
		hready_i.write(true);
		tick(1);
		hsel_i.write(false);
		htrans_i.write(0);
		hwdata_i.write(data);
		for (int i = 0; i < opt_.timeout_cycles; ++i) {
			tick(1);
			if (hready_o.read()) {
				hwdata_i.write(0);
				return true;
			}
		}
		hwdata_i.write(0);
		return false;
	}

	// Returns true and sets 'out' if read is successful, returns false on timeout
	bool ahb_read_tx(uint32_t addr, uint32_t& out) {
		if (!hready_o.read()) return false;
		hsel_i.write(true);
		haddr_i.write(addr);
		hwrite_i.write(false);
		htrans_i.write(2);
		hsize_i.write(2);
		hburst_i.write(0);
		hprot_i.write(0);
		hready_i.write(true);
		tick(1);
		hsel_i.write(false);
		htrans_i.write(0);
		for (int i = 0; i < opt_.timeout_cycles; ++i) {
			tick(1);
			if (hready_o.read()) {
				out = hrdata_o.read().to_uint();
				return true;
			}
		}
		return false;
	}

	// Helper functions for AXI4-Lite transactions (for DMA and SPM access)
	bool data_write_tx(uint32_t addr, uint64_t data) {
		s_axi_awaddr_i.write(addr);
		s_axi_awvalid_i.write(true);
		s_axi_wdata_i.write(sc_dt::sc_biguint<64>(data));
		s_axi_wstrb_i.write(0xFF);
		s_axi_wvalid_i.write(true);
		s_axi_bready_i.write(true);

		bool aw_done = false;
		bool w_done = false;
		for (int i = 0; i < opt_.timeout_cycles; ++i) {
			if (!aw_done && s_axi_awready_o.read()) aw_done = true;
			if (!w_done && s_axi_wready_o.read()) w_done = true;
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

	// Returns true if read is successful and sets 'out', returns false on timeout
	bool data_read_tx(uint32_t addr, uint64_t& out) {
		s_axi_araddr_i.write(addr);
		s_axi_arvalid_i.write(true);
		s_axi_rready_i.write(true);

		bool ar_done = false;
		for (int i = 0; i < opt_.timeout_cycles; ++i) {
			if (!ar_done && s_axi_arready_o.read()) ar_done = true;
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

	// Streaming/pipelined AXI4-Lite write helper for DMA bursts
	bool data_write_stream(const std::vector<uint32_t>& addrs,
	                      const std::vector<uint64_t>& data_words,
	                      uint32_t max_outstanding = 1,
	                      uint32_t no_progress_timeout = 0) {
		if (addrs.size() != data_words.size()) return false;
		if (addrs.empty()) return true;
		if (no_progress_timeout == 0) no_progress_timeout = static_cast<uint32_t>(opt_.timeout_cycles);

		const uint32_t total = static_cast<uint32_t>(addrs.size());
		const uint32_t window = std::max<uint32_t>(1, max_outstanding);

		uint32_t aw_accepted = 0;
		uint32_t w_accepted = 0;
		uint32_t b_accepted = 0;
		uint32_t no_progress_cycles = 0;

		bool aw_valid = false;
		bool w_valid = false;

		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_bready_i.write(true);

		while (b_accepted < total) {
			bool progress = false;

			if (!aw_valid && aw_accepted < total && (aw_accepted - b_accepted) < window) {
				s_axi_awaddr_i.write(addrs[aw_accepted]);
				aw_valid = true;
			}
			if (!w_valid && w_accepted < total && (w_accepted - b_accepted) < window) {
				s_axi_wdata_i.write(sc_dt::sc_biguint<64>(data_words[w_accepted]));
				s_axi_wstrb_i.write(0xFF);
				w_valid = true;
			}

			s_axi_awvalid_i.write(aw_valid);
			s_axi_wvalid_i.write(w_valid);

			if (aw_valid && s_axi_awready_o.read()) {
				aw_valid = false;
				aw_accepted += 1;
				progress = true;
			}
			if (w_valid && s_axi_wready_o.read()) {
				w_valid = false;
				w_accepted += 1;
				progress = true;
			}
			if (s_axi_bvalid_o.read() && s_axi_bready_i.read()) {
				b_accepted += 1;
				progress = true;
			}

			tick(1);
			if (progress) {
				no_progress_cycles = 0;
			} else {
				no_progress_cycles += 1;
				if (no_progress_cycles >= no_progress_timeout) {
					s_axi_awvalid_i.write(false);
					s_axi_wvalid_i.write(false);
					s_axi_bready_i.write(false);
					tick(1);
					return false;
				}
			}
		}

		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_bready_i.write(false);
		tick(1);
		return true;
	}

	// Streaming/pipelined AXI4-Lite read helper for DMA bursts
	bool data_read_stream(const std::vector<uint32_t>& addrs,
	                     std::vector<uint64_t>& out_words,
	                     uint32_t max_outstanding = 1,
	                     uint32_t no_progress_timeout = 0) {
		if (addrs.empty()) {
			out_words.clear();
			return true;
		}
		if (no_progress_timeout == 0) no_progress_timeout = static_cast<uint32_t>(opt_.timeout_cycles);

		const uint32_t total = static_cast<uint32_t>(addrs.size());
		const uint32_t window = std::max<uint32_t>(1, max_outstanding);
		out_words.assign(total, 0);

		uint32_t ar_accepted = 0;
		uint32_t r_accepted = 0;
		uint32_t no_progress_cycles = 0;
		bool ar_valid = false;

		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(true);

		while (r_accepted < total) {
			bool progress = false;

			if (!ar_valid && ar_accepted < total && (ar_accepted - r_accepted) < window) {
				s_axi_araddr_i.write(addrs[ar_accepted]);
				ar_valid = true;
			}

			s_axi_arvalid_i.write(ar_valid);

			if (ar_valid && s_axi_arready_o.read()) {
				ar_valid = false;
				ar_accepted += 1;
				progress = true;
			}

			if (s_axi_rvalid_o.read() && s_axi_rready_i.read()) {
				out_words[r_accepted] = s_axi_rdata_o.read().to_uint64();
				r_accepted += 1;
				progress = true;
			}

			tick(1);
			if (progress) {
				no_progress_cycles = 0;
			} else {
				no_progress_cycles += 1;
				if (no_progress_cycles >= no_progress_timeout) {
					s_axi_arvalid_i.write(false);
					s_axi_rready_i.write(false);
					tick(1);
					return false;
				}
			}
		}

		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(false);
		tick(1);
		return true;
	}

	// Helper functions for MMIO access using AHB-Lite
	void mmio_write(uint32_t addr, uint32_t data) {
		if (!ahb_write_tx(addr, data)) throw std::runtime_error("MMIO write timeout");
	}

	// Returns the read value, or throws an exception on timeout
	uint32_t mmio_read(uint32_t addr) {
		uint32_t out = 0;
		if (!ahb_read_tx(addr, out)) throw std::runtime_error("MMIO read timeout");
		if (addr >= CLUSTER_HDDU_BASE && addr < (CLUSTER_HDDU_BASE + 0x1000)) {
			uint32_t stable = 0;
			if (!ahb_read_tx(addr, stable)) throw std::runtime_error("HDDU MMIO read timeout");
			out = stable;
		}
		return out;
	}

	std::string format_hddu_status(uint32_t st) const {
		std::ostringstream oss;
		oss << "0x" << std::hex << st << std::dec
			<< " [idle=" << ((st >> (int)hybridacc::cluster::HdduStatusBit::IDLE) & 0x1)
			<< " busy=" << ((st >> (int)hybridacc::cluster::HdduStatusBit::BUSY) & 0x1)
			<< " done=" << ((st >> (int)hybridacc::cluster::HdduStatusBit::DONE) & 0x1)
			<< " stall=" << ((st >> (int)hybridacc::cluster::HdduStatusBit::STALL) & 0x1)
			<< " error=" << ((st >> (int)hybridacc::cluster::HdduStatusBit::ERROR) & 0x1)
			<< "]";
		return oss.str();
	}

	std::string format_agu_status(uint32_t st) const {
		std::ostringstream oss;
		oss << "0x" << std::hex << st << std::dec
			<< " [busy=" << ((st >> (int)hybridacc::cluster::AguStatusBit::STATUS_BUSY) & 0x1)
			<< " done=" << ((st >> (int)hybridacc::cluster::AguStatusBit::STATUS_DONE) & 0x1)
			<< " error=" << ((st >> (int)hybridacc::cluster::AguStatusBit::STATUS_ERROR) & 0x1)
			<< " stall=" << ((st >> (int)hybridacc::cluster::AguStatusBit::STATUS_STALL) & 0x1)
			<< "]";
		return oss.str();
	}

	void dump_agu_runtime(uint32_t bank, const char* name) {
		const uint32_t b = CLUSTER_HDDU_BASE + bank * AGU_BANK_STRIDE;
		const uint32_t base_addr = mmio_read(b + REG_BASE_ADDR);
		const uint32_t ctrl = mmio_read(b + REG_CTRL);
		const uint32_t status = mmio_read(b + REG_STATUS);
		const uint32_t err = mmio_read(b + REG_ERR_CODE);
		const uint32_t dbg_tag = mmio_read(b + REG_DBG_TAG);
		const uint32_t dbg_addr = mmio_read(b + REG_DBG_ADDR);
		std::cout << "[cluster_tb][DEBUG][AGU] " << name
				  << " base=0x" << std::hex << base_addr
				  << " ctrl=0x" << ctrl
				  << " status=" << format_agu_status(status)
				  << " err=0x" << err
				  << " dbg_tag=0x" << dbg_tag
				  << " dbg_addr=0x" << dbg_addr
				  << std::dec << std::endl;
	}

	void dump_runtime_snapshot(const std::string& tag, uint8_t current_spm_map) {
		const uint32_t hddu_ctrl = mmio_read(CLUSTER_HDDU_BASE + HDDU_CTRL);
		const uint32_t hddu_status = mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
		const uint32_t plane_en = mmio_read(CLUSTER_HDDU_BASE + HDDU_PLANE_EN);
		const uint32_t plane_mode = mmio_read(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE);

		std::cout << "[cluster_tb][DEBUG][SNAPSHOT] " << tag
				  << " cycle=" << cycle_
				  << " interrupt=" << interrupt_o.read()
				  << " spm_map=0x" << std::hex << static_cast<uint32_t>(current_spm_map)
				  << " hddu_ctrl=0x" << hddu_ctrl
				  << " hddu_status=" << format_hddu_status(hddu_status)
				  << " plane_en=0x" << plane_en
				  << " plane_mode=0x" << plane_mode
				  << std::dec << std::endl;

		dump_agu_runtime(AGU_PS, "PS");
		dump_agu_runtime(AGU_PD, "PD");
		dump_agu_runtime(AGU_PLI, "PLI");
		dump_agu_runtime(AGU_PLO, "PLO");
	}

	// Helper function to configure an AGU bank with the given configuration
	void cfg_agu(uint32_t bank, const cluster_json::AguCfg& c) {
		if (!c.enable) return;
		const uint32_t b = CLUSTER_HDDU_BASE + bank * AGU_BANK_STRIDE;
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

	// Helper function to perform power-on reset sequence
	void power_on_reset() {
		power_enable_i.write(true);
		reset_n.write(false);
		wait_cycles(4);
		reset_n.write(true);
		wait_cycles(4);
	}

	// Helper function to configure SPM mapping
	void config_spm_map(uint8_t map_val) {
		mmio_write(CLUSTER_SPM_CFG_MAP, static_cast<uint32_t>(map_val));
		mmio_write(CLUSTER_SPM_CFG_UPDATE, 0x1u);
	}

	// Helper function to configure HDDU global settings (plane enable and mode)
	void cfg_hddu_global(uint32_t plane_en, uint32_t plane_mode) {
		mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN, plane_en);
		mmio_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE, plane_mode);
		mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_START));
	}

	// Helper function to send a NoC command with the given command and parameter
	void noc_cmd_write(uint32_t packed_cmd) { mmio_write(CLUSTER_NOC_CMD, packed_cmd); }

	// Helper function to load a PE program into the instruction memory using NoC commands
	void load_pe_program(const std::vector<uint16_t>& inst16) {
		for (uint32_t pc = 0; pc < inst16.size(); ++pc) {
			noc_cmd_write(pack_load_program(static_cast<uint16_t>(pc * 2), inst16[pc]));
		}
	}

	// Helper function to send scan chain data in reverse order (if needed by the hardware)
	void send_scan_chain_words_reverse(const std::vector<uint32_t>& words) {
		for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
			noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, words[static_cast<size_t>(i)]));
		}
	}

	// Helper functions to start and stop the compute wave, and to wait for HDDU completion
	void start_all() {
		noc_cmd_write(pack_noc_cmd(CMD_START_PE, 0));
		mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_START));
	}

	// Helper function to stop the compute wave and all PEs
	void stop_all() {
		mmio_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << (int)hybridacc::cluster::HdduCtrllBit::CTRL_STOP));
		noc_cmd_write(pack_noc_cmd(CMD_STOP_PE, 0));
	}

	// Helper function to poll HDDU status until done or error, returns true if done, false if error or timeout
	bool wait_hddu_done(uint32_t timeout_cycles, uint32_t poll_step = 1,
						   const std::string& debug_tag = std::string(),
						   uint8_t current_spm_map = 0xE4) {
		const bool irq_asserted = wait_interrupt_asserted(timeout_cycles);
		if (!irq_asserted) {
			std::cout << "[cluster_tb][COMPUTE][timeout] " << debug_tag
					  << " interrupt=0"
					  << " hddu_status=" << format_hddu_status(mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS))
					  << std::endl;
			return false;
		}

		const uint32_t st_irq = mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
		std::cout << "[cluster_tb][COMPUTE][irq] " << debug_tag
				  << " interrupt=1"
				  << " hddu_status=" << format_hddu_status(st_irq)
				  << std::endl;
		if (st_irq & (1u << (int)hybridacc::cluster::HdduStatusBit::ERROR)) {
			return false;
		}
		if (st_irq & (1u << (int)hybridacc::cluster::HdduStatusBit::DONE)) {
			return true;
		}

		// Rare timing corner: interrupt arrives slightly before DONE visibility in MMIO.
		uint32_t last_status = std::numeric_limits<uint32_t>::max();
		const uint32_t checkpoint = std::max<uint32_t>(poll_step, 100000u);
		bool stall_snapshot_taken = false;
		for (uint32_t waited = 0; waited < timeout_cycles; waited += poll_step) {
			const uint32_t st = mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
			if (st != last_status) {
				std::cout << "[cluster_tb][COMPUTE][status] " << debug_tag
						  << " waited=" << waited
						  << " interrupt=" << interrupt_o.read()
						  << " hddu_status=" << format_hddu_status(st)
						  << std::endl;
				last_status = st;
			}
			if (!stall_snapshot_taken && (st & (1u << (int)hybridacc::cluster::HdduStatusBit::STALL))) {
				stall_snapshot_taken = true;
				dump_runtime_snapshot("stall-detected " + debug_tag + " waited=" + std::to_string(waited), current_spm_map);
			}
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::ERROR)) {
				const uint32_t st_confirm = mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
				if (st_confirm & (1u << (int)hybridacc::cluster::HdduStatusBit::ERROR)) {
					return false;
				}
			}
			if (st & (1u << (int)hybridacc::cluster::HdduStatusBit::DONE)) return true;
			if (waited > 0 && (waited % checkpoint) == 0) {
				std::cout << "[cluster_tb][COMPUTE][waiting] " << debug_tag
						  << " waited=" << waited
						  << " interrupt=" << interrupt_o.read()
						  << " hddu_status=" << format_hddu_status(st)
						  << std::endl;
			}
			wait_cycles(poll_step);
		}
		std::cout << "[cluster_tb][COMPUTE][timeout] " << debug_tag
				  << " interrupt=" << interrupt_o.read()
				  << " hddu_status=" << format_hddu_status(mmio_read(CLUSTER_HDDU_BASE + HDDU_STATUS))
				  << std::endl;
		return false;
	}

	// Helper function to pack a vector of 16-bit values into a vector of 64-bit words for AXI transactions
	static std::vector<uint64_t> pack_to_64(const std::vector<uint16_t>& in) {
		std::vector<uint64_t> out;
		out.reserve((in.size() + 3) / 4);
		for (size_t i = 0; i < in.size(); i += 4) {
			uint64_t w = 0;
			if (i + 0 < in.size()) w |= static_cast<uint64_t>(in[i + 0]) << 0;
			if (i + 1 < in.size()) w |= static_cast<uint64_t>(in[i + 1]) << 16;
			if (i + 2 < in.size()) w |= static_cast<uint64_t>(in[i + 2]) << 32;
			if (i + 3 < in.size()) w |= static_cast<uint64_t>(in[i + 3]) << 48;
			out.push_back(w);
		}
		return out;
	}

	// Helper function to pack a vector of 32-bit values into a vector of 64-bit words for AXI transactions
	void preload_dram_shadow(uint32_t base_addr, const std::vector<uint64_t>& words) {
		constexpr uint32_t kWordBytes = 8;
		for (uint32_t i = 0; i < words.size(); ++i) {
			dram_shadow_[base_addr + i * kWordBytes] = words[i];
		}
	}

	// Helper function to read back a vector of 64-bit words from the dram_shadow_ for verification
	std::vector<uint64_t> readback_dram_shadow(uint32_t base_addr, uint32_t word_count) const {
		constexpr uint32_t kWordBytes = 8;
		std::vector<uint64_t> out(word_count, 0);
		for (uint32_t i = 0; i < word_count; ++i) {
			auto it = dram_shadow_.find(base_addr + i * kWordBytes);
			out[i] = (it == dram_shadow_.end()) ? 0ULL : it->second;
		}
		return out;
	}

	// Helper function to enqueue a DMA transfer for a given wave and direction, calculates total words and estimated latency
	void enqueue_dma(const cluster_json::DmaWaveCfg& wave,
	                 cluster_json::DmaTransferCfg::Direction dir,
	                 uint32_t total_words) {
		dma_done_waves_.erase(wave.wave_id);
		DmaRequest req;
		req.wave_id = wave.wave_id;
		req.wave = wave;
		req.dir = dir;
		req.latency_cycles = std::max<uint32_t>(1, dma_timing_.estimate_latency(total_words));
		dma_queue_.push_back(std::move(req));
		std::cout << "[cluster_tb][DMA][enqueue] wave=" << wave.wave_id
				  << " dir=" << (dir == cluster_json::DmaTransferCfg::Direction::DramToSpm ? "D2S" : "S2D")
				  << " words=" << total_words
				  << " est_latency=" << dma_timing_.estimate_latency(total_words)
				  << " queue_depth=" << dma_queue_.size()
				  << std::endl;
		dma_req_evt_.notify(sc_core::SC_ZERO_TIME);
	}

	// Helper function to wait for a DMA transfer to complete for a given wave ID, returns true if done, false if timeout
	bool wait_dma_done(uint32_t wave_id, uint32_t timeout_cycles = 200000) {
		std::cout << "[cluster_tb][DMA][wait] wave=" << wave_id
				  << " timeout=" << timeout_cycles << std::endl;
		for (uint32_t i = 0; i < timeout_cycles; ++i) {
			if (dma_done_waves_.count(wave_id) > 0) {
				std::cout << "[cluster_tb][DMA][done] wave=" << wave_id
						  << " waited=" << i << std::endl;
				return true;
			}
			tick(1);
		}
		std::cout << "[cluster_tb][DMA][timeout] wave=" << wave_id << std::endl;
		return false;
	}

	// Helper function to issue DMA transfers for a given wave and direction, returns total words enqueued
	uint32_t issue_dma_stage(const cluster_json::DmaWaveCfg& wave,
	                         cluster_json::DmaTransferCfg::Direction dir) {
		uint32_t total_words = 0;
		for (const auto& t : wave.transfers) {
			if (t.direction == dir) total_words += t.size_words64;
		}
		if (total_words > 0) enqueue_dma(wave, dir, total_words);
		return total_words;
	}

	// Helper function to run a compute wave with the given plan and mode, returns true if completed successfully, false on error or timeout
	bool run_compute_wave(const cluster_json::ClusterPlan& plan, bool is_ultra,
					  int wave_id, uint8_t current_spm_map) {
		std::cout << "[cluster_tb][COMPUTE][start] wave=" << wave_id
				  << " plan='" << plan.name
				  << "' mask=0x" << std::hex << plan.global_mask << std::dec
				  << " mode=" << (is_ultra ? "ultra" : "linear")
				  << " spm_map=0x" << std::hex << static_cast<uint32_t>(current_spm_map)
				  << std::dec << std::endl;
		std::cout << "[cluster_tb][COMPUTE] agu_ps  " << plan.agu_ps << std::endl;
		std::cout << "[cluster_tb][COMPUTE] agu_pd  " << plan.agu_pd << std::endl;
		std::cout << "[cluster_tb][COMPUTE] agu_pli " << plan.agu_pli << std::endl;
		std::cout << "[cluster_tb][COMPUTE] agu_plo " << plan.agu_plo << std::endl;
		cfg_agu(AGU_PS, plan.agu_ps);
		cfg_agu(AGU_PD, plan.agu_pd);
		cfg_agu(AGU_PLI, plan.agu_pli);
		cfg_agu(AGU_PLO, plan.agu_plo);
		cfg_hddu_global(plan.global_mask, is_ultra ? 0x2u : 0x1u);
		start_all();
		const uint32_t compute_timeout = WAVE_TIMEOUT_CYCLES;
		const uint32_t poll_step = std::min<uint32_t>(POLL_INTERVAL_CYCLES, compute_timeout);
		const std::string debug_tag = "wave=" + std::to_string(wave_id) + " plan='" + plan.name + "'";
		const bool done = wait_hddu_done(compute_timeout, std::max<uint32_t>(1, poll_step), debug_tag, current_spm_map);
		stop_all();
		if (!done) {
			dump_runtime_snapshot("compute-fail " + debug_tag, current_spm_map);
		}
		std::cout << "[cluster_tb][COMPUTE][" << (done ? "done" : "fail") << "] wave="
				  << wave_id << " plan='" << plan.name << "'" << std::endl;
		return done;
	}


	// Helper function to process DMA requests from the queue, performs AXI transactions and simulates latency, runs in a separate thread
	void dma_process() {
		while (true) {
			if (dma_queue_.empty()) {
				wait(dma_req_evt_);
			}
			while (!dma_queue_.empty()) {
				auto req = dma_queue_.front();
				dma_queue_.pop_front();
				std::cout << "[cluster_tb][DMA][process] wave=" << req.wave_id
						  << " dir=" << (req.dir == cluster_json::DmaTransferCfg::Direction::DramToSpm ? "D2S" : "S2D")
						  << " transfers=" << req.wave.transfers.size()
						  << " latency=" << req.latency_cycles << std::endl;
				for (const auto& t : req.wave.transfers) {
					if (t.direction != req.dir) continue;
					const bool is_d2s = (req.dir == cluster_json::DmaTransferCfg::Direction::DramToSpm);
					const uint32_t dst_spm = (req.wave.buf_sel == 1 && t.dst_parallel_spm_addr != 0) ? t.dst_parallel_spm_addr : t.dst_spm_addr;
					const uint32_t src_spm = (req.wave.buf_sel == 1 && t.src_parallel_spm_addr != 0) ? t.src_parallel_spm_addr : t.src_spm_addr;

					auto src_addrs = cluster_json::build_dma_addr_list(
						t.src_addr_gen,
						is_d2s ? t.src_dram_addr : src_spm,
						t.size_words64);
					auto dst_addrs = cluster_json::build_dma_addr_list(
						t.dst_addr_gen,
						is_d2s ? dst_spm : t.dst_dram_addr,
						t.size_words64);

					std::cout << "[cluster_tb][DMA][xfer] tensor='" << t.tensor
							  << "' words=" << t.size_words64
							  << " src=0x" << std::hex << (src_addrs.empty() ? 0 : src_addrs.front())
							  << " dst=0x" << (dst_addrs.empty() ? 0 : dst_addrs.front())
							  << std::dec << std::endl;

					if (is_d2s) {
						std::vector<uint64_t> write_words;
						write_words.reserve(t.size_words64);
						for (uint32_t i = 0; i < t.size_words64; ++i) {
							const uint64_t v = dram_shadow_.count(src_addrs[i]) ? dram_shadow_[src_addrs[i]] : 0ULL;
							write_words.push_back(v);
						}
						if (!data_write_stream(dst_addrs, write_words, 1, static_cast<uint32_t>(opt_.timeout_cycles) * 16u)) {
							SC_REPORT_ERROR("cluster_tb", "DMA D2S AXI stream write timeout");
						}
					} else {
						std::vector<uint64_t> read_words;
						if (!data_read_stream(src_addrs, read_words, 1, static_cast<uint32_t>(opt_.timeout_cycles) * 16u)) {
							SC_REPORT_ERROR("cluster_tb", "DMA S2D AXI stream read timeout");
						}
						for (uint32_t i = 0; i < t.size_words64; ++i) {
							dram_shadow_[dst_addrs[i]] = read_words[i];
						}
					}
				}

				for (uint32_t c = 0; c < req.latency_cycles; ++c) {
					wait(clk.posedge_event());
				}
				dma_done_waves_.insert(req.wave_id);
				std::cout << "[cluster_tb][DMA][process-done] wave=" << req.wave_id << std::endl;
				dma_done_evt_.notify(sc_core::SC_ZERO_TIME);
			}
		}
	}

	// Helper function to print a summary of the loaded configuration from JSON and the prepared data, for debugging purposes
	void print_loaded_config(
		const std::shared_ptr<JsonValue>& meta,
		const std::shared_ptr<JsonValue>& hardware,
		const std::shared_ptr<JsonValue>& files,
		const std::vector<uint16_t>& act,
		const std::vector<uint16_t>& wgt,
		const std::vector<uint16_t>& psum,
		const std::vector<uint16_t>& gold,
		const std::vector<uint16_t>& pe_prog,
		const std::vector<uint32_t>& scan_chain_data,
		uint32_t addr_act,
		uint32_t addr_wgt,
		uint32_t addr_psum,
		uint32_t addr_out,
		const std::unordered_map<std::string, cluster_json::SpmSectionAddr>& section_map,
		const std::vector<cluster_json::DmaWaveCfg>& waves,
		const std::vector<cluster_json::ClusterPlan>& plans,
		bool is_ultra) {
		std::cout << "\n[cluster_tb] ===== JSON Configuration Summary =====" << std::endl;
		std::cout << "[cluster_tb] ultra_mode=" << (is_ultra ? "true" : "false") << std::endl;

		if (meta && meta->is_object()) {
			std::cout << "[cluster_tb] meta:" << std::endl;
			for (const auto& [k, v] : meta->as_object()) {
				std::cout << "  - " << k << " = " << cluster_json::format_json(v) << std::endl;
			}
		}

		if (hardware && hardware->is_object()) {
			std::cout << "[cluster_tb] hardware:" << std::endl;
			for (const auto& [k, v] : hardware->as_object()) {
				std::cout << "  - " << k << " = " << cluster_json::format_json(v) << std::endl;
			}
		}

		if (files && files->is_object()) {
			std::cout << "[cluster_tb] software.files:" << std::endl;
			for (const auto& [k, v] : files->as_object()) {
				std::cout << "  - " << k << " = " << cluster_json::format_json(v) << std::endl;
			}
		}

		std::cout << "[cluster_tb] binary sizes(fp16/word): act=" << act.size()
				  << " wgt=" << wgt.size()
				  << " psum=" << psum.size()
				  << " gold=" << gold.size()
				  << " pe_prog(inst16)=" << pe_prog.size()
				  << " scan_chain(words32)=" << scan_chain_data.size()
				  << std::endl;

		std::cout << "[cluster_tb] dram mapping: act=0x" << std::hex << addr_act
				  << " wgt=0x" << addr_wgt
				  << " psum=0x" << addr_psum
				  << " out=0x" << addr_out << std::dec << std::endl;

		std::cout << "[cluster_tb] dma timing: bw_words64/cycle=" << dma_timing_.bandwidth_words64_per_cycle
				  << " startup=" << dma_timing_.startup_latency_cycles
				  << " burst=" << dma_timing_.burst_size_words64 << std::endl;

		std::cout << "[cluster_tb] section_map count=" << section_map.size() << std::endl;
		for (const auto& [name, addr] : section_map) {
			std::cout << "  - section='" << name << "' linear=0x" << std::hex << addr.linear_addr
					  << " parallel=0x" << addr.parallel_addr << std::dec << std::endl;
		}

		std::cout << "[cluster_tb] dma waves=" << waves.size() << std::endl;
		for (size_t i = 0; i < waves.size(); ++i) {
			const auto& w = waves[i];
			std::cout << "  - wave[" << i << "] id=" << w.wave_id
					  << " compute_plan_idx=" << w.compute_plan_idx
					  << " spm_map=0x" << std::hex << static_cast<uint32_t>(w.spm_map_val) << std::dec
					  << " buf_sel=" << w.buf_sel
					  << " transfers=" << w.transfers.size() << std::endl;
			if (!w.spm_map_reason.empty()) {
				std::cout << "      spm_map_reason='" << w.spm_map_reason << "'" << std::endl;
			}
			for (size_t j = 0; j < w.transfers.size(); ++j) {
				const auto& t = w.transfers[j];
				std::cout << "      * t[" << j << "] tensor='" << t.tensor
						  << "' dir="
						  << (t.direction == cluster_json::DmaTransferCfg::Direction::DramToSpm ? "D2S" : "S2D")
						  << " words=" << t.size_words64
						  << " src_dram=0x" << std::hex << t.src_dram_addr
						  << " dst_spm=0x" << t.dst_spm_addr
						  << " src_spm=0x" << t.src_spm_addr
						  << " dst_dram=0x" << t.dst_dram_addr
						  << std::dec << std::endl;
			}
		}

		std::cout << "[cluster_tb] cluster plans=" << plans.size() << std::endl;
		for (size_t i = 0; i < plans.size(); ++i) {
			const auto& plan = plans[i];
			std::cout << "  - plan[" << i << "] name='" << plan.name
					  << "' global_mask=0x" << std::hex << plan.global_mask
					  << " ultra_mode=" << std::dec << (plan.ultra_mode ? 1 : 0) << std::endl;
			std::cout << "      agu_ps  " << plan.agu_ps << std::endl;
			std::cout << "      agu_pd  " << plan.agu_pd << std::endl;
			std::cout << "      agu_pli " << plan.agu_pli << std::endl;
			std::cout << "      agu_plo " << plan.agu_plo << std::endl;
		}
		std::cout << "[cluster_tb] ===== End Configuration Summary =====\n" << std::endl;
	}

	// Main function to run the testbench using a configuration file path, returns true if successful, false on failure
	bool run_from_config(const std::string& path) {
		fs::path p(path);
		auto root = JsonParser::parse_file((p / "config.json").string());
		if (!root || !root->is_object()) {
			std::cerr << "[cluster_tb] failed to parse config.json: " << (p / "config.json") << std::endl;
			return false;
		}

		auto meta = (*root)["meta"];
		auto hardware = (*root)["hardware"];
		auto software = (*root)["software"];
		if (!meta || !software) {
			std::cerr << "[cluster_tb] missing meta/software in config" << std::endl;
			return false;
		}

		const bool is_ultra = (*meta)["ultra_mode"] ? (*meta)["ultra_mode"]->as_bool() : false;

		dma_timing_ = DmaTimingModel{};
		if (hardware) {
			auto dt = (*hardware)["dma_timing"];
			if (dt && dt->is_object()) {
				if ((*dt)["bandwidth_words64_per_cycle"]) {
					dma_timing_.bandwidth_words64_per_cycle = static_cast<uint32_t>((*dt)["bandwidth_words64_per_cycle"]->as_int64());
				}
				if ((*dt)["startup_latency_cycles"]) {
					dma_timing_.startup_latency_cycles = static_cast<uint32_t>((*dt)["startup_latency_cycles"]->as_int64());
				}
				if ((*dt)["burst_size_words64"]) {
					dma_timing_.burst_size_words64 = static_cast<uint32_t>((*dt)["burst_size_words64"]->as_int64());
				}
			}
		}

		auto files = (*software)["files"];
		if (!files || !files->is_object()) {
			std::cerr << "[cluster_tb] missing software.files" << std::endl;
			return false;
		}

		std::cout << "[cluster_tb] Loading binary files..." << std::endl;
		auto act = read_binary_file<uint16_t>((p / (*files)["activation"]->as_string()).string());
		auto wgt = read_binary_file<uint16_t>((p / (*files)["weight"]->as_string()).string());
		std::vector<uint16_t> psum;
		if ((*files)["partial_sum"]) psum = read_binary_file<uint16_t>((p / (*files)["partial_sum"]->as_string()).string());

		std::vector<uint16_t> gold;
		if ((*files)["output"]) gold = read_binary_file<uint16_t>((p / (*files)["output"]->as_string()).string());
		if (gold.empty() && (*files)["output_gold"]) gold = read_binary_file<uint16_t>((p / (*files)["output_gold"]->as_string()).string());

		std::vector<uint16_t> pe_prog;
		if ((*files)["pe_program"]) {
			const fs::path pe_prog_path = p / (*files)["pe_program"]->as_string();
			if (fs::exists(pe_prog_path)) pe_prog = read_binary_file<uint16_t>(pe_prog_path.string());
		}

		std::vector<uint32_t> scan_chain_data;
		if (fs::exists(p / "scan_chain.bin")) scan_chain_data = read_binary_file<uint32_t>((p / "scan_chain.bin").string());

		std::cout << "[cluster_tb] Loaded binaries: act=" << act.size()
				  << " wgt=" << wgt.size()
				  << " psum=" << psum.size()
				  << " gold=" << gold.size()
				  << " pe_prog=" << pe_prog.size()
				  << " scan_chain=" << scan_chain_data.size() << std::endl;

		uint32_t addr_act = 0x00000000;
		uint32_t addr_wgt = 0x10000000;
		uint32_t addr_psum = 0x20000000;
		uint32_t addr_out = 0x30000000;
		auto dram_map = (*software)["dram_mapping"];
		if (dram_map && dram_map->is_object()) {
			if ((*dram_map)["activation"]) addr_act = static_cast<uint32_t>((*dram_map)["activation"]->as_int64());
			if ((*dram_map)["weight"]) addr_wgt = static_cast<uint32_t>((*dram_map)["weight"]->as_int64());
			if ((*dram_map)["partial_sum"]) addr_psum = static_cast<uint32_t>((*dram_map)["partial_sum"]->as_int64());
			if ((*dram_map)["output"]) addr_out = static_cast<uint32_t>((*dram_map)["output"]->as_int64());
		}

		const uint32_t addr_out_run = cluster_json::get_spm_tensor_addr_or_default(software, "output", addr_out, is_ultra);
		const auto section_map = cluster_json::parse_spm_sections(software);
		const auto waves = cluster_json::parse_dma_waves(software, section_map);
		const auto plans = cluster_json::parse_cluster_plans(software);
		if (plans.empty()) {
			std::cerr << "[cluster_tb] no cluster_plans in config" << std::endl;
			return false;
		}

		print_loaded_config(
			meta,
			hardware,
			files,
			act,
			wgt,
			psum,
			gold,
			pe_prog,
			scan_chain_data,
			addr_act,
			addr_wgt,
			addr_psum,
			addr_out,
			section_map,
			waves,
			plans,
			is_ultra);

		std::cout << "[cluster_tb] ===== Start Simulation =====" << std::endl;

		power_on_reset();
		uint8_t current_spm_map = 0xE4;
		if (!waves.empty() && waves[0].has_spm_map) {
			current_spm_map = waves[0].spm_map_val;
		}
		std::cout << "[cluster_tb][SPM_MAP][init] map=0x" << std::hex
				  << static_cast<uint32_t>(current_spm_map) << std::dec << std::endl;
		config_spm_map(current_spm_map);
		preload_dram_shadow(addr_act, pack_to_64(act));
		preload_dram_shadow(addr_wgt, pack_to_64(wgt));
		if (!psum.empty()) preload_dram_shadow(addr_psum, pack_to_64(psum));

		if (!scan_chain_data.empty()) send_scan_chain_words_reverse(scan_chain_data);
		load_pe_program(pe_prog);

		uint32_t pending_prefetch_words = 0; // Track the number of words in the currently pending prefetch to estimate timing for compute start
		if (!waves.empty()) {
			std::cout << "[cluster_tb][PIPELINE] prefetch wave 0" << std::endl;
			pending_prefetch_words = issue_dma_stage(waves[0], cluster_json::DmaTransferCfg::Direction::DramToSpm);
		}

		for (size_t i = 0; i < plans.size(); ++i) {
			std::cout << "[cluster_tb][PIPELINE] wave-step=" << i
					  << " plan='" << plans[i].name << "'" << std::endl;
			if (i < waves.size() && pending_prefetch_words > 0) {
				const uint32_t d2s_timeout = std::max<uint32_t>(200000u, pending_prefetch_words * 128u);
				if (!wait_dma_done(waves[i].wave_id, d2s_timeout)) {
					std::cerr << "[cluster_tb] D2S timeout wave_id=" << waves[i].wave_id
							  << " words=" << pending_prefetch_words << std::endl;
					return false;
				}
			}

			pending_prefetch_words = 0;
			if (i + 1 < waves.size()) {
				std::cout << "[cluster_tb][PIPELINE] prefetch next wave=" << (i + 1) << std::endl;
				pending_prefetch_words = issue_dma_stage(waves[i + 1], cluster_json::DmaTransferCfg::Direction::DramToSpm);
			}

			if (i < waves.size()) {
				const auto& wave_cfg = waves[i];
				const uint8_t target_map = wave_cfg.has_spm_map ? wave_cfg.spm_map_val : current_spm_map;
				if (i == 0 || target_map != current_spm_map || wave_cfg.has_spm_map) {
					std::cout << "[cluster_tb][SPM_MAP][apply] wave=" << wave_cfg.wave_id
							  << " map=0x" << std::hex << static_cast<uint32_t>(target_map)
							  << " prev=0x" << static_cast<uint32_t>(current_spm_map)
							  << std::dec;
					if (!wave_cfg.spm_map_reason.empty()) {
						std::cout << " reason='" << wave_cfg.spm_map_reason << "'";
					}
					std::cout << std::endl;
					config_spm_map(target_map);
					current_spm_map = target_map;
				}
			}

			const int wave_id = (i < waves.size()) ? static_cast<int>(waves[i].wave_id) : static_cast<int>(i);
			if (!run_compute_wave(plans[i], is_ultra, wave_id, current_spm_map)) {
				std::cerr << "[cluster_tb] compute timeout/fail plan_idx=" << i
						  << " name=" << plans[i].name << std::endl;
				return false;
			}

			if (i < waves.size()) {
				std::cout << "[cluster_tb][PIPELINE] writeback wave=" << i << std::endl;
				const uint32_t s2d_words = issue_dma_stage(waves[i], cluster_json::DmaTransferCfg::Direction::SpmToDram);
				if (s2d_words > 0) {
					const uint32_t s2d_timeout = std::max<uint32_t>(200000u, s2d_words * 128u);
					if (!wait_dma_done(waves[i].wave_id, s2d_timeout)) {
						std::cerr << "[cluster_tb] S2D timeout wave_id=" << waves[i].wave_id
								  << " words=" << s2d_words << std::endl;
						return false;
					}
				}
			}
		}

		size_t out_words = (gold.size() + 3) / 4;
		bool has_output_dma_writeback = false;
		for (const auto& w : waves) {
			for (const auto& t : w.transfers) {
				if (t.tensor == "output" && t.direction == cluster_json::DmaTransferCfg::Direction::SpmToDram) {
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
			for (uint32_t i = 0; i < out_words; ++i) {
				uint64_t w = 0;
				if (!data_read_tx(addr_out_run + i * 8, w)) {
					std::cerr << "[cluster_tb] output readback failed at word " << i << std::endl;
					return false;
				}
				res_vec.push_back(w);
			}
		}

		std::vector<uint16_t> res_fp16;
		res_fp16.reserve(res_vec.size() * 4);
		for (uint64_t w : res_vec) {
			res_fp16.push_back(w & 0xFFFF);
			res_fp16.push_back((w >> 16) & 0xFFFF);
			res_fp16.push_back((w >> 32) & 0xFFFF);
			res_fp16.push_back((w >> 48) & 0xFFFF);
		}
		if (res_fp16.size() > gold.size()) res_fp16.resize(gold.size());

		auto stats = verify_fp16_vectors(gold, res_fp16, 2e-2f, true);
		std::cout << "[cluster_tb] ===== Simulation Finished =====" << std::endl;
		std::cout << "[cluster_tb] sim_cycles=" << cycle_ << "\n" << stats << std::endl;
		return stats.cosine_similarity >= 0.99f;
	}

	// Main process function that runs the testbench, waits for the clock, checks for test path, and runs the simulation from config
	void main_process() {
		wait(clk.posedge_event());
		cycle_ += 1;

		if (opt_.test_path.empty()) {
			std::cerr << "[cluster_tb] missing test path, use -d <dir>" << std::endl;
			pass_ = false;
			sc_core::sc_stop();
			return;
		}

		pass_ = run_from_config(opt_.test_path);
		sc_core::sc_stop();
	}
};

} // namespace cluster_sim

int sc_main(int argc, char* argv[]) {
	using namespace cluster_sim;
	SimOptions opt;

	for (int i = 1; i < argc; ++i) {
		const std::string a(argv[i]);
		if (a == "-d" || a == "--dir") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for -d/--dir" << std::endl;
				return 1;
			}
			opt.test_path = argv[++i];
		} else if (a == "--clock-period") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for --clock-period" << std::endl;
				return 1;
			}
			opt.clock_period_ns = std::stoi(argv[++i]);
		} else if (a == "--timeout-cycles") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for --timeout-cycles" << std::endl;
				return 1;
			}
			opt.timeout_cycles = std::stoi(argv[++i]);
		} else if (a == "--verbose" || a == "-v") {
			opt.verbose = true;
		} else if (a == "--dry-run") {
			opt.dry_run = true;
		} else if (a == "--no-dry-run") {
			opt.dry_run = false;
		} else if (a == "-f" || a == "--trace-file") {
			if (i + 1 >= argc) {
				std::cerr << "missing value for -f/--trace-file" << std::endl;
				return 1;
			}
			opt.trace_file = argv[++i];
			opt.enable_trace = true;
		} else if (a == "--help" || a == "-h") {
			std::cout << "Usage: test_cluster_sim -d <dir> [--clock-period <ns>] [--timeout-cycles <cycles>] [--verbose] [-f <trace>]" << std::endl;
			return 0;
		} else {
			std::cerr << "unknown arg: " << a << std::endl;
			return 1;
		}
	}

	Cluster_tb tb("cluster_tb", opt);
	sc_core::sc_start();
	return tb.pass_ ? 0 : 1;
}
