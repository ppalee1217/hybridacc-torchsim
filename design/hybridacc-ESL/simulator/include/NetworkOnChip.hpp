#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"
#include "NoC/MBUS.hpp"
#include "NoC/NoCRouter.hpp"
#include "ProcessElement.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {

SC_MODULE(NetworkOnChip) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // NoC Router interface ports
    sc_in<bool> command_mode;
    sc_in<sc_uint<32>> command_data;

    // New Valid-Ready Interface (Dual Plane)
    VRDIF<noc::router_req_t> req0_in;
    VRDIF<noc::router_req_t> req1_in;
    VRDIF<noc_addr_req_t> req2_in;
    VRDOF<noc::router_resp_t> resp2_out;

    // ---------------------------------------------
    // parameters
    size_t num_port;
    size_t num_pes_per_port;
    size_t num_pes() const { return num_port * num_pes_per_port; }

    // Internal modules
    noc::NoCRouter router;
    sc_vector<noc::MBUS> mbus;
    sc_vector<sc_vector<pe::ProcessElement>> pes;

    // Internal signals - NoC/MBus
    sc_vector<VRDSIG<noc_request_t>> noc0_to_bus_req;
    sc_vector<VRDSIG<noc_request_t>> noc1_to_bus_req;
    sc_vector<VRDSIG<noc_addr_req_t>> noc2_to_bus_req;
    sc_vector<VRDSIG<noc_response_t>> bus_to_noc2_resp;

    sc_signal<bool> scan_chain_enable;

    // Scan chain signals
    sc_vector<sc_signal<ScanChainFormat>> router_scan_chain_out;
    sc_vector<sc_signal<ScanChainFormat>> mbus_scan_chain_out;

    //  Internal signals - MBus/PE
    sc_vector<sc_vector<sc_signal<bool>>> router_enable;
    sc_vector<sc_vector<sc_signal<PERouterMode>>> router_mode;
    sc_vector<sc_vector<VRDSIG<noc_request_t>>> bus_to_pe_noc0_req;
    sc_vector<sc_vector<VRDSIG<noc_request_t>>> bus_to_pe_noc1_req;
    sc_vector<sc_vector<VRDSIG<noc_addr_req_t>>> bus_to_pe_noc2_req;
    sc_vector<sc_vector<VRDSIG<noc_response_t>>> pe_to_bus_noc2_resp;
    sc_vector<sc_vector<sc_signal<bool>>> pe_busy;

    //  Internal signals - PE/PE
    sc_vector<sc_vector<VRDSIG<uint64_t>>> ln_pli_plo;


    NetworkOnChip(sc_module_name name, size_t num_port, size_t num_pes_per_port)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          req0_in("req0_in"),
          req1_in("req1_in"),
          req2_in("req2_in"),
          resp2_out("resp2_out"),
          num_port(num_port),
          num_pes_per_port(num_pes_per_port),
          router("NoC_Router", num_port),
          mbus("mbus"),
          pes("pes"),
          noc0_to_bus_req("noc0_to_bus_req", num_port),
          noc1_to_bus_req("noc1_to_bus_req", num_port),
          noc2_to_bus_req("noc2_to_bus_req", num_port),
          bus_to_noc2_resp("bus_to_noc2_resp", num_port),
          scan_chain_enable("scan_chain_enable"),
          router_scan_chain_out("router_scan_chain_out", num_port),
          mbus_scan_chain_out("mbus_scan_chain_out", num_port),
          router_enable("router_enable"),
          router_mode("router_mode"),
          bus_to_pe_noc0_req("bus_to_pe_noc0_req"),
          bus_to_pe_noc1_req("bus_to_pe_noc1_req"),
          bus_to_pe_noc2_req("bus_to_pe_noc2_req"),
          pe_to_bus_noc2_resp("pe_to_bus_noc2_resp"),
          pe_busy("pe_busy"),
          ln_pli_plo("ln_pli_plo")
    {
        DEBUG_MSG("[Create] NetworkOnChip with " << num_port << " ports, "
                  << num_pes_per_port << " PEs per port", DEBUG_LEVEL_NOC_TOP);

        std::cout << "NetworkOnChip: Initializing with "
                  << num_port << " ports, "
                  << num_pes_per_port << " PEs per port" << std::endl;

        // 初始化 MBUS 向量
        mbus.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new noc::MBUS(n, num_pes_per_port);
        });

        // 初始化 PE 二維向量
        pes.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<pe::ProcessElement>(n, num_pes_per_port, [](const char* m, size_t j) {
                return new pe::ProcessElement(m);
            });
        });

        // 初始化內部信號二維向量
        router_enable.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<sc_signal<bool>>(n, num_pes_per_port);
        });

        router_mode.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<sc_signal<PERouterMode>>(n, num_pes_per_port);
        });

        bus_to_pe_noc0_req.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_request_t>>(n, num_pes_per_port);
        });

        bus_to_pe_noc1_req.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_request_t>>(n, num_pes_per_port);
        });

        bus_to_pe_noc2_req.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_addr_req_t>>(n, num_pes_per_port);
        });

        pe_to_bus_noc2_resp.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<VRDSIG<noc_response_t>>(n, num_pes_per_port);
        });

        pe_busy.init(num_port, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<sc_signal<bool>>(n, num_pes_per_port);
        });

        ln_pli_plo.init(num_port+1, [this, num_pes_per_port](const char* n, size_t i) {
            return new sc_vector<VRDSIG<uint64_t>>(n, num_pes_per_port);
        });

        // Bind internal modules
        bind();
    }

    NetworkOnChip(sc_module_name name)
        : NetworkOnChip(name, 3, 16) // Default to 3 ports, 16 PEs per port
    {}

    // Debug: Dump internal state
    void dump_state() {
        std::cout << "\n[NoC] Dumping NetworkOnChip State:" << std::endl;
        std::cout << "--- MBUS ---" << std::endl;
        for(auto& mbus_inst : mbus) {
            mbus_inst.dump_state();
        }
        std::cout << "[NoC] End of NetworkOnChip State Dump\n" << std::endl;
    }

