#include <systemc>
#include <iostream>
#include <iomanip>
#include <array>
#include <vector>
#include <functional>
#include <string>

#include "Cluster/HybridDataDeliverUnit.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc::cluster;

struct TestResult {
	std::string name;
	bool pass;
	std::string detail;
};

static void print_report(const std::vector<TestResult>& results, const std::string& title) {
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

class HDDUTestBench {
public:
	sc_clock clk{"clk", 10, SC_NS};
	sc_signal<bool> reset_n;

	std::array<sc_signal<sc_uint<32>>, 4> spm_addr;
	std::array<sc_signal<bool>, 4> spm_req;
	std::array<sc_signal<bool>, 4> spm_we;
	std::array<sc_signal<sc_biguint<256>>, 4> spm_wdata;
	std::array<sc_signal<sc_biguint<256>>, 4> spm_rdata;
	std::array<sc_signal<bool>, 4> spm_ready;

	std::array<sc_signal<sc_biguint<256>>, 4> noc_out_data;
	std::array<sc_signal<sc_uint<7>>, 4> noc_out_addr;
	std::array<sc_signal<bool>, 4> noc_out_valid;
	std::array<sc_signal<bool>, 4> noc_out_ready;

	sc_signal<sc_biguint<256>> noc_in3_data;
	sc_signal<sc_uint<7>> noc_in3_addr;
	sc_signal<bool> noc_in3_valid;
	sc_signal<bool> noc_in3_ready;

	sc_signal<sc_uint<32>> mmio_addr;
	sc_signal<bool> mmio_write;
	sc_signal<sc_uint<32>> mmio_wdata;
	sc_signal<sc_uint<32>> mmio_rdata;
	sc_signal<bool> interrupt;

	HybridDataDeliverUnit<> dut{"HDDU_DUT"};

	HDDUTestBench() {
		dut.clk(clk);
		dut.reset_n(reset_n);
		for (int i = 0; i < 4; ++i) {
			dut.spm_addr[i](spm_addr[i]);
			dut.spm_req[i](spm_req[i]);
			dut.spm_we[i](spm_we[i]);
			dut.spm_wdata[i](spm_wdata[i]);
			dut.spm_rdata[i](spm_rdata[i]);
			dut.spm_ready[i](spm_ready[i]);

			dut.noc_out_data[i](noc_out_data[i]);
			dut.noc_out_addr[i](noc_out_addr[i]);
			dut.noc_out_valid[i](noc_out_valid[i]);
			dut.noc_out_ready[i](noc_out_ready[i]);
		}
		dut.noc_in3_data(noc_in3_data);
		dut.noc_in3_addr(noc_in3_addr);
		dut.noc_in3_valid(noc_in3_valid);
		dut.noc_in3_ready(noc_in3_ready);
		dut.mmio_addr(mmio_addr);
		dut.mmio_write(mmio_write);
		dut.mmio_wdata(mmio_wdata);
		dut.mmio_rdata(mmio_rdata);
		dut.interrupt(interrupt);

		reset_n.write(false);
		for (int i = 0; i < 4; ++i) {
			spm_addr[i].write(0);
			spm_req[i].write(false);
			spm_we[i].write(false);
			spm_wdata[i].write(0);
			spm_rdata[i].write(0);
			spm_ready[i].write(true);

			noc_out_data[i].write(0);
			noc_out_addr[i].write(0);
			noc_out_valid[i].write(false);
			noc_out_ready[i].write(true);
		}
		noc_in3_data.write(0);
		noc_in3_addr.write(0);
		noc_in3_valid.write(false);
		noc_in3_ready.write(false);
		mmio_addr.write(0);
		mmio_write.write(false);
		mmio_wdata.write(0);
		interrupt.write(false);
	}

	void tick(int n = 1) { for (int i = 0; i < n; ++i) sc_start(10, SC_NS); }

	void reset() {
		reset_n.write(false);
		noc_in3_valid.write(false);
		mmio_write.write(false);
		for (int i = 0; i < 4; ++i) {
			spm_ready[i].write(true);
			noc_out_ready[i].write(true);
		}
		tick(3);
		reset_n.write(true);
		tick(2);
	}

	void mmio_wr(uint32_t addr, uint32_t data) {
		mmio_addr.write(addr);
		mmio_wdata.write(data);
		mmio_write.write(true);
		tick(1);
		mmio_write.write(false);
		tick(1);
	}

	uint32_t mmio_rd(uint32_t addr) {
		mmio_addr.write(addr);
		tick(1);
		return mmio_rdata.read().to_uint();
	}

	void setup_basic_agu_bank(int bank, uint32_t base, uint16_t tag_base, bool ultra = false, uint16_t iter0 = 1, uint16_t iter1 = 1, uint16_t iter2 = 1) {
		const uint32_t b = static_cast<uint32_t>(bank) * 0x100;
		const uint32_t ctrl = b + 0x20;
		const uint32_t tag = b + 0x40;
		const uint32_t base_addr = b + 0x00;
		const uint32_t iter01 = b + 0x08;
		const uint32_t iter2_reg = b + 0x10;
		const uint32_t str0 = b + 0x44;
		const uint32_t mask = b + 0x54;

		mmio_wr(base_addr, base);
		mmio_wr(iter01, (static_cast<uint32_t>(iter1) << 16) | iter0);
		mmio_wr(iter2_reg, iter2);
		mmio_wr(tag, tag_base);
		mmio_wr(str0, 1);
		mmio_wr(mask, 0xF);
		if (ultra) {
			mmio_wr(ctrl, 0x8);
			mmio_wr(ctrl, 0x9);
		} else {
			mmio_wr(ctrl, 0x1);
		}
	}

	void setup_basic_agu_bank0(uint32_t base, uint16_t tag_base, bool ultra = false, uint16_t iter0 = 1, uint16_t iter1 = 1, uint16_t iter2 = 1) {
		setup_basic_agu_bank(0, base, tag_base, ultra, iter0, iter1, iter2);
	}

	void load_spm_read_data(int port, uint32_t low32) {
		sc_biguint<256> payload = 0;
		payload.range(31, 0) = low32;
		spm_rdata[port].write(payload);
	}

	uint32_t last_noc_addr(int port) const {
		return noc_out_addr[port].read().to_uint();
	}

	bool any_noc_valid012() const {
		return noc_out_valid[0].read() || noc_out_valid[1].read() || noc_out_valid[2].read();
	}
};

int sc_main(int argc, char* argv[]) {
	(void)argc; (void)argv;

	HDDUTestBench tb;
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

	run("Reset default PLANE_EN", [&]() {
		tb.reset();
		uint32_t v = tb.mmio_rd(0x808);
		return TestResult{"", v == 0xF, "PLANE_EN=0x" + std::to_string(v)};
	});

	run("Global MMIO write/read PLANE_EN", [&]() {
		tb.reset();
		tb.mmio_wr(0x808, 0x5);
		uint32_t v = tb.mmio_rd(0x808);
		return TestResult{"", v == 0x5, "PLANE_EN=0x" + std::to_string(v)};
	});

	run("Global MMIO write/read PLANE_MODE", [&]() {
		tb.reset();
		tb.mmio_wr(0x80C, 0xB);
		uint32_t v = tb.mmio_rd(0x80C);
		return TestResult{"", v == 0xB, "PLANE_MODE=0x" + std::to_string(v)};
	});

	run("Bank0 MMIO passthrough read", [&]() {
		tb.reset();
		tb.mmio_wr(0x000, 0x1234);
		uint32_t v = tb.mmio_rd(0x000);
		return TestResult{"", v == 0x1234, "B0.BASE=0x" + std::to_string(v)};
	});

	run("Bank1 MMIO passthrough read", [&]() {
		tb.reset();
		tb.mmio_wr(0x100, 0x5678);
		uint32_t v = tb.mmio_rd(0x100);
		return TestResult{"", v == 0x5678, "B1.BASE=0x" + std::to_string(v)};
	});

	run("AGU start drives sram_read_req", [&]() {
		tb.reset();
		tb.load_spm_read_data(0, 0xA5A5A5A5);
		tb.setup_basic_agu_bank0(0x40, 0x03, false);
		tb.tick(2);
		bool req = tb.spm_req[0].read() && !tb.spm_we[0].read();
		uint32_t addr = tb.spm_addr[0].read().to_uint();
		return TestResult{"", req && addr == 0x40, std::string("req=") + (req ? "1" : "0") + ", addr=" + std::to_string(addr)};
	});

	run("Backpressure increments stall counter", [&]() {
		tb.reset();
		tb.spm_ready[0].write(false);
		tb.setup_basic_agu_bank0(0x80, 0x01, false);
		tb.tick(5);
		uint32_t stall = tb.mmio_rd(0x838);
		return TestResult{"", stall > 0, "stall=" + std::to_string(stall)};
	});

	run("SPM read data forms NoC packet", [&]() {
		tb.reset();
		sc_biguint<256> payload = 0;
		payload.range(31, 0) = 0xDEADBEEF;
		tb.spm_rdata[0].write(payload);
		tb.setup_basic_agu_bank0(0x100, 0x0A, false);
		tb.tick(2);
		tb.noc_out_ready[0].write(false);
		tb.tick(1);
		bool v = tb.noc_out_valid[0].read();
		bool ok_data = (tb.noc_out_data[0].read() == payload);
		tb.noc_out_ready[0].write(true);
		return TestResult{"", v && ok_data, std::string("valid=") + (v ? "1" : "0")};
	});

	run("NoC addr encodes tag", [&]() {
		tb.reset();
		tb.load_spm_read_data(0, 0x1234);
		tb.setup_basic_agu_bank0(0x100, 0x15, false);
		tb.tick(2);
		uint32_t a = tb.last_noc_addr(0);
		return TestResult{"", (a & 0x3F) == 0x15, "addr=0x" + std::to_string(a)};
	});

	run("NoC addr encodes ultra bit", [&]() {
		tb.reset();
		tb.load_spm_read_data(0, 0x2222);
		tb.setup_basic_agu_bank0(0x200, 0x08, true);
		tb.tick(2);
		uint32_t a = tb.last_noc_addr(0);
		bool ultra = ((a >> 6) & 0x1) != 0;
		return TestResult{"", ultra, "addr=0x" + std::to_string(a)};
	});

	run("NoC valid holds when not ready", [&]() {
		tb.reset();
		tb.noc_out_ready[0].write(false);
		tb.load_spm_read_data(0, 0x3333);
		tb.setup_basic_agu_bank0(0x300, 0x05, false);
		tb.tick(2);
		tb.tick(2);
		bool hold = tb.noc_out_valid[0].read();
		tb.noc_out_ready[0].write(true);
		return TestResult{"", hold, std::string("hold=") + (hold ? "1" : "0")};
	});

	run("NoC valid clears after handshake", [&]() {
		tb.reset();
		tb.noc_out_ready[0].write(false);
		tb.load_spm_read_data(0, 0x4444);
		tb.setup_basic_agu_bank0(0x310, 0x06, false);
		tb.tick(2);
		tb.tick(1);
		tb.noc_out_ready[0].write(true);
		tb.tick(2);
		bool cleared = !tb.noc_out_valid[0].read();
		return TestResult{"", cleared, std::string("noc_valid=") + (tb.noc_out_valid[0].read() ? "1" : "0")};
	});

	run("Arb state selects active bank", [&]() {
		tb.reset();
		tb.mmio_wr(0x100, 0x88); // bank1 active
		tb.mmio_wr(0x108, 0x00010001);
		tb.mmio_wr(0x110, 1);
		tb.mmio_wr(0x140, 0x2);
		tb.mmio_wr(0x120, 0x1); // start bank1
		tb.tick(2);
		uint32_t s = tb.dut.arb_state.read().to_uint();
		return TestResult{"", s == 1, "arb_state=" + std::to_string(s)};
	});

	run("PLO recv writes to SPM3", [&]() {
		tb.reset();
		tb.setup_basic_agu_bank(3, 0x400, 0x00, false);
		tb.tick(1);
		uint32_t rx_before = tb.mmio_rd(0x834);
		sc_biguint<256> payload = 0;
		payload.range(31, 0) = 0xCAFEBABE;
		tb.noc_in3_data.write(payload);
		int wr_seen = 0;
		int ready_seen = 0;
		for (int i = 0; i < 6; ++i) {
			tb.noc_in3_valid.write(true);
			tb.tick(1);
			if (tb.spm_req[3].read() && tb.spm_we[3].read()) wr_seen++;
			if (tb.noc_in3_ready.read()) ready_seen++;
		}
		tb.noc_in3_valid.write(false);
		tb.tick(1);
		uint32_t rx_after = tb.mmio_rd(0x834);
		bool ok = (wr_seen > 0) && (ready_seen > 0) && (rx_after > rx_before);
		return TestResult{"", ok,
			"wr_seen=" + std::to_string(wr_seen) +
			", ready_seen=" + std::to_string(ready_seen) +
			", rx_delta=" + std::to_string(rx_after - rx_before)};
	});

	run("Mixed multi-cycle stress with backpressure", [&]() {
		tb.reset();
		tb.setup_basic_agu_bank(0, 0x500, 0x01, false, 8, 1, 1);
		tb.setup_basic_agu_bank(1, 0x600, 0x02, false, 8, 1, 1);
		tb.setup_basic_agu_bank(2, 0x700, 0x03, true, 8, 1, 1);
		tb.setup_basic_agu_bank(3, 0x800, 0x00, false, 8, 1, 1);
		uint32_t rx_b_before = tb.mmio_rd(0x834);

		int seen_wr3 = 0;
		int seen_rd012 = 0;
		for (int c = 0; c < 40; ++c) {
			tb.spm_rdata[0].write(static_cast<uint32_t>(0x1000 + c));
			tb.spm_rdata[1].write(static_cast<uint32_t>(0x2000 + c));
			tb.spm_rdata[2].write(static_cast<uint32_t>(0x3000 + c));

			tb.spm_ready[0].write((c % 3) != 0);
			tb.spm_ready[1].write((c % 4) != 0);
			tb.spm_ready[2].write((c % 5) != 0);
			tb.spm_ready[3].write((c % 2) == 0);

			tb.noc_out_ready[0].write((c % 2) == 0);
			tb.noc_out_ready[1].write((c % 3) != 1);
			tb.noc_out_ready[2].write((c % 4) != 2);

			tb.noc_in3_data.write(static_cast<uint32_t>(0xABC00000u + static_cast<uint32_t>(c)));
			tb.noc_in3_valid.write((c % 3) == 0);

			tb.tick(1);
			if ((tb.spm_req[0].read() && !tb.spm_we[0].read()) ||
				(tb.spm_req[1].read() && !tb.spm_we[1].read()) ||
				(tb.spm_req[2].read() && !tb.spm_we[2].read())) {
				seen_rd012++;
			}
			if (tb.spm_req[3].read() && tb.spm_we[3].read()) {
				seen_wr3++;
			}
		}
		tb.noc_in3_valid.write(false);

		uint32_t stall = tb.mmio_rd(0x838);
		uint32_t rx_b = tb.mmio_rd(0x834);
		uint32_t rx_b_delta = rx_b - rx_b_before;

		bool ok = (seen_wr3 > 0) && (seen_rd012 > 0) && (stall > 0) && (rx_b_delta > 0);
		return TestResult{"", ok,
			"seen_rd012=" + std::to_string(seen_rd012) +
			", seen_wr3=" + std::to_string(seen_wr3) +
			", stall=" + std::to_string(stall) +
			", rx_b_delta=" + std::to_string(rx_b_delta)};
	});

	print_report(results, "HDDU Unit Test Report");

	int pass_cnt = 0;
	for (const auto& r : results) if (r.pass) pass_cnt++;
	return (pass_cnt == static_cast<int>(results.size())) ? 0 : 1;
}
