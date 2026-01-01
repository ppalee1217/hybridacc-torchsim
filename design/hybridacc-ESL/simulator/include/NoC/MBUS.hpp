#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"

using namespace sc_core;

namespace hybridacc {
namespace noc {

static const int NUM_PES_DEFAULT = 16;

SC_MODULE(MBUS) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // === Process Element (PE) interface ports ===
    // Router config port
    sc_vector<sc_out<bool>> router_enable;
    sc_vector<sc_out<PERouterMode>> router_mode;

    // NoC interface ports - using VRDOF/VRDIF with internal signals
    // Updated for Dual-Plane: bus_to_pe_req now carries NoC-0 and NoC-1 requests?
    // Actually, PE has noc0_req_in and noc1_req_in.
    // MBUS needs to output to both.
    // Let's assume MBUS connects to PE's noc0 and noc1 ports.
    // But wait, the design says "MBUS module will support two distinct channels... integrated within the same MBUS module".
    // And "The top-level module will instantiate two separate router networks and MBus arrays." in 3.1.
    // BUT in 3.2 MBUS Design Updates: "The MBUS module will support two distinct channels... integrated within the same MBUS module".
    // This is contradictory. 3.1 says "Instantiate mbus0 and mbus1". 3.2 says "integrated within the same MBUS module".
    // Looking at the user request: "MBUS 中 NoC interface 我需要拆分成兩組" (Split NoC interface in MBUS into two groups).
    // And the PErouter change added noc0_req_in and noc1_req_in.
    // If we follow 3.1 "Instantiate mbus0 and mbus1", then MBUS itself doesn't need to change much, just maybe specialized?
    // But 3.2 says "Dual-Channel Architecture... integrated within the same MBUS module".
    // Let's follow 3.2 and the user request to split the interface IN MBUS.
    // So MBUS will have noc0_to_bus_req, noc1_to_bus_req, bus_to_noc1_resp.
    // And it needs to drive PE's noc0_req and noc1_req.

    // PE interface ports
    sc_vector<VRDOF<noc_request_t>> bus_to_pe_noc0_req; // To PE's noc0_req_in
    sc_vector<VRDOF<noc_request_t>> bus_to_pe_noc1_req; // To PE's noc1_req_in
    sc_vector<VRDIF<noc_response_t>> pe_to_bus_noc1_resp; // From PE's noc1_resp_out

    // Control ports
    sc_vector<sc_in<bool>> pe_busy;

    // ===  NoC Router interface ports ===
    // NoC-0 (Control & Push)
    VRDIF<noc_request_t> noc0_to_bus_req;

    // NoC-1 (Local Network)
    VRDIF<noc_request_t> noc1_to_bus_req;
    VRDOF<noc_response_t> bus_to_noc1_resp;

    // ID, mode, enable scan-chain ports
    sc_in<bool> scan_chain_enable;
    sc_in<ScanChainFormat> scan_chain_in;
    sc_out<ScanChainFormat> scan_chain_out;

    // Constructor
    SC_HAS_PROCESS(MBUS);
    MBUS(sc_module_name name, size_t num_pes)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          num_pes(num_pes),
          bus_to_pe_noc0_req("bus_to_pe_noc0_req", num_pes),
          bus_to_pe_noc1_req("bus_to_pe_noc1_req", num_pes),
          pe_to_bus_noc1_resp("pe_to_bus_noc1_resp", num_pes),
          router_enable("router_enable", num_pes),
          router_mode("router_mode", num_pes),
          pe_busy("pe_busy", num_pes),
          noc0_to_bus_req("noc0_to_bus_req"),
          noc1_to_bus_req("noc1_to_bus_req"),
          bus_to_noc1_resp("bus_to_noc1_resp"),
          scan_chain_enable("scan_chain_enable"),
          scan_chain_in("scan_chain_in"),
          scan_chain_out("scan_chain_out"),
          pe_scan_chain_signals_reg("pe_scan_chain_signal_reg", num_pes),
          pe_scan_chain_signals_next("pe_scan_chain_signal_next", num_pes) {

        DEBUG_MSG("[Create] MBUS with " << num_pes << " PEs", DEBUG_LEVEL_NOC_COMPONENTS);

        // Register sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Combinational processes with different sensitivity lists

        // Scan-chain shifting logic
        SC_METHOD(comb_scan_chain_shift);
        sensitive << scan_chain_enable << scan_chain_in;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i];
        }

