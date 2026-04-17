#include <systemc>
#include <array>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ComputeCluster.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc;

struct TestResult {
	std::string name;
	bool pass;
	std::string detail;
};

static void print_report(const std::vector<TestResult>& results, const std::string& title) {
	// set default format
	std::cout << std::dec << std::setfill(' ') << std::left;
	std::cout << "\n==========================================================================\n";
	std::cout << title << "\n";
	std::cout << "==========================================================================\n";
	std::cout << std::left << std::setw(4) << "#"
			  << std::setw(45) << "Test"
			  << std::setw(8) << "Result"
			  << "Detail\n";
	std::cout << "--------------------------------------------------------------------------\n";

	int idx = 1;
	int pass_cnt = 0;
	for (const auto& r : results) {
		if (r.pass) pass_cnt++;
		std::cout << std::left << std::setw(4) << idx++
				  << std::setw(45) << r.name
				  << std::setw(8) << (r.pass ? "PASS" : "FAIL")
				  << r.detail << "\n";
	}

	std::cout << "--------------------------------------------------------------------------\n";
	std::cout << "Summary: " << pass_cnt << " / " << results.size() << " passed\n";
	std::cout << "==========================================================================\n";
}

class ClusterUnitTestBench {
public:
	static constexpr uint32_t kCmdSpmCfgMap = 0x0000;
	static constexpr uint32_t kCmdSpmCfgUpdate = 0x0004;
	static constexpr uint32_t kCmdSpmArbPolicy = 0x0008;
	static constexpr uint32_t kCmdSpmPmuCtrl = 0x000C;
	static constexpr uint32_t kCmdSpmPmuCycleCntLo = 0x0010;
	static constexpr uint32_t kCmdSpmPmuCycleCntHi = 0x0014;
	static constexpr uint32_t kCmdSpmPmuArbStallLo = 0x0018;
	static constexpr uint32_t kCmdSpmPmuArbStallHi = 0x001C;
	static constexpr uint32_t kCmdSpmPmuCreditStallLo = 0x0020;
	static constexpr uint32_t kCmdSpmPmuCreditStallHi = 0x0024;
	static constexpr uint32_t kCmdSpmPmuPort0TxnLo = 0x0040;
	static constexpr uint32_t kCmdSpmPmuPort0TxnHi = 0x0044;
	static constexpr uint32_t kCmdSpmPmuPort1TxnLo = 0x0048;
	static constexpr uint32_t kCmdSpmPmuPort1TxnHi = 0x004C;
	static constexpr uint32_t kCmdSpmPmuPort2TxnLo = 0x0050;
	static constexpr uint32_t kCmdSpmPmuPort2TxnHi = 0x0054;
	static constexpr uint32_t kCmdSpmPmuPort3TxnLo = 0x0058;
	static constexpr uint32_t kCmdSpmPmuPort3TxnHi = 0x005C;
	static constexpr uint32_t kCmdHdduBase = 0x1000;
	static constexpr uint32_t kCmdNocData = 0x2000;
	static constexpr uint32_t kCmdNocStatus = 0x2004;
	static constexpr uint32_t kCmdClusterMode = 0x2100;
	static constexpr uint32_t kCmdClusterCtrl = 0x2104;
	static constexpr uint32_t kCmdClusterStatus = 0x2108;
	static constexpr uint32_t kCmdClusterErrorCode = 0x210C;
	static constexpr uint32_t kCmdClusterSubstate = 0x2110;

	static constexpr uint32_t kClusterModeDirectDebug = 0u;
	static constexpr uint32_t kClusterModeLayerManaged = 1u;
	static constexpr uint32_t kClusterCtrlStart = (1u << 0);
	static constexpr uint32_t kClusterCtrlStop = (1u << 1);
	static constexpr uint32_t kClusterCtrlSoftReset = (1u << 2);
	static constexpr uint32_t kClusterStatusIdle = (1u << 0);
	static constexpr uint32_t kClusterStatusBusy = (1u << 1);
	static constexpr uint32_t kClusterStatusDone = (1u << 2);
	static constexpr uint32_t kClusterStatusQuiesced = (1u << 3);
	static constexpr uint32_t kNocStatusAllActivePesHalted = (1u << 1);
	static constexpr uint32_t kNocCmdReset = 0u;
	static constexpr uint32_t kNocCmdStopPe = 3u;
	static constexpr uint32_t kNocCmdStartPe = 4u;

