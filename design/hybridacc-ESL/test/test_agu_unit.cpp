#include <systemc>
#include <iostream>
#include <iomanip>
#include <vector>
#include <functional>
#include <string>
#include <sstream>

#include "Cluster/AddressGenerateUnit.hpp"

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

class AGUTestBench {
public:
	sc_clock clk{"clk", 10, SC_NS};
	sc_signal<bool> reset_n;

	sc_signal<bool> cfg_write;
	sc_signal<sc_uint<8>> cfg_addr;
	sc_signal<sc_uint<32>> cfg_wdata;
	sc_signal<sc_uint<32>> cfg_rdata;

	sc_signal<bool> start;
	sc_signal<bool> stop;

	sc_signal<bool> gen_valid;
	sc_signal<bool> gen_ready;
	sc_signal<sc_uint<32>> gen_addr;
	sc_signal<sc_uint<16>> gen_tag;
	sc_signal<bool> gen_ultra;
	sc_signal<sc_uint<16>> gen_mask;

	sc_signal<bool> busy;
	sc_signal<bool> done;
	sc_signal<sc_uint<2>> fsm_state;

	AddressGenerateUnit dut{"AGU_DUT"};

	AGUTestBench() {
		dut.clk(clk);
		dut.reset_n(reset_n);
		dut.cfg_write(cfg_write);
		dut.cfg_addr(cfg_addr);
		dut.cfg_wdata(cfg_wdata);
		dut.cfg_rdata(cfg_rdata);
		dut.start(start);
		dut.stop(stop);
		dut.gen_valid(gen_valid);
		dut.gen_ready(gen_ready);
		dut.gen_addr(gen_addr);
		dut.gen_tag(gen_tag);
		dut.gen_ultra(gen_ultra);
		dut.gen_mask(gen_mask);
		dut.busy(busy);
		dut.done(done);
		dut.fsm_state(fsm_state);

		cfg_write.write(false);
		cfg_addr.write(0);
		cfg_wdata.write(0);
		start.write(false);
		stop.write(false);
		gen_ready.write(true);
		reset_n.write(false);
	}

	void tick(int n = 1) {
		for (int i = 0; i < n; ++i) sc_start(10, SC_NS);
	}

	void reset() {
		reset_n.write(false);
		cfg_write.write(false);
		start.write(false);
		stop.write(false);
		gen_ready.write(true);
		tick(3);
		reset_n.write(true);
		tick(2);
	}

	void mmio_wr(uint8_t addr, uint32_t data) {
		cfg_addr.write(addr);
		cfg_wdata.write(data);
		cfg_write.write(true);
		tick(1);
		cfg_write.write(false);
		tick(1);
	}

	uint32_t mmio_rd(uint8_t addr) {
		cfg_addr.write(addr);
		tick(1);
		return cfg_rdata.read().to_uint();
	}

	void pulse_start() {
		start.write(true);
		tick(1);
		start.write(false);
		tick(1);
	}

	void pulse_stop() {
		stop.write(true);
		tick(1);
		stop.write(false);
		tick(1);
	}

	bool wait_until(const std::function<bool()>& pred, int max_cycles = 20) {
		for (int i = 0; i < max_cycles; ++i) {
			if (pred()) return true;
			tick(1);
		}
		return pred();
	}
};

