/**
 * @file SimUtils.hpp
 * @brief Shared simulation utilities for HybridAcc SoC simulator.
 *
 * Contains:
 *   - Minimal ELF32 loader
 *   - FakeDram (AXI4 byte-addressable DRAM model)
 *   - ManifestBuilder (section loader manifest generator)
 *   - BootTestDriver (host AXI4-Lite driver: boot → loader → core → EBREAK)
 */

#pragma once

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include <systemc>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "Core/CoreController.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc::core;

// ============================================================================
// Minimal ELF32 parser (loads PT_LOAD segments into a flat byte map)
// ============================================================================

namespace elf {

#pragma pack(push, 1)
struct Elf32_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf32_Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};
#pragma pack(pop)

static constexpr uint32_t PT_LOAD = 1;

struct Segment {
    uint32_t paddr;
    std::vector<uint8_t> data;
    uint32_t memsz;
};

/// Load ELF file, return entry point and segments
inline bool load_elf(const std::string& path,
                     uint32_t& entry,
                     std::vector<Segment>& segs)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot open ELF: " << path << "\n"; return false; }

    Elf32_Ehdr ehdr;
    f.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));

    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        std::cerr << "Not an ELF file\n"; return false;
    }
    if (ehdr.e_ident[4] != 1) {
        std::cerr << "Not a 32-bit ELF\n"; return false;
    }

    entry = ehdr.e_entry;
    segs.clear();

    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        f.seekg(ehdr.e_phoff + i * ehdr.e_phentsize);
        Elf32_Phdr phdr;
        f.read(reinterpret_cast<char*>(&phdr), sizeof(phdr));
        if (phdr.p_type != PT_LOAD) continue;

        Segment seg;
        seg.paddr = phdr.p_paddr;
        seg.memsz = phdr.p_memsz;
        seg.data.resize(phdr.p_memsz, 0);
        if (phdr.p_filesz > 0) {
            f.seekg(phdr.p_offset);
            f.read(reinterpret_cast<char*>(seg.data.data()), phdr.p_filesz);
        }
        segs.push_back(std::move(seg));
    }
    return true;
}

} // namespace elf

// ============================================================================
// FakeDram — byte-addressable, services AXI4 read/write
// ============================================================================

