#pragma once

#include <systemc>
#include <vector>
#include <deque>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <array>
#include <string>

#include "Cluster/SRAM.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {



template <unsigned NUM_NOC_PORT = 4, unsigned NUM_BANKS_PER_GROUP = 3, unsigned BANK_WIDTH_BITS = 64, unsigned ADDR_WIDTH = 32>
SC_MODULE(ScratchpadMemory) {
    static constexpr size_t kNumGroups = NUM_NOC_PORT;
    static constexpr size_t kBanksPerGroup = NUM_BANKS_PER_GROUP;
    static constexpr size_t kNumBanks = kNumGroups * kBanksPerGroup;
    static constexpr size_t kNumNoCPorts = NUM_NOC_PORT;
    static constexpr uint64_t kFullByteMask = (1ULL << (BANK_WIDTH_BITS / 8)) - 1;

    enum class SrcType : uint8_t {
        SRC_NOC = 0,
        SRC_DMA = 1
    };

    struct BankReadMeta {
        SrcType src_reg;
        uint8_t port_reg;
        uint8_t slice_reg;
    };

    struct AccessDecode {
        bool valid_sig;
        bool parallel_sig;
        uint32_t row_reg;
        uint8_t bank_sel_reg;
    };

    struct DmaAddrDecode {
        bool valid_sig;
        uint8_t group_reg;
        uint32_t local_addr_reg;
    };

    struct GroupReq {
        bool valid_sig;
        bool is_noc_sig;
        uint8_t noc_port_reg;
        bool is_write_sig;
        bool parallel_sig;
        uint32_t row_reg;
        uint8_t bank_sel_reg;
        sc_biguint<NUM_BANKS_PER_GROUP*BANK_WIDTH_BITS> noc_wdata_reg;
        sc_biguint<BANK_WIDTH_BITS> dma_wdata_reg;
    };

    // Clock/Reset
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Global config
    sc_in< sc_uint<8> > config_map_i;      // 4 ports * 2-bit group id
    sc_in<bool> config_update_i;
    sc_in<bool> arb_policy_i;              // reserved extension, 0 = fixed NoC>DMA

    // NoC ports: port 0~2 read-only, port 3 write-only
    sc_vector< sc_in<bool> > noc_req_vld_i;
    sc_vector< sc_out<bool> > noc_req_rdy_o;
    sc_vector< sc_in< sc_uint<ADDR_WIDTH> > > noc_addr_i;
    sc_vector< sc_in<bool> > noc_mode_i; // 0:linear, 1:parallel
    sc_vector< sc_out< sc_biguint<NUM_BANKS_PER_GROUP*BANK_WIDTH_BITS> > > noc_rdata_o;
    sc_vector< sc_in< sc_biguint<NUM_BANKS_PER_GROUP*BANK_WIDTH_BITS> > > noc_wdata_i;
    sc_vector< sc_out<bool> > noc_resp_vld_o;

    // DMA port (64b)
    sc_in<bool> dma_req_vld_i;
    sc_out<bool> dma_req_rdy_o;
    sc_in< sc_uint<ADDR_WIDTH> > dma_addr_i; // global addr = group + local
    sc_in<bool> dma_rw_i;                    // 0:read, 1:write
    sc_in< sc_biguint<BANK_WIDTH_BITS> > dma_wdata_i;
    sc_out< sc_biguint<BANK_WIDTH_BITS> > dma_rdata_o;
    sc_out<bool> dma_done_o;

    // Bank interface signals (_sig)
    sc_vector< sc_signal< sc_uint<ADDR_WIDTH> > > bank_req_addr_sig;
    sc_vector< sc_signal<bool> > bank_req_valid_sig;
    sc_vector< sc_signal<bool> > bank_req_ready_sig;
    sc_vector< sc_signal< sc_biguint<BANK_WIDTH_BITS> > > bank_resp_data_sig;
    sc_vector< sc_signal<bool> > bank_resp_valid_sig;
    sc_vector< sc_signal<bool> > bank_resp_ready_sig;
    sc_vector< sc_signal<bool> > bank_write_en_sig;
    sc_vector< sc_signal< sc_uint<ADDR_WIDTH> > > bank_write_addr_sig;
    sc_vector< sc_signal< sc_biguint<BANK_WIDTH_BITS> > > bank_write_data_sig;
    sc_vector< sc_signal< sc_uint<8> > > bank_write_mask_sig;

    // SRAM bank instances
    std::array< SRAM<BANK_WIDTH_BITS, ADDR_WIDTH>*, kNumBanks > bank_inst;

    // Register states (_reg)
    std::array<uint8_t, kNumNoCPorts> active_map_reg;
    std::array<sc_biguint<NUM_BANKS_PER_GROUP*BANK_WIDTH_BITS>, kNumNoCPorts> noc_rdata_reg;
    std::array<uint8_t, kNumNoCPorts> noc_read_pending_cnt_reg;
    std::array<bool, kNumNoCPorts> noc_write_ack_pending_reg;

    bool dma_read_pending_reg;
    bool dma_write_ack_pending_reg;
    sc_biguint<BANK_WIDTH_BITS> dma_rdata_reg;

    std::array< std::deque<BankReadMeta>, kNumBanks > bank_read_meta_q_reg;

    const uint32_t bank_depth_words;
    const uint32_t bank_word_bytes;
    const uint32_t group_linear_words;
    const uint32_t group_parallel_base;
    const uint32_t group_span_words;

    SC_HAS_PROCESS(ScratchpadMemory);

    ScratchpadMemory(sc_module_name name,
                     uint32_t bank_depth_words = 1024,
                     size_t bank_latency = 1,
                     size_t bank_pipeline_depth = 1)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          config_map_i("config_map_i"),
          config_update_i("config_update_i"),
          arb_policy_i("arb_policy_i"),
          noc_req_vld_i("noc_req_vld_i", kNumNoCPorts),
          noc_req_rdy_o("noc_req_rdy_o", kNumNoCPorts),
          noc_addr_i("noc_addr_i", kNumNoCPorts),
          noc_mode_i("noc_mode_i", kNumNoCPorts),
          noc_rdata_o("noc_rdata_o", kNumNoCPorts),
          noc_wdata_i("noc_wdata_i", kNumNoCPorts),
          noc_resp_vld_o("noc_resp_vld_o", kNumNoCPorts),
          dma_req_vld_i("dma_req_vld_i"),
          dma_req_rdy_o("dma_req_rdy_o"),
          dma_addr_i("dma_addr_i"),
          dma_rw_i("dma_rw_i"),
          dma_wdata_i("dma_wdata_i"),
          dma_rdata_o("dma_rdata_o"),
          dma_done_o("dma_done_o"),
          bank_req_addr_sig("bank_req_addr_sig", kNumBanks),
          bank_req_valid_sig("bank_req_valid_sig", kNumBanks),
          bank_req_ready_sig("bank_req_ready_sig", kNumBanks),
          bank_resp_data_sig("bank_resp_data_sig", kNumBanks),
          bank_resp_valid_sig("bank_resp_valid_sig", kNumBanks),
          bank_resp_ready_sig("bank_resp_ready_sig", kNumBanks),
          bank_write_en_sig("bank_write_en_sig", kNumBanks),
          bank_write_addr_sig("bank_write_addr_sig", kNumBanks),
          bank_write_data_sig("bank_write_data_sig", kNumBanks),
          bank_write_mask_sig("bank_write_mask_sig", kNumBanks),
          bank_inst{},
          active_map_reg{0, 1, 2, 3},
          noc_rdata_reg{},
          noc_read_pending_cnt_reg{0, 0, 0, 0},
          noc_write_ack_pending_reg{false, false, false, false},
          dma_read_pending_reg(false),
          dma_write_ack_pending_reg(false),
          dma_rdata_reg(0),
          bank_read_meta_q_reg{},
          bank_depth_words(bank_depth_words),
          bank_word_bytes(8),
          group_linear_words(bank_depth_words * static_cast<uint32_t>(kBanksPerGroup)),
          group_parallel_base(bank_depth_words * static_cast<uint32_t>(kBanksPerGroup)),
          group_span_words(bank_depth_words * static_cast<uint32_t>(kBanksPerGroup + 1))
    {
        const size_t bank_size_bytes = static_cast<size_t>(bank_depth_words) * bank_word_bytes;
        for (size_t b = 0; b < kNumBanks; ++b) {
            const std::string bank_name = "bank_" + std::to_string(b);
            bank_inst[b] = new SRAM<BANK_WIDTH_BITS, ADDR_WIDTH>(bank_name.c_str(), bank_size_bytes, bank_latency, bank_pipeline_depth);

            bank_inst[b]->clk(clk);
            bank_inst[b]->reset_n(reset_n);

            bank_inst[b]->req_addr(bank_req_addr_sig[b]);
            bank_inst[b]->req_valid(bank_req_valid_sig[b]);
            bank_inst[b]->req_ready(bank_req_ready_sig[b]);
            bank_inst[b]->resp_data(bank_resp_data_sig[b]);
            bank_inst[b]->resp_valid(bank_resp_valid_sig[b]);
            bank_inst[b]->resp_ready(bank_resp_ready_sig[b]);

            bank_inst[b]->write_en(bank_write_en_sig[b]);
            bank_inst[b]->write_addr(bank_write_addr_sig[b]);
            bank_inst[b]->write_data(bank_write_data_sig[b]);
            bank_inst[b]->write_mask(bank_write_mask_sig[b]);
        }

        SC_CTHREAD(main_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

    ~ScratchpadMemory() override {
        for (size_t b = 0; b < kNumBanks; ++b) {
            delete bank_inst[b];
            bank_inst[b] = nullptr;
        }
    }

private:
    static uint8_t extract_map_field(sc_uint<8> cfg, uint8_t port) {
        return static_cast<uint8_t>((cfg >> (port * 2)) & 0x3);
    }

    static bool valid_permutation(const std::array<uint8_t, kNumNoCPorts>& map_reg) {
        bool seen[4] = {false, false, false, false};
        for (size_t p = 0; p < kNumNoCPorts; ++p) {
            if (map_reg[p] >= 4) return false;
            if (seen[map_reg[p]]) return false;
            seen[map_reg[p]] = true;
        }
        return true;
    }

    AccessDecode decode_local_addr(uint32_t local_addr_reg, bool parallel_sig) const {
        AccessDecode dec{};
        dec.valid_sig = false;
        dec.parallel_sig = parallel_sig;
        dec.row_reg = 0;
        dec.bank_sel_reg = 0;

        if (parallel_sig) {
            if (local_addr_reg >= group_parallel_base && local_addr_reg < group_span_words) {
                dec.valid_sig = true;
                dec.row_reg = local_addr_reg - group_parallel_base;
                dec.bank_sel_reg = 0;
            }
        } else {
            if (local_addr_reg < group_linear_words) {
                dec.valid_sig = true;
                dec.row_reg = local_addr_reg / static_cast<uint32_t>(kBanksPerGroup);
                dec.bank_sel_reg = static_cast<uint8_t>(local_addr_reg % static_cast<uint32_t>(kBanksPerGroup));
            }
        }

        return dec;
    }

    DmaAddrDecode decode_dma_addr(sc_uint<ADDR_WIDTH> dma_addr_reg) const {
        DmaAddrDecode dec{};
        dec.valid_sig = false;
        dec.group_reg = 0;
        dec.local_addr_reg = 0;

        const uint64_t global_addr = dma_addr_reg.to_uint64();
        const uint64_t group_idx = global_addr / static_cast<uint64_t>(group_span_words);
        const uint64_t local = global_addr % static_cast<uint64_t>(group_span_words);

        if (group_idx < kNumGroups) {
            dec.valid_sig = true;
            dec.group_reg = static_cast<uint8_t>(group_idx);
            dec.local_addr_reg = static_cast<uint32_t>(local);
        }
        return dec;
    }

    static uint32_t bank_index(uint8_t group_reg, uint8_t bank_sel_reg) {
        return static_cast<uint32_t>(group_reg) * static_cast<uint32_t>(kBanksPerGroup) + static_cast<uint32_t>(bank_sel_reg);
    }

    void clear_bank_cmd_sig() {
        for (size_t b = 0; b < kNumBanks; ++b) {
            bank_req_valid_sig[b].write(false);
            bank_write_en_sig[b].write(false);
            bank_req_addr_sig[b].write(0);
            bank_write_addr_sig[b].write(0);
            bank_write_data_sig[b].write(0);
            bank_write_mask_sig[b].write(0);
            bank_resp_ready_sig[b].write(true);
        }
    }

    void main_process() {
        // Reset
        for (size_t p = 0; p < kNumNoCPorts; ++p) {
            noc_req_rdy_o[p].write(false);
            noc_rdata_o[p].write(0);
            noc_resp_vld_o[p].write(false);
            active_map_reg[p] = static_cast<uint8_t>(p);
            noc_rdata_reg[p] = 0;
            noc_read_pending_cnt_reg[p] = 0;
            noc_write_ack_pending_reg[p] = false;
        }

        dma_req_rdy_o.write(false);
        dma_rdata_o.write(0);
        dma_done_o.write(false);
        dma_rdata_reg = 0;
        dma_read_pending_reg = false;
        dma_write_ack_pending_reg = false;

        for (size_t b = 0; b < kNumBanks; ++b) {
            bank_read_meta_q_reg[b].clear();
        }

        clear_bank_cmd_sig();
        wait();

        while (true) {
            // Default output values (pulse outputs are one-cycle)
            for (size_t p = 0; p < kNumNoCPorts; ++p) {
                noc_resp_vld_o[p].write(false);
            }
            dma_done_o.write(false);

            clear_bank_cmd_sig();

            // -----------------------------
            // Stage A: bank read responses
            // -----------------------------
            auto noc_rdata_next = noc_rdata_reg;
            auto noc_read_pending_cnt_next = noc_read_pending_cnt_reg;
            auto noc_write_ack_pending_next = noc_write_ack_pending_reg;
            bool dma_read_pending_next = dma_read_pending_reg;
            bool dma_write_ack_pending_next = dma_write_ack_pending_reg;
            sc_biguint<BANK_WIDTH_BITS> dma_rdata_next = dma_rdata_reg;
            auto active_map_next = active_map_reg;

            for (size_t b = 0; b < kNumBanks; ++b) {
                if (!bank_resp_valid_sig[b].read()) continue;
                if (bank_read_meta_q_reg[b].empty()) continue;

                const BankReadMeta meta_reg = bank_read_meta_q_reg[b].front();
                bank_read_meta_q_reg[b].pop_front();
                const sc_biguint<BANK_WIDTH_BITS> bank_data_reg = bank_resp_data_sig[b].read();

                if (meta_reg.src_reg == SrcType::SRC_NOC) {
                    const uint8_t p = meta_reg.port_reg;
                    const uint8_t slice = meta_reg.slice_reg;
                    noc_rdata_next[p].range(static_cast<int>(slice) * BANK_WIDTH_BITS + BANK_WIDTH_BITS-1, static_cast<int>(slice) * BANK_WIDTH_BITS) = bank_data_reg;

                    if (noc_read_pending_cnt_next[p] > 0) {
                        noc_read_pending_cnt_next[p]--;
                        if (noc_read_pending_cnt_next[p] == 0) {
                            noc_resp_vld_o[p].write(true);
                        }
                    }
                } else {
                    dma_rdata_next = bank_data_reg;
                    if (dma_read_pending_next) {
                        dma_read_pending_next = false;
                        dma_done_o.write(true);
                    }
                }
            }

            // -----------------------------
            // Stage B: delayed write acks
            // -----------------------------
            for (size_t p = 0; p < kNumNoCPorts; ++p) {
                if (noc_write_ack_pending_reg[p]) {
                    noc_resp_vld_o[p].write(true);
                    noc_write_ack_pending_next[p] = false;
                }
            }
            if (dma_write_ack_pending_reg) {
                dma_done_o.write(true);
                dma_write_ack_pending_next = false;
            }

            // -------------------------------------
            // Stage C: config update (wave boundary)
            // -------------------------------------
            if (config_update_i.read()) {
                std::array<uint8_t, kNumNoCPorts> proposal_reg{};
                for (size_t p = 0; p < kNumNoCPorts; ++p) {
                    proposal_reg[p] = extract_map_field(config_map_i.read(), static_cast<uint8_t>(p));
                }
                if (valid_permutation(proposal_reg)) {
                    active_map_next = proposal_reg;
                }
            }

            // -------------------------------------
            // Stage D: build group requests (NoC>DMA)
            // -------------------------------------
            std::array<GroupReq, kNumGroups> group_req_sig{};
            for (size_t g = 0; g < kNumGroups; ++g) {
                group_req_sig[g].valid_sig = false;
            }

            // NoC request candidates (highest priority)
            for (size_t p = 0; p < kNumNoCPorts; ++p) {
                const uint8_t g = active_map_reg[p];
                const bool port_idle_sig = (noc_read_pending_cnt_reg[p] == 0) && (!noc_write_ack_pending_reg[p]);
                const bool req_sig = noc_req_vld_i[p].read() && port_idle_sig;
                if (!req_sig) continue;

                const bool is_parallel_sig = noc_mode_i[p].read();
                const AccessDecode dec_sig = decode_local_addr(noc_addr_i[p].read().to_uint(), is_parallel_sig);
                if (!dec_sig.valid_sig) continue;

                const bool is_write_sig = (p == 3);

                GroupReq req{};
                req.valid_sig = true;
                req.is_noc_sig = true;
                req.noc_port_reg = static_cast<uint8_t>(p);
                req.is_write_sig = is_write_sig;
                req.parallel_sig = dec_sig.parallel_sig;
                req.row_reg = dec_sig.row_reg;
                req.bank_sel_reg = dec_sig.bank_sel_reg;
                req.noc_wdata_reg = noc_wdata_i[p].read();
                req.dma_wdata_reg = 0;

                group_req_sig[g] = req;
            }

            // DMA candidate (secondary priority, per target group)
            const bool dma_idle_sig = (!dma_read_pending_reg) && (!dma_write_ack_pending_reg);
            const DmaAddrDecode dma_dec_sig = decode_dma_addr(dma_addr_i.read());

            if (dma_req_vld_i.read() && dma_idle_sig && dma_dec_sig.valid_sig) {
                const AccessDecode dma_local_dec_sig = decode_local_addr(dma_dec_sig.local_addr_reg, false);
                if (dma_local_dec_sig.valid_sig) {
                    const uint8_t g = dma_dec_sig.group_reg;
                    if (!group_req_sig[g].valid_sig) {
                        GroupReq req{};
                        req.valid_sig = true;
                        req.is_noc_sig = false;
                        req.noc_port_reg = 0;
                        req.is_write_sig = dma_rw_i.read();
                        req.parallel_sig = false;
                        req.row_reg = dma_local_dec_sig.row_reg;
                        req.bank_sel_reg = dma_local_dec_sig.bank_sel_reg;
                        req.noc_wdata_reg = 0;
                        req.dma_wdata_reg = dma_wdata_i.read();
                        group_req_sig[g] = req;
                    }
                }
            }

            // -------------------------------------
            // Stage E: readiness + issue to banks
            // -------------------------------------
            for (size_t p = 0; p < kNumNoCPorts; ++p) {
                noc_req_rdy_o[p].write(false);
            }
            dma_req_rdy_o.write(false);

            for (size_t g = 0; g < kNumGroups; ++g) {
                if (!group_req_sig[g].valid_sig) continue;
                GroupReq& req = group_req_sig[g];

                bool bank_ready_sig = true;
                if (req.parallel_sig) {
                    for (size_t s = 0; s < kBanksPerGroup; ++s) {
                        const uint32_t bidx = bank_index(static_cast<uint8_t>(g), static_cast<uint8_t>(s));
                        bank_ready_sig = bank_ready_sig && bank_req_ready_sig[bidx].read();
                    }
                } else {
                    const uint32_t bidx = bank_index(static_cast<uint8_t>(g), req.bank_sel_reg);
                    bank_ready_sig = bank_req_ready_sig[bidx].read();
                }

                if (!bank_ready_sig) continue;

                const sc_uint<ADDR_WIDTH> byte_addr_sig = static_cast<uint64_t>(req.row_reg) * static_cast<uint64_t>(bank_word_bytes);

                if (req.parallel_sig) {
                    for (size_t s = 0; s < kBanksPerGroup; ++s) {
                        const uint32_t bidx = bank_index(static_cast<uint8_t>(g), static_cast<uint8_t>(s));
                        if (req.is_write_sig) {
                            bank_write_en_sig[bidx].write(true);
                            bank_write_addr_sig[bidx].write(byte_addr_sig);
                            bank_write_data_sig[bidx].write(req.noc_wdata_reg.range(static_cast<int>(s) * BANK_WIDTH_BITS + BANK_WIDTH_BITS-1, static_cast<int>(s) * BANK_WIDTH_BITS));
                            bank_write_mask_sig[bidx].write(kFullByteMask);
                        } else {
                            bank_req_valid_sig[bidx].write(true);
                            bank_req_addr_sig[bidx].write(byte_addr_sig);
                            BankReadMeta meta{};
                            meta.src_reg = req.is_noc_sig ? SrcType::SRC_NOC : SrcType::SRC_DMA;
                            meta.port_reg = req.noc_port_reg;
                            meta.slice_reg = static_cast<uint8_t>(s);
                            bank_read_meta_q_reg[bidx].push_back(meta);
                        }
                    }

                    if (req.is_noc_sig) {
                        noc_req_rdy_o[req.noc_port_reg].write(true);
                        if (req.is_write_sig) {
                            noc_write_ack_pending_next[req.noc_port_reg] = true;
                        } else {
                            noc_read_pending_cnt_next[req.noc_port_reg] = static_cast<uint8_t>(kBanksPerGroup);
                            noc_rdata_next[req.noc_port_reg] = 0;
                        }
                    }
                } else {
                    const uint32_t bidx = bank_index(static_cast<uint8_t>(g), req.bank_sel_reg);

                    if (req.is_write_sig) {
                        bank_write_en_sig[bidx].write(true);
                        bank_write_addr_sig[bidx].write(byte_addr_sig);
                        if (req.is_noc_sig) {
                            bank_write_data_sig[bidx].write(req.noc_wdata_reg.range(63, 0));
                        } else {
                            bank_write_data_sig[bidx].write(req.dma_wdata_reg);
                        }
                        bank_write_mask_sig[bidx].write(kFullByteMask);
                    } else {
                        bank_req_valid_sig[bidx].write(true);
                        bank_req_addr_sig[bidx].write(byte_addr_sig);
                        BankReadMeta meta{};
                        meta.src_reg = req.is_noc_sig ? SrcType::SRC_NOC : SrcType::SRC_DMA;
                        meta.port_reg = req.noc_port_reg;
                        meta.slice_reg = 0;
                        bank_read_meta_q_reg[bidx].push_back(meta);
                    }

                    if (req.is_noc_sig) {
                        noc_req_rdy_o[req.noc_port_reg].write(true);
                        if (req.is_write_sig) {
                            noc_write_ack_pending_next[req.noc_port_reg] = true;
                        } else {
                            noc_read_pending_cnt_next[req.noc_port_reg] = 1;
                            noc_rdata_next[req.noc_port_reg] = 0;
                        }
                    } else {
                        dma_req_rdy_o.write(true);
                        if (req.is_write_sig) {
                            dma_write_ack_pending_next = true;
                        } else {
                            dma_read_pending_next = true;
                        }
                    }
                }
            }

            // Register update (_next -> _reg)
            for (size_t p = 0; p < kNumNoCPorts; ++p) {
                noc_rdata_o[p].write(noc_rdata_next[p]);
            }
            dma_rdata_o.write(dma_rdata_next);

            active_map_reg = active_map_next;
            noc_rdata_reg = noc_rdata_next;
            noc_read_pending_cnt_reg = noc_read_pending_cnt_next;
            noc_write_ack_pending_reg = noc_write_ack_pending_next;
            dma_read_pending_reg = dma_read_pending_next;
            dma_write_ack_pending_reg = dma_write_ack_pending_next;
            dma_rdata_reg = dma_rdata_next;

            wait();
        }
    }
};

} // namespace cluster
} // namespace hybridacc