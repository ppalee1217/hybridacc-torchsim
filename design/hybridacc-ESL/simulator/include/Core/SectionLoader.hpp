#pragma once

/**
 * @file SectionLoader.hpp
 * @brief cc_section_loader — Consumes manifest entries, reads payloads from
 *        DRAM via AXI4, and copies them into local SRAM (I-SRAM / D-SRAM).
 *
 * @par FSM
 *   LD_IDLE → LD_FETCH → LD_ROUTE → LD_WRITE_LOCAL → LD_VERIFY →
 *   LD_DONE / LD_ERR
 *
 * @par Section routing
 *   - `.hacc.core` → Instruction SRAM
 *   - All others    → Data SRAM descriptor/payload ABI region
 *
 * @par Error codes
 *   See @c LoaderError in Types.hpp.
 *
 * @par Explicitly NOT done
 *   - No cluster NoC program stream emission.
 *   - No DMA command injection.
 *   - No NLU config stream.
 *
 * @par Spec reference
 *   Core.md §8.3  cc_section_loader
 */

#include <systemc>
#include <cstdint>
#include <vector>
#include "Utils/utils.hpp"
#include "Core/Types.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

template <unsigned ISRAM_BYTES = kIsramBytes, unsigned DSRAM_BYTES = kDataSramBytes>
SC_MODULE(SectionLoader) {

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- Kick / manifest control (from cc_boot_host_if) ---
    sc_in<bool>          kick_i;               ///< W1P from MANIFEST_KICK
    sc_in<sc_uint<32>>   manifest_addr_lo_i;
    sc_in<sc_uint<32>>   manifest_addr_hi_i;
    sc_in<sc_uint<32>>   manifest_size_i;

    // --- DRAM AXI4 read master (for fetching manifest + payload) ---
    sc_out<bool>         m_mem_axi_ar_valid_o;
    sc_in<bool>          m_mem_axi_ar_ready_i;
    sc_out<sc_uint<32>>  m_mem_axi_ar_addr_o;
    sc_out<sc_uint<8>>   m_mem_axi_ar_len_o;
    sc_in<bool>          m_mem_axi_r_valid_i;
    sc_out<bool>         m_mem_axi_r_ready_o;
    sc_in<sc_biguint<kMemAxiDataWidth>> m_mem_axi_r_data_i;
    sc_in<sc_uint<2>>    m_mem_axi_r_resp_i;
    sc_in<bool>          m_mem_axi_r_last_i;

    // --- Instruction SRAM loader write port ---
    sc_out<bool>         isram_wr_en_o;
    sc_out<sc_uint<32>>  isram_wr_addr_o;
    sc_out<sc_uint<32>>  isram_wr_data_o;
    sc_out<sc_uint<4>>   isram_wr_strb_o;

    // --- Data SRAM loader write port ---
    sc_out<bool>         dsram_wr_en_o;
    sc_out<sc_uint<32>>  dsram_wr_addr_o;
    sc_out<sc_uint<32>>  dsram_wr_data_o;
    sc_out<sc_uint<4>>   dsram_wr_strb_o;

    // --- load_phase indicator (active during load) ---
    sc_out<bool>         load_phase_o;

    // --- Status outputs (to cc_boot_host_if) ---
    sc_out<bool>         busy_o;
    sc_out<bool>         done_o;
    sc_out<sc_uint<32>>  status_o;
    sc_out<sc_uint<32>>  err_code_o;
    sc_out<sc_uint<32>>  err_info_o;

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(SectionLoader);

    SectionLoader(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          kick_i("kick_i"),
          manifest_addr_lo_i("manifest_addr_lo_i"),
          manifest_addr_hi_i("manifest_addr_hi_i"),
          manifest_size_i("manifest_size_i"),
          m_mem_axi_ar_valid_o("m_mem_axi_ar_valid_o"),
          m_mem_axi_ar_ready_i("m_mem_axi_ar_ready_i"),
          m_mem_axi_ar_addr_o("m_mem_axi_ar_addr_o"),
          m_mem_axi_ar_len_o("m_mem_axi_ar_len_o"),
          m_mem_axi_r_valid_i("m_mem_axi_r_valid_i"),
          m_mem_axi_r_ready_o("m_mem_axi_r_ready_o"),
          m_mem_axi_r_data_i("m_mem_axi_r_data_i"),
          m_mem_axi_r_resp_i("m_mem_axi_r_resp_i"),
          m_mem_axi_r_last_i("m_mem_axi_r_last_i"),
          isram_wr_en_o("isram_wr_en_o"),
          isram_wr_addr_o("isram_wr_addr_o"),
          isram_wr_data_o("isram_wr_data_o"),
          isram_wr_strb_o("isram_wr_strb_o"),
          dsram_wr_en_o("dsram_wr_en_o"),
          dsram_wr_addr_o("dsram_wr_addr_o"),
          dsram_wr_data_o("dsram_wr_data_o"),
          dsram_wr_strb_o("dsram_wr_strb_o"),
          load_phase_o("load_phase_o"),
          busy_o("busy_o"),
          done_o("done_o"),
          status_o("status_o"),
          err_code_o("err_code_o"),
          err_info_o("err_info_o")
    {
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    // ========================================================================
    // FSM states (per Core.md §8.3)
    // ========================================================================

    enum class LdState : uint32_t {
        LD_IDLE        = 0,
        LD_FETCH       = 1,  ///< read manifest entry from DRAM
        LD_ROUTE       = 2,  ///< determine target SRAM
        LD_WRITE_LOCAL = 3,  ///< stream payload from DRAM into local SRAM
        LD_VERIFY      = 4,  ///< optional CRC check
        LD_DONE        = 5,
        LD_ERR         = 6,
    };

    static constexpr unsigned kManifestEntryBytes = 32;
    static constexpr unsigned kBeatBytes = kMemAxiDataWidth / 8;

    // ========================================================================
    // Helpers
    // ========================================================================

    void reset_outputs() {
        m_mem_axi_ar_valid_o.write(false);
        m_mem_axi_ar_addr_o.write(0);
        m_mem_axi_ar_len_o.write(0);
        m_mem_axi_r_ready_o.write(false);
        isram_wr_en_o.write(false);
        isram_wr_addr_o.write(0);
        isram_wr_data_o.write(0);
        isram_wr_strb_o.write(0);
        dsram_wr_en_o.write(false);
        dsram_wr_addr_o.write(0);
        dsram_wr_data_o.write(0);
        dsram_wr_strb_o.write(0);
        load_phase_o.write(false);
        busy_o.write(false);
        done_o.write(false);
        status_o.write(0);
        err_code_o.write(0);
        err_info_o.write(0);
    }

    uint32_t make_status(LdState st, unsigned entry_idx) const {
        uint32_t s = 0;
        if (st != LdState::LD_IDLE && st != LdState::LD_DONE && st != LdState::LD_ERR)
            s |= 0x01; // busy
        if (st == LdState::LD_DONE)  s |= 0x02; // done
        if (st == LdState::LD_ERR)   s |= 0x04; // err
        s |= ((static_cast<uint32_t>(st) & 0x0F) << 4);
        s |= ((entry_idx & 0xFFFF) << 16);
        return s;
    }

    /**
     * @brief Validate a manifest entry before payload transfer.
     * @return LoaderError code (LD_OK on success).
     */
    uint32_t validate_entry(const ManifestEntry& e) const {
        // Bad section kind
        if (e.section_kind == 0 || e.section_kind > static_cast<uint16_t>(SectionKind::DEBUG))
            return static_cast<uint32_t>(LoaderError::LD_ERR_BAD_SECTION_KIND);

        // Route target check
        bool is_core = (e.section_kind == static_cast<uint16_t>(SectionKind::CORE));
        if (is_core) {
            if (e.local_addr + e.size_bytes > ISRAM_BYTES)
                return static_cast<uint32_t>(LoaderError::LD_ERR_LOCAL_ADDR_OOB);
        } else {
            // Data RAM: local_addr is relative to kBaseDataRam
            uint32_t rel = e.local_addr;
            if (rel + e.size_bytes > DSRAM_BYTES)
                return static_cast<uint32_t>(LoaderError::LD_ERR_SIZE_OOB);
        }

        if (e.size_bytes == 0)
            return static_cast<uint32_t>(LoaderError::LD_ERR_SIZE_OOB);

        return static_cast<uint32_t>(LoaderError::LD_OK);
    }

    /**
     * @brief Issue a single-beat AXI AR and wait for R data.
     *        Blocks (wait cycles) until complete.
     * @return true on success, false on AXI error.
     */
    bool axi_read_beat(uint32_t addr, sc_biguint<kMemAxiDataWidth>& data) {
        // AR phase
        m_mem_axi_ar_valid_o.write(true);
        m_mem_axi_ar_addr_o.write(addr);
        m_mem_axi_ar_len_o.write(0); // single beat
        m_mem_axi_r_ready_o.write(true);
        wait();

        while (!m_mem_axi_ar_ready_i.read()) {
            m_mem_axi_ar_valid_o.write(true);
            m_mem_axi_ar_addr_o.write(addr);
            m_mem_axi_ar_len_o.write(0);
            m_mem_axi_r_ready_o.write(true);
            wait();
        }
        m_mem_axi_ar_valid_o.write(false);

        // R phase
        m_mem_axi_r_ready_o.write(true);
        while (!m_mem_axi_r_valid_i.read()) {
            m_mem_axi_r_ready_o.write(true);
            wait();
        }
        data = m_mem_axi_r_data_i.read();
        bool ok = (m_mem_axi_r_resp_i.read().to_uint() == 0);
        m_mem_axi_r_ready_o.write(false);
        return ok;
    }

    /**
     * @brief Extract a 32-bit word from a wide AXI beat at byte offset.
     */
    static uint32_t extract_word(const sc_biguint<kMemAxiDataWidth>& beat,
                                 unsigned byte_off) {
        unsigned bit_lo = byte_off * 8;
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; ++b) {
            w |= (static_cast<uint32_t>(beat.range(bit_lo + b*8 + 7,
                                                    bit_lo + b*8).to_uint()) << (b*8));
        }
        return w;
    }

    // ========================================================================
    // Main sequential process
    // ========================================================================

    void seq_process() {
        reset_outputs();
        wait();

        LdState ld_state = LdState::LD_IDLE;
        unsigned entry_idx = 0;
        unsigned total_entries = 0;
        uint32_t manifest_base = 0;
        ManifestEntry cur_entry{};
        bool route_to_isram = false;
        uint32_t payload_dram_addr = 0;
        uint32_t payload_local_addr = 0;
        uint32_t payload_remain = 0;

        while (true) {
            // Default deassertions
            m_mem_axi_ar_valid_o.write(false);
            m_mem_axi_r_ready_o.write(false);
            isram_wr_en_o.write(false);
            dsram_wr_en_o.write(false);

            switch (ld_state) {
            // ----------------------------------------------------------------
            case LdState::LD_IDLE: {
                load_phase_o.write(false);
                busy_o.write(false);

                if (kick_i.read()) {
                    // Check manifest alignment
                    uint32_t msize = manifest_size_i.read().to_uint();
                    uint32_t maddr = manifest_addr_lo_i.read().to_uint();
                    if ((msize % kManifestEntryBytes != 0) || (maddr & 0x03)) {
                        err_code_o.write(static_cast<uint32_t>(
                            LoaderError::LD_ERR_MANIFEST_SIZE));
                        err_info_o.write(msize);
                        ld_state = LdState::LD_ERR;
                    } else {
                        manifest_base = maddr;
                        total_entries = msize / kManifestEntryBytes;
                        entry_idx = 0;
                        err_code_o.write(0);
                        err_info_o.write(0);
                        load_phase_o.write(true);
                        busy_o.write(true);
                        ld_state = LdState::LD_FETCH;
                        TRACE_EVENT("section_load", "Core", TRACE_BEGIN, 0, 0, "{}");
                        DEBUG_MSG("SectionLoader: kick entries=" << total_entries,
                                  DEBUG_LEVEL_CORE_COMPONENTS);
                    }
                }
                status_o.write(make_status(ld_state, entry_idx));
                break;
            }
            // ----------------------------------------------------------------
            case LdState::LD_FETCH: {
                load_phase_o.write(true);
                busy_o.write(true);

                if (entry_idx >= total_entries) {
                    ld_state = LdState::LD_DONE;
                    break;
                }

                // Read manifest entry words from DRAM
                // ManifestEntry = 8 words (32 bytes). May need multiple AXI beats.
                uint32_t entry_addr = manifest_base +
                                      entry_idx * kManifestEntryBytes;
                uint32_t words[8];
                unsigned words_read = 0;

                while (words_read < 8) {
                    sc_biguint<kMemAxiDataWidth> beat;
                    uint32_t beat_addr = entry_addr + words_read * 4;
                    if (!axi_read_beat(beat_addr, beat)) {
                        err_code_o.write(static_cast<uint32_t>(LoaderError::LD_ERR_AXI));
                        err_info_o.write(beat_addr);
                        ld_state = LdState::LD_ERR;
                        break;
                    }
                    // Extract words from this beat
                    unsigned words_per_beat = kBeatBytes / 4;
                    for (unsigned w = 0; w < words_per_beat && words_read < 8; ++w) {
                        words[words_read] = extract_word(beat, w * 4);
                        ++words_read;
                    }
                }
                if (ld_state == LdState::LD_ERR) break;

                // Parse ManifestEntry
                cur_entry.section_kind = static_cast<uint16_t>(words[0] & 0xFFFF);
                cur_entry.flags        = static_cast<uint16_t>(words[0] >> 16);
                cur_entry.dram_addr_lo = words[1];
                cur_entry.dram_addr_hi = words[2];
                cur_entry.local_addr   = words[3];
                cur_entry.size_bytes   = words[4];
                cur_entry.crc32        = words[5];
                cur_entry.attr0        = words[6];
                cur_entry.reserved     = words[7];

                ld_state = LdState::LD_ROUTE;
                status_o.write(make_status(ld_state, entry_idx));
                break;
            }
            // ----------------------------------------------------------------
            case LdState::LD_ROUTE: {
                uint32_t err = validate_entry(cur_entry);
                if (err != 0) {
                    err_code_o.write(err);
                    err_info_o.write(entry_idx);
                    ld_state = LdState::LD_ERR;
                    break;
                }
                route_to_isram = (cur_entry.section_kind ==
                                  static_cast<uint16_t>(SectionKind::CORE));
                payload_dram_addr  = cur_entry.dram_addr_lo;
                payload_local_addr = cur_entry.local_addr;
                payload_remain     = cur_entry.size_bytes;
                ld_state = LdState::LD_WRITE_LOCAL;
                status_o.write(make_status(ld_state, entry_idx));
                break;
            }
            // ----------------------------------------------------------------
            case LdState::LD_WRITE_LOCAL: {
                if (payload_remain == 0) {
                    ld_state = LdState::LD_VERIFY;
                    break;
                }

                // Read one AXI beat from DRAM
                sc_biguint<kMemAxiDataWidth> beat;
                if (!axi_read_beat(payload_dram_addr, beat)) {
                    err_code_o.write(static_cast<uint32_t>(LoaderError::LD_ERR_AXI));
                    err_info_o.write(payload_dram_addr);
                    ld_state = LdState::LD_ERR;
                    break;
                }

                // Write words into local SRAM
                unsigned words_in_beat = kBeatBytes / 4;
                for (unsigned w = 0; w < words_in_beat && payload_remain > 0; ++w) {
                    uint32_t word = extract_word(beat, w * 4);
                    uint32_t nbytes = (payload_remain >= 4) ? 4 : payload_remain;
                    uint32_t strb = (1u << nbytes) - 1;

                    if (route_to_isram) {
                        isram_wr_en_o.write(true);
                        isram_wr_addr_o.write(payload_local_addr);
                        isram_wr_data_o.write(word);
                        isram_wr_strb_o.write(strb);
                    } else {
                        dsram_wr_en_o.write(true);
                        dsram_wr_addr_o.write(payload_local_addr);
                        dsram_wr_data_o.write(word);
                        dsram_wr_strb_o.write(strb);
                    }
                    wait();
                    isram_wr_en_o.write(false);
                    dsram_wr_en_o.write(false);

                    payload_local_addr += 4;
                    payload_dram_addr  += 4;
                    payload_remain     -= nbytes;
                }
                status_o.write(make_status(ld_state, entry_idx));
                break;
            }
            // ----------------------------------------------------------------
            case LdState::LD_VERIFY: {
                // Simplified: CRC verification placeholder.
                // A full implementation would re-read and compute CRC32.
                // For ESL, skip CRC and advance.
                ++entry_idx;
                ld_state = LdState::LD_FETCH;
                status_o.write(make_status(ld_state, entry_idx));
                break;
            }
            // ----------------------------------------------------------------
            case LdState::LD_DONE: {
                load_phase_o.write(false);
                busy_o.write(false);
                done_o.write(true);
                TRACE_EVENT("section_load", "Core", TRACE_END, 0, 0, "{}");
                status_o.write(make_status(ld_state, entry_idx));
                // DEBUG_MSG("SectionLoader: all entries loaded (" << total_entries << ")",
                //           DEBUG_LEVEL_CORE_COMPONENTS);
                // Stay in DONE until next kick resets
                if (kick_i.read()) {
                    done_o.write(false);
                    ld_state = LdState::LD_IDLE;
                }
                break;
            }
            // ----------------------------------------------------------------
            case LdState::LD_ERR: {
                load_phase_o.write(false);
                busy_o.write(false);
                TRACE_EVENT("section_load", "Core", TRACE_END, 0, 0, "{}");
                status_o.write(make_status(ld_state, entry_idx));
                DEBUG_MSG("SectionLoader: error code="
                          << err_code_o.read().to_uint()
                          << " entry=" << entry_idx,
                          DEBUG_LEVEL_CORE_COMPONENTS);
                // Stay in ERR until next kick
                if (kick_i.read()) {
                    err_code_o.write(0);
                    ld_state = LdState::LD_IDLE;
                }
                break;
            }
            default:
                ld_state = LdState::LD_IDLE;
                break;
            }

            wait();
        }
    }
};

} // namespace core
} // namespace hybridacc
