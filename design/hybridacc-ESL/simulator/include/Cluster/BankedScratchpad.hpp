#pragma once

#include <systemc>
#include <vector>
#include <iostream>
#include <iomanip>
#include "Cluster/SRAM.hpp"
#include "utils.hpp" // For VRDIF/VRDOF

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

// BankedScratchpad: A collection of SRAM banks with exclusive access controls
// Features:
// 1. Multiple SRAM Banks.
// 2. 3 Dedicated NoC Ports (NoC0 Read, NoC1 Read, NoC2 Write).
//    - These ports can be mapped to specific banks via `noc_bank_sel` signals.
//    - When a bank is mapped to a NoC port, it is considered "LOCKED".
// 3. 1 DMA Port (Read/Write)
//    - Accesses banks based on address (Upper bits select bank).
//    - Accessing a LOCKED bank triggers `error_intr`.
// 4. Data Width: 256 bits.

template <unsigned NUM_BANKS = 8, unsigned BANK_ADDR_WIDTH = 12, unsigned DATA_WIDTH = 256>
SC_MODULE(BankedScratchpad) {
public:
    // --- Ports ---
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Control Configuration (Selects which bank is connected/locked to each NoC port)
    // Value = Bank Index (0 to NUM_BANKS-1). If >= NUM_BANKS, port is disabled/unlocked.
    sc_in<unsigned int> noc0_bank_sel; // Read Port
    sc_in<unsigned int> noc1_bank_sel; // Read Port
    sc_in<unsigned int> noc2_bank_sel; // Write Port

    // Interrupt
    sc_out<bool> error_intr;

    // --- NoC Interfaces ---
    // NoC0 (Read Only)
    VRDIF<sc_uint<BANK_ADDR_WIDTH>> noc0_req;
    VRDOF<sc_biguint<DATA_WIDTH>>   noc0_resp;

    // NoC1 (Read Only)
    VRDIF<sc_uint<BANK_ADDR_WIDTH>> noc1_req;
    VRDOF<sc_biguint<DATA_WIDTH>>   noc1_resp;

    // NoC2 (Write Only)
    // To use VRDIF, we need a struct for the write request payload {addr, data, mask}
    struct WriteReq {
        sc_uint<BANK_ADDR_WIDTH> addr;
        sc_biguint<DATA_WIDTH>   data;
        sc_uint<DATA_WIDTH/8>    mask;

        bool operator==(const WriteReq& other) const {
             return addr == other.addr && data == other.data && mask == other.mask;
        }
        friend std::ostream& operator<<(std::ostream& os, const WriteReq& w) {
             os << "WReq(A=" << w.addr << ")"; return os;
        }
    };
    VRDIF<WriteReq> noc2_req;

    // --- DMA Interface (Read/Write, Global Address Space) ---
    // Combined request struct for Read/Write
    struct DMAReq {
        sc_uint<32>              addr;
        bool                     write_en;
        sc_biguint<DATA_WIDTH>   write_data;
        sc_uint<DATA_WIDTH/8>    write_mask;

        bool operator==(const DMAReq& other) const {
             return addr == other.addr && write_en == other.write_en;
        }
        friend std::ostream& operator<<(std::ostream& os, const DMAReq& w) {
             os << "DMAReq(A=" << w.addr << ", WR=" << w.write_en << ")"; return os;
        }
    };
    VRDIF<DMAReq>                 dma_req;
    VRDOF<sc_biguint<DATA_WIDTH>> dma_resp;

    // --- Internal Components ---
    std::vector<SRAM<DATA_WIDTH, BANK_ADDR_WIDTH>*> banks;

    // Signals to connect to SRAM banks
    // We need vectors of signals. Using sc_vector or raw pointers.
    // Given usage, raw pointers to signals/modules is easiest for dynamic banks loop.
    std::vector<sc_signal<sc_uint<BANK_ADDR_WIDTH>>*> s_bank_req_addr;
    std::vector<sc_signal<bool>*>                     s_bank_req_valid;
    std::vector<sc_signal<bool>*>                     s_bank_req_ready;
    std::vector<sc_signal<sc_biguint<DATA_WIDTH>>*>   s_bank_resp_data;
    std::vector<sc_signal<bool>*>                     s_bank_resp_valid;
    std::vector<sc_signal<bool>*>                     s_bank_resp_ready;
    std::vector<sc_signal<bool>*>                     s_bank_write_en;
    std::vector<sc_signal<sc_uint<BANK_ADDR_WIDTH>>*> s_bank_write_addr;
    std::vector<sc_signal<sc_biguint<DATA_WIDTH>>*>   s_bank_write_data;
    std::vector<sc_signal<sc_uint<DATA_WIDTH/8>>*>    s_bank_write_mask;

    // Constructor
    BankedScratchpad(sc_module_name name) :
        sc_module(name),
        noc0_req("noc0_req"),
        noc0_resp("noc0_resp"),
        noc1_req("noc1_req"),
        noc1_resp("noc1_resp"),
        noc2_req("noc2_req"),
        dma_req("dma_req"),
        dma_resp("dma_resp")
    {
        // Instantiate Banks
        for (unsigned i = 0; i < NUM_BANKS; ++i) {
            std::string s_name = "SRAM_Bank_" + std::to_string(i);
            SRAM<DATA_WIDTH, BANK_ADDR_WIDTH>* bank = new SRAM<DATA_WIDTH, BANK_ADDR_WIDTH>(s_name.c_str(), (1<<BANK_ADDR_WIDTH) * (DATA_WIDTH/8)); // Total bytes per bank
            banks.push_back(bank);

            // Create signals
            s_bank_req_addr.push_back(new sc_signal<sc_uint<BANK_ADDR_WIDTH>>("req_addr_" + std::to_string(i)));
            s_bank_req_valid.push_back(new sc_signal<bool>("req_valid_" + std::to_string(i)));
            s_bank_req_ready.push_back(new sc_signal<bool>("req_ready_" + std::to_string(i)));
            s_bank_resp_data.push_back(new sc_signal<sc_biguint<DATA_WIDTH>>("resp_data_" + std::to_string(i)));
            s_bank_resp_valid.push_back(new sc_signal<bool>("resp_valid_" + std::to_string(i)));
            s_bank_resp_ready.push_back(new sc_signal<bool>("resp_ready_" + std::to_string(i)));
            s_bank_write_en.push_back(new sc_signal<bool>("write_en_" + std::to_string(i)));
            s_bank_write_addr.push_back(new sc_signal<sc_uint<BANK_ADDR_WIDTH>>("write_addr_" + std::to_string(i)));
            s_bank_write_data.push_back(new sc_signal<sc_biguint<DATA_WIDTH>>("write_data_" + std::to_string(i)));
            s_bank_write_mask.push_back(new sc_signal<sc_uint<DATA_WIDTH/8>>("write_mask_" + std::to_string(i)));

            // Bind signals
            bank->clk(clk);
            bank->reset_n(reset_n);
            bank->req_addr(*s_bank_req_addr[i]);
            bank->req_valid(*s_bank_req_valid[i]);
            bank->req_ready(*s_bank_req_ready[i]);
            bank->resp_data(*s_bank_resp_data[i]);
            bank->resp_valid(*s_bank_resp_valid[i]);
            bank->resp_ready(*s_bank_resp_ready[i]);
            bank->write_en(*s_bank_write_en[i]);
            bank->write_addr(*s_bank_write_addr[i]);
            bank->write_data(*s_bank_write_data[i]);
            bank->write_mask(*s_bank_write_mask[i]);
        }

        SC_METHOD(ctrl_logic);
        sensitive << clk.pos() << reset_n << noc0_bank_sel << noc1_bank_sel << noc2_bank_sel;
        // Sensitivity for interface signals
        sensitive << dma_req.valid_in << dma_req.data_in << dma_resp.ready_in;
        sensitive << noc0_req.valid_in << noc0_req.data_in << noc0_resp.ready_in;
        sensitive << noc1_req.valid_in << noc1_req.data_in << noc1_resp.ready_in;
        sensitive << noc2_req.valid_in << noc2_req.data_in;

        // Make sensitive to all bank ready/valid signals
        for (unsigned i = 0; i < NUM_BANKS; ++i) {
            sensitive << *s_bank_req_ready[i] << *s_bank_resp_valid[i] << *s_bank_resp_data[i];
        }
    }

    // Helper: Address decoding
    // Bit mapping: [ 31 ... | BankIdx | Offset ]
    // Assuming Offset width is log2(Bank Size in Bytes) ? Or word addressed?
    // SRAM module uses "word address" (index of element). But exposed as byte addressable interface usually?
    // Let's check SRAM.hpp: `req_addr` is `sc_uint<ADDR_WIDTH>`, `read_word(uint32_t byte_addr)`.
    // It seems SRAM expects byte address passed to `read_word`.
    // But `req_addr` is passed directly. The new SRAM implementation treats `req_addr` as a generic address.
    // Wait, the new SRAM implementation takes `req_addr` and stores it in `pipeline`.
    // Then calls `read_word(pipeline.front().addr)`. `read_word` expects `byte_addr`.
    // So `req_addr` acts as Byte Address.
    // Our BANK_ADDR_WIDTH should be enough to cover the Bytes.

    // Bank Selection:
    // Global Address = (BankID << BANK_ADDR_WIDTH) | Offset
    unsigned get_bank_id(uint32_t addr) {
        return (addr >> BANK_ADDR_WIDTH) & (NUM_BANKS - 1); // Simple masking if POT
    }

    uint32_t get_bank_offset(uint32_t addr) {
        return addr & ((1<<BANK_ADDR_WIDTH) - 1);
    }

    // Control Logic / Crossbar (Combinational)
    void ctrl_logic() {
        bool reset_active = !reset_n.read();

        // Default outputs
        error_intr.write(false);
        dma_req.ready_out.write(false);
        dma_resp.valid_out.write(false);
        dma_resp.data_out.write(0);

        noc0_req.ready_out.write(false);
        noc0_resp.valid_out.write(false);
        noc0_resp.data_out.write(0);

        noc1_req.ready_out.write(false);
        noc1_resp.valid_out.write(false);
        noc1_resp.data_out.write(0);

        noc2_req.ready_out.write(false);

        // Reset state for bank inputs
        if (reset_active) {
            for (unsigned i = 0; i < NUM_BANKS; ++i) {
                s_bank_req_valid[i]->write(false);
                s_bank_write_en[i]->write(false);
                s_bank_resp_ready[i]->write(false);
            }
            return;
        }

        // --- Decode Selections ---
        unsigned sel0 = noc0_bank_sel.read();
        unsigned sel1 = noc1_bank_sel.read();
        unsigned sel2 = noc2_bank_sel.read();

        // Lock Status (Implicit): Banks pointed to by sel0, sel1, sel2 are locked.

        // --- DMA Handling ---
        bool dma_error = false;
        DMAReq dma_in = dma_req.data_in.read();

        // Decode DMA target
        unsigned dma_bank_idx = get_bank_id(dma_in.addr);
        uint32_t dma_offset = get_bank_offset(dma_in.addr);

        // Check Lock
        bool bank_locked_by_noc0 = (sel0 < NUM_BANKS) && (sel0 == dma_bank_idx);
        bool bank_locked_by_noc1 = (sel1 < NUM_BANKS) && (sel1 == dma_bank_idx);
        bool bank_locked_by_noc2 = (sel2 < NUM_BANKS) && (sel2 == dma_bank_idx);

        if (dma_req.valid_in.read()) {
            if (dma_bank_idx >= NUM_BANKS) {
                // Out of bound access - treat as error or ignore
                dma_error = true;
            } else if (bank_locked_by_noc0 || bank_locked_by_noc1 || bank_locked_by_noc2) {
                // LOCKED! Access Denied.
                dma_error = true;
            }
        }

        if (dma_error) {
            error_intr.write(true); // Raise interrupt
            // Eat the request to prevent stalling? Or just stall?
            // Usually error response is immediate.
            dma_req.ready_out.write(true); // Accept it to clear it
            // No response valid (or error response if protocol supported)
        }

        // --- Multiplexing Logic for each Bank ---
        for (unsigned i = 0; i < NUM_BANKS; ++i) {
            bool is_noc0 = (sel0 == i);
            bool is_noc1 = (sel1 == i);
            bool is_noc2 = (sel2 == i); // Write port

            // Priority / Muxing:
            // If Locked by NoC, NoC gets exclusive access.
            // If Not Locked, DMA gets access.

            // Read Path (Mux inputs to Bank `req` port)
            // Bank `req` is for READ requests in SRAM.hpp.
            // Bank `write` is independent port.

            // -- READ REQUEST MUX --
            if (is_noc0) {
                // NoC0 Read
                s_bank_req_addr[i]->write(noc0_req.data_in.read());
                s_bank_req_valid[i]->write(noc0_req.valid_in.read());
                // Routing ready back
                if (noc0_req.valid_in.read()) noc0_req.ready_out.write(s_bank_req_ready[i]->read());

            } else if (is_noc1) {
                // NoC1 Read
                s_bank_req_addr[i]->write(noc1_req.data_in.read());
                s_bank_req_valid[i]->write(noc1_req.valid_in.read());
                if (noc1_req.valid_in.read()) noc1_req.ready_out.write(s_bank_req_ready[i]->read());

            } else if (!is_noc2 && !dma_error && dma_req.valid_in.read() && (dma_bank_idx == i) && !dma_in.write_en) {
                // DMA Read (Only if not locked by ANY NoC port)
                // Note: is_noc2 check is redundant if locked logic works, but safe.
                s_bank_req_addr[i]->write(dma_offset);
                s_bank_req_valid[i]->write(true);
                dma_req.ready_out.write(s_bank_req_ready[i]->read());

            } else {
                // Idle
                s_bank_req_valid[i]->write(false);
                s_bank_req_addr[i]->write(0);
            }

            // -- WRITE PATH MUX --
            if (is_noc2) {
                // NoC2 Write
                WriteReq noc2_in = noc2_req.data_in.read();
                s_bank_write_en[i]->write(noc2_req.valid_in.read());
                s_bank_write_addr[i]->write(noc2_in.addr);
                s_bank_write_data[i]->write(noc2_in.data);
                s_bank_write_mask[i]->write(noc2_in.mask);
                if (noc2_req.valid_in.read()) noc2_req.ready_out.write(true); // SRAM write is always ready in valid cycle (sync)

            } else if (!is_noc0 && !is_noc1 && !dma_error && dma_req.valid_in.read() && (dma_bank_idx == i) && dma_in.write_en) {
                // DMA Write
                s_bank_write_en[i]->write(true);
                s_bank_write_addr[i]->write(dma_offset);
                s_bank_write_data[i]->write(dma_in.write_data);
                s_bank_write_mask[i]->write(dma_in.write_mask);
                dma_req.ready_out.write(true); // Sync write

            } else {
                s_bank_write_en[i]->write(false);
                s_bank_write_addr[i]->write(0);
            }

            // -- RESPONSE PATH DEMUX -- (Routing data from Bank -> Output)
            bool resp_avail = s_bank_resp_valid[i]->read();
            if (resp_avail) {
                if (is_noc0) {
                    noc0_resp.valid_out.write(true);
                    noc0_resp.data_out.write(s_bank_resp_data[i]->read());
                    s_bank_resp_ready[i]->write(noc0_resp.ready_in.read());
                } else if (is_noc1) {
                    noc1_resp.valid_out.write(true);
                    noc1_resp.data_out.write(s_bank_resp_data[i]->read());
                    s_bank_resp_ready[i]->write(noc1_resp.ready_in.read());
                } else if (!is_noc2) { // Assuming if NoC2 (write) is active, no reads happen.
                     // DMA Response
                     dma_resp.valid_out.write(true);
                     dma_resp.data_out.write(s_bank_resp_data[i]->read());
                     s_bank_resp_ready[i]->write(dma_resp.ready_in.read());
                } else {
                     // Should not happen (Bank active but no owner?)
                     s_bank_resp_ready[i]->write(true); // Drain garbage
                }
            } else {
                s_bank_resp_ready[i]->write(false);
            }
        }
    }
};

} // namespace cluster
} // namespace hybridacc
