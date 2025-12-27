#pragma once

#include <cstdint>
#include <systemc>
#include "utils.hpp"
#include "FIFO.hpp" // Include FIFO

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace noc {

static const int NUM_PORTS_DEFAULT = 4;

// Define Router Request/Response types
typedef request_t<sc_biguint<256>, uint16_t> router_req_t;
typedef response_t<sc_biguint<256>> router_resp_t;


// === FSM State & Registers ===
enum class RouterState {
    IDLE,
    SEND_REQ,
    COLLECT_RESP,
    PUSH_RESP
};

inline std::ostream& operator<<(std::ostream& os, RouterState state) {
    switch (state) {
        case RouterState::IDLE: return os << "IDLE";
        case RouterState::SEND_REQ: return os << "SEND_REQ";
        case RouterState::COLLECT_RESP: return os << "COLLECT_RESP";
        case RouterState::PUSH_RESP: return os << "PUSH_RESP";
        default: return os << "UNKNOWN";
    }
}

inline void sc_trace(sc_core::sc_trace_file* tf, const RouterState& state, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(state), name);
}


SC_MODULE(NoCRouter) {
public:
    // Ports
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // ===  NoC Router interface ports ===
    // Command and configuration ports
    // For scan-chain programming and PE programming (addr = 0x100)
    sc_in<bool> command_mode;
    sc_in<sc_uint<32>> command_data;

    // New Valid-Ready Interface
    VRDIF<router_req_t> req_in;
    VRDOF<router_resp_t> resp_out;

    // ===  NoC MBUS interface ports ===
    // NoC interface ports - using VRDIF/VRDOF
    sc_vector<VRDOF<noc_request_t>> noc_to_bus_req; // .addr([8] cmd, [7:6] channel, [5:0] tag)
    sc_vector<VRDIF<noc_response_t>> bus_to_noc_resp;

    // ID, mode, enable scan-chain ports
    sc_out<bool> scan_chain_enable; // broadcast to all ports
    sc_vector<sc_in<ScanChainFormat>> scan_chain_in;
    sc_vector<sc_out<ScanChainFormat>> scan_chain_out;

    size_t num_ports;

    SC_HAS_PROCESS(NoCRouter);
    NoCRouter(sc_module_name name, size_t num_ports)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          command_mode("command_mode"),
          command_data("command_data"),
          req_in("req_in"),
          resp_out("resp_out"),
          noc_to_bus_req("noc_to_bus_req", num_ports),
          bus_to_noc_resp("bus_to_noc_resp", num_ports),
          scan_chain_enable("scan_chain_enable"),
          scan_chain_in("scan_chain_in", num_ports),
          scan_chain_out("scan_chain_out", num_ports),
          num_ports(num_ports),
          scan_chain_data_reg("scan_chain_data_reg"),
          scan_chain_data_next("scan_chain_data_next"),
          scan_chain_enable_reg("scan_chain_enable_reg"),
          scan_chain_enable_next("scan_chain_enable_next"),
          pending_read_reg("pending_read_reg"),
          pending_read_next("pending_read_next"),
          pending_read_simd_reg("pending_read_simd_reg"),
          pending_read_simd_next("pending_read_simd_next"),
          rx_stall_sig("rx_stall_sig"),
          req_fifo("req_fifo", 4),
          resp_fifo("resp_fifo", 4)
    {
        DEBUG_MSG("[Create] NoCRouter with " << num_ports << " ports", DEBUG_LEVEL_NOC_COMPONENTS);

        std::cout << "NoCRouter: Initializing with " << num_ports << " ports" << std::endl;

        // Bind FIFOs
        req_fifo.clk(clk);
        req_fifo.reset_n(reset_n);
        req_fifo.data_in(req_fifo_in_sig);
        req_fifo.push(req_fifo_push_sig);
        req_fifo.data_out(req_fifo_out_sig);
        req_fifo.pop(req_fifo_pop_sig);
        req_fifo.empty(req_fifo_empty_sig);
        req_fifo.full(req_fifo_full_sig);

        resp_fifo.clk(clk);
        resp_fifo.reset_n(reset_n);
        resp_fifo.data_in(resp_fifo_in_sig);
        resp_fifo.push(resp_fifo_push_sig);
        resp_fifo.data_out(resp_fifo_out_sig);
        resp_fifo.pop(resp_fifo_pop_sig);
        resp_fifo.empty(resp_fifo_empty_sig);
        resp_fifo.full(resp_fifo_full_sig);

        // Register sequential process
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // Request Processing (Tx)
        SC_METHOD(process_requests);
        sensitive << req_fifo_empty_sig << req_fifo_out_sig << rx_stall_sig
                  << command_mode << command_data;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << noc_to_bus_req[i].ready_in;
        }

        // Response Processing (Rx)
        SC_METHOD(process_responses);
        sensitive << pending_read_reg << pending_read_simd_reg << resp_fifo_full_sig;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << bus_to_noc_resp[i].valid_in << bus_to_noc_resp[i].data_in;
        }

        // Combinational processes
        SC_METHOD(comb_command_process);
        sensitive << command_mode << command_data << scan_chain_enable_reg;

        SC_METHOD(comb_scan_chain_output);
        sensitive << scan_chain_enable_reg << scan_chain_data_reg;
        for (size_t i = 0; i < num_ports; ++i) {
            sensitive << scan_chain_in[i];
        }

        // Interface logic (connect ports to FIFOs)
        SC_METHOD(comb_interface_logic);
        sensitive << req_in.valid_in << req_in.data_in << req_fifo_full_sig
                  << resp_out.ready_in << resp_fifo_empty_sig << resp_fifo_out_sig;

        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    // Overloaded constructor with default number of PEs
    NoCRouter(sc_module_name name)
        : NoCRouter(name, NUM_PORTS_DEFAULT) // Default to 4 PEs
    {}

