#pragma once

#include <iostream>
#include <systemc>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <cassert>

#include "Utils/SRAM.hpp"
#include "AXI4_lite/axi4-lite.hpp"
#include "Utils/FIFO.hpp"
#include "Utils/utils.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

/**
 * @brief Scratchpad Memory (SPM) SystemC Module
 * Implementation based on SPM.md specification
 *
 * RTL-style refactoring goals:
 *  1. All state elements are sc_signal<T> with _reg suffix (updated only in seq_process).
 *  2. Every group of output signals is driven by a dedicated SC_METHOD (comb_*).
 *  3. Internal software-style std::deque queues are replaced by hybridacc::FIFO<T>
 *     module instances with explicit signal wiring (port-based interface).
 *  4. Intermediate combinational "fire" / "next" signals connect comb processes to
 *     the sequential register-update process without creating delta-cycle loops.
 */
template <
    unsigned NUM_NOC_PORTS          = 4,
    unsigned BANKS_PER_GROUP        = 3,
    unsigned BANK_DATA_WIDTH        = 64,
    unsigned BANK_DEPTH             = 8192,
    unsigned SRAM_BANK_LATENCY      = 1,
    unsigned SRAM_BANK_PIPELINE_DEPTH = 1,
    unsigned ADDR_WIDTH             = 32,
    unsigned MAX_OUTSTANDING        = 8,
    unsigned DMA_MAX_OUTSTANDING    = 8>
