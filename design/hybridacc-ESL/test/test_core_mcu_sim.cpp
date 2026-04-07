/**
 * @file test_core_mcu_sim.cpp
 * @brief Core Controller simulation testbench.
 *
 * Loads a GCC-compiled RV32 ELF into fake DRAM, then:
 *   1. TB (host) writes BootHostIf CSRs to configure manifest + kick loader.
 *   2. Waits for loader_done (controller_irq_o pulse #1 from irq_summary bit0).
 *   3. Clears the IRQ, enables core (HACC_CTRL.core_en = 1).
 *   4. Core executes firmware until EBREAK → controller_irq_o pulse #2.
 *   5. sc_stop().
 *
 * External buses (cluster cmd/data, NLU cmd/data) are stubbed:
 *   - Print on any event.
 *   - Read responses return 0xDEADBEEF.
 *
 * Usage:
 *   ./test_core_mcu_sim <path-to-rv32-elf> [max_cycles] [--core-debug]
 *                        [--dump-dsram <addr> <length>]
 */

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
bool load_elf(const std::string& path,
              uint32_t& entry,
              std::vector<Segment>& segs)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot open ELF: " << path << "\n"; return false; }

    Elf32_Ehdr ehdr;
    f.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));

    // Minimal validation
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        std::cerr << "Not an ELF file\n"; return false;
    }
    if (ehdr.e_ident[4] != 1) { // ELFCLASS32
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
// Fake DRAM — byte-addressable, services AXI4 read/write
// ============================================================================

class FakeDram : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // --- Loader AXI4 read-only port ---
    sc_in<bool>          ldr_ar_valid;
    sc_out<bool>         ldr_ar_ready;
    sc_in<sc_uint<32>>   ldr_ar_addr;
    sc_in<sc_uint<8>>    ldr_ar_len;
    sc_out<bool>         ldr_r_valid;
    sc_in<bool>          ldr_r_ready;
    sc_out<sc_biguint<kMemAxiDataWidth>> ldr_r_data;
    sc_out<sc_uint<2>>   ldr_r_resp;
    sc_out<bool>         ldr_r_last;

    // --- DMA AXI4 full port ---
    sc_in<bool>          dma_aw_valid;
    sc_out<bool>         dma_aw_ready;
    sc_in<sc_uint<32>>   dma_aw_addr;
    sc_in<sc_uint<8>>    dma_aw_len;
    sc_in<bool>          dma_w_valid;
    sc_out<bool>         dma_w_ready;
    sc_in<sc_biguint<kMemAxiDataWidth>> dma_w_data;
    sc_in<sc_uint<kMemAxiDataWidth / 8>> dma_w_strb;
    sc_in<bool>          dma_w_last;
    sc_out<bool>         dma_b_valid;
    sc_in<bool>          dma_b_ready;
    sc_out<sc_uint<2>>   dma_b_resp;
    sc_in<bool>          dma_ar_valid;
    sc_out<bool>         dma_ar_ready;
    sc_in<sc_uint<32>>   dma_ar_addr;
    sc_in<sc_uint<8>>    dma_ar_len;
    sc_out<bool>         dma_r_valid;
    sc_in<bool>          dma_r_ready;
    sc_out<sc_biguint<kMemAxiDataWidth>> dma_r_data;
    sc_out<sc_uint<2>>   dma_r_resp;
    sc_out<bool>         dma_r_last;

    std::map<uint32_t, uint8_t> mem_;

    SC_HAS_PROCESS(FakeDram);

    FakeDram(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          ldr_ar_valid("ldr_ar_valid"), ldr_ar_ready("ldr_ar_ready"),
          ldr_ar_addr("ldr_ar_addr"), ldr_ar_len("ldr_ar_len"),
          ldr_r_valid("ldr_r_valid"), ldr_r_ready("ldr_r_ready"),
          ldr_r_data("ldr_r_data"), ldr_r_resp("ldr_r_resp"),
          ldr_r_last("ldr_r_last"),
          dma_aw_valid("dma_aw_valid"), dma_aw_ready("dma_aw_ready"),
          dma_aw_addr("dma_aw_addr"), dma_aw_len("dma_aw_len"),
          dma_w_valid("dma_w_valid"), dma_w_ready("dma_w_ready"),
          dma_w_data("dma_w_data"), dma_w_strb("dma_w_strb"),
          dma_w_last("dma_w_last"),
          dma_b_valid("dma_b_valid"), dma_b_ready("dma_b_ready"),
          dma_b_resp("dma_b_resp"),
          dma_ar_valid("dma_ar_valid"), dma_ar_ready("dma_ar_ready"),
          dma_ar_addr("dma_ar_addr"), dma_ar_len("dma_ar_len"),
          dma_r_valid("dma_r_valid"), dma_r_ready("dma_r_ready"),
          dma_r_data("dma_r_data"), dma_r_resp("dma_r_resp"),
          dma_r_last("dma_r_last")
    {
        SC_CTHREAD(ldr_read_proc, clk.pos());
        reset_signal_is(reset_n, false);

        SC_CTHREAD(dma_read_proc, clk.pos());
        reset_signal_is(reset_n, false);

        SC_CTHREAD(dma_write_proc, clk.pos());
        reset_signal_is(reset_n, false);
    }

    /// Load raw bytes into DRAM at given address
    void load_bytes(uint32_t addr, const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i)
            mem_[addr + static_cast<uint32_t>(i)] = data[i];
    }

    uint8_t read_byte(uint32_t addr) const {
        auto it = mem_.find(addr);
        return (it != mem_.end()) ? it->second : 0;
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

    void ldr_read_proc() {
        ldr_ar_ready.write(false);
        ldr_r_valid.write(false);
        ldr_r_data.write(0);
        ldr_r_resp.write(0);
        ldr_r_last.write(false);
        wait();

        while (true) {
            ldr_ar_ready.write(true);
            ldr_r_valid.write(false);
            ldr_r_last.write(false);
            wait();

            if (ldr_ar_valid.read()) {
                uint32_t addr = ldr_ar_addr.read().to_uint();
                unsigned len  = ldr_ar_len.read().to_uint();
                ldr_ar_ready.write(false);

                for (unsigned i = 0; i <= len; ++i) {
                    ldr_r_valid.write(true);
                    ldr_r_data.write(read_beat(addr + i * kBeatBytes));
                    ldr_r_resp.write(0); // OKAY
                    ldr_r_last.write(i == len);
                    wait();
                    while (!ldr_r_ready.read()) wait();
                }
                ldr_r_valid.write(false);
                ldr_r_last.write(false);
            }
        }
    }

    void dma_read_proc() {
        dma_ar_ready.write(false);
        dma_r_valid.write(false);
        dma_r_data.write(0);
        dma_r_resp.write(0);
        dma_r_last.write(false);
        wait();

        while (true) {
            dma_ar_ready.write(true);
            dma_r_valid.write(false);
            dma_r_last.write(false);
            wait();

            if (dma_ar_valid.read()) {
                uint32_t addr = dma_ar_addr.read().to_uint();
                unsigned len  = dma_ar_len.read().to_uint();
                dma_ar_ready.write(false);

                for (unsigned i = 0; i <= len; ++i) {
                    dma_r_valid.write(true);
                    dma_r_data.write(read_beat(addr + i * kBeatBytes));
                    dma_r_resp.write(0);
                    dma_r_last.write(i == len);
                    wait();
                    while (!dma_r_ready.read()) wait();
                }
                dma_r_valid.write(false);
                dma_r_last.write(false);
            }
        }
    }

    void dma_write_proc() {
        dma_aw_ready.write(false);
        dma_w_ready.write(false);
        dma_b_valid.write(false);
        dma_b_resp.write(0);
        wait();

        while (true) {
            dma_aw_ready.write(true);
            dma_w_ready.write(false);
            dma_b_valid.write(false);
            wait();

            if (dma_aw_valid.read()) {
                uint32_t addr = dma_aw_addr.read().to_uint();
                unsigned len  = dma_aw_len.read().to_uint();
                dma_aw_ready.write(false);

                // Accept W beats
                for (unsigned i = 0; i <= len; ++i) {
                    dma_w_ready.write(true);
                    wait();
                    while (!dma_w_valid.read()) {
                        dma_w_ready.write(true);
                        wait();
                    }
                    sc_biguint<kMemAxiDataWidth> wdata = dma_w_data.read();
                    uint32_t strb = dma_w_strb.read().to_uint();
                    uint32_t beat_addr = addr + i * kBeatBytes;
                    for (unsigned b = 0; b < kBeatBytes; ++b) {
                        if (strb & (1u << b)) {
                            uint8_t byte_val = static_cast<uint8_t>(
                                wdata.range(b * 8 + 7, b * 8).to_uint());
                            mem_[beat_addr + b] = byte_val;
                        }
                    }
                    dma_w_ready.write(false);
                }

                // B response
                dma_b_valid.write(true);
                dma_b_resp.write(0);
                wait();
                while (!dma_b_ready.read()) wait();
                dma_b_valid.write(false);
            }
        }
    }
};

// ============================================================================
// Host AXI4-Lite driver helpers (drive BootHostIf CSR bus)
// ============================================================================

static constexpr unsigned NUM_CLUSTERS = 1;
static constexpr unsigned NUM_NLU = 0;

// ============================================================================
// Test driver (SC_CTHREAD)
// ============================================================================

class TestDriver : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Host AXI4-Lite interface (directly bound to CoreController's slave port)
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

    // Monitor
    sc_in<bool>          controller_irq_i;

    // Config from main
    uint32_t manifest_dram_addr = 0;
    uint32_t manifest_num_entries = 0;
    uint32_t boot_addr = 0;
    uint32_t max_cycles = 100000;

    SC_HAS_PROCESS(TestDriver);

    TestDriver(sc_module_name name)
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
    // BootHostIf CSR offsets (host-visible address space)
    static constexpr uint32_t kHaccCap0          = 0x0000;
    static constexpr uint32_t kHaccCtrl          = 0x0008;
    static constexpr uint32_t kHaccStatus        = 0x000C;
    static constexpr uint32_t kCoreBootAddr      = 0x0010;
    static constexpr uint32_t kManifestAddrLo    = 0x0020;
    static constexpr uint32_t kManifestAddrHi    = 0x0024;
    static constexpr uint32_t kManifestSize      = 0x0028;
    static constexpr uint32_t kManifestKick      = 0x002C;
    static constexpr uint32_t kLoaderStatus      = 0x0030;
    static constexpr uint32_t kIrqSummary        = 0x0040;
    static constexpr uint32_t kIrqForceAck       = 0x0044;

    /// AXI4-Lite write transaction
    void axi_write(uint32_t addr, uint32_t data) {
        // AW + W phase
        aw_valid_o.write(true);
        aw_addr_o.write(addr);
        w_valid_o.write(true);
        w_data_o.write(data);
        w_strb_o.write(0xF);
        wait();

        // Wait AW accepted
        while (!aw_ready_i.read()) wait();
        aw_valid_o.write(false);

        // Wait W accepted
        while (!w_ready_i.read()) wait();
        w_valid_o.write(false);

        // Wait B response
        b_ready_o.write(true);
        while (!b_valid_i.read()) wait();
        b_ready_o.write(false);
        wait();
    }

    /// AXI4-Lite read transaction
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
        // Reset outputs
        aw_valid_o.write(false);
        aw_addr_o.write(0);
        w_valid_o.write(false);
        w_data_o.write(0);
        w_strb_o.write(0);
        b_ready_o.write(false);
        ar_valid_o.write(false);
        ar_addr_o.write(0);
        r_ready_o.write(false);
        wait();

        // Wait a few cycles for reset to propagate
        for (int i = 0; i < 5; ++i) wait();

        std::cout << "[TB] === Phase 1: Configure BootHostIf and kick loader ===" << std::endl;

        // Set boot address
        axi_write(kCoreBootAddr, boot_addr);
        std::cout << "[TB] CORE_BOOT_ADDR = 0x" << std::hex << boot_addr << std::dec << std::endl;

        // Set manifest address + size
        axi_write(kManifestAddrLo, manifest_dram_addr);
        axi_write(kManifestAddrHi, 0);
        axi_write(kManifestSize, manifest_num_entries * 32); // 32 bytes per entry
        std::cout << "[TB] MANIFEST: dram=0x" << std::hex << manifest_dram_addr
                  << " entries=" << std::dec << manifest_num_entries << std::endl;

        // Kick loader
        axi_write(kManifestKick, 1);
        std::cout << "[TB] Loader kicked" << std::endl;

        // Wait for controller_irq (loader done → irq_summary bit0)
        uint32_t cycle = 0;
        while (!controller_irq_i.read() && cycle < max_cycles) {
            wait();
            ++cycle;
        }

        if (cycle >= max_cycles) {
            std::cerr << "[TB] ERROR: Timeout waiting for loader done IRQ after "
                      << max_cycles << " cycles" << std::endl;
            sc_stop();
            return;
        }

        // Read IRQ summary
        uint32_t irq = axi_read(kIrqSummary);
        std::cout << "[TB] Loader IRQ received at cycle " << cycle
                  << ", IRQ_SUMMARY=0x" << std::hex << irq << std::dec << std::endl;

        // Acknowledge loader done IRQ
        axi_write(kIrqForceAck, irq);
        // Small delay for IRQ to clear
        for (int i = 0; i < 3; ++i) wait();

        std::cout << "[TB] === Phase 2: Enable core ===" << std::endl;

        // HACC_CTRL: core_enable = 1 (bit0)
        axi_write(kHaccCtrl, 0x01);
        std::cout << "[TB] Core enabled, firmware executing from 0x"
                  << std::hex << boot_addr << std::dec << std::endl;

        // Poll IRQ_SUMMARY for bit3 (core_halted from EBREAK).
        // Note: controller_irq_o stays high because loader_done (bit0) is
        //       a live signal that never deasserts.  We must check bit3
        //       explicitly.
        cycle = 0;
        irq = 0;
        while (!(irq & 0x08) && cycle < max_cycles) {
            wait();
            ++cycle;
            // Poll every 16 cycles to reduce AXI traffic
            if ((cycle & 0xF) == 0 || controller_irq_i.read()) {
                irq = axi_read(kIrqSummary);
            }
        }

        if (cycle >= max_cycles) {
            std::cerr << "[TB] ERROR: Timeout waiting for EBREAK halt IRQ after "
                      << max_cycles << " cycles" << std::endl;
            // Read final summary for diagnostics
            irq = axi_read(kIrqSummary);
            std::cerr << "[TB] Final IRQ_SUMMARY=0x" << std::hex << irq
                      << std::dec << std::endl;
            sc_stop();
            return;
        }

        std::cout << "[TB] === Simulation complete ===" << std::endl;
        std::cout << "[TB] EBREAK halt detected at cycle " << cycle
                  << ", IRQ_SUMMARY=0x" << std::hex << irq << std::dec << std::endl;

        if (irq & 0x08) {
            std::cout << "[TB] PASS: Core halted via EBREAK (bit3 set)" << std::endl;
        } else {
            std::cout << "[TB] WARN: IRQ raised but bit3 not set — unexpected cause"
                      << std::endl;
        }

        sc_stop();
    }
};