	static constexpr uint32_t kHdduPlaneEn = 0x808;
	static constexpr uint32_t kHdduPlaneMode = 0x80C;
	static constexpr uint32_t kHdduCtrl = 0x800;
	static constexpr uint32_t kHdduCtrlStart     = (1u << 0); // aligned with HdduCtrlBit::START
	static constexpr uint32_t kHdduCtrlStop      = (1u << 1); // aligned with HdduCtrlBit::STOP
	static constexpr uint32_t kHdduCtrlSoftReset = (1u << 2); // aligned with HdduCtrlBit::SOFT_RESET
	static constexpr uint32_t kHdduMaxOutstanding = 0x818;

	static constexpr uint32_t kHdduAguBankStride = 0x100;
	static constexpr uint32_t kHdduRegBaseAddr = 0x00;
	static constexpr uint32_t kHdduRegIter01 = 0x08;
	static constexpr uint32_t kHdduRegIter23 = 0x0C;
	static constexpr uint32_t kHdduRegCtrl = 0x20;
	static constexpr uint32_t kHdduRegTagBase = 0x40;
	static constexpr uint32_t kHdduRegStride0 = 0x10;
	static constexpr uint32_t kHdduRegTagStride0 = 0x44;
	static constexpr uint32_t kHdduRegMaskCfg = 0x54;

	sc_clock clk{"clk", 10, SC_NS};
	sc_signal<bool> reset_n;
	sc_signal<bool> power_enable_i;
	sc_signal<bool> interrupt_o;

	sc_signal<bool> s_axi_awvalid_i;
	sc_signal<bool> s_axi_awready_o;
	sc_signal<sc_uint<32>> s_axi_awaddr_i;
	sc_signal<bool> s_axi_wvalid_i;
	sc_signal<bool> s_axi_wready_o;
	sc_signal<sc_biguint<64>> s_axi_wdata_i;
	sc_signal<sc_uint<8>> s_axi_wstrb_i;
	sc_signal<bool> s_axi_bvalid_o;
	sc_signal<bool> s_axi_bready_i;
	sc_signal<sc_uint<2>> s_axi_bresp_o;
	sc_signal<bool> s_axi_arvalid_i;
	sc_signal<bool> s_axi_arready_o;
	sc_signal<sc_uint<32>> s_axi_araddr_i;
	sc_signal<bool> s_axi_rvalid_o;
	sc_signal<bool> s_axi_rready_i;
	sc_signal<sc_biguint<64>> s_axi_rdata_o;
	sc_signal<sc_uint<2>> s_axi_rresp_o;

	sc_signal<bool> hsel_i;
	sc_signal<sc_uint<32>> haddr_i;
	sc_signal<bool> hwrite_i;
	sc_signal<sc_uint<2>> htrans_i;
	sc_signal<sc_uint<3>> hsize_i;
	sc_signal<sc_uint<3>> hburst_i;
	sc_signal<sc_uint<4>> hprot_i;
	sc_signal<bool> hready_i;
	sc_signal<sc_uint<32>> hwdata_i;
	sc_signal<bool> hready_o;
	sc_signal<bool> hresp_o;
	sc_signal<sc_uint<32>> hrdata_o;

	ComputeCluster<> dut;

	ClusterUnitTestBench()
		: dut("ComputeCluster_DUT", NetWorkOnChipConfig(4, 4)) {
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
	}

	void tick(int n = 1) {
		for (int i = 0; i < n; ++i) {
			sc_start(10, SC_NS);
		}
	}

	void reset_with_power_on() {
		power_enable_i.write(true);
		reset_n.write(false);
		hsel_i.write(false);
		htrans_i.write(0);
		hwrite_i.write(false);
		hwdata_i.write(0);
		s_axi_awvalid_i.write(false);
		s_axi_wvalid_i.write(false);
		s_axi_bready_i.write(false);
		s_axi_arvalid_i.write(false);
		s_axi_rready_i.write(false);
		tick(3);
		reset_n.write(true);
		tick(2);
	}