class FakeDram : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // --- AXI4 full port (shared by loader and DMA via CoreController mux) ---
    sc_in<bool>          aw_valid;
    sc_out<bool>         aw_ready;
    sc_in<sc_uint<32>>   aw_addr;
    sc_in<sc_uint<8>>    aw_len;
    sc_in<bool>          w_valid;
    sc_out<bool>         w_ready;
    sc_in<sc_biguint<kMemAxiDataWidth>> w_data;
    sc_in<sc_uint<kMemAxiDataWidth / 8>> w_strb;
    sc_in<bool>          w_last;
    sc_out<bool>         b_valid;
    sc_in<bool>          b_ready;
    sc_out<sc_uint<2>>   b_resp;
    sc_in<bool>          ar_valid;
    sc_out<bool>         ar_ready;
    sc_in<sc_uint<32>>   ar_addr;
    sc_in<sc_uint<8>>    ar_len;
    sc_out<bool>         r_valid;
    sc_in<bool>          r_ready;
    sc_out<sc_biguint<kMemAxiDataWidth>> r_data;
    sc_out<sc_uint<2>>   r_resp;
    sc_out<bool>         r_last;

    std::map<uint32_t, uint8_t> mem_;
    uint64_t dram_base_ = 0;  // base address in system address space
    uint64_t dram_size_ = 0;   // 0 = unlimited (no range check)
    uint64_t oob_read_count_ = 0;
    uint64_t oob_write_count_ = 0;

    SC_HAS_PROCESS(FakeDram);

    FakeDram(sc_module_name name, uint64_t dram_size = 0, uint64_t dram_base = 0)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          aw_valid("aw_valid"), aw_ready("aw_ready"),
          aw_addr("aw_addr"), aw_len("aw_len"),
          w_valid("w_valid"), w_ready("w_ready"),
          w_data("w_data"), w_strb("w_strb"),
          w_last("w_last"),
          b_valid("b_valid"), b_ready("b_ready"),
          b_resp("b_resp"),
          ar_valid("ar_valid"), ar_ready("ar_ready"),
          ar_addr("ar_addr"), ar_len("ar_len"),
          r_valid("r_valid"), r_ready("r_ready"),
          r_data("r_data"), r_resp("r_resp"),
          r_last("r_last"),
          dram_base_(dram_base),
          dram_size_(dram_size)
    {
        SC_CTHREAD(read_proc, clk.pos());
        reset_signal_is(reset_n, false);

        SC_CTHREAD(write_proc, clk.pos());
        reset_signal_is(reset_n, false);
    }

    bool check_range(uint64_t addr, uint64_t len, const char* caller) const {
        if (dram_size_ == 0) return true;
        uint64_t end = dram_base_ + dram_size_;
        if (addr < dram_base_ || addr + len > end) {
            std::cerr << "[DRAM] WARNING: out-of-range " << caller
                      << " addr=0x" << std::hex << addr
                      << " len=0x" << len
                      << " valid=[0x" << dram_base_ << ", 0x" << end << ")"
                      << std::dec << std::endl;
            return false;
        }
        return true;
    }

    void load_bytes(uint32_t addr, const uint8_t* data, size_t len) {
        check_range(addr, len, "load_bytes");
        for (size_t i = 0; i < len; ++i)
            mem_[addr + static_cast<uint32_t>(i)] = data[i];
    }

    void store32(uint32_t addr, uint32_t val) {
        uint8_t buf[4];
        buf[0] = val & 0xFF;
        buf[1] = (val >> 8) & 0xFF;
        buf[2] = (val >> 16) & 0xFF;
        buf[3] = (val >> 24) & 0xFF;
        load_bytes(addr, buf, 4);
    }

    uint8_t read_byte(uint32_t addr) const {
        auto it = mem_.find(addr);
        return (it != mem_.end()) ? it->second : 0;
    }

    void print_oob_summary() const {
        if (oob_read_count_ > 0)
            std::cerr << "[DRAM] Total out-of-range reads:  " << oob_read_count_ << std::endl;
        if (oob_write_count_ > 0)
            std::cerr << "[DRAM] Total out-of-range writes: " << oob_write_count_ << std::endl;
    }

    uint32_t read_word(uint32_t addr) const {
        uint32_t v = 0;
        for (int b = 0; b < 4; ++b)
            v |= static_cast<uint32_t>(read_byte(addr + b)) << (b * 8);
        return v;
    }

    sc_biguint<kMemAxiDataWidth> read_beat(uint32_t addr) const {
        static constexpr unsigned kBytes = kMemAxiDataWidth / 8;
        sc_biguint<kMemAxiDataWidth> val = 0;
        for (unsigned b = 0; b < kBytes; ++b) {
            uint32_t byte_val = read_byte(addr + b);
            val |= (sc_biguint<kMemAxiDataWidth>(byte_val) << (b * 8));
        }
        return val;
    }

