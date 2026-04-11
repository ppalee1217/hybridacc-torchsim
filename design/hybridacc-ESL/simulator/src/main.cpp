/**
 * @file main.cpp
 * @brief hybridacc-sim — HybridAcc SoC simulator entry point.
 *
 * Integrates:
 *   - HybridAcc<1> (CoreController + CmdToAhbBridge + ComputeCluster)
 *   - FakeDram (AXI4 byte-addressable DRAM model)
 *   - BootTestDriver (host AXI4-Lite driver)
 *   - ELF32 loader + ManifestBuilder
 *
 * CLI:
 *   hybridacc-sim [-M dram_size(K/M/G)] [--mirror file.bin]
 *                 [--max-cycles N] [--core-debug] [--dma-check]
 *                 <firmware.elf>
 */

#include <systemc>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "HybridAcc.hpp"
#include "SimUtils.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc;
using namespace hybridacc::core;

// ============================================================================
// CLI helpers
// ============================================================================

static uint64_t parse_size(const std::string& s) {
    char* end = nullptr;
    uint64_t val = std::strtoull(s.c_str(), &end, 0);
    if (end && *end) {
        switch (*end) {
            case 'K': case 'k': val *= 1024; break;
            case 'M': case 'm': val *= 1024 * 1024; break;
            case 'G': case 'g': val *= 1024ULL * 1024 * 1024; break;
            default: break;
        }
    }
    return val;
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [-M dram_size] [--mirror file.bin] [--max-cycles N]"
              << " [--core-debug] [--dma-check] <firmware.elf>\n";
}

// ============================================================================
// DRAM mirror I/O — sparse format
//
// File layout: sequence of records, each:
//   [uint32_t addr] [uint32_t length] [length bytes of data]
// Terminated by addr=0 length=0 (8-byte sentinel).
//
// Contiguous byte runs in FakeDram::mem_ are coalesced into a single record.
// This avoids dumping gigabytes of zeros for unused DRAM regions.
// ============================================================================

static constexpr uint32_t kMirrorCoalesceGap = 64; // merge runs ≤ this gap apart
static constexpr uint32_t kSparseMagic   = 0x53505253u; // "SPRS" little-endian
static constexpr uint32_t kSparseVersion = 1u;

static void write_le32(std::ofstream& f, uint32_t v) {
    uint8_t buf[4] = {
        static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8),
        static_cast<uint8_t>(v >> 16),
        static_cast<uint8_t>(v >> 24),
    };
    f.write(reinterpret_cast<const char*>(buf), 4);
}

static uint32_t read_le32(std::ifstream& f) {
    uint8_t buf[4];
    f.read(reinterpret_cast<char*>(buf), 4);
    return static_cast<uint32_t>(buf[0])
         | (static_cast<uint32_t>(buf[1]) << 8)
         | (static_cast<uint32_t>(buf[2]) << 16)
         | (static_cast<uint32_t>(buf[3]) << 24);
}

static bool load_mirror(FakeDram& dram, const std::string& path,
                        uint64_t dram_size, uint32_t base = 0) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "[SIM] Cannot open mirror: " << path << "\n"; return false; }
    auto file_size = static_cast<uint64_t>(f.tellg());
    f.seekg(0);

    // Detect format: sparse files start with magic "SPRS" header.
    bool is_sparse = false;
    if (file_size >= 8) {
        uint32_t magic = read_le32(f);
        is_sparse = (magic == kSparseMagic);
        f.seekg(0);
    }

    if (!is_sparse) {
        // Legacy flat format: load entire file at base
        if (file_size > dram_size) {
            std::cerr << "[SIM] ERROR: mirror file (" << file_size
                      << " bytes) exceeds DRAM capacity (" << dram_size
                      << " bytes, -M). Aborting load.\n";
            return false;
        }
        std::vector<uint8_t> buf(static_cast<size_t>(file_size));
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(file_size));
        dram.load_bytes(base, buf.data(), buf.size());
        std::cout << "[SIM] Loaded mirror (flat): " << path
                  << " (" << file_size << " bytes at 0x"
                  << std::hex << base << std::dec << ")\n";
        return true;
    }

    // Sparse format: skip 8-byte header (magic + version)
    f.seekg(8);
    uint64_t total_bytes = 0;
    uint32_t num_records = 0;
    while (f.good() && static_cast<uint64_t>(f.tellg()) + 8 <= file_size) {
        uint32_t addr = read_le32(f);
        uint32_t len  = read_le32(f);
        if (addr == 0 && len == 0) break;  // sentinel
        if (static_cast<uint64_t>(f.tellg()) + len > file_size) {
            std::cerr << "[SIM] WARNING: truncated mirror record at offset "
                      << f.tellg() << "\n";
            break;
        }
        std::vector<uint8_t> buf(len);
        f.read(reinterpret_cast<char*>(buf.data()), len);
        dram.load_bytes(addr, buf.data(), buf.size());
        total_bytes += len;
        ++num_records;
    }
    std::cout << "[SIM] Loaded mirror (sparse): " << path
              << " (" << num_records << " records, "
              << total_bytes << " data bytes)\n";
    return true;
}

