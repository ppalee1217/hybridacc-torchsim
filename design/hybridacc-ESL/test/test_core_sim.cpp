/**
 * @file test_core_sim.cpp
 * @brief Core Controller DMA simulation testbench.
 *
 * Instantiates CoreController<1,0> with:
 *   - FakeDram (DRAM model)
 *   - FakeClusterSpm (real storage-backed cluster SPM)
 *   - BootTestDriver (host AXI driver)
 *
 * Firmware tests:
 *   1. DMA DRAM → Cluster SPM, core waits for interrupt
 *   2. DMA Cluster SPM → DRAM, core waits for interrupt
 *
 * Pre-loads a test pattern in DRAM at 0x80020000 for test 1.
 * Post-sim verifies DRAM at 0x80030000 for test 2 and DSRAM for results.
 *
 * Usage:
 *   ./test_core_sim <path-to-rv32-elf> [max_cycles] [--core-debug]
 */

#include "core_tb_utils.hpp"

static constexpr unsigned NUM_CLUSTERS = 1;
static constexpr unsigned NUM_NLU = 0;
static constexpr unsigned kNluPorts = NUM_NLU > 0 ? NUM_NLU : 1;

// DRAM addresses matching firmware expectations
static constexpr uint32_t kTestDramSrc  = 0x80020000;
static constexpr uint32_t kTestDramDst  = 0x80030000;
static constexpr uint32_t kTestBytes    = 64; // 16 words

// ============================================================================
// sc_main
// ============================================================================