// ============================================================================
// Stub responders for cluster cmd / data / NLU buses
// ============================================================================

class BusStub : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Cluster command bus (per cluster)
    sc_in<bool>          cl_cmd_req_valid[NUM_CLUSTERS];
    sc_in<bool>          cl_cmd_req_write[NUM_CLUSTERS];
    sc_in<sc_uint<32>>   cl_cmd_req_addr[NUM_CLUSTERS];
    sc_in<sc_uint<32>>   cl_cmd_req_wdata[NUM_CLUSTERS];
    sc_out<bool>         cl_cmd_resp_valid[NUM_CLUSTERS];
    sc_out<sc_uint<32>>  cl_cmd_resp_rdata[NUM_CLUSTERS];

    // NLU command bus
    static constexpr unsigned kNluPorts = NUM_NLU > 0 ? NUM_NLU : 1;
    sc_in<bool>          nlu_cmd_req_valid[kNluPorts];
    sc_in<bool>          nlu_cmd_req_write[kNluPorts];
    sc_in<sc_uint<32>>   nlu_cmd_req_addr[kNluPorts];
    sc_in<sc_uint<32>>   nlu_cmd_req_wdata[kNluPorts];
    sc_out<bool>         nlu_cmd_resp_valid[kNluPorts];
    sc_out<sc_uint<32>>  nlu_cmd_resp_rdata[kNluPorts];

    // Cluster data AXI4-Lite (per cluster)
    sc_in<bool>          cl_data_aw_valid[NUM_CLUSTERS];
    sc_out<bool>         cl_data_aw_ready[NUM_CLUSTERS];
    sc_in<sc_uint<32>>   cl_data_aw_addr[NUM_CLUSTERS];
    sc_in<bool>          cl_data_w_valid[NUM_CLUSTERS];
    sc_out<bool>         cl_data_w_ready[NUM_CLUSTERS];
    sc_in<sc_biguint<kClAxiDataWidth>>        cl_data_w_data[NUM_CLUSTERS];
    sc_in<sc_uint<kClAxiDataWidth / 8>>       cl_data_w_strb[NUM_CLUSTERS];
    sc_out<bool>         cl_data_b_valid[NUM_CLUSTERS];
    sc_in<bool>          cl_data_b_ready[NUM_CLUSTERS];
    sc_out<sc_uint<2>>   cl_data_b_resp[NUM_CLUSTERS];
    sc_in<bool>          cl_data_ar_valid[NUM_CLUSTERS];
    sc_out<bool>         cl_data_ar_ready[NUM_CLUSTERS];
    sc_in<sc_uint<32>>   cl_data_ar_addr[NUM_CLUSTERS];
    sc_out<bool>         cl_data_r_valid[NUM_CLUSTERS];
    sc_in<bool>          cl_data_r_ready[NUM_CLUSTERS];
    sc_out<sc_biguint<kClAxiDataWidth>>       cl_data_r_data[NUM_CLUSTERS];
    sc_out<sc_uint<2>>   cl_data_r_resp[NUM_CLUSTERS];

    // Cluster IRQ outputs (active-low, no IRQ from stubs)
    sc_out<bool>         cluster_irq[NUM_CLUSTERS];
    sc_out<bool>         nlu_irq[kNluPorts];

    // NLU data requester (stub: nothing requests)
    sc_out<bool>         nlu_data_req_valid;
    sc_out<bool>         nlu_data_req_write;
    sc_out<sc_uint<32>>  nlu_data_req_cluster_id;
    sc_out<sc_uint<32>>  nlu_data_req_addr;
    sc_out<sc_biguint<kClAxiDataWidth>>       nlu_data_req_wdata;
    sc_out<sc_uint<kClAxiDataWidth / 8>>      nlu_data_req_wstrb;
    sc_in<bool>          nlu_data_req_ready;
    sc_in<bool>          nlu_data_resp_valid;
    sc_in<sc_biguint<kClAxiDataWidth>>        nlu_data_resp_rdata;
    sc_in<bool>          nlu_data_resp_error;

    SC_HAS_PROCESS(BusStub);

    BusStub(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          nlu_data_req_valid("nlu_data_req_valid"),
          nlu_data_req_write("nlu_data_req_write"),
          nlu_data_req_cluster_id("nlu_data_req_cluster_id"),
          nlu_data_req_addr("nlu_data_req_addr"),
          nlu_data_req_wdata("nlu_data_req_wdata"),
          nlu_data_req_wstrb("nlu_data_req_wstrb"),
          nlu_data_req_ready("nlu_data_req_ready"),
          nlu_data_resp_valid("nlu_data_resp_valid"),
          nlu_data_resp_rdata("nlu_data_resp_rdata"),
          nlu_data_resp_error("nlu_data_resp_error")
    {
        SC_CTHREAD(cmd_stub_proc, clk.pos());
        reset_signal_is(reset_n, false);

        SC_CTHREAD(data_stub_proc, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    void cmd_stub_proc() {
        // Reset
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            cl_cmd_resp_valid[c].write(false);
            cl_cmd_resp_rdata[c].write(0xDEADBEEF);
        }
        for (unsigned n = 0; n < kNluPorts; ++n) {
            nlu_cmd_resp_valid[n].write(false);
            nlu_cmd_resp_rdata[n].write(0xDEADBEEF);
        }
        cluster_irq[0].write(false);
        for (unsigned n = 0; n < kNluPorts; ++n) nlu_irq[n].write(false);
        nlu_data_req_valid.write(false);
        nlu_data_req_write.write(false);
        nlu_data_req_cluster_id.write(0);
        nlu_data_req_addr.write(0);
        nlu_data_req_wdata.write(0);
        nlu_data_req_wstrb.write(0);
        wait();

        while (true) {
            // Cluster cmd: 1-cycle response with 0xDEADBEEF
            for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
                if (cl_cmd_req_valid[c].read()) {
                    uint32_t addr  = cl_cmd_req_addr[c].read().to_uint();
                    bool     wr    = cl_cmd_req_write[c].read();
                    uint32_t wdata = cl_cmd_req_wdata[c].read().to_uint();
                    std::cout << "[STUB] Cluster[" << c << "] cmd "
                              << (wr ? "WRITE" : "READ ")
                              << " addr=0x" << std::hex << addr;
                    if (wr) std::cout << " data=0x" << wdata;
                    std::cout << std::dec << " @" << sc_time_stamp() << std::endl;
                    cl_cmd_resp_valid[c].write(true);
                    cl_cmd_resp_rdata[c].write(0xDEADBEEF);
                } else {
                    cl_cmd_resp_valid[c].write(false);
                }
            }

            // NLU cmd: 1-cycle response with 0xDEADBEEF
            for (unsigned n = 0; n < kNluPorts; ++n) {
                if (nlu_cmd_req_valid[n].read()) {
                    uint32_t addr = nlu_cmd_req_addr[n].read().to_uint();
                    bool     wr   = nlu_cmd_req_write[n].read();
                    std::cout << "[STUB] NLU[" << n << "] cmd "
                              << (wr ? "WRITE" : "READ ")
                              << " addr=0x" << std::hex << addr << std::dec
                              << " @" << sc_time_stamp() << std::endl;
                    nlu_cmd_resp_valid[n].write(true);
                    nlu_cmd_resp_rdata[n].write(0xDEADBEEF);
                } else {
                    nlu_cmd_resp_valid[n].write(false);
                }
            }
            wait();
        }
    }

    void data_stub_proc() {
        // Reset
        for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
            cl_data_aw_ready[c].write(false);
            cl_data_w_ready[c].write(false);
            cl_data_b_valid[c].write(false);
            cl_data_b_resp[c].write(0);
            cl_data_ar_ready[c].write(false);
            cl_data_r_valid[c].write(false);
            cl_data_r_data[c].write(0xDEADBEEFDEADBEEFULL);
            cl_data_r_resp[c].write(0);
        }
        wait();

        while (true) {
            for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
                // AW + W → B (write stub)
                if (cl_data_aw_valid[c].read()) {
                    uint32_t addr = cl_data_aw_addr[c].read().to_uint();
                    std::cout << "[STUB] Cluster[" << c << "] data AW addr=0x"
                              << std::hex << addr << std::dec
                              << " @" << sc_time_stamp() << std::endl;
                    cl_data_aw_ready[c].write(true);
                    cl_data_w_ready[c].write(true);
                } else {
                    cl_data_aw_ready[c].write(false);
                    cl_data_w_ready[c].write(false);
                }

                // Provide B response when W is consumed
                if (cl_data_w_valid[c].read() && cl_data_w_ready[c].read()) {
                    cl_data_b_valid[c].write(true);
                    cl_data_b_resp[c].write(0);
                } else if (cl_data_b_valid[c].read() && cl_data_b_ready[c].read()) {
                    cl_data_b_valid[c].write(false);
                }

                // AR → R (read stub)
                if (cl_data_ar_valid[c].read()) {
                    uint32_t addr = cl_data_ar_addr[c].read().to_uint();
                    std::cout << "[STUB] Cluster[" << c << "] data AR addr=0x"
                              << std::hex << addr << std::dec
                              << " @" << sc_time_stamp() << std::endl;
                    cl_data_ar_ready[c].write(true);
                    cl_data_r_valid[c].write(true);
                    cl_data_r_data[c].write(0xDEADBEEFDEADBEEFULL);
                    cl_data_r_resp[c].write(0);
                } else {
                    cl_data_ar_ready[c].write(false);
                    if (cl_data_r_valid[c].read() && cl_data_r_ready[c].read()) {
                        cl_data_r_valid[c].write(false);
                    }
                }
            }
            wait();
        }
    }
};