static void dump_mirror(const FakeDram& dram, const std::string& path,
                        uint64_t /*dram_size*/, uint32_t /*base*/ = 0) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::cerr << "[SIM] Cannot write mirror: " << path << "\n"; return; }

    // Write sparse format header
    write_le32(f, kSparseMagic);
    write_le32(f, kSparseVersion);

    // Walk the sparse map, coalescing contiguous (or near-contiguous) runs
    const auto& mem = dram.mem_;
    if (mem.empty()) {
        write_le32(f, 0); write_le32(f, 0);  // sentinel
        std::cout << "[SIM] Dumped mirror (sparse): " << path << " (empty)\n";
        return;
    }

    uint64_t total_bytes = 0;
    uint32_t num_records = 0;

    auto it = mem.begin();
    uint32_t run_start = it->first;
    uint32_t run_end   = it->first + 1;  // exclusive
    ++it;

    auto flush_run = [&]() {
        uint32_t len = run_end - run_start;
        write_le32(f, run_start);
        write_le32(f, len);
        for (uint32_t a = run_start; a < run_end; ++a) {
            uint8_t b = dram.read_byte(a);
            f.write(reinterpret_cast<const char*>(&b), 1);
        }
        total_bytes += len;
        ++num_records;
    };

    for (; it != mem.end(); ++it) {
        uint32_t addr = it->first;
        if (addr <= run_end + kMirrorCoalesceGap) {
            // Extend current run (include small gaps as zeros)
            run_end = addr + 1;
        } else {
            flush_run();
            run_start = addr;
            run_end   = addr + 1;
        }
    }
    flush_run();  // last run

    // Sentinel
    write_le32(f, 0);
    write_le32(f, 0);

    std::cout << "[SIM] Dumped mirror (sparse): " << path
              << " (" << num_records << " records, "
              << total_bytes << " data bytes, file ~"
              << (total_bytes + num_records * 8 + 8) << " bytes)\n";
}

// ============================================================================
// sc_main
// ============================================================================

static constexpr unsigned NUM_CLUSTERS = 1;

// DRAM addresses for manifest / payload
static constexpr uint32_t kManifestDramAddr = 0x80000000;
static constexpr uint32_t kPayloadDramBase  = 0x80010000;

// DMA test addresses (matching firmware expectations)
static constexpr uint32_t kTestDramSrc = 0x80020000;
static constexpr uint32_t kTestDramDst = 0x80030000;
static constexpr uint32_t kTestBytes   = 64;