private:
    static constexpr unsigned kBeatBytes = kMemAxiDataWidth / 8;

    void read_proc() {
        ar_ready.write(false);
        r_valid.write(false);
        r_data.write(0);
        r_resp.write(0);
        r_last.write(false);
        wait();

        while (true) {
            ar_ready.write(true);
            r_valid.write(false);
            r_last.write(false);
            wait();

            if (ar_valid.read()) {
                uint32_t addr = ar_addr.read().to_uint();
                unsigned len  = ar_len.read().to_uint();
                ar_ready.write(false);

                if (dram_size_ && ((uint64_t)addr < dram_base_ || (uint64_t)addr + (uint64_t)(len + 1) * kBeatBytes > dram_base_ + dram_size_)) {
                    if (oob_read_count_++ < 10)
                        std::cerr << "[DRAM] OOB AXI READ  addr=0x" << std::hex << addr
                                  << " len=" << std::dec << len
                                  << " @ " << sc_time_stamp() << std::endl;
                }

                for (unsigned i = 0; i <= len; ++i) {
                    r_valid.write(true);
                    r_data.write(read_beat(addr + i * kBeatBytes));
                    r_resp.write(0);
                    r_last.write(i == len);
                    wait();
                    while (!r_ready.read()) wait();
                }
                r_valid.write(false);
                r_last.write(false);
            }
        }
    }

    void write_proc() {
        aw_ready.write(false);
        w_ready.write(false);
        b_valid.write(false);
        b_resp.write(0);
        wait();

        while (true) {
            aw_ready.write(true);
            w_ready.write(false);
            b_valid.write(false);
            wait();

            if (aw_valid.read()) {
                uint32_t addr = aw_addr.read().to_uint();
                unsigned len  = aw_len.read().to_uint();
                aw_ready.write(false);

                if (dram_size_ && ((uint64_t)addr < dram_base_ || (uint64_t)addr + (uint64_t)(len + 1) * kBeatBytes > dram_base_ + dram_size_)) {
                    if (oob_write_count_++ < 10)
                        std::cerr << "[DRAM] OOB AXI WRITE addr=0x" << std::hex << addr
                                  << " len=" << std::dec << len
                                  << " @ " << sc_time_stamp() << std::endl;
                }

                for (unsigned i = 0; i <= len; ++i) {
                    w_ready.write(true);
                    wait();
                    while (!w_valid.read()) {
                        w_ready.write(true);
                        wait();
                    }
                    sc_biguint<kMemAxiDataWidth> wdata = w_data.read();
                    uint32_t strb = w_strb.read().to_uint();
                    uint32_t beat_addr = addr + i * kBeatBytes;
                    for (unsigned b = 0; b < kBeatBytes; ++b) {
                        if (strb & (1u << b)) {
                            uint8_t byte_val = static_cast<uint8_t>(
                                wdata.range(b * 8 + 7, b * 8).to_uint());
                            mem_[beat_addr + b] = byte_val;
                        }
                    }
                    w_ready.write(false);
                }

                b_valid.write(true);
                b_resp.write(0);
                wait();
                while (!b_ready.read()) wait();
                b_valid.write(false);
            }
        }
    }
};

// ============================================================================
// FakeClusterSpm — storage-backed cluster SPM stub
//
// Provides both:
//   - 32-bit cmd interface (for core MMIO reads/writes via CmdFabric)
//   - 64-bit data AXI interface (for DMA/NLU via ClusterDataFabric)
// Both share the same underlying byte-addressable storage.
// ============================================================================