// ============================================================================
// Build manifest in DRAM for the section loader
// ============================================================================

struct ManifestBuilder {
    uint32_t dram_base;         ///< Where in DRAM to place the manifest table
    uint32_t next_payload_addr; ///< Where to place the next section payload
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

        // Copy payload to DRAM
        dram->load_bytes(next_payload_addr, data, size);
        // Align next payload to 16 bytes (AXI beat size)
        next_payload_addr += (size + 15) & ~15u;
    }

    /// Write manifest table to DRAM and return (base, num_entries)
    void finalize() {
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            uint32_t addr = dram_base + static_cast<uint32_t>(i) * 32;
            // Word 0: section_kind (lo16) | flags (hi16)
            uint32_t w0 = e.section_kind | (static_cast<uint32_t>(e.flags) << 16);
            store32(addr + 0,  w0);
            store32(addr + 4,  e.dram_addr_lo);
            store32(addr + 8,  e.dram_addr_hi);
            store32(addr + 12, e.local_addr);
            store32(addr + 16, e.size_bytes);
            store32(addr + 20, e.crc32);
            store32(addr + 24, e.attr0);
            store32(addr + 28, e.reserved);
        }
    }

private:
    void store32(uint32_t addr, uint32_t val) {
        uint8_t buf[4];
        buf[0] = val & 0xFF;
        buf[1] = (val >> 8) & 0xFF;
        buf[2] = (val >> 16) & 0xFF;
        buf[3] = (val >> 24) & 0xFF;
        dram->load_bytes(addr, buf, 4);
    }
};