SC_MODULE(ScratchpadMemory) {

    // -------------------------------------------------------------------------
    // Parameters
    // -------------------------------------------------------------------------
    static constexpr unsigned NUM_GROUPS     = NUM_NOC_PORTS;
    static constexpr unsigned TOTAL_BANKS    = NUM_GROUPS * BANKS_PER_GROUP;
    static constexpr unsigned NOC_DATA_WIDTH = BANKS_PER_GROUP * BANK_DATA_WIDTH;

    // Address mapping constants (word-address perspective)
    static constexpr unsigned GROUP_LINEAR_WORDS = BANKS_PER_GROUP * BANK_DEPTH;
    static constexpr unsigned GROUP_SPAN_WORDS   = (BANKS_PER_GROUP + 1) * BANK_DEPTH;

    // Per-bank word helpers
    static constexpr unsigned  BYTES_PER_BANK_WORD = BANK_DATA_WIDTH / 8;
    static constexpr uint64_t  BANK_BYTE_MASK       = (1ULL << BYTES_PER_BANK_WORD) - 1;

    // Payload types
    using spm_req_t  = spm_request_t <ADDR_WIDTH, NOC_DATA_WIDTH>;
    using spm_resp_t = spm_response_t<NOC_DATA_WIDTH>;

    // -------------------------------------------------------------------------
    // SC-compatible internal struct types
    // (required for hybridacc::FIFO<T> which stores values in sc_signal<T>)
    // -------------------------------------------------------------------------
    enum class ReqMode : uint8_t { LINEAR = 0, PARALLEL = 1 };

    struct ReadMeta {
        bool     is_dma  = false;
        uint8_t  port_id = 0;
        ReqMode  mode    = ReqMode::LINEAR;
        uint32_t addr    = 0; // local word-address within the group

        bool operator==(const ReadMeta& o) const {
            return is_dma == o.is_dma && port_id == o.port_id &&
                   mode == o.mode && addr == o.addr;
        }
        bool operator!=(const ReadMeta& o) const { return !(*this == o); }
        friend std::ostream& operator<<(std::ostream& os, const ReadMeta& m) {
            os << "{dma=" << m.is_dma << ",p=" << (int)m.port_id
               << ",mode=" << ((m.mode == ReqMode::LINEAR) ? "LIN" : "PAR")
               << ",addr=" << m.addr << "}";
            return os;
        }
        friend void sc_trace(sc_core::sc_trace_file* tf, const ReadMeta& m,
                             const std::string& name) {
            // Struct contains no primitive sc_dt types, trace is a no-op.
            (void)tf; (void)m; (void)name;
        }
    };

    struct DmaWriteReq {
        sc_uint<ADDR_WIDTH>           addr{0};
        sc_biguint<BANK_DATA_WIDTH>   data{0};
        sc_uint<BANK_DATA_WIDTH / 8>  strb{0};

        bool operator==(const DmaWriteReq& o) const {
            return addr == o.addr && data == o.data && strb == o.strb;
        }
        bool operator!=(const DmaWriteReq& o) const { return !(*this == o); }
        friend std::ostream& operator<<(std::ostream& os, const DmaWriteReq& r) {
            os << "{addr=0x" << std::hex << r.addr.to_uint()
               << ",strb=0x" << r.strb.to_uint() << "}";
            return os;
        }
        friend void sc_trace(sc_core::sc_trace_file* tf, const DmaWriteReq& r,
                             const std::string& name) {
            sc_core::sc_trace(tf, r.addr, name + ".addr");
            sc_core::sc_trace(tf, r.strb, name + ".strb");
        }
    };

    // -------------------------------------------------------------------------
    // FIFO depth constants
    // -------------------------------------------------------------------------
    static constexpr unsigned DMA_AW_FIFO_DEPTH        = DMA_MAX_OUTSTANDING;
    static constexpr unsigned DMA_W_FIFO_DEPTH         = DMA_MAX_OUTSTANDING;
    static constexpr unsigned DMA_WRITE_REQ_FIFO_DEPTH = DMA_MAX_OUTSTANDING;
    static constexpr unsigned DMA_READ_REQ_FIFO_DEPTH  = DMA_MAX_OUTSTANDING;
    static constexpr unsigned DMA_READ_RESP_FIFO_DEPTH = DMA_MAX_OUTSTANDING;
    static constexpr unsigned GROUP_META_FIFO_DEPTH    = MAX_OUTSTANDING;
    // +2 headroom: write-resp (immediate) and read-resp can both push in one cycle
    static constexpr unsigned PORT_RESP_FIFO_DEPTH     = MAX_OUTSTANDING + 2;

    // =========================================================================
    // 2.2  Global & Config ports
    // =========================================================================
    sc_in<bool>       clk      {"clk"};
    sc_in<bool>       reset_n  {"reset_n"};
    sc_in<bool>       pmu_rst_i{"pmu_rst_i"};
    sc_in<bool>       drop_noc_resp_i{"drop_noc_resp_i"};
    sc_in<bool>       soft_reset_i{"soft_reset_i"};   // unified SOFT_RESET

    sc_in<sc_uint<8>> config_map_i    {"config_map_i"};
    sc_in<bool>       config_update_i {"config_update_i"};
    sc_in<bool>       arb_policy_i    {"arb_policy_i"};

    // =========================================================================
    // 2.3  NoC slave ports (NUM_NOC_PORTS)
    // =========================================================================
    sc_vector<sc_in<bool>>        spm_req_valid_i{"spm_req_valid_i", NUM_NOC_PORTS};
    sc_vector<sc_out<bool>>       spm_req_ready_o{"spm_req_ready_o", NUM_NOC_PORTS};
    sc_vector<sc_in<spm_req_t>>   spm_req_i      {"spm_req_i",       NUM_NOC_PORTS};

    sc_vector<sc_out<bool>>       spm_resp_valid_o{"spm_resp_valid_o", NUM_NOC_PORTS};
    sc_vector<sc_in<bool>>        spm_resp_ready_i{"spm_resp_ready_i", NUM_NOC_PORTS};
    sc_vector<sc_out<spm_resp_t>> spm_resp_o      {"spm_resp_o",       NUM_NOC_PORTS};

    // =========================================================================
    // 2.4  AXI4-Lite slave interface (DMA)
    // =========================================================================
    sc_in<bool>                        s_axi_awvalid_i{"s_axi_awvalid_i"};
    sc_out<bool>                       s_axi_awready_o{"s_axi_awready_o"};
    sc_in<sc_uint<ADDR_WIDTH>>         s_axi_awaddr_i {"s_axi_awaddr_i"};

    sc_in<bool>                        s_axi_wvalid_i{"s_axi_wvalid_i"};
    sc_out<bool>                       s_axi_wready_o{"s_axi_wready_o"};
    sc_in<sc_biguint<BANK_DATA_WIDTH>> s_axi_wdata_i {"s_axi_wdata_i"};
    sc_in<sc_uint<BANK_DATA_WIDTH/8>>  s_axi_wstrb_i {"s_axi_wstrb_i"};

    sc_out<bool>                       s_axi_bvalid_o{"s_axi_bvalid_o"};
    sc_in<bool>                        s_axi_bready_i{"s_axi_bready_i"};
    sc_out<sc_uint<2>>                 s_axi_bresp_o {"s_axi_bresp_o"};

    sc_in<bool>                        s_axi_arvalid_i{"s_axi_arvalid_i"};
    sc_out<bool>                       s_axi_arready_o{"s_axi_arready_o"};
    sc_in<sc_uint<ADDR_WIDTH>>         s_axi_araddr_i {"s_axi_araddr_i"};

    sc_out<bool>                        s_axi_rvalid_o{"s_axi_rvalid_o"};
    sc_in<bool>                         s_axi_rready_i{"s_axi_rready_i"};
    sc_out<sc_biguint<BANK_DATA_WIDTH>> s_axi_rdata_o {"s_axi_rdata_o"};
    sc_out<sc_uint<2>>                  s_axi_rresp_o {"s_axi_rresp_o"};

    // =========================================================================
    // 2.5  PMU output ports
    // =========================================================================
    sc_out<sc_uint<64>>              pmu_cycle_cnt_o       {"pmu_cycle_cnt_o"};
    sc_vector<sc_out<sc_uint<64>>>   pmu_port_txn_cnt_o    {"pmu_port_txn_cnt_o", NUM_NOC_PORTS};
    sc_out<sc_uint<64>>              pmu_arb_stall_cnt_o   {"pmu_arb_stall_cnt_o"};
    sc_out<sc_uint<64>>              pmu_credit_stall_cnt_o{"pmu_credit_stall_cnt_o"};

    // =========================================================================
    // Sub-module: SRAM banks + their port wires
    // =========================================================================
    std::array<SRAM<BANK_DATA_WIDTH, ADDR_WIDTH>*, TOTAL_BANKS> banks{};

    sc_vector<sc_signal<sc_uint<ADDR_WIDTH>>>           bank_req_addr_sig   {"bank_req_addr_sig",   TOTAL_BANKS};
    sc_vector<sc_signal<bool>>                          bank_req_valid_sig  {"bank_req_valid_sig",  TOTAL_BANKS};
    sc_vector<sc_signal<bool>>                          bank_req_ready_sig  {"bank_req_ready_sig",  TOTAL_BANKS};
    sc_vector<sc_signal<sc_biguint<BANK_DATA_WIDTH>>>   bank_resp_data_sig  {"bank_resp_data_sig",  TOTAL_BANKS};
    sc_vector<sc_signal<bool>>                          bank_resp_valid_sig {"bank_resp_valid_sig", TOTAL_BANKS};
    sc_vector<sc_signal<bool>>                          bank_resp_ready_sig {"bank_resp_ready_sig", TOTAL_BANKS};
    sc_vector<sc_signal<bool>>                          bank_write_en_sig   {"bank_write_en_sig",   TOTAL_BANKS};
    sc_vector<sc_signal<sc_uint<ADDR_WIDTH>>>           bank_write_addr_sig {"bank_write_addr_sig", TOTAL_BANKS};
    sc_vector<sc_signal<sc_biguint<BANK_DATA_WIDTH>>>   bank_write_data_sig {"bank_write_data_sig", TOTAL_BANKS};
    sc_vector<sc_signal<sc_uint<BANK_DATA_WIDTH/8>>>    bank_write_mask_sig {"bank_write_mask_sig", TOTAL_BANKS};

    // =========================================================================
    // FIFO module instances  (replace std::deque)
    // Each FIFO has a full set of sc_signal wires for its port interface.
    // =========================================================================

    // -- DMA AW channel --
    hybridacc::FIFO<sc_uint<ADDR_WIDTH>>           dma_aw_fifo        {"dma_aw_fifo",        DMA_AW_FIFO_DEPTH};
    sc_signal<bool>                                dma_aw_fifo_empty  {"dma_aw_fifo_empty"};
    sc_signal<bool>                                dma_aw_fifo_full   {"dma_aw_fifo_full"};
    sc_signal<sc_uint<ADDR_WIDTH>>                 dma_aw_fifo_din    {"dma_aw_fifo_din"};
    sc_signal<sc_uint<ADDR_WIDTH>>                 dma_aw_fifo_dout   {"dma_aw_fifo_dout"};
    sc_signal<bool>                                dma_aw_fifo_push   {"dma_aw_fifo_push"};
    sc_signal<bool>                                dma_aw_fifo_pop    {"dma_aw_fifo_pop"};
    sc_signal<bool>                                dma_aw_fifo_clear  {"dma_aw_fifo_clear"};

    // -- DMA W data channel --
    hybridacc::FIFO<sc_biguint<BANK_DATA_WIDTH>>   dma_w_data_fifo       {"dma_w_data_fifo",       DMA_W_FIFO_DEPTH};
    sc_signal<bool>                                dma_w_data_fifo_empty {"dma_w_data_fifo_empty"};
    sc_signal<bool>                                dma_w_data_fifo_full  {"dma_w_data_fifo_full"};
    sc_signal<sc_biguint<BANK_DATA_WIDTH>>         dma_w_data_fifo_din   {"dma_w_data_fifo_din"};
    sc_signal<sc_biguint<BANK_DATA_WIDTH>>         dma_w_data_fifo_dout  {"dma_w_data_fifo_dout"};
    sc_signal<bool>                                dma_w_data_fifo_push  {"dma_w_data_fifo_push"};
    sc_signal<bool>                                dma_w_data_fifo_pop   {"dma_w_data_fifo_pop"};
    sc_signal<bool>                                dma_w_data_fifo_clear {"dma_w_data_fifo_clear"};

    // -- DMA W strobe channel --
    hybridacc::FIFO<sc_uint<BANK_DATA_WIDTH/8>>    dma_w_strb_fifo       {"dma_w_strb_fifo",       DMA_W_FIFO_DEPTH};
    sc_signal<bool>                                dma_w_strb_fifo_empty {"dma_w_strb_fifo_empty"};
    sc_signal<bool>                                dma_w_strb_fifo_full  {"dma_w_strb_fifo_full"};
    sc_signal<sc_uint<BANK_DATA_WIDTH/8>>          dma_w_strb_fifo_din   {"dma_w_strb_fifo_din"};
    sc_signal<sc_uint<BANK_DATA_WIDTH/8>>          dma_w_strb_fifo_dout  {"dma_w_strb_fifo_dout"};
    sc_signal<bool>                                dma_w_strb_fifo_push  {"dma_w_strb_fifo_push"};
    sc_signal<bool>                                dma_w_strb_fifo_pop   {"dma_w_strb_fifo_pop"};
    sc_signal<bool>                                dma_w_strb_fifo_clear {"dma_w_strb_fifo_clear"};

    // -- DMA merged write-request (AW+W combined) --
    hybridacc::FIFO<DmaWriteReq>                   dma_write_req_fifo       {"dma_write_req_fifo",       DMA_WRITE_REQ_FIFO_DEPTH};
    sc_signal<bool>                                dma_write_req_fifo_empty {"dma_write_req_fifo_empty"};
    sc_signal<bool>                                dma_write_req_fifo_full  {"dma_write_req_fifo_full"};
    sc_signal<DmaWriteReq>                         dma_write_req_fifo_din   {"dma_write_req_fifo_din"};
    sc_signal<DmaWriteReq>                         dma_write_req_fifo_dout  {"dma_write_req_fifo_dout"};
    sc_signal<bool>                                dma_write_req_fifo_push  {"dma_write_req_fifo_push"};
    sc_signal<bool>                                dma_write_req_fifo_pop   {"dma_write_req_fifo_pop"};
    sc_signal<bool>                                dma_write_req_fifo_clear {"dma_write_req_fifo_clear"};

    // -- DMA AR (read-request) channel --
    hybridacc::FIFO<sc_uint<ADDR_WIDTH>>           dma_read_req_fifo       {"dma_read_req_fifo",       DMA_READ_REQ_FIFO_DEPTH};
    sc_signal<bool>                                dma_read_req_fifo_empty {"dma_read_req_fifo_empty"};
    sc_signal<bool>                                dma_read_req_fifo_full  {"dma_read_req_fifo_full"};
    sc_signal<sc_uint<ADDR_WIDTH>>                 dma_read_req_fifo_din   {"dma_read_req_fifo_din"};
    sc_signal<sc_uint<ADDR_WIDTH>>                 dma_read_req_fifo_dout  {"dma_read_req_fifo_dout"};
    sc_signal<bool>                                dma_read_req_fifo_push  {"dma_read_req_fifo_push"};
    sc_signal<bool>                                dma_read_req_fifo_pop   {"dma_read_req_fifo_pop"};
    sc_signal<bool>                                dma_read_req_fifo_clear {"dma_read_req_fifo_clear"};

    // -- DMA R (read-response) channel --
    hybridacc::FIFO<sc_biguint<BANK_DATA_WIDTH>>   dma_read_resp_fifo       {"dma_read_resp_fifo",       DMA_READ_RESP_FIFO_DEPTH};
    sc_signal<bool>                                dma_read_resp_fifo_empty {"dma_read_resp_fifo_empty"};
    sc_signal<bool>                                dma_read_resp_fifo_full  {"dma_read_resp_fifo_full"};
    sc_signal<sc_biguint<BANK_DATA_WIDTH>>         dma_read_resp_fifo_din   {"dma_read_resp_fifo_din"};
    sc_signal<sc_biguint<BANK_DATA_WIDTH>>         dma_read_resp_fifo_dout  {"dma_read_resp_fifo_dout"};
    sc_signal<bool>                                dma_read_resp_fifo_push  {"dma_read_resp_fifo_push"};
    sc_signal<bool>                                dma_read_resp_fifo_pop   {"dma_read_resp_fifo_pop"};
    sc_signal<bool>                                dma_read_resp_fifo_clear {"dma_read_resp_fifo_clear"};

    // -- Per-group read-metadata FIFOs (track in-flight reads per SRAM group) --
    sc_vector<hybridacc::FIFO<ReadMeta>>           group_meta_fifo      {"group_meta_fifo"};
    sc_vector<sc_signal<bool>>                     group_meta_fifo_empty{"group_meta_fifo_empty", NUM_GROUPS};
    sc_vector<sc_signal<bool>>                     group_meta_fifo_full {"group_meta_fifo_full",  NUM_GROUPS};
    sc_vector<sc_signal<ReadMeta>>                 group_meta_fifo_din  {"group_meta_fifo_din",   NUM_GROUPS};
    sc_vector<sc_signal<ReadMeta>>                 group_meta_fifo_dout {"group_meta_fifo_dout",  NUM_GROUPS};
    sc_vector<sc_signal<bool>>                     group_meta_fifo_push {"group_meta_fifo_push",  NUM_GROUPS};
    sc_vector<sc_signal<bool>>                     group_meta_fifo_pop  {"group_meta_fifo_pop",   NUM_GROUPS};
    sc_vector<sc_signal<bool>>                     group_meta_fifo_clear{"group_meta_fifo_clear", NUM_GROUPS};

    // -- Per-port response FIFOs (queue responses before the output register) --
    sc_vector<hybridacc::FIFO<spm_resp_t>>         port_resp_fifo      {"port_resp_fifo"};
    sc_vector<sc_signal<bool>>                     port_resp_fifo_empty{"port_resp_fifo_empty", NUM_NOC_PORTS};
    sc_vector<sc_signal<bool>>                     port_resp_fifo_full {"port_resp_fifo_full",  NUM_NOC_PORTS};
    sc_vector<sc_signal<spm_resp_t>>               port_resp_fifo_din  {"port_resp_fifo_din",   NUM_NOC_PORTS};
    sc_vector<sc_signal<spm_resp_t>>               port_resp_fifo_dout {"port_resp_fifo_dout",  NUM_NOC_PORTS};
    sc_vector<sc_signal<bool>>                     port_resp_fifo_push {"port_resp_fifo_push",  NUM_NOC_PORTS};
    sc_vector<sc_signal<bool>>                     port_resp_fifo_pop  {"port_resp_fifo_pop",   NUM_NOC_PORTS};
    sc_vector<sc_signal<bool>>                     port_resp_fifo_clear{"port_resp_fifo_clear", NUM_NOC_PORTS};

    // =========================================================================
    // State registers  (_reg suffix → updated ONLY in seq_process)
    // =========================================================================

    // S0: per-port skid buffers
    sc_vector<sc_signal<bool>>       skid_valid_reg{"skid_valid_reg", NUM_NOC_PORTS};
    sc_vector<sc_signal<spm_req_t>>  skid_data_reg {"skid_data_reg",  NUM_NOC_PORTS};

    // In-flight read credit (one slot per outstanding read request)
    sc_vector<sc_signal<sc_uint<8>>> credit_cnt_reg{"credit_cnt_reg", NUM_NOC_PORTS};

    // Port-to-group mapping register
    sc_vector<sc_signal<sc_uint<2>>> active_map_reg{"active_map_reg", NUM_NOC_PORTS};

    // DMA state counters
    sc_signal<sc_uint<8>> dma_b_pending_cnt_reg  {"dma_b_pending_cnt_reg"};
    sc_signal<sc_uint<8>> dma_rd_inflight_cnt_reg{"dma_rd_inflight_cnt_reg"};

    // PMU counters
    sc_signal<sc_uint<64>>              pmu_cycle_cnt_reg        {"pmu_cycle_cnt_reg"};
    sc_vector<sc_signal<sc_uint<64>>>   pmu_port_txn_cnt_reg     {"pmu_port_txn_cnt_reg", NUM_NOC_PORTS};
    sc_signal<sc_uint<64>>              pmu_arb_stall_cnt_reg    {"pmu_arb_stall_cnt_reg"};
    sc_signal<sc_uint<64>>              pmu_credit_stall_cnt_reg {"pmu_credit_stall_cnt_reg"};

    // =========================================================================
    // Intermediate combinational signals
    // (wires between comb_* processes and seq_process)
    // =========================================================================

    // Per-port: whether this port's request was consumed by the bank arb this cycle
    sc_vector<sc_signal<bool>> port_req_fire_sig    {"port_req_fire_sig",    NUM_NOC_PORTS};
    // Per-port: whether the fired request is a write (used for credit / response routing)
    sc_vector<sc_signal<bool>> port_req_is_write_sig{"port_req_is_write_sig", NUM_NOC_PORTS};

    // Per-port: immediate write-response push (from bank-arb, bypasses read pipeline)
    sc_vector<sc_signal<bool>>       wr_resp_push_sig{"wr_resp_push_sig", NUM_NOC_PORTS};
    sc_vector<sc_signal<spm_resp_t>> wr_resp_data_sig{"wr_resp_data_sig", NUM_NOC_PORTS};

    // Per-port: read-response push (from response-merge stage, comes from SRAM pipeline)
    sc_vector<sc_signal<bool>>       rd_resp_push_sig{"rd_resp_push_sig", NUM_NOC_PORTS};
    sc_vector<sc_signal<spm_resp_t>> rd_resp_data_sig{"rd_resp_data_sig", NUM_NOC_PORTS};

    // DMA fire signals
    sc_signal<bool>                         dma_write_fire_sig     {"dma_write_fire_sig"};
    sc_signal<bool>                         dma_read_fire_sig      {"dma_read_fire_sig"};
    sc_signal<bool>                         dma_read_merge_fire_sig{"dma_read_merge_fire_sig"};
    sc_signal<sc_biguint<BANK_DATA_WIDTH>>  dma_read_merge_data_sig{"dma_read_merge_data_sig"};

    // PMU stall wires
    sc_signal<bool> credit_stall_sig{"credit_stall_sig"};
    sc_signal<bool> arb_stall_sig   {"arb_stall_sig"};

    // Trace context
    uint32_t    trace_pid  = 0;
    int         trace_id   = -1;
    bool        trace_init = false;
    std::string last_state_spm = "IDLE";
    std::array<std::string, NUM_NOC_PORTS> last_state_noc_req{};
    std::array<std::string, NUM_NOC_PORTS> last_state_noc_resp{};
    std::string last_state_group_meta = "IDLE";
    std::string last_state_dma = "IDLE";
    static constexpr uint64_t DBG_REPORT_PERIOD = 256;

    SC_HAS_PROCESS(ScratchpadMemory);

    // =========================================================================
    // Unified status helpers
    // =========================================================================
    /** All internal FIFOs empty, no pending DMA, no skid buffered requests. */
    bool spm_quiesced() const {
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            if (skid_valid_reg[p].read()) return false;
            if (!port_resp_fifo_empty[p].read()) return false;
        }
        for (unsigned g = 0; g < NUM_GROUPS; ++g) {
            if (!group_meta_fifo_empty[g].read()) return false;
        }
        if (!dma_aw_fifo_empty.read())        return false;
        if (!dma_w_data_fifo_empty.read())    return false;
        if (!dma_w_strb_fifo_empty.read())    return false;
        if (!dma_write_req_fifo_empty.read()) return false;
        if (!dma_read_req_fifo_empty.read())  return false;
        if (!dma_read_resp_fifo_empty.read()) return false;
        if (dma_b_pending_cnt_reg.read() != 0) return false;
        if (dma_rd_inflight_cnt_reg.read() != 0) return false;
        return true;
    }

    // =========================================================================
    // Constructor
    // =========================================================================
    ScratchpadMemory(sc_module_name name) : sc_module(name) {
        // --- Instantiate SRAM banks ---
        for (unsigned i = 0; i < TOTAL_BANKS; ++i) {
            std::string s = std::string(name) + "_bank_" + std::to_string(i);
            size_t bytes  = static_cast<size_t>(BANK_DEPTH) * (BANK_DATA_WIDTH / 8);
            banks[i] = new SRAM<BANK_DATA_WIDTH, ADDR_WIDTH>(
                s.c_str(), bytes, SRAM_BANK_LATENCY, SRAM_BANK_PIPELINE_DEPTH);
            banks[i]->clk       (clk);
            banks[i]->reset_n   (reset_n);
            banks[i]->req_addr  (bank_req_addr_sig[i]);
            banks[i]->req_valid (bank_req_valid_sig[i]);
            banks[i]->req_ready (bank_req_ready_sig[i]);
            banks[i]->resp_data (bank_resp_data_sig[i]);
            banks[i]->resp_valid(bank_resp_valid_sig[i]);
            banks[i]->resp_ready(bank_resp_ready_sig[i]);
            banks[i]->write_en  (bank_write_en_sig[i]);
            banks[i]->write_addr(bank_write_addr_sig[i]);
            banks[i]->write_data(bank_write_data_sig[i]);
            banks[i]->write_mask(bank_write_mask_sig[i]);
        }

        // --- Wire scalar DMA FIFOs ---
        bind_fifo(dma_aw_fifo,
                  dma_aw_fifo_empty, dma_aw_fifo_full,
                  dma_aw_fifo_din,   dma_aw_fifo_dout,
                  dma_aw_fifo_push,  dma_aw_fifo_pop, dma_aw_fifo_clear);
        bind_fifo(dma_w_data_fifo,
                  dma_w_data_fifo_empty, dma_w_data_fifo_full,
                  dma_w_data_fifo_din,   dma_w_data_fifo_dout,
                  dma_w_data_fifo_push,  dma_w_data_fifo_pop, dma_w_data_fifo_clear);
        bind_fifo(dma_w_strb_fifo,
                  dma_w_strb_fifo_empty, dma_w_strb_fifo_full,
                  dma_w_strb_fifo_din,   dma_w_strb_fifo_dout,
                  dma_w_strb_fifo_push,  dma_w_strb_fifo_pop, dma_w_strb_fifo_clear);
        bind_fifo(dma_write_req_fifo,
                  dma_write_req_fifo_empty, dma_write_req_fifo_full,
                  dma_write_req_fifo_din,   dma_write_req_fifo_dout,
                  dma_write_req_fifo_push,  dma_write_req_fifo_pop, dma_write_req_fifo_clear);
        bind_fifo(dma_read_req_fifo,
                  dma_read_req_fifo_empty, dma_read_req_fifo_full,
                  dma_read_req_fifo_din,   dma_read_req_fifo_dout,
                  dma_read_req_fifo_push,  dma_read_req_fifo_pop, dma_read_req_fifo_clear);
        bind_fifo(dma_read_resp_fifo,
                  dma_read_resp_fifo_empty, dma_read_resp_fifo_full,
                  dma_read_resp_fifo_din,   dma_read_resp_fifo_dout,
                  dma_read_resp_fifo_push,  dma_read_resp_fifo_pop, dma_read_resp_fifo_clear);

        // --- Instantiate & wire per-group metadata FIFOs ---
        group_meta_fifo.init(NUM_GROUPS,[](const char* n, size_t) {
            return new hybridacc::FIFO<ReadMeta>(n, GROUP_META_FIFO_DEPTH);
        });
        for (unsigned g = 0; g < NUM_GROUPS; ++g) {
            bind_fifo(group_meta_fifo[g],
                      group_meta_fifo_empty[g], group_meta_fifo_full[g],
                      group_meta_fifo_din[g], group_meta_fifo_dout[g],
                      group_meta_fifo_push[g], group_meta_fifo_pop[g],
                      group_meta_fifo_clear[g]);
        }

        // --- Instantiate & wire per-port response FIFOs ---
        port_resp_fifo.init(NUM_NOC_PORTS,[](const char* n, size_t) {
            return new hybridacc::FIFO<spm_resp_t>(n, PORT_RESP_FIFO_DEPTH);
        });
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            bind_fifo(port_resp_fifo[p],
                      port_resp_fifo_empty[p], port_resp_fifo_full[p],
                      port_resp_fifo_din[p], port_resp_fifo_dout[p],
                      port_resp_fifo_push[p], port_resp_fifo_pop[p],
                      port_resp_fifo_clear[p]);
        }

        // Initial config map: port i -> group i
        for (unsigned i = 0; i < NUM_NOC_PORTS; ++i)
            active_map_reg[i].write(sc_uint<2>(i % NUM_GROUPS));

        // =================================================================
        // Process registrations
        // =================================================================

        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // ---- comb_spm_req_ready: spm_req_ready_o ----
        SC_METHOD(comb_spm_req_ready);
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) sensitive << skid_valid_reg[p];

        // ---- comb_dma_chan_ready: AXI AW/W/AR ready ----
        SC_METHOD(comb_dma_chan_ready);
        sensitive << dma_aw_fifo_full << dma_w_data_fifo_full
                  << dma_read_req_fifo_full << dma_read_resp_fifo_full
                  << dma_rd_inflight_cnt_reg;

        // ---- comb_dma_chan_push: AXI AW/W/AR push + payload wiring ----
        SC_METHOD(comb_dma_chan_push);
        sensitive << s_axi_awvalid_i << s_axi_awaddr_i << dma_aw_fifo_full
              << s_axi_wvalid_i  << s_axi_wdata_i  << s_axi_wstrb_i
              << dma_w_data_fifo_full << dma_w_strb_fifo_full
              << s_axi_arvalid_i << s_axi_araddr_i
              << dma_read_req_fifo_full << dma_read_resp_fifo_full
              << dma_rd_inflight_cnt_reg;

        // ---- comb_dma_aw_w_merge: pop AW/W FIFOs, push write-req FIFO ----
        SC_METHOD(comb_dma_aw_w_merge);
        sensitive << dma_aw_fifo_empty   << dma_aw_fifo_dout
                  << dma_w_data_fifo_empty << dma_w_data_fifo_dout
                  << dma_w_strb_fifo_dout  << dma_write_req_fifo_full;

        // ---- comb_bank_req_arb: priority arbitration, bank drives, fire signals ----
        SC_METHOD(comb_bank_req_arb);
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            sensitive << skid_valid_reg[p]  << skid_data_reg[p]
                      << credit_cnt_reg[p]  << active_map_reg[p]
                      << spm_req_valid_i[p] << spm_req_i[p];
        }
        for (unsigned g = 0; g < NUM_GROUPS; ++g) sensitive << group_meta_fifo_full[g];
        sensitive << dma_write_req_fifo_empty << dma_write_req_fifo_dout
                  << dma_read_req_fifo_empty  << dma_read_req_fifo_dout
                  << dma_rd_inflight_cnt_reg  << dma_b_pending_cnt_reg
                  << dma_read_resp_fifo_full;
        // Back pressure: bank pipeline full → req_ready goes low
        for (unsigned b = 0; b < TOTAL_BANKS; ++b) sensitive << bank_req_ready_sig[b];

        // ---- comb_resp_merge: bank responses -> rd_resp / dma_read_merge ----
        SC_METHOD(comb_resp_merge);
        for (unsigned g = 0; g < NUM_GROUPS; ++g) {
            sensitive << group_meta_fifo_empty[g] << group_meta_fifo_dout[g];
            for (unsigned k = 0; k < BANKS_PER_GROUP; ++k) {
                sensitive << bank_resp_valid_sig[g * BANKS_PER_GROUP + k]
                          << bank_resp_data_sig[g * BANKS_PER_GROUP + k];
            }
        }
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) sensitive << port_resp_fifo_full[p];
        sensitive << dma_read_resp_fifo_full;

        // ---- comb_port_resp_fifo_ctrl: merge wr/rd pushes into port_resp_fifo ----
        SC_METHOD(comb_port_resp_fifo_ctrl);
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            sensitive << wr_resp_push_sig[p] << wr_resp_data_sig[p]
                      << rd_resp_push_sig[p] << rd_resp_data_sig[p];
        }

        // ---- comb_spm_resp_output: spm_resp_valid_o / spm_resp_o ----
        SC_METHOD(comb_spm_resp_output);
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            sensitive << port_resp_fifo_empty[p] << port_resp_fifo_dout[p];
        }

        // ---- comb_port_resp_fifo_pop: pop on valid/ready handshake ----
        SC_METHOD(comb_port_resp_fifo_pop);
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            sensitive << port_resp_fifo_empty[p] << spm_resp_ready_i[p];
        }

        // ---- comb_dma_resp_b: s_axi_bvalid_o / s_axi_bresp_o ----
        SC_METHOD(comb_dma_resp_b);
        sensitive << dma_b_pending_cnt_reg;

        // ---- comb_dma_resp_r: s_axi_rvalid_o / s_axi_rdata_o / s_axi_rresp_o ----
        SC_METHOD(comb_dma_resp_r);
        sensitive << dma_read_resp_fifo_empty << dma_read_resp_fifo_dout;

        // ---- comb_dma_read_resp_fifo_push: merge result -> DMA R FIFO ----
        SC_METHOD(comb_dma_read_resp_fifo_push);
        sensitive << dma_read_merge_fire_sig << dma_read_merge_data_sig;

        // ---- comb_dma_read_resp_fifo_pop: R handshake ----
        SC_METHOD(comb_dma_read_resp_fifo_pop);
        sensitive << dma_read_resp_fifo_empty << s_axi_rready_i;

        sensitive << drop_noc_resp_i;

        // ---- comb_bank_resp_ready: accept only when merge will consume ----
        SC_METHOD(comb_bank_resp_ready);
        for (unsigned g = 0; g < NUM_GROUPS; ++g)
            sensitive << group_meta_fifo_pop[g]
                      << group_meta_fifo_empty[g]
                      << group_meta_fifo_dout[g];

        // ---- pmu_output_process: PMU register -> output ports ----
        SC_METHOD(pmu_output_process);
        sensitive << pmu_cycle_cnt_reg << pmu_arb_stall_cnt_reg << pmu_credit_stall_cnt_reg;
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) sensitive << pmu_port_txn_cnt_reg[p];

        // ---- trace_process ----
        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    ~ScratchpadMemory() { for (auto b : banks) delete b; }

    // =========================================================================
    // Trace helpers
    // =========================================================================
    void set_trace_context(uint32_t pid, int tid_base) {
        trace_pid  = pid;
        trace_id   = tid_base;
        trace_init = false;
        last_state_spm = last_state_dma = last_state_group_meta = "IDLE";
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            last_state_noc_req[p]  = "IDLE";
            last_state_noc_resp[p] = "IDLE";
        }

        const int bank_tid_base = trace_id + get_trace_num() - static_cast<int>(TOTAL_BANKS)+1;
        for (unsigned b = 0; b < TOTAL_BANKS; ++b)
            banks[b]->set_trace_context(trace_pid, bank_tid_base + static_cast<int>(b));
    }

    int get_trace_num() const { return static_cast<int>(3 + 2 * NUM_NOC_PORTS + TOTAL_BANKS); }

    std::pair<uint32_t, uint32_t> enable_perffeto_trace(uint32_t start_pid, uint32_t start_tid) {
        set_trace_context(start_pid, static_cast<int>(start_tid));
        return {start_pid + 1, start_tid + static_cast<uint32_t>(get_trace_num() + 1)};
    }