class FakeClusterSpm : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // --- CMD interface (from CmdFabric, 32-bit) ---
    sc_in<bool>          cmd_req_valid;
    sc_in<bool>          cmd_req_write;
    sc_in<sc_uint<32>>   cmd_req_addr;
    sc_in<sc_uint<32>>   cmd_req_wdata;
    sc_out<bool>         cmd_resp_valid;
    sc_out<sc_uint<32>>  cmd_resp_rdata;

    // --- DATA AXI interface (64-bit, from ClusterDataFabric) ---
    sc_in<bool>          data_aw_valid;
    sc_out<bool>         data_aw_ready;
    sc_in<sc_uint<32>>   data_aw_addr;
    sc_in<bool>          data_w_valid;
    sc_out<bool>         data_w_ready;
    sc_in<sc_biguint<kClAxiDataWidth>>       data_w_data;
    sc_in<sc_uint<kClAxiDataWidth / 8>>      data_w_strb;
    sc_out<bool>         data_b_valid;
    sc_in<bool>          data_b_ready;
    sc_out<sc_uint<2>>   data_b_resp;
    sc_in<bool>          data_ar_valid;
    sc_out<bool>         data_ar_ready;
    sc_in<sc_uint<32>>   data_ar_addr;
    sc_out<bool>         data_r_valid;
    sc_in<bool>          data_r_ready;
    sc_out<sc_biguint<kClAxiDataWidth>>      data_r_data;
    sc_out<sc_uint<2>>   data_r_resp;

    // --- IRQ output (active high) ---
    sc_out<bool>         cluster_irq;

    std::map<uint32_t, uint8_t> mem_;

    static constexpr unsigned kDataBeatBytes = kClAxiDataWidth / 8;

    SC_HAS_PROCESS(FakeClusterSpm);

    FakeClusterSpm(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          cmd_req_valid("cmd_req_valid"), cmd_req_write("cmd_req_write"),
          cmd_req_addr("cmd_req_addr"), cmd_req_wdata("cmd_req_wdata"),
          cmd_resp_valid("cmd_resp_valid"), cmd_resp_rdata("cmd_resp_rdata"),
          data_aw_valid("data_aw_valid"), data_aw_ready("data_aw_ready"),
          data_aw_addr("data_aw_addr"),
          data_w_valid("data_w_valid"), data_w_ready("data_w_ready"),
          data_w_data("data_w_data"), data_w_strb("data_w_strb"),
          data_b_valid("data_b_valid"), data_b_ready("data_b_ready"),
          data_b_resp("data_b_resp"),
          data_ar_valid("data_ar_valid"), data_ar_ready("data_ar_ready"),
          data_ar_addr("data_ar_addr"),
          data_r_valid("data_r_valid"), data_r_ready("data_r_ready"),
          data_r_data("data_r_data"), data_r_resp("data_r_resp"),
          cluster_irq("cluster_irq")
    {
        SC_CTHREAD(cmd_proc, clk.pos());
        reset_signal_is(reset_n, false);

        SC_CTHREAD(data_proc, clk.pos());
        reset_signal_is(reset_n, false);
    }

    // ---- Public accessors for post-sim verification ----

    uint8_t read_byte(uint32_t addr) const {
        auto it = mem_.find(addr);
        return (it != mem_.end()) ? it->second : 0;
    }

    uint32_t read_word(uint32_t addr) const {
        uint32_t v = 0;
        for (int b = 0; b < 4; ++b)
            v |= static_cast<uint32_t>(read_byte(addr + b)) << (b * 8);
        return v;
    }

    void write_word(uint32_t addr, uint32_t val) {
        for (int b = 0; b < 4; ++b)
            mem_[addr + b] = (val >> (b * 8)) & 0xFF;
    }

private:
    sc_biguint<kClAxiDataWidth> read_data_beat(uint32_t addr) const {
        sc_biguint<kClAxiDataWidth> val = 0;
        for (unsigned b = 0; b < kDataBeatBytes; ++b) {
            uint32_t byte_val = read_byte(addr + b);
            val |= (sc_biguint<kClAxiDataWidth>(byte_val) << (b * 8));
        }
        return val;
    }

    void write_data_beat(uint32_t addr,
                         const sc_biguint<kClAxiDataWidth>& data,
                         uint32_t strb) {
        for (unsigned b = 0; b < kDataBeatBytes; ++b) {
            if (strb & (1u << b)) {
                uint8_t byte_val = static_cast<uint8_t>(
                    data.range(b * 8 + 7, b * 8).to_uint());
                mem_[addr + b] = byte_val;
            }
        }
    }

    void cmd_write_word(uint32_t addr, uint32_t val) {
        for (int b = 0; b < 4; ++b)
            mem_[addr + b] = (val >> (b * 8)) & 0xFF;
    }

    uint32_t cmd_read_word(uint32_t addr) const {
        // If none of the 4 bytes were ever written, return 0xDEADBEEF
        // (matches real HW default / BusStub behaviour).
        bool any_written = false;
        for (int b = 0; b < 4; ++b) {
            if (mem_.count(addr + b)) { any_written = true; break; }
        }
        if (!any_written) return 0xDEADBEEF;
        uint32_t v = 0;
        for (int b = 0; b < 4; ++b)
            v |= static_cast<uint32_t>(read_byte(addr + b)) << (b * 8);
        return v;
    }

    // 32-bit cmd interface: 1-cycle latency response
    void cmd_proc() {
        cmd_resp_valid.write(false);
        cmd_resp_rdata.write(0);
        cluster_irq.write(false);
        wait();

        while (true) {
            if (cmd_req_valid.read()) {
                uint32_t addr = cmd_req_addr.read().to_uint();
                bool wr = cmd_req_write.read();
                if (wr) {
                    uint32_t wdata = cmd_req_wdata.read().to_uint();
                    cmd_write_word(addr, wdata);
                    cmd_resp_valid.write(true);
                    cmd_resp_rdata.write(0);
                } else {
                    cmd_resp_valid.write(true);
                    cmd_resp_rdata.write(cmd_read_word(addr));
                }
            } else {
                cmd_resp_valid.write(false);
            }
            wait();
        }
    }

    // 64-bit data AXI interface
    void data_proc() {
        data_aw_ready.write(false);
        data_w_ready.write(false);
        data_b_valid.write(false);
        data_b_resp.write(0);
        data_ar_ready.write(false);
        data_r_valid.write(false);
        data_r_data.write(0);
        data_r_resp.write(0);
        wait();

        while (true) {
            // Ready to accept new transactions
            data_aw_ready.write(true);
            data_ar_ready.write(true);
            data_w_ready.write(false);
            data_b_valid.write(false);
            data_r_valid.write(false);
            wait();

            if (data_aw_valid.read()) {
                // AW handshake occurred
                uint32_t wr_addr = data_aw_addr.read().to_uint();
                data_aw_ready.write(false);
                data_ar_ready.write(false);

                // Accept W
                data_w_ready.write(true);
                wait();
                while (!data_w_valid.read()) {
                    data_w_ready.write(true);
                    wait();
                }
                write_data_beat(wr_addr,
                                data_w_data.read(),
                                data_w_strb.read().to_uint());
                data_w_ready.write(false);

                // B response
                data_b_valid.write(true);
                data_b_resp.write(0);
                wait();
                while (!data_b_ready.read()) wait();
                data_b_valid.write(false);

            } else if (data_ar_valid.read()) {
                // AR handshake occurred
                uint32_t rd_addr = data_ar_addr.read().to_uint();
                data_aw_ready.write(false);
                data_ar_ready.write(false);

                // R data
                data_r_valid.write(true);
                data_r_data.write(read_data_beat(rd_addr));
                data_r_resp.write(0);
                wait();
                while (!data_r_ready.read()) wait();
                data_r_valid.write(false);
            }
        }
    }
};

