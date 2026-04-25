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
 *                 [--fast-boot]
 *                 [--trace file.json] [--trace-level N]
 *                 <firmware.elf>
 */

#include <systemc>
#include <array>
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

static bool parse_trace_level(const std::string& s, uint32_t& out_level) {
    char* end = nullptr;
    unsigned long val = std::strtoul(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || val < 1 || val > 4) {
        return false;
    }
    out_level = static_cast<uint32_t>(val);
    return true;
}

static bool parse_positive_double(const std::string& s, double& out_value) {
    char* end = nullptr;
    double value = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0' || value <= 0.0) {
        return false;
    }
    out_value = value;
    return true;
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [-M dram_size] [--mirror file.bin] [--max-cycles N]"
              << " [--clock-period ns]"
              << " [--core-debug] [--dma-check] [--fast-boot]"
              << " [--trace file.json] [--trace-level N]"
              << " <firmware.elf>\n";
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
static constexpr uint32_t kClusterTraceStartPid = 100;
static constexpr uint32_t kClusterTraceStartTid = 1000;

// DRAM addresses for manifest / payload
static constexpr uint32_t kManifestDramAddr = 0x80000000;
static constexpr uint32_t kPayloadDramBase  = 0x80010000;

// DMA test addresses (matching firmware expectations)
static constexpr uint32_t kTestDramSrc = 0x80020000;
static constexpr uint32_t kTestDramDst = 0x80030000;
static constexpr uint32_t kTestBytes   = 64;
static constexpr uint32_t kPadTestDramSrc = 0x80040000;
static constexpr uint32_t kPadTestDramDst = 0x80050000;
static constexpr uint32_t kReluTestDramSrc = 0x80060000;
static constexpr uint32_t kReluTestDramDst = 0x80070000;

static constexpr std::array<uint64_t, 4> kPadTestSrcBeats = {
    0x1111111111111111ull,
    0x2222222222222222ull,
    0x3333333333333333ull,
    0x4444444444444444ull,
};

static constexpr std::array<uint64_t, 16> kPadExpectedBeats = {
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x1111111111111111ull, 0x2222222222222222ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x3333333333333333ull, 0x4444444444444444ull, 0x0000000000000000ull,
    0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
};

static constexpr std::array<uint64_t, 4> kReluTestSrcBeats = {
    0xC0003C003800BC00ull,
    0x04003E00B8008000ull,
    0xB40000004400FC00ull,
    0x7BFF3C01BA004200ull,
};

static constexpr std::array<uint64_t, 4> kReluExpectedBeats = {
    0x00003C0038000000ull,
    0x04003E0000000000ull,
    0x0000000044000000ull,
    0x7BFF3C0100004200ull,
};

static void store_u64(FakeDram& dram, uint32_t addr, uint64_t value) {
    dram.store32(addr + 0, static_cast<uint32_t>(value & 0xFFFFFFFFu));
    dram.store32(addr + 4, static_cast<uint32_t>(value >> 32));
}

static uint64_t read_u64(const FakeDram& dram, uint32_t addr) {
    return uint64_t(dram.read_word(addr)) |
           (uint64_t(dram.read_word(addr + 4)) << 32);
}

template <size_t N>
static bool verify_dram_beats(const FakeDram& dram,
                              const std::string& label,
                              uint32_t base_addr,
                              const std::array<uint64_t, N>& expected) {
    bool ok = true;
    std::cout << "\n[SIM] === " << label << " ===" << std::endl;
    for (size_t i = 0; i < N; ++i) {
        const uint64_t actual = read_u64(dram, base_addr + static_cast<uint32_t>(i * 8));
        if (actual != expected[i]) {
            std::cout << "[SIM] FAIL: dram[0x" << std::hex
                      << (base_addr + static_cast<uint32_t>(i * 8))
                      << "] = 0x" << actual
                      << " expected 0x" << expected[i]
                      << std::dec << std::endl;
            ok = false;
        }
    }
    if (ok) {
        std::cout << "[SIM] " << label << ": PASS" << std::endl;
    }
    return ok;
}

int sc_main(int argc, char* argv[]) {
    // ========================================================================
    // Parse CLI
    // ========================================================================

    std::string elf_path;
    std::string mirror_path;
    uint64_t dram_size = 256ULL * 1024 * 1024; // 256 MB default
    uint32_t max_cycles = 500000;
    double clock_period_ns = 2.0;
    bool core_debug = false;
    bool dma_check = false;
    bool fw_check = false;
    bool fast_boot = false;
    std::string trace_path;
    uint32_t trace_level = 2;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-M" && i + 1 < argc) {
            dram_size = parse_size(argv[++i]);
        } else if (arg == "--mirror" && i + 1 < argc) {
            mirror_path = argv[++i];
        } else if (arg == "--max-cycles" && i + 1 < argc) {
            max_cycles = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--clock-period" && i + 1 < argc) {
            if (!parse_positive_double(argv[++i], clock_period_ns)) {
                std::cerr << "[SIM] Invalid clock period: expected positive floating-point ns\n";
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--core-debug") {
            core_debug = true;
        } else if (arg == "--fw-check") {
            fw_check = true;
        } else if (arg == "--dma-check") {
            dma_check = true;
        } else if (arg == "--fast-boot") {
            fast_boot = true;
        } else if (arg == "--trace" && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (arg == "--trace-level" && i + 1 < argc) {
            if (!parse_trace_level(argv[++i], trace_level)) {
                std::cerr << "[SIM] Invalid trace level: expected 1..4\n";
                print_usage(argv[0]);
                return 1;
            }
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
    std::cout << "[SIM] Clock period: " << std::fixed << std::setprecision(3)
              << clock_period_ns << " ns" << std::endl;
    std::cout << "[SIM] Clusters: " << NUM_CLUSTERS << std::endl;
    std::cout << "[SIM] Fast boot: " << (fast_boot ? "enabled" : "disabled") << std::endl;

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

    const sc_time clock_period(clock_period_ns, SC_NS);
    const sc_time reset_time(clock_period_ns * 5.0, SC_NS);

    sc_clock clk("clk", clock_period);
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

    auto classify_segment = [](const elf::Segment& seg,
                               SectionKind& kind,
                               uint32_t& local_addr) -> bool {
        if (seg.paddr >= kBaseInstRam && seg.paddr < kBaseInstRam + kIsramBytes) {
            kind = SectionKind::CORE;
            local_addr = seg.paddr - kBaseInstRam;
            return true;
        }
        if (seg.paddr >= kBaseDataRam && seg.paddr < kBaseDataRam + kDataSramBytes) {
            kind = SectionKind::JOB;
            local_addr = seg.paddr - kBaseDataRam;
            return true;
        }
        return false;
    };

    if (fast_boot) {
        for (const auto& seg : elf_segs) {
            SectionKind kind;
            uint32_t local_addr;

            if (!classify_segment(seg, kind, local_addr)) {
                std::cout << "[SIM] WARNING: Skipping segment paddr=0x"
                          << std::hex << seg.paddr << std::dec << std::endl;
                continue;
            }

            if (!dut.fast_load_section(kind,
                                       local_addr,
                                       seg.data.data(),
                                       static_cast<uint32_t>(seg.memsz))) {
                std::cerr << "[SIM] ERROR: Fast boot preload failed for segment paddr=0x"
                          << std::hex << seg.paddr << std::dec << std::endl;
                return 1;
            }

            std::cout << "[SIM] Fast-boot preload: kind="
                      << static_cast<int>(kind) << " local=0x" << std::hex
                      << local_addr << " size=0x" << seg.memsz
                      << std::dec << std::endl;
        }
    } else {
        // ====================================================================
        // Build manifest from ELF segments
        // ====================================================================

        ManifestBuilder builder(&dram, kManifestDramAddr, kPayloadDramBase);

        for (const auto& seg : elf_segs) {
            SectionKind kind;
            uint32_t local_addr;

            if (!classify_segment(seg, kind, local_addr)) {
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

        driver.manifest_dram_addr = kManifestDramAddr;
        driver.manifest_num_entries = static_cast<uint32_t>(builder.entries.size());
    }

    // Pre-load DMA test pattern (for dma-check mode)
    if (dma_check) {
        std::cout << "[SIM] Pre-loading DMA test pattern at 0x"
                  << std::hex << kTestDramSrc << std::dec << std::endl;
        for (uint32_t i = 0; i < kTestBytes / 4; ++i) {
            dram.store32(kTestDramSrc + i * 4, i + 1);
        }

        std::cout << "[SIM] Pre-loading DMA padding source at 0x"
                  << std::hex << kPadTestDramSrc << std::dec << std::endl;
        for (size_t i = 0; i < kPadTestSrcBeats.size(); ++i) {
            store_u64(dram, kPadTestDramSrc + static_cast<uint32_t>(i * 8), kPadTestSrcBeats[i]);
        }

        std::cout << "[SIM] Pre-loading DMA ReLU source at 0x"
                  << std::hex << kReluTestDramSrc << std::dec << std::endl;
        for (size_t i = 0; i < kReluTestSrcBeats.size(); ++i) {
            store_u64(dram, kReluTestDramSrc + static_cast<uint32_t>(i * 8), kReluTestSrcBeats[i]);
        }
    }

    // Configure driver
    driver.boot_addr = entry_point;
    driver.max_cycles = max_cycles;
    driver.fast_boot = fast_boot;
    driver.skip_run = fast_boot;

    // ========================================================================
    // Power-on clusters before simulation
    // ========================================================================

    for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
        dut.set_cluster_power_enable(c, true);
    }

    // ========================================================================
    // Trace setup
    // ========================================================================
    if (!trace_path.empty()) {
        PerfettoTrace::getInstance().setLevel(trace_level);
        PerfettoTrace::getInstance().open(trace_path);
        dut.enable_perffeto_trace(kClusterTraceStartPid, kClusterTraceStartTid);

        std::cout << "[SIM] Trace enabled: " << trace_path
                  << " (level " << trace_level << ")" << std::endl;

        // Host-side trace lanes use fixed pid/tid values.
        TRACE_THREAD_NAME(0, 0, "CPU Pipeline");
        TRACE_THREAD_NAME(0, 1, "DMA Engine");
        TRACE_THREAD_NAME(0, 2, "DRAM");
    }

    // ========================================================================
    // Reset and run
    // ========================================================================

    reset_n.write(false);
    sc_start(reset_time);
    reset_n.write(true);

    std::cout << "[SIM] Reset released"
              << (core_debug ? " (core-debug ON)" : "") << std::endl;

    if (fast_boot) {
        uint32_t cycle = 0;

        std::cout << "[TB] === Phase 1: Fast local preload ===" << std::endl;
        std::cout << "[TB] Loader bypassed; local SRAM already preloaded" << std::endl;
        std::cout << "[TB] === Phase 2: Enable core ===" << std::endl;
        dut.fast_boot_start(entry_point);
        std::cout << "[TB] Core enabled directly, boot_addr=0x" << std::hex
                  << entry_point << std::dec << std::endl;

        while (!dut.core_halted() && cycle < max_cycles) {
            sc_start(clock_period);
            ++cycle;
        }

        if (cycle >= max_cycles) {
            std::cerr << "[TB] ERROR: Timeout waiting for EBREAK halt" << std::endl;
            std::cerr << "[TB] Final IRQ_SUMMARY=0x" << std::hex
                      << dut.irq_summary() << std::dec << std::endl;
            driver.run_timed_out = true;
        } else {
            std::cout << "[TB] === Simulation complete ===" << std::endl;
            std::cout << "[TB] EBREAK at cycle " << cycle
                      << ", IRQ_SUMMARY=0x" << std::hex << dut.irq_summary()
                      << std::dec << std::endl;
            driver.run_passed = true;
        }
    } else {
        sc_start();
    }

    const uint64_t cluster_run_cycles = dut.cluster_run_cycles(0);
    const uint64_t dma_active_cycles = dut.dma_active_cycles();
    const uint64_t overlap_cycles = dut.cluster_dma_overlap_cycles(0);
    const double overlap_vs_compute_pct = cluster_run_cycles == 0
        ? 0.0
        : (100.0 * static_cast<double>(overlap_cycles) / static_cast<double>(cluster_run_cycles));
    const double overlap_vs_dma_pct = dma_active_cycles == 0
        ? 0.0
        : (100.0 * static_cast<double>(overlap_cycles) / static_cast<double>(dma_active_cycles));

    std::cout << "[SIM] Cluster RUN cycles source: HDDU/AGU busy" << std::endl;
    std::cout << "[SIM] Cluster RUN cycles: " << cluster_run_cycles << std::endl;
    std::cout << "[SIM] DMA active cycles: " << dma_active_cycles << std::endl;
    std::cout << "[SIM] Compute/DMA overlap cycles: " << overlap_cycles << std::endl;
    std::cout << std::fixed << std::setprecision(2)
              << "[SIM] Compute/DMA overlap ratio (vs compute): "
              << overlap_vs_compute_pct << "%" << std::endl
              << "[SIM] Compute/DMA overlap ratio (vs DMA): "
              << overlap_vs_dma_pct << "%" << std::endl
              << std::defaultfloat;
    std::cout << "[SIM] Simulation ended at " << sc_time_stamp() << std::endl;
    dram.print_oob_summary();

    // ========================================================================
    // Write trace file
    // ========================================================================
    if (!trace_path.empty()) {
        PerfettoTrace::getInstance().close();
        std::cout << "[SIM] Trace written to " << trace_path << std::endl;
    }

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
        bool loopback_ok = true;
        std::cout << "\n[SIM] === DMA Loopback Verification ===" << std::endl;
        for (uint32_t i = 0; i < kTestBytes / 4; ++i) {
            uint32_t expected = i + 1;  // loopback: dst should match src pattern
            uint32_t actual = dram.read_word(kTestDramDst + i * 4);
            if (actual != expected) {
                std::cout << "[SIM] FAIL: dram[0x" << std::hex
                          << (kTestDramDst + i * 4) << "] = 0x" << actual
                          << " expected 0x" << expected << std::dec << std::endl;
                loopback_ok = false;
            }
        }
        if (loopback_ok) {
            std::cout << "[SIM] DMA loopback (DRAM→SPM→DRAM): PASS" << std::endl;
        }

        const bool pad_ok = verify_dram_beats(dram,
                                              "DMA load-pad writeback verification",
                                              kPadTestDramDst,
                                              kPadExpectedBeats);
        const bool relu_ok = verify_dram_beats(dram,
                                               "DMA store-ReLU verification",
                                               kReluTestDramDst,
                                               kReluExpectedBeats);
        dram_ok = loopback_ok && pad_ok && relu_ok;
    }

    // Summary
    std::cout << "\n[SIM] === OVERALL ===" << std::endl;
    bool driver_ok = driver.run_passed && !driver.run_timed_out;
    if (!driver_ok) {
        std::cout << "[SIM] Boot/EBREAK run status: FAIL"
                  << (driver.run_timed_out ? " (timeout)" : "")
                  << std::endl;
    }
    bool all_pass = driver_ok && fw_ok && dram_ok;
    std::cout << "[SIM] " << (all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
              << std::endl;

    return all_pass ? 0 : 1;
}