        // Router configuration output (from scan-chain registers)
        SC_METHOD(comb_router_config);
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i];
        }

        // NoC-0 request routing (Combinational, Write-Only)
        SC_METHOD(comb_noc0_routing);
        sensitive << noc0_to_bus_req.valid_in << noc0_to_bus_req.data_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i]
                      << bus_to_pe_noc0_req[i].ready_in;
        }

        // NoC-1 request routing (Pipelined/Stateful for Read)
        SC_METHOD(comb_noc1_routing);
        sensitive << noc1_to_bus_req.valid_in << noc1_to_bus_req.data_in << scan_chain_enable;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i]
                      << bus_to_pe_noc1_req[i].ready_in;
        }

        // PE response to NoC-1 (including collision detection)
        SC_METHOD(comb_pe_to_noc1_response);
        sensitive << scan_chain_enable << rx_mask_reg;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_to_bus_noc1_resp[i].valid_in
                      << pe_to_bus_noc1_resp[i].data_in;
        }

        // PE response ready signals
        SC_METHOD(comb_pe_response_ready);
        sensitive << bus_to_noc1_resp.ready_in << rx_mask_reg;

        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    // Overloaded constructor with default number of PEs
    MBUS(sc_module_name name)
        : MBUS(name, NUM_PES_DEFAULT) // Default to NUM_PES_DEFAULT PEs
    {}

    // Debug: Dump internal state
    void dump_state() {
        std::cout << "\n[MBUS] Dumping MBUS State:" << std::endl;
        for(auto &config_sig : pe_scan_chain_signals_reg) {
            ScanChainFormat config = config_sig.read();
            std::cout << config << std::endl;
        }
        std::cout << "[MBUS] End of MBUS State Dump\n" << std::endl;
    }

