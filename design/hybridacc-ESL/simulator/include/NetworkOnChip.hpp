#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"
#include "NoC/MBUS.hpp"
#include "NoC/NoCRouter.hpp"
#include "ProcessElement.hpp"


namespace hybridacc {

SC_MODULE(NetworkOnChip) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // NoC Router interface ports
    sc_in<bool> command_mode;
    sc_in<sc_uint<32>> command_data;

    sc_in<bool> en;
    sc_in<bool> wen;
    sc_in<sc_uint<10>> addr; // [9] cmd, [8] SIMD, [7:6] channel, [5:0] tag
    sc_in<sc_uint<256>> data_in;
    sc_out<sc_uint<256>> data_out;

    // ---------------------------------------------
    // parameters
    size_t num_port;
    size_t num_pes_per_port;
    size_t num_pes() const { return num_port * num_pes_per_port; }

    // Internal modules
    noc::NoCRouter router;
    noc::sc_vector<MBUS> mbus;
    sc_vector<sc_vector<pe::ProcessElement>> pes;

    // Internal signals - NoC/MBus
    sc_vector<VRDSIG<noc_request_t>> noc_to_bus_req;
    sc_vector<VRDSIG<noc_response_t>> bus_to_noc_resp;

    sc_signal<bool> scan_chain_enable;
    sc_vector<sc_signal<ScanChainFormat>> scan_chain_in;
    sc_vector<sc_signal<ScanChainFormat>> scan_chain_out;

    //  Internal signals - MBus/PE
    sc_vector<sc_vector<sc_signal<bool>>> router_enable;
    sc_vector<sc_vector<sc_signal<PERouterMode>>> router_mode;
    sc_vector<sc_vector<VRDSIG<noc_request_t>>> bus_to_pe_req;
    sc_vector<sc_vector<VRDSIG<noc_response_t>>> pe_to_bus_resp;
    sc_vector<sc_vector<sc_signal<bool>>> pe_busy;

    //  Internal signals - PE/PE
    sc_vector<sc_vector<VRDSIG<uint64_t>>> ln_pli_plo;


