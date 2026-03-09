#include <systemc>
#include <iostream>
#include <iomanip>
#include <array>
#include <vector>
#include <deque>
#include <functional>
#include <string>
#include <random>

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
			  << std::setw(60) << "Test"
			  << std::setw(8) << "Result"
			  << "Detail\n";
	std::cout << "--------------------------------------------------------------------------\n";

	int idx = 1;
	int pass_cnt = 0;
	for (const auto& r : results) {
		if (r.pass) pass_cnt++;
		std::cout << std::left << std::setw(4) << idx++
				  << std::setw(60) << r.name
				  << std::setw(8) << (r.pass ? "PASS" : "FAIL")
				  << r.detail << "\n";
	}

	std::cout << "--------------------------------------------------------------------------\n";
	std::cout << "Summary: " << pass_cnt << " / " << results.size() << " passed\n";
	std::cout << "==========================================================================\n";
}

class HDDUTestBench {
public:
	static constexpr int DATA_BITS = 192;
	using data_t = sc_biguint<DATA_BITS>;
	using noc_req_t = request_t<data_t, uint16_t>;
	using noc_addr_req_t = ::noc_addr_req_t;
	using noc_resp_t = response_t<data_t>;
	using spm_req_t = spm_request_t<32, DATA_BITS>;
	using spm_resp_t = spm_response_t<DATA_BITS>;

	sc_clock clk{"clk", 10, SC_NS};
	sc_signal<bool> reset_n;

	std::array<sc_signal<bool>, 4> spm_req_valid;
	std::array<sc_signal<bool>, 4> spm_req_ready;
	std::array<sc_signal<spm_req_t>, 4> spm_req_payload;
	std::array<sc_signal<bool>, 4> spm_resp_valid;
	std::array<sc_signal<bool>, 4> spm_resp_ready;
	std::array<sc_signal<spm_resp_t>, 4> spm_resp_payload;
	std::array<data_t, 4> spm_read_data_img{};
	std::array<std::deque<spm_resp_t>, 4> spm_resp_queue{};
	std::array<bool, 4> spm_auto_resp_enable{};
	std::array<bool, 4> spm_read_data_from_req_addr{};

	std::array<sc_signal<noc_req_t>, 3> noc_out_req_data;
	std::array<sc_signal<bool>, 3> noc_out_req_valid;
	std::array<sc_signal<bool>, 3> noc_out_req_ready;

	sc_signal<noc_addr_req_t> noc_plo_req_data;
	sc_signal<bool> noc_plo_req_valid;
	sc_signal<bool> noc_plo_req_ready;

	sc_signal<noc_resp_t> noc_plo_resp_data;
	sc_signal<bool> noc_plo_resp_valid;
	sc_signal<bool> noc_plo_resp_ready;

	sc_signal<sc_uint<32>> mmio_addr;
	sc_signal<bool> mmio_write;
	sc_signal<sc_uint<32>> mmio_wdata;
	sc_signal<sc_uint<32>> mmio_rdata;
	sc_signal<bool> interrupt;

	HybridDataDeliverUnit<32, 6, DATA_BITS> dut{"HDDU_DUT"};

	HDDUTestBench() {
		dut.clk(clk);
		dut.reset_n(reset_n);
		for (int i = 0; i < 4; ++i) {
			dut.spm_req_valid[i](spm_req_valid[i]);
			dut.spm_req_ready[i](spm_req_ready[i]);
			dut.spm_req_payload[i](spm_req_payload[i]);
			dut.spm_resp_valid[i](spm_resp_valid[i]);
			dut.spm_resp_ready[i](spm_resp_ready[i]);
			dut.spm_resp_payload[i](spm_resp_payload[i]);
		}

		dut.noc_ps_out.data_out(noc_out_req_data[0]);
		dut.noc_ps_out.valid_out(noc_out_req_valid[0]);
		dut.noc_ps_out.ready_in(noc_out_req_ready[0]);
		dut.noc_pd_out.data_out(noc_out_req_data[1]);
		dut.noc_pd_out.valid_out(noc_out_req_valid[1]);
		dut.noc_pd_out.ready_in(noc_out_req_ready[1]);
		dut.noc_pli_out.data_out(noc_out_req_data[2]);
		dut.noc_pli_out.valid_out(noc_out_req_valid[2]);
		dut.noc_pli_out.ready_in(noc_out_req_ready[2]);

		dut.noc_plo_out.data_out(noc_plo_req_data);
		dut.noc_plo_out.valid_out(noc_plo_req_valid);
		dut.noc_plo_out.ready_in(noc_plo_req_ready);
		dut.noc_plo_in.data_in(noc_plo_resp_data);
		dut.noc_plo_in.valid_in(noc_plo_resp_valid);
		dut.noc_plo_in.ready_out(noc_plo_resp_ready);
		dut.mmio_addr(mmio_addr);
		dut.mmio_write(mmio_write);
		dut.mmio_wdata(mmio_wdata);
		dut.mmio_rdata(mmio_rdata);
		dut.interrupt(interrupt);

		reset_n.write(false);
		for (int i = 0; i < 4; ++i) {
			spm_req_valid[i].write(false);
			spm_req_ready[i].write(false);
			spm_req_payload[i].write(spm_req_t{});
			spm_resp_valid[i].write(false);
			spm_resp_ready[i].write(false);
			spm_resp_payload[i].write(spm_resp_t{});
			spm_read_data_img[i] = 0;
			spm_resp_queue[i].clear();
			spm_auto_resp_enable[i] = true;
			spm_read_data_from_req_addr[i] = false;
		}
		for (int i = 0; i < 3; ++i) {
			noc_out_req_data[i].write(noc_req_t{});
			noc_out_req_valid[i].write(false);
			noc_out_req_ready[i].write(false);
		}
		noc_plo_req_data.write(noc_addr_req_t{});
		noc_plo_req_valid.write(false);
		noc_plo_req_ready.write(false);
		noc_plo_resp_data.write(noc_resp_t{});
		noc_plo_resp_valid.write(false);
		noc_plo_resp_ready.write(false);
		mmio_addr.write(0);
		mmio_write.write(false);
		mmio_wdata.write(0);
		interrupt.write(false);
	}

	void set_spm_req_ready_all(bool v) {
		for (int i = 0; i < 4; ++i) {
			spm_req_ready[i].write(v);
		}
	}

	void set_spm_req_ready(int port, bool v) {
		spm_req_ready[port].write(v);
	}

	void set_noc_send_ready_all(bool v) {
		for (int i = 0; i < 3; ++i) {
			noc_out_req_ready[i].write(v);
		}
	}

	void set_noc_send_ready(int port, bool v) {
		noc_out_req_ready[port].write(v);
	}

	void set_noc_plo_req_ready(bool v) {
		noc_plo_req_ready.write(v);
	}

	void disable_all_ready() {
		set_spm_req_ready_all(false);
		set_noc_send_ready_all(false);
		set_noc_plo_req_ready(false);
	}

	void enable_all_ready() {
		set_spm_req_ready_all(true);
		set_noc_send_ready_all(true);
		set_noc_plo_req_ready(true);
	}

	void tick(int n = 1) {
		for (int t = 0; t < n; ++t) {
			for (int i = 0; i < 4; ++i) {
				const bool has_resp = !spm_resp_queue[i].empty();
				spm_resp_valid[i].write(has_resp);
				spm_resp_payload[i].write(has_resp ? spm_resp_queue[i].front() : spm_resp_t{});
			}

			sc_start(10, SC_NS);

			for (int i = 0; i < 4; ++i) {
				if (!spm_resp_queue[i].empty() && spm_resp_ready[i].read()) {
					spm_resp_queue[i].pop_front();
				}
				if (spm_req_valid[i].read() && spm_req_ready[i].read() && spm_auto_resp_enable[i]) {
					const auto req = spm_req_payload[i].read();
					spm_resp_t resp{};
					if (req.wen) {
						resp.rdata = 0;
						resp.code = SPM_RESPONSE_CODE::SPM_OK;
					} else {
						if (spm_read_data_from_req_addr[i]) {
							data_t payload = 0;
							payload.range(31, 0) = req.addr.to_uint();
							resp.rdata = payload;
						} else {
							resp.rdata = spm_read_data_img[i];
						}
						resp.code = SPM_RESPONSE_CODE::SPM_OK;
					}
					spm_resp_queue[i].push_back(resp);
				}
			}
		}
	}