private:
    // === FIFOs ===
    hybridacc::pe::FIFO<router_req_t> req_fifo;
    hybridacc::pe::FIFO<router_resp_t> resp_fifo;

    // FIFO Signals
    sc_signal<router_req_t> req_fifo_in_sig;
    sc_signal<bool> req_fifo_push_sig;
    sc_signal<router_req_t> req_fifo_out_sig;
    sc_signal<bool> req_fifo_pop_sig;
    sc_signal<bool> req_fifo_empty_sig;
    sc_signal<bool> req_fifo_full_sig;

    sc_signal<router_resp_t> resp_fifo_in_sig;
    sc_signal<bool> resp_fifo_push_sig;
    sc_signal<router_resp_t> resp_fifo_out_sig;
    sc_signal<bool> resp_fifo_pop_sig;
    sc_signal<bool> resp_fifo_empty_sig;
    sc_signal<bool> resp_fifo_full_sig;

    // === Sequential Elements ===
    sc_signal<ScanChainFormat> scan_chain_data_reg;
    sc_signal<ScanChainFormat> scan_chain_data_next;
    sc_signal<bool> scan_chain_enable_reg;
    sc_signal<bool> scan_chain_enable_next;

    sc_signal<bool> pending_read_reg;
    sc_signal<bool> pending_read_next;
    sc_signal<bool> pending_read_simd_reg;
    sc_signal<bool> pending_read_simd_next;

    // Internal signal for Rx stall
    sc_signal<bool> rx_stall_sig;

    // === Sequential Process ===
    void seq_process() {
        // Reset initialization
        ScanChainFormat init_config;
        init_config.ps_id = 0;
        init_config.pd_id = 0;
        init_config.pli_id = 0;
        init_config.plo_id = 0;
        init_config.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
        init_config.enable = false;
        scan_chain_data_reg.write(init_config);
        scan_chain_enable_reg.write(false);

        pending_read_reg.write(false);
        pending_read_simd_reg.write(false);

        wait();

        while (true) {
            scan_chain_data_reg.write(scan_chain_data_next.read());
            scan_chain_enable_reg.write(scan_chain_enable_next.read());
            pending_read_reg.write(pending_read_next.read());
            pending_read_simd_reg.write(pending_read_simd_next.read());
            wait();
        }
    }


    // === Interface Logic (Combinational) ===
    void comb_interface_logic() {
        // Input: Req Port -> Req FIFO
        if (req_in.valid_in.read() && !req_fifo_full_sig.read()) {
            req_in.ready_out.write(true);
            req_fifo_in_sig.write(req_in.data_in.read());
            req_fifo_push_sig.write(true);
        } else {
            req_in.ready_out.write(false);
            req_fifo_push_sig.write(false);
        }

        // Output: Resp FIFO -> Resp Port
        if (!resp_fifo_empty_sig.read()) {
            resp_out.valid_out.write(true);
            resp_out.data_out.write(resp_fifo_out_sig.read());
            if (resp_out.ready_in.read()) {
                resp_fifo_pop_sig.write(true);
            } else {
                resp_fifo_pop_sig.write(false);
            }
        } else {
            resp_out.valid_out.write(false);
            resp_fifo_pop_sig.write(false);
        }
    }

    // === Request Processing (Tx) ===
    void process_requests() {
        // Default outputs
        req_fifo_pop_sig.write(false);
        pending_read_next.write(false);
        pending_read_simd_next.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            noc_to_bus_req[i].valid_out.write(false);
            noc_to_bus_req[i].data_out.write(noc_request_t());
        }

        // Check Rx Stall
        if (rx_stall_sig.read()) {
            // Stall Tx: Hold pending read mask
            pending_read_next.write(pending_read_reg.read());
            pending_read_simd_next.write(pending_read_simd_reg.read());
            return;
        }

        // 1. Sideband Command (High Priority)
        bool cmd_active = command_mode.read();
        sc_uint<32> cmd_val = command_data.read();
        message_command_t cmd_type = static_cast<message_command_t>(cmd_val.range(3, 0).to_uint());
        bool is_pe_cmd = cmd_active && (cmd_type != message_command_t::CMD_NOC_SCAN_CHAIN);

        if (is_pe_cmd) {
            noc_request_t cmd_req;
            cmd_req.addr = 0x100;
            cmd_req.data = cmd_val;
            cmd_req.is_w = true;

            for (size_t i = 0; i < num_ports; ++i) {
                noc_to_bus_req[i].valid_out.write(true);
                noc_to_bus_req[i].data_out.write(cmd_req);
            }
            return; // Block FIFO requests
        }

        // 2. FIFO Request
        if (!req_fifo_empty_sig.read()) {
            router_req_t req = req_fifo_out_sig.read();

            // Decode
            bool is_simd = (req.addr >> 8) & 0x1;
            sc_uint<8> base_addr = req.addr & 0xFF;
            bool is_write = req.is_w;
            size_t mask = req.mask;

            // Broadcast / SIMD
            for (size_t i = 0; i < num_ports; ++i) {
                if (is_simd) {
                    assert(num_ports <= 4); // SIMD mode only supports up to 4 ports
                    noc_request_t r;
                    r.addr = base_addr;
                    r.data = req.data.range(64*i + 63, 64*i).to_uint64();
                    r.is_w = is_write;
                    r.mask = mask;
                    noc_to_bus_req[i].data_out.write(r);
                } else { // Broadcast
                    noc_request_t r;
                    r.addr = base_addr;
                    r.data = req.data.range(63, 0).to_uint64();
                    r.is_w = is_write;
                    r.mask = mask;
                    noc_to_bus_req[i].data_out.write(r);
                }
            }

            // Check Readiness
            bool all_ready = true;
            for (size_t i = 0; i < num_ports; ++i) {
                if (!noc_to_bus_req[i].ready_in.read()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                // Send Request
                for (size_t i = 0; i < num_ports; ++i) {
                    noc_to_bus_req[i].valid_out.write(true);
                }
                req_fifo_pop_sig.write(true);

                if (!is_write) {
                    pending_read_next.write(true);
                    pending_read_simd_next.write(is_simd);
                }
            }
        }
    }

    // === Response Processing (Rx) ===
    void process_responses() {
        // Default outputs
        resp_fifo_push_sig.write(false);
        resp_fifo_in_sig.write(router_resp_t());
        rx_stall_sig.write(false);

        for (size_t i = 0; i < num_ports; ++i) {
            bus_to_noc_resp[i].ready_out.write(false);
        }

        bool is_pending_read = pending_read_reg.read();
        bool is_simd = pending_read_simd_reg.read();

        if (!is_pending_read) return;

        // Check if all expected responses are valid
        bool all_valid = true;
        sc_biguint<256> collected_data = 0;
        uint64_t valid_rx = 0;
        bool error_flag = false;

        for (size_t i = 0; i < num_ports; ++i) {
            if(bus_to_noc_resp[i].valid_in.read()) {
                valid_rx |= (1ULL << i);
                noc_response_t resp = bus_to_noc_resp[i].data_in.read();
                if (resp.status == NOC_RESPONSE_STATUS::NOC_ERROR) {
                    error_flag = true;
                }
                collected_data.range(64*i + 63, 64*i) = resp.data;
            }
        }

        if (is_simd) {
            // SIMD mode: expect response from all ports
            for (size_t i = 0; i < num_ports; ++i) {
                if (!(valid_rx & (1ULL << i))) {
                    all_valid = false;
                    break;
                }
            }
        } else {
            // Broadcast mode: expect response from only one of ports
            bool resp_received = false;
            for (size_t i = 0; i < num_ports; ++i) {
                if (valid_rx & (1ULL << i)) {
                    if (resp_received) {
                        // More than one response received -> error
                        error_flag = true;
                    }
                    resp_received = true;
                    collected_data.range(63, 0) = bus_to_noc_resp[i].data_in.read().data;
                }
            }
            if (!resp_received) {
                all_valid = false;
            }
        }





        for (size_t i = 0; i < num_ports; ++i) {
            DEBUG_MSG("[NoCRouter] Checking response from port " << i
                      << ": valid=" << bus_to_noc_resp[i].valid_in.read()
                      << ", data=0x" << std::hex << bus_to_noc_resp[i].data_in.read().data
                      << ", status=" << static_cast<int>(bus_to_noc_resp[i].data_in.read().status)
                      << std::dec, DEBUG_LEVEL_NOC_COMPONENTS);
        }

        if (all_valid) {
            if (!resp_fifo_full_sig.read()) {
                // Accept responses
                for (size_t i = 0; i < num_ports; ++i) {
                    if (valid_rx & (1ULL << i)) {
                        bus_to_noc_resp[i].ready_out.write(true);
                    }
                }

                // Push to FIFO
                router_resp_t final_resp;
                final_resp.data = collected_data;
                // Note: router_resp_t doesn't have status field in current definition,
                // assuming data is enough or error handling is done elsewhere/ignored for now.

                resp_fifo_push_sig.write(true);
                resp_fifo_in_sig.write(final_resp);

                // rx_stall_sig remains false (default)
            } else {
                // FIFO full, cannot accept -> Stall
                rx_stall_sig.write(true);
            }
        } else {
            // Waiting for responses -> Stall
            rx_stall_sig.write(true);

            // Note: We keep ready_out low, so MBUS holds the data.
        }
    }

    // === Combinational: Command Processing ===
    void comb_command_process() {
        bool cmd_mode = command_mode.read();
        sc_uint<32> cmd_data = command_data.read();

        if (cmd_mode) {
            message_command_t cmd_type = static_cast<message_command_t>(cmd_data.range(3, 0).to_uint());

            if (cmd_type == message_command_t::CMD_NOC_SCAN_CHAIN) {
                // Parse scan-chain data from command_data
                ScanChainFormat sc_format = parse_scan_chain_data(cmd_data.to_uint());
                scan_chain_data_next.write(sc_format);
                scan_chain_enable_next.write(true);

                DEBUG_MSG("[NoCRouter] Scan-chain command received: "  << sc_format, DEBUG_LEVEL_NOC_COMPONENTS);

            } else {
                // PE command: keep scan-chain state
                scan_chain_data_next.write(scan_chain_data_reg.read());
                scan_chain_enable_next.write(false);
            }
        } else {
            // Normal mode: keep scan-chain state, disable scan-chain
            scan_chain_data_next.write(scan_chain_data_reg.read());
            scan_chain_enable_next.write(false);
        }
    }

    // === Combinational: Scan-chain Output ===
    void comb_scan_chain_output() {
        bool scan_en = scan_chain_enable_reg.read();
        ScanChainFormat sc_data = scan_chain_data_reg.read();

        scan_chain_enable.write(scan_en);

        if (scan_en) {
            // First port gets the data from register
            scan_chain_out[0].write(sc_data);

            // Chain the scan data through all ports
            for (size_t i = 1; i < num_ports; ++i) {
                scan_chain_out[i].write(scan_chain_in[i-1].read());
            }
        } else {
            // Not in scan mode: output default
            ScanChainFormat default_out;
            default_out.ps_id = 0;
            default_out.pd_id = 0;
            default_out.pli_id = 0;
            default_out.plo_id = 0;
            default_out.route_mode = PERouterMode::PLI_FROM_LN_PLO_TO_LN;
            default_out.enable = false;

            for (size_t i = 0; i < num_ports; ++i) {
                scan_chain_out[i].write(default_out);
            }
        }
    }

    // Trace support
    int trace_id = 0;
    std::string last_state_str = "IDLE";
    bool trace_init = false;