int sc_main(int argc, char* argv[]) {
	(void)argc; (void)argv;

	AGUTestBench tb;
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

	run("Reset default CTRL", [&]() {
		tb.reset();
		uint32_t ctrl = tb.mmio_rd(AddressGenerateUnit::REG_CTRL);
		return TestResult{"", ctrl == 0, "CTRL=" + std::to_string(ctrl)};
	});

	run("MMIO write/read BASE_ADDR", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_BASE_ADDR, 0x1234);
		uint32_t v = tb.mmio_rd(AddressGenerateUnit::REG_BASE_ADDR);
		return TestResult{"", v == 0x1234, "BASE=0x" + std::to_string(v)};
	});

	run("MMIO write/read ITER01", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00020003); // iter0=3 iter1=2
		uint32_t v = tb.mmio_rd(AddressGenerateUnit::REG_ITER01);
		return TestResult{"", v == 0x00020003, "ITER01=0x" + std::to_string(v)};
	});

	run("Start via input pin", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00010004);
		tb.gen_ready.write(true);
		tb.pulse_start();
		bool b = tb.wait_until([&]() { return tb.busy.read(); }, 5);
		tb.gen_ready.write(true);
		return TestResult{"", b, std::string("busy=") + (b ? "1" : "0")};
	});

	run("Start via CTRL bit0", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00010004);
		tb.gen_ready.write(false);
		tb.mmio_wr(AddressGenerateUnit::REG_CTRL, 0x1);
		bool b = tb.wait_until([&]() { return tb.busy.read(); }, 5);
		tb.gen_ready.write(true);
		return TestResult{"", b, std::string("busy=") + (b ? "1" : "0")};
	});

	run("Address progression with stride0", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_BASE_ADDR, 100);
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00010003); // iter0=3
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE0, 4);
		tb.gen_ready.write(false);
		tb.pulse_start();

		bool ready = tb.wait_until([&]() { return tb.gen_valid.read(); }, 5);
		if (!ready) return TestResult{"", false, "gen_valid timeout"};

		uint32_t a0 = tb.gen_addr.read();
		tb.gen_ready.write(true); tb.tick(1);
		tb.gen_ready.write(false); tb.tick(1);
		uint32_t a1 = tb.gen_addr.read();
		tb.gen_ready.write(true); tb.tick(1);
		tb.gen_ready.write(false); tb.tick(1);
		uint32_t a2 = tb.gen_addr.read();
		tb.gen_ready.write(true);
		bool ok = (a0 == 100) && (a1 == 104) && (a2 == 108);
		return TestResult{"", ok, "addr=" + std::to_string(a0) + "," + std::to_string(a1) + "," + std::to_string(a2)};
	});

	run("Loop carry iter0->iter1", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_BASE_ADDR, 0);
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00020002); // iter0=2 iter1=2
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE0, 1);
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE1, 10);
		tb.gen_ready.write(false);
		tb.pulse_start();

		bool ready = tb.wait_until([&]() { return tb.gen_valid.read(); }, 5);
		if (!ready) return TestResult{"", false, "gen_valid timeout"};

		uint32_t a0 = tb.gen_addr.read();
		tb.gen_ready.write(true); tb.tick(1); tb.gen_ready.write(false); tb.tick(1);
		uint32_t a1 = tb.gen_addr.read();
		tb.gen_ready.write(true); tb.tick(1); tb.gen_ready.write(false); tb.tick(1);
		uint32_t a2 = tb.gen_addr.read();
		tb.gen_ready.write(true); tb.tick(1); tb.gen_ready.write(false); tb.tick(1);
		uint32_t a3 = tb.gen_addr.read();
		tb.gen_ready.write(true);
		bool ok = (a0 == 0 && a1 == 1 && a2 == 10 && a3 == 11);
		return TestResult{"", ok, "seq=" + std::to_string(a0) + "," + std::to_string(a1) + "," + std::to_string(a2) + "," + std::to_string(a3)};
	});

	run("Tag level0 stride", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00010003); // iter0=3
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_BASE, 5);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_STRIDE0, 2);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_CTRL, 0);
		tb.gen_ready.write(false);
		tb.pulse_start();

		bool ready = tb.wait_until([&]() { return tb.gen_valid.read(); }, 5);
		if (!ready) return TestResult{"", false, "gen_valid timeout"};

		uint32_t t0 = tb.gen_tag.read().to_uint();
		tb.gen_ready.write(true); tb.tick(1); tb.gen_ready.write(false); tb.tick(1);
		uint32_t t1 = tb.gen_tag.read().to_uint();
		tb.gen_ready.write(true); tb.tick(1); tb.gen_ready.write(false); tb.tick(1);
		uint32_t t2 = tb.gen_tag.read().to_uint();
		tb.gen_ready.write(true);
		bool ok = (t0 == 5 && t1 == 7 && t2 == 9);
		return TestResult{"", ok, "tag=" + std::to_string(t0) + "," + std::to_string(t1) + "," + std::to_string(t2)};
	});

	run("Tag level1 stride", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00030002); // iter0=2 iter1=3
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_BASE, 1);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_STRIDE1, 4);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_CTRL, 1); // use idx1
		tb.gen_ready.write(false);
		tb.pulse_start();

		bool ready = tb.wait_until([&]() { return tb.gen_valid.read(); }, 5);
		if (!ready) return TestResult{"", false, "gen_valid timeout"};

		uint32_t t0 = tb.gen_tag.read().to_uint();
		tb.gen_ready.write(true); tb.tick(1); tb.gen_ready.write(false); tb.tick(1);
		uint32_t t1 = tb.gen_tag.read().to_uint();
		tb.gen_ready.write(true); tb.tick(1); tb.gen_ready.write(false); tb.tick(1);
		uint32_t t2 = tb.gen_tag.read().to_uint();
		tb.gen_ready.write(true);
		bool ok = (t0 == 1 && t1 == 1 && t2 == 5);
		return TestResult{"", ok, "tag=" + std::to_string(t0) + "," + std::to_string(t1) + "," + std::to_string(t2)};
	});

	run("Ultra flag from CTRL bit3", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_CTRL, 0x9); // start + ultra
		tb.tick(2);
		bool u = tb.gen_ultra.read();
		return TestResult{"", u, std::string("ultra=") + (u ? "1" : "0")};
	});

	run("Mask from MASK_CFG", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_MASK_CFG, 0x00A5);
		tb.pulse_start();
		uint32_t m = tb.gen_mask.read().to_uint();
		return TestResult{"", m == 0x00A5, "mask=0x" + std::to_string(m)};
	});

	run("Busy and not quiesced when gen_ready low", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_CTRL, 0x1);
		tb.gen_ready.write(false);
		tb.tick(2);
		uint32_t status = tb.mmio_rd(AddressGenerateUnit::REG_STATUS);
		bool busy     = ((status >> 1) & 0x1) != 0;   // BUSY  = bit 1
		bool quiesced = ((status >> 3) & 0x1) != 0;    // QUIESCED = bit 3
		tb.gen_ready.write(true);
		return TestResult{"", busy && !quiesced, "STATUS=0x" + std::to_string(status)};
	});

	run("Stop pin clears busy", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_CTRL, 0x1);
		tb.tick(2);
		tb.pulse_stop();
		bool b = tb.busy.read();
		return TestResult{"", !b, std::string("busy=") + (b ? "1" : "0")};
	});

	struct LoopTraceEntry {
		int cycle;
		sc_time t;
		bool ready;
		bool valid;
		bool fire;
		uint32_t addr;
		uint32_t tag;
	};

	struct LoopRunResult {
		std::vector<LoopTraceEntry> traces;
		std::vector<uint32_t> fired_addr;
		std::vector<uint32_t> fired_tag;
		bool seen_done = false;
		bool hold_ok = true;
	};

	auto setup_full_loop = [&](uint32_t base_addr, uint32_t tag_base, uint32_t tag_stride1) {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_BASE_ADDR, base_addr);
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00030002); // iter0=2, iter1=3
		tb.mmio_wr(AddressGenerateUnit::REG_ITER23, 0x00020002); // iter2=2, iter3=2
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE0, 1);
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE1, 10);
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE2, 100);
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE3, 1000);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_BASE, tag_base);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_STRIDE1, tag_stride1);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_CTRL, 2); // use idx2
		tb.mmio_wr(AddressGenerateUnit::REG_MASK_CFG, 0x00F3);
	};

	auto build_expected = [&](uint32_t base_addr,
					   uint32_t tag_base,
					   uint32_t tag_stride1,
					   std::vector<uint32_t>& exp_addr,
					   std::vector<uint32_t>& exp_tag) {
		exp_addr.clear();
		exp_tag.clear();
		for (uint32_t i3 = 0; i3 < 2; ++i3) {
			for (uint32_t i2 = 0; i2 < 2; ++i2) {
				for (uint32_t i1 = 0; i1 < 3; ++i1) {
					for (uint32_t i0 = 0; i0 < 2; ++i0) {
						exp_addr.push_back(base_addr + i0 * 1 + i1 * 10 + i2 * 100 + i3 * 1000);
						exp_tag.push_back((tag_base + i2 * tag_stride1) & 0x3F);
					}
				}
			}
		}
	};

	auto run_full_loop = [&](uint32_t base_addr,
					 uint32_t tag_base,
					 uint32_t tag_stride1,
					 int max_cycles,
					 bool start_ready_low,
					 const std::function<bool(int)>& ready_fn) {
		LoopRunResult out;
		setup_full_loop(base_addr, tag_base, tag_stride1);

		tb.gen_ready.write(start_ready_low ? false : true);
		tb.pulse_start();

		bool prev_stall_valid = false;
		uint32_t hold_addr = 0;
		uint32_t hold_tag = 0;

		for (int cycle = 0; cycle < max_cycles; ++cycle) {
			const bool ready = ready_fn(cycle);
			tb.gen_ready.write(ready);

			const bool valid = tb.gen_valid.read();
			const bool fire = valid && ready;
			const uint32_t addr = tb.gen_addr.read().to_uint();
			const uint32_t tag = tb.gen_tag.read().to_uint();

			out.traces.push_back({cycle, sc_time_stamp(), ready, valid, fire, addr, tag});

			if (valid && !ready) {
				if (!prev_stall_valid) {
					hold_addr = addr;
					hold_tag = tag;
					prev_stall_valid = true;
				} else if (addr != hold_addr || tag != hold_tag) {
					out.hold_ok = false;
				}
			} else {
				prev_stall_valid = false;
			}

			if (fire) {
				out.fired_addr.push_back(addr);
				out.fired_tag.push_back(tag);
			}

			if (tb.done.read()) {
				out.seen_done = true;
				break;
			}

			tb.tick(1);
		}

		return out;
	};

	auto print_loop_trace = [&](const std::string& title,
					  const std::vector<LoopTraceEntry>& traces,
					  bool id_mode,
					  bool skip_idle_ready,
					  int max_rows) {
		std::cout << "\n" << title << "\n";
		if (id_mode) {
			std::cout << std::left
					  << std::setw(5) << "ID"
					  << std::setw(10) << "Cycle"
					  << std::setw(14) << "Time(ns)"
					  << std::setw(18) << "Addr(dec/hex)"
					  << std::setw(12) << "Tag(dec)"
					  << "Tag(hex)"
					  << "\n";
			std::cout << "---------------------------------------------------------------\n";
		} else {
			std::cout << std::left
					  << std::setw(8) << "Cycle"
					  << std::setw(12) << "Time(ns)"
					  << std::setw(8) << "Ready"
					  << std::setw(8) << "Valid"
					  << std::setw(8) << "Fire"
					  << std::setw(18) << "Addr(dec/hex)"
					  << std::setw(12) << "Tag(dec)"
					  << "Tag(hex)"
					  << "\n";
			std::cout << "-----------------------------------------------------------------------\n";
		}

		int printed = 0;
		for (const auto& e : traces) {
			if (skip_idle_ready && !e.valid && e.ready) {
				continue;
			}
			if (max_rows > 0 && printed >= max_rows) {
				break;
			}

			std::ostringstream addr_os;
			addr_os << e.addr << "/0x" << std::hex << std::setw(8) << std::setfill('0') << e.addr;
			std::ostringstream tag_os;
			tag_os << "0x" << std::hex << std::setw(2) << std::setfill('0') << e.tag;

			if (id_mode) {
				std::cout << std::left
						  << std::setw(5) << printed
						  << std::setw(10) << e.cycle
						  << std::setw(14) << e.t.to_seconds() * 1e9
						  << std::setw(18) << addr_os.str()
						  << std::setw(12) << e.tag
						  << tag_os.str()
						  << "\n";
			} else {
				std::cout << std::left
						  << std::setw(8) << e.cycle
						  << std::setw(12) << e.t.to_seconds() * 1e9
						  << std::setw(8) << (e.ready ? "1" : "0")
						  << std::setw(8) << (e.valid ? "1" : "0")
						  << std::setw(8) << (e.fire ? "1" : "0")
						  << std::setw(18) << addr_os.str()
						  << std::setw(12) << e.tag
						  << tag_os.str()
						  << "\n";
			}
			printed++;
		}
	};

	run("Full loop L0~L3 verbose trace", [&]() {
		auto out = run_full_loop(10000, 5, 3, 128, false, [](int) { return true; });
		print_loop_trace("[AGU Verbose] Full loop L0~L3 trace", out.traces, true, true, -1);

		std::vector<uint32_t> exp_addr;
		std::vector<uint32_t> exp_tag;
		build_expected(10000, 5, 3, exp_addr, exp_tag);

		bool seq_ok = (out.fired_addr.size() == exp_addr.size()) && (out.fired_tag.size() == exp_tag.size());
		if (seq_ok) {
			for (size_t i = 0; i < exp_addr.size(); ++i) {
				if (out.fired_addr[i] != exp_addr[i] || out.fired_tag[i] != exp_tag[i]) {
					seq_ok = false;
					break;
				}
			}
		}

		std::ostringstream detail;
		detail << "samples=" << out.fired_addr.size()
			   << ", expected=" << exp_addr.size()
			   << ", done=" << (out.seen_done ? 1 : 0)
			   << ", last_addr=" << (out.fired_addr.empty() ? 0 : out.fired_addr.back())
			   << ", last_tag=" << (out.fired_tag.empty() ? 0 : out.fired_tag.back());

		return TestResult{"", seq_ok && out.seen_done, detail.str()};
	});

	run("Full loop L0~L3 periodic backpressure", [&]() {
		auto out = run_full_loop(
			20000,
			9,
			5,
			320,
			true,
			[](int cycle) { return !((cycle % 5) == 2 || (cycle % 5) == 3); }
		);
		print_loop_trace("[AGU Verbose] Full loop L0~L3 with periodic backpressure", out.traces, false, true, -1);

		std::vector<uint32_t> exp_addr;
		std::vector<uint32_t> exp_tag;
		build_expected(20000, 9, 5, exp_addr, exp_tag);

		bool seq_ok = (out.fired_addr.size() == exp_addr.size()) && (out.fired_tag.size() == exp_tag.size());
		if (seq_ok) {
			for (size_t i = 0; i < exp_addr.size(); ++i) {
				if (out.fired_addr[i] != exp_addr[i] || out.fired_tag[i] != exp_tag[i]) {
					seq_ok = false;
					break;
				}
			}
		}

		std::ostringstream detail;
		detail << "fires=" << out.fired_addr.size()
			   << ", expected=" << exp_addr.size()
			   << ", hold_ok=" << (out.hold_ok ? 1 : 0)
			   << ", done=" << (out.seen_done ? 1 : 0)
			   << ", last_fire_addr=" << (out.fired_addr.empty() ? 0 : out.fired_addr.back())
			   << ", last_fire_tag=" << (out.fired_tag.empty() ? 0 : out.fired_tag.back());

		return TestResult{"", seq_ok && out.hold_ok && out.seen_done, detail.str()};
	});

	run("Full loop L0~L3 pseudo-random backpressure", [&]() {
		uint32_t lcg = 0x12345678u;
		auto out = run_full_loop(
			30000,
			13,
			7,
			512,
			true,
			[&lcg](int) {
				lcg = 1664525u * lcg + 1013904223u;
				return ((lcg >> 30) != 0u); // ~75% ready, deterministic
			}
		);
		print_loop_trace(
			"[AGU Verbose] Full loop L0~L3 with pseudo-random backpressure (seed=0x12345678)",
			out.traces,
			false,
			true,
			48
		);

		std::vector<uint32_t> exp_addr;
		std::vector<uint32_t> exp_tag;
		build_expected(30000, 13, 7, exp_addr, exp_tag);

		bool seq_ok = (out.fired_addr.size() == exp_addr.size()) && (out.fired_tag.size() == exp_tag.size());
		if (seq_ok) {
			for (size_t i = 0; i < exp_addr.size(); ++i) {
				if (out.fired_addr[i] != exp_addr[i] || out.fired_tag[i] != exp_tag[i]) {
					seq_ok = false;
					break;
				}
			}
		}

		std::ostringstream detail;
		detail << "fires=" << out.fired_addr.size()
			   << ", expected=" << exp_addr.size()
			   << ", hold_ok=" << (out.hold_ok ? 1 : 0)
			   << ", done=" << (out.seen_done ? 1 : 0)
			   << ", sample_printed=" << 48;

		return TestResult{"", seq_ok && out.hold_ok && out.seen_done, detail.str()};
	});

	run("PS config intermittent stall (base0 iter[1,3,3,16])", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_BASE_ADDR, 0);
		tb.mmio_wr(AddressGenerateUnit::REG_BASE_ADDR_H, 0);
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, (3u << 16) | 1u);   // iter0=1, iter1=3
		tb.mmio_wr(AddressGenerateUnit::REG_ITER23, (16u << 16) | 3u);  // iter2=3, iter3=16
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE0, 1);
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE1, 1);
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE2, 3);
		tb.mmio_wr(AddressGenerateUnit::REG_STRIDE3, 9);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_BASE, 0);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_STRIDE0, 1);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_STRIDE1, 1);
		tb.mmio_wr(AddressGenerateUnit::REG_TAG_CTRL, 0x2); // use idx2
		tb.mmio_wr(AddressGenerateUnit::REG_MASK_CFG, 0xF);
		tb.mmio_wr(AddressGenerateUnit::REG_CTRL, 0x0); // ultra=0

		tb.gen_ready.write(false);
		tb.pulse_start();

		std::vector<uint32_t> fired_addr;
		std::vector<uint32_t> fired_tag;
		bool hold_ok = true;
		bool seen_done = false;

		bool prev_stall = false;
		uint32_t hold_addr = 0;
		uint32_t hold_tag = 0;

		uint32_t lcg = 0x31415926u;
		int stall_left = -1;
		for (int cycle = 0; cycle < 3000; ++cycle) {
			if (stall_left < 0) {
				lcg = 1664525u * lcg + 1013904223u;
				stall_left = static_cast<int>((lcg >> 30) & 0x3u); // 0~3 cycles stall burst
			}
			const bool ready = (stall_left == 0);
			if (stall_left == 0) {
				stall_left = -1; // keep at least one ready cycle between stall bursts
			} else {
				stall_left--;
			}
			tb.gen_ready.write(ready);

			const bool valid = tb.gen_valid.read();
			const bool fire = valid && ready;
			const uint32_t addr = tb.gen_addr.read().to_uint();
			const uint32_t tag = tb.gen_tag.read().to_uint();

			if (valid && !ready) {
				if (!prev_stall) {
					hold_addr = addr;
					hold_tag = tag;
					prev_stall = true;
				} else if (addr != hold_addr || tag != hold_tag) {
					hold_ok = false;
				}
			} else {
				prev_stall = false;
			}

			if (fire) {
				fired_addr.push_back(addr);
				fired_tag.push_back(tag);
			}

			if (tb.done.read()) {
				seen_done = true;
				break;
			}

			tb.tick(1);
		}

		std::vector<uint32_t> exp_addr;
		std::vector<uint32_t> exp_tag;
		exp_addr.reserve(144);
		exp_tag.reserve(144);
		for (uint32_t i3 = 0; i3 < 16; ++i3) {
			for (uint32_t i2 = 0; i2 < 3; ++i2) {
				for (uint32_t i1 = 0; i1 < 3; ++i1) {
					for (uint32_t i0 = 0; i0 < 1; ++i0) {
						exp_addr.push_back(i0 * 1 + i1 * 1 + i2 * 3 + i3 * 9);
						exp_tag.push_back(i2 & 0x3F);
					}
				}
			}
		}

		bool seq_ok = (fired_addr.size() == exp_addr.size()) && (fired_tag.size() == exp_tag.size());
		if (seq_ok) {
			for (size_t i = 0; i < exp_addr.size(); ++i) {
				if (fired_addr[i] != exp_addr[i] || fired_tag[i] != exp_tag[i]) {
					seq_ok = false;
					break;
				}
			}
		}

		std::ostringstream detail;
		detail << "fires=" << fired_addr.size()
			   << ", expected=" << exp_addr.size()
			   << ", hold_ok=" << (hold_ok ? 1 : 0)
			   << ", done=" << (seen_done ? 1 : 0)
			   << ", first=" << (fired_addr.empty() ? 0 : fired_addr.front())
			   << ", last=" << (fired_addr.empty() ? 0 : fired_addr.back());

		return TestResult{"", seq_ok && hold_ok && seen_done, detail.str()};
	});

	run("Done pulse appears", [&]() {
		tb.reset();
		tb.mmio_wr(AddressGenerateUnit::REG_ITER01, 0x00010001);
		tb.pulse_start();
		bool d = tb.wait_until([&]() { return tb.done.read(); }, 8);
		return TestResult{"", d, std::string("done=") + (d ? "1" : "0")};
	});

	print_report(results, "AGU Unit Test Report");

	int pass_cnt = 0;
	for (const auto& r : results) if (r.pass) pass_cnt++;
	return (pass_cnt == static_cast<int>(results.size())) ? 0 : 1;
}