private:
    size_t num_pes;

    // === Sequential Elements ===
    // Scan-chain signals for each PE
    sc_vector<sc_signal<ScanChainFormat>> pe_scan_chain_signals_reg;
    sc_vector<sc_signal<ScanChainFormat>> pe_scan_chain_signals_next;

    // Request Mask Register (for response routing)
    sc_signal<uint64_t> rx_mask_reg;
    sc_signal<uint64_t> rx_mask_next;

    // Internal signal to share calculated mask
    sc_signal<uint64_t> tx_mask_wire;

    // Internal signal for NoC request ready
    sc_signal<bool> noc0_req_ready_sig;
    sc_signal<bool> noc1_req_ready_sig;

    // === Sequential Process ===
    void seq_process() {
        // Reset initialization
        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat init_config;
            init_config.ps_id = 0;
            init_config.pd_id = 0;
            init_config.pli_id = 0;
            init_config.plo_id = 0;
            init_config.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
            init_config.enable = false;
            pe_scan_chain_signals_reg[i].write(init_config);
        }
        rx_mask_reg.write(0);

        wait();

        // Sequential logic
        while (true) {
            for (size_t i = 0; i < num_pes; ++i) {
                pe_scan_chain_signals_reg[i].write(pe_scan_chain_signals_next[i].read());
            }

            uint64_t next_mask = rx_mask_next.read();
            if (next_mask != rx_mask_reg.read()) {
                DEBUG_MSG("[MBUS] rx_mask changed: 0x" << std::hex << rx_mask_reg.read() << " -> 0x" << next_mask << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }
            rx_mask_reg.write(next_mask);

            wait();
        }
    }

    // === Combinational: Scan-chain Shifting ===
    void comb_scan_chain_shift() {
        if (scan_chain_enable.read()) {
            // Shift scan-chain data every cycle
            ScanChainFormat first_config = scan_chain_in.read();
            pe_scan_chain_signals_next[0].write(first_config);

            for (size_t i = 1; i < num_pes; ++i) {
                ScanChainFormat prev_config = pe_scan_chain_signals_reg[i - 1].read();
                pe_scan_chain_signals_next[i].write(prev_config);
            }

            // Output from last PE
            ScanChainFormat last_config = pe_scan_chain_signals_reg[num_pes - 1].read();
            scan_chain_out.write(last_config);
        } else {
            // Hold current configuration
            for (size_t i = 0; i < num_pes; ++i) {
                pe_scan_chain_signals_next[i].write(pe_scan_chain_signals_reg[i].read());
            }

            // Output default
            ScanChainFormat default_out;
            default_out.ps_id = 0;
            default_out.pd_id = 0;
            default_out.pli_id = 0;
            default_out.plo_id = 0;
            default_out.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
            default_out.enable = false;
            scan_chain_out.write(default_out);
        }
    }

    // === Combinational: Router Configuration Output ===
    void comb_router_config() {
        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat config = pe_scan_chain_signals_reg[i].read();
            router_enable[i].write(config.enable);
            router_mode[i].write(config.route_mode);
        }
    }

    // === Combinational: NoC-0 Request Routing (Write-Only) ===
    void comb_noc0_routing() {
        bool scan_mode = scan_chain_enable.read();
        bool noc_valid = noc0_to_bus_req.valid_in.read();
        noc_request_t noc_req = noc0_to_bus_req.data_in.read();

        // Calculate target PE mask
        uint64_t tx_mask = 0;
        if (!scan_mode) {
            tx_mask = calculate_target_pe_mask(noc_req.addr);
            if (noc_valid) {
                DEBUG_MSG("[MBUS] NoC-0 Routing data: 0x" << std::hex << noc_req.data << ", Addr 0x" << noc_req.addr << ", Mask 0x" << tx_mask << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
            }
        }

        // Check if all target PEs are ready
        bool all_ready = true;
        if (tx_mask != 0) {
            for (size_t i = 0; i < num_pes; ++i) {
                if (tx_mask & (1ULL << i)) {
                    if (!bus_to_pe_noc0_req[i].ready_in.read()) {
                        all_ready = false;
                        break;
                    }
                }
            }
        }

        // Set NoC ready signal
        bool noc_ready = !scan_mode && all_ready;

        noc0_req_ready_sig.write(noc_ready);
        noc0_to_bus_req.ready_out.write(noc_ready);

        // Route request to target PEs
        for (size_t i = 0; i < num_pes; ++i) {
            bool is_target = (tx_mask & (1ULL << i)) != 0;
            bool send_to_pe = noc_valid && !scan_mode && is_target && all_ready;

            bus_to_pe_noc0_req[i].data_out.write(noc_req);
            bus_to_pe_noc0_req[i].valid_out.write(send_to_pe);
        }
    }

    // === Combinational: NoC-1 Request Routing (Read/Write) ===
    void comb_noc1_routing() {
        bool scan_mode = scan_chain_enable.read();
        bool noc_valid = noc1_to_bus_req.valid_in.read();
        noc_request_t noc_req = noc1_to_bus_req.data_in.read();

        // Calculate target PE mask
        uint64_t tx_mask = 0;
        if (!scan_mode) {
            tx_mask = calculate_target_pe_mask(noc_req.addr);
            if (noc_valid) {
                DEBUG_MSG("[MBUS] NoC-1 Routing data: 0x" << std::hex << noc_req.data << ", Addr 0x" << noc_req.addr << ", Mask 0x" << tx_mask << std::dec << ", Mode: " << (noc_req.is_w ? "Write" : "Read"), DEBUG_LEVEL_NOC_COMPONENTS);
            }
        }
        tx_mask_wire.write(tx_mask);

        // Check if all target PEs are ready
        bool all_ready = true;
        if (tx_mask != 0) {
            for (size_t i = 0; i < num_pes; ++i) {
                if (tx_mask & (1ULL << i)) {
                    if (!bus_to_pe_noc1_req[i].ready_in.read()) {
                        all_ready = false;
                        break;
                    }
                }
            }
        }

        // Set NoC ready signal
        bool noc_ready = !scan_mode && all_ready;

        noc1_req_ready_sig.write(noc_ready);
        noc1_to_bus_req.ready_out.write(noc_ready);

        // Route request to target PEs
        for (size_t i = 0; i < num_pes; ++i) {
            bool is_target = (tx_mask & (1ULL << i)) != 0;
            bool send_to_pe = noc_valid && !scan_mode && is_target && all_ready;

            bus_to_pe_noc1_req[i].data_out.write(noc_req);
            bus_to_pe_noc1_req[i].valid_out.write(send_to_pe);
        }

        // update next_rx_mask (Only for NoC-1 Read)
        if(!scan_mode && noc_valid && noc_ready && !noc_req.is_w) {
            rx_mask_next.write(tx_mask);
        } else {
            rx_mask_next.write(0);
        }
    }

    // === Combinational: PE Response to NoC-1 (with collision detection) ===
    void comb_pe_to_noc1_response() {
        bool scan_mode = scan_chain_enable.read();
        uint64_t active_mask = rx_mask_reg.read();

        // Check which PEs have valid responses
        uint64_t resp_mask = 0;
        noc_response_t pe_resp;
        pe_resp.data = 0;
        pe_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;

        for (size_t i = 0; i < num_pes; ++i) {
            // Only consider PEs that are in the active mask
            if ((active_mask & (1ULL << i)) && pe_to_bus_noc1_resp[i].valid_in.read()) {
                DEBUG_MSG("[MBUS] Received response from PE " << i
                          << ": data=0x" << std::hex << pe_to_bus_noc1_resp[i].data_in.read().data
                          << ", status=" << static_cast<int>(pe_to_bus_noc1_resp[i].data_in.read().status)
                          << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
                resp_mask |= (1ULL << i);
                if (resp_mask == (1ULL << i)) { // First response
                    pe_resp = pe_to_bus_noc1_resp[i].data_in.read();
                }
            }
        }

        noc_response_t noc_resp;
        bool noc_resp_valid = false;

        if (scan_mode) {
            // Scan mode: no response
            noc_resp.data = 0;
            noc_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;
            noc_resp_valid = false;
        } else if (active_mask != 0 && resp_mask != 0) {
            // Read request waiting for response
            int resp_count = __builtin_popcountll(resp_mask);

            if (resp_count == 1) {
                // Exactly one PE responded (correct)
                noc_resp = pe_resp;
                noc_resp.status = NOC_RESPONSE_STATUS::NOC_OK;
            } else {
                // Multiple PEs responded (collision error)
                noc_resp.data = 0;
                noc_resp.status = NOC_RESPONSE_STATUS::NOC_ERROR;
                DEBUG_MSG("[MBUS] ERROR: Response collision from " << resp_count << " PEs", DEBUG_LEVEL_NOC_COMPONENTS);
            }
            noc_resp_valid = true;
        } else {
            // No request or read request but no response yet
            noc_resp.data = 0;
            noc_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;
            noc_resp_valid = false;
        }

        // Drive Output Port directly (Combinational)
        bus_to_noc1_resp.data_out.write(noc_resp);
        bus_to_noc1_resp.valid_out.write(noc_resp_valid);
    }

    // === Combinational: PE Response Ready Signals ===
    void comb_pe_response_ready() {
        // Only ready if NoC is ready to accept response AND PE is in the active mask
        bool noc_ready = bus_to_noc1_resp.ready_in.read(); // Direct connection
        uint64_t active_mask = rx_mask_reg.read();

        for (size_t i = 0; i < num_pes; ++i) {
            bool is_active = (active_mask & (1ULL << i)) != 0;
            pe_to_bus_noc1_resp[i].ready_out.write(noc_ready && is_active);
        }
    }

    // === Helper Functions ===

    // Calculate which PEs should receive this request based on address
    uint64_t calculate_target_pe_mask(uint16_t addr) const {
        bool command = (addr == 0x100); // addr[8] = 1 for command
        uint8_t tag = addr & 0x3F;          // addr[5:0] = PE ID tag
        uint8_t channel = (addr >> 6) & 0x3; // addr[7:6] = channel

        uint64_t mask = 0;

        for (size_t i = 0; i < num_pes; ++i) {
            ScanChainFormat config = pe_scan_chain_signals_reg[i].read();

            if (!config.enable) continue;

            uint8_t pe_channel_id = 0;
            switch (channel) {
                case NOC_CHANNEL_PS:  pe_channel_id = config.ps_id; break;
                case NOC_CHANNEL_PD:  pe_channel_id = config.pd_id; break;
                case NOC_CHANNEL_PLI: pe_channel_id = config.pli_id; break;
                case NOC_CHANNEL_PLO: pe_channel_id = config.plo_id; break;
            }

            if (pe_channel_id == tag || command) {
                mask |= (1ULL << i);
            }
        }

        return mask;
    }

    // Trace support
    int trace_id = -1;
    std::string last_state = "IDLE";
    bool trace_init = false;

public:
    void set_trace_id(int id) { trace_id = id; }

private:
    void trace_process() {
        if (trace_id == -1) return;

        if (!trace_init) {
            TRACE_THREAD_NAME(TRACE_PID::MBUS, trace_id, "MBUS " + std::to_string(trace_id));
            TRACE_EVENT(last_state, "MBUS_State", TRACE_BEGIN, TRACE_PID::MBUS, trace_id, "{}");
            trace_init = true;
        }

        std::string current_state;
        bool waiting_resp = (rx_mask_reg.read() != 0);
        bool processing_req = noc0_to_bus_req.valid_in.read() || noc1_to_bus_req.valid_in.read();

        if (waiting_resp && processing_req) {
            current_state = "WAITING_RESP_PROCESSING_REQ";
        } else if (waiting_resp) {
            current_state = "WAITING_RESP";
        } else if (processing_req) {
            current_state = "PROCESSING_REQ";
        } else {
            current_state = "IDLE";
        }

        if (current_state != last_state) {
            TRACE_EVENT(last_state, "MBUS_State", TRACE_END, TRACE_PID::MBUS, trace_id, "{}");
            TRACE_EVENT(current_state, "MBUS_State", TRACE_BEGIN, TRACE_PID::MBUS, trace_id, "{}");
            last_state = current_state;
        }
    }
};

} // namespace noc
} // namespace hybridacc