public:
    void set_trace_id(int id) { trace_id = id; }

private:
    void trace_process() {
        if (!trace_init) {
            TRACE_THREAD_NAME(TRACE_PID::NOC_ROUTER, trace_id, "NoC Router");
            TRACE_EVENT(last_state_str, "NoC_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, trace_id, "{}");
            trace_init = true;
        }

        // Map enum to string
        std::string current_state_str;
        if (req_fifo_empty_sig.read() && pending_read_reg.read() == false) {
            current_state_str = "IDLE";
        } else if (!req_fifo_empty_sig.read()) {
            current_state_str = "PROCESSING_REQ";
        } else if (pending_read_reg.read() != false) {
            current_state_str = "WAITING_RESP";
        } else {
            current_state_str = "PROCESSING_REQ_WAITING_RESP";
        }

        if (current_state_str != last_state_str) {
            TRACE_EVENT(last_state_str, "NoC_State", TRACE_END, TRACE_PID::NOC_ROUTER, trace_id, "{}");
            TRACE_EVENT(current_state_str, "NoC_State", TRACE_BEGIN, TRACE_PID::NOC_ROUTER, trace_id, "{}");
            last_state_str = current_state_str;
        }
    }
};

} // namespace noc
} // namespace hybridacc

/*

新版 `NoCRouter` 的詳細行為解釋：

### 1. 核心架構 (Core Architecture)

`NoCRouter` 是 NoC 的大腦，負責協調上層控制器與下層 MBUS/PE 之間的通訊。它現在採用 **混合式處理架構 (Hybrid Processing Architecture)**：

*   **數據路徑 (Data Path)**：透過 FIFO 緩衝，支援全雙工流控 (Full-Duplex Flow Control)，保證數據不丟失。
*   **控制路徑 (Control Path)**：透過 Sideband (旁路) 介面，支援高優先級的廣播命令，繞過 FIFO 直接執行。

### 2. 介面定義 (Interfaces)

*   **數據介面 (Data Interface)**：
    *   `req_in` (Valid/Ready)：接收來自上層的讀寫請求。
    *   `resp_out` (Valid/Ready)：回傳讀取結果或寫入確認。
*   **控制介面 (Control Interface)**：
    *   `command_mode` & `command_data`：用於發送全局控制命令 (如 Start/Stop PE) 或配置 Scan Chain。
*   **下層介面 (Downstream Interface)**：
    *   `noc_to_bus_req` & `bus_to_noc_resp`：連接到多個 MBUS，使用標準握手協定。

### 3. 詳細行為流程 (Life of a Request)

`process_thread` 是核心執行緒，它在每個時脈週期都會檢查並執行以下邏輯：

#### A. 優先處理 Sideband Command (High Priority)
這是最新的變更點。執行緒會**優先**檢查 `command_mode` 是否為高：

1.  **檢測**：若 `command_mode=1` 且指令類型**不是** `CMD_NOC_SCAN_CHAIN`（例如 `CMD_START_PE`, `CMD_LOAD_PROGRAM`）。
2.  **構建請求**：立即構建一個廣播寫入請求 (`addr=0x100`, `is_w=true`)。
3.  **廣播 (Broadcast)**：
    *   將請求同時送往所有 `MBUS` 端口。
    *   **同步等待**：Router 會暫停並等待，直到**所有** MBUS 都回傳 Ready，確保命令被所有 PE 接收。
4.  **完成**：命令發送完畢後，結束該週期的處理。這意味著 Sideband Command 會暫時阻塞 FIFO 中的數據處理，確保控制命令的即時性。

#### B. 處理 FIFO 數據請求 (Normal Priority)
如果沒有 Sideband Command，Router 才會檢查 `req_fifo`：

1.  **取出請求 (Pop)**：從 FIFO 取出一個 `router_req_t`。
2.  **解碼 (Decode)**：
    *   **SIMD (addr[8]=1)**：將 256-bit 數據切分為 4x64-bit，分別送給 Port 0-3。
    *   **Broadcast (addr[8]=0)**：將低 64-bit 數據複製送給所有端口。
    *   **Command (addr[9]=1)**：視為廣播寫入。
3.  **發送 (Dispatch)**：
    *   將請求送往目標 MBUS 端口。
    *   同樣執行**同步等待**，直到所有目標 MBUS 都接收 (Ready=1)。
4.  **回應收集 (Response Collection)**：
    *   **寫入**：立即生成 `NOC_OK`。
    *   **讀取**：進入收集迴圈，等待所有目標 MBUS 回傳數據，並將其聚合 (Aggregation) 成一個 256-bit 的回應包。
5.  **回傳**：將結果推入 `resp_fifo`。

#### C. Scan Chain 配置 (Configuration)
*   `CMD_NOC_SCAN_CHAIN` 指令由獨立的組合邏輯 (`comb_command_process`) 處理。
*   它不經過 `process_thread`，也不會阻塞數據流，專門用於設定 Router 內部的路由表與 ID。

### 4. 設計特點總結

1.  **雙軌制 (Dual-Track)**：
    *   **數據流**走 FIFO，保證吞吐量與順序。
    *   **控制流**走 Sideband，保證低延遲與高優先級。
2.  **絕對同步 (Strict Synchronization)**：
    *   無論是廣播數據還是命令，Router 都會強制等待所有目標 Ready。這雖然可能被最慢的 PE 拖慢，但保證了系統狀態的一致性 (Lock-step behavior)。
3.  **無阻塞 (Non-Blocking I/O)**：
    *   對上層而言，只要 FIFO 沒滿，就可以一直送數據，不用管下層 PE 是否忙碌。Router 會自動處理內部的背壓 (Backpressure)。

*/