// ============================================================================
// ManifestBuilder — generates manifest table in DRAM for section loader
// ============================================================================

struct ManifestBuilder {
    uint32_t dram_base;
    uint32_t next_payload_addr;
    std::vector<ManifestEntry> entries;
    FakeDram* dram;

    ManifestBuilder(FakeDram* d, uint32_t manifest_base, uint32_t payload_base)
        : dram_base(manifest_base), next_payload_addr(payload_base), dram(d) {}

    void add_section(SectionKind kind, uint32_t local_addr,
                     const uint8_t* data, uint32_t size) {
        ManifestEntry e{};
        e.section_kind = static_cast<uint16_t>(kind);
        e.flags = 0;
        e.dram_addr_lo = next_payload_addr;
        e.dram_addr_hi = 0;
        e.local_addr   = local_addr;
        e.size_bytes   = size;
        e.crc32 = 0;
        e.attr0 = 0;
        e.reserved = 0;
        entries.push_back(e);

        dram->load_bytes(next_payload_addr, data, size);
        next_payload_addr += (size + 15) & ~15u; // align to 16
    }

    void finalize() {
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            uint32_t addr = dram_base + static_cast<uint32_t>(i) * 32;
            dram->store32(addr + 0,  e.section_kind | (static_cast<uint32_t>(e.flags) << 16));
            dram->store32(addr + 4,  e.dram_addr_lo);
            dram->store32(addr + 8,  e.dram_addr_hi);
            dram->store32(addr + 12, e.local_addr);
            dram->store32(addr + 16, e.size_bytes);
            dram->store32(addr + 20, e.crc32);
            dram->store32(addr + 24, e.attr0);
            dram->store32(addr + 28, e.reserved);
        }
    }
};

