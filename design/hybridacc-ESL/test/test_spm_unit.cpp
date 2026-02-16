#include <systemc>
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

#include "Cluster/ScratchpadMemory.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc::cluster;

static sc_biguint<192> make_192(uint64_t w0, uint64_t w1, uint64_t w2) {
	sc_biguint<192> v = 0;
	v.range(63, 0) = w0;
	v.range(127, 64) = w1;
	v.range(191, 128) = w2;
	return v;
}

int sc_main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	constexpr unsigned ADDR_W = 16;
	constexpr uint32_t BANK_D = 192;                  // D (enough linear space for 16KB DMA stress)
	constexpr uint32_t NUM_GROUPS = 4;
	constexpr uint32_t GROUP_LINEAR = 3 * BANK_D;     // 3D
	constexpr uint32_t GROUP_SPAN = 4 * BANK_D;       // 4D

	sc_clock clk("clk", 10, SC_NS);
	sc_signal<bool> reset_n;

	sc_signal< sc_uint<8> > config_map_i;
	sc_signal<bool> config_update_i;
	sc_signal<bool> arb_policy_i;

	sc_vector< sc_signal<bool> > noc_req_vld_i("noc_req_vld_i", 4);
	sc_vector< sc_signal<bool> > noc_req_rdy_o("noc_req_rdy_o", 4);
	sc_vector< sc_signal< sc_uint<ADDR_W> > > noc_addr_i("noc_addr_i", 4);
	sc_vector< sc_signal<bool> > noc_mode_i("noc_mode_i", 4);
	sc_vector< sc_signal< sc_biguint<192> > > noc_rdata_o("noc_rdata_o", 4);
	sc_vector< sc_signal< sc_biguint<192> > > noc_wdata_i("noc_wdata_i", 4);
	sc_vector< sc_signal<bool> > noc_resp_vld_o("noc_resp_vld_o", 4);

	sc_signal<bool> dma_req_vld_i;
	sc_signal<bool> dma_req_rdy_o;
	sc_signal< sc_uint<ADDR_W> > dma_addr_i;
	sc_signal<bool> dma_rw_i;
	sc_signal< sc_biguint<64> > dma_wdata_i;
	sc_signal< sc_biguint<64> > dma_rdata_o;
	sc_signal<bool> dma_done_o;

	ScratchpadMemory<4,3,64,ADDR_W> dut("ScratchpadMemory_DUT", BANK_D, 1, 1);
	dut.clk(clk);
	dut.reset_n(reset_n);
	dut.config_map_i(config_map_i);
	dut.config_update_i(config_update_i);
	dut.arb_policy_i(arb_policy_i);

	for (int p = 0; p < 4; ++p) {
		dut.noc_req_vld_i[p](noc_req_vld_i[p]);
		dut.noc_req_rdy_o[p](noc_req_rdy_o[p]);
		dut.noc_addr_i[p](noc_addr_i[p]);
		dut.noc_mode_i[p](noc_mode_i[p]);
		dut.noc_rdata_o[p](noc_rdata_o[p]);
		dut.noc_wdata_i[p](noc_wdata_i[p]);
		dut.noc_resp_vld_o[p](noc_resp_vld_o[p]);
	}

	dut.dma_req_vld_i(dma_req_vld_i);
	dut.dma_req_rdy_o(dma_req_rdy_o);
	dut.dma_addr_i(dma_addr_i);
	dut.dma_rw_i(dma_rw_i);
	dut.dma_wdata_i(dma_wdata_i);
	dut.dma_rdata_o(dma_rdata_o);
	dut.dma_done_o(dma_done_o);

	auto tick = [&](int n = 1) {
		for (int i = 0; i < n; ++i) {
			sc_start(10, SC_NS);
		}
	};

	auto sim_cycle = [&]() -> uint64_t {
		const uint64_t tick_value = sc_time(10, SC_NS).value();
		return static_cast<uint64_t>(sc_time_stamp().value() / tick_value);
	};

	auto log_status = [&](const std::string& msg) {
		std::cout << "[SPM-TEST][C" << sim_cycle() << "] " << msg << std::endl;
	};

	auto clear_noc_inputs = [&]() {
		for (int p = 0; p < 4; ++p) {
			noc_req_vld_i[p].write(false);
			noc_addr_i[p].write(0);
			noc_mode_i[p].write(false);
			noc_wdata_i[p].write(0);
		}
	};

	auto wait_noc_resp = [&](int port, int timeout_cycles = 50) {
		for (int i = 0; i < timeout_cycles; ++i) {
			if (noc_resp_vld_o[port].read()) return true;
			tick(1);
		}
		return false;
	};

	auto wait_dma_done = [&](int timeout_cycles = 50) {
		for (int i = 0; i < timeout_cycles; ++i) {
			if (dma_done_o.read()) return true;
			tick(1);
		}
		return false;
	};

	auto issue_noc_write = [&](int port, uint32_t local_addr, bool mode_parallel, const sc_biguint<192>& wdata) {
		log_status("NoC write req: port=" + std::to_string(port) +
				   " addr=" + std::to_string(local_addr) +
				   " mode=" + std::string(mode_parallel ? "192b" : "64b"));
		noc_addr_i[port].write(local_addr);
		noc_mode_i[port].write(mode_parallel);
		noc_wdata_i[port].write(wdata);
		noc_req_vld_i[port].write(true);

		for (int i = 0; i < 50 && !noc_req_rdy_o[port].read(); ++i) tick(1);
		assert(noc_req_rdy_o[port].read());
		noc_req_vld_i[port].write(false);

		assert(wait_noc_resp(port));
		log_status("NoC write resp done: port=" + std::to_string(port));
	};

	auto issue_noc_read = [&](int port, uint32_t local_addr, bool mode_parallel) {
		log_status("NoC read req: port=" + std::to_string(port) +
				   " addr=" + std::to_string(local_addr) +
				   " mode=" + std::string(mode_parallel ? "192b" : "64b"));
		noc_addr_i[port].write(local_addr);
		noc_mode_i[port].write(mode_parallel);
		noc_req_vld_i[port].write(true);

		for (int i = 0; i < 50 && !noc_req_rdy_o[port].read(); ++i) tick(1);
		assert(noc_req_rdy_o[port].read());
		noc_req_vld_i[port].write(false);

		assert(wait_noc_resp(port));
		log_status("NoC read resp done: port=" + std::to_string(port));
		return noc_rdata_o[port].read();
	};

	auto issue_dma_read = [&](uint32_t global_addr, bool verbose = true) {
		if (verbose) {
			log_status("DMA read req: gaddr=" + std::to_string(global_addr));
		}
		dma_addr_i.write(global_addr);
		dma_rw_i.write(false);
		dma_req_vld_i.write(true);
		for (int i = 0; i < 5000 && !dma_req_rdy_o.read(); ++i) tick(1);
		assert(dma_req_rdy_o.read());
		dma_req_vld_i.write(false);
		assert(wait_dma_done());
		if (verbose) {
			std::ostringstream oss;
			oss << "DMA read done: data=0x" << std::hex << dma_rdata_o.read().to_uint64();
			log_status(oss.str());
		}
		return dma_rdata_o.read();
	};

	auto issue_dma_write = [&](uint32_t global_addr, uint64_t data, bool verbose = true) {
		if (verbose) {
			std::ostringstream oss;
			oss << "DMA write req: gaddr=" << global_addr << " data=0x" << std::hex << data;
			log_status(oss.str());
		}
		dma_addr_i.write(global_addr);
		dma_rw_i.write(true);
		dma_wdata_i.write(data);
		dma_req_vld_i.write(true);
		for (int i = 0; i < 5000 && !dma_req_rdy_o.read(); ++i) tick(1);
		assert(dma_req_rdy_o.read());
		dma_req_vld_i.write(false);
		assert(wait_dma_done());
		if (verbose) {
			log_status("DMA write done");
		}
	};

	// Reset sequence
	reset_n.write(false);
	config_map_i.write(0);
	config_update_i.write(false);
	arb_policy_i.write(false);
	clear_noc_inputs();
	dma_req_vld_i.write(false);
	dma_addr_i.write(0);
	dma_rw_i.write(false);
	dma_wdata_i.write(0);

	tick(3);
	reset_n.write(true);
	tick(2);
	log_status("Reset released");

	// ------------------------------------------------------------
	// Case 1: 192-bit parallel write/read + mapping update
	// default map is [0,1,2,3], so port3 -> group3
	// ------------------------------------------------------------
	const uint32_t par_row = 2;
	const uint32_t par_local_addr = GROUP_LINEAR + par_row; // 3D + row
	const sc_biguint<192> pat192 = make_192(0x1111222233334444ULL,
											0xAAAABBBBCCCCDDDDULL,
											0x0123456789ABCDEFULL);
	log_status("Case1 start: 192-bit write/read + mapping update");

	issue_noc_write(3, par_local_addr, true, pat192);

	// Update map to [3,0,1,2], so port0 -> group3 (for read-back)
	// cfg bits: p0=3, p1=0, p2=1, p3=2 => 0b10_01_00_11 = 0x93
	config_map_i.write(0x93);
	config_update_i.write(true);
	tick(1);
	config_update_i.write(false);
	tick(1);
	log_status("Mapping updated to [3,0,1,2]");

	const sc_biguint<192> r192 = issue_noc_read(0, par_local_addr, true);
	assert(r192 == pat192);
    {
        std::ostringstream oss;
        oss << "Expected 192-bit data: 0x" << std::hex << pat192;
        log_status(oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Readback 192-bit data: 0x" << std::hex << r192;
        log_status(oss.str());
    }
	log_status("Case1 pass: readback matches expected 192-bit payload");

	// ------------------------------------------------------------
	// Case 2: NoC > DMA arbitration on same group
	// remap back to identity [0,1,2,3]
	// ------------------------------------------------------------
	config_map_i.write(0xE4); // p0=0,p1=1,p2=2,p3=3
	config_update_i.write(true);
	tick(1);
	config_update_i.write(false);
	tick(1);
	log_status("Case2 start: NoC > DMA arbitration, mapping restored to identity");

	// preload group0 linear addr0 via DMA write
	const uint32_t group0_global_base = 0 * GROUP_SPAN;
	issue_dma_write(group0_global_base + 0, 0xCAFEBABEDEADBEEFULL);

	// Same cycle: issue NoC read(port0->group0) and DMA read(group0)
	log_status("Inject concurrent NoC read + DMA read to same group");
	noc_addr_i[0].write(0);
	noc_mode_i[0].write(false);
	noc_req_vld_i[0].write(true);
	dma_addr_i.write(group0_global_base + 0);
	dma_rw_i.write(false);
	dma_req_vld_i.write(true);

	tick(1);
	// NoC should be accepted first; DMA should be blocked this cycle
	assert(noc_req_rdy_o[0].read());
	assert(!dma_req_rdy_o.read());
	log_status("Observed priority: NoC accepted, DMA blocked in same cycle");

	// Deassert NoC request, keep DMA until accepted
	noc_req_vld_i[0].write(false);

	// DMA should be accepted after NoC request leaves
	for (int i = 0; i < 50 && !dma_req_rdy_o.read(); ++i) tick(1);
	assert(dma_req_rdy_o.read());
	tick(1);
	dma_req_vld_i.write(false);

	assert(wait_noc_resp(0));
	const uint64_t noc_r0 = static_cast<uint64_t>(noc_rdata_o[0].read().range(63, 0).to_uint64());
	assert(noc_r0 == 0xCAFEBABEDEADBEEFULL);
	{
		std::ostringstream oss;
		oss << "NoC readback data=0x" << std::hex << noc_r0;
		log_status(oss.str());
	}

	// Allow DMA completion in background; this case checks priority ordering.
	tick(5);
	log_status("Case2 pass: arbitration behavior validated");

	// Re-initialize DUT state before long stress loop.
	log_status("Prepare Case3: reset DUT to clean pending state");
	reset_n.write(false);
	config_map_i.write(0);
	config_update_i.write(false);
	arb_policy_i.write(false);
	clear_noc_inputs();
	dma_req_vld_i.write(false);
	dma_addr_i.write(0);
	dma_rw_i.write(false);
	dma_wdata_i.write(0);
	tick(3);
	reset_n.write(true);
	tick(2);
	log_status("Case3 reset released");

	// ------------------------------------------------------------
	// Case 3: DMA stress test, 4KB ~ 16KB payload
	// ------------------------------------------------------------
	log_status("Case3 start: DMA stress 4KB~16KB write/read verify");
	const uint32_t total_linear_words = NUM_GROUPS * GROUP_LINEAR;
	auto linear_word_to_global_addr = [&](uint32_t linear_word_idx) -> uint32_t {
		const uint32_t group = linear_word_idx / GROUP_LINEAR;
		const uint32_t local = linear_word_idx % GROUP_LINEAR;
		return group * GROUP_SPAN + local;
	};
	auto pattern64 = [](uint32_t word_idx) -> uint64_t {
		return (0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(word_idx + 1U)) ^
			   (0xD1B54A32D192ED03ULL + static_cast<uint64_t>(word_idx) * 0x10001ULL);
	};

	for (uint32_t size_kb : {4U, 8U, 12U, 16U}) {
		const uint32_t bytes = size_kb * 1024U;
		const uint32_t words = bytes / 8U;
		assert(words <= total_linear_words);

		log_status("Case3 subtest start: size=" + std::to_string(size_kb) + "KB");

		for (uint32_t i = 0; i < words; ++i) {
			const uint32_t gaddr = linear_word_to_global_addr(i);
			issue_dma_write(gaddr, pattern64(i), false);
			if ((i & 0xFFU) == 0xFFU) {
				log_status("  write progress: " + std::to_string(i + 1) + "/" + std::to_string(words) + " words");
			}
		}

		for (uint32_t i = 0; i < words; ++i) {
			const uint32_t gaddr = linear_word_to_global_addr(i);
			const uint64_t r = static_cast<uint64_t>(issue_dma_read(gaddr, false).to_uint64());
			assert(r == pattern64(i));
			if ((i & 0xFFU) == 0xFFU) {
				log_status("  read progress: " + std::to_string(i + 1) + "/" + std::to_string(words) + " words");
			}
		}

		log_status("Case3 subtest pass: size=" + std::to_string(size_kb) + "KB");
	}

	log_status("Case3 pass: all DMA stress sizes verified");

	std::cout << "[PASS] ScratchpadMemory test passed." << std::endl;
	return 0;
}