int sc_main(int argc, char* argv[]) {
    // ========================================================================
    // Parse CLI
    // ========================================================================

    std::string elf_path;
    std::string mirror_path;
    uint64_t dram_size = 256ULL * 1024 * 1024; // 256 MB default
    uint32_t max_cycles = 500000;
    bool core_debug = false;
    bool dma_check = false;
    bool fw_check = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-M" && i + 1 < argc) {
            dram_size = parse_size(argv[++i]);
        } else if (arg == "--mirror" && i + 1 < argc) {
            mirror_path = argv[++i];
        } else if (arg == "--max-cycles" && i + 1 < argc) {
            max_cycles = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--core-debug") {
            core_debug = true;
        } else if (arg == "--fw-check") {
            fw_check = true;
        } else if (arg == "--dma-check") {
            dma_check = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (elf_path.empty()) {
            elf_path = arg;
        } else {
            std::cerr << "[SIM] Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (elf_path.empty()) {
        std::cerr << "[SIM] Error: firmware ELF path required\n";
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "[SIM] === HybridAcc SoC Simulator ===" << std::endl;
    std::cout << "[SIM] DRAM size: " << (dram_size / 1024) << " KB" << std::endl;
    std::cout << "[SIM] Max cycles: " << max_cycles << std::endl;
    std::cout << "[SIM] Clusters: " << NUM_CLUSTERS << std::endl;

    // ========================================================================
    // Load ELF
    // ========================================================================

    uint32_t entry_point = 0;
    std::vector<elf::Segment> elf_segs;
    if (!elf::load_elf(elf_path, entry_point, elf_segs)) return 1;

    std::cout << "[SIM] ELF: " << elf_path << std::endl;
    std::cout << "[SIM] Entry: 0x" << std::hex << entry_point << std::dec << std::endl;
    for (size_t i = 0; i < elf_segs.size(); ++i) {
        std::cout << "[SIM]   seg[" << i << "] paddr=0x" << std::hex
                  << elf_segs[i].paddr << " memsz=0x" << elf_segs[i].memsz
                  << std::dec << std::endl;
    }

    // ========================================================================
    // Signals
    // ========================================================================

    sc_clock clk("clk", 10, SC_NS);
    sc_signal<bool> reset_n("reset_n");
    sc_signal<bool> controller_irq("controller_irq");

    // Host AXI4-Lite
    sc_signal<bool>         h_aw_valid, h_aw_ready;
    sc_signal<sc_uint<32>>  h_aw_addr;
    sc_signal<bool>         h_w_valid, h_w_ready;
    sc_signal<sc_uint<32>>  h_w_data;
    sc_signal<sc_uint<4>>   h_w_strb;
    sc_signal<bool>         h_b_valid, h_b_ready;
    sc_signal<sc_uint<2>>   h_b_resp;
    sc_signal<bool>         h_ar_valid, h_ar_ready;
    sc_signal<sc_uint<32>>  h_ar_addr;
    sc_signal<bool>         h_r_valid, h_r_ready;
    sc_signal<sc_uint<32>>  h_r_data;
    sc_signal<sc_uint<2>>   h_r_resp;

    // DRAM AXI (64-bit data width)
    sc_signal<bool>         mem_aw_valid, mem_aw_ready;
    sc_signal<sc_uint<32>>  mem_aw_addr;
    sc_signal<sc_uint<8>>   mem_aw_len;
    sc_signal<bool>         mem_w_valid, mem_w_ready;
    sc_signal<sc_biguint<kMemAxiDataWidth>> mem_w_data;
    sc_signal<sc_uint<kMemAxiDataWidth / 8>> mem_w_strb;
    sc_signal<bool>         mem_w_last;
    sc_signal<bool>         mem_b_valid, mem_b_ready;
    sc_signal<sc_uint<2>>   mem_b_resp;
    sc_signal<bool>         mem_ar_valid, mem_ar_ready;
    sc_signal<sc_uint<32>>  mem_ar_addr;
    sc_signal<sc_uint<8>>   mem_ar_len;
    sc_signal<bool>         mem_r_valid, mem_r_ready;
    sc_signal<sc_biguint<kMemAxiDataWidth>> mem_r_data;
    sc_signal<sc_uint<2>>   mem_r_resp;
    sc_signal<bool>         mem_r_last;

    // ========================================================================
    // DUT: HybridAcc<1>
    // ========================================================================

    HybridAcc<NUM_CLUSTERS> dut("hybridacc");
    dut.core_debug = core_debug;
    dut.core_ctrl.core_debug = core_debug;
    dut.clk(clk);
    dut.reset_n(reset_n);

    // Host AXI
    dut.s_ctrl_aw_valid_i(h_aw_valid);
    dut.s_ctrl_aw_ready_o(h_aw_ready);
    dut.s_ctrl_aw_addr_i(h_aw_addr);
    dut.s_ctrl_w_valid_i(h_w_valid);
    dut.s_ctrl_w_ready_o(h_w_ready);
    dut.s_ctrl_w_data_i(h_w_data);
    dut.s_ctrl_w_strb_i(h_w_strb);
    dut.s_ctrl_b_valid_o(h_b_valid);
    dut.s_ctrl_b_ready_i(h_b_ready);
    dut.s_ctrl_b_resp_o(h_b_resp);
    dut.s_ctrl_ar_valid_i(h_ar_valid);
    dut.s_ctrl_ar_ready_o(h_ar_ready);
    dut.s_ctrl_ar_addr_i(h_ar_addr);
    dut.s_ctrl_r_valid_o(h_r_valid);
    dut.s_ctrl_r_ready_i(h_r_ready);
    dut.s_ctrl_r_data_o(h_r_data);
    dut.s_ctrl_r_resp_o(h_r_resp);

    // DRAM AXI
    dut.m_mem_axi_aw_valid_o(mem_aw_valid);
    dut.m_mem_axi_aw_ready_i(mem_aw_ready);
    dut.m_mem_axi_aw_addr_o(mem_aw_addr);
    dut.m_mem_axi_aw_len_o(mem_aw_len);
    dut.m_mem_axi_w_valid_o(mem_w_valid);
    dut.m_mem_axi_w_ready_i(mem_w_ready);
    dut.m_mem_axi_w_data_o(mem_w_data);
    dut.m_mem_axi_w_strb_o(mem_w_strb);
    dut.m_mem_axi_w_last_o(mem_w_last);
    dut.m_mem_axi_b_valid_i(mem_b_valid);
    dut.m_mem_axi_b_ready_o(mem_b_ready);
    dut.m_mem_axi_b_resp_i(mem_b_resp);
    dut.m_mem_axi_ar_valid_o(mem_ar_valid);
    dut.m_mem_axi_ar_ready_i(mem_ar_ready);
    dut.m_mem_axi_ar_addr_o(mem_ar_addr);
    dut.m_mem_axi_ar_len_o(mem_ar_len);
    dut.m_mem_axi_r_valid_i(mem_r_valid);
    dut.m_mem_axi_r_ready_o(mem_r_ready);
    dut.m_mem_axi_r_data_i(mem_r_data);
    dut.m_mem_axi_r_resp_i(mem_r_resp);
    dut.m_mem_axi_r_last_i(mem_r_last);

    // IRQ
    dut.controller_irq_o(controller_irq);

    // ========================================================================
    // FakeDram
    // ========================================================================

    FakeDram dram("fake_dram", dram_size, kManifestDramAddr);
    dram.clk(clk);
    dram.reset_n(reset_n);
    dram.aw_valid(mem_aw_valid);
    dram.aw_ready(mem_aw_ready);
    dram.aw_addr(mem_aw_addr);
    dram.aw_len(mem_aw_len);
    dram.w_valid(mem_w_valid);
    dram.w_ready(mem_w_ready);
    dram.w_data(mem_w_data);
    dram.w_strb(mem_w_strb);
    dram.w_last(mem_w_last);
    dram.b_valid(mem_b_valid);
    dram.b_ready(mem_b_ready);
    dram.b_resp(mem_b_resp);
    dram.ar_valid(mem_ar_valid);
    dram.ar_ready(mem_ar_ready);
    dram.ar_addr(mem_ar_addr);
    dram.ar_len(mem_ar_len);
    dram.r_valid(mem_r_valid);
    dram.r_ready(mem_r_ready);
    dram.r_data(mem_r_data);
    dram.r_resp(mem_r_resp);
    dram.r_last(mem_r_last);

    // ========================================================================
    // BootTestDriver
    // ========================================================================

    BootTestDriver driver("driver");
    driver.clk(clk);
    driver.reset_n(reset_n);
    driver.aw_valid_o(h_aw_valid);
    driver.aw_ready_i(h_aw_ready);
    driver.aw_addr_o(h_aw_addr);
    driver.w_valid_o(h_w_valid);
    driver.w_ready_i(h_w_ready);
    driver.w_data_o(h_w_data);
    driver.w_strb_o(h_w_strb);
    driver.b_valid_i(h_b_valid);
    driver.b_ready_o(h_b_ready);
    driver.b_resp_i(h_b_resp);
    driver.ar_valid_o(h_ar_valid);
    driver.ar_ready_i(h_ar_ready);
    driver.ar_addr_o(h_ar_addr);
    driver.r_valid_i(h_r_valid);
    driver.r_ready_o(h_r_ready);
    driver.r_data_i(h_r_data);
    driver.r_resp_i(h_r_resp);
    driver.controller_irq_i(controller_irq);

    // ========================================================================
    // Load DRAM mirror (if specified)
    // ========================================================================

    if (!mirror_path.empty()) {
        if (!load_mirror(dram, mirror_path, dram_size, kManifestDramAddr)) return 1;
    }

    // ========================================================================
    // Build manifest from ELF segments
    // ========================================================================

    ManifestBuilder builder(&dram, kManifestDramAddr, kPayloadDramBase);

    for (const auto& seg : elf_segs) {
        SectionKind kind;
        uint32_t local_addr;

        if (seg.paddr >= kBaseInstRam && seg.paddr < kBaseInstRam + kIsramBytes) {
            kind = SectionKind::CORE;
            local_addr = seg.paddr - kBaseInstRam;
        } else if (seg.paddr >= kBaseDataRam && seg.paddr < kBaseDataRam + kDataSramBytes) {
            kind = SectionKind::JOB;
            local_addr = seg.paddr - kBaseDataRam;
        } else {
            std::cout << "[SIM] WARNING: Skipping segment paddr=0x"
                      << std::hex << seg.paddr << std::dec << std::endl;
            continue;
        }

        builder.add_section(kind, local_addr,
                            seg.data.data(),
                            static_cast<uint32_t>(seg.memsz));
        std::cout << "[SIM] Manifest entry: kind="
                  << static_cast<int>(kind) << " local=0x" << std::hex
                  << local_addr << " size=0x" << seg.memsz
                  << std::dec << std::endl;
    }

    builder.finalize();
    std::cout << "[SIM] Manifest: " << builder.entries.size()
              << " entries at DRAM 0x" << std::hex << kManifestDramAddr
              << std::dec << std::endl;

    // Pre-load DMA test pattern (for dma-check mode)
    if (dma_check) {
        std::cout << "[SIM] Pre-loading DMA test pattern at 0x"
                  << std::hex << kTestDramSrc << std::dec << std::endl;
        for (uint32_t i = 0; i < kTestBytes / 4; ++i) {
            dram.store32(kTestDramSrc + i * 4, i + 1);
        }
    }

    // Configure driver
    driver.manifest_dram_addr = kManifestDramAddr;
    driver.manifest_num_entries = static_cast<uint32_t>(builder.entries.size());
    driver.boot_addr = entry_point;
    driver.max_cycles = max_cycles;

    // ========================================================================
    // Power-on clusters before simulation
    // ========================================================================

    for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
        dut.set_cluster_power_enable(c, true);
    }

    // ========================================================================
    // Reset and run
    // ========================================================================

    reset_n.write(false);
    sc_start(50, SC_NS);
    reset_n.write(true);

    std::cout << "[SIM] Reset released"
              << (core_debug ? " (core-debug ON)" : "") << std::endl;
    sc_start();

    std::cout << "[SIM] Simulation ended at " << sc_time_stamp() << std::endl;
    dram.print_oob_summary();

    // ========================================================================
    // Post-simulation: dump mirror if specified
    // ========================================================================

    if (!mirror_path.empty()) {
        dump_mirror(dram, mirror_path + ".out", dram_size, kManifestDramAddr);
    }

    // ========================================================================
    // Post-simulation: verify firmware test results
    // ========================================================================

    bool fw_ok = true;
    if (fw_check) {
        uint32_t t_total = dut.dsram_read_word(0x00);
        uint32_t t_pass  = dut.dsram_read_word(0x04);
        uint32_t t_fail  = dut.dsram_read_word(0x08);
        uint32_t t_ff    = dut.dsram_read_word(0x0C);

        // Detect uninitialised DSRAM
        const uint32_t kSentinel = 0xCAFECAFE;
        if (t_total == kSentinel && t_pass == kSentinel &&
            t_fail == kSentinel && t_ff == kSentinel) {
            t_total = t_pass = t_fail = t_ff = 0;
        }

        std::cout << "\n[SIM] === Firmware Test Results ===" << std::endl;
        std::cout << "[SIM] Total: " << t_total
                  << "  Pass: " << t_pass
                  << "  Fail: " << t_fail
                  << "  First-fail: " << t_ff << std::endl;
        fw_ok = (t_fail == 0);
    }

    // DMA verification (if enabled)
    bool dram_ok = true;
    if (dma_check) {
        std::cout << "\n[SIM] === DMA Loopback Verification ===" << std::endl;
        for (uint32_t i = 0; i < kTestBytes / 4; ++i) {
            uint32_t expected = i + 1;  // loopback: dst should match src pattern
            uint32_t actual = dram.read_word(kTestDramDst + i * 4);
            if (actual != expected) {
                std::cout << "[SIM] FAIL: dram[0x" << std::hex
                          << (kTestDramDst + i * 4) << "] = 0x" << actual
                          << " expected 0x" << expected << std::dec << std::endl;
                dram_ok = false;
            }
        }
        if (dram_ok) std::cout << "[SIM] DMA loopback (DRAM→SPM→DRAM): PASS" << std::endl;
    }

    // Summary
    std::cout << "\n[SIM] === OVERALL ===" << std::endl;
    bool all_pass = fw_ok && dram_ok;
    std::cout << "[SIM] " << (all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
              << std::endl;

    return all_pass ? 0 : 1;
}