	void reset() {
		reset_n.write(false);
		noc_plo_resp_valid.write(false);
		mmio_write.write(false);
		for (int i = 0; i < 4; ++i) {
			spm_req_ready[i].write(false);
			spm_resp_valid[i].write(false);
			spm_resp_payload[i].write(spm_resp_t{});
			spm_resp_queue[i].clear();
			spm_auto_resp_enable[i] = true;
			spm_read_data_from_req_addr[i] = false;
			if (i < 3) noc_out_req_ready[i].write(false);
		}
		noc_plo_req_ready.write(false);
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

	void setup_basic_agu_bank(int bank, uint32_t base, uint16_t tag_base, bool ultra = false,
						  uint16_t iter0 = 1, uint16_t iter1 = 1,
						  uint16_t iter2 = 1, uint16_t iter3 = 1) {
		const uint32_t b = static_cast<uint32_t>(bank) * 0x100;
		const uint32_t ctrl = b + 0x20;
		const uint32_t tag = b + 0x40;
		const uint32_t base_addr = b + 0x00;
		const uint32_t iter01 = b + 0x08;
		const uint32_t iter23 = b + 0x0C;
		const uint32_t str0 = b + 0x10;
		const uint32_t tag_stride0 = b + 0x44;
		const uint32_t mask = b + 0x54;

		mmio_wr(base_addr, base);
		mmio_wr(iter01, (static_cast<uint32_t>(iter1) << 16) | iter0);
		mmio_wr(iter23, (static_cast<uint32_t>(iter3) << 16) | iter2);
		mmio_wr(tag, tag_base);
		mmio_wr(str0, 1);
		mmio_wr(tag_stride0, 1);
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
		data_t payload = 0;
		payload.range(31, 0) = low32;
		spm_read_data_img[port] = payload;
	}

	uint32_t last_noc_addr(int port) const {
		return noc_out_req_data[port].read().addr;
	}

	bool any_noc_valid012() const {
		return noc_out_req_valid[0].read() || noc_out_req_valid[1].read() || noc_out_req_valid[2].read();
	}
};

int sc_main(int argc, char* argv[]) {
	(void)argc; (void)argv;

	HDDUTestBench tb;
	using data_t = HDDUTestBench::data_t;
	using noc_resp_t = HDDUTestBench::noc_resp_t;
	std::vector<TestResult> results;

	auto enter_config_phase = [&]() {
		tb.reset();
		tb.disable_all_ready();
	};

	auto enter_runtime_phase = [&](bool spm_req, bool noc_send, bool plo_req) {
		tb.set_spm_req_ready_all(spm_req);
		tb.set_noc_send_ready_all(noc_send);
		tb.set_noc_plo_req_ready(plo_req);
	};

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
		enter_config_phase();
		uint32_t v = tb.mmio_rd(0x808);
		return TestResult{"", v == 0xF, "PLANE_EN=0x" + std::to_string(v)};
	});

	run("Global MMIO write/read PLANE_EN", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0x5);
		uint32_t v = tb.mmio_rd(0x808);
		return TestResult{"", v == 0x5, "PLANE_EN=0x" + std::to_string(v)};
	});

	run("Global MMIO write/read PLANE_MODE", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x80C, 0xB);
		uint32_t v = tb.mmio_rd(0x80C);
		return TestResult{"", v == 0xB, "PLANE_MODE=0x" + std::to_string(v)};
	});

	run("Bank0 MMIO passthrough read", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x000, 0x1234);
		uint32_t v = tb.mmio_rd(0x000);
		return TestResult{"", v == 0x1234, "B0.BASE=0x" + std::to_string(v)};
	});

	run("Bank1 MMIO passthrough read", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x100, 0x5678);
		uint32_t v = tb.mmio_rd(0x100);
		return TestResult{"", v == 0x5678, "B1.BASE=0x" + std::to_string(v)};
	});

	run("AGU start drives sram_read_req", [&]() {
		enter_config_phase();
		tb.load_spm_read_data(0, 0xA5A5A5A5);
		tb.setup_basic_agu_bank0(0x40, 0x03, false);
		enter_runtime_phase(true, false, false);
		bool req = false;
		uint32_t addr = 0;
		for (int i = 0; i < 16 && !req; ++i) {
			tb.tick(1);
			if (tb.spm_req_valid[0].read() && !tb.spm_req_payload[0].read().wen) {
				req = true;
				addr = tb.spm_req_payload[0].read().addr.to_uint();
			}
		}
		return TestResult{"", req && addr == 0x40, std::string("req=") + (req ? "1" : "0") + ", addr=" + std::to_string(addr)};
	});

	run("Backpressure increments stall counter", [&]() {
		enter_config_phase();
		tb.set_spm_req_ready(0, false);
		tb.setup_basic_agu_bank0(0x80, 0x01, false);
		tb.tick(5);
		uint32_t stall = tb.mmio_rd(0x834);
		return TestResult{"", stall > 0, "stall=" + std::to_string(stall)};
	});

	run("SPM read data forms NoC packet", [&]() {
		enter_config_phase();
		data_t payload = 0;
		payload.range(31, 0) = 0xDEADBEEF;
		tb.spm_read_data_img[0] = payload;
		tb.setup_basic_agu_bank0(0x100, 0x0A, false);
		enter_runtime_phase(true, true, true);
		tb.set_noc_send_ready(0, false);
		bool v = false;
		bool ok_data = false;
		for (int i = 0; i < 40 && !v; ++i) {
			tb.tick(1);
			if (tb.noc_out_req_valid[0].read()) {
				v = true;
				ok_data = (tb.noc_out_req_data[0].read().data == payload);
			}
		}
		tb.set_noc_send_ready(0, true);
		return TestResult{"", v && ok_data, std::string("valid=") + (v ? "1" : "0")};
	});

	run("NoC addr encodes tag", [&]() {
		enter_config_phase();
		tb.load_spm_read_data(0, 0x1234);
		tb.setup_basic_agu_bank0(0x100, 0x15, false);
		enter_runtime_phase(true, true, false);
		tb.set_noc_send_ready(0, false);
		bool v = false;
		uint32_t a = 0;
		for (int i = 0; i < 40 && !v; ++i) {
			tb.tick(1);
			if (tb.noc_out_req_valid[0].read()) {
				v = true;
				a = tb.last_noc_addr(0);
			}
		}
		tb.set_noc_send_ready(0, true);
		return TestResult{"", v && ((a & 0x3F) == 0x15), "addr=0x" + std::to_string(a)};
	});

	run("NoC addr encodes ultra bit", [&]() {
		enter_config_phase();
		tb.load_spm_read_data(0, 0x2222);
		tb.setup_basic_agu_bank0(0x200, 0x08, true);
		enter_runtime_phase(true, true, false);
		tb.set_noc_send_ready(0, false);
		bool v = false;
		uint32_t a = 0;
		for (int i = 0; i < 40 && !v; ++i) {
			tb.tick(1);
			if (tb.noc_out_req_valid[0].read()) {
				v = true;
				a = tb.last_noc_addr(0);
			}
		}
		bool ultra = ((a >> 6) & 0x1) != 0;
		tb.set_noc_send_ready(0, true);
		return TestResult{"", v && ultra, "addr=0x" + std::to_string(a)};
	});

	run("NoC valid holds when not ready", [&]() {
		enter_config_phase();
		tb.load_spm_read_data(0, 0x3333);
		tb.setup_basic_agu_bank0(0x300, 0x05, false);
		enter_runtime_phase(true, true, false);
		tb.set_noc_send_ready(0, false);
		tb.tick(2);
		tb.tick(2);
		bool hold = tb.noc_out_req_valid[0].read();
		tb.set_noc_send_ready(0, true);
		return TestResult{"", hold, std::string("hold=") + (hold ? "1" : "0")};
	});

	run("NoC valid clears after handshake", [&]() {
		enter_config_phase();
		tb.load_spm_read_data(0, 0x4444);
		tb.setup_basic_agu_bank0(0x310, 0x06, false);
		enter_runtime_phase(true, true, false);
		tb.set_noc_send_ready(0, false);
		tb.tick(2);
		tb.tick(1);
		tb.set_noc_send_ready(0, true);
		bool cleared = false;
		for (int i = 0; i < 10; ++i) {
			tb.tick(1);
			if (!tb.noc_out_req_valid[0].read()) {
				cleared = true;
				break;
			}
		}
		return TestResult{"", cleared, std::string("noc_valid=") + (tb.noc_out_req_valid[0].read() ? "1" : "0")};
	});

	run("Arb state selects active bank", [&]() {
		enter_config_phase();
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
		enter_config_phase();
		tb.setup_basic_agu_bank(3, 0x400, 0x00, false);
		enter_runtime_phase(true, false, true);
		tb.tick(1);
		uint32_t rx_before = tb.mmio_rd(0x830);
		data_t payload = 0;
		payload.range(31, 0) = 0xCAFEBABE;
		noc_resp_t resp{};
		resp.data = payload;
		resp.status = NOC_RESPONSE_STATUS::NOC_OK;
		tb.noc_plo_resp_data.write(resp);
		int wr_seen = 0;
		int ready_seen = 0;
		for (int i = 0; i < 6; ++i) {
			tb.noc_plo_resp_valid.write(true);
			tb.tick(1);
			if (tb.spm_req_valid[3].read() && tb.spm_req_payload[3].read().wen) wr_seen++;
			if (tb.noc_plo_resp_ready.read()) ready_seen++;
		}
		tb.noc_plo_resp_valid.write(false);
		tb.tick(1);
		uint32_t rx_after = tb.mmio_rd(0x830);
		bool ok = (wr_seen > 0) && (ready_seen > 0) && (rx_after > rx_before);
		return TestResult{"", ok,
			"wr_seen=" + std::to_string(wr_seen) +
			", ready_seen=" + std::to_string(ready_seen) +
			", rx_delta=" + std::to_string(rx_after - rx_before)};
	});

	run("Mixed multi-cycle stress with backpressure", [&]() {
		enter_config_phase();
		tb.setup_basic_agu_bank(0, 0x500, 0x01, false, 8, 1, 1);
		tb.setup_basic_agu_bank(1, 0x600, 0x02, false, 8, 1, 1);
		tb.setup_basic_agu_bank(2, 0x700, 0x03, true, 8, 1, 1);
		tb.setup_basic_agu_bank(3, 0x800, 0x00, false, 8, 1, 1);
		enter_runtime_phase(false, false, true);
		uint32_t rx_b_before = tb.mmio_rd(0x834);

		int seen_wr3 = 0;
		int seen_rd012 = 0;
		for (int c = 0; c < 40; ++c) {
			tb.spm_read_data_img[0] = static_cast<uint32_t>(0x1000 + c);
			tb.spm_read_data_img[1] = static_cast<uint32_t>(0x2000 + c);
			tb.spm_read_data_img[2] = static_cast<uint32_t>(0x3000 + c);

			tb.set_spm_req_ready(0, (c % 3) != 0);
			tb.set_spm_req_ready(1, (c % 4) != 0);
			tb.set_spm_req_ready(2, (c % 5) != 0);
			tb.set_spm_req_ready(3, (c % 2) == 0);

			tb.set_noc_send_ready(0, (c % 2) == 0);
			tb.set_noc_send_ready(1, (c % 3) != 1);
			tb.set_noc_send_ready(2, (c % 4) != 2);

			noc_resp_t resp{};
			resp.data = static_cast<uint32_t>(0xABC00000u + static_cast<uint32_t>(c));
			resp.status = NOC_RESPONSE_STATUS::NOC_OK;
			tb.noc_plo_resp_data.write(resp);
			tb.noc_plo_resp_valid.write((c % 3) == 0);

			tb.tick(1);
			if ((tb.spm_req_valid[0].read() && !tb.spm_req_payload[0].read().wen) ||
				(tb.spm_req_valid[1].read() && !tb.spm_req_payload[1].read().wen) ||
				(tb.spm_req_valid[2].read() && !tb.spm_req_payload[2].read().wen)) {
				seen_rd012++;
			}
			if (tb.spm_req_valid[3].read() && tb.spm_req_payload[3].read().wen) {
				seen_wr3++;
			}
		}
		tb.noc_plo_resp_valid.write(false);

		uint32_t stall = tb.mmio_rd(0x834);
		uint32_t rx_b = tb.mmio_rd(0x830);
		uint32_t rx_b_delta = rx_b - rx_b_before;

		bool ok = (seen_wr3 > 0) && (seen_rd012 > 0) && (stall > 0) && (rx_b_delta > 0);
		return TestResult{"", ok,
			"seen_rd012=" + std::to_string(seen_rd012) +
			", seen_wr3=" + std::to_string(seen_wr3) +
			", stall=" + std::to_string(stall) +
			", rx_b_delta=" + std::to_string(rx_b_delta)};
	});

	run("PS: SPM request fields + NoC packet(tag/data) correctness", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0x1);
		data_t payload = 0;
		payload.range(31, 0) = 0x11223344;
		tb.spm_read_data_img[0] = payload;
		tb.setup_basic_agu_bank0(0x120, 0x09, false);
		enter_runtime_phase(true, true, false);
		tb.set_noc_send_ready(0, false);

		bool saw_spm_req = false;
		uint32_t spm_addr = 0;
		bool spm_wen = true;
		for (int i = 0; i < 16 && !saw_spm_req; ++i) {
			tb.tick(1);
			if (tb.spm_req_valid[0].read() && tb.spm_req_ready[0].read()) {
				auto req = tb.spm_req_payload[0].read();
				saw_spm_req = true;
				spm_addr = req.addr.to_uint();
				spm_wen = req.wen;
			}
		}

		bool saw_noc_req = false;
		uint32_t noc_addr = 0;
		data_t noc_data = 0;
		for (int i = 0; i < 20 && !saw_noc_req; ++i) {
			tb.tick(1);
			if (tb.noc_out_req_valid[0].read()) {
				saw_noc_req = true;
				noc_addr = tb.noc_out_req_data[0].read().addr;
				noc_data = tb.noc_out_req_data[0].read().data;
			}
		}
		tb.set_noc_send_ready(0, true);

		const bool ok = saw_spm_req && !spm_wen && (spm_addr == 0x120)
			&& saw_noc_req && ((noc_addr & 0x3F) == 0x09) && (noc_data == payload);
		return TestResult{"", ok,
			"spm_req=" + std::to_string(saw_spm_req ? 1 : 0)
			+ ", spm_addr=" + std::to_string(spm_addr)
			+ ", noc_req=" + std::to_string(saw_noc_req ? 1 : 0)
			+ ", noc_tag=" + std::to_string(noc_addr & 0x3F)};
	});

	run("PD: multi-packet tag stride mapping request->response->NoC", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0x2);
		tb.spm_read_data_from_req_addr[1] = true;

		const uint32_t b = 0x100;
		tb.mmio_wr(b + 0x00, 0x200);
		tb.mmio_wr(b + 0x08, 0x00010003);
		tb.mmio_wr(b + 0x0C, 0x00010001);
		tb.mmio_wr(b + 0x10, 1);
		tb.mmio_wr(b + 0x40, 0x05);
		tb.mmio_wr(b + 0x44, 3);
		tb.mmio_wr(b + 0x4C, 0x0);
		tb.mmio_wr(b + 0x20, 0x1);
		enter_runtime_phase(true, true, false);

		std::vector<uint32_t> spm_addrs;
		std::vector<uint32_t> noc_tags;
		std::vector<uint32_t> noc_data_lsb;
		std::vector<uint32_t> spm_unique;
		std::vector<uint32_t> noc_tag_unique;
		std::vector<uint32_t> noc_data_unique;
		for (int i = 0; i < 200 && (spm_unique.size() < 3 || noc_tag_unique.size() < 3); ++i) {
			tb.tick(1);
			if (tb.spm_req_valid[1].read() && tb.spm_req_ready[1].read() && !tb.spm_req_payload[1].read().wen) {
				const uint32_t a = tb.spm_req_payload[1].read().addr.to_uint();
				spm_addrs.push_back(a);
				if (spm_unique.empty() || spm_unique.back() != a) spm_unique.push_back(a);
			}
			if (tb.noc_out_req_valid[1].read() && tb.noc_out_req_ready[1].read()) {
				auto req = tb.noc_out_req_data[1].read();
				const uint32_t tag = req.addr & 0x3F;
				const uint32_t d = req.data.range(31, 0).to_uint();
				noc_tags.push_back(tag);
				noc_data_lsb.push_back(d);
				if (noc_tag_unique.empty() || noc_tag_unique.back() != tag) {
					noc_tag_unique.push_back(tag);
					noc_data_unique.push_back(d);
				}
			}
		}

		bool ok = (spm_unique.size() >= 3) && (noc_tag_unique.size() >= 3) && (noc_data_unique.size() >= 3);
		for (int i = 0; i < 3 && ok; ++i) {
			const uint32_t exp_addr = 0x200 + static_cast<uint32_t>(i);
			const uint32_t exp_tag = (0x05 + static_cast<uint32_t>(i) * 3u) & 0x3Fu;
			ok = ok && (spm_unique[i] == exp_addr) && (noc_tag_unique[i] == exp_tag) && (noc_data_unique[i] == exp_addr);
		}

		return TestResult{"", ok,
			"spm_n=" + std::to_string(spm_addrs.size())
			+ ", spm_u=" + std::to_string(spm_unique.size())
			+ ", noc_n=" + std::to_string(noc_tags.size())
			+ ", noc_u=" + std::to_string(noc_tag_unique.size())};
	});

	run("PLI: streaming SPM requests without waiting responses", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0x4);
		tb.spm_auto_resp_enable[2] = false;
		tb.setup_basic_agu_bank(2, 0x300, 0x00, false, 4, 1, 1);
		enter_runtime_phase(true, false, false);

		std::vector<uint32_t> req_addrs;
		std::vector<uint32_t> req_unique;
		for (int i = 0; i < 200 && req_unique.size() < 4; ++i) {
			tb.tick(1);
			if (tb.spm_req_valid[2].read() && tb.spm_req_ready[2].read() && !tb.spm_req_payload[2].read().wen) {
				const uint32_t a = tb.spm_req_payload[2].read().addr.to_uint();
				req_addrs.push_back(a);
				if (req_unique.empty() || req_unique.back() != a) req_unique.push_back(a);
			}
		}

		bool seq_ok = req_unique.size() >= 4;
		for (int i = 0; i < 4 && seq_ok; ++i) {
			seq_ok = seq_ok && (req_unique[i] == (0x300u + static_cast<uint32_t>(i)));
		}

		return TestResult{"", seq_ok,
			"issued=" + std::to_string(req_addrs.size())
			+ ", uniq=" + std::to_string(req_unique.size())
			+ ", first_addr=" + std::to_string(req_addrs.empty() ? 0 : req_addrs.front())};
	});

	run("PLO: NoC request carries expected tag+ultra", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0x8);
		tb.setup_basic_agu_bank(3, 0x900, 0x21, true, 3, 1, 1);
		enter_runtime_phase(false, false, true);

		std::vector<uint32_t> noc_addr;
		std::vector<uint32_t> tag_unique;
		for (int i = 0; i < 200 && tag_unique.size() < 3; ++i) {
			tb.tick(1);
			if (tb.noc_plo_req_valid.read() && tb.noc_plo_req_ready.read()) {
				const uint32_t a = tb.noc_plo_req_data.read().addr;
				noc_addr.push_back(a);
				const uint32_t tag = a & 0x3Fu;
				if (tag_unique.empty() || tag_unique.back() != tag) tag_unique.push_back(tag);
			}
		}

		bool ok = (tag_unique.size() >= 3);
		for (int i = 0; i < 3 && ok; ++i) {
			const uint32_t exp_tag = (0x21u + static_cast<uint32_t>(i)) & 0x3Fu;
			ok = ok && (tag_unique[i] == exp_tag);
		}
		for (auto a : noc_addr) {
			ok = ok && (((a >> 6) & 0x1u) == 1u);
		}

		return TestResult{"", ok,
			"noc_req_n=" + std::to_string(noc_addr.size())
			+ ", tag_u=" + std::to_string(tag_unique.size())};
	});

	run("PLO: NoC response data writes back to mapped SPM addresses", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0x8);
		tb.setup_basic_agu_bank(3, 0xA00, 0x12, false, 3, 1, 1);
		enter_runtime_phase(true, false, true);

		std::vector<uint32_t> wr_addr;
		std::vector<uint32_t> wr_data;

		int rsp_idx = 0;
		for (int cyc = 0; cyc < 120 && wr_addr.size() < 3; ++cyc) {
			noc_resp_t resp{};
			if (rsp_idx < 3) {
				resp.data = static_cast<uint32_t>(0xCA000000u + static_cast<uint32_t>(rsp_idx));
				resp.status = NOC_RESPONSE_STATUS::NOC_OK;
				tb.noc_plo_resp_data.write(resp);
				tb.noc_plo_resp_valid.write(true);
			} else {
				tb.noc_plo_resp_valid.write(false);
			}

			tb.tick(1);

			if (rsp_idx < 3 && tb.noc_plo_resp_ready.read()) {
				rsp_idx++;
			}
			if (tb.spm_req_valid[3].read() && tb.spm_req_ready[3].read() && tb.spm_req_payload[3].read().wen) {
				wr_addr.push_back(tb.spm_req_payload[3].read().addr.to_uint());
				wr_data.push_back(tb.spm_req_payload[3].read().wdata.range(31, 0).to_uint());
			}
		}
		tb.noc_plo_resp_valid.write(false);
		tb.tick(1);

		bool ok = (wr_addr.size() >= 3) && (wr_data.size() >= 3);
		for (int i = 0; i < 3 && ok; ++i) {
			const uint32_t a = wr_addr[i];
			ok = ok
				&& (a >= 0xA00u)
				&& (a <= 0xA02u)
				&& (wr_data[i] == (0xCA000000u + static_cast<uint32_t>(i)));
		}
		for (size_t i = 1; i < wr_addr.size() && ok; ++i) {
			ok = ok && (wr_addr[i] >= wr_addr[i - 1]);
		}

		return TestResult{"", ok,
			"rsp_hs=" + std::to_string(rsp_idx)
			+ ", spm_wr_n=" + std::to_string(wr_addr.size())};
	});

	run("Custom AGU profile PS/PD/PLI/PLO (MMIO+runtime)", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0xF);

		auto cfg_agu = [&](int bank,
						  uint32_t base_addr,
						  uint16_t iter0, uint16_t iter1, uint16_t iter2, uint16_t iter3,
						  uint32_t stride0, uint32_t stride1, uint32_t stride2, uint32_t stride3,
						  uint32_t lane_cfg,
						  uint32_t tag_base,
						  uint32_t tag_stride0, uint32_t tag_stride1,
						  uint32_t tag_ctrl,
						  uint32_t mask_cfg,
						  bool ultra) {
			const uint32_t b = static_cast<uint32_t>(bank) * 0x100;
			tb.mmio_wr(b + 0x00, base_addr);
			tb.mmio_wr(b + 0x08, (static_cast<uint32_t>(iter1) << 16) | iter0);
			tb.mmio_wr(b + 0x0C, (static_cast<uint32_t>(iter3) << 16) | iter2);
			tb.mmio_wr(b + 0x10, stride0);
			tb.mmio_wr(b + 0x14, stride1);
			tb.mmio_wr(b + 0x18, stride2);
			tb.mmio_wr(b + 0x1C, stride3);
			tb.mmio_wr(b + 0x28, lane_cfg);
			tb.mmio_wr(b + 0x40, tag_base);
			tb.mmio_wr(b + 0x44, tag_stride0);
			tb.mmio_wr(b + 0x48, tag_stride1);
			tb.mmio_wr(b + 0x4C, tag_ctrl);
			tb.mmio_wr(b + 0x54, mask_cfg);
			const uint32_t ctrl = ultra ? 0x9 : 0x1;
			tb.mmio_wr(b + 0x20, ctrl);
		};

		cfg_agu(0, 0x0, 1, 3, 3, 16, 1, 1, 3, 9, 0x0, 0x0, 1, 1, 0x2, 0xF, false);   // PS
		cfg_agu(1, 0x0, 1, 16, 200, 1, 1, 200, 1, 0, 0x0, 0x0, 1, 1, 0x1, 0xF, false); // PD
		cfg_agu(2, 0x0, 4, 14, 198, 1, 1, 3168, 16, 0, 0x0, 0x0, 1, 1, 0x1, 0xF, false); // PLI
		cfg_agu(3, 0x0, 4, 14, 198, 1, 1, 3168, 16, 0, 0x0, 0x0, 1, 1, 0x1, 0xF, false); // PLO

		auto rd = [&](int bank, uint32_t off) {
			return tb.mmio_rd(static_cast<uint32_t>(bank) * 0x100 + off);
		};

		bool mmio_ok = true;
		mmio_ok = mmio_ok && (rd(0, 0x00) == 0x0) && (rd(0, 0x08) == 0x00030001) && (rd(0, 0x0C) == 0x00100003)
			&& (rd(0, 0x10) == 1) && (rd(0, 0x14) == 1) && (rd(0, 0x18) == 3) && (rd(0, 0x1C) == 9)
			&& (rd(0, 0x28) == 0x0) && (rd(0, 0x40) == 0x0) && (rd(0, 0x44) == 1) && (rd(0, 0x48) == 1)
			&& (rd(0, 0x4C) == 0x2) && (rd(0, 0x54) == 0xF);

		mmio_ok = mmio_ok && (rd(1, 0x00) == 0x0) && (rd(1, 0x08) == 0x00100001) && (rd(1, 0x0C) == 0x000100C8)
			&& (rd(1, 0x10) == 1) && (rd(1, 0x14) == 200) && (rd(1, 0x18) == 1) && (rd(1, 0x1C) == 0)
			&& (rd(1, 0x28) == 0x0) && (rd(1, 0x40) == 0x0) && (rd(1, 0x44) == 1) && (rd(1, 0x48) == 1)
			&& (rd(1, 0x4C) == 0x1) && (rd(1, 0x54) == 0xF);

		mmio_ok = mmio_ok && (rd(2, 0x00) == 0x0) && (rd(2, 0x08) == 0x000E0004) && (rd(2, 0x0C) == 0x000100C6)
			&& (rd(2, 0x10) == 1) && (rd(2, 0x14) == 3168) && (rd(2, 0x18) == 16) && (rd(2, 0x1C) == 0)
			&& (rd(2, 0x28) == 0x0) && (rd(2, 0x40) == 0x0) && (rd(2, 0x44) == 1) && (rd(2, 0x48) == 1)
			&& (rd(2, 0x4C) == 0x1) && (rd(2, 0x54) == 0xF);

		mmio_ok = mmio_ok && (rd(3, 0x00) == 0x0) && (rd(3, 0x08) == 0x000E0004) && (rd(3, 0x0C) == 0x000100C6)
			&& (rd(3, 0x10) == 1) && (rd(3, 0x14) == 3168) && (rd(3, 0x18) == 16) && (rd(3, 0x1C) == 0)
			&& (rd(3, 0x28) == 0x0) && (rd(3, 0x40) == 0x0) && (rd(3, 0x44) == 1) && (rd(3, 0x48) == 1)
			&& (rd(3, 0x4C) == 0x1) && (rd(3, 0x54) == 0xF);

		struct PlaneGoldenCfg {
			uint32_t base;
			std::array<uint16_t, 4> iter;
			std::array<uint32_t, 4> stride;
			uint32_t tag_base;
			uint32_t tag_stride0;
			uint32_t tag_stride1;
			uint32_t tag_ctrl;
			bool ultra;
		};

		auto total_count = [](const PlaneGoldenCfg& cfg) -> uint64_t {
			return static_cast<uint64_t>(cfg.iter[0])
				* static_cast<uint64_t>(cfg.iter[1])
				* static_cast<uint64_t>(cfg.iter[2])
				* static_cast<uint64_t>(cfg.iter[3]);
		};

		auto idx_from_linear = [](uint64_t n, const PlaneGoldenCfg& cfg) {
			std::array<uint32_t, 4> idx{};
			idx[0] = static_cast<uint32_t>(n % cfg.iter[0]); n /= cfg.iter[0];
			idx[1] = static_cast<uint32_t>(n % cfg.iter[1]); n /= cfg.iter[1];
			idx[2] = static_cast<uint32_t>(n % cfg.iter[2]); n /= cfg.iter[2];
			idx[3] = static_cast<uint32_t>(n % cfg.iter[3]);
			return idx;
		};

		auto expect_addr = [&](const PlaneGoldenCfg& cfg, uint64_t n) -> uint32_t {
			auto idx = idx_from_linear(n, cfg);
			uint64_t sum = static_cast<uint64_t>(cfg.base);
			sum += static_cast<uint64_t>(idx[0]) * static_cast<uint64_t>(cfg.stride[0]);
			sum += static_cast<uint64_t>(idx[1]) * static_cast<uint64_t>(cfg.stride[1]);
			sum += static_cast<uint64_t>(idx[2]) * static_cast<uint64_t>(cfg.stride[2]);
			sum += static_cast<uint64_t>(idx[3]) * static_cast<uint64_t>(cfg.stride[3]);
			return static_cast<uint32_t>(sum & 0xFFFFFFFFu);
		};

		auto expect_tag = [&](const PlaneGoldenCfg& cfg, uint64_t n) -> uint32_t {
			auto idx = idx_from_linear(n, cfg);
			const uint32_t level = cfg.tag_ctrl & 0x3u;
			uint32_t tag_index = 0;
			uint32_t tag_stride = 1;
			if (level == 0) {
				tag_index = idx[0];
				tag_stride = cfg.tag_stride0;
			} else if (level == 1) {
				tag_index = idx[1];
				tag_stride = cfg.tag_stride1;
			} else if (level == 2) {
				tag_index = idx[2];
				tag_stride = cfg.tag_stride1;
			} else {
				tag_index = idx[3];
				tag_stride = cfg.tag_stride1;
			}
			return (cfg.tag_base + tag_index * tag_stride) & 0x3Fu;
		};

		const PlaneGoldenCfg ps{0x0, {1, 3, 3, 16}, {1, 1, 3, 9}, 0x0, 1, 1, 0x2, false};
		const PlaneGoldenCfg pd{0x0, {1, 16, 200, 1}, {1, 200, 1, 0}, 0x0, 1, 1, 0x1, false};
		const PlaneGoldenCfg pli{0x0, {4, 14, 198, 1}, {1, 3168, 16, 0}, 0x0, 1, 1, 0x1, false};
		const PlaneGoldenCfg plo{0x0, {4, 14, 198, 1}, {1, 3168, 16, 0}, 0x0, 1, 1, 0x1, false};

		const uint64_t n_ps = total_count(ps);
		const uint64_t n_pd = total_count(pd);
		const uint64_t n_pli = total_count(pli);
		const uint64_t n_plo = total_count(plo);

		tb.spm_read_data_from_req_addr[0] = true;
		tb.spm_read_data_from_req_addr[1] = true;
		tb.spm_read_data_from_req_addr[2] = true;

		enter_runtime_phase(true, true, true);

		std::array<uint64_t, 3> spm_rd_seen{0, 0, 0};
		std::array<uint64_t, 3> noc_tx_seen{0, 0, 0};
		uint64_t noc_plo_req_seen = 0;
		uint64_t noc_plo_rsp_sent = 0;
		uint64_t spm_plo_wr_seen = 0;

		bool mismatch = false;
		std::string mismatch_detail;

		auto fail = [&](const std::string& msg) {
			if (!mismatch) {
				mismatch = true;
				mismatch_detail = msg;
			}
		};

		const uint32_t plo_data_seed = 0xAA550000u;
		const int max_cycles = 200000;

		for (int cyc = 0; cyc < max_cycles; ++cyc) {
			noc_resp_t resp{};
			if (noc_plo_rsp_sent < n_plo) {
				resp.data = static_cast<uint32_t>(plo_data_seed + static_cast<uint32_t>(noc_plo_rsp_sent));
				resp.status = NOC_RESPONSE_STATUS::NOC_OK;
				tb.noc_plo_resp_data.write(resp);
				tb.noc_plo_resp_valid.write(true);
			} else {
				tb.noc_plo_resp_valid.write(false);
			}

			tb.tick(1);

			if (!mismatch) {
				if (tb.spm_req_valid[0].read() && tb.spm_req_ready[0].read() && !tb.spm_req_payload[0].read().wen) {
					if (spm_rd_seen[0] >= n_ps) {
						fail("PS SPM read count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.spm_req_payload[0].read().addr.to_uint();
						const uint32_t exp_addr = expect_addr(ps, spm_rd_seen[0]);
						if (got_addr != exp_addr) {
							fail("PS SPM addr mismatch idx=" + std::to_string(spm_rd_seen[0])
								+ " exp=" + std::to_string(exp_addr)
								+ " got=" + std::to_string(got_addr));
						}
						spm_rd_seen[0]++;
					}
				}

				if (tb.spm_req_valid[1].read() && tb.spm_req_ready[1].read() && !tb.spm_req_payload[1].read().wen) {
					if (spm_rd_seen[1] >= n_pd) {
						fail("PD SPM read count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.spm_req_payload[1].read().addr.to_uint();
						const uint32_t exp_addr = expect_addr(pd, spm_rd_seen[1]);
						if (got_addr != exp_addr) {
							fail("PD SPM addr mismatch idx=" + std::to_string(spm_rd_seen[1])
								+ " exp=" + std::to_string(exp_addr)
								+ " got=" + std::to_string(got_addr));
						}
						spm_rd_seen[1]++;
					}
				}

				if (tb.spm_req_valid[2].read() && tb.spm_req_ready[2].read() && !tb.spm_req_payload[2].read().wen) {
					if (spm_rd_seen[2] >= n_pli) {
						fail("PLI SPM read count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.spm_req_payload[2].read().addr.to_uint();
						const uint32_t exp_addr = expect_addr(pli, spm_rd_seen[2]);
						if (got_addr != exp_addr) {
							fail("PLI SPM addr mismatch idx=" + std::to_string(spm_rd_seen[2])
								+ " exp=" + std::to_string(exp_addr)
								+ " got=" + std::to_string(got_addr));
						}
						spm_rd_seen[2]++;
					}
				}

				if (tb.noc_out_req_valid[0].read() && tb.noc_out_req_ready[0].read()) {
					if (noc_tx_seen[0] >= n_ps) {
						fail("PS NoC tx count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.noc_out_req_data[0].read();
						const uint32_t exp_addr = expect_addr(ps, noc_tx_seen[0]);
						const uint32_t exp_tag = expect_tag(ps, noc_tx_seen[0]);
						const uint32_t got_tag = req.addr & 0x3F;
						const uint32_t got_data = req.data.range(31, 0).to_uint();
						if (got_tag != exp_tag || got_data != exp_addr) {
							fail("PS NoC mismatch idx=" + std::to_string(noc_tx_seen[0])
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_data=" + std::to_string(exp_addr)
								+ " got_data=" + std::to_string(got_data));
						}
						noc_tx_seen[0]++;
					}
				}

				if (tb.noc_out_req_valid[1].read() && tb.noc_out_req_ready[1].read()) {
					if (noc_tx_seen[1] >= n_pd) {
						fail("PD NoC tx count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.noc_out_req_data[1].read();
						const uint32_t exp_addr = expect_addr(pd, noc_tx_seen[1]);
						const uint32_t exp_tag = expect_tag(pd, noc_tx_seen[1]);
						const uint32_t got_tag = req.addr & 0x3F;
						const uint32_t got_data = req.data.range(31, 0).to_uint();
						if (got_tag != exp_tag || got_data != exp_addr) {
							fail("PD NoC mismatch idx=" + std::to_string(noc_tx_seen[1])
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_data=" + std::to_string(exp_addr)
								+ " got_data=" + std::to_string(got_data));
						}
						noc_tx_seen[1]++;
					}
				}

				if (tb.noc_out_req_valid[2].read() && tb.noc_out_req_ready[2].read()) {
					if (noc_tx_seen[2] >= n_pli) {
						fail("PLI NoC tx count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.noc_out_req_data[2].read();
						const uint32_t exp_addr = expect_addr(pli, noc_tx_seen[2]);
						const uint32_t exp_tag = expect_tag(pli, noc_tx_seen[2]);
						const uint32_t got_tag = req.addr & 0x3F;
						const uint32_t got_data = req.data.range(31, 0).to_uint();
						if (got_tag != exp_tag || got_data != exp_addr) {
							fail("PLI NoC mismatch idx=" + std::to_string(noc_tx_seen[2])
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_data=" + std::to_string(exp_addr)
								+ " got_data=" + std::to_string(got_data));
						}
						noc_tx_seen[2]++;
					}
				}

				if (tb.noc_plo_req_valid.read() && tb.noc_plo_req_ready.read()) {
					if (noc_plo_req_seen >= n_plo) {
						fail("PLO NoC req count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.noc_plo_req_data.read().addr;
						const uint32_t got_tag = got_addr & 0x3F;
						const bool got_ultra = ((got_addr >> 6) & 0x1u) != 0;
						const uint32_t exp_tag = expect_tag(plo, noc_plo_req_seen);
						if (got_tag != exp_tag || got_ultra != plo.ultra) {
							fail("PLO NoC req mismatch idx=" + std::to_string(noc_plo_req_seen)
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_ultra=" + std::to_string(plo.ultra ? 1 : 0)
								+ " got_ultra=" + std::to_string(got_ultra ? 1 : 0));
						}
						noc_plo_req_seen++;
					}
				}

				if (tb.spm_req_valid[3].read() && tb.spm_req_ready[3].read() && tb.spm_req_payload[3].read().wen) {
					if (spm_plo_wr_seen >= n_plo) {
						fail("PLO SPM write count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.spm_req_payload[3].read();
						const uint32_t got_addr = req.addr.to_uint();
						const uint32_t got_data = req.wdata.range(31, 0).to_uint();
						const uint32_t exp_addr = expect_addr(plo, spm_plo_wr_seen);
						const uint32_t exp_data = plo_data_seed + static_cast<uint32_t>(spm_plo_wr_seen);
						if (got_addr != exp_addr || got_data != exp_data) {
							fail("PLO SPM write mismatch idx=" + std::to_string(spm_plo_wr_seen)
								+ " exp_addr=" + std::to_string(exp_addr)
								+ " got_addr=" + std::to_string(got_addr)
								+ " exp_data=" + std::to_string(exp_data)
								+ " got_data=" + std::to_string(got_data));
						}
						spm_plo_wr_seen++;
					}
				}

				if (tb.noc_plo_resp_valid.read() && tb.noc_plo_resp_ready.read()) {
					noc_plo_rsp_sent++;
					if (noc_plo_rsp_sent > n_plo) {
						fail("PLO NoC response handshake overflow at cycle=" + std::to_string(cyc));
					}
				}
			}

			const bool done_all =
				(spm_rd_seen[0] == n_ps) &&
				(spm_rd_seen[1] == n_pd) &&
				(spm_rd_seen[2] == n_pli) &&
				(noc_tx_seen[0] == n_ps) &&
				(noc_tx_seen[1] == n_pd) &&
				(noc_tx_seen[2] == n_pli) &&
				(noc_plo_req_seen == n_plo) &&
				(noc_plo_rsp_sent == n_plo) &&
				(spm_plo_wr_seen == n_plo);

			if (mismatch || done_all) {
				break;
			}
		}

		tb.noc_plo_resp_valid.write(false);
		tb.tick(2);

		const bool counts_ok =
			(spm_rd_seen[0] == n_ps) &&
			(spm_rd_seen[1] == n_pd) &&
			(spm_rd_seen[2] == n_pli) &&
			(noc_tx_seen[0] == n_ps) &&
			(noc_tx_seen[1] == n_pd) &&
			(noc_tx_seen[2] == n_pli) &&
			(noc_plo_req_seen == n_plo) &&
			(noc_plo_rsp_sent == n_plo) &&
			(spm_plo_wr_seen == n_plo);

		const bool runtime_ok = !mismatch && counts_ok;
		const bool ok = mmio_ok && runtime_ok;

		std::string detail =
			"mmio=" + std::to_string(mmio_ok ? 1 : 0)
			+ ", ps=" + std::to_string(spm_rd_seen[0]) + "/" + std::to_string(n_ps)
			+ ", pd=" + std::to_string(spm_rd_seen[1]) + "/" + std::to_string(n_pd)
			+ ", pli=" + std::to_string(spm_rd_seen[2]) + "/" + std::to_string(n_pli)
			+ ", plo_req=" + std::to_string(noc_plo_req_seen) + "/" + std::to_string(n_plo)
			+ ", plo_rsp=" + std::to_string(noc_plo_rsp_sent) + "/" + std::to_string(n_plo)
			+ ", plo_wr=" + std::to_string(spm_plo_wr_seen) + "/" + std::to_string(n_plo);
		if (!mismatch_detail.empty()) {
			detail += ", first_err={" + mismatch_detail + "}";
		}

		return TestResult{"", ok, detail};
	});

	run("Custom AGU profile PS/PD/PLI/PLO + random backpressure (no data loss)", [&]() {
		enter_config_phase();
		tb.mmio_wr(0x808, 0xF);

		auto cfg_agu = [&](int bank,
						  uint32_t base_addr,
						  uint16_t iter0, uint16_t iter1, uint16_t iter2, uint16_t iter3,
						  uint32_t stride0, uint32_t stride1, uint32_t stride2, uint32_t stride3,
						  uint32_t lane_cfg,
						  uint32_t tag_base,
						  uint32_t tag_stride0, uint32_t tag_stride1,
						  uint32_t tag_ctrl,
						  uint32_t mask_cfg,
						  bool ultra) {
			const uint32_t b = static_cast<uint32_t>(bank) * 0x100;
			tb.mmio_wr(b + 0x00, base_addr);
			tb.mmio_wr(b + 0x08, (static_cast<uint32_t>(iter1) << 16) | iter0);
			tb.mmio_wr(b + 0x0C, (static_cast<uint32_t>(iter3) << 16) | iter2);
			tb.mmio_wr(b + 0x10, stride0);
			tb.mmio_wr(b + 0x14, stride1);
			tb.mmio_wr(b + 0x18, stride2);
			tb.mmio_wr(b + 0x1C, stride3);
			tb.mmio_wr(b + 0x28, lane_cfg);
			tb.mmio_wr(b + 0x40, tag_base);
			tb.mmio_wr(b + 0x44, tag_stride0);
			tb.mmio_wr(b + 0x48, tag_stride1);
			tb.mmio_wr(b + 0x4C, tag_ctrl);
			tb.mmio_wr(b + 0x54, mask_cfg);
			const uint32_t ctrl = ultra ? 0x9 : 0x1;
			tb.mmio_wr(b + 0x20, ctrl);
		};

		cfg_agu(0, 0x0, 1, 3, 3, 16, 1, 1, 3, 9, 0x0, 0x0, 1, 1, 0x2, 0xF, false);   // PS
		cfg_agu(1, 0x0, 1, 16, 200, 1, 1, 200, 1, 0, 0x0, 0x0, 1, 1, 0x1, 0xF, false); // PD
		cfg_agu(2, 0x0, 4, 14, 198, 1, 1, 3168, 16, 0, 0x0, 0x0, 1, 1, 0x1, 0xF, false); // PLI
		cfg_agu(3, 0x0, 4, 14, 198, 1, 1, 3168, 16, 0, 0x0, 0x0, 1, 1, 0x1, 0xF, false); // PLO

		struct PlaneGoldenCfg {
			uint32_t base;
			std::array<uint16_t, 4> iter;
			std::array<uint32_t, 4> stride;
			uint32_t tag_base;
			uint32_t tag_stride0;
			uint32_t tag_stride1;
			uint32_t tag_ctrl;
			bool ultra;
		};

		auto total_count = [](const PlaneGoldenCfg& cfg) -> uint64_t {
			return static_cast<uint64_t>(cfg.iter[0])
				* static_cast<uint64_t>(cfg.iter[1])
				* static_cast<uint64_t>(cfg.iter[2])
				* static_cast<uint64_t>(cfg.iter[3]);
		};

		auto idx_from_linear = [](uint64_t n, const PlaneGoldenCfg& cfg) {
			std::array<uint32_t, 4> idx{};
			idx[0] = static_cast<uint32_t>(n % cfg.iter[0]); n /= cfg.iter[0];
			idx[1] = static_cast<uint32_t>(n % cfg.iter[1]); n /= cfg.iter[1];
			idx[2] = static_cast<uint32_t>(n % cfg.iter[2]); n /= cfg.iter[2];
			idx[3] = static_cast<uint32_t>(n % cfg.iter[3]);
			return idx;
		};

		auto expect_addr = [&](const PlaneGoldenCfg& cfg, uint64_t n) -> uint32_t {
			auto idx = idx_from_linear(n, cfg);
			uint64_t sum = static_cast<uint64_t>(cfg.base);
			sum += static_cast<uint64_t>(idx[0]) * static_cast<uint64_t>(cfg.stride[0]);
			sum += static_cast<uint64_t>(idx[1]) * static_cast<uint64_t>(cfg.stride[1]);
			sum += static_cast<uint64_t>(idx[2]) * static_cast<uint64_t>(cfg.stride[2]);
			sum += static_cast<uint64_t>(idx[3]) * static_cast<uint64_t>(cfg.stride[3]);
			return static_cast<uint32_t>(sum & 0xFFFFFFFFu);
		};

		auto expect_tag = [&](const PlaneGoldenCfg& cfg, uint64_t n) -> uint32_t {
			auto idx = idx_from_linear(n, cfg);
			const uint32_t level = cfg.tag_ctrl & 0x3u;
			uint32_t tag_index = 0;
			uint32_t tag_stride = 1;
			if (level == 0) {
				tag_index = idx[0];
				tag_stride = cfg.tag_stride0;
			} else if (level == 1) {
				tag_index = idx[1];
				tag_stride = cfg.tag_stride1;
			} else if (level == 2) {
				tag_index = idx[2];
				tag_stride = cfg.tag_stride1;
			} else {
				tag_index = idx[3];
				tag_stride = cfg.tag_stride1;
			}
			return (cfg.tag_base + tag_index * tag_stride) & 0x3Fu;
		};

		const PlaneGoldenCfg ps{0x0, {1, 3, 3, 16}, {1, 1, 3, 9}, 0x0, 1, 1, 0x2, false};
		const PlaneGoldenCfg pd{0x0, {1, 16, 200, 1}, {1, 200, 1, 0}, 0x0, 1, 1, 0x1, false};
		const PlaneGoldenCfg pli{0x0, {4, 14, 198, 1}, {1, 3168, 16, 0}, 0x0, 1, 1, 0x1, false};
		const PlaneGoldenCfg plo{0x0, {4, 14, 198, 1}, {1, 3168, 16, 0}, 0x0, 1, 1, 0x1, false};

		const uint64_t n_ps = total_count(ps);
		const uint64_t n_pd = total_count(pd);
		const uint64_t n_pli = total_count(pli);
		const uint64_t n_plo = total_count(plo);

		tb.spm_read_data_from_req_addr[0] = true;
		tb.spm_read_data_from_req_addr[1] = true;
		tb.spm_read_data_from_req_addr[2] = true;

		std::mt19937 rng(0xC0FFEEu);
		auto rand_hit = [&](int pct) -> bool {
			return static_cast<int>(rng() % 100u) < pct;
		};
		auto rand_span_5_30 = [&]() -> int {
			return 5 + static_cast<int>(rng() % 26u);
		};
		auto update_windowed_signal = [&](bool& state, int& remain, int active_pct) {
			if (remain <= 0) {
				state = rand_hit(active_pct);
				remain = rand_span_5_30();
			}
			remain--;
		};

		bool spm0_rdy_state = true;
		bool spm1_rdy_state = true;
		bool spm2_rdy_state = true;
		bool spm3_rdy_state = true;
		bool noc0_rdy_state = true;
		bool noc1_rdy_state = true;
		bool noc2_rdy_state = true;
		bool noc_plo_req_rdy_state = true;
		bool noc_plo_rsp_vld_state = false;
		int spm0_rdy_remain = 0;
		int spm1_rdy_remain = 0;
		int spm2_rdy_remain = 0;
		int spm3_rdy_remain = 0;
		int noc0_rdy_remain = 0;
		int noc1_rdy_remain = 0;
		int noc2_rdy_remain = 0;
		int noc_plo_req_rdy_remain = 0;
		int noc_plo_rsp_vld_remain = 0;

		std::array<uint64_t, 3> spm_rd_seen{0, 0, 0};
		std::array<uint64_t, 3> noc_tx_seen{0, 0, 0};
		uint64_t noc_plo_req_seen = 0;
		uint64_t noc_plo_rsp_hs = 0;
		uint64_t spm_plo_wr_seen = 0;

		bool mismatch = false;
		std::string mismatch_detail;
		auto fail = [&](const std::string& msg) {
			if (!mismatch) {
				mismatch = true;
				mismatch_detail = msg;
			}
		};

		const uint32_t plo_data_seed = 0x5A5A0000u;
		const int max_cycles = 600000;

		for (int cyc = 0; cyc < max_cycles; ++cyc) {
			update_windowed_signal(spm0_rdy_state, spm0_rdy_remain, 70);
			update_windowed_signal(spm1_rdy_state, spm1_rdy_remain, 65);
			update_windowed_signal(spm2_rdy_state, spm2_rdy_remain, 60);
			update_windowed_signal(spm3_rdy_state, spm3_rdy_remain, 75);
			update_windowed_signal(noc0_rdy_state, noc0_rdy_remain, 58);
			update_windowed_signal(noc1_rdy_state, noc1_rdy_remain, 62);
			update_windowed_signal(noc2_rdy_state, noc2_rdy_remain, 55);
			update_windowed_signal(noc_plo_req_rdy_state, noc_plo_req_rdy_remain, 57);
			update_windowed_signal(noc_plo_rsp_vld_state, noc_plo_rsp_vld_remain, 68);

			tb.set_spm_req_ready(0, spm0_rdy_state);
			tb.set_spm_req_ready(1, spm1_rdy_state);
			tb.set_spm_req_ready(2, spm2_rdy_state);
			tb.set_spm_req_ready(3, spm3_rdy_state);
			tb.set_noc_send_ready(0, noc0_rdy_state);
			tb.set_noc_send_ready(1, noc1_rdy_state);
			tb.set_noc_send_ready(2, noc2_rdy_state);
			tb.set_noc_plo_req_ready(noc_plo_req_rdy_state);

			noc_resp_t resp{};
			if (noc_plo_rsp_hs < n_plo && noc_plo_rsp_vld_state) {
				resp.data = static_cast<uint32_t>(plo_data_seed + static_cast<uint32_t>(noc_plo_rsp_hs));
				resp.status = NOC_RESPONSE_STATUS::NOC_OK;
				tb.noc_plo_resp_data.write(resp);
				tb.noc_plo_resp_valid.write(true);
			} else {
				tb.noc_plo_resp_valid.write(false);
			}

			tb.tick(1);

			if (!mismatch) {
				if (tb.spm_req_valid[0].read() && tb.spm_req_ready[0].read() && !tb.spm_req_payload[0].read().wen) {
					if (spm_rd_seen[0] >= n_ps) {
						fail("PS SPM read count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.spm_req_payload[0].read().addr.to_uint();
						const uint32_t exp_addr = expect_addr(ps, spm_rd_seen[0]);
						if (got_addr != exp_addr) {
							fail("PS SPM addr mismatch idx=" + std::to_string(spm_rd_seen[0])
								+ " exp=" + std::to_string(exp_addr)
								+ " got=" + std::to_string(got_addr));
						}
						spm_rd_seen[0]++;
					}
				}

				if (tb.spm_req_valid[1].read() && tb.spm_req_ready[1].read() && !tb.spm_req_payload[1].read().wen) {
					if (spm_rd_seen[1] >= n_pd) {
						fail("PD SPM read count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.spm_req_payload[1].read().addr.to_uint();
						const uint32_t exp_addr = expect_addr(pd, spm_rd_seen[1]);
						if (got_addr != exp_addr) {
							fail("PD SPM addr mismatch idx=" + std::to_string(spm_rd_seen[1])
								+ " exp=" + std::to_string(exp_addr)
								+ " got=" + std::to_string(got_addr));
						}
						spm_rd_seen[1]++;
					}
				}

				if (tb.spm_req_valid[2].read() && tb.spm_req_ready[2].read() && !tb.spm_req_payload[2].read().wen) {
					if (spm_rd_seen[2] >= n_pli) {
						fail("PLI SPM read count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.spm_req_payload[2].read().addr.to_uint();
						const uint32_t exp_addr = expect_addr(pli, spm_rd_seen[2]);
						if (got_addr != exp_addr) {
							fail("PLI SPM addr mismatch idx=" + std::to_string(spm_rd_seen[2])
								+ " exp=" + std::to_string(exp_addr)
								+ " got=" + std::to_string(got_addr));
						}
						spm_rd_seen[2]++;
					}
				}

				if (tb.noc_out_req_valid[0].read() && tb.noc_out_req_ready[0].read()) {
					if (noc_tx_seen[0] >= n_ps) {
						fail("PS NoC tx count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.noc_out_req_data[0].read();
						const uint32_t exp_addr = expect_addr(ps, noc_tx_seen[0]);
						const uint32_t exp_tag = expect_tag(ps, noc_tx_seen[0]);
						const uint32_t got_tag = req.addr & 0x3F;
						const uint32_t got_data = req.data.range(31, 0).to_uint();
						if (got_tag != exp_tag || got_data != exp_addr) {
							fail("PS NoC mismatch idx=" + std::to_string(noc_tx_seen[0])
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_data=" + std::to_string(exp_addr)
								+ " got_data=" + std::to_string(got_data));
						}
						noc_tx_seen[0]++;
					}
				}

				if (tb.noc_out_req_valid[1].read() && tb.noc_out_req_ready[1].read()) {
					if (noc_tx_seen[1] >= n_pd) {
						fail("PD NoC tx count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.noc_out_req_data[1].read();
						const uint32_t exp_addr = expect_addr(pd, noc_tx_seen[1]);
						const uint32_t exp_tag = expect_tag(pd, noc_tx_seen[1]);
						const uint32_t got_tag = req.addr & 0x3F;
						const uint32_t got_data = req.data.range(31, 0).to_uint();
						if (got_tag != exp_tag || got_data != exp_addr) {
							fail("PD NoC mismatch idx=" + std::to_string(noc_tx_seen[1])
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_data=" + std::to_string(exp_addr)
								+ " got_data=" + std::to_string(got_data));
						}
						noc_tx_seen[1]++;
					}
				}

				if (tb.noc_out_req_valid[2].read() && tb.noc_out_req_ready[2].read()) {
					if (noc_tx_seen[2] >= n_pli) {
						fail("PLI NoC tx count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.noc_out_req_data[2].read();
						const uint32_t exp_addr = expect_addr(pli, noc_tx_seen[2]);
						const uint32_t exp_tag = expect_tag(pli, noc_tx_seen[2]);
						const uint32_t got_tag = req.addr & 0x3F;
						const uint32_t got_data = req.data.range(31, 0).to_uint();
						if (got_tag != exp_tag || got_data != exp_addr) {
							fail("PLI NoC mismatch idx=" + std::to_string(noc_tx_seen[2])
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_data=" + std::to_string(exp_addr)
								+ " got_data=" + std::to_string(got_data));
						}
						noc_tx_seen[2]++;
					}
				}

				if (tb.noc_plo_req_valid.read() && tb.noc_plo_req_ready.read()) {
					if (noc_plo_req_seen >= n_plo) {
						fail("PLO NoC req count overflow at cycle=" + std::to_string(cyc));
					} else {
						const uint32_t got_addr = tb.noc_plo_req_data.read().addr;
						const uint32_t got_tag = got_addr & 0x3F;
						const bool got_ultra = ((got_addr >> 6) & 0x1u) != 0;
						const uint32_t exp_tag = expect_tag(plo, noc_plo_req_seen);
						if (got_tag != exp_tag || got_ultra != plo.ultra) {
							fail("PLO NoC req mismatch idx=" + std::to_string(noc_plo_req_seen)
								+ " exp_tag=" + std::to_string(exp_tag)
								+ " got_tag=" + std::to_string(got_tag)
								+ " exp_ultra=" + std::to_string(plo.ultra ? 1 : 0)
								+ " got_ultra=" + std::to_string(got_ultra ? 1 : 0));
						}
						noc_plo_req_seen++;
					}
				}

				if (tb.spm_req_valid[3].read() && tb.spm_req_ready[3].read() && tb.spm_req_payload[3].read().wen) {
					if (spm_plo_wr_seen >= n_plo) {
						fail("PLO SPM write count overflow at cycle=" + std::to_string(cyc));
					} else {
						auto req = tb.spm_req_payload[3].read();
						const uint32_t got_addr = req.addr.to_uint();
						const uint32_t got_data = req.wdata.range(31, 0).to_uint();
						const uint32_t exp_addr = expect_addr(plo, spm_plo_wr_seen);
						const uint32_t exp_data = plo_data_seed + static_cast<uint32_t>(spm_plo_wr_seen);
						if (got_addr != exp_addr || got_data != exp_data) {
							fail("PLO SPM write mismatch idx=" + std::to_string(spm_plo_wr_seen)
								+ " exp_addr=" + std::to_string(exp_addr)
								+ " got_addr=" + std::to_string(got_addr)
								+ " exp_data=" + std::to_string(exp_data)
								+ " got_data=" + std::to_string(got_data));
						}
						spm_plo_wr_seen++;
					}
				}

				if (tb.noc_plo_resp_valid.read() && tb.noc_plo_resp_ready.read()) {
					noc_plo_rsp_hs++;
					if (noc_plo_rsp_hs > n_plo) {
						fail("PLO NoC response handshake overflow at cycle=" + std::to_string(cyc));
					}
				}
			}

			const bool done_all =
				(spm_rd_seen[0] == n_ps) &&
				(spm_rd_seen[1] == n_pd) &&
				(spm_rd_seen[2] == n_pli) &&
				(noc_tx_seen[0] == n_ps) &&
				(noc_tx_seen[1] == n_pd) &&
				(noc_tx_seen[2] == n_pli) &&
				(noc_plo_req_seen == n_plo) &&
				(noc_plo_rsp_hs == n_plo) &&
				(spm_plo_wr_seen == n_plo);

			if (mismatch || done_all) {
				break;
			}
		}

		tb.noc_plo_resp_valid.write(false);
		tb.tick(2);

		const bool counts_ok =
			(spm_rd_seen[0] == n_ps) &&
			(spm_rd_seen[1] == n_pd) &&
			(spm_rd_seen[2] == n_pli) &&
			(noc_tx_seen[0] == n_ps) &&
			(noc_tx_seen[1] == n_pd) &&
			(noc_tx_seen[2] == n_pli) &&
			(noc_plo_req_seen == n_plo) &&
			(noc_plo_rsp_hs == n_plo) &&
			(spm_plo_wr_seen == n_plo);

		const bool ok = !mismatch && counts_ok;

		std::string detail =
			"seed=0xC0FFEE"
			", ps=" + std::to_string(spm_rd_seen[0]) + "/" + std::to_string(n_ps)
			+ ", pd=" + std::to_string(spm_rd_seen[1]) + "/" + std::to_string(n_pd)
			+ ", pli=" + std::to_string(spm_rd_seen[2]) + "/" + std::to_string(n_pli)
			+ ", plo_req=" + std::to_string(noc_plo_req_seen) + "/" + std::to_string(n_plo)
			+ ", plo_rsp=" + std::to_string(noc_plo_rsp_hs) + "/" + std::to_string(n_plo)
			+ ", plo_wr=" + std::to_string(spm_plo_wr_seen) + "/" + std::to_string(n_plo);
		if (!mismatch_detail.empty()) {
			detail += ", first_err={" + mismatch_detail + "}";
		}

		return TestResult{"", ok, detail};
	});

	print_report(results, "HDDU Unit Test Report");

	int pass_cnt = 0;
	for (const auto& r : results) if (r.pass) pass_cnt++;
	return (pass_cnt == static_cast<int>(results.size())) ? 0 : 1;
}