private:
    void bind() {
        // NoC Router bindings (Dual Plane Integrated)
        router.clk(clk);
        router.reset_n(reset_n);
        router.command_mode(command_mode);
        router.command_data(command_data);

        // Bind external ports to router
        bind_vr_interface(router.req0_in, req0_in);
        bind_vr_interface(router.req1_in, req1_in);
        bind_vr_interface(router.req2_in, req2_in);
        bind_vr_interface(resp2_out, router.resp2_out);

        for (size_t i = 0; i < num_port; ++i) {
            // Connect NoC to MBUS request signals
            connect_vr_signals(router.noc0_to_bus_req[i], noc0_to_bus_req[i]);
            connect_vr_signals(router.noc1_to_bus_req[i], noc1_to_bus_req[i]);
            connect_vr_signals(router.noc2_to_bus_req[i], noc2_to_bus_req[i]);
            connect_vr_signals(router.bus_to_noc2_resp[i], bus_to_noc2_resp[i]);

            // Scan chain: router -> mbus
            router.scan_chain_in[i](mbus_scan_chain_out[i]);
            router.scan_chain_out[i](router_scan_chain_out[i]);
        }
        router.scan_chain_enable(scan_chain_enable);

        // MBUS bindings
        for (size_t i = 0; i < num_port; ++i) {
            noc::MBUS& mbus_inst = mbus[i];

            mbus_inst.clk(clk);
            mbus_inst.reset_n(reset_n);

            // Connect NoC to MBUS request signals
            connect_vr_signals(mbus_inst.noc0_to_bus_req, noc0_to_bus_req[i]);
            connect_vr_signals(mbus_inst.noc1_to_bus_req, noc1_to_bus_req[i]);
            connect_vr_signals(mbus_inst.noc2_to_bus_req, noc2_to_bus_req[i]);
            connect_vr_signals(mbus_inst.bus_to_noc2_resp, bus_to_noc2_resp[i]);

            // Scan chain: router -> mbus
            mbus_inst.scan_chain_enable(scan_chain_enable);
            mbus_inst.scan_chain_in(router_scan_chain_out[i]);
            mbus_inst.scan_chain_out(mbus_scan_chain_out[i]);

            // Connect PE interface signals
            for (size_t j = 0; j < num_pes_per_port; ++j) {
                mbus_inst.router_enable[j](router_enable[i][j]);
                mbus_inst.router_mode[j](router_mode[i][j]);

                connect_vr_signals(mbus_inst.bus_to_pe_noc0_req[j], bus_to_pe_noc0_req[i][j]);
                connect_vr_signals(mbus_inst.bus_to_pe_noc1_req[j], bus_to_pe_noc1_req[i][j]);
                connect_vr_signals(mbus_inst.bus_to_pe_noc2_req[j], bus_to_pe_noc2_req[i][j]);
                connect_vr_signals(mbus_inst.pe_to_bus_noc2_resp[j], pe_to_bus_noc2_resp[i][j]);

                mbus_inst.pe_busy[j](pe_busy[i][j]);
            }
        }

        // PEs bindings
        for (size_t i = 0; i < num_port; ++i) {
            for (size_t j = 0; j < num_pes_per_port; ++j) {
                pe::ProcessElement& pe_inst = pes[i][j];

                // Connect clock and reset
                pe_inst.clk(clk);
                pe_inst.reset_n(reset_n);

                // Router config ports
                pe_inst.router_enable(router_enable[i][j]);
                pe_inst.router_mode(router_mode[i][j]);

                // NoC interface
                connect_vr_signals(pe_inst.noc0_req, bus_to_pe_noc0_req[i][j]);
                connect_vr_signals(pe_inst.noc1_req, bus_to_pe_noc1_req[i][j]);
                connect_vr_signals(pe_inst.noc2_req, bus_to_pe_noc2_req[i][j]);
                connect_vr_signals(pe_inst.noc2_resp, pe_to_bus_noc2_resp[i][j]);

                // PE busy signal
                pe_inst.pe_busy(pe_busy[i][j]);

                // Connect Local Network signals (PE-to-PE communication)
                connect_vr_signals(pe_inst.ln_pli, ln_pli_plo[i][j]);
                connect_vr_signals(pe_inst.ln_plo, ln_pli_plo[i+1][j]);
            }
        }

        // Set Trace IDs
        router.set_trace_id(0);
        for (size_t i = 0; i < num_port; ++i) {
            mbus[i].set_trace_id(i + 1);
            for (size_t j = 0; j < num_pes_per_port; ++j) {
                pes[i][j].set_trace_id(i * num_pes_per_port + j);
            }
        }
    }
};

} // namespace hybridacc


