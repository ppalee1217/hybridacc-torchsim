#include <systemc>
#include <iostream>
#include <iomanip>
#include "Cluster/HybridDataDeliverUnit.hpp"

using namespace sc_core;
using namespace sc_dt;
using namespace hybridacc::cluster;

// Mock SRAM Module
class MockSRAM : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Request Interface
    sc_in<sc_uint<32>> req_addr;
    sc_in<bool> req_valid;
    sc_out<bool> req_ready;

    // Response Interface
    sc_out<sc_biguint<256>> resp_data;
    sc_out<bool> resp_valid;
    sc_in<bool> resp_ready;

    // Internal Memory
    sc_uint<32> memory[1024];

    // Pipeline for latency simulation
    struct Request {
        sc_uint<32> addr;
        int cycles_left;
    };
    std::vector<Request> pipeline;
    const int LATENCY = 5;

    SC_CTOR(MockSRAM) {
        SC_CTHREAD(logic, clk.pos());
        reset_signal_is(reset_n, false);

        // Init memory
        for(int i=0; i<1024; i++) memory[i] = i;
    }

    void logic() {
        req_ready.write(false);
        resp_valid.write(false);
        resp_data.write(0);
        pipeline.clear();

        wait();

        while(true) {
            // Accept Request
            req_ready.write(true);
            if (req_valid.read()) {
                Request r;
                r.addr = req_addr.read();
                r.cycles_left = LATENCY;
                pipeline.push_back(r);
                // std::cout << "@" << sc_time_stamp() << " SRAM: Read Req Addr=" << r.addr << std::endl;
            }

            // Process Pipeline
            bool output_valid = false;
            sc_biguint<256> output_data = 0;

            for (auto it = pipeline.begin(); it != pipeline.end(); ) {
                it->cycles_left--;
                if (it->cycles_left == 0) {
                    if (!output_valid) { // Only one output per cycle
                        output_valid = true;
                        // Construct dummy data: {addr, addr, ...}
                        sc_uint<32> val = memory[it->addr % 1024];
                        for(int i=0; i<8; i++)
                            output_data.range(32*i+31, 32*i) = val;

                        it = pipeline.erase(it);
                    } else {
                        it->cycles_left++; // Stall this one
                        ++it;
                    }
                } else {
                    ++it;
                }
            }

            // Drive Output
            if (output_valid) {
                resp_valid.write(true);
                resp_data.write(output_data);
                // Wait for ready if needed (simple model assumes always ready or handles backpressure next cycle)
                // In this simple model, if HDDU is not ready, we drop data or stall.
                // Real SRAM would hold. Let's assume HDDU is always ready if we respect pipeline.
                // But HDDU asserts dma_in_ready based on NoC.
                // So we should check resp_ready.
                if (!resp_ready.read()) {
                    // Stall! (Not implemented in this simple mock, assuming test won't overflow)
                    std::cout << "WARNING: SRAM Output Stalled!" << std::endl;
                }
            } else {
                resp_valid.write(false);
            }

            wait();
        }
    }
};

// Mock NoC Module
class MockNoC : public sc_module {
public:
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    sc_in<sc_biguint<256>> data_in;
    sc_in<sc_uint<10>> addr_in;
    sc_in<bool> valid_in;
    sc_out<bool> ready_out;

    int received_count;

    SC_CTOR(MockNoC) {
        SC_CTHREAD(logic, clk.pos());
        reset_signal_is(reset_n, false);
    }

    void logic() {
        ready_out.write(false);
        received_count = 0;
        wait();

        while(true) {
            ready_out.write(true); // Always ready
            if (valid_in.read()) {
                received_count++;
                std::cout << "@" << sc_time_stamp() << " NoC: Recv Pkt Tag=" << addr_in.read()
                          << " Data=" << std::hex << data_in.read() << std::dec << std::endl;
            }
            wait();
        }
    }
};

