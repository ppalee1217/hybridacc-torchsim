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
    sc_vector<VRDOF<noc_request_t>> bus_to_pe_req;
    sc_vector<VRDIF<noc_response_t>> pe_to_bus_resp;

    // Control ports
    sc_vector<sc_in<bool>> pe_busy;

    // ===  NoC Router interface ports ===
    // NoC interface ports - using VRDIF/VRDOF
    VRDIF<noc_request_t> noc_to_bus_req;
    VRDOF<noc_response_t> bus_to_noc_resp;

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
          bus_to_pe_req("bus_to_pe_req", num_pes),
          pe_to_bus_resp("pe_to_bus_resp", num_pes),
          router_enable("router_enable", num_pes),
          router_mode("router_mode", num_pes),
          pe_busy("pe_busy", num_pes),
          noc_to_bus_req("noc_to_bus_req"),
          bus_to_noc_resp("bus_to_noc_resp"),
          scan_chain_enable("scan_chain_enable"),
          scan_chain_in("scan_chain_in"),
          scan_chain_out("scan_chain_out"),
          pe_scan_chain_signals_reg("pe_scan_chain_signal_reg", num_pes),
          pe_scan_chain_signals_next("pe_scan_chain_signal_next", num_pes) {

        DEBUG_MSG("[Create] MBUS with " << num_pes << " PEs", DEBUG_LEVEL_NOC_COMPONENTS);

        std::cout << "MBUS: Initializing with " << num_pes << " PEs" << std::endl;

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

        // NoC request routing to PEs
        SC_METHOD(comb_noc_to_pe_routing);
        sensitive << noc_to_bus_req.valid_in << noc_to_bus_req.data_in << scan_chain_enable << req_mask_reg;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_scan_chain_signals_reg[i]
                      << bus_to_pe_req[i].ready_in;
        }

        // Request Mask Update Logic
        SC_METHOD(comb_req_mask_update);
        sensitive << req_mask_reg << noc_to_bus_req.valid_in << noc_to_bus_req.data_in
                  << target_pe_mask_sig << bus_to_noc_resp.ready_in << bus_to_noc_resp.valid_out
                  << noc_to_bus_req.ready_out; // Note: reading own output

        // PE response to NoC (including collision detection)
        SC_METHOD(comb_pe_to_noc_response);
        sensitive << noc_to_bus_req.valid_in << noc_to_bus_req.data_in
                  << bus_to_noc_resp.ready_in << scan_chain_enable << req_mask_reg;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_to_bus_resp[i].valid_in
                      << pe_to_bus_resp[i].data_in;
        }

        // PE response ready signals
        SC_METHOD(comb_pe_response_ready);
        sensitive << bus_to_noc_resp.ready_in << req_mask_reg;
        for (size_t i = 0; i < num_pes; ++i) {
            sensitive << pe_to_bus_resp[i].valid_in;
        }

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
    sc_signal<uint64_t> req_mask_reg;
    sc_signal<uint64_t> req_mask_next;

    // Internal signal to share calculated mask
    sc_signal<uint64_t> target_pe_mask_sig;

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
        req_mask_reg.write(0);

        wait();

        // Sequential logic
        while (true) {
            for (size_t i = 0; i < num_pes; ++i) {
                pe_scan_chain_signals_reg[i].write(pe_scan_chain_signals_next[i].read());
            }
            req_mask_reg.write(req_mask_next.read());

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

    // === Combinational: NoC Request to PE Routing ===
    void comb_noc_to_pe_routing() {
        bool scan_mode = scan_chain_enable.read();
        bool noc_valid = noc_to_bus_req.valid_in.read();
        noc_request_t noc_req = noc_to_bus_req.data_in.read();
        uint64_t current_mask = req_mask_reg.read();

        // Calculate target PE mask
        uint64_t target_mask = 0;
        if (noc_valid && !scan_mode) {
            target_mask = calculate_target_pe_mask(noc_req.addr);
            DEBUG_MSG("Routing data: 0x" << std::hex << noc_req.data << ", Addr 0x" << noc_req.addr << ", Mask 0x" << target_mask << std::dec << ", Mode: " << (noc_req.is_w ? "Write" : "Read"), DEBUG_LEVEL_NOC_COMPONENTS);
        }
        target_pe_mask_sig.write(target_mask);

        // Check if all target PEs are ready
        bool all_ready = true;
        if (target_mask != 0) {
            for (size_t i = 0; i < num_pes; ++i) {
                if (target_mask & (1ULL << i)) {
                    if (!bus_to_pe_req[i].ready_in.read()) {
                        all_ready = false;
                        break;
                    }
                }
            }
        }

        // Set NoC ready signal
        // Ready if not in scan mode, PEs are ready, AND we are not waiting for a response (mask == 0)
        bool noc_ready = !scan_mode && (!noc_valid || all_ready) && (current_mask == 0);
        noc_to_bus_req.ready_out.write(noc_ready);

        // Route request to target PEs
        for (size_t i = 0; i < num_pes; ++i) {
            bool is_target = (target_mask & (1ULL << i)) != 0;
            bool send_to_pe = noc_valid && !scan_mode && is_target && all_ready && (current_mask == 0);

            bus_to_pe_req[i].data_out.write(noc_req);
            bus_to_pe_req[i].valid_out.write(send_to_pe);
        }
    }

    // === Combinational: Request Mask Update ===
    void comb_req_mask_update() {
        uint64_t current_mask = req_mask_reg.read();
        uint64_t next_mask = current_mask;

        bool noc_req_valid = noc_to_bus_req.valid_in.read();
        bool noc_req_ready = noc_to_bus_req.ready_out.read(); // From comb_noc_to_pe_routing
        bool is_write = noc_to_bus_req.data_in.read().is_w;
        uint64_t target_mask = target_pe_mask_sig.read();

        bool noc_resp_valid = bus_to_noc_resp.valid_out.read(); // From comb_pe_to_noc_response
        bool noc_resp_ready = bus_to_noc_resp.ready_in.read();

        // Logic:
        // 1. If Request Accepted (Valid & Ready) AND Read -> Set Mask
        // 2. If Response Sent (Valid & Ready) -> Clear Mask

        if (noc_req_valid && noc_req_ready && !is_write) {
            next_mask = target_mask;
        } else if (noc_resp_valid && noc_resp_ready) {
            next_mask = 0;
        }

        req_mask_next.write(next_mask);
    }

    // === Combinational: PE Response to NoC (with collision detection) ===
    void comb_pe_to_noc_response() {
        bool scan_mode = scan_chain_enable.read();
        noc_request_t noc_req = noc_to_bus_req.data_in.read();
        bool is_write = noc_req.is_w;
        bool noc_req_valid = noc_to_bus_req.valid_in.read();
        uint64_t active_mask = req_mask_reg.read();

        // Check which PEs have valid responses
        uint64_t resp_mask = 0;
        noc_response_t pe_resp;
        pe_resp.data = 0;
        pe_resp.status = NOC_RESPONSE_STATUS::NOC_NOP;

        for (size_t i = 0; i < num_pes; ++i) {
            // Only consider PEs that are in the active mask
            if ((active_mask & (1ULL << i)) && pe_to_bus_resp[i].valid_in.read()) {
                resp_mask |= (1ULL << i);
                if (resp_mask == (1ULL << i)) { // First response
                    pe_resp = pe_to_bus_resp[i].data_in.read();
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
        } else if (noc_req_valid && is_write) {
            // Write request: immediate ACK (Combinational bypass)
            noc_resp.data = 0;
            noc_resp.status = NOC_RESPONSE_STATUS::NOC_OK;
            noc_resp_valid = true;
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

        bus_to_noc_resp.data_out.write(noc_resp);
        bus_to_noc_resp.valid_out.write(noc_resp_valid);
    }

    // === Combinational: PE Response Ready Signals ===
    void comb_pe_response_ready() {
        // Only ready if NoC is ready to accept response AND PE is in the active mask
        bool noc_ready = bus_to_noc_resp.ready_in.read();
        uint64_t active_mask = req_mask_reg.read();

        for (size_t i = 0; i < num_pes; ++i) {
            bool is_active = (active_mask & (1ULL << i)) != 0;
            pe_to_bus_resp[i].ready_out.write(noc_ready && is_active);
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
            TRACE_THREAD_NAME(1, trace_id, "MBUS " + std::to_string(trace_id));
            TRACE_EVENT(last_state, "MBUS_State", "B", 1, trace_id, "{}");
            trace_init = true;
        }

        std::string current_state;
        if (req_mask_reg.read() != 0) {
            current_state = "WAITING_RESP";
        } else if (noc_to_bus_req.valid_in.read()) {
            current_state = "PROCESSING_REQ";
        } else {
            current_state = "IDLE";
        }

        if (current_state != last_state) {
            TRACE_EVENT(last_state, "MBUS_State", "E", 1, trace_id, "{}");
            TRACE_EVENT(current_state, "MBUS_State", "B", 1, trace_id, "{}");
            last_state = current_state;
        }
    }
};

} // namespace noc
} // namespace hybridacc


/*

`MBUS` (Module Bus) 是 `hybridacc` 專案中連接 **NoC Router** 與多個 **Process Elements (PEs)** 之間的中介層。

簡單來說，`MBUS` 的作用就像是一個本地的總線（Bus）或集線器，負責將來自 NoC 的封包分發給掛載在它底下的多個 PE，並收集 PE 的回應回傳給 NoC。

以下是 `MBUS` 行為的詳細解釋。
### 1. 核心功能概述
*   **一對多連接**：一個 `MBUS` 連接一個 NoC Router 端口和多個 PE（預設為 16 個）。
*   **配置管理 (Scan Chain)**：透過掃描鏈 (Scan Chain) 機制來配置每個 PE 的 ID 和路由模式。
*   **請求路由 (NoC -> PE)**：根據 NoC 請求中的地址，將數據轉發給符合條件的一個或多個 PE。
*   **回應仲裁 (PE -> NoC)**：處理 PE 對讀取/寫入請求的回應，並檢測讀取衝突。

### 2. 詳細行為分析

#### A. 配置機制 (Scan Chain)
`MBUS` 內部不透過標準的記憶體映射寫入來配置 PE，而是使用一條串行的 **Scan Chain**。
*   **暫存器**：每個 PE 在 `MBUS` 內都有一組配置暫存器 (`pe_scan_chain_signals_reg`)，包含：
    *   `ps_id`, `pd_id`, `pli_id`, `plo_id`：不同通道的 ID。
    *   `route_mode`：路由模式。
    *   `enable`：是否啟用該 PE。
*   **運作方式**：
    *   當 `scan_chain_enable` 為高電位時，數據會像移位暫存器一樣，從 `scan_chain_in` 進入，逐個 PE 傳遞，最後從 `scan_chain_out` 輸出。
    *   這允許系統在初始化階段串行地設定所有 PE 的屬性。

#### B. 請求路由 (NoC to PE) - `comb_noc_to_pe_routing`
當 NoC Router 送來一個請求 (`noc_to_bus_req`) 時，`MBUS` 會決定哪些 PE 應該接收這個請求：
1.  **地址解碼**：透過 `calculate_target_pe_mask` 函數解析地址。
    *   **Channel**：地址的第 6-7 位元決定通道類型 (PS, PD, PLI, PLO)。
    *   **Tag**：地址的低 6 位元是目標 ID。
    *   **Command**：如果地址是 `0x100`，則視為廣播命令。
2.  **目標匹配**：
    *   它會檢查每個 PE 的配置。如果 PE 被啟用 (`enable`) 且其對應通道的 ID 與請求中的 Tag 相符，該 PE 就會被選中。
    *   這支援 **Multicast (多播)**：如果多個 PE 配置了相同的 ID，它們都會收到數據。
3.  **流控 (Flow Control)**：
    *   `MBUS` 會檢查所有目標 PE 的 `ready_in` 信號。
    *   只有當**所有**目標 PE 都準備好接收時，`MBUS` 才會向 NoC 發送 `ready_out`，並將數據轉發給 PE。這確保了數據的一致性。

#### C. 回應處理 (PE to NoC) - `comb_pe_to_noc_response`
處理從 PE 回傳給 NoC 的訊號：
1.  **寫入請求 (Write)**：
    *   如果是寫入操作 (`is_w` 為真)，`MBUS` 不需要等待 PE 的實際數據回應，它會立即向 NoC 回傳 `NOC_OK` 作為寫入確認 (Write Acknowledge)。
2.  **讀取請求 (Read)**：
    *   如果是讀取操作，`MBUS` 會監聽所有 PE 的 `pe_to_bus_resp`。
    *   **正常情況**：如果只有**一個** PE 回傳有效數據，該數據會被轉發給 NoC，狀態為 `NOC_OK`。
    *   **衝突檢測 (Collision)**：如果有多個 PE 同時回傳數據（例如多個 PE 有相同的 ID 且被讀取），`MBUS` 會檢測到衝突，並回傳 `NOC_ERROR` 給 NoC。這是一種錯誤保護機制。
    *   **等待**：如果沒有 PE 回應，則回傳 `NOC_NOP`。

#### D. 輸出控制
*   `comb_router_config`：將內部暫存器的配置值直接輸出到 `router_enable` 和 `router_mode` 端口，這些信號會直接控制連接的 PE 硬體行為。

### 總結
`MBUS` 是一個智慧型的路由器擴展器。它允許系統設計者將多個 PE 視為一個群組掛載在 NoC 的單個節點上，並透過靈活的 ID 配置實現單播、多播或廣播，同時處理基本的流控和錯誤檢測。

*/