/*
以下是詳細的行為解析：

### 1. 整體架構層次 (Hierarchy)

`NetworkOnChip` 內部包含了三個主要的層級，形成一個樹狀結構：

1.  **Top Level: `NoCRouter` (1 個)**
    *   這是 NoC 的大腦與入口。
    *   它負責接收外部請求 (`req_in`)，解析地址，並將請求分發給正確的 `MBUS` 端口。
    *   它同時負責收集所有 `MBUS` 回傳的數據，打包後透過 `resp_out` 回傳給外部。
    *   **行為特點**：具備 FIFO 緩衝，支援廣播 (Broadcast) 與單播 (Unicast)，並保證請求與回應的順序性。

2.  **Middle Level: `MBUS` (多個，數量 = `num_port`)**
    *   這是中間層的匯流排 (Bus)，每個 `MBUS` 管理一組 PEs（例如 1 個 MBUS 管 16 個 PEs）。
    *   **下行 (Request)**：它接收來自 `NoCRouter` 的請求，並根據地址仲裁，決定是廣播給該組所有 PE，還是傳給特定的 PE。
    *   **上行 (Response)**：它負責仲裁該組內所有 PE 的回傳請求 (Round-Robin 或 Fixed Priority)，將選中的數據回傳給 `NoCRouter`。
    *   **行為特點**：它是 "One-to-Many" (分發) 和 "Many-to-One" (匯集) 的樞紐。

3.  **Leaf Level: `ProcessElement` (多個，數量 = `num_port * num_pes_per_port`)**
    *   這是實際執行運算的單元。
    *   每個 PE 都有一個內建的 `PErouter` 來處理 NoC 的通訊協定。

### 2. 外部介面行為 (External Interface)

這是這次重構的重點，現在對外完全採用標準的 **Valid-Ready Handshake**：

*   **輸入 (`req_in` - `VRDIF`)**：
    *   外部模組（如 DMA 或 Controller）發送 `noc::router_req_t` 結構的請求。
    *   **Backpressure**：如果 NoC 內部忙碌（例如 FIFO 滿了），`req_in.ready` 會拉低，外部必須暫停發送。這保證了數據不會遺失。

*   **輸出 (`resp_out` - `VRDOF`)**：
    *   當 PE 完成運算或讀取數據後，結果會透過這裡回傳 `noc::router_resp_t`。
    *   外部模組準備好接收時拉高 Ready，NoC 才會送出 Valid 數據。

*   **控制信號 (`command_mode`, `command_data`)**：
    *   這是一組旁路 (Sideband) 信號，通常用於特殊的全局控制或模式切換，不走主要的數據流通道。

### 3. 內部數據流 (Data Flow)

#### A. 請求路徑 (Request Path: Host -> PE)
1.  **NoCRouter**: 從 `req_in` 收到封包 -> 存入 `req_fifo` -> `process_thread` 取出 -> 解析地址。
2.  **Dispatch**: `NoCRouter` 根據地址將請求送往對應的 `noc_to_bus_req[i]` 介面。
3.  **MBUS**: 第 `i` 個 `MBUS` 收到請求 -> 檢查地址是給自己底下的哪個 PE。
4.  **PE**: `MBUS`透過 `bus_to_pe_req[j]` 將數據送入 PE 的 `PErouter`。

#### B. 回應路徑 (Response Path: PE -> Host)
1.  **PE**: 運算完成，透過 `pe_to_bus_resp[j]` 發出回應。
2.  **MBUS**: 仲裁多個 PE 的回應，選中一個，透過 `bus_to_noc_resp[i]` 送往 `NoCRouter`。
3.  **NoCRouter**: 收集來自各個 `MBUS` 的回應 -> 存入 `resp_fifo` -> 透過 `resp_out` 送出。

### 4. 特殊機制

#### A. 掃描鏈 (Scan Chain)
程式碼中有大量的 `scan_chain_...` 信號。這是在系統啟動或重置時使用的機制：
*   **目的**：為每個 PE 分配唯一的 ID (Coordinate)，並設定路由模式。
*   **路徑**：`NoCRouter` -> `MBUS[0]` -> `MBUS[1]` ... -> `MBUS[N]`。
*   這就像一條長鏈，配置數據像貪食蛇一樣流過所有硬體單元，完成初始化。

#### B. 本地網路 (Local Network - `ln_pli_plo`)
*   除了樹狀的 NoC 結構外，PE 之間還有一條橫向的連接 (`ln_pli`, `ln_plo`)。
*   這通常用於 **Systolic Array (脈動陣列)** 模式，讓數據可以直接從 PE[i] 流向 PE[i+1]，而不需要經過上層的 Router，適合做矩陣乘法或卷積運算。

### 總結
`NetworkOnChip` 就像是一個 **封裝良好的黑盒子**。
*   **對外**：它看起來像一個帶有 FIFO 的單一裝置，你只要餵給它 Request，它就會吐出 Response，不用擔心內部複雜的路由。
*   **對內**：它管理著複雜的層級連接，確保數據能準確地在 48 個 (3x16) 或更多 PE 之間流動，並處理所有的握手與阻塞問題。

*/