// ============================================================================
// BootTestDriver — host AXI4-Lite driver
//
// Configures BootHostIf, kicks section loader, waits for IRQ,
// enables core, waits for EBREAK halt IRQ.
// ============================================================================

class BootTestDriver : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Host AXI4-Lite interface
    sc_out<bool>         aw_valid_o;
    sc_in<bool>          aw_ready_i;
    sc_out<sc_uint<32>>  aw_addr_o;
    sc_out<bool>         w_valid_o;
    sc_in<bool>          w_ready_i;
    sc_out<sc_uint<32>>  w_data_o;
    sc_out<sc_uint<4>>   w_strb_o;
    sc_in<bool>          b_valid_i;
    sc_out<bool>         b_ready_o;
    sc_in<sc_uint<2>>    b_resp_i;
    sc_out<bool>         ar_valid_o;
    sc_in<bool>          ar_ready_i;
    sc_out<sc_uint<32>>  ar_addr_o;
    sc_in<bool>          r_valid_i;
    sc_out<bool>         r_ready_o;
    sc_in<sc_uint<32>>   r_data_i;
    sc_in<sc_uint<2>>    r_resp_i;

    sc_in<bool>          controller_irq_i;

    // Config
    uint32_t manifest_dram_addr = 0;
    uint32_t manifest_num_entries = 0;
    uint32_t boot_addr = 0;
    uint32_t max_cycles = 500000;

    SC_HAS_PROCESS(BootTestDriver);

    BootTestDriver(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          aw_valid_o("aw_valid_o"), aw_ready_i("aw_ready_i"),
          aw_addr_o("aw_addr_o"),
          w_valid_o("w_valid_o"), w_ready_i("w_ready_i"),
          w_data_o("w_data_o"), w_strb_o("w_strb_o"),
          b_valid_i("b_valid_i"), b_ready_o("b_ready_o"),
          b_resp_i("b_resp_i"),
          ar_valid_o("ar_valid_o"), ar_ready_i("ar_ready_i"),
          ar_addr_o("ar_addr_o"),
          r_valid_i("r_valid_i"), r_ready_o("r_ready_o"),
          r_data_i("r_data_i"), r_resp_i("r_resp_i"),
          controller_irq_i("controller_irq_i")
    {
        SC_CTHREAD(main_proc, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    static constexpr uint32_t kHaccCtrl          = 0x0008;
    static constexpr uint32_t kCoreBootAddr      = 0x0010;
    static constexpr uint32_t kManifestAddrLo    = 0x0020;
    static constexpr uint32_t kManifestAddrHi    = 0x0024;
    static constexpr uint32_t kManifestSize      = 0x0028;
    static constexpr uint32_t kManifestKick      = 0x002C;
    static constexpr uint32_t kIrqSummary        = 0x0040;
    static constexpr uint32_t kIrqForceAck       = 0x0044;

    void axi_write(uint32_t addr, uint32_t data) {
        aw_valid_o.write(true);
        aw_addr_o.write(addr);
        w_valid_o.write(true);
        w_data_o.write(data);
        w_strb_o.write(0xF);
        wait();
        while (!aw_ready_i.read()) wait();
        aw_valid_o.write(false);
        while (!w_ready_i.read()) wait();
        w_valid_o.write(false);
        b_ready_o.write(true);
        while (!b_valid_i.read()) wait();
        b_ready_o.write(false);
        wait();
    }

    uint32_t axi_read(uint32_t addr) {
        ar_valid_o.write(true);
        ar_addr_o.write(addr);
        wait();
        while (!ar_ready_i.read()) wait();
        ar_valid_o.write(false);
        r_ready_o.write(true);
        while (!r_valid_i.read()) wait();
        uint32_t val = r_data_i.read().to_uint();
        r_ready_o.write(false);
        wait();
        return val;
    }

    void main_proc() {
        aw_valid_o.write(false); aw_addr_o.write(0);
        w_valid_o.write(false); w_data_o.write(0); w_strb_o.write(0);
        b_ready_o.write(false);
        ar_valid_o.write(false); ar_addr_o.write(0);
        r_ready_o.write(false);
        wait();

        for (int i = 0; i < 5; ++i) wait();

        TRACE_EVENT("host_configure", "Core", TRACE_BEGIN, 0, 0, "{}");
        std::cout << "[TB] === Phase 1: Configure BootHostIf ===" << std::endl;

        axi_write(kCoreBootAddr, boot_addr);
        axi_write(kManifestAddrLo, manifest_dram_addr);
        axi_write(kManifestAddrHi, 0);
        axi_write(kManifestSize, manifest_num_entries * 32);
        axi_write(kManifestKick, 1);
        std::cout << "[TB] Loader kicked" << std::endl;
        TRACE_EVENT("host_configure", "Core", TRACE_END, 0, 0, "{}");

        TRACE_EVENT("wait_loader", "Core", TRACE_BEGIN, 0, 0, "{}");

        uint32_t cycle = 0;
        while (!controller_irq_i.read() && cycle < max_cycles) {
            wait(); ++cycle;
        }
        if (cycle >= max_cycles) {
            std::cerr << "[TB] ERROR: Timeout waiting for loader IRQ" << std::endl;
            sc_stop(); return;
        }

        uint32_t irq = axi_read(kIrqSummary);
        std::cout << "[TB] Loader IRQ at cycle " << cycle
                  << ", IRQ_SUMMARY=0x" << std::hex << irq << std::dec << std::endl;
        axi_write(kIrqForceAck, irq);
        for (int i = 0; i < 3; ++i) wait();
        TRACE_EVENT("wait_loader", "Core", TRACE_END, 0, 0, "{}");

        TRACE_EVENT("core_running", "Core", TRACE_BEGIN, 0, 0, "{}");
        std::cout << "[TB] === Phase 2: Enable core ===" << std::endl;
        axi_write(kHaccCtrl, 0x01);
        std::cout << "[TB] Core enabled, boot_addr=0x" << std::hex << boot_addr
                  << std::dec << std::endl;

        // Poll IRQ_SUMMARY for bit3 (core_halted from EBREAK)
        cycle = 0; irq = 0;
        while (!(irq & 0x08) && cycle < max_cycles) {
            wait(); ++cycle;
            if ((cycle & 0xF) == 0 || controller_irq_i.read()) {
                irq = axi_read(kIrqSummary);
            }
        }
        if (cycle >= max_cycles) {
            std::cerr << "[TB] ERROR: Timeout waiting for EBREAK halt" << std::endl;
            irq = axi_read(kIrqSummary);
            std::cerr << "[TB] Final IRQ_SUMMARY=0x" << std::hex << irq
                      << std::dec << std::endl;
            sc_stop(); return;
        }

        std::cout << "[TB] === Simulation complete ===" << std::endl;
        std::cout << "[TB] EBREAK at cycle " << cycle
                  << ", IRQ_SUMMARY=0x" << std::hex << irq << std::dec << std::endl;
        TRACE_EVENT("core_running", "Core", TRACE_END, 0, 0, "{}");
        sc_stop();
    }
};
