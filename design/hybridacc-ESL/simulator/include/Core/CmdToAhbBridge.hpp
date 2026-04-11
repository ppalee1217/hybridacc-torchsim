/**
 * @file CmdToAhbBridge.hpp
 * @brief Bridge from CoreController simple cmd protocol to AHB-Lite master.
 *
 * CoreController CmdFabric uses a single-cycle valid/ack protocol:
 *   - cl_cmd_req_valid_o, cl_cmd_req_write_o, cl_cmd_req_addr_o, cl_cmd_req_wdata_o
 *   - cl_cmd_resp_valid_i, cl_cmd_resp_rdata_i
 *
 * ComputeCluster expects AHB-Lite slave protocol:
 *   - hsel, haddr, hwrite, htrans, hsize, hburst, hprot, hready_i, hwdata
 *   - hready_o, hresp, hrdata
 *
 * The bridge translates between these two protocols.
 *
 * AHB-Lite timing:
 *   Cycle 0: Address Phase — assert hsel, haddr, hwrite, htrans=NONSEQ(2)
 *   Cycle 1: Data Phase — hwdata valid for writes, read data returned via hrdata
 *   During read: ComputeCluster inserts 1 wait state (hready_o=0 on read data phase)
 */

#pragma once

#include <systemc>

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace core {

SC_MODULE(CmdToAhbBridge) {
    // Clock & Reset
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // --- Cmd interface (from CoreController CmdFabric) ---
    sc_in<bool>          cmd_req_valid;
    sc_in<bool>          cmd_req_write;
    sc_in<sc_uint<32>>   cmd_req_addr;
    sc_in<sc_uint<32>>   cmd_req_wdata;
    sc_out<bool>         cmd_resp_valid;
    sc_out<sc_uint<32>>  cmd_resp_rdata;

    // --- AHB-Lite master (to ComputeCluster) ---
    sc_out<bool>         hsel_o;
    sc_out<sc_uint<32>>  haddr_o;
    sc_out<bool>         hwrite_o;
    sc_out<sc_uint<2>>   htrans_o;
    sc_out<sc_uint<3>>   hsize_o;
    sc_out<sc_uint<3>>   hburst_o;
    sc_out<sc_uint<4>>   hprot_o;
    sc_out<bool>         hready_o;     // master's ready to slave
    sc_out<sc_uint<32>>  hwdata_o;

    sc_in<bool>          hready_i;     // slave's ready
    sc_in<bool>          hresp_i;
    sc_in<sc_uint<32>>   hrdata_i;

    SC_HAS_PROCESS(CmdToAhbBridge);

    CmdToAhbBridge(sc_module_name name)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          cmd_req_valid("cmd_req_valid"), cmd_req_write("cmd_req_write"),
          cmd_req_addr("cmd_req_addr"), cmd_req_wdata("cmd_req_wdata"),
          cmd_resp_valid("cmd_resp_valid"), cmd_resp_rdata("cmd_resp_rdata"),
          hsel_o("hsel_o"), haddr_o("haddr_o"), hwrite_o("hwrite_o"),
          htrans_o("htrans_o"), hsize_o("hsize_o"), hburst_o("hburst_o"),
          hprot_o("hprot_o"), hready_o("hready_o"), hwdata_o("hwdata_o"),
          hready_i("hready_i"), hresp_i("hresp_i"), hrdata_i("hrdata_i")
    {
        SC_CTHREAD(bridge_proc, clk.pos());
        reset_signal_is(reset_n, false);
    }

private:
    void bridge_proc() {
        // Reset outputs
        cmd_resp_valid.write(false);
        cmd_resp_rdata.write(0);
        hsel_o.write(false);
        haddr_o.write(0);
        hwrite_o.write(false);
        htrans_o.write(0);  // IDLE
        hsize_o.write(2);   // WORD (32-bit)
        hburst_o.write(0);  // SINGLE
        hprot_o.write(0);
        hready_o.write(true);
        hwdata_o.write(0);
        wait();

        while (true) {
            cmd_resp_valid.write(false);

            if (cmd_req_valid.read()) {
                const bool is_write = cmd_req_write.read();
                const uint32_t addr = cmd_req_addr.read().to_uint();
                const uint32_t wdata = cmd_req_wdata.read().to_uint();

                // --- AHB Address Phase ---
                hsel_o.write(true);
                haddr_o.write(addr);
                hwrite_o.write(is_write);
                htrans_o.write(2);  // NONSEQ
                hsize_o.write(2);   // WORD
                hburst_o.write(0);  // SINGLE
                hready_o.write(true);
                hwdata_o.write(is_write ? sc_uint<32>(wdata) : sc_uint<32>(0));
                wait(); // Address phase sampled by slave

                // --- AHB Data Phase ---
                // Deassert address phase signals
                hsel_o.write(false);
                htrans_o.write(0);  // IDLE
                hwdata_o.write(is_write ? sc_uint<32>(wdata) : sc_uint<32>(0));

                // Wait for slave to be ready
                while (!hready_i.read()) {
                    wait();
                }

                // Capture read data (valid when hready_i=1)
                sc_uint<32> rdata = hrdata_i.read();

                // For reads, the ComputeCluster inserts 1 extra wait-state
                // Wait one more cycle until hready_i is high again
                if (!is_write) {
                    wait();
                    while (!hready_i.read()) {
                        wait();
                    }
                    rdata = hrdata_i.read();
                }

                // Send response back to CmdFabric
                cmd_resp_valid.write(true);
                cmd_resp_rdata.write(rdata);
                wait();
            } else {
                // Idle: no transaction
                hsel_o.write(false);
                htrans_o.write(0);
                hready_o.write(true);
                wait();
            }
        }
    }
};

} // namespace core
} // namespace hybridacc