    NetworkOnChip(sc_core::sc_module_name name, size_t num_port, size_t num_pes_per_port)
        : sc_core::sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          en("en"),
          wen("wen"),
          addr("addr"),
          data_in("data_in"),
          data_out("data_out"),
          num_port(num_port),
          num_pes_per_port(num_pes_per_port),
          router("NoC_Router", num_port),
          noc_to_bus_req("noc_to_bus_req", num_port),
          bus_to_noc_resp("bus_to_noc_resp", num_port),
          scan_chain_enable("scan_chain_enable"),
          scan_chain_in("scan_chain_in", num_port),
          scan_chain_out("scan_chain_out", num_port),
          router_enable("router_enable"),
          router_mode("router_mode"),
          bus_to_pe_req("bus_to_pe_req"),
          pe_to_bus_resp("pe_to_bus_resp"),
          pe_busy("pe_busy"),
          ln_pli_plo("ln_pli_plo"),
          pes("pes"),
          mbus("mbus")
    {
        DEBUG_MSG("[Create] NetworkOnChip with " << num_port << " ports, "
                  << num_pes_per_port << " PEs per port");

        // 用 lambda 建立每一個 element
        mbus.init(num_pes, [this](const char* n, size_t i) {
            return new MBUS(n, i);   // 若 MBUS 需要其他參數可在此補上
        });

        // 建立 PE 二維向量
        pes.init(num_port, [this](const char* n, size_t i) {
            sc_vector<pe::ProcessElement> pe_vec("pe_vec");
            pe_vec.init(num_pes_per_port, [this, i](const char* m, size_t j) {
                return new pe::ProcessElement(m); // 若 PE 需要其他參數可在此補上
            });
            return pe_vec;
        });

        // 初始化內部信號向量
        router_enable.init(num_port, [this](const char* n, size_t i) {
            sc_vector<sc_signal<bool>> sig_vec("sig_vec");
            sig_vec.init(num_pes_per_port);
            return sig_vec;
        });
        router_mode.init(num_port, [this](const char* n, size_t i) {
            sc_vector<sc_signal<PERouterMode>> sig_vec("sig_vec");
            sig_vec.init(num_pes_per_port);
            return sig_vec;
        });
        bus_to_pe_req.init(num_port, [this](const char* n, size_t i) {
            sc_vector<VRDSIG<noc_request_t>> sig_vec("sig_vec");
            sig_vec.init(num_pes_per_port);
            return sig_vec;
        });
        pe_to_bus_resp.init(num_port, [this](const char* n, size_t i) {
            sc_vector<VRDSIG<noc_response_t>> sig_vec("sig_vec");
            sig_vec.init(num_pes_per_port);
            return sig_vec;
        });
        pe_busy.init(num_port, [this](const char* n, size_t i) {
            sc_vector<sc_signal<bool>> sig_vec("sig_vec");
            sig_vec.init(num_pes_per_port);
            return sig_vec;
        });
        ln_pli_plo.init(num_port+1, [this](const char* n, size_t i) {
            sc_vector<VRDSIG<uint64_t>> sig_vec("sig_vec");
            sig_vec.init(num_pes_per_port);
            return sig_vec;
        });

        // Bind internal modules
        bind();
    }

private:
    void bind() {
        // NoC Router bindings
        router.clk(clk);
        router.reset_n(reset_n);
        router.command_mode(command_mode);
        router.command_data(command_data);
        router.en(en);
        router.wen(wen);
        router.addr(addr);
        router.data_in(data_in);
        router.data_out(data_out);
        for (size_t i = 0; i < num_port; ++i) {
            // Connect NoC to MBUS request signals
            connect_vr_signals(router.noc_to_bus_req[i], noc_to_bus_req[i]);
            connect_vr_signals(router.bus_to_noc_resp[i], bus_to_noc_resp[i]);

            // Connect scan-chain signals
            router.scan_chain_in[i](scan_chain_in[i]);
            router.scan_chain_out[i](scan_chain_out[i]);
        }
        router.scan_chain_enable(scan_chain_enable);

        // MBUS bindings
        for (size_t i = 0; i < num_port; ++i) {
            MBUS& mbus_inst = mbus[i];

            mbus_inst.clk(clk);
            mbus_inst.reset_n(reset_n);

            // Connect NoC to MBUS request signals
            connect_vr_signals(mbus_inst.noc_to_bus_req, noc_to_bus_req[i]);
            connect_vr_signals(mbus_inst.bus_to_noc_resp, bus_to_noc_resp[i]);

            // Connect scan-chain signals
            mbus_inst.scan_chain_in(scan_chain_out[i]);
            mbus_inst.scan_chain_out(scan_chain_in[i]);
            mbus_inst.scan_chain_enable(scan_chain_enable); // broadcast

            // Connect PE interface signals
            for (size_t j = 0; j < num_pes_per_port; ++j) {
                // Router config ports
                mbus_inst.router_enable[j](router_enable[i][j]);
                mbus_inst.router_mode[j](router_mode[i][j]);

                // NoC <-> PE ports
                connect_vr_signals(mbus_inst.bus_to_pe_req[j], bus_to_pe_req[i][j]);
                connect_vr_signals(mbus_inst.pe_to_bus_resp[j], pe_to_bus_resp[i][j]);

                // PE busy signals
                mbus_inst.pe_busy[j](pe_busy[i][j]);
            }
        }

        // PEs bindings
        for (size_t i = 0; i < num_port; ++i) {
            for (size_t j = 0; j < num_pes_per_port; ++j) {;
                pe::ProcessElement& pe_inst = pes[i][j];
                // Connect MBUS to PE request/response signals
                pe_inst.clk(clk);
                pe_inst.reset_n(reset_n);

                pe_inst.router_enable(router_enable[i][j]);
                pe_inst.router_mode(router_mode[i][j]);

                connect_vr_signals(pe_inst.noc_req, bus_to_pe_req[j]);
                connect_vr_signals(pe_inst.noc_resp, pe_to_bus_resp[j]);


                pe_inst.pe_busy(pe_busy[i][j]);

                // Connect Local Network signals
                connect_vr_signals(pe_inst.ln_pli, ln_pli_plo[i][j]);
                connect_vr_signals(pe_inst.ln_plo, ln_pli_plo[i+1][j]);
            }
        }
    }
}

} // namespace hybridacc