int sc_main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <rv32-elf> [max_cycles] [--core-debug]" << std::endl;
        return 1;
    }

    std::string elf_path;
    uint32_t max_cycles = 500000;
    bool core_debug = false;
    bool dma_check = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--core-debug") {
            core_debug = true;
        } else if (arg == "--dma-check") {
            dma_check = true;
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
    if (!elf::load_elf(elf_path, entry_point, elf_segs)) return 1;

    std::cout << "[TB] ELF loaded: " << elf_path << std::endl;
    std::cout << "[TB] Entry point: 0x" << std::hex << entry_point << std::dec << std::endl;
    std::cout << "[TB] Segments: " << elf_segs.size() << std::endl;
    for (size_t i = 0; i < elf_segs.size(); ++i) {
        std::cout << "[TB]   seg[" << i << "] paddr=0x" << std::hex
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

    // DRAM AXI (single shared port)
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

    // Cluster[0] cmd signals
    sc_signal<bool>         cl_cmd_req_valid[NUM_CLUSTERS];
    sc_signal<bool>         cl_cmd_req_write[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_cmd_req_addr[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_cmd_req_wdata[NUM_CLUSTERS];
    sc_signal<bool>         cl_cmd_resp_valid[NUM_CLUSTERS];
    sc_signal<sc_uint<32>>  cl_cmd_resp_rdata[NUM_CLUSTERS];

    // Cluster[0] data AXI signals
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

    // Cluster IRQ
    sc_signal<bool>         cluster_irq[NUM_CLUSTERS];

    // NLU dummy signals
    sc_signal<bool>         nlu_cmd_req_valid[kNluPorts];
    sc_signal<bool>         nlu_cmd_req_write[kNluPorts];
    sc_signal<sc_uint<32>>  nlu_cmd_req_addr[kNluPorts];
    sc_signal<sc_uint<32>>  nlu_cmd_req_wdata[kNluPorts];
    sc_signal<bool>         nlu_cmd_resp_valid[kNluPorts];
    sc_signal<sc_uint<32>>  nlu_cmd_resp_rdata[kNluPorts];
    sc_signal<bool>         nlu_irq[kNluPorts];

    // NLU data requester (stub: inactive)
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

    // DMA DRAM AXI (shared port)
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
    for (unsigned n = 0; n < kNluPorts; ++n)    dut.nlu_irq_i[n](nlu_irq[n]);

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
    // FakeDram
    // ========================================================================

    FakeDram dram("fake_dram");
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
    // FakeClusterSpm (cluster[0])
    // ========================================================================

    FakeClusterSpm cluster_spm("cluster_spm_0");
    cluster_spm.clk(clk);
    cluster_spm.reset_n(reset_n);

    // Cmd interface
    cluster_spm.cmd_req_valid(cl_cmd_req_valid[0]);
    cluster_spm.cmd_req_write(cl_cmd_req_write[0]);
    cluster_spm.cmd_req_addr(cl_cmd_req_addr[0]);
    cluster_spm.cmd_req_wdata(cl_cmd_req_wdata[0]);
    cluster_spm.cmd_resp_valid(cl_cmd_resp_valid[0]);
    cluster_spm.cmd_resp_rdata(cl_cmd_resp_rdata[0]);

    // Data AXI interface
    cluster_spm.data_aw_valid(cl_data_aw_valid[0]);
    cluster_spm.data_aw_ready(cl_data_aw_ready[0]);
    cluster_spm.data_aw_addr(cl_data_aw_addr[0]);
    cluster_spm.data_w_valid(cl_data_w_valid[0]);
    cluster_spm.data_w_ready(cl_data_w_ready[0]);
    cluster_spm.data_w_data(cl_data_w_data[0]);
    cluster_spm.data_w_strb(cl_data_w_strb[0]);
    cluster_spm.data_b_valid(cl_data_b_valid[0]);
    cluster_spm.data_b_ready(cl_data_b_ready[0]);
    cluster_spm.data_b_resp(cl_data_b_resp[0]);
    cluster_spm.data_ar_valid(cl_data_ar_valid[0]);
    cluster_spm.data_ar_ready(cl_data_ar_ready[0]);
    cluster_spm.data_ar_addr(cl_data_ar_addr[0]);
    cluster_spm.data_r_valid(cl_data_r_valid[0]);
    cluster_spm.data_r_ready(cl_data_r_ready[0]);
    cluster_spm.data_r_data(cl_data_r_data[0]);
    cluster_spm.data_r_resp(cl_data_r_resp[0]);

    // Cluster IRQ
    cluster_spm.cluster_irq(cluster_irq[0]);

    // NLU stubs: keep inactive
    for (unsigned n = 0; n < kNluPorts; ++n) {
        nlu_cmd_resp_valid[n].write(false);
        nlu_cmd_resp_rdata[n].write(0);
        nlu_irq[n].write(false);
    }

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
    // Populate DRAM: ELF → manifest + DMA test data
    // ========================================================================

    const uint32_t kManifestDramAddr = 0x80000000;
    const uint32_t kPayloadDramBase  = 0x80010000;

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
            std::cout << "[TB] WARNING: Skipping segment at paddr=0x"
                      << std::hex << seg.paddr << std::dec << std::endl;
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
    std::cout << "[TB] Manifest: " << builder.entries.size()
              << " entries at DRAM 0x" << std::hex << kManifestDramAddr
              << std::dec << std::endl;

    // Pre-load DRAM test pattern for DMA test 1 (DRAM → Cluster SPM)
    // Pattern: word[i] = i + 1
    std::cout << "[TB] Pre-loading DRAM test pattern at 0x" << std::hex
              << kTestDramSrc << std::dec << std::endl;
    for (uint32_t i = 0; i < kTestBytes / 4; ++i) {
        dram.store32(kTestDramSrc + i * 4, i + 1);
    }

    // Configure driver
    driver.manifest_dram_addr = kManifestDramAddr;
    driver.manifest_num_entries = static_cast<uint32_t>(builder.entries.size());
    driver.boot_addr = entry_point;
    driver.max_cycles = max_cycles;

    // ========================================================================
    // Reset and run
    // ========================================================================

    reset_n.write(false);
    sc_start(50, SC_NS);
    reset_n.write(true);

    std::cout << "[TB] Reset released" << (core_debug ? " (core_debug ON)" : "")
              << std::endl;
    sc_start();

    std::cout << "[TB] Simulation ended at " << sc_time_stamp() << std::endl;

    // ========================================================================
    // Post-simulation verification
    // ========================================================================

    // Read DSRAM test results
    uint32_t t_total = dut.dsram_read_word(0x00);
    uint32_t t_pass  = dut.dsram_read_word(0x04);
    uint32_t t_fail  = dut.dsram_read_word(0x08);
    uint32_t t_ff    = dut.dsram_read_word(0x0C);

    // Detect uninitialised DSRAM (firmware never wrote test results)
    const uint32_t kSentinel = 0xCAFECAFE;
    if (t_total == kSentinel && t_pass == kSentinel &&
        t_fail == kSentinel && t_ff   == kSentinel) {
        t_total = t_pass = t_fail = t_ff = 0;
    }

    std::cout << "\n[TB] === Firmware Test Results ===" << std::endl;
    std::cout << "[TB] Total: " << t_total
              << "  Pass: " << t_pass
              << "  Fail: " << t_fail
              << "  First-fail: " << t_ff << std::endl;

    // Verify Cluster SPM contents (test 1: DRAM → Cluster SPM)
    bool cl_ok = true;
    bool dram_ok = true;

    if (dma_check) {
        std::cout << "\n[TB] === Cluster SPM Verification (Test 1) ===" << std::endl;
        for (uint32_t i = 0; i < kTestBytes / 4; ++i) {
            uint32_t expected = i + 1;
            uint32_t actual = cluster_spm.read_word(i * 4);
            if (actual != expected) {
                std::cout << "[TB] FAIL: cluster_spm[0x" << std::hex << (i * 4)
                          << "] = 0x" << actual << " expected 0x" << expected
                          << std::dec << std::endl;
                cl_ok = false;
            }
        }
        if (cl_ok) std::cout << "[TB] Cluster SPM: PASS" << std::endl;

        // Verify DRAM contents (test 2: Cluster SPM → DRAM)
        std::cout << "\n[TB] === DRAM Verification (Test 2) ===" << std::endl;
        for (uint32_t i = 0; i < kTestBytes / 4; ++i) {
            uint32_t expected = 0xA0 + i;
            uint32_t actual = dram.read_word(kTestDramDst + i * 4);
            if (actual != expected) {
                std::cout << "[TB] FAIL: dram[0x" << std::hex
                          << (kTestDramDst + i * 4)
                          << "] = 0x" << actual << " expected 0x" << expected
                          << std::dec << std::endl;
                dram_ok = false;
            }
        }
        if (dram_ok) std::cout << "[TB] DRAM: PASS" << std::endl;
    }

    // Summary
    std::cout << "\n[TB] === OVERALL ===" << std::endl;
    bool all_pass = (t_fail == 0) && cl_ok && dram_ok;
    std::cout << "[TB] " << (all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
              << std::endl;

    return all_pass ? 0 : 1;
}
