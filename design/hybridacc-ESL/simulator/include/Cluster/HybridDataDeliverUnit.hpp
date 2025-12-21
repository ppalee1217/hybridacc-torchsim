#ifndef HYBRID_DATA_DELIVER_UNIT_HPP
#define HYBRID_DATA_DELIVER_UNIT_HPP

#include <systemc>
#include "Cluster/AddressGenerateUnit.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

class HybridDataDeliverUnit : public sc_module {
public:
    // IO Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // DMA / SRAM Interface
    // Data coming FROM SRAM/DMA
    sc_in<sc_biguint<256>> dma_in_data;
    sc_in<bool> dma_in_valid;
    sc_out<bool> dma_in_ready;

    // Request TO SRAM (Added based on architecture needs)
    sc_out<sc_uint<32>> sram_read_addr;
    sc_out<bool> sram_read_req;
    sc_in<bool> sram_read_ready; // SRAM ready to accept request

    // NoC Interface
    sc_out<sc_biguint<256>> noc_out_data;
    sc_out<sc_uint<10>> noc_out_addr; // Tag/Dest
    sc_out<bool> noc_out_valid;
    sc_in<bool> noc_out_ready;

    // MMIO Interface
    sc_in<sc_uint<32>> mmio_addr;
    sc_in<bool> mmio_write;
    sc_in<sc_uint<32>> mmio_wdata;
    sc_out<sc_uint<32>> mmio_rdata;

    // Internal Modules
    AddressGenerateUnit* w_agu;
    AddressGenerateUnit* i_agu;
    AddressGenerateUnit* a_agu;

    // Signals connecting to AGUs
    // Weight AGU
    sc_signal<sc_uint<32>> w_req_addr;
    sc_signal<sc_uint<32>> w_req_tag;
    sc_signal<bool> w_req_valid;
    sc_signal<bool> w_req_lock;
    sc_signal<bool> w_req_ready;
    sc_signal<sc_uint<32>> w_mmio_rdata;

    // Input AGU
    sc_signal<sc_uint<32>> i_req_addr;
    sc_signal<sc_uint<32>> i_req_tag;
    sc_signal<bool> i_req_valid;
    sc_signal<bool> i_req_lock;
    sc_signal<bool> i_req_ready;
    sc_signal<sc_uint<32>> i_mmio_rdata;

    // Acc AGU
    sc_signal<sc_uint<32>> a_req_addr;
    sc_signal<sc_uint<32>> a_req_tag;
    sc_signal<bool> a_req_valid;
    sc_signal<bool> a_req_lock;
    sc_signal<bool> a_req_ready;
    sc_signal<sc_uint<32>> a_mmio_rdata;

    // Arbiter Configuration
    sc_signal<sc_uint<32>> cfg_arb_ctrl; // 0x300

    // Tag FIFO to store tags of outstanding requests
    sc_fifo<sc_uint<10>> tag_fifo;

    // Arbiter State
    enum ArbiterState {
        ARB_IDLE,
        ARB_LOCKED_W,
        ARB_LOCKED_I,
        ARB_LOCKED_A
    };
    sc_signal<int> arb_state;

    SC_CTOR(HybridDataDeliverUnit) : tag_fifo(16) { // Depth 16 for outstanding requests
        // Instantiate AGUs
        w_agu = new AddressGenerateUnit("W_AGU");
        w_agu->clk(clk); w_agu->reset_n(reset_n);
        w_agu->mmio_addr(mmio_addr); w_agu->mmio_write(mmio_write); w_agu->mmio_wdata(mmio_wdata); w_agu->mmio_rdata(w_mmio_rdata);
        w_agu->req_addr(w_req_addr); w_agu->req_tag(w_req_tag); w_agu->req_valid(w_req_valid); w_agu->req_lock(w_req_lock); w_agu->req_ready(w_req_ready);

        i_agu = new AddressGenerateUnit("I_AGU");
        i_agu->clk(clk); i_agu->reset_n(reset_n);
        i_agu->mmio_addr(mmio_addr); i_agu->mmio_write(mmio_write); i_agu->mmio_wdata(mmio_wdata); i_agu->mmio_rdata(i_mmio_rdata);
        i_agu->req_addr(i_req_addr); i_agu->req_tag(i_req_tag); i_agu->req_valid(i_req_valid); i_agu->req_lock(i_req_lock); i_agu->req_ready(i_req_ready);

        a_agu = new AddressGenerateUnit("A_AGU");
        a_agu->clk(clk); a_agu->reset_n(reset_n);
        a_agu->mmio_addr(mmio_addr); a_agu->mmio_write(mmio_write); a_agu->mmio_wdata(mmio_wdata); a_agu->mmio_rdata(a_mmio_rdata);
        a_agu->req_addr(a_req_addr); a_agu->req_tag(a_req_tag); a_agu->req_valid(a_req_valid); a_agu->req_lock(a_req_lock); a_agu->req_ready(a_req_ready);

        SC_CTHREAD(arbiter_logic, clk.pos());
        reset_signal_is(reset_n, false);

        SC_CTHREAD(data_path_logic, clk.pos());
        reset_signal_is(reset_n, false);

        SC_METHOD(mmio_read_mux);
        sensitive << mmio_addr << w_mmio_rdata << i_mmio_rdata << a_mmio_rdata << cfg_arb_ctrl;

        SC_METHOD(mmio_write_logic);
        sensitive << clk.pos(); // Synchronous write for Arbiter config
    }

