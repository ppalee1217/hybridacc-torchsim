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
	static constexpr uint32_t kCmdHdduBase = 0x1000;
	static constexpr uint32_t kCmdNocData = 0x2000;

	static constexpr uint32_t kHdduPlaneEn = 0x808;
	static constexpr uint32_t kHdduPlaneMode = 0x80C;
	static constexpr uint32_t kHdduMaxOutstanding = 0x818;

	sc_clock clk{"clk", 10, SC_NS};
	sc_signal<bool> reset_n;
	sc_signal<bool> power_enable_i;
	sc_signal<bool> interrupt_o;

	sc_signal<bool> data_req_vld_i;
	sc_signal<bool> data_req_rdy_o;
	sc_signal<sc_uint<32>> data_addr_i;
	sc_signal<bool> data_write_i;
	sc_signal<sc_biguint<64>> data_wdata_i;
	sc_signal<sc_biguint<64>> data_rdata_o;
	sc_signal<bool> data_done_o;

	sc_signal<bool> cmd_req_vld_i;
	sc_signal<bool> cmd_req_rdy_o;
	sc_signal<sc_uint<32>> cmd_addr_i;
	sc_signal<bool> cmd_write_i;
	sc_signal<sc_uint<32>> cmd_wdata_i;
	sc_signal<sc_uint<32>> cmd_rdata_o;
	sc_signal<bool> cmd_done_o;

	ComputeCluster<> dut;

	ClusterUnitTestBench()
		: dut("ComputeCluster_DUT", NetWorkOnChipConfig(4, 4)) {
		dut.clk(clk);
		dut.reset_n(reset_n);
		dut.power_enable_i(power_enable_i);
		dut.interrupt_o(interrupt_o);

		dut.data_req_vld_i(data_req_vld_i);
		dut.data_req_rdy_o(data_req_rdy_o);
		dut.data_addr_i(data_addr_i);
		dut.data_write_i(data_write_i);
		dut.data_wdata_i(data_wdata_i);
		dut.data_rdata_o(data_rdata_o);
		dut.data_done_o(data_done_o);

		dut.cmd_req_vld_i(cmd_req_vld_i);
		dut.cmd_req_rdy_o(cmd_req_rdy_o);
		dut.cmd_addr_i(cmd_addr_i);
		dut.cmd_write_i(cmd_write_i);
		dut.cmd_wdata_i(cmd_wdata_i);
		dut.cmd_rdata_o(cmd_rdata_o);
		dut.cmd_done_o(cmd_done_o);

		reset_n.write(false);
		power_enable_i.write(false);
		data_req_vld_i.write(false);
		data_addr_i.write(0);
		data_write_i.write(false);
		data_wdata_i.write(0);
		cmd_req_vld_i.write(false);
		cmd_addr_i.write(0);
		cmd_write_i.write(false);
		cmd_wdata_i.write(0);
	}

	void tick(int n = 1) {
		for (int i = 0; i < n; ++i) {
			sc_start(10, SC_NS);
		}
	}

	void reset_with_power_on() {
		power_enable_i.write(true);
		reset_n.write(false);
		cmd_req_vld_i.write(false);
		data_req_vld_i.write(false);
		tick(3);
		reset_n.write(true);
		tick(2);
	}

	bool cmd_write(uint32_t addr, uint32_t data, int timeout_cycles = 20) {
		cmd_addr_i.write(addr);
		cmd_write_i.write(true);
		cmd_wdata_i.write(data);
		cmd_req_vld_i.write(true);

		bool done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (cmd_done_o.read()) {
				done = true;
				break;
			}
		}

		cmd_req_vld_i.write(false);
		cmd_write_i.write(false);
		tick(1);
		return done;
	}

	bool cmd_read(uint32_t addr, uint32_t& out, int timeout_cycles = 20) {
		cmd_addr_i.write(addr);
		cmd_write_i.write(false);
		cmd_wdata_i.write(0);
		cmd_req_vld_i.write(true);

		bool done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (cmd_done_o.read()) {
				done = true;
				out = cmd_rdata_o.read().to_uint();
				break;
			}
		}

		cmd_req_vld_i.write(false);
		tick(1);
		return done;
	}

	bool cmd_read_hddu(uint32_t addr, uint32_t& out) {
		uint32_t throwaway = 0;
		if (!cmd_read(addr, throwaway)) {
			return false;
		}
		return cmd_read(addr, out);
	}

	bool data_write64(uint32_t addr, uint64_t data, int timeout_cycles = 80) {
		data_addr_i.write(addr);
		data_write_i.write(true);
		data_wdata_i.write(data);
		data_req_vld_i.write(true);

		bool done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (data_done_o.read()) {
				done = true;
				break;
			}
		}

		data_req_vld_i.write(false);
		data_write_i.write(false);
		tick(1);
		return done;
	}

	bool data_read64(uint32_t addr, uint64_t& out, int timeout_cycles = 80) {
		data_addr_i.write(addr);
		data_write_i.write(false);
		data_req_vld_i.write(true);

		bool done = false;
		for (int i = 0; i < timeout_cycles; ++i) {
			tick(1);
			if (data_done_o.read()) {
				done = true;
				out = data_rdata_o.read().to_uint64();
				break;
			}
		}

		data_req_vld_i.write(false);
		tick(1);
		return done;
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

	run("Power-on reset sets cmd ready", [&]() {
		tb.reset_with_power_on();
		const bool ok = tb.cmd_req_rdy_o.read();
		return TestResult{"", ok, std::string("cmd_req_rdy=") + (ok ? "1" : "0")};
	});

	run("Power-off drives cmd ready low", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		const bool ok = !tb.cmd_req_rdy_o.read();
		return TestResult{"", ok, std::string("cmd_req_rdy=") + (tb.cmd_req_rdy_o.read() ? "1" : "0")};
	});

	run("Power-off drives data ready low", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		const bool ok = !tb.data_req_rdy_o.read();
		return TestResult{"", ok, std::string("data_req_rdy=") + (tb.data_req_rdy_o.read() ? "1" : "0")};
	});

	run("SPM CFG_MAP MMIO write/read", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.cmd_write(ClusterUnitTestBench::kCmdSpmCfgMap, 0x93);
		uint32_t v = 0;
		bool r_ok = tb.cmd_read(ClusterUnitTestBench::kCmdSpmCfgMap, v);
		return TestResult{"", w_ok && r_ok && v == 0x93, "CFG_MAP=0x" + std::to_string(v)};
	});

	run("SPM ARB_POLICY MMIO write/read", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.cmd_write(ClusterUnitTestBench::kCmdSpmArbPolicy, 1);
		uint32_t v = 0;
		bool r_ok = tb.cmd_read(ClusterUnitTestBench::kCmdSpmArbPolicy, v);
		return TestResult{"", w_ok && r_ok && v == 1, "ARB_POLICY=" + std::to_string(v)};
	});

	run("SPM CFG_UPDATE write accepted", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.cmd_write(ClusterUnitTestBench::kCmdSpmCfgUpdate, 1);
		return TestResult{"", w_ok, std::string("cmd_done=") + (w_ok ? "1" : "0")};
	});

	run("SPM unknown offset reads zero", [&]() {
		tb.reset_with_power_on();
		uint32_t v = 123;
		bool r_ok = tb.cmd_read(0x000C, v);
		return TestResult{"", r_ok && v == 0, "SPM[0x000C]=" + std::to_string(v)};
	});

	run("HDDU PLANE_EN passthrough read/write", [&]() {
		tb.reset_with_power_on();
		const uint32_t addr = ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneEn;
		bool w_ok = tb.cmd_write(addr, 0x5);
		uint32_t v = 0;
		bool r_ok = tb.cmd_read_hddu(addr, v);
		return TestResult{"", w_ok && r_ok && ((v & 0xFFFFu) == 0x5u), "PLANE_EN=0x" + std::to_string(v)};
	});

	run("HDDU PLANE_MODE passthrough read/write", [&]() {
		tb.reset_with_power_on();
		const uint32_t addr = ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduPlaneMode;
		bool w_ok = tb.cmd_write(addr, 0x2);
		uint32_t v = 0;
		bool r_ok = tb.cmd_read_hddu(addr, v);
		return TestResult{"", w_ok && r_ok && ((v & 0xFFFFu) == 0x2u), "PLANE_MODE=0x" + std::to_string(v)};
	});

	run("HDDU MAX_OUTSTANDING passthrough", [&]() {
		tb.reset_with_power_on();
		const uint32_t addr = ClusterUnitTestBench::kCmdHdduBase + ClusterUnitTestBench::kHdduMaxOutstanding;
		bool w_ok = tb.cmd_write(addr, 12);
		uint32_t v = 0;
		bool r_ok = tb.cmd_read_hddu(addr, v);
		return TestResult{"", w_ok && r_ok && v == 12, "MAX_OUT=" + std::to_string(v)};
	});

	run("NoC command write and readback", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.cmd_write(ClusterUnitTestBench::kCmdNocData, 0x12345674);
		uint32_t v = 0;
		bool r_ok = tb.cmd_read(ClusterUnitTestBench::kCmdNocData, v);
		return TestResult{"", w_ok && r_ok && v == 0x12345674, "NOC_CMD=0x" + std::to_string(v)};
	});

	run("NoC unknown offset reads zero", [&]() {
		tb.reset_with_power_on();
		uint32_t v = 1;
		bool r_ok = tb.cmd_read(0x2004, v);
		return TestResult{"", r_ok && v == 0, "NOC[0x2004]=" + std::to_string(v)};
	});

	run("Out-of-range MMIO reads zero", [&]() {
		tb.reset_with_power_on();
		uint32_t v = 1;
		bool r_ok = tb.cmd_read(0x3000, v);
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

	run("Power-off blocks command completion", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		tb.cmd_addr_i.write(ClusterUnitTestBench::kCmdSpmCfgMap);
		tb.cmd_write_i.write(true);
		tb.cmd_wdata_i.write(0x77);
		tb.cmd_req_vld_i.write(true);
		tb.tick(3);
		bool ok = !tb.cmd_done_o.read();
		tb.cmd_req_vld_i.write(false);
		tb.cmd_write_i.write(false);
		tb.tick(1);
		return TestResult{"", ok, std::string("cmd_done=") + (tb.cmd_done_o.read() ? "1" : "0")};
	});

	run("Power-off blocks data completion", [&]() {
		tb.reset_with_power_on();
		tb.power_enable_i.write(false);
		tb.tick(1);
		tb.data_addr_i.write(0x24);
		tb.data_write_i.write(true);
		tb.data_wdata_i.write(0xAA55AA55AA55AA55ULL);
		tb.data_req_vld_i.write(true);
		tb.tick(5);
		bool ok = !tb.data_done_o.read();
		tb.data_req_vld_i.write(false);
		tb.data_write_i.write(false);
		tb.tick(1);
		return TestResult{"", ok, std::string("data_done=") + (tb.data_done_o.read() ? "1" : "0")};
	});

	run("Power-cycle resets SPM CFG_MAP", [&]() {
		tb.reset_with_power_on();
		bool w_ok = tb.cmd_write(ClusterUnitTestBench::kCmdSpmCfgMap, 0xA5);
		tb.power_enable_i.write(false);
		tb.tick(3);
		tb.power_enable_i.write(true);
		tb.tick(3);
		uint32_t v = 0;
		bool r_ok = tb.cmd_read(ClusterUnitTestBench::kCmdSpmCfgMap, v);
		return TestResult{"", w_ok && r_ok && v == 0, "CFG_MAP_after_power_cycle=0x" + std::to_string(v)};
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