int sc_main(int argc, char* argv[]) {
    sc_clock clk("clk", 10, SC_NS);
    sc_signal<bool> reset_n;

    // Signals
    sc_signal<sc_biguint<256>> dma_data;
    sc_signal<bool> dma_valid;
    sc_signal<bool> dma_ready;

    sc_signal<sc_uint<32>> sram_addr;
    sc_signal<bool> sram_req;
    sc_signal<bool> sram_ready;

    sc_signal<sc_biguint<256>> noc_data;
    sc_signal<sc_uint<10>> noc_addr;
    sc_signal<bool> noc_valid;
    sc_signal<bool> noc_ready;

    sc_signal<sc_uint<32>> mmio_addr;
    sc_signal<bool> mmio_write;
    sc_signal<sc_uint<32>> mmio_wdata;
    sc_signal<sc_uint<32>> mmio_rdata;

    // Modules
    HybridDataDeliverUnit hddu("HDDU");
    MockSRAM sram("SRAM");
    MockNoC noc("NoC");

    // Connections
    hddu.clk(clk); hddu.reset_n(reset_n);
    hddu.dma_in_data(dma_data); hddu.dma_in_valid(dma_valid); hddu.dma_in_ready(dma_ready);
    hddu.sram_read_addr(sram_addr); hddu.sram_read_req(sram_req); hddu.sram_read_ready(sram_ready);
    hddu.noc_out_data(noc_data); hddu.noc_out_addr(noc_addr); hddu.noc_out_valid(noc_valid); hddu.noc_out_ready(noc_ready);
    hddu.mmio_addr(mmio_addr); hddu.mmio_write(mmio_write); hddu.mmio_wdata(mmio_wdata); hddu.mmio_rdata(mmio_rdata);

    sram.clk(clk); sram.reset_n(reset_n);
    sram.req_addr(sram_addr); sram.req_valid(sram_req); sram.req_ready(sram_ready);
    sram.resp_data(dma_data); sram.resp_valid(dma_valid); sram.resp_ready(dma_ready);

    noc.clk(clk); noc.reset_n(reset_n);
    noc.data_in(noc_data); noc.addr_in(noc_addr); noc.valid_in(noc_valid); noc.ready_out(noc_ready);

    // Trace
    sc_trace_file *tf = sc_create_vcd_trace_file("hddu_trace");
    sc_trace(tf, clk, "clk");
    sc_trace(tf, reset_n, "reset_n");
    sc_trace(tf, sram_req, "sram_req");
    sc_trace(tf, sram_addr, "sram_addr");
    sc_trace(tf, noc_valid, "noc_valid");
    sc_trace(tf, noc_addr, "noc_addr");
    sc_trace(tf, hddu.arb_state, "arb_state");

    // Simulation
    std::cout << "Starting Simulation..." << std::endl;

    // Reset
    reset_n = 0;
    mmio_write = 0;
    sc_start(20, SC_NS);
    reset_n = 1;
    sc_start(20, SC_NS);

    // Helper to write MMIO
    auto mmio_wr = [&](uint32_t addr, uint32_t data) {
        mmio_addr = addr;
        mmio_wdata = data;
        mmio_write = 1;
        sc_start(10, SC_NS);
        mmio_write = 0;
        sc_start(10, SC_NS);
    };

    // Configure Weight AGU (0x000 - 0x0FF)
    // Loop 0: 4 iterations, stride 1
    mmio_wr(0x000, 0x1000); // Base Addr
    mmio_wr(0x008, 0x00010004); // Iter 0=4, Iter 1=1
    mmio_wr(0x010, 1); // Stride 0
    mmio_wr(0x040, 0x10); // Tag Base
    mmio_wr(0x044, 1); // Tag Stride
    mmio_wr(0x04C, 0); // Tag Loop = 0 (Change tag every inner loop)

    // Configure Input AGU (0x100 - 0x1FF)
    // Loop 0: 4 iterations, stride 1
    mmio_wr(0x100, 0x2000); // Base Addr
    mmio_wr(0x108, 0x00010004); // Iter 0=4, Iter 1=1
    mmio_wr(0x110, 1); // Stride 0
    mmio_wr(0x140, 0x20); // Tag Base
    mmio_wr(0x144, 1); // Tag Stride
    mmio_wr(0x14C, 0); // Tag Loop = 0

    // Start AGUs
    std::cout << "Starting AGUs..." << std::endl;
    mmio_wr(0x020, 1); // Start W_AGU
    mmio_wr(0x120, 1); // Start I_AGU

    // Run for some time
    sc_start(500, SC_NS);

    std::cout << "Simulation Finished." << std::endl;
    sc_close_vcd_trace_file(tf);

    return 0;
}