	bool ahb_write(uint32_t addr, uint32_t data, int timeout_cycles = 20) {
		if (!hready_o.read()) return false;

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

	bool ahb_read(uint32_t addr, uint32_t& out, int timeout_cycles = 20) {
		if (!hready_o.read()) return false;

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

	bool data_write64(uint32_t addr, uint64_t data, int timeout_cycles = 80) {
		s_axi_awaddr_i.write(addr);
		s_axi_awvalid_i.write(true);
		s_axi_wdata_i.write(data);
		s_axi_wstrb_i.write(0xFF);
		s_axi_wvalid_i.write(true);
		s_axi_bready_i.write(true);

		bool aw_done = false;
		bool w_done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (!aw_done && s_axi_awready_o.read()) {
				aw_done = true;
				s_axi_awvalid_i.write(false);
			}
			if (!w_done && s_axi_wready_o.read()) {
				w_done = true;
				s_axi_wvalid_i.write(false);
			}
			if (aw_done && w_done && s_axi_bvalid_o.read()) {
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

	bool data_read64(uint32_t addr, uint64_t& out, int timeout_cycles = 80) {
		s_axi_araddr_i.write(addr);
		s_axi_arvalid_i.write(true);
		s_axi_rready_i.write(true);

		bool ar_done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (!ar_done && s_axi_arready_o.read()) {
				ar_done = true;
				s_axi_arvalid_i.write(false);
			}
			if (ar_done && s_axi_rvalid_o.read()) {
				out = s_axi_rdata_o.read().to_uint64();
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
};

int sc_main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	ClusterUnitTestBench tb;
	std::vector<TestResult> results;

	auto run = [&](const std::string& name, const std::function<TestResult()>& fn) {
		try {
			results.push_back(fn());
			results.back().name = name;
		} catch (const std::exception& e) {
			results.push_back({name, false, std::string("Exception: ") + e.what()});
		} catch (...) {
			results.push_back({name, false, "Unknown exception"});
		}
	};

	run("Power-on reset sets AHB ready", [&]() {
		tb.reset_with_power_on();
		const bool ok = tb.hready_o.read();
		return TestResult{"", ok, std::string("hready=") + (ok ? "1" : "0")};
	});

	run("Power-off drives AHB ready low", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		const bool ok = !tb.hready_o.read();
		return TestResult{"", ok, std::string("hready=") + (tb.hready_o.read() ? "1" : "0")};
	});

	run("Power-off drives data ready low", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		const bool ok = !tb.s_axi_awready_o.read() && !tb.s_axi_wready_o.read() && !tb.s_axi_arready_o.read();
		return TestResult{"", ok, std::string("awready=") + (tb.s_axi_awready_o.read() ? "1" : "0")};
	});

	run("SPM CFG_MAP MMIO write/read", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdSpmCfgMap, 0x93);
		uint32_t v = 0;
		bool r_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmCfgMap, v);
		return TestResult{"", w_ok && r_ok && v == 0x93, "CFG_MAP=0x" + std::to_string(v)};
	});