private:
    // =========================================================================
    // Helper: bind a FIFO module to its signal wires
    // =========================================================================
    template <typename T>
    void bind_fifo(hybridacc::FIFO<T>& f,
                   sc_signal<bool>& empty_sig, sc_signal<bool>& full_sig,
                   sc_signal<T>& din_sig,      sc_signal<T>& dout_sig,
                   sc_signal<bool>& push_sig,  sc_signal<bool>& pop_sig,
                   sc_signal<bool>& clear_sig)
    {
        f.clk     (clk);
        f.reset_n (reset_n);
        f.empty   (empty_sig);
        f.full    (full_sig);
        f.data_in (din_sig);
        f.data_out(dout_sig);
        f.push    (push_sig);
        f.pop     (pop_sig);
        f.clear   (clear_sig);
    }

    // =========================================================================
    // comb_spm_req_ready
    // spm_req_ready_o[p] = !skid_valid_reg[p]
    // =========================================================================
    void comb_spm_req_ready() {
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p)
            spm_req_ready_o[p].write(!skid_valid_reg[p].read());
    }

    // =========================================================================
    // comb_dma_chan_ready
    // AW/W/AR ready signals from FIFO fullness and in-flight count.
    // =========================================================================
    void comb_dma_chan_ready() {
        s_axi_awready_o.write(!dma_aw_fifo_full.read());
        s_axi_wready_o.write(!dma_w_data_fifo_full.read());

        const unsigned inflight = dma_rd_inflight_cnt_reg.read().to_uint();
        const bool ar_ok = !dma_read_req_fifo_full.read() &&
                           !dma_read_resp_fifo_full.read() &&
                           (inflight < DMA_MAX_OUTSTANDING);
        s_axi_arready_o.write(ar_ok);
    }

    // =========================================================================
    // comb_dma_chan_push
    // Drive DMA AW/W/AR FIFO push and payload wires combinationally from
    // AXI valid + FIFO status (handshake-equivalent conditions).
    // =========================================================================
    void comb_dma_chan_push() {
        dma_aw_fifo_push.write(s_axi_awvalid_i.read() && !dma_aw_fifo_full.read());
        dma_aw_fifo_din.write(s_axi_awaddr_i.read());

        dma_w_data_fifo_push.write(s_axi_wvalid_i.read() && !dma_w_data_fifo_full.read());
        dma_w_data_fifo_din.write(s_axi_wdata_i.read());

        dma_w_strb_fifo_push.write(s_axi_wvalid_i.read() && !dma_w_strb_fifo_full.read());
        dma_w_strb_fifo_din.write(s_axi_wstrb_i.read());

        const bool ar_ok = !dma_read_req_fifo_full.read() &&
                           !dma_read_resp_fifo_full.read() &&
                           (dma_rd_inflight_cnt_reg.read().to_uint() < DMA_MAX_OUTSTANDING);
        dma_read_req_fifo_push.write(s_axi_arvalid_i.read() && ar_ok);
        dma_read_req_fifo_din.write(s_axi_araddr_i.read());
    }

    // =========================================================================
    // comb_dma_aw_w_merge
    // When AW FIFO and W FIFO both have data and write-req FIFO has space,
    // pop one entry from each and push a merged DmaWriteReq.
    // =========================================================================
    void comb_dma_aw_w_merge() {
        const bool can = !dma_aw_fifo_empty.read() &&
                         !dma_w_data_fifo_empty.read() &&
                         !dma_w_strb_fifo_empty.read() &&
                         !dma_write_req_fifo_full.read();

        dma_aw_fifo_pop.write(can);
        dma_w_data_fifo_pop.write(can);
        dma_w_strb_fifo_pop.write(can);
        dma_write_req_fifo_push.write(can);

        DmaWriteReq merged;
        if (can) {
            merged.addr = dma_aw_fifo_dout.read();
            merged.data = dma_w_data_fifo_dout.read();
            merged.strb = dma_w_strb_fifo_dout.read();
        }
        dma_write_req_fifo_din.write(merged);
    }

    // =========================================================================
    // comb_bank_req_arb
    //
    // Priority arbitration across NoC ports (0 = highest) and DMA.  For each
    // group at most one request is issued per cycle (group_busy[]).
    //
    // Drives:
    //   bank_req_valid_sig[], bank_req_addr_sig[]
    //   bank_write_en_sig[], bank_write_addr_sig[], bank_write_data_sig[],
    //   bank_write_mask_sig[]
    //   group_meta_fifo_push[], group_meta_fifo_din[]
    //   port_req_fire_sig[], port_req_is_write_sig[]
    //   wr_resp_push_sig[], wr_resp_data_sig[]
    //   dma_write_fire_sig, dma_read_fire_sig
    //   dma_write_req_fifo_pop, dma_read_req_fifo_pop
    //   credit_stall_sig, arb_stall_sig
    // =========================================================================
    void comb_bank_req_arb() {
        // --- Defaults ---
        for (unsigned b = 0; b < TOTAL_BANKS; ++b) {
            bank_req_valid_sig[b].write(false);
            bank_req_addr_sig[b].write(0);
            bank_write_en_sig[b].write(false);
            bank_write_addr_sig[b].write(0);
            bank_write_data_sig[b].write(0);
            bank_write_mask_sig[b].write(0);
        }
        for (unsigned g = 0; g < NUM_GROUPS; ++g) {
            group_meta_fifo_push[g].write(false);
            group_meta_fifo_din[g].write(ReadMeta{});
        }
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            port_req_fire_sig[p].write(false);
            port_req_is_write_sig[p].write(false);
            wr_resp_push_sig[p].write(false);
            wr_resp_data_sig[p].write(spm_resp_t{});
        }
        dma_write_fire_sig.write(false);
        dma_read_fire_sig.write(false);
        dma_write_req_fifo_pop.write(false);
        dma_read_req_fifo_pop.write(false);
        credit_stall_sig.write(false);
        arb_stall_sig.write(false);

        std::array<bool, NUM_GROUPS> group_busy{};
        group_busy.fill(false);
        bool local_credit_stall = false;
        bool local_arb_stall    = false;

        // --- NoC ports (static priority 0 = highest) ---
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            spm_req_t req;
            bool valid = false;

            if (skid_valid_reg[p].read()) {
                req   = skid_data_reg[p].read();
                valid = true;
            } else if (spm_req_valid_i[p].read()) {
                req   = spm_req_i[p].read();
                valid = true;
            }

            if (!valid) continue;

            // Credit check (read-only)
            if (!req.wen && credit_cnt_reg[p].read() == 0) {
                local_credit_stall = true;
                continue;
            }

            // Group-busy / meta-FIFO-full check
            unsigned g = active_map_reg[p].read().to_uint();
            if (g >= NUM_GROUPS || group_busy[g] ||
                (!req.wen && group_meta_fifo_full[g].read())) {
                DEBUG_MSG(" stall p=" << p << " g=" << g << " busy=" << group_busy[g] << " full=" << group_meta_fifo_full[g].read(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                local_arb_stall = true;
                continue;
            }

            DEBUG_MSG(" issue p=" << p << " g=" << g << " laddr=" << req.addr.to_uint()  << " data=" << std::hex << req.wdata.to_uint64() << std::dec << " wen=" << req.wen << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
            // Issue to bank pipeline
            uint32_t laddr      = req.addr.to_uint();
            bool     is_par     = (laddr >= GROUP_LINEAR_WORDS);
            unsigned base_bank  = g * BANKS_PER_GROUP;

            if (is_par) {
                uint32_t row = laddr - GROUP_LINEAR_WORDS;
                if (req.wen) {
                    for (unsigned k = 0; k < BANKS_PER_GROUP; ++k) {
                        bank_write_en_sig[base_bank+k].write(true);
                        bank_write_addr_sig[base_bank+k].write(
                            sc_uint<ADDR_WIDTH>(row * BYTES_PER_BANK_WORD));
                        sc_biguint<NOC_DATA_WIDTH> full_w = req.wdata;
                        bank_write_data_sig[base_bank+k].write(
                            sc_biguint<BANK_DATA_WIDTH>(
                                full_w.range((k+1)*BANK_DATA_WIDTH-1, k*BANK_DATA_WIDTH)));
                        bank_write_mask_sig[base_bank+k].write(
                            sc_uint<BANK_DATA_WIDTH/8>(BANK_BYTE_MASK));
                    }
                } else {
                    // Back pressure: all banks in group must be ready
                    bool all_banks_ready = true;
                    for (unsigned k = 0; k < BANKS_PER_GROUP; ++k)
                        if (!bank_req_ready_sig[base_bank+k].read()) { all_banks_ready = false; break; }
                    if (!all_banks_ready) {
                        local_arb_stall = true;
                        continue;
                    }
                    for (unsigned k = 0; k < BANKS_PER_GROUP; ++k) {
                        bank_req_valid_sig[base_bank+k].write(true);
                        bank_req_addr_sig[base_bank+k].write(
                            sc_uint<ADDR_WIDTH>(row * BYTES_PER_BANK_WORD));
                    }
                    ReadMeta meta;
                    meta.is_dma  = false;
                    meta.port_id = static_cast<uint8_t>(p);
                    meta.mode    = ReqMode::PARALLEL;
                    meta.addr    = laddr;
                    group_meta_fifo_push[g].write(true);
                    group_meta_fifo_din[g].write(meta);
                    DEBUG_MSG(" META_PUSH PAR p=" << p << " g=" << g << " laddr=" << laddr << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                }
            } else {
                unsigned bidx = (laddr / BANK_DEPTH) + base_bank;
                uint32_t row  = laddr % BANK_DEPTH;
                if (req.wen) {
                    bank_write_en_sig[bidx].write(true);
                    bank_write_addr_sig[bidx].write(
                        sc_uint<ADDR_WIDTH>(row * BYTES_PER_BANK_WORD));
                    bank_write_data_sig[bidx].write(
                        sc_biguint<BANK_DATA_WIDTH>(req.wdata.range(BANK_DATA_WIDTH-1, 0)));
                    bank_write_mask_sig[bidx].write(
                        sc_uint<BANK_DATA_WIDTH/8>(BANK_BYTE_MASK));
                } else {
                    // Back pressure: bank pipeline must be ready
                    if (!bank_req_ready_sig[bidx].read()) {
                        local_arb_stall = true;
                        continue;
                    }
                    bank_req_valid_sig[bidx].write(true);
                    bank_req_addr_sig[bidx].write(
                        sc_uint<ADDR_WIDTH>(row * BYTES_PER_BANK_WORD));
                    ReadMeta meta;
                    meta.is_dma  = false;
                    meta.port_id = static_cast<uint8_t>(p);
                    meta.mode    = ReqMode::LINEAR;
                    meta.addr    = laddr;
                    group_meta_fifo_push[g].write(true);
                    group_meta_fifo_din[g].write(meta);
                    DEBUG_MSG(" META_PUSH LIN p=" << p << " g=" << g << " laddr=" << laddr << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                }
            }

            // Immediate write response
            if (req.wen) {
                spm_resp_t wr;
                wr.code  = SPM_RESPONSE_CODE::SPM_OK;
                wr.rdata = 0;
                wr_resp_push_sig[p].write(true);
                wr_resp_data_sig[p].write(wr);
            }

            group_busy[g] = true;
            port_req_fire_sig[p].write(true);
            port_req_is_write_sig[p].write(req.wen ? true : false);
        }

        // --- DMA write (priority after all NoC ports) ---
        if (!dma_write_req_fifo_empty.read() &&
            dma_b_pending_cnt_reg.read().to_uint() < DMA_MAX_OUTSTANDING) {

            const DmaWriteReq wr = dma_write_req_fifo_dout.read();
            uint32_t  gwaddr     = wr.addr.to_uint() / BYTES_PER_BANK_WORD;
            unsigned  grp        = gwaddr / GROUP_SPAN_WORDS;

            if (grp < NUM_GROUPS && !group_busy[grp]) {
                unsigned lidx = gwaddr % GROUP_SPAN_WORDS;
                unsigned bidx = (lidx / BANK_DEPTH) + grp * BANKS_PER_GROUP;
                uint32_t row  = lidx % BANK_DEPTH;

                bank_write_en_sig[bidx].write(true);
                bank_write_addr_sig[bidx].write(sc_uint<ADDR_WIDTH>(row * BYTES_PER_BANK_WORD));
                bank_write_data_sig[bidx].write(wr.data);
                bank_write_mask_sig[bidx].write(wr.strb);

                dma_write_req_fifo_pop.write(true);
                dma_write_fire_sig.write(true);
                group_busy[grp] = true;
            }
        }

        // --- DMA read (lowest priority) ---
        if (!dma_read_req_fifo_empty.read()) {
            DEBUG_MSG(" DMA_RD_ARB: fifo_not_empty addr=0x" << std::hex << dma_read_req_fifo_dout.read().to_uint() << std::dec
                      << " resp_full=" << dma_read_resp_fifo_full.read()
                      << " inflight=" << dma_rd_inflight_cnt_reg.read().to_uint()
                      << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
        }
        if (!dma_read_req_fifo_empty.read() &&
            !dma_read_resp_fifo_full.read() &&
            dma_rd_inflight_cnt_reg.read().to_uint() < DMA_MAX_OUTSTANDING) {

            uint32_t  gwaddr = dma_read_req_fifo_dout.read().to_uint() / BYTES_PER_BANK_WORD;
            unsigned  grp    = gwaddr / GROUP_SPAN_WORDS;

            DEBUG_MSG(" DMA_RD_ARB: gwaddr=" << gwaddr << " grp=" << grp
                      << " group_busy=" << group_busy[grp]
                      << " meta_full=" << (grp < NUM_GROUPS ? group_meta_fifo_full[grp].read() : true)
                      << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);

            if (grp < NUM_GROUPS && !group_busy[grp] &&
                !group_meta_fifo_full[grp].read()) {

                unsigned lidx = gwaddr % GROUP_SPAN_WORDS;
                unsigned bidx = (lidx / BANK_DEPTH) + grp * BANKS_PER_GROUP;
                uint32_t row  = lidx % BANK_DEPTH;

                DEBUG_MSG(" DMA_RD_ARB: lidx=" << lidx << " bidx=" << bidx << " row=" << row
                          << " bank_ready=" << bank_req_ready_sig[bidx].read()
                          << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);

                // Back pressure: bank pipeline must be ready
                if (bank_req_ready_sig[bidx].read()) {
                    bank_req_valid_sig[bidx].write(true);
                    bank_req_addr_sig[bidx].write(sc_uint<ADDR_WIDTH>(row * BYTES_PER_BANK_WORD));

                    ReadMeta meta;
                    meta.is_dma  = true;
                    meta.port_id = 0;
                    meta.mode    = ReqMode::LINEAR;
                    meta.addr    = lidx;
                    group_meta_fifo_push[grp].write(true);
                    group_meta_fifo_din[grp].write(meta);
                    DEBUG_MSG(" META_PUSH DMA grp=" << grp << " addr=" << lidx << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);

                    dma_read_req_fifo_pop.write(true);
                    dma_read_fire_sig.write(true);
                    group_busy[grp] = true;
                }
            }
        }

        credit_stall_sig.write(local_credit_stall);
        arb_stall_sig.write(local_arb_stall);
    }

    // =========================================================================
    // comb_resp_merge
    //
    // For each SRAM group: when the head of group_meta_fifo is valid and all
    // required bank response valids are asserted, compose and push the response
    // to either the NoC port_resp_fifo or the DMA read-resp path.
    //
    // Drives:
    //   rd_resp_push_sig[], rd_resp_data_sig[]
    //   dma_read_merge_fire_sig, dma_read_merge_data_sig
    //   group_meta_fifo_pop[]
    // =========================================================================
    void comb_resp_merge() {

        // Defaults
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            rd_resp_push_sig[p].write(false);
            rd_resp_data_sig[p].write(spm_resp_t{});
        }
        dma_read_merge_fire_sig.write(false);
        dma_read_merge_data_sig.write(0);
        for (unsigned g = 0; g < NUM_GROUPS; ++g)
            group_meta_fifo_pop[g].write(false);

        for (unsigned g = 0; g < NUM_GROUPS; ++g) {
            if (group_meta_fifo_empty[g].read()) continue;

            const ReadMeta meta    = group_meta_fifo_dout[g].read();
            const unsigned bbase   = g * BANKS_PER_GROUP;
            DEBUG_MSG(" resp_merge g=" << g << " is_dma=" << meta.is_dma
                      << " port=" << (int)meta.port_id << " mode=" << (int)meta.mode
                      << " addr=" << meta.addr, DEBUG_LEVEL_CLUSTER_COMPONENTS);

            // Check all required bank response valids
            bool all_avail = true;
            if (meta.mode == ReqMode::LINEAR) {
                unsigned bidx = (meta.addr / BANK_DEPTH) + bbase;
                if (!bank_resp_valid_sig[bidx].read()) all_avail = false;
            } else {
                for (unsigned k = 0; k < BANKS_PER_GROUP; ++k)
                    if (!bank_resp_valid_sig[bbase + k].read()) { all_avail = false; break; }
            }
            if (!all_avail) {
                DEBUG_MSG(" resp_merge g=" << g << " STALL: all_avail=0 @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                continue;
            }

            if (!meta.is_dma) {
                unsigned p = meta.port_id;
                if (drop_noc_resp_i.read()) {
                    group_meta_fifo_pop[g].write(true);
                    continue;
                }
                if (port_resp_fifo_full[p].read()) {
                    DEBUG_MSG(" resp_merge g=" << g << " STALL: port_resp_fifo_full[" << p << "]=1 @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                }
            }

            if (meta.is_dma) {
                if (!dma_read_resp_fifo_full.read()) {
                    unsigned bidx = (meta.addr / BANK_DEPTH) + bbase;
                    dma_read_merge_data_sig.write(bank_resp_data_sig[bidx].read());
                    dma_read_merge_fire_sig.write(true);
                    group_meta_fifo_pop[g].write(true);
                    DEBUG_MSG(" DMA RESP MERGE grp=" << g << " addr=" << meta.addr << " data=" << std::hex << bank_resp_data_sig[bidx].read() << std::dec << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                }
            } else {
                unsigned p = meta.port_id;
                if (!port_resp_fifo_full[p].read()) {
                    spm_resp_t resp;
                    resp.code = SPM_RESPONSE_CODE::SPM_OK;
                    sc_biguint<NOC_DATA_WIDTH> rdata = 0;
                    if (meta.mode == ReqMode::LINEAR) {
                        unsigned bidx = (meta.addr / BANK_DEPTH) + bbase;
                        rdata.range(BANK_DATA_WIDTH - 1, 0) = bank_resp_data_sig[bidx].read();
                        rdata.range(NOC_DATA_WIDTH - 1, BANK_DATA_WIDTH) = 0;
                    } else {
                        for (unsigned k = 0; k < BANKS_PER_GROUP; ++k)
                            rdata.range((k+1)*BANK_DATA_WIDTH-1, k*BANK_DATA_WIDTH) =
                                bank_resp_data_sig[bbase + k].read();
                    }
                    resp.rdata = rdata;
                    DEBUG_MSG(" comb_resp_merge p=" << p << " rdata=" << std::hex << rdata << std::dec << " at " << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                    rd_resp_push_sig[p].write(true);
                    rd_resp_data_sig[p].write(resp);
                    group_meta_fifo_pop[g].write(true);
                }
            }
        }
    }

    // =========================================================================
    // comb_port_resp_fifo_ctrl
    //
    // Merge write-response (wr_resp_push_sig) and read-response (rd_resp_push_sig)
    // into the per-port response FIFO inputs.
    // By design these two sources never conflict for the same port in the same
    // cycle (write-resp is immediate; read-resp comes ≥1 cycle after the request).
    // =========================================================================
    void comb_port_resp_fifo_ctrl() {
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            const bool wr = wr_resp_push_sig[p].read();
            const bool rd = rd_resp_push_sig[p].read();
            // Structural assertion: both must not occur simultaneously.
            assert(!(wr && rd) && "wr and rd response conflict on same port");

            if (wr) {
                port_resp_fifo_push[p].write(true);
                port_resp_fifo_din[p].write(wr_resp_data_sig[p].read());
            } else if (rd) {
                port_resp_fifo_push[p].write(true);
                port_resp_fifo_din[p].write(rd_resp_data_sig[p].read());
            } else {
                port_resp_fifo_push[p].write(false);
                port_resp_fifo_din[p].write(spm_resp_t{});
            }
        }
    }

    // =========================================================================
    // comb_spm_resp_output
    // Drive NoC response ports directly from port_resp_fifo (FWFT).
    // =========================================================================
    void comb_spm_resp_output() {
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            spm_resp_valid_o[p].write(!port_resp_fifo_empty[p].read());
            spm_resp_o[p].write(port_resp_fifo_dout[p].read());
        }
    }

    // =========================================================================
    // comb_port_resp_fifo_pop
    // Pop the per-port FIFO on valid/ready handshake.
    // =========================================================================
    void comb_port_resp_fifo_pop() {
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            port_resp_fifo_pop[p].write(
                !port_resp_fifo_empty[p].read() && spm_resp_ready_i[p].read());
        }
    }

    // =========================================================================
    // comb_dma_resp_b   (AXI B channel)
    // =========================================================================
    void comb_dma_resp_b() {
        const bool pending = (dma_b_pending_cnt_reg.read() > 0);
        s_axi_bvalid_o.write(pending);
        s_axi_bresp_o.write(0); // OKAY
    }

    // =========================================================================
    // comb_dma_resp_r   (AXI R channel)
    // =========================================================================
    void comb_dma_resp_r() {
        const bool valid = !dma_read_resp_fifo_empty.read();
        s_axi_rvalid_o.write(valid);
        s_axi_rdata_o.write(valid ? dma_read_resp_fifo_dout.read()
                                   : sc_biguint<BANK_DATA_WIDTH>(0));
        s_axi_rresp_o.write(0); // OKAY
    }

    // =========================================================================
    // comb_dma_read_resp_fifo_push
    // Forward dma_read_merge_fire / data into the DMA read-resp FIFO inputs.
    // =========================================================================
    void comb_dma_read_resp_fifo_push() {
        dma_read_resp_fifo_push.write(dma_read_merge_fire_sig.read());
        dma_read_resp_fifo_din.write(dma_read_merge_data_sig.read());
    }

    // =========================================================================
    // comb_dma_read_resp_fifo_pop
    // Pop the DMA read-resp FIFO when the R channel handshakes.
    // =========================================================================
    void comb_dma_read_resp_fifo_pop() {
        dma_read_resp_fifo_pop.write(!dma_read_resp_fifo_empty.read() && s_axi_rready_i.read());
    }

    // =========================================================================
    // comb_bank_resp_ready
    // Accept SRAM output only when comb_resp_merge will actually consume it
    // (group_meta_fifo_pop[g] == true).  This back-pressures the SRAM output
    // register when the downstream port_resp_fifo or dma_read_resp_fifo is full.
    // =========================================================================
    void comb_bank_resp_ready() {
        for (unsigned b = 0; b < TOTAL_BANKS; ++b) {
            bank_resp_ready_sig[b].write(false);
        }

        for (unsigned g = 0; g < NUM_GROUPS; ++g) {
            if (!group_meta_fifo_pop[g].read() || group_meta_fifo_empty[g].read()) {
                continue;
            }

            const ReadMeta meta = group_meta_fifo_dout[g].read();
            const unsigned bbase = g * BANKS_PER_GROUP;

            if (meta.mode == ReqMode::PARALLEL) {
                for (unsigned k = 0; k < BANKS_PER_GROUP; ++k) {
                    bank_resp_ready_sig[bbase + k].write(true);
                }
            } else {
                const unsigned k = (meta.addr / BANK_DEPTH);
                if (k < BANKS_PER_GROUP) {
                    bank_resp_ready_sig[bbase + k].write(true);
                }
            }
        }
    }

    // =========================================================================
    // pmu_output_process
    // =========================================================================
    void pmu_output_process() {
        pmu_cycle_cnt_o.write(pmu_cycle_cnt_reg.read());
        pmu_arb_stall_cnt_o.write(pmu_arb_stall_cnt_reg.read());
        pmu_credit_stall_cnt_o.write(pmu_credit_stall_cnt_reg.read());
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p)
            pmu_port_txn_cnt_o[p].write(pmu_port_txn_cnt_reg[p].read());
    }

    // =========================================================================
    // seq_process  (SC_CTHREAD)
    //
    // The ONLY place where _reg signals are written.
    // No output ports are driven here (except via comb_* processes downstream).
    // =========================================================================
    void seq_process() {
        // ----- Reset -----
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            skid_valid_reg[p].write(false);
            skid_data_reg[p].write(spm_req_t{});
            credit_cnt_reg[p].write(sc_uint<8>(MAX_OUTSTANDING));
            active_map_reg[p].write(sc_uint<2>(p % NUM_GROUPS));
            pmu_port_txn_cnt_reg[p].write(0);
            port_resp_fifo_clear[p].write(true);
        }
        for (unsigned g = 0; g < NUM_GROUPS; ++g)
            group_meta_fifo_clear[g].write(true);

        dma_b_pending_cnt_reg.write(0);
        dma_rd_inflight_cnt_reg.write(0);
        pmu_cycle_cnt_reg.write(0);
        pmu_arb_stall_cnt_reg.write(0);
        pmu_credit_stall_cnt_reg.write(0);

        // Pulse all FIFO clears for one cycle
        dma_aw_fifo_clear.write(true);
        dma_w_data_fifo_clear.write(true);
        dma_w_strb_fifo_clear.write(true);
        dma_write_req_fifo_clear.write(true);
        dma_read_req_fifo_clear.write(true);
        dma_read_resp_fifo_clear.write(true);

        wait();  // hold reset for one cycle

        // Deassert all clears
        dma_aw_fifo_clear.write(false);
        dma_w_data_fifo_clear.write(false);
        dma_w_strb_fifo_clear.write(false);
        dma_write_req_fifo_clear.write(false);
        dma_read_req_fifo_clear.write(false);
        dma_read_resp_fifo_clear.write(false);
        for (unsigned g = 0; g < NUM_GROUPS; ++g)  group_meta_fifo_clear[g].write(false);
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) port_resp_fifo_clear[p].write(false);

        // ----- Main loop -----
        while (true) {
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                port_resp_fifo_clear[p].write(drop_noc_resp_i.read());
            }

            // WDATA and WSTRB are generated from the same AXI W handshake.
            // If they diverge, the merged DMA write packet can carry mismatched mask/data.
            if (dma_w_data_fifo_empty.read() != dma_w_strb_fifo_empty.read()) {
                SC_REPORT_ERROR(
                    name(),
                    "dma_w_data_fifo_empty != dma_w_strb_fifo_empty (W channel desync)");
            }
            if (dma_w_data_fifo_full.read() != dma_w_strb_fifo_full.read()) {
                SC_REPORT_ERROR(
                    name(),
                    "dma_w_data_fifo_full != dma_w_strb_fifo_full (W channel desync)");
            }

            // -- PMU cycle / reset --
            if (pmu_rst_i.read()) {
                pmu_cycle_cnt_reg.write(0);
                pmu_arb_stall_cnt_reg.write(0);
                pmu_credit_stall_cnt_reg.write(0);
                for (unsigned p = 0; p < NUM_NOC_PORTS; ++p)
                    pmu_port_txn_cnt_reg[p].write(0);
            } else {
                pmu_cycle_cnt_reg.write(pmu_cycle_cnt_reg.read() + 1);
            }

            // -- Config update --
            if (config_update_i.read()) {
                sc_uint<8> map = config_map_i.read();
                std::array<bool, 4> used = {false, false, false, false};
                bool ok = true;
                for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                    unsigned g = (map >> (p * 2)) & 0x3u;
                    if (g >= NUM_GROUPS || used[g]) { ok = false; break; }
                    used[g] = true;
                }
                if (ok) {
                    for (unsigned p = 0; p < NUM_NOC_PORTS; ++p)
                        active_map_reg[p].write(sc_uint<2>((map >> (p * 2)) & 0x3u));
                }
            }

            // -- Soft reset: clear FIFOs / skid / DMA counters, preserve SRAM & config --
            if (soft_reset_i.read()) {
                for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                    skid_valid_reg[p].write(false);
                    credit_cnt_reg[p].write(sc_uint<8>(MAX_OUTSTANDING));
                    port_resp_fifo_clear[p].write(true);
                }
                for (unsigned g = 0; g < NUM_GROUPS; ++g)
                    group_meta_fifo_clear[g].write(true);
                dma_b_pending_cnt_reg.write(0);
                dma_rd_inflight_cnt_reg.write(0);
                dma_aw_fifo_clear.write(true);
                dma_w_data_fifo_clear.write(true);
                dma_w_strb_fifo_clear.write(true);
                dma_write_req_fifo_clear.write(true);
                dma_read_req_fifo_clear.write(true);
                dma_read_resp_fifo_clear.write(true);
                wait();
                // Deassert clears after one cycle
                for (unsigned p = 0; p < NUM_NOC_PORTS; ++p)
                    port_resp_fifo_clear[p].write(false);
                for (unsigned g = 0; g < NUM_GROUPS; ++g)
                    group_meta_fifo_clear[g].write(false);
                dma_aw_fifo_clear.write(false);
                dma_w_data_fifo_clear.write(false);
                dma_w_strb_fifo_clear.write(false);
                dma_write_req_fifo_clear.write(false);
                dma_read_req_fifo_clear.write(false);
                dma_read_resp_fifo_clear.write(false);
                continue;  // restart main loop
            }

            // -- Skid buffer update --
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                const bool fire       = port_req_fire_sig[p].read();
                const bool from_skid  = skid_valid_reg[p].read();
                const bool incoming   = spm_req_valid_i[p].read() && !from_skid;

                if (fire && from_skid) {
                    // Skid buffer consumed
                    skid_valid_reg[p].write(false);
                } else if (incoming && !fire) {
                    // New request stalled → save into skid buffer
                    skid_valid_reg[p].write(true);
                    skid_data_reg[p].write(spm_req_i[p].read());
                }
                // incoming && fire: request consumed directly, skid stays false
            }

            // -- Credit counter update --
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                const bool rd_issue = port_req_fire_sig[p].read() &&
                                      !port_req_is_write_sig[p].read();
                const bool rd_done  = rd_resp_push_sig[p].read();
                sc_uint<8> cnt = credit_cnt_reg[p].read();
                if (rd_issue && !rd_done) cnt = cnt - 1;
                if (!rd_issue &&  rd_done) cnt = cnt + 1;
                credit_cnt_reg[p].write(cnt);
            }

            // -- PMU transaction counters --
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                if (port_req_fire_sig[p].read())
                    pmu_port_txn_cnt_reg[p].write(pmu_port_txn_cnt_reg[p].read() + 1);
            }

            // -- PMU stall counters --
            if (credit_stall_sig.read())
                pmu_credit_stall_cnt_reg.write(pmu_credit_stall_cnt_reg.read() + 1);
            if (arb_stall_sig.read())
                pmu_arb_stall_cnt_reg.write(pmu_arb_stall_cnt_reg.read() + 1);

            // -- DMA B-pending counter --
            {
                const bool b_fire  = s_axi_bvalid_o.read() && s_axi_bready_i.read();
                const bool wr_fire = dma_write_fire_sig.read();
                sc_uint<8> bp = dma_b_pending_cnt_reg.read();
                if ( wr_fire && !b_fire) bp = bp + 1;
                if (!wr_fire &&  b_fire && bp > 0) bp = bp - 1;
                dma_b_pending_cnt_reg.write(bp);
            }

            // -- DMA read in-flight counter --
            {
                const bool rd_issue = dma_read_fire_sig.read();
                const bool rd_merge = dma_read_merge_fire_sig.read();
                sc_uint<8> inf = dma_rd_inflight_cnt_reg.read();
                if ( rd_issue && !rd_merge) inf = inf + 1;
                if (!rd_issue &&  rd_merge && inf > 0) inf = inf - 1;
                dma_rd_inflight_cnt_reg.write(inf);
            }

            // DMA issue debug print
            if (dma_write_fire_sig.read()) {
                DEBUG_MSG("DMA WRITE ISSUE addr=" << std::hex << dma_write_req_fifo_dout.read().addr.to_uint() << " data=" << dma_write_req_fifo_dout.read().data.to_uint64() << std::dec << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
            }
            if (dma_read_fire_sig.read()) {
                DEBUG_MSG("DMA READ ISSUE addr=" << std::hex << dma_read_req_fifo_dout.read().to_uint() << std::dec << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
            }

            // NoC issue debug print
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                if (port_req_fire_sig[p].read()) {
                    DEBUG_MSG("NOC REQ ISSUE p=" << p << " is_write=" << port_req_is_write_sig[p].read() << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                }
            }

            // NoC response debug print
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                if (rd_resp_push_sig[p].read()) {
                    DEBUG_MSG("NOC RESP PUSH p=" << p << " data=" << std::hex << rd_resp_data_sig[p].read().rdata << std::dec << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                }
            }

            // SRAM request debug print
            for (unsigned b = 0; b < TOTAL_BANKS; ++b) {
                if (bank_req_valid_sig[b].read()) {
                    DEBUG_MSG("SRAM REQ VALID b=" << b << " addr=" << bank_req_addr_sig[b].read() << " write_en=" << bank_write_en_sig[b].read() << " write_data=" << std::hex << bank_write_data_sig[b].read() << std::dec << " write_mask=" << bank_write_mask_sig[b].read() << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                }
            }

            // SRAM response debug print
            for (unsigned g = 0; g < NUM_GROUPS; ++g) {
                if (group_meta_fifo_pop[g].read()) {
                    const ReadMeta meta = group_meta_fifo_dout[g].read();
                    if (meta.mode == ReqMode::LINEAR) {
                        unsigned bidx = (meta.addr / BANK_DEPTH) + g * BANKS_PER_GROUP;
                        DEBUG_MSG("SRAM RESP POP g=" << g << " bidx=" << bidx << " data=" << std::hex << bank_resp_data_sig[bidx].read() << std::dec << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                    } else  {
                        for (unsigned k = 0; k < BANKS_PER_GROUP; ++k) {
                            unsigned bidx = (k) + g * BANKS_PER_GROUP;
                            DEBUG_MSG("SRAM RESP POP g=" << g << " bidx=" << bidx << " data=" << std::hex << bank_resp_data_sig[bidx].read() << std::dec << " @" << sc_time_stamp(), DEBUG_LEVEL_CLUSTER_COMPONENTS);
                        }
                    }
                }
            }

            wait();
        }
    }

    // =========================================================================
    // trace_process
    // =========================================================================
    void trace_process() {
        if (trace_id < 0) return;

        const uint32_t tid_state = static_cast<uint32_t>(trace_id + 1);
        const uint32_t tid_noc_req_base  = static_cast<uint32_t>(trace_id + 2);
        const uint32_t tid_noc_resp_base = tid_noc_req_base + NUM_NOC_PORTS;
        const uint32_t tid_dma           = tid_noc_resp_base + NUM_NOC_PORTS;

        auto btj =[](bool v) -> const char* { return v ? "true" : "false"; };
        auto csv_u =[](const auto& arr) {
            std::ostringstream o;
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) o << ",";
                o << static_cast<unsigned int>(arr[i]);
            }
            return o.str();
        };

        std::array<unsigned, NUM_NOC_PORTS> credits{};
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p)
            credits[p] = credit_cnt_reg[p].read().to_uint();

        if (!trace_init) {
            TRACE_THREAD_NAME(trace_pid, tid_state, std::string(name()) + " SPM_State");
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                TRACE_THREAD_NAME(trace_pid, tid_noc_req_base + p,
                                  std::string(name()) + " SPM_NoC_REQ_L" + std::to_string(p));
                TRACE_THREAD_NAME(trace_pid, tid_noc_resp_base + p,
                                  std::string(name()) + " SPM_NoC_RESP_L" + std::to_string(p));
            }
            TRACE_THREAD_NAME(trace_pid, tid_dma,   std::string(name()) + " SPM_DMA");
            TRACE_EVENT(last_state_spm, "SPM_State", TRACE_BEGIN, trace_pid, tid_state, "{}");
            for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
                const spm_req_t skid = skid_data_reg[p].read();
                TRACE_EVENT(last_state_noc_req[p], "SPM_NoC_REQ", TRACE_BEGIN,
                            trace_pid, tid_noc_req_base + p,
                            std::string("{\"lane\": ") + std::to_string(p)
                            + ", \"credit\": " + std::to_string(credits[p])
                            + "}");
                TRACE_EVENT(last_state_noc_resp[p], "SPM_NoC_RESP", TRACE_BEGIN,
                            trace_pid, tid_noc_resp_base + p,
                            std::string("{\"lane\": ") + std::to_string(p)
                            + ", \"credit\": " + std::to_string(credits[p]) + "}");
            }
            TRACE_EVENT(last_state_dma, "SPM_DMA",   TRACE_BEGIN, trace_pid, tid_dma, "{}");
            trace_init = true;
        }

        auto trace_state =[&](std::string& last, const std::string& cur,
                               const std::string& cat, uint32_t tid, const std::string& args) {
            if (cur != last) {
                TRACE_EVENT(last, cat, TRACE_END,   trace_pid, tid, "{}");
                TRACE_EVENT(cur,  cat, TRACE_BEGIN, trace_pid, tid, args);
                last = cur;
            }
        };

        const bool in_reset = !reset_n.read();
        const bool pmu_rst  = pmu_rst_i.read();

        bool has_noc_req = false, has_noc_resp = false;
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            has_noc_req  = has_noc_req  || spm_req_valid_i[p].read();
            has_noc_resp = has_noc_resp || spm_resp_valid_o[p].read();
        }

        const bool has_dma_req =
            !dma_aw_fifo_empty.read() || !dma_w_data_fifo_empty.read() ||
            !dma_write_req_fifo_empty.read() || !dma_read_req_fifo_empty.read() ||
            (dma_rd_inflight_cnt_reg.read() > 0);
        const bool has_dma_resp =
            (dma_b_pending_cnt_reg.read() > 0) || !dma_read_resp_fifo_empty.read();

        std::array<uint8_t, NUM_NOC_PORTS> map_snap{};
        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p)
            map_snap[p] = static_cast<uint8_t>(active_map_reg[p].read().to_uint());

        std::string spm_st = "IDLE";
        if (in_reset)                         spm_st = "RESET";
        else if (pmu_rst)                     spm_st = "PMU_RESET";
        else if (has_dma_req || has_dma_resp) spm_st = "DMA_ACTIVE";
        else if (has_noc_req || has_noc_resp) spm_st = "NOC_ACTIVE";

        trace_state(last_state_spm, spm_st, "SPM_State", tid_state,
                    std::string("{\"reset_n\": ") + btj(!in_reset)
                    + ", \"map\":[" + csv_u(map_snap) + "]}");

        for (unsigned p = 0; p < NUM_NOC_PORTS; ++p) {
            const bool req_v = spm_req_valid_i[p].read();
            const bool req_r = spm_req_ready_o[p].read();
            const bool req_fire = req_v && req_r;
            const spm_req_t skid = skid_data_reg[p].read();

            std::string req_st = "IDLE";
            if      (req_fire)           req_st = "XFER";
            else if (req_v && !req_r)    req_st = "BACKPRESSURE";
            else if (req_v)              req_st = "VALID";

            trace_state(last_state_noc_req[p], req_st, "SPM_NoC_REQ",
                        tid_noc_req_base + p,
                        std::string("{\"lane\": ") + std::to_string(p)
                        + ", \"valid\": " + btj(req_v)
                        + ", \"ready\": " + btj(req_r)
                        + ", \"credit\": " + std::to_string(credits[p])
                        + "}");

            const bool resp_v = spm_resp_valid_o[p].read();
            const bool resp_r = spm_resp_ready_i[p].read();
            const bool resp_fire = resp_v && resp_r;

            std::string resp_st = "IDLE";
            if      (resp_fire)          resp_st = "XFER";
            else if (resp_v && !resp_r) resp_st = "BACKPRESSURE";
            else if (resp_v)             resp_st = "VALID";

            trace_state(last_state_noc_resp[p], resp_st, "SPM_NoC_RESP",
                        tid_noc_resp_base + p,
                        std::string("{\"lane\": ") + std::to_string(p)
                        + ", \"valid\": " + btj(resp_v)
                        + ", \"ready\": " + btj(resp_r)
                        + ", \"credit\": " + std::to_string(credits[p]) + "}");
        }

        const bool dma_aw_fire = s_axi_awvalid_i.read() && s_axi_awready_o.read();
        const bool dma_w_fire  = s_axi_wvalid_i.read() && s_axi_wready_o.read();
        const bool dma_ar_fire = s_axi_arvalid_i.read() && s_axi_arready_o.read();
        const bool dma_b_fire  = s_axi_bvalid_o.read() && s_axi_bready_i.read();
        const bool dma_r_fire  = s_axi_rvalid_o.read() && s_axi_rready_i.read();

        std::string dma_st = "IDLE";
        if      (dma_b_fire || dma_r_fire)              dma_st = "RESP";
        else if (dma_aw_fire || dma_w_fire || dma_ar_fire) dma_st = "REQ";
        else if (has_dma_req || has_dma_resp)           dma_st = "PENDING";

        trace_state(last_state_dma, dma_st, "SPM_DMA", tid_dma,
                std::string("{\"wr_req_full\": ") + btj(dma_write_req_fifo_full.read())
                + ", \"rd_inflight\": "   + std::to_string(dma_rd_inflight_cnt_reg.read().to_uint())
                + ", \"b_pending\": "     + std::to_string(dma_b_pending_cnt_reg.read().to_uint())
                + ", \"r_resp_full\": "   + btj(dma_read_resp_fifo_full.read()) + "}");
    }
};

} // namespace cluster
} // namespace hybridacc