    void mmio_read_mux() {
        sc_uint<32> addr = mmio_addr.read();
        if (addr >= 0x000 && addr < 0x100) {
            mmio_rdata.write(w_mmio_rdata.read());
        } else if (addr >= 0x100 && addr < 0x200) {
            mmio_rdata.write(i_mmio_rdata.read());
        } else if (addr >= 0x200 && addr < 0x300) {
            mmio_rdata.write(a_mmio_rdata.read());
        } else if (addr == 0x300) {
            mmio_rdata.write(cfg_arb_ctrl.read());
        } else {
            mmio_rdata.write(0);
        }
    }

    void mmio_write_logic() {
        if (!reset_n.read()) {
            cfg_arb_ctrl.write(0);
        } else if (mmio_write.read()) {
            if (mmio_addr.read() == 0x300) {
                cfg_arb_ctrl.write(mmio_wdata.read());
            }
        }
    }

    void arbiter_logic() {
        // Reset
        w_req_ready.write(false);
        i_req_ready.write(false);
        a_req_ready.write(false);
        sram_read_req.write(false);
        sram_read_addr.write(0);
        arb_state.write(ARB_IDLE);

        wait();

        while (true) {
            // Default
            w_req_ready.write(false);
            i_req_ready.write(false);
            a_req_ready.write(false);
            sram_read_req.write(false);

            bool sram_ready = sram_read_ready.read();
            bool fifo_full = (tag_fifo.num_free() == 0);

            // Only arbitrate if SRAM is ready and we have space for the tag
            if (sram_ready && !fifo_full) {
                int state = arb_state.read();
                bool lock_en = cfg_arb_ctrl.read()[31]; // Assuming bit 31 is global lock enable or per-AGU lock is used
                // Actually AGU has its own lock signal.

                // Priority: W > I > A (Fixed)
                // Or Round Robin (Not implemented yet, sticking to Fixed for simplicity as per doc default)

                bool grant_w = false;
                bool grant_i = false;
                bool grant_a = false;

                if (state == ARB_LOCKED_W) {
                    if (w_req_valid.read()) grant_w = true;
                    else if (!w_req_lock.read()) arb_state.write(ARB_IDLE); // Lock released
                } else if (state == ARB_LOCKED_I) {
                    if (i_req_valid.read()) grant_i = true;
                    else if (!i_req_lock.read()) arb_state.write(ARB_IDLE);
                } else if (state == ARB_LOCKED_A) {
                    if (a_req_valid.read()) grant_a = true;
                    else if (!a_req_lock.read()) arb_state.write(ARB_IDLE);
                } else {
                    // IDLE - Arbitration
                    if (w_req_valid.read()) {
                        grant_w = true;
                        if (w_req_lock.read()) arb_state.write(ARB_LOCKED_W);
                    } else if (i_req_valid.read()) {
                        grant_i = true;
                        if (i_req_lock.read()) arb_state.write(ARB_LOCKED_I);
                    } else if (a_req_valid.read()) {
                        grant_a = true;
                        if (a_req_lock.read()) arb_state.write(ARB_LOCKED_A);
                    }
                }

                if (grant_w) {
                    w_req_ready.write(true);
                    sram_read_addr.write(w_req_addr.read());
                    sram_read_req.write(true);
                    tag_fifo.write(w_req_tag.read().range(9, 0)); // Store 10-bit tag
                } else if (grant_i) {
                    i_req_ready.write(true);
                    sram_read_addr.write(i_req_addr.read());
                    sram_read_req.write(true);
                    tag_fifo.write(i_req_tag.read().range(9, 0));
                } else if (grant_a) {
                    a_req_ready.write(true);
                    sram_read_addr.write(a_req_addr.read());
                    sram_read_req.write(true);
                    tag_fifo.write(a_req_tag.read().range(9, 0));
                }
            }

            wait();
        }
    }

    void data_path_logic() {
        // Reset
        dma_in_ready.write(false);
        noc_out_valid.write(false);
        noc_out_data.write(0);
        noc_out_addr.write(0);

        wait();

        while (true) {
            bool noc_ready = noc_out_ready.read();
            bool data_valid = dma_in_valid.read();
            bool tag_avail = (tag_fifo.num_available() > 0);

            // We are ready for DMA data if NoC is ready AND we have a tag waiting
            // (Assuming strict ordering: Data returns in same order as Requests)
            bool ready_for_data = noc_ready && tag_avail;
            dma_in_ready.write(ready_for_data);

            if (ready_for_data && data_valid) {
                sc_uint<10> tag;
                tag_fifo.read(tag); // Pop tag

                noc_out_data.write(dma_in_data.read());
                noc_out_addr.write(tag);
                noc_out_valid.write(true);
            } else {
                noc_out_valid.write(false);
            }

            wait();
        }
    }
};

} // namespace cluster
} // namespace hybridacc

#endif // HYBRID_DATA_DELIVER_UNIT_HPP