	run("SPM ARB_POLICY MMIO write/read", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdSpmArbPolicy, 1);
		uint32_t v = 0;
		bool r_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmArbPolicy, v);
		return TestResult{"", w_ok && r_ok && v == 1, "ARB_POLICY=" + std::to_string(v)};
	});

	run("SPM CFG_UPDATE write accepted", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdSpmCfgUpdate, 1);
		return TestResult{"", w_ok, std::string("ahb_write=") + (w_ok ? "1" : "0")};
	});

	run("SPM PMU cycle counter MMIO increments", [&]() {
		tb.reset_with_power_on();
		uint32_t c0 = 0;
		uint32_t c1 = 0;
		bool r0_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuCycleCntLo, c0);
		tb.tick(5);
		bool r1_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuCycleCntLo, c1);
		const bool ok = r0_ok && r1_ok && (c1 > c0);
		return TestResult{"", ok, "cycle_lo_before=" + std::to_string(c0) + ", after=" + std::to_string(c1)};
	});

	run("SPM PMU counter windows MMIO readable", [&]() {
		tb.reset_with_power_on();
		uint32_t cycle_hi = 0;
		uint32_t arb_lo = 0;
		uint32_t arb_hi = 0;
		uint32_t credit_lo = 0;
		uint32_t credit_hi = 0;
		uint32_t port0_lo = 0;
		uint32_t port0_hi = 0;

		bool ok = true;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuCycleCntHi, cycle_hi);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuArbStallLo, arb_lo);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuArbStallHi, arb_hi);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuCreditStallLo, credit_lo);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuCreditStallHi, credit_hi);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort0TxnLo, port0_lo);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort0TxnHi, port0_hi);

		const bool values_ok = (arb_lo == 0) && (arb_hi == 0) && (credit_lo == 0) && (credit_hi == 0)
			&& (port0_lo == 0) && (port0_hi == 0);
		return TestResult{"", ok && values_ok,
			"cycle_hi=" + std::to_string(cycle_hi)
			+ ", arb_lo=" + std::to_string(arb_lo)
			+ ", credit_lo=" + std::to_string(credit_lo)
			+ ", port0_lo=" + std::to_string(port0_lo)};
	});

	run("SPM PMU control reset via MMIO", [&]() {
		tb.reset_with_power_on();
		tb.tick(8);
		uint32_t before = 0;
		bool rb_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuCycleCntLo, before);
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdSpmPmuCtrl, 1);
		tb.tick(1);
		uint32_t after = 0;
		bool ra_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuCycleCntLo, after);
		const bool reset_effect = (before > 4U) ? (after < before) : (after <= before);
		return TestResult{"", rb_ok && w_ok && ra_ok && reset_effect,
			"cycle_before=" + std::to_string(before) + ", after_reset=" + std::to_string(after)};
	});

	run("SPM PMU port0 txn counter increments after HDDU AGU start", [&]() {
		tb.reset_with_power_on();

		uint32_t pmu_before = 0;
		bool rb_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort0TxnLo, pmu_before);

		const uint32_t b0 = ClusterUnitTestBench::kCmdHdduBase + 0u * ClusterUnitTestBench::kHdduAguBankStride;
		bool ok = rb_ok;
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduCtrl, ClusterUnitTestBench::kHdduCtrlStart);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, 0x1u);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneMode, 0x0u);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduMaxOutstanding, 4u);

		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegBaseAddr, 0x40u);
		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegIter01, (1u << 16) | 1u);
		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegIter23, (1u << 16) | 1u);
		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegTagBase, 0x03u);
		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegStride0, 1u);
		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegTagStride0, 1u);
		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegMaskCfg, 0xFu);
		ok = ok && tb.ahb_write(b0 + ClusterUnitTestBench::kHdduRegCtrl, 0x1u);

		uint32_t pmu_after = pmu_before;
		bool increased = false;
		for (int i = 0; i < 80; ++i) {
			tb.tick(1);
			uint32_t v = 0;
			if (!tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort0TxnLo, v)) {
				ok = false;
				break;
			}
			pmu_after = v;
			if (pmu_after > pmu_before) {
				increased = true;
				break;
			}
		}

		return TestResult{"", ok && increased,
			"port0_txn_before=" + std::to_string(pmu_before) + ", after=" + std::to_string(pmu_after)};
	});

	run("SPM PMU port2 txn counter increments after HDDU AGU start", [&]() {
		tb.reset_with_power_on();

		uint32_t pmu_before = 0;
		bool rb_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort2TxnLo, pmu_before);

		const uint32_t b2 = ClusterUnitTestBench::kCmdHdduBase + 2u * ClusterUnitTestBench::kHdduAguBankStride;
		bool ok = rb_ok;
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduCtrl, ClusterUnitTestBench::kHdduCtrlStart);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, 0x4u);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneMode, 0x0u);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduMaxOutstanding, 4u);

		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegBaseAddr, 0x80u);
		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegIter01, (1u << 16) | 1u);
		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegIter23, (1u << 16) | 1u);
		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegTagBase, 0x07u);
		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegStride0, 1u);
		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegTagStride0, 1u);
		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegMaskCfg, 0xFu);
		ok = ok && tb.ahb_write(b2 + ClusterUnitTestBench::kHdduRegCtrl, 0x1u);

		uint32_t pmu_after = pmu_before;
		bool increased = false;
		for (int i = 0; i < 80; ++i) {
			tb.tick(1);
			uint32_t v = 0;
			if (!tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort2TxnLo, v)) {
				ok = false;
				break;
			}
			pmu_after = v;
			if (pmu_after > pmu_before) {
				increased = true;
				break;
			}
		}

		return TestResult{"", ok && increased,
			"port2_txn_before=" + std::to_string(pmu_before) + ", after=" + std::to_string(pmu_after)};
	});

	run("SPM PMU all 4 port counters under concurrent enable", [&]() {
		tb.reset_with_power_on();

		const std::array<uint32_t, 4> port_txn_lo_addr = {
			ClusterUnitTestBench::kCmdSpmPmuPort0TxnLo,
			ClusterUnitTestBench::kCmdSpmPmuPort1TxnLo,
			ClusterUnitTestBench::kCmdSpmPmuPort2TxnLo,
			ClusterUnitTestBench::kCmdSpmPmuPort3TxnLo,
		};

		std::array<uint32_t, 4> before = {0, 0, 0, 0};
		std::array<uint32_t, 4> after = {0, 0, 0, 0};
		std::array<bool, 4> increased = {false, false, false, false};

		bool ok = true;
		for (size_t p = 0; p < 4; ++p) {
			ok = ok && tb.ahb_read(port_txn_lo_addr[p], before[p]);
			after[p] = before[p];
		}

		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduCtrl, ClusterUnitTestBench::kHdduCtrlStart);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, 0xFu);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneMode, 0x0u);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduMaxOutstanding, 8u);

		for (uint32_t bank = 0; bank < 4; ++bank) {
			const uint32_t b = ClusterUnitTestBench::kCmdHdduBase + bank * ClusterUnitTestBench::kHdduAguBankStride;
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegBaseAddr, 0x100u + bank * 0x40u);
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegIter01, (1u << 16) | 1u);
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegIter23, (1u << 16) | 1u);
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegTagBase, 0x10u + bank);
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegStride0, 1u);
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegTagStride0, 1u);
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegMaskCfg, 0xFu);
			ok = ok && tb.ahb_write(b + ClusterUnitTestBench::kHdduRegCtrl, 0x1u);
		}

		for (int cycle = 0; cycle < 160; ++cycle) {
			tb.tick(1);
			for (size_t p = 0; p < 4; ++p) {
				uint32_t v = 0;
				if (!tb.ahb_read(port_txn_lo_addr[p], v)) {
					ok = false;
					continue;
				}
				after[p] = v;
				if (after[p] > before[p]) {
					increased[p] = true;
				}
			}
			if (increased[0] && increased[1] && increased[2] && increased[3]) {
				break;
			}
		}

		const bool expected_behavior = increased[0] && increased[1] && increased[2] && (after[3] == before[3]);
		return TestResult{"", ok && expected_behavior,
			"delta=["
			+ std::to_string(after[0] - before[0]) + ","
			+ std::to_string(after[1] - before[1]) + ","
			+ std::to_string(after[2] - before[2]) + ","
			+ std::to_string(after[3] - before[3]) + "]"};
	});

	run("SPM PMU port3 requires PLO response path", [&]() {
		tb.reset_with_power_on();

		uint32_t p3_before = 0;
		bool ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort3TxnLo, p3_before);

		const uint32_t b3 = ClusterUnitTestBench::kCmdHdduBase + 3u * ClusterUnitTestBench::kHdduAguBankStride;
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduCtrl, ClusterUnitTestBench::kHdduCtrlStart);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, 0x8u);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduRegBaseAddr + 3u * ClusterUnitTestBench::kHdduAguBankStride, 0x200u);
		ok = ok && tb.ahb_write(b3 + ClusterUnitTestBench::kHdduRegIter01, (1u << 16) | 1u);
		ok = ok && tb.ahb_write(b3 + ClusterUnitTestBench::kHdduRegIter23, (1u << 16) | 1u);
		ok = ok && tb.ahb_write(b3 + ClusterUnitTestBench::kHdduRegTagBase, 0x21u);
		ok = ok && tb.ahb_write(b3 + ClusterUnitTestBench::kHdduRegStride0, 1u);
		ok = ok && tb.ahb_write(b3 + ClusterUnitTestBench::kHdduRegTagStride0, 1u);
		ok = ok && tb.ahb_write(b3 + ClusterUnitTestBench::kHdduRegMaskCfg, 0xFu);
		ok = ok && tb.ahb_write(b3 + ClusterUnitTestBench::kHdduRegCtrl, 0x1u);

		tb.tick(120);
		uint32_t p3_after = p3_before;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmPmuPort3TxnLo, p3_after);

		return TestResult{"", ok && (p3_after == p3_before),
			"port3_txn_before=" + std::to_string(p3_before) + ", after=" + std::to_string(p3_after)};
	});

	run("SPM unknown offset reads zero", [&]() {
		tb.reset_with_power_on();
		uint32_t v = 123;
		bool r_ok = tb.ahb_read(0x0028, v);
		return TestResult{"", r_ok && v == 0, "SPM[0x0028]=" + std::to_string(v)};
	});

	run("HDDU PLANE_EN passthrough read/write", [&]() {
		tb.reset_with_power_on();
		const uint32_t addr = ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn;
		bool w_ok = tb.ahb_write(addr, 0x5);
		uint32_t v = 0;
		bool r_ok = tb.ahb_read(addr, v);
		return TestResult{"", w_ok && r_ok && ((v & 0xFFFFu) == 0x5u), "PLANE_EN=0x" + std::to_string(v)};
	});

	run("HDDU PLANE_MODE passthrough read/write", [&]() {
		tb.reset_with_power_on();
		const uint32_t addr = ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneMode;
		bool w_ok = tb.ahb_write(addr, 0x2);
		uint32_t v = 0;
		bool r_ok = tb.ahb_read(addr, v);
		return TestResult{"", w_ok && r_ok && ((v & 0xFFFFu) == 0x2u), "PLANE_MODE=0x" + std::to_string(v)};
	});

	run("NoC command write and readback", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdNocData, 0x12345674);
		uint32_t v = 0;
		bool r_ok = tb.ahb_read(ClusterUnitTestBench::kCmdNocData, v);
		return TestResult{"", w_ok && r_ok && v == 0x12345674, "NOC_CMD=0x" + std::to_string(v)};
	});

	run("NoC status read exposes halted bit", [&]() {
		tb.reset_with_power_on();
		uint32_t v = 1;
		bool r_ok = tb.ahb_read(ClusterUnitTestBench::kCmdNocStatus, v);
		return TestResult{"", r_ok && ((v & ClusterUnitTestBench::kNocStatusAllActivePesHalted) != 0u),
			"NOC_STATUS=0x" + std::to_string(v)};
	});

	run("Cluster mode register write/read", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);
		uint32_t v = 0;
		bool r_ok = tb.ahb_read(ClusterUnitTestBench::kCmdClusterMode, v);
		return TestResult{"", w_ok && r_ok && v == ClusterUnitTestBench::kClusterModeLayerManaged,
			"CLUSTER_MODE=" + std::to_string(v)};
	});

	run("Layer-managed cluster start preserves direct SPM/HDDU writes", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);

		uint32_t noc_cmd = 0;
		uint32_t cluster_status = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdNocData, noc_cmd);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, cluster_status);

		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdSpmCfgMap, 0x5A);
		uint32_t cfg_map = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmCfgMap, cfg_map);

		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, 0x3u);
		uint32_t plane_en = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, plane_en);

		const bool cluster_started = ((noc_cmd & 0xFu) == ClusterUnitTestBench::kNocCmdStartPe)
			&& ((cluster_status & ClusterUnitTestBench::kClusterStatusBusy) != 0u);
		const bool direct_ok = (cfg_map == 0x5Au) && ((plane_en & 0xFFFFu) == 0x3u);
		return TestResult{"", ok && cluster_started && direct_ok,
			"noc_cmd=0x" + std::to_string(noc_cmd)
			+ ", cluster_status=0x" + std::to_string(cluster_status)
			+ ", cfg_map=0x" + std::to_string(cfg_map)};
	});

	run("Layer-managed cluster stop reaches done and quiesced", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStop);

		uint32_t noc_cmd = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdNocData, noc_cmd);

		uint32_t cluster_status = 0;
		bool reached = false;
		for (int i = 0; i < 8 && ok; ++i) {
			tb.tick(1);
			ok = tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, cluster_status);
			const uint32_t required = ClusterUnitTestBench::kClusterStatusDone
				| ClusterUnitTestBench::kClusterStatusQuiesced;
			if ((cluster_status & required) == required) {
				reached = true;
				break;
			}
		}

		return TestResult{"", ok && ((noc_cmd & 0xFu) == ClusterUnitTestBench::kNocCmdStopPe) && reached,
			"noc_cmd=0x" + std::to_string(noc_cmd)
			+ ", cluster_status=0x" + std::to_string(cluster_status)};
	});

	run("Layer-managed cluster soft reset issues NOC reset", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlSoftReset);

		uint32_t noc_cmd = 0;
		uint32_t cluster_status = 0;
		bool reached = false;
		for (int i = 0; i < 8 && ok; ++i) {
			tb.tick(1);
			ok = tb.ahb_read(ClusterUnitTestBench::kCmdNocData, noc_cmd);
			ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, cluster_status);
			const uint32_t required = ClusterUnitTestBench::kClusterStatusIdle
				| ClusterUnitTestBench::kClusterStatusDone;
			if (((noc_cmd & 0xFu) == ClusterUnitTestBench::kNocCmdReset)
				&& ((cluster_status & required) == required)) {
				reached = true;
				break;
			}
		}

		return TestResult{"", ok && reached,
			"noc_cmd=0x" + std::to_string(noc_cmd)
			+ ", cluster_status=0x" + std::to_string(cluster_status)};
	});

	run("Out-of-range MMIO reads zero", [&]() {
		tb.reset_with_power_on();
		uint32_t v = 1;
		bool r_ok = tb.ahb_read(0x3000, v);
		return TestResult{"", r_ok && v == 0, "MMIO[0x3000]=" + std::to_string(v)};
	});

	run("Data slave 64-bit write/read roundtrip", [&]() {
		tb.reset_with_power_on();
		const uint32_t addr = 0x40;
		const uint64_t pattern = 0x1122334455667788ULL;
		bool w_ok = tb.data_write64(addr, pattern);
		uint64_t rv = 0;
		bool r_ok = tb.data_read64(addr, rv);
		return TestResult{"", w_ok && r_ok && rv == pattern, "RDATA=0x" + std::to_string(rv)};
	});

	run("Power-off blocks AHB writes", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdSpmCfgMap, 0x77, 2);
		return TestResult{"", !w_ok, std::string("ahb_write=") + (w_ok ? "1" : "0")};
	});

	run("Power-off blocks data completion", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		tb.s_axi_awaddr_i.write(0x24);
		tb.s_axi_awvalid_i.write(true);
		tb.s_axi_wdata_i.write(0xAA55AA55AA55AA55ULL);
		tb.s_axi_wstrb_i.write(0xFF);
		tb.s_axi_wvalid_i.write(true);
		tb.s_axi_bready_i.write(true);
		tb.tick(5);
		bool ok = !tb.s_axi_bvalid_o.read();
		tb.s_axi_awvalid_i.write(false);
		tb.s_axi_wvalid_i.write(false);
		tb.s_axi_bready_i.write(false);
		tb.tick(1);
		return TestResult{"", ok, std::string("bvalid=") + (tb.s_axi_bvalid_o.read() ? "1" : "0")};
	});

	run("Power-cycle resets SPM CFG_MAP", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.ahb_write(ClusterUnitTestBench::kCmdSpmCfgMap, 0xA5);
		tb.power_enable_i.write(false);
		tb.tick(3);
		tb.power_enable_i.write(true);
		tb.tick(3);
		uint32_t v = 0;
		bool r_ok = tb.ahb_read(ClusterUnitTestBench::kCmdSpmCfgMap, v);
		return TestResult{"", w_ok && r_ok && v == 0, "CFG_MAP_after_power_cycle=0x" + std::to_string(v)};
	});

	// ── Multi-layer lifecycle and mixed-control scenarios ──

	run("Multi-layer: START->STOP->quiesce->SOFT_RESET->START", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);

		// Layer 0: START
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);
		uint32_t st = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, st);
		bool layer0_busy = (st & ClusterUnitTestBench::kClusterStatusBusy) != 0;

		// Layer 0: STOP
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStop);

		// Wait for DONE+QUIESCED
		bool stop_done = false;
		for (int i = 0; i < 16 && ok; ++i) {
			tb.tick(1);
			ok = tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, st);
			if ((st & (ClusterUnitTestBench::kClusterStatusDone | ClusterUnitTestBench::kClusterStatusQuiesced))
				== (ClusterUnitTestBench::kClusterStatusDone | ClusterUnitTestBench::kClusterStatusQuiesced)) {
				stop_done = true;
				break;
			}
		}

		// SOFT_RESET
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlSoftReset);
		bool reset_done = false;
		for (int i = 0; i < 8 && ok; ++i) {
			tb.tick(1);
			ok = tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, st);
			uint32_t noc_cmd = 0;
			tb.ahb_read(ClusterUnitTestBench::kCmdNocData, noc_cmd);
			if ((noc_cmd & 0xFu) == ClusterUnitTestBench::kNocCmdReset
				&& (st & ClusterUnitTestBench::kClusterStatusDone)) {
				reset_done = true;
				break;
			}
		}

		// Layer 1: START again
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, st);
		bool layer1_busy = (st & ClusterUnitTestBench::kClusterStatusBusy) != 0;

		return TestResult{"", ok && layer0_busy && stop_done && reset_done && layer1_busy,
			"l0_busy=" + std::to_string(layer0_busy) + " stop_done=" + std::to_string(stop_done)
			+ " reset_done=" + std::to_string(reset_done) + " l1_busy=" + std::to_string(layer1_busy)};
	});

	run("Repeated START/STOP across two layers", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);

		bool both_ok = true;
		for (int layer = 0; layer < 2; ++layer) {
			ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
				ClusterUnitTestBench::kClusterCtrlStart);
			uint32_t st = 0;
			ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, st);
			both_ok = both_ok && ((st & ClusterUnitTestBench::kClusterStatusBusy) != 0);

			ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
				ClusterUnitTestBench::kClusterCtrlStop);
			bool done = false;
			for (int i = 0; i < 16 && ok; ++i) {
				tb.tick(1);
				ok = tb.ahb_read(ClusterUnitTestBench::kCmdClusterStatus, st);
				if ((st & ClusterUnitTestBench::kClusterStatusDone) != 0) {
					done = true;
					break;
				}
			}
			both_ok = both_ok && done;
		}

		return TestResult{"", ok && both_ok, "both_layers_ok=" + std::to_string(both_ok)};
	});

	run("Cluster START preserves direct HDDU/AGU/SPM writes", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);

		// Write SPM cfg_map, HDDU plane_en, and an AGU register BEFORE cluster START
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdSpmCfgMap, 0xBB);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, 0x5u);

		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);

		// Verify registers survived cluster START
		uint32_t cfg_map = 0, plane_en = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdSpmCfgMap, cfg_map);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn, plane_en);

		return TestResult{"", ok && cfg_map == 0xBBu && (plane_en & 0xFFFF) == 0x5u,
			"cfg_map=0x" + std::to_string(cfg_map) + " plane_en=0x" + std::to_string(plane_en & 0xFFFF)};
	});

	run("Substate transitions: IDLE->RUNNING->(STOPPING)->IDLE", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);

		uint32_t ss = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterSubstate, ss);
		bool idle0 = (ss == static_cast<uint32_t>(cluster::ClusterSubstate::IDLE));

		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterSubstate, ss);
		bool running = (ss == static_cast<uint32_t>(cluster::ClusterSubstate::RUNNING));

		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStop);
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterSubstate, ss);
		// With plain-variable ClusterControlUnit, background_tick may resolve
		// STOPPING→IDLE in the same cycle if NoC is already quiesced.
		bool stopping_or_idle = (ss == static_cast<uint32_t>(cluster::ClusterSubstate::STOPPING)
			|| ss == static_cast<uint32_t>(cluster::ClusterSubstate::IDLE));

		// Eventually reaches IDLE
		bool back_idle = false;
		for (int i = 0; i < 16 && ok; ++i) {
			ok = tb.ahb_read(ClusterUnitTestBench::kCmdClusterSubstate, ss);
			if (ss == static_cast<uint32_t>(cluster::ClusterSubstate::IDLE)) {
				back_idle = true;
				break;
			}
			tb.tick(1);
		}

		return TestResult{"", ok && idle0 && running && stopping_or_idle && back_idle,
			"idle0=" + std::to_string(idle0) + " running=" + std::to_string(running)
			+ " stop_or_idle=" + std::to_string(stopping_or_idle) + " back_idle=" + std::to_string(back_idle)};
	});

	run("SOFT_RESET when active reaches IDLE via quiesce", [&]() {
		tb.reset_with_power_on();
		bool ok = tb.ahb_write(ClusterUnitTestBench::kCmdClusterMode,
			ClusterUnitTestBench::kClusterModeLayerManaged);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlStart);
		ok = ok && tb.ahb_write(ClusterUnitTestBench::kCmdClusterCtrl,
			ClusterUnitTestBench::kClusterCtrlSoftReset);

		uint32_t ss = 0;
		ok = ok && tb.ahb_read(ClusterUnitTestBench::kCmdClusterSubstate, ss);
		// May be WAIT_QUIESCED or already resolved to IDLE
		bool wait_q_or_idle = (ss == static_cast<uint32_t>(cluster::ClusterSubstate::WAIT_QUIESCED)
			|| ss == static_cast<uint32_t>(cluster::ClusterSubstate::IDLE));

		// Eventually returns to IDLE
		bool back_idle = false;
		for (int i = 0; i < 16 && ok; ++i) {
			ok = tb.ahb_read(ClusterUnitTestBench::kCmdClusterSubstate, ss);
			if (ss == static_cast<uint32_t>(cluster::ClusterSubstate::IDLE)) {
				back_idle = true;
				break;
			}
			tb.tick(1);
		}

		return TestResult{"", ok && wait_q_or_idle && back_idle,
			"wq_or_idle=" + std::to_string(wait_q_or_idle) + " back_idle=" + std::to_string(back_idle)};
	});

	print_report(results, "ComputeCluster Unit Tests (Basic Debug/Check)");

	bool all_pass = true;
	for (const auto& r : results) {
		if (!r.pass) {
			all_pass = false;
			break;
		}
	}

	return all_pass ? 0 : 1;
}
