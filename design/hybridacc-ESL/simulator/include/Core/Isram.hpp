#pragma once

/**
 * @file Isram.hpp
 * @brief cc_inst_ram — Instruction SRAM for the HACC Core Controller.
 *
 * Stores the `.hacc.core` firmware image.  Provides a single-cycle
 * instruction-fetch port for cc_core_mcu and a write port for
 * cc_section_loader during load phase.
 *
 * Internally wraps a cluster::SRAM<32,32> (latency=1, pipeline_depth=1)
 * as the storage back-end.  Loader writes go through the SRAM synchronous
 * write port; MCU instruction reads access the SRAM memory array
 * combinationally to match the pipeline's zero-wait fetch timing.
 *
 * @par Spec reference
 *   Core.md §6.2 cc_inst_ram
 */

#include <systemc>
#include <cstdint>
#include "Utils/utils.hpp"
#include "Core/Types.hpp"
#include "Utils/SRAM.hpp"

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

template <unsigned SRAM_BYTES = kIsramBytes>
SC_MODULE(Isram) {

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // --- MCU instruction-fetch port ---
    sc_in<bool>            mcu_im_valid_i;
    sc_in<sc_uint<32>>     mcu_im_addr_i;
    sc_out<sc_uint<32>>    mcu_im_rdata_o;

    // --- Section-loader write port ---
    sc_in<bool>            loader_wr_valid_i;
    sc_in<sc_uint<32>>     loader_wr_addr_i;
    sc_in<sc_uint<32>>     loader_wr_data_i;
    sc_in<sc_uint<4>>      loader_wr_strb_i;
    sc_out<bool>           loader_wr_ready_o;

    // --- Phase indicator ---
    sc_in<bool>            load_phase_i;

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(Isram);

    Isram(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          mcu_im_valid_i("mcu_im_valid_i"),
          mcu_im_addr_i("mcu_im_addr_i"),
          mcu_im_rdata_o("mcu_im_rdata_o"),
          loader_wr_valid_i("loader_wr_valid_i"),
          loader_wr_addr_i("loader_wr_addr_i"),
          loader_wr_data_i("loader_wr_data_i"),
          loader_wr_strb_i("loader_wr_strb_i"),
          loader_wr_ready_o("loader_wr_ready_o"),
          load_phase_i("load_phase_i"),
          sram_("sram", SRAM_BYTES, /*latency=*/1, /*pip_depth=*/1)
    {
        // ---- Bind SRAM clock / reset ----
        sram_.clk(clk);
        sram_.reset_n(reset_n);

        // ---- Bind SRAM synchronous write port ----
        sram_.write_en(sram_wr_en_);
        sram_.write_addr(sram_wr_addr_);
        sram_.write_data(sram_wr_data_);
        sram_.write_mask(sram_wr_mask_);

        // ---- Bind SRAM read-pipeline (unused — tied off) ----
        sram_.req_valid(sram_rd_req_valid_);
        sram_.req_addr(sram_rd_req_addr_);
        sram_.req_ready(sram_rd_req_ready_);
        sram_.resp_data(sram_rd_resp_data_);
        sram_.resp_valid(sram_rd_resp_valid_);
        sram_.resp_ready(sram_rd_resp_ready_);
        sram_rd_resp_ready_.write(true);

        // ---- Combinational processes ----
        SC_METHOD(comb_write_bridge);
        sensitive << load_phase_i << loader_wr_valid_i
                  << loader_wr_addr_i << loader_wr_data_i << loader_wr_strb_i;

        SC_METHOD(comb_read_process);
        sensitive << mcu_im_valid_i << mcu_im_addr_i << load_phase_i;

        SC_METHOD(comb_loader_ready);
        sensitive << load_phase_i;
    }

private:
    // ========================================================================
    // SRAM back-end
    // ========================================================================

    cluster::SRAM<32, 32> sram_;

    // ---- SRAM write-port signals ----
    sc_signal<bool>           sram_wr_en_;
    sc_signal<sc_uint<32>>    sram_wr_addr_;
    sc_signal<sc_biguint<32>> sram_wr_data_;
    sc_signal<sc_uint<4>>     sram_wr_mask_;

    // ---- SRAM read-pipeline signals (unused, tied off) ----
    sc_signal<bool>           sram_rd_req_valid_;
    sc_signal<sc_uint<32>>    sram_rd_req_addr_;
    sc_signal<bool>           sram_rd_req_ready_;
    sc_signal<sc_biguint<32>> sram_rd_resp_data_;
    sc_signal<bool>           sram_rd_resp_valid_;
    sc_signal<bool>           sram_rd_resp_ready_;

    // ========================================================================
    // Processes
    // ========================================================================

    /**
     * @brief Bridge loader write request → SRAM synchronous write port.
     */
    void comb_write_bridge() {
        const bool do_write = load_phase_i.read() && loader_wr_valid_i.read();
        sram_wr_en_.write(do_write);
        sram_wr_addr_.write(loader_wr_addr_i.read());
        sram_wr_data_.write(loader_wr_data_i.read().to_uint());
        sram_wr_mask_.write(loader_wr_strb_i.read());
    }

    /**
     * @brief Combinational instruction read from SRAM memory array.
     */
    void comb_read_process() {
        if (!load_phase_i.read() && mcu_im_valid_i.read()) {
            const uint32_t byte_addr = mcu_im_addr_i.read().to_uint();
            const uint32_t base = byte_addr & (SRAM_BYTES - 1);
            uint32_t val = 0;
            for (unsigned b = 0; b < 4; ++b) {
                if (base + b < SRAM_BYTES)
                    val |= static_cast<uint32_t>(sram_.mem[base + b]) << (b * 8);
            }
            mcu_im_rdata_o.write(val);
        } else {
            mcu_im_rdata_o.write(0);
        }
    }

    /**
     * @brief Loader-ready is always asserted during load phase.
     */
    void comb_loader_ready() {
        loader_wr_ready_o.write(load_phase_i.read());
    }

public:
    /** Read one byte from the backing SRAM array (for data-port access). */
    uint8_t read_byte(uint32_t byte_addr) const {
        const uint32_t off = byte_addr & (SRAM_BYTES - 1);
        return sram_.mem[off];
    }
};

} // namespace core
} // namespace hybridacc