// ============================================================================
// sc_main
// ============================================================================

int sc_main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <rv32-elf> [max_cycles] [--core-debug]"
                  << " [--dump-dsram <hex_addr> <hex_length>]" << std::endl;
        return 1;
    }

    // Parse arguments
    std::string elf_path;
    uint32_t max_cycles = 500000;
    bool core_debug = false;
    bool dump_dsram = false;
    uint32_t dump_addr = 0;
    uint32_t dump_len  = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--core-debug") {
            core_debug = true;
        } else if (arg == "--dump-dsram") {
            dump_dsram = true;
            if (i + 2 < argc) {
                dump_addr = static_cast<uint32_t>(std::strtoul(argv[i+1], nullptr, 0));
                dump_len  = static_cast<uint32_t>(std::strtoul(argv[i+2], nullptr, 0));
                i += 2;
            } else {
                std::cerr << "Error: --dump-dsram requires <addr> <length>" << std::endl;
                return 1;
            }
        } else if (elf_path.empty()) {
            elf_path = arg;
        } else {
            max_cycles = static_cast<uint32_t>(std::atoi(argv[i]));
        }
    }

    if (elf_path.empty()) {
        std::cerr << "Error: ELF path required" << std::endl;
        return 1;
    }

    // Parse ELF
    uint32_t entry_point = 0;
    std::vector<elf::Segment> elf_segs;
    if (!elf::load_elf(elf_path, entry_point, elf_segs)) {
        return 1;
    }
    std::cout << "[TB] ELF loaded: " << elf_path << std::endl;
    std::cout << "[TB] Entry point: 0x" << std::hex << entry_point << std::dec << std::endl;
    std::cout << "[TB] Segments: " << elf_segs.size() << std::endl;
    for (size_t i = 0; i < elf_segs.size(); ++i) {
        std::cout << "[TB]   seg[" << i << "] paddr=0x" << std::hex
                  << elf_segs[i].paddr << " memsz=0x" << elf_segs[i].memsz
                  << std::dec << std::endl;
    }

    // Clock and reset
    sc_clock clk("clk", 10, SC_NS);
    sc_signal<bool> reset_n("reset_n");

    // Controller IRQ
    sc_signal<bool> controller_irq("controller_irq");

    // Host AXI4-Lite signals
    sc_signal<bool>         h_aw_valid("h_aw_valid");
    sc_signal<bool>         h_aw_ready("h_aw_ready");
    sc_signal<sc_uint<32>>  h_aw_addr("h_aw_addr");
    sc_signal<bool>         h_w_valid("h_w_valid");
    sc_signal<bool>         h_w_ready("h_w_ready");
    sc_signal<sc_uint<32>>  h_w_data("h_w_data");
    sc_signal<sc_uint<4>>   h_w_strb("h_w_strb");
    sc_signal<bool>         h_b_valid("h_b_valid");
    sc_signal<bool>         h_b_ready("h_b_ready");
    sc_signal<sc_uint<2>>   h_b_resp("h_b_resp");
    sc_signal<bool>         h_ar_valid("h_ar_valid");
    sc_signal<bool>         h_ar_ready("h_ar_ready");
    sc_signal<sc_uint<32>>  h_ar_addr("h_ar_addr");
    sc_signal<bool>         h_r_valid("h_r_valid");
    sc_signal<bool>         h_r_ready("h_r_ready");
    sc_signal<sc_uint<32>>  h_r_data("h_r_data");
    sc_signal<sc_uint<2>>   h_r_resp("h_r_resp");

    // DRAM AXI4 signals — loader read port
    sc_signal<bool>         ldr_ar_valid("ldr_ar_valid");
    sc_signal<bool>         ldr_ar_ready("ldr_ar_ready");
    sc_signal<sc_uint<32>>  ldr_ar_addr("ldr_ar_addr");
    sc_signal<sc_uint<8>>   ldr_ar_len("ldr_ar_len");
    sc_signal<bool>         ldr_r_valid("ldr_r_valid");
    sc_signal<bool>         ldr_r_ready("ldr_r_ready");
    sc_signal<sc_biguint<kMemAxiDataWidth>> ldr_r_data("ldr_r_data");
    sc_signal<sc_uint<2>>   ldr_r_resp("ldr_r_resp");
    sc_signal<bool>         ldr_r_last("ldr_r_last");

    // DRAM AXI4 signals — DMA port
    sc_signal<bool>         dma_aw_valid("dma_aw_valid");
    sc_signal<bool>         dma_aw_ready("dma_aw_ready");
    sc_signal<sc_uint<32>>  dma_aw_addr("dma_aw_addr");
    sc_signal<sc_uint<8>>   dma_aw_len("dma_aw_len");
    sc_signal<bool>         dma_w_valid("dma_w_valid");
    sc_signal<bool>         dma_w_ready("dma_w_ready");
    sc_signal<sc_biguint<kMemAxiDataWidth>> dma_w_data("dma_w_data");
    sc_signal<sc_uint<kMemAxiDataWidth / 8>> dma_w_strb("dma_w_strb");
    sc_signal<bool>         dma_w_last("dma_w_last");
    sc_signal<bool>         dma_b_valid("dma_b_valid");
    sc_signal<bool>         dma_b_ready("dma_b_ready");
    sc_signal<sc_uint<2>>   dma_b_resp("dma_b_resp");
    sc_signal<bool>         dma_ar_valid("dma_ar_valid");
    sc_signal<bool>         dma_ar_ready("dma_ar_ready");
    sc_signal<sc_uint<32>>  dma_ar_addr("dma_ar_addr");
    sc_signal<sc_uint<8>>   dma_ar_len("dma_ar_len");
    sc_signal<bool>         dma_r_valid("dma_r_valid");
    sc_signal<bool>         dma_r_ready("dma_r_ready");
    sc_signal<sc_biguint<kMemAxiDataWidth>> dma_r_data("dma_r_data");
    sc_signal<sc_uint<2>>   dma_r_resp("dma_r_resp");
    sc_signal<bool>         dma_r_last("dma_r_last");

    // Cluster cmd signals
    sc_signal<bool>         cl_cmd_req_valid[NUM_CLUSTERS];
    sc_signal<bool>         cl_cmd_req_write[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_cmd_req_addr[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_cmd_req_wdata[NUM_CLUSTERS];
    sc_signal<bool>         cl_cmd_resp_valid[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_cmd_resp_rdata[NUM_CLUSTERS];

    // NLU cmd signals
    static constexpr unsigned kNluPorts = NUM_NLU > 0 ? NUM_NLU : 1;
    sc_signal<bool>         nlu_cmd_req_valid[kNluPorts];
    sc_signal<bool>         nlu_cmd_req_write[kNluPorts];
    sc_signal<sc_uint<32>>  nlu_cmd_req_addr[kNluPorts];
    sc_signal<sc_uint<32>>  nlu_cmd_req_wdata[kNluPorts];
    sc_signal<bool>         nlu_cmd_resp_valid[kNluPorts];
    sc_signal<sc_uint<32>>  nlu_cmd_resp_rdata[kNluPorts];

    // Cluster IRQ
    sc_signal<bool>         cluster_irq[NUM_CLUSTERS];
    sc_signal<bool>         nlu_irq[kNluPorts];

    // Cluster data AXI4-Lite signals
    sc_signal<bool>         cl_data_aw_valid[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_aw_ready[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_data_aw_addr[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_w_valid[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_w_ready[NUM_CLUSTERS];
    sc_signal<sc_biguint<kClAxiDataWidth>> cl_data_w_data[NUM_CLUSTERS];
    sc_signal<sc_uint<kClAxiDataWidth / 8>> cl_data_w_strb[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_b_valid[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_b_ready[NUM_CLUSTERS];
    sc_signal<sc_uint<2>>   cl_data_b_resp[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_ar_valid[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_ar_ready[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_data_ar_addr[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_r_valid[NUM_CLUSTERS];
    sc_signal<bool>         cl_data_r_ready[NUM_CLUSTERS];
    sc_signal<sc_biguint<kClAxiDataWidth>> cl_data_r_data[NUM_CLUSTERS];
    sc_signal<sc_uint<2>>   cl_data_r_resp[NUM_CLUSTERS];

    // NLU data requester signals
    sc_signal<bool>         nlu_data_req_valid("nlu_data_req_valid");
    sc_signal<bool>         nlu_data_req_write("nlu_data_req_write");
    sc_signal<sc_uint<32>>  nlu_data_req_cluster_id("nlu_data_req_cluster_id");
    sc_signal<sc_uint<32>>  nlu_data_req_addr("nlu_data_req_addr");
    sc_signal<sc_biguint<kClAxiDataWidth>> nlu_data_req_wdata("nlu_data_req_wdata");
    sc_signal<sc_uint<kClAxiDataWidth / 8>> nlu_data_req_wstrb("nlu_data_req_wstrb");
    sc_signal<bool>         nlu_data_req_ready("nlu_data_req_ready");
    sc_signal<bool>         nlu_data_resp_valid("nlu_data_resp_valid");
    sc_signal<sc_biguint<kClAxiDataWidth>> nlu_data_resp_rdata("nlu_data_resp_rdata");
    sc_signal<bool>         nlu_data_resp_error("nlu_data_resp_error");

    // ========================================================================
    // DUT: CoreController
    // ========================================================================
    CoreController<NUM_CLUSTERS, NUM_NLU> dut("dut");
    dut.core_debug = core_debug;

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

    // DMA DRAM AXI
    dut.m_dma_mem_axi_aw_valid_o(dma_aw_valid);
    dut.m_dma_mem_axi_aw_ready_i(dma_aw_ready);
    dut.m_dma_mem_axi_aw_addr_o(dma_aw_addr);
    dut.m_dma_mem_axi_aw_len_o(dma_aw_len);
    dut.m_dma_mem_axi_w_valid_o(dma_w_valid);
    dut.m_dma_mem_axi_w_ready_i(dma_w_ready);
    dut.m_dma_mem_axi_w_data_o(dma_w_data);
    dut.m_dma_mem_axi_w_strb_o(dma_w_strb);
    dut.m_dma_mem_axi_w_last_o(dma_w_last);
    dut.m_dma_mem_axi_b_valid_i(dma_b_valid);
    dut.m_dma_mem_axi_b_ready_o(dma_b_ready);
    dut.m_dma_mem_axi_b_resp_i(dma_b_resp);
    dut.m_dma_mem_axi_ar_valid_o(dma_ar_valid);
    dut.m_dma_mem_axi_ar_ready_i(dma_ar_ready);
    dut.m_dma_mem_axi_ar_addr_o(dma_ar_addr);
    dut.m_dma_mem_axi_ar_len_o(dma_ar_len);
    dut.m_dma_mem_axi_r_valid_i(dma_r_valid);
    dut.m_dma_mem_axi_r_ready_o(dma_r_ready);
    dut.m_dma_mem_axi_r_data_i(dma_r_data);
    dut.m_dma_mem_axi_r_resp_i(dma_r_resp);
    dut.m_dma_mem_axi_r_last_i(dma_r_last);

    // Loader DRAM AXI
    dut.m_ldr_mem_axi_ar_valid_o(ldr_ar_valid);
    dut.m_ldr_mem_axi_ar_ready_i(ldr_ar_ready);
    dut.m_ldr_mem_axi_ar_addr_o(ldr_ar_addr);
    dut.m_ldr_mem_axi_ar_len_o(ldr_ar_len);
    dut.m_ldr_mem_axi_r_valid_i(ldr_r_valid);
    dut.m_ldr_mem_axi_r_ready_o(ldr_r_ready);
    dut.m_ldr_mem_axi_r_data_i(ldr_r_data);
    dut.m_ldr_mem_axi_r_resp_i(ldr_r_resp);
    dut.m_ldr_mem_axi_r_last_i(ldr_r_last);

    // Cluster cmd / data
    for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
        dut.cl_cmd_req_valid_o[c](cl_cmd_req_valid[c]);
        dut.cl_cmd_req_write_o[c](cl_cmd_req_write[c]);
        dut.cl_cmd_req_addr_o[c](cl_cmd_req_addr[c]);
        dut.cl_cmd_req_wdata_o[c](cl_cmd_req_wdata[c]);
        dut.cl_cmd_resp_valid_i[c](cl_cmd_resp_valid[c]);
        dut.cl_cmd_resp_rdata_i[c](cl_cmd_resp_rdata[c]);

        dut.m_cl_data_aw_valid_o[c](cl_data_aw_valid[c]);
        dut.m_cl_data_aw_ready_i[c](cl_data_aw_ready[c]);
        dut.m_cl_data_aw_addr_o[c](cl_data_aw_addr[c]);
        dut.m_cl_data_w_valid_o[c](cl_data_w_valid[c]);
        dut.m_cl_data_w_ready_i[c](cl_data_w_ready[c]);
        dut.m_cl_data_w_data_o[c](cl_data_w_data[c]);
        dut.m_cl_data_w_strb_o[c](cl_data_w_strb[c]);
        dut.m_cl_data_b_valid_i[c](cl_data_b_valid[c]);
        dut.m_cl_data_b_ready_o[c](cl_data_b_ready[c]);
        dut.m_cl_data_b_resp_i[c](cl_data_b_resp[c]);
        dut.m_cl_data_ar_valid_o[c](cl_data_ar_valid[c]);
        dut.m_cl_data_ar_ready_i[c](cl_data_ar_ready[c]);
        dut.m_cl_data_ar_addr_o[c](cl_data_ar_addr[c]);
        dut.m_cl_data_r_valid_i[c](cl_data_r_valid[c]);
        dut.m_cl_data_r_ready_o[c](cl_data_r_ready[c]);
        dut.m_cl_data_r_data_i[c](cl_data_r_data[c]);
        dut.m_cl_data_r_resp_i[c](cl_data_r_resp[c]);
    }

    // NLU cmd
    for (unsigned n = 0; n < kNluPorts; ++n) {
        dut.nlu_cmd_req_valid_o[n](nlu_cmd_req_valid[n]);
        dut.nlu_cmd_req_write_o[n](nlu_cmd_req_write[n]);
        dut.nlu_cmd_req_addr_o[n](nlu_cmd_req_addr[n]);
        dut.nlu_cmd_req_wdata_o[n](nlu_cmd_req_wdata[n]);
        dut.nlu_cmd_resp_valid_i[n](nlu_cmd_resp_valid[n]);
        dut.nlu_cmd_resp_rdata_i[n](nlu_cmd_resp_rdata[n]);
    }

    // IRQs
    for (unsigned c = 0; c < NUM_CLUSTERS; ++c) dut.cluster_irq_i[c](cluster_irq[c]);
    for (unsigned n = 0; n < kNluPorts; ++n) dut.nlu_irq_i[n](nlu_irq[n]);

    dut.controller_irq_o(controller_irq);

    // NLU data
    dut.nlu_data_req_valid_i(nlu_data_req_valid);
    dut.nlu_data_req_write_i(nlu_data_req_write);
    dut.nlu_data_req_cluster_id_i(nlu_data_req_cluster_id);
    dut.nlu_data_req_addr_i(nlu_data_req_addr);
    dut.nlu_data_req_wdata_i(nlu_data_req_wdata);
    dut.nlu_data_req_wstrb_i(nlu_data_req_wstrb);
    dut.nlu_data_req_ready_o(nlu_data_req_ready);
    dut.nlu_data_resp_valid_o(nlu_data_resp_valid);
    dut.nlu_data_resp_rdata_o(nlu_data_resp_rdata);
    dut.nlu_data_resp_error_o(nlu_data_resp_error);

    // ========================================================================
    // Fake DRAM
    // ========================================================================
    FakeDram dram("fake_dram");
    dram.clk(clk);
    dram.reset_n(reset_n);

    // Loader port
    dram.ldr_ar_valid(ldr_ar_valid);
    dram.ldr_ar_ready(ldr_ar_ready);
    dram.ldr_ar_addr(ldr_ar_addr);
    dram.ldr_ar_len(ldr_ar_len);
    dram.ldr_r_valid(ldr_r_valid);
    dram.ldr_r_ready(ldr_r_ready);
    dram.ldr_r_data(ldr_r_data);
    dram.ldr_r_resp(ldr_r_resp);
    dram.ldr_r_last(ldr_r_last);

    // DMA port
    dram.dma_aw_valid(dma_aw_valid);
    dram.dma_aw_ready(dma_aw_ready);
    dram.dma_aw_addr(dma_aw_addr);
    dram.dma_aw_len(dma_aw_len);
    dram.dma_w_valid(dma_w_valid);
    dram.dma_w_ready(dma_w_ready);
    dram.dma_w_data(dma_w_data);
    dram.dma_w_strb(dma_w_strb);
    dram.dma_w_last(dma_w_last);
    dram.dma_b_valid(dma_b_valid);
    dram.dma_b_ready(dma_b_ready);
    dram.dma_b_resp(dma_b_resp);
    dram.dma_ar_valid(dma_ar_valid);
    dram.dma_ar_ready(dma_ar_ready);
    dram.dma_ar_addr(dma_ar_addr);
    dram.dma_ar_len(dma_ar_len);
    dram.dma_r_valid(dma_r_valid);
    dram.dma_r_ready(dma_r_ready);
    dram.dma_r_data(dma_r_data);
    dram.dma_r_resp(dma_r_resp);
    dram.dma_r_last(dma_r_last);

    // ========================================================================
    // Bus stubs
    // ========================================================================
    BusStub stubs("bus_stubs");
    stubs.clk(clk);
    stubs.reset_n(reset_n);

    for (unsigned c = 0; c < NUM_CLUSTERS; ++c) {
        stubs.cl_cmd_req_valid[c](cl_cmd_req_valid[c]);
        stubs.cl_cmd_req_write[c](cl_cmd_req_write[c]);
        stubs.cl_cmd_req_addr[c](cl_cmd_req_addr[c]);
        stubs.cl_cmd_req_wdata[c](cl_cmd_req_wdata[c]);
        stubs.cl_cmd_resp_valid[c](cl_cmd_resp_valid[c]);
        stubs.cl_cmd_resp_rdata[c](cl_cmd_resp_rdata[c]);

        stubs.cl_data_aw_valid[c](cl_data_aw_valid[c]);
        stubs.cl_data_aw_ready[c](cl_data_aw_ready[c]);
        stubs.cl_data_aw_addr[c](cl_data_aw_addr[c]);
        stubs.cl_data_w_valid[c](cl_data_w_valid[c]);
        stubs.cl_data_w_ready[c](cl_data_w_ready[c]);
        stubs.cl_data_w_data[c](cl_data_w_data[c]);
        stubs.cl_data_w_strb[c](cl_data_w_strb[c]);
        stubs.cl_data_b_valid[c](cl_data_b_valid[c]);
        stubs.cl_data_b_ready[c](cl_data_b_ready[c]);
        stubs.cl_data_b_resp[c](cl_data_b_resp[c]);
        stubs.cl_data_ar_valid[c](cl_data_ar_valid[c]);
        stubs.cl_data_ar_ready[c](cl_data_ar_ready[c]);
        stubs.cl_data_ar_addr[c](cl_data_ar_addr[c]);
        stubs.cl_data_r_valid[c](cl_data_r_valid[c]);
        stubs.cl_data_r_ready[c](cl_data_r_ready[c]);
        stubs.cl_data_r_data[c](cl_data_r_data[c]);
        stubs.cl_data_r_resp[c](cl_data_r_resp[c]);

        stubs.cluster_irq[c](cluster_irq[c]);
    }

    for (unsigned n = 0; n < kNluPorts; ++n) {
        stubs.nlu_cmd_req_valid[n](nlu_cmd_req_valid[n]);
        stubs.nlu_cmd_req_write[n](nlu_cmd_req_write[n]);
        stubs.nlu_cmd_req_addr[n](nlu_cmd_req_addr[n]);
        stubs.nlu_cmd_req_wdata[n](nlu_cmd_req_wdata[n]);
        stubs.nlu_cmd_resp_valid[n](nlu_cmd_resp_valid[n]);
        stubs.nlu_cmd_resp_rdata[n](nlu_cmd_resp_rdata[n]);
        stubs.nlu_irq[n](nlu_irq[n]);
    }

    stubs.nlu_data_req_valid(nlu_data_req_valid);
    stubs.nlu_data_req_write(nlu_data_req_write);
    stubs.nlu_data_req_cluster_id(nlu_data_req_cluster_id);
    stubs.nlu_data_req_addr(nlu_data_req_addr);
    stubs.nlu_data_req_wdata(nlu_data_req_wdata);
    stubs.nlu_data_req_wstrb(nlu_data_req_wstrb);
    stubs.nlu_data_req_ready(nlu_data_req_ready);
    stubs.nlu_data_resp_valid(nlu_data_resp_valid);
    stubs.nlu_data_resp_rdata(nlu_data_resp_rdata);
    stubs.nlu_data_resp_error(nlu_data_resp_error);

    // ========================================================================
    // Test driver
    // ========================================================================
    TestDriver driver("driver");
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
    // Populate DRAM: ELF segments → manifest + payload
    // ========================================================================

    // Place manifest at DRAM 0x8000_0000, payloads after
    const uint32_t kManifestDramAddr = 0x80000000;
    const uint32_t kPayloadDramBase  = 0x80010000;

    ManifestBuilder builder(&dram, kManifestDramAddr, kPayloadDramBase);

    for (const auto& seg : elf_segs) {
        // Determine section kind and local address based on ELF paddr
        SectionKind kind;
        uint32_t local_addr;

        if (seg.paddr >= kBaseInstRam && seg.paddr < kBaseInstRam + kIsramBytes) {
            // Code segment → ISRAM
            kind = SectionKind::CORE;
            local_addr = seg.paddr - kBaseInstRam;
        } else if (seg.paddr >= kBaseDataRam && seg.paddr < kBaseDataRam + kDataSramBytes) {
            // Data segment → DSRAM
            kind = SectionKind::JOB; // any non-CORE kind routes to DSRAM
            local_addr = seg.paddr - kBaseDataRam;
        } else {
            std::cout << "[TB] WARNING: Skipping segment at paddr=0x"
                      << std::hex << seg.paddr << " (not in ISRAM/DSRAM range)"
                      << std::dec << std::endl;
            continue;
        }

        builder.add_section(kind, local_addr,
                            seg.data.data(),
                            static_cast<uint32_t>(seg.memsz));
        std::cout << "[TB] Manifest entry: kind=" << kind
                  << " local=0x" << std::hex << local_addr
                  << " size=0x" << seg.memsz << std::dec << std::endl;
    }

    builder.finalize();
    std::cout << "[TB] Manifest built: " << builder.entries.size()
              << " entries at DRAM 0x" << std::hex << kManifestDramAddr
              << std::dec << std::endl;

    // Configure driver
    driver.manifest_dram_addr = kManifestDramAddr;
    driver.manifest_num_entries = static_cast<uint32_t>(builder.entries.size());
    driver.boot_addr = entry_point;
    driver.max_cycles = max_cycles;

    // ========================================================================
    // Reset sequence and run
    // ========================================================================

    reset_n.write(false);
    sc_start(50, SC_NS);  // 5 clock cycles of reset
    reset_n.write(true);

    std::cout << "[TB] Reset released, simulation starting..."
              << (core_debug ? " (core_debug ON)" : "") << std::endl;
    sc_start();

    std::cout << "[TB] Simulation ended at " << sc_time_stamp() << std::endl;

    // ========================================================================
    // Post-simulation: Dump Data SRAM contents if requested
    // ========================================================================
    if (dump_dsram) {
        std::cout << "\n[TB] === DSRAM Dump: addr=0x" << std::hex << dump_addr
                  << " length=0x" << dump_len << " ===" << std::dec << std::endl;
        // Align to 4-byte boundary
        const uint32_t start = dump_addr & ~3u;
        const uint32_t end   = (dump_addr + dump_len + 3) & ~3u;
        for (uint32_t a = start; a < end; a += 16) {
            std::cout << "  0x" << std::hex << std::setw(8) << std::setfill('0')
                      << (kBaseDataRam + a) << ": ";
            // Print 4 words per line (16 bytes)
            for (unsigned w = 0; w < 4 && (a + w * 4) < end; ++w) {
                uint32_t val = dut.dsram_read_word(a + w * 4);
                std::cout << std::hex << std::setw(8) << std::setfill('0')
                          << val << " ";
            }
            // Print ASCII representation
            std::cout << " |";
            for (unsigned b = 0; b < 16 && (a + b) < end; ++b) {
                uint32_t word = dut.dsram_read_word((a + (b & ~3u)));
                uint8_t byte = (word >> ((b & 3) * 8)) & 0xFF;
                std::cout << (char)((byte >= 0x20 && byte < 0x7F) ? byte : '.');
            }
            std::cout << "|" << std::dec << std::endl;
        }
        std::cout << "[TB] === End DSRAM Dump ===" << std::endl;
    }

    return 0;
}
