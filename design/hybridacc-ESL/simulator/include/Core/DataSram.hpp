#pragma once

/**
 * @file DataSram.hpp
 * @brief cc_data_ram — Unified local data SRAM for the HACC Core Controller.
 *
 * Holds all non-.hacc.core sections: descriptor/payload, software stack,
 * event/debug.  Provides a 32-bit MCU load/store port and a loader write
 * port.
 *
 * Internally wraps a cluster::SRAM<32,32> (latency=1, pipeline_depth=1)
 * as the storage back-end.  All writes (loader and MCU store) go through
 * the SRAM synchronous write port; MCU loads read the SRAM memory array
 * combinationally.
 *
 * @par Spec reference
 *   Core.md §6.3 – §6.4  cc_data_ram
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

template <unsigned SRAM_BYTES = kDataSramBytes>
SC_MODULE(DataSram) {

    // ========================================================================
    // Ports
    // ========================================================================

    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // --- MCU 32-bit load/store port ---
    sc_in<bool>         mcu_dm_valid_i;
    sc_in<bool>         mcu_dm_write_i;  ///< 1=store, 0=load
    sc_in<sc_uint<32>>  mcu_dm_addr_i;
    sc_in<sc_uint<32>>  mcu_dm_wdata_i;
    sc_in<sc_uint<4>>   mcu_dm_wstrb_i;
    sc_out<sc_uint<32>> mcu_dm_rdata_o;

    // --- Loader write port ---
    sc_in<bool>         loader_wr_valid_i;
    sc_in<sc_uint<32>>  loader_wr_addr_i;
    sc_in<sc_uint<32>>  loader_wr_data_i;
    sc_in<sc_uint<4>>   loader_wr_strb_i;
    sc_out<bool>        loader_wr_ready_o;

    sc_in<bool>         load_phase_i;

    // ========================================================================
    // Constructor
    // ========================================================================

    SC_HAS_PROCESS(DataSram);

    DataSram(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          mcu_dm_valid_i("mcu_dm_valid_i"),
          mcu_dm_write_i("mcu_dm_write_i"),
          mcu_dm_addr_i("mcu_dm_addr_i"),
          mcu_dm_wdata_i("mcu_dm_wdata_i"),
          mcu_dm_wstrb_i("mcu_dm_wstrb_i"),
          mcu_dm_rdata_o("mcu_dm_rdata_o"),
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
        sensitive << load_phase_i
                  << loader_wr_valid_i << loader_wr_addr_i
                  << loader_wr_data_i  << loader_wr_strb_i
                  << mcu_dm_valid_i << mcu_dm_write_i
                  << mcu_dm_addr_i << mcu_dm_wdata_i << mcu_dm_wstrb_i;

        SC_METHOD(comb_rdata_process);
        sensitive << mcu_dm_valid_i << mcu_dm_write_i
                  << mcu_dm_addr_i << load_phase_i;

        SC_METHOD(comb_loader_ready);
        sensitive << load_phase_i;
    }

    void load_bytes(uint32_t byte_addr, const uint8_t* data, uint32_t size) {
        for (uint32_t i = 0; i < size; ++i) {
            const uint32_t off = (byte_addr + i) & (SRAM_BYTES - 1);
            sram_.mem[off] = data[i];
        }
    }

    /** Debug helper – read one byte from the backing SRAM array. */
    uint8_t read_byte(uint32_t byte_addr) const {
        const uint32_t off = byte_addr & (SRAM_BYTES - 1);
        return sram_.mem[off];
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
     * @brief Mux loader writes (load phase) and MCU stores (run phase)
     *        onto the SRAM synchronous write port.
     */
    void comb_write_bridge() {
        bool do_write = false;
        uint32_t addr = 0, wdata = 0, strb = 0;

        if (load_phase_i.read()) {
            if (loader_wr_valid_i.read()) {
                do_write = true;
                addr  = loader_wr_addr_i.read().to_uint();
                wdata = loader_wr_data_i.read().to_uint();
                strb  = loader_wr_strb_i.read().to_uint();
            }
        } else {
            if (mcu_dm_valid_i.read() && mcu_dm_write_i.read()) {
                do_write = true;
                addr  = mcu_dm_addr_i.read().to_uint();
                wdata = mcu_dm_wdata_i.read().to_uint();
                strb  = mcu_dm_wstrb_i.read().to_uint();
            }
        }

        sram_wr_en_.write(do_write);
        sram_wr_addr_.write(addr);
        sram_wr_data_.write(static_cast<sc_biguint<32>>(wdata));
        sram_wr_mask_.write(strb);
    }

    /**
     * @brief Combinational MCU load read — returns data from SRAM memory.
     */
    void comb_rdata_process() {
        if (!load_phase_i.read() && mcu_dm_valid_i.read() &&
            !mcu_dm_write_i.read()) {
            const uint32_t byte_addr = mcu_dm_addr_i.read().to_uint();
            const uint32_t base = (byte_addr & ~3u) & (SRAM_BYTES - 1);
            uint32_t val = 0;
            for (unsigned b = 0; b < 4; ++b) {
                if (base + b < SRAM_BYTES)
                    val |= static_cast<uint32_t>(sram_.mem[base + b]) << (b * 8);
            }
            mcu_dm_rdata_o.write(val);
        } else {
            mcu_dm_rdata_o.write(0);
        }
    }

    /**
     * @brief Loader-ready is always asserted during load phase.
     */
    void comb_loader_ready() {
        loader_wr_ready_o.write(load_phase_i.read());
    }
};

} // namespace core
} // namespace hybridacc
