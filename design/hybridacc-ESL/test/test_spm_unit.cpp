/**
 * @file test_spm_unit.cpp
 * @brief ScratchpadMemory (SPM) Unit Testbench
 *
 * SC_MODULE-based testbench (spm_tb) that exercises the SPM through 11 test cases:
 *   TC1  : Basic Reset Verification
 *   TC2  : Simple DMA Write + Read
 *   TC3  : Simple NoC Write + Read (64-bit linear mode)
 *   TC4  : NoC Parallel Write + Read (192-bit parallel mode)
 *   TC5  : DMA Write -> NoC Read  (cross-interface, same physical bank)
 *   TC6  : NoC Write -> DMA Read  (cross-interface, same physical bank)
 *   TC7  : Multi-data DMA Write + Read  (burst, sequential addresses)
 *   TC8  : Multi-data NoC Write + Read  (burst, 4 ports / parallel mode)
 *   TC9  : Mixed DMA + NoC with random per-response back-pressure
 *   TC10 : Parallel <-> Linear Addressing Consistency
 *   TC11 : Same-group DMA/NoC contention priority + response integrity
 */

#include <systemc>
#include <cassert>
#include <iostream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "Cluster/ScratchpadMemory.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc::cluster;

// ============================================================================
// Testbench Module
// ============================================================================
SC_MODULE(spm_tb) {

    // -------------------------------------------------------------------------
    // DUT Template Parameters
    // -------------------------------------------------------------------------
    static constexpr unsigned P_NUM_NOC_PORTS      = 4;
    static constexpr unsigned P_BANKS_PER_GROUP    = 3;
    static constexpr unsigned P_BANK_DATA_WIDTH    = 64;
    static constexpr unsigned P_BANK_DEPTH         = 256;  // Words per bank
    static constexpr unsigned P_SRAM_LATENCY       = 1;
    static constexpr unsigned P_SRAM_PIPE_DEPTH    = 1;
    static constexpr unsigned P_ADDR_WIDTH         = 16;
    static constexpr unsigned P_MAX_OUTSTANDING    = 8;
    static constexpr unsigned P_DMA_MAX_OUTSTANDING= 8;

    // Derived address-layout constants (mirror ScratchpadMemory constants)
    static constexpr uint32_t NOC_DATA_WIDTH = P_BANKS_PER_GROUP * P_BANK_DATA_WIDTH; // 192
    static constexpr uint32_t GROUP_LINEAR   = P_BANKS_PER_GROUP * P_BANK_DEPTH;       // 768
    static constexpr uint32_t GROUP_SPAN     = (P_BANKS_PER_GROUP + 1) * P_BANK_DEPTH; // 1024
    static constexpr uint32_t BPW           = P_BANK_DATA_WIDTH / 8;                   // 8 bytes/word
    // DMA byte address range per Group = GROUP_SPAN * BPW = 8192

    using DUT_T    = ScratchpadMemory<P_NUM_NOC_PORTS, P_BANKS_PER_GROUP,
                                      P_BANK_DATA_WIDTH, P_BANK_DEPTH,
                                      P_SRAM_LATENCY, P_SRAM_PIPE_DEPTH,
                                      P_ADDR_WIDTH, P_MAX_OUTSTANDING,
                                      P_DMA_MAX_OUTSTANDING>;
    using noc_req_t  = spm_request_t <P_ADDR_WIDTH, NOC_DATA_WIDTH>;
    using noc_resp_t = spm_response_t<NOC_DATA_WIDTH>;

    // -------------------------------------------------------------------------
    // Sub-module: DUT
    // -------------------------------------------------------------------------
    DUT_T    dut;
    sc_clock clk;

    // -------------------------------------------------------------------------
    // Signals
    // -------------------------------------------------------------------------
    sc_signal<bool>       reset_n;
    sc_signal<bool>       pmu_rst_i;
    sc_signal<sc_uint<8>> config_map_i;
    sc_signal<bool>       config_update_i;
    sc_signal<bool>       arb_policy_i;

    // NoC ports
    sc_vector<sc_signal<bool>>       noc_req_valid_i {"noc_req_valid_i",  P_NUM_NOC_PORTS};
    sc_vector<sc_signal<bool>>       noc_req_ready_o {"noc_req_ready_o",  P_NUM_NOC_PORTS};
    sc_vector<sc_signal<noc_req_t>>  noc_req_i       {"noc_req_i",        P_NUM_NOC_PORTS};
    sc_vector<sc_signal<bool>>       noc_resp_valid_o{"noc_resp_valid_o", P_NUM_NOC_PORTS};
    sc_vector<sc_signal<bool>>       noc_resp_ready_i{"noc_resp_ready_i", P_NUM_NOC_PORTS};
    sc_vector<sc_signal<noc_resp_t>> noc_resp_o      {"noc_resp_o",       P_NUM_NOC_PORTS};

    // DMA – AXI4-Lite
    sc_signal<bool>                            dma_awvalid_i;
    sc_signal<bool>                            dma_awready_o;
    sc_signal<sc_uint<P_ADDR_WIDTH>>           dma_awaddr_i;
    sc_signal<bool>                            dma_wvalid_i;
    sc_signal<bool>                            dma_wready_o;
    sc_signal<sc_biguint<P_BANK_DATA_WIDTH>>   dma_wdata_i;
    sc_signal<sc_uint<P_BANK_DATA_WIDTH / 8>>  dma_wstrb_i;
    sc_signal<bool>                            dma_bvalid_o;
    sc_signal<bool>                            dma_bready_i;
    sc_signal<sc_uint<2>>                      dma_bresp_o;
    sc_signal<bool>                            dma_arvalid_i;
    sc_signal<bool>                            dma_arready_o;
    sc_signal<sc_uint<P_ADDR_WIDTH>>           dma_araddr_i;
    sc_signal<bool>                            dma_rvalid_o;
    sc_signal<bool>                            dma_rready_i;
    sc_signal<sc_biguint<P_BANK_DATA_WIDTH>>   dma_rdata_o;
    sc_signal<sc_uint<2>>                      dma_rresp_o;

    // PMU outputs
    sc_signal<sc_uint<64>>            pmu_cycle_o;
    sc_vector<sc_signal<sc_uint<64>>> pmu_port_txn_o{"pmu_port_txn_o", P_NUM_NOC_PORTS};
    sc_signal<sc_uint<64>>            pmu_arb_stall_o;
    sc_signal<sc_uint<64>>            pmu_credit_stall_o;

    // =========================================================================
    // Constructor
    // =========================================================================
    SC_HAS_PROCESS(spm_tb);

    explicit spm_tb(sc_module_name nm)
        : sc_module(nm), dut("dut"), clk("clk", 10, SC_NS)
    {
        // Clock & resets
        dut.clk           (clk);
        dut.reset_n       (reset_n);
        dut.pmu_rst_i     (pmu_rst_i);
        dut.config_map_i  (config_map_i);
        dut.config_update_i(config_update_i);
        dut.arb_policy_i  (arb_policy_i);

        // NoC
        dut.spm_req_valid_i (noc_req_valid_i);
        dut.spm_req_ready_o (noc_req_ready_o);
        dut.spm_req_i       (noc_req_i);
        dut.spm_resp_valid_o(noc_resp_valid_o);
        dut.spm_resp_ready_i(noc_resp_ready_i);
        dut.spm_resp_o      (noc_resp_o);

        // DMA
        dut.s_axi_awvalid_i(dma_awvalid_i);
        dut.s_axi_awready_o(dma_awready_o);
        dut.s_axi_awaddr_i (dma_awaddr_i);
        dut.s_axi_wvalid_i (dma_wvalid_i);
        dut.s_axi_wready_o (dma_wready_o);
        dut.s_axi_wdata_i  (dma_wdata_i);
        dut.s_axi_wstrb_i  (dma_wstrb_i);
        dut.s_axi_bvalid_o (dma_bvalid_o);
        dut.s_axi_bready_i (dma_bready_i);
        dut.s_axi_bresp_o  (dma_bresp_o);
        dut.s_axi_arvalid_i(dma_arvalid_i);
        dut.s_axi_arready_o(dma_arready_o);
        dut.s_axi_araddr_i (dma_araddr_i);
        dut.s_axi_rvalid_o (dma_rvalid_o);
        dut.s_axi_rready_i (dma_rready_i);
        dut.s_axi_rdata_o  (dma_rdata_o);
        dut.s_axi_rresp_o  (dma_rresp_o);

        // PMU
        dut.pmu_cycle_cnt_o       (pmu_cycle_o);
        dut.pmu_port_txn_cnt_o    (pmu_port_txn_o);
        dut.pmu_arb_stall_cnt_o   (pmu_arb_stall_o);
        dut.pmu_credit_stall_cnt_o(pmu_credit_stall_o);

        SC_THREAD(run_all_tests);
    }

    // =========================================================================
    // Utility helpers
    // =========================================================================

    /** Advance simulation by N full clock cycles. */
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) wait(10, SC_NS);
    }

    /** Current simulation cycle number (0-based). */
    uint64_t cycle() const {
        return sc_time_stamp().value() / sc_time(10, SC_NS).value();
    }

    /** Prefixed log. */
    void log(const std::string& msg) const {
        std::cout << "[TB][C" << std::setw(6) << cycle() << "] " << msg << "\n";
    }

    /** Build a 192-bit word from three 64-bit lanes [63:0] [127:64] [191:128]. */
    static sc_biguint<192> make192(uint64_t w0, uint64_t w1, uint64_t w2) {
        sc_biguint<192> v = 0;
        v.range(63,  0)   = w0;
        v.range(127, 64)  = w1;
        v.range(191, 128) = w2;
        return v;
    }

    /**
     * Convert a (port, linear_word_addr) pair into the equivalent DMA byte
     * address that accesses the same physical SRAM word.
     *   group = port (default 1-to-1 mapping, config_map = 0xE4)
     *   dma_byte = (group * GROUP_SPAN + local_addr) * BPW
     */
    static uint32_t noc_to_dma_byte(int port, uint32_t local_addr) {
        return (static_cast<uint32_t>(port) * GROUP_SPAN + local_addr) * BPW;
    }

    // =========================================================================
    // DUT control helpers
    // =========================================================================

    /** Clear all NoC request inputs; set resp_ready LOW (TB controls when to accept). */
    void clear_noc() {
        for (unsigned p = 0; p < P_NUM_NOC_PORTS; ++p) {
            noc_req_valid_i[p].write(false);
            noc_req_i[p].write(noc_req_t{});
            noc_resp_ready_i[p].write(false);  // TB drives ready explicitly in consume_noc_resp
        }
    }

    /** Clear all DMA inputs (de-assert valid/ready). */
    void clear_dma() {
        dma_awvalid_i.write(false); dma_awaddr_i.write(0);
        dma_wvalid_i .write(false); dma_wdata_i.write(0); dma_wstrb_i.write(0);
        dma_bready_i .write(false);
        dma_arvalid_i.write(false); dma_araddr_i.write(0);
        dma_rready_i .write(false);
    }

    // -------------------------------------------------------------------------
    // NoC operation helpers
    // -------------------------------------------------------------------------

    /**
     * Poll noc_resp_valid_o[port] until high or timeout expires.
     * @return true if response appeared.
     */
    bool wait_noc_resp(int port, int timeout = 300) {
        for (int i = 0; i < timeout; ++i) {
            if (noc_resp_valid_o[port].read()) return true;
            tick(1);
        }
        return false;
    }

    /**
     * Consume exactly ONE NoC response on the given port.
     * Asserts resp_ready, waits for valid, reads data, then de-asserts and
     * inserts one isolation cycle to prevent accidental double-consumption.
     */
    noc_resp_t consume_noc_resp(int port, int timeout = 300) {
        noc_resp_ready_i[port].write(true);
        if (!wait_noc_resp(port, timeout)) {
            SC_REPORT_ERROR("TB", ("consume_noc_resp timeout port=" +
                                   std::to_string(port)).c_str());
        }
        noc_resp_t resp = noc_resp_o[port].read();
        tick(1);                             // handshake cycle
        noc_resp_ready_i[port].write(false);
        tick(1);                             // isolation gap
        return resp;
    }

    /**
     * Drive a single NoC request (write or read) to 'port', blocking until
     * spm_req_ready_o acknowledges that the request was accepted into the
     * skid buffer or consumed directly by the arbiter.
     * Does NOT wait for the response.
     */
    void noc_issue(int port, uint32_t local_addr, bool wen,
                   const sc_biguint<192>& wdata, int timeout = 150) {
        noc_req_t req{};
        req.addr  = local_addr;
        req.wen   = wen;
        req.wdata = wdata;
        noc_req_i[port].write(req);
        noc_req_valid_i[port].write(true);
        bool ok = false;
        for (int i = 0; i < timeout; ++i) {
            if (noc_req_ready_o[port].read()) { ok = true; break; }
            tick(1);
        }
        tick(1);
        if (!ok) {
            SC_REPORT_ERROR("TB", ("noc_issue timeout port=" +
                                   std::to_string(port)).c_str());
        }
        noc_req_valid_i[port].write(false);
    }

    /**
     * Full blocking NoC write: issue write request, wait for write-ack response.
     * @param local_addr  Group-local word address.
     *   - [0, GROUP_LINEAR) → linear mode  (single bank, 64-bit)
     *   - [GROUP_LINEAR, GROUP_SPAN) → parallel mode (all 3 banks, 192-bit)
     */
    void noc_write(int port, uint32_t local_addr, const sc_biguint<192>& wdata) {
        noc_issue(port, local_addr, true, wdata);
        noc_resp_t wr = consume_noc_resp(port);
        SC_ASSERT( "TB", wr.code == SPM_RESPONSE_CODE::SPM_OK,
                  "noc_write: write-ack code must be SPM_OK");
    }

    /**
     * Full blocking NoC read. Returns 192-bit response data.
     * For linear mode, only rdata[63:0] carries the read value.
     */
    sc_biguint<192> noc_read(int port, uint32_t local_addr) {
        noc_issue(port, local_addr, false, sc_biguint<192>(0));
        noc_resp_t rd = consume_noc_resp(port);
        SC_ASSERT( "TB", rd.code == SPM_RESPONSE_CODE::SPM_OK,
                  "noc_read: response code must be SPM_OK");
        return rd.rdata;
    }

    /**
     * Drain all pending NoC responses on 'port' to clear residual state
     * between test cases.
     */
    void drain_noc(int port, int max_cycles = 24) {
        noc_resp_ready_i[port].write(true);
        tick(4);
        for (int i = 0; i < max_cycles && noc_resp_valid_o[port].read(); ++i)
            tick(1);
        noc_resp_ready_i[port].write(false);
        SC_ASSERT( "TB", !noc_resp_valid_o[port].read(), "drain_noc: channel still non-empty");
    }

    // -------------------------------------------------------------------------
    // DMA operation helpers
    // -------------------------------------------------------------------------

    /**
     * DMA AXI4-Lite write (blocking until B-response received).
     * @param byte_addr  Global byte address.
     * @param data       64-bit write data.
     * @param strb       Byte strobe (default: all 8 bytes active).
     */
    void dma_write(uint32_t byte_addr, uint64_t data,
                   uint8_t strb = 0xFF, int timeout = 300) {
        dma_awaddr_i .write(byte_addr);
        dma_awvalid_i.write(true);
        dma_wdata_i  .write(data);
        dma_wstrb_i  .write(strb);
        dma_wvalid_i .write(true);
        dma_bready_i .write(true);

        bool aw_ok = false, w_ok = false;
        for (int i = 0; i < timeout; ++i) {
            if (dma_awready_o.read()) aw_ok = true;
            if (dma_wready_o.read()) w_ok = true;
            if (aw_ok && w_ok) break;
            tick(1);
            if (aw_ok) dma_awvalid_i.write(false);
            if (w_ok)  dma_wvalid_i .write(false);
        }
        SC_ASSERT( "TB", aw_ok && w_ok, "dma_write: AW/W handshake timeout");
        tick(1);                    // latch into FIFOs
        dma_awvalid_i.write(false);
        dma_wvalid_i .write(false);

        // Wait for B response
        bool b_ok = false;
        for (int i = 0; i < timeout; ++i) {
            if (dma_bvalid_o.read()) { b_ok = true; break; }
            tick(1);
        }
        SC_ASSERT( "TB", b_ok, "dma_write: B-channel timeout");
        tick(1);                    // consume B handshake
        dma_bready_i.write(false);
    }

    /**
     * DMA AXI4-Lite read (blocking until R-response received).
     * @param byte_addr  Global byte address.
     * @returns 64-bit read data.
     */
    uint64_t dma_read(uint32_t byte_addr, int timeout = 300) {
        dma_araddr_i .write(byte_addr);
        dma_arvalid_i.write(true);
        dma_rready_i .write(true);

        bool ar_ok = false;
        for (int i = 0; i < timeout; ++i) {
            if (dma_arready_o.read()) { ar_ok = true; break; }
            tick(1);
        }
        SC_ASSERT( "TB", ar_ok, "dma_read: AR-channel timeout");
        tick(1);
        dma_arvalid_i.write(false);

        bool r_ok = false;
        for (int i = 0; i < timeout; ++i) {
            if (dma_rvalid_o.read()) { r_ok = true; break; }
            tick(1);
        }
        SC_ASSERT( "TB", r_ok, "dma_read: R-channel timeout");
        uint64_t result = dma_rdata_o.read().to_uint64();
        tick(1);                    // consume R handshake
        dma_rready_i.write(false);
        return result;
    }

    /**
     * Wait until a DMA read response is visible without consuming it.
     * @return true if dma_rvalid_o asserted before timeout.
     */
    bool wait_dma_rvalid(int timeout = 300) {
        for (int i = 0; i < timeout; ++i) {
            if (dma_rvalid_o.read()) return true;
            tick(1);
        }
        return false;
    }

    // =========================================================================
    //  TC1 – Basic Reset Verification
    // =========================================================================
    void tc1_basic_reset() {
        log("TC1: Basic Reset Verification");

        // All NoC ports must be ready (skid empty) and have no response pending
        for (unsigned p = 0; p < P_NUM_NOC_PORTS; ++p) {
            SC_ASSERT( "TB", noc_req_ready_o[p].read(),
                      "TC1: spm_req_ready_o must be 1 after reset");
            SC_ASSERT( "TB", !noc_resp_valid_o[p].read(),
                      "TC1: spm_resp_valid_o must be 0 after reset");
        }
        // DMA response channels idle
        SC_ASSERT( "TB", !dma_bvalid_o.read(), "TC1: dma_bvalid must be 0 after reset");
        SC_ASSERT( "TB", !dma_rvalid_o.read(), "TC1: dma_rvalid must be 0 after reset");

        // PMU cycle counter must increment
        uint64_t c0 = pmu_cycle_o.read().to_uint64();
        tick(5);
        uint64_t c1 = pmu_cycle_o.read().to_uint64();
        SC_ASSERT( "TB", c1 > c0, "TC1: PMU cycle counter must increment");
        log("  PMU cycle: [" + std::to_string(c0) + " -> " + std::to_string(c1) + "]");

        // PMU reset clears all PMU counters
        pmu_rst_i.write(true); tick(1);
        pmu_rst_i.write(false); tick(1);
        uint64_t c_after = pmu_cycle_o.read().to_uint64();
        SC_ASSERT( "TB", c_after <= 2, "TC1: PMU cycle must be near-zero after pmu_rst");
        log("  Post-pmu_rst cycle count = " + std::to_string(c_after));

        log("  TC1 PASS");
    }

    // =========================================================================
    //  TC2 – Simple DMA Write + Read
    // =========================================================================
    void tc2_simple_dma_wr_rd() {
        log("TC2: Simple DMA Write + Read");

        const uint32_t byte_addr = 0x0000;
        const uint64_t pattern   = 0xDEAD'BEEF'CAFE'BABEull;

        dma_write(byte_addr, pattern);
        tick(2);
        uint64_t got = dma_read(byte_addr);

        std::ostringstream oss;
        oss << "  addr=0x" << std::hex << byte_addr
            << " exp=0x" << pattern << " got=0x" << got;
        log(oss.str());
        SC_ASSERT( "TB", got == pattern, "TC2: DMA round-trip data mismatch");

        log("  TC2 PASS");
    }

    // =========================================================================
    //  TC3 – Simple NoC Linear Write + Read  (64-bit, port 0)
    // =========================================================================
    void tc3_simple_noc_linear_wr_rd() {
        log("TC3: Simple NoC Linear Write + Read");

        const uint32_t       laddr = 42;
        const uint64_t       v64   = 0x0123'4567'89AB'CDEFull;
        const sc_biguint<192> wd   = make192(v64, 0, 0);

        noc_write(0, laddr, wd);
        tick(1);
        sc_biguint<64> got = noc_read(0, laddr).range(63, 0);
        sc_biguint<64> exp = sc_biguint<64>(v64);

        std::ostringstream oss;
        oss << "  laddr=" << laddr
            << " exp=0x" << std::hex << exp.to_uint64()
            << " got=0x" << got.to_uint64();
        log(oss.str());
        SC_ASSERT( "TB", got == exp, "TC3: NoC linear round-trip data mismatch");

        log("  TC3 PASS");
    }

    // =========================================================================
    //  TC4 – Simple NoC Parallel Write + Read  (192-bit, port 1)
    // =========================================================================
    void tc4_simple_noc_parallel_wr_rd() {
        log("TC4: Simple NoC Parallel Write + Read (192-bit)");

        const uint32_t       par_laddr = GROUP_LINEAR + 5;  // row 5
        const sc_biguint<192> pat = make192(
            0x1111'2222'3333'4444ull,
            0xAAAA'BBBB'CCCC'DDDDull,
            0x0123'4567'89AB'CDEFull);

        noc_write(1, par_laddr, pat);   // port 1 → Group 1
        tick(1);
        sc_biguint<192> got = noc_read(1, par_laddr);

        std::ostringstream oss;
        oss << "  exp=" << std::hex << pat << "\n  got=" << got;
        log(oss.str());
        SC_ASSERT( "TB", got == pat, "TC4: NoC parallel round-trip data mismatch");

        log("  TC4 PASS");
    }

    // =========================================================================
    //  TC5 – DMA Write -> NoC Read  (cross-interface)
    //   Write via DMA to Group 0, linear word 20.
    //   Read back via NoC port 0 at the same local address.
    // =========================================================================
    void tc5_dma_write_noc_read() {
        log("TC5: DMA Write -> NoC Read (cross-interface)");

        const int        port     = 0;
        const uint32_t   laddr    = 20;
        const uint32_t   dma_byte = noc_to_dma_byte(port, laddr);
        const uint64_t   pat64    = 0xFEED'FACE'CAFE'D00Dull;

        dma_write(dma_byte, pat64);
        tick(2);

        sc_biguint<64> got = noc_read(port, laddr).range(63, 0);

        std::ostringstream oss;
        oss << "  dma_byte=0x" << std::hex << dma_byte
            << " laddr=" << std::dec << laddr
            << " exp=0x" << std::hex << pat64
            << " got=0x" << got.to_uint64();
        log(oss.str());
        SC_ASSERT( "TB", got.to_uint64() == pat64, "TC5: DMA-write / NoC-read data mismatch");

        log("  TC5 PASS");
    }

    // =========================================================================
    //  TC6 – NoC Write -> DMA Read  (cross-interface)
    //   Write via NoC port 2 (Group 2), linear word 35.
    //   Read back via DMA at the corresponding byte address.
    // =========================================================================
    void tc6_noc_write_dma_read() {
        log("TC6: NoC Write -> DMA Read (cross-interface)");

        const int        port     = 2;
        const uint32_t   laddr    = 35;
        const uint64_t   pat64    = 0x8765'4321'ABCD'EF01ull;
        const uint32_t   dma_byte = noc_to_dma_byte(port, laddr);

        noc_write(port, laddr, make192(pat64, 0, 0));
        tick(2);

        uint64_t got = dma_read(dma_byte);

        std::ostringstream oss;
        oss << "  port=" << port << " laddr=" << laddr
            << " dma_byte=0x" << std::hex << dma_byte
            << " exp=0x" << pat64 << " got=0x" << got;
        log(oss.str());
        SC_ASSERT( "TB", got == pat64, "TC6: NoC-write / DMA-read data mismatch");

        log("  TC6 PASS");
    }

    // =========================================================================
    //  TC7 – Multi-data DMA Write + Read  (burst)
    //   Write 32 sequential 64-bit words via DMA to Group 1,
    //   then read each back and verify.
    // =========================================================================
    void tc7_multi_dma_burst() {
        log("TC7: Multi-data DMA Burst Write + Read (32 words, Group 1)");

        constexpr int    COUNT   = 32;
        const uint32_t   base_b  = 1u * GROUP_SPAN * BPW   // Group 1 start
                                 + 200u * BPW;              // skip first 200 words

        // Write phase
        for (int i = 0; i < COUNT; ++i) {
            uint64_t pat = 0xA700'0000'0000'0000ull | static_cast<uint64_t>(i);
            dma_write(base_b + static_cast<uint32_t>(i) * BPW, pat);
        }
        tick(4);

        // Read & verify phase
        for (int i = 0; i < COUNT; ++i) {
            uint64_t exp = 0xA700'0000'0000'0000ull | static_cast<uint64_t>(i);
            uint64_t got = dma_read(base_b + static_cast<uint32_t>(i) * BPW);
            if (got != exp) {
                std::ostringstream oss;
                oss << "TC7 mismatch at i=" << i
                    << " exp=0x" << std::hex << exp << " got=0x" << got;
                SC_REPORT_ERROR("TB", oss.str().c_str());
            }
        }

        log("  TC7 PASS (" + std::to_string(COUNT) + " words verified)");
    }

    // =========================================================================
    //  TC8 – Multi-data NoC Write + Read  (4 ports, parallel mode)
    //   Each port writes COUNT parallel (192-bit) words to its own Group,
    //   then reads each back and verifies. Each port done sequentially.
    // =========================================================================
    void tc8_multi_noc_parallel_burst() {
        log("TC8: Multi-data NoC Parallel Burst (4 ports × 16 words)");

        constexpr int    COUNT    = 16;
        const uint32_t   par_base = GROUP_LINEAR + 30;  // row 30 in parallel region

        for (unsigned p = 0; p < P_NUM_NOC_PORTS; ++p) {
            // Write phase
            for (int i = 0; i < COUNT; ++i) {
                sc_biguint<192> pat = make192(
                    (static_cast<uint64_t>(p) << 40) | static_cast<uint64_t>(0x10'0000 + i),
                    (static_cast<uint64_t>(p) << 40) | static_cast<uint64_t>(0x20'0000 + i),
                    (static_cast<uint64_t>(p) << 40) | static_cast<uint64_t>(0x30'0000 + i));
                noc_write(static_cast<int>(p),
                          par_base + static_cast<uint32_t>(i), pat);
            }
            tick(2);
            // Read & verify phase
            for (int i = 0; i < COUNT; ++i) {
                sc_biguint<192> exp = make192(
                    (static_cast<uint64_t>(p) << 40) | static_cast<uint64_t>(0x10'0000 + i),
                    (static_cast<uint64_t>(p) << 40) | static_cast<uint64_t>(0x20'0000 + i),
                    (static_cast<uint64_t>(p) << 40) | static_cast<uint64_t>(0x30'0000 + i));
                sc_biguint<192> got = noc_read(static_cast<int>(p),
                                               par_base + static_cast<uint32_t>(i));
                if (got != exp) {
                    std::ostringstream oss;
                    oss << "TC8 p=" << p << " i=" << i
                        << " exp=" << std::hex << exp << " got=" << got;
                    SC_REPORT_ERROR("TB", oss.str().c_str());
                }
            }
        }

        log("  TC8 PASS");
    }

    // =========================================================================
    //  TC9 – Mixed DMA + NoC with Random Per-Response Back-Pressure
    //
    //  Phase A: DMA writes COUNT words to Group 3; NoC port 3 reads them back
    //           with a random [0,15]-cycle back-pressure window before each
    //           response is consumed.
    //
    //  Phase B: NoC port 0 writes COUNT words; DMA reads them back with a
    //           random [0,15]-cycle delay before asserting rready.
    //
    //  Data integrity is verified for every transaction.
    // =========================================================================
    void tc9_mixed_random_backpressure() {
        log("TC9: Mixed DMA + NoC with Random Back-Pressure");

        std::mt19937 rng(0xBADC0FFEu);
        std::uniform_int_distribution<int> bp_dist(0, 15);

        constexpr int    COUNT  = 20;
        const uint32_t   lbase  = 110;  // linear local word-address base
        const uint32_t   g3_dma = 3u * GROUP_SPAN * BPW;  // Group-3 DMA byte base

        // ---- Phase A: DMA write, NoC port-3 read with random back-pressure ----
        log("  Phase A: DMA write -> NoC read (random resp back-pressure)");

        for (int i = 0; i < COUNT; ++i) {
            uint64_t pat = 0xE900'0000'0000'0000ull | static_cast<uint64_t>(i);
            dma_write(g3_dma + (lbase + static_cast<uint32_t>(i)) * BPW, pat);
        }
        tick(4);

        for (int i = 0; i < COUNT; ++i) {
            uint32_t laddr = lbase + static_cast<uint32_t>(i);

            // Issue read, then hold resp_ready low for a random interval
            noc_issue(3, laddr, false, sc_biguint<192>(0));
            noc_resp_ready_i[3].write(false);
            tick(bp_dist(rng));

            noc_resp_t resp = consume_noc_resp(3);
            SC_ASSERT( "TB", resp.code == SPM_RESPONSE_CODE::SPM_OK, "TC9-A: resp code wrong");

            uint64_t exp = 0xE900'0000'0000'0000ull | static_cast<uint64_t>(i);
            if (resp.rdata.range(63, 0).to_uint64() != exp) {
                std::ostringstream oss;
                oss << "TC9-A mismatch i=" << i
                    << " exp=0x" << std::hex << exp
                    << " got=0x" << resp.rdata.range(63, 0).to_uint64();
                SC_REPORT_ERROR("TB", oss.str().c_str());
            }
        }

        // ---- Phase B: NoC port-0 write, DMA read with random R-channel delay ----
        log("  Phase B: NoC write -> DMA read (random rready back-pressure)");

        const uint32_t lbase2 = 160;
        for (int i = 0; i < COUNT; ++i) {
            uint64_t pat = 0xD100'0000'0000'0000ull | static_cast<uint64_t>(i);
            noc_write(0, lbase2 + static_cast<uint32_t>(i), make192(pat, 0, 0));
            std::cout << "  NoC write: laddr=" << (lbase2 + i) << " data=0x" << std::hex << pat << "\n";
        }
        tick(4);

        for (int i = 0; i < COUNT; ++i) {
            uint32_t byte_addr = noc_to_dma_byte(0, lbase2 + static_cast<uint32_t>(i));

            // Issue AR request
            dma_araddr_i .write(byte_addr);
            dma_arvalid_i.write(true);
            dma_rready_i .write(false);   // back-pressure ON

            bool ar_ok = false;
            for (int j = 0; j < 300 && !ar_ok; ++j) {
                if (dma_arready_o.read()) { ar_ok = true; break;}
                tick(1);
            }
            SC_ASSERT( "TB", ar_ok, "TC9-B: AR timeout");
            tick(1);
            dma_arvalid_i.write(false);

            // Hold rready low for a random interval
            tick(bp_dist(rng));

            // Release rready and wait for data
            dma_rready_i.write(true);
            bool r_ok = false;
            for (int j = 0; j < 300 && !r_ok; ++j) {
                if (dma_rvalid_o.read()) r_ok = true;
                else tick(1);
            }
            SC_ASSERT( "TB", r_ok, "TC9-B: R timeout");

            uint64_t exp = 0xD100'0000'0000'0000ull | static_cast<uint64_t>(i);
            uint64_t got = dma_rdata_o.read().to_uint64();
            tick(1);
            dma_rready_i.write(false);

            if (got != exp) {
                std::ostringstream oss;
                oss << "TC9-B mismatch i=" << i
                    << " exp=0x" << std::hex << exp << " got=0x" << got;
                SC_REPORT_ERROR("TB", oss.str().c_str());
            }
        }

        log("  TC9 PASS");
    }

    // =========================================================================
    //  TC10 – Parallel <-> Linear Addressing Consistency
    //
    //  The two access modes must decompose and re-compose the same physical
    //  SRAM rows in a strictly defined bit-lane order:
    //
    //  Physical layout for parallel write to (Group g, row R):
    //    bank[g*3+0][row=R] = wdata[ 63: 0]   (k=0)
    //    bank[g*3+1][row=R] = wdata[127:64]   (k=1)
    //    bank[g*3+2][row=R] = wdata[191:128]  (k=2)
    //
    //  Linear read of (Group g, bank k, row R):
    //    local_addr = k * BANK_DEPTH + R
    //    → rdata[63:0] = bank[g*3+k][row=R] = wdata[64k+63 : 64k]
    //
    //  Sub-test A: parallel write → 3× linear reads
    //  Sub-test B: 3× linear writes → parallel read
    //
    //  Uses port 0 / Group 0, row ROW = 9.
    // =========================================================================
    void tc10_parallel_linear_consistency() {
        log("TC10: Parallel <-> Linear Addressing Consistency");

        constexpr int      PORT = 0;
        constexpr uint32_t ROW  = 9;
        constexpr uint32_t D    = P_BANK_DEPTH; // 256

        // Parallel-region local address for this row
        const uint32_t par_addr = GROUP_LINEAR + ROW;

        // Linear local addresses for each bank at the same row:
        //   bank k → local_addr = k*D + ROW
        const uint32_t lin[3] = { ROW, D + ROW, 2*D + ROW };

        // ----- Sub-test A: parallel write, then 3 linear reads -----
        log("  Sub-A: parallel write (192b) -> 3 linear reads (64b each)");

        const sc_biguint<192> pat_par = make192(
            0xA1B1'C1D1'E1F1'0101ull,   // → bank 0
            0xA2B2'C2D2'E2F2'0202ull,   // → bank 1
            0xA3B3'C3D3'E3F3'0303ull);  // → bank 2

        noc_write(PORT, par_addr, pat_par);
        tick(2);

        for (int k = 0; k < 3; ++k) {
            uint64_t exp = pat_par.range(64*k + 63, 64*k).to_uint64();
            uint64_t got = noc_read(PORT, lin[k]).range(63, 0).to_uint64();
            std::ostringstream oss;
            oss << "  bank" << k
                << " (laddr=" << lin[k] << ")"
                << " exp=0x" << std::hex << exp
                << " got=0x" << got;
            log(oss.str());
            if (got != exp) {
                SC_REPORT_ERROR("TB",
                    ("TC10-A: bank-" + std::to_string(k) + " linear-read mismatch").c_str());
            }
        }

        // ----- Sub-test B: 3 linear writes, then one parallel read -----
        log("  Sub-B: 3 linear writes (64b each) -> parallel read (192b)");

        const uint64_t bv[3] = {
            0x1234'5678'9ABC'DEF0ull,
            0xFEDC'BA98'7654'3210ull,
            0xC0FF'EE00'DEAD'BEEFull
        };

        for (int k = 0; k < 3; ++k)
            noc_write(PORT, lin[k], make192(bv[k], 0, 0));
        tick(2);

        sc_biguint<192> par_rd = noc_read(PORT, par_addr);
        for (int k = 0; k < 3; ++k) {
            uint64_t exp = bv[k];
            uint64_t got = par_rd.range(64*k + 63, 64*k).to_uint64();
            std::ostringstream oss;
            oss << "  bank" << k
                << " exp=0x" << std::hex << exp
                << " got=0x" << got;
            log(oss.str());
            if (got != exp) {
                SC_REPORT_ERROR("TB",
                    ("TC10-B: parallel[" + std::to_string(k) + "] mismatch").c_str());
            }
        }

        log("  TC10 PASS");
    }

    // =========================================================================
    //  TC11 – Same-group DMA/NoC Contention Priority + Response Integrity
    //
    //  Part A: 同 group、同時發出 NoC read 與 DMA read，驗證 NoC response
    //          先於 DMA read response 出現，符合目前仲裁順序
    //          NoC > DMA write > DMA read。
    //
    //  Part B: 同一個 NoC port 連續發出多筆 read outstanding，同時穿插 DMA
    //          write/read，驗證：
    //          1) NoC read response 無遺漏
    //          2) 回應順序與 issue 順序一致
    //          3) DMA read 亦可正確完成
    // =========================================================================
    void tc11_same_group_contention_integrity() {
        log("TC11: Same-group DMA/NoC contention priority + integrity");

        constexpr int PORT = 2;  // default map: port 2 -> group 2
        const uint32_t base_laddr = 64;

        // -----------------------------------------------------------------
        // Part A: same-cycle NoC read vs DMA read priority
        // -----------------------------------------------------------------
        log("  Part A: same-group concurrent NoC-read vs DMA-read priority");

        const uint32_t noc_laddr_a = base_laddr;
        const uint32_t dma_laddr_a = base_laddr + 1;
        const uint64_t noc_pat_a   = 0xC211'0000'0000'00A1ull;
        const uint64_t dma_pat_a   = 0xC211'0000'0000'00B2ull;
        const uint32_t dma_byte_a  = noc_to_dma_byte(PORT, dma_laddr_a);

        dma_write(noc_to_dma_byte(PORT, noc_laddr_a), noc_pat_a);
        dma_write(dma_byte_a, dma_pat_a);
        tick(2);

        // 同一拍送出 NoC read 與 DMA AR
        noc_req_t req{};
        req.addr  = noc_laddr_a;
        req.wen   = false;
        req.wdata = 0;
        noc_req_i[PORT].write(req);
        noc_req_valid_i[PORT].write(true);

        dma_araddr_i.write(dma_byte_a);
        dma_arvalid_i.write(true);
        dma_rready_i.write(false);  // 先觀察 valid 出現順序

        bool noc_req_ok = false;
        bool dma_ar_ok  = false;
        for (int i = 0; i < 80 && !(noc_req_ok && dma_ar_ok); ++i) {
            if (noc_req_ready_o[PORT].read()) noc_req_ok = true;
            if (dma_arready_o.read()) dma_ar_ok = true;
            tick(1);
            if (noc_req_ok) noc_req_valid_i[PORT].write(false);
            if (dma_ar_ok)  dma_arvalid_i.write(false);
        }
        SC_ASSERT("TB", noc_req_ok, "TC11-A: NoC read issue timeout");
        SC_ASSERT("TB", dma_ar_ok,  "TC11-A: DMA AR issue timeout");

        int noc_valid_cycle = -1;
        int dma_valid_cycle = -1;
        for (int i = 0; i < 120 && (noc_valid_cycle < 0 || dma_valid_cycle < 0); ++i) {
            if (noc_valid_cycle < 0 && noc_resp_valid_o[PORT].read()) {
                noc_valid_cycle = static_cast<int>(cycle());
            }
            if (dma_valid_cycle < 0 && dma_rvalid_o.read()) {
                dma_valid_cycle = static_cast<int>(cycle());
            }
            if (noc_valid_cycle >= 0 && dma_valid_cycle >= 0) break;
            tick(1);
        }

        SC_ASSERT("TB", noc_valid_cycle >= 0, "TC11-A: NoC response did not arrive");
        SC_ASSERT("TB", dma_valid_cycle >= 0, "TC11-A: DMA read response did not arrive");
        SC_ASSERT("TB", noc_valid_cycle <= dma_valid_cycle,
                  "TC11-A: expected NoC response to appear before or with DMA read response");

        noc_resp_t noc_resp_a = consume_noc_resp(PORT);
        SC_ASSERT("TB", noc_resp_a.code == SPM_RESPONSE_CODE::SPM_OK,
                  "TC11-A: NoC response code must be SPM_OK");
        SC_ASSERT("TB", noc_resp_a.rdata.range(63, 0).to_uint64() == noc_pat_a,
                  "TC11-A: NoC response data mismatch");

        dma_rready_i.write(true);
        SC_ASSERT("TB", wait_dma_rvalid(80), "TC11-A: DMA read valid missing after releasing rready");
        uint64_t dma_got_a = dma_rdata_o.read().to_uint64();
        tick(1);
        dma_rready_i.write(false);
        SC_ASSERT("TB", dma_got_a == dma_pat_a, "TC11-A: DMA read response data mismatch");

        {
            std::ostringstream oss;
            oss << "    priority-observed noc_valid_cycle=" << noc_valid_cycle
                << " dma_valid_cycle=" << dma_valid_cycle
                << " noc_data=0x" << std::hex << noc_resp_a.rdata.range(63, 0).to_uint64()
                << " dma_data=0x" << dma_got_a;
            log(oss.str());
        }

        // -----------------------------------------------------------------
        // Part B: NoC outstanding order + no-loss under DMA interleaving
        // -----------------------------------------------------------------
        log("  Part B: ordered NoC outstanding reads with interleaved DMA traffic");

        constexpr int COUNT = 4;
        std::vector<uint64_t> exp_noc(COUNT);
        std::vector<uint64_t> got_noc;
        got_noc.reserve(COUNT);

        const uint32_t noc_base_b = base_laddr + 16;
        for (int i = 0; i < COUNT; ++i) {
            exp_noc[i] = 0xC322'0000'0000'0000ull | static_cast<uint64_t>(i);
            dma_write(noc_to_dma_byte(PORT, noc_base_b + static_cast<uint32_t>(i)), exp_noc[i]);
        }
        tick(2);

        // 先累積多筆 NoC read request，不立即消費 response。
        for (int i = 0; i < COUNT; ++i) {
            noc_issue(PORT, noc_base_b + static_cast<uint32_t>(i), false, sc_biguint<192>(0));
        }

        // 穿插 DMA write/read，模擬 cluster-like contention。
        const uint32_t dma_mix_laddr = noc_base_b + 32;
        const uint64_t dma_mix_pat   = 0xC322'FFFF'0000'00AAull;
        dma_write(noc_to_dma_byte(PORT, dma_mix_laddr), dma_mix_pat);
        tick(1);
        uint64_t dma_mix_got = dma_read(noc_to_dma_byte(PORT, dma_mix_laddr));
        SC_ASSERT("TB", dma_mix_got == dma_mix_pat, "TC11-B: interleaved DMA round-trip mismatch");

        for (int i = 0; i < COUNT; ++i) {
            noc_resp_t resp = consume_noc_resp(PORT);
            SC_ASSERT("TB", resp.code == SPM_RESPONSE_CODE::SPM_OK,
                      "TC11-B: NoC response code must be SPM_OK");
            got_noc.push_back(resp.rdata.range(63, 0).to_uint64());
        }

        SC_ASSERT("TB", got_noc.size() == exp_noc.size(),
                  "TC11-B: NoC response count mismatch (possible packet loss)");
        for (int i = 0; i < COUNT; ++i) {
            if (got_noc[i] != exp_noc[i]) {
                std::ostringstream oss;
                oss << "TC11-B: NoC response order/data mismatch at i=" << i
                    << " exp=0x" << std::hex << exp_noc[i]
                    << " got=0x" << got_noc[i];
                SC_REPORT_ERROR("TB", oss.str().c_str());
            }
        }

        log("  TC11 PASS");
    }

    // =========================================================================
    // Top-Level Test Runner (SC_THREAD)
    // =========================================================================
    void run_all_tests() {
        // De-assert everything before asserting reset
        reset_n.write(false);
        pmu_rst_i.write(false);
        config_map_i.write(0xE4);   // default Port-to-Group map: 0→0, 1→1, 2→2, 3→3
        config_update_i.write(false);
        arb_policy_i.write(false);
        clear_noc();
        clear_dma();

        tick(4);
        reset_n.write(true);
        tick(3);

        log("==================================================");
        log("ScratchpadMemory Unit Test Suite");
        log("  BANK_DEPTH="    + std::to_string(P_BANK_DEPTH)   +
            "  GROUP_LINEAR=" + std::to_string(GROUP_LINEAR)    +
            "  GROUP_SPAN="   + std::to_string(GROUP_SPAN));
        log("==================================================");

        tc1_basic_reset();
        tc2_simple_dma_wr_rd();
        tc3_simple_noc_linear_wr_rd();
        tc4_simple_noc_parallel_wr_rd();
        tc5_dma_write_noc_read();
        tc6_noc_write_dma_read();
        tc7_multi_dma_burst();
        tc8_multi_noc_parallel_burst();
        tc9_mixed_random_backpressure();
        tc10_parallel_linear_consistency();
        tc11_same_group_contention_integrity();

        tick(4);
        log("==================================================");
        log("[PASS] All 11 test cases passed.");
        log("==================================================");

        sc_stop();
    }
};

// ============================================================================
// sc_main – instantiate and run the testbench
// ============================================================================
int sc_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    spm_tb tb("spm_tb");
    sc_start();

    return 0;
}