/*

這份 `MBUS.hpp` 定義了一個名為 `MBUS` (Module Bus) 的 SystemC 模組。它是 **NoC Router** 與 **Process Elements (PEs)** 之間的中介層。

簡單來說，它的作用是讓一個 NoC 節點可以掛載多個 PE（預設 16 個），並負責這些 PE 的**定址、廣播、配置與數據匯流**。

以下是針對目前程式碼行為的詳細說明：

### 1. 模組架構與介面

*   **位置**: 介於 NoC Router 與 PE 陣列之間。
*   **連接性**:
    *   **上游 (NoC Side)**: 一組 `VRDIF` (接收請求) 和 `VRDOF` (發送回應) 介面。
    *   **下游 (PE Side)**: `sc_vector` 陣列，連接 N 個 PE。包含請求發送 (`bus_to_pe_req`)、回應接收 (`pe_to_bus_resp`) 以及忙碌信號 (`pe_busy`)。
*   **配置介面**: 獨立的 Scan Chain 端口 (`scan_chain_in/out`, `scan_chain_enable`)，用於串行配置每個 PE 的 ID 和模式。

### 2. 核心行為詳解

#### A. 配置機制 (Scan Chain Configuration)
MBUS 不使用記憶體映射 (Memory Mapped) 的方式來設定 PE 的 ID，而是使用 **Scan Chain**。
*   **行為**: 當 `scan_chain_enable` 為 High 時，每個 Clock Cycle 會將配置數據從 `scan_chain_in` 移入，並經過內部的移位暫存器 (`pe_scan_chain_signals_reg`) 傳遞給下一個 PE，最後從 `scan_chain_out` 輸出。
*   **配置內容**: 每個 PE 包含：
    *   `ps_id`, `pd_id`, `pli_id`, `plo_id`: 四種不同通道的 ID (用於靈活定址)。
    *   `route_mode`: 路由模式設定。
    *   `enable`: 是否啟用該 PE。
*   **輸出**: 這些配置會直接輸出到 `router_enable` 和 `router_mode` 端口，控制 PE 的行為。

#### B. 請求路由 (Request Routing: NoC -> PE)
當 NoC 送來一個請求 (`noc_to_bus_req`)，MBUS 負責決定傳給哪些 PE。邏輯在 `comb_noc_to_pe_routing` 與 `calculate_target_pe_mask` 中：

1.  **地址解碼**:
    *   **Command**: 若地址為 `0x100`，視為對所有啟用 PE 的廣播命令。
    *   **Channel & Tag**: 若非命令，則解析地址的高位 (Channel) 與低位 (Tag)。
    *   **比對**: 檢查每個 PE 對應 Channel 的 ID 是否與 Tag 相符。
2.  **多播支援 (Multicast)**:
    *   如果多個 PE 的 ID 相同且匹配，它們會同時被選中 (Bitmask 機制)。
3.  **嚴格流控 (All-or-Nothing Flow Control)**:
    *   MBUS 會檢查**所有**目標 PE 的 `ready_in` 信號。
    *   **只有當所有目標 PE 都準備好接收時**，MBUS 才會接收 NoC 的請求 (`ready_out` 拉高) 並轉發數據。這保證了多播操作的同步性，不會發生部分 PE 收到、部分沒收到的情況。

#### C. 讀取回應與仲裁 (Response Handling: PE -> NoC)
當 NoC 發送的是**讀取請求 (Read Request)** 時，MBUS 需要收集 PE 的回應並回傳給 NoC。

1.  **Mask 追蹤 (`req_mask_reg`)**:
    *   當一個讀取請求成功發送給一組 PE 後，MBUS 會將這組 PE 的 Mask 存入 `req_mask_reg`。
    *   這表示 MBUS 進入 "等待回應" 狀態，只接受來自這些特定 PE 的回應。
2.  **回應收集**:
    *   邏輯在 `comb_pe_to_noc_response`。
    *   它會監聽 Mask 中所有 PE 的 `pe_to_bus_resp`。
3.  **衝突檢測 (Collision Detection)**:
    *   **正常情況**: 如果 Mask 中只有**一個** PE 回傳有效數據，該數據會被轉發給 NoC，狀態標記為 `NOC_OK`。
    *   **衝突**: 如果 Mask 中有**多個** PE 同時回傳數據（例如廣播讀取，且多個 PE 都有數據），MBUS 會檢測到衝突，回傳 `NOC_ERROR`，且數據為 0。這是一種硬體保護機制。
4.  **寫入請求**:
    *   寫入請求 (`is_w`) 不會設定 `req_mask_reg`，因此 MBUS 不會等待 PE 的回應，視為 Fire-and-Forget (或由更上層確認)。

#### D. 回應管線 (Response Pipeline)
*   為了優化時序，從 PE 收集到的回應不會直接組合輸出到 NoC，而是先經過一級管線暫存器 (`resp_pipe_data_reg`)。
*   這切斷了 PE 回應路徑與 NoC 回應路徑的 Critical Path。

### 3. 狀態總結
MBUS 的運作可以視為一個簡單的狀態機 (雖然實作上主要是組合邏輯配合 Mask 暫存器)：
1.  **IDLE**: 等待 NoC 請求。
2.  **PROCESSING_REQ**: 收到請求，解析地址，等待目標 PE Ready。
3.  **WAITING_RESP** (僅讀取): 請求已發送，鎖定 `req_mask_reg`，等待 PE 回傳數據，期間不接受新的 NoC 請求 (因為 `noc_ready` 會被拉低)。

### 4. 程式碼關鍵片段對照
*   **L264 `comb_noc_to_pe_routing`**: 決定封包去向。
*   **L308 `comb_req_mask_update`**: 決定是否進入等待回應模式 (只有讀取會)。
*   **L333 `comb_pe_to_noc_response`**: 處理回應與衝突檢測。
*   **L414 `calculate_target_pe_mask`**: 核心的地址解碼邏輯。

*/
