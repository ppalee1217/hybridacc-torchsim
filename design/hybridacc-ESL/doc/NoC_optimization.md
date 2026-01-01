# NoC Optimization Plan: Dual-Plane Architecture

## 1. Overview
The current NoC architecture uses a single channel to handle four distinct types of traffic: Port Static (PS), Port Dynamic (PD), Port Local Input (PLI), and Port Local Output (PLO), along with control commands. To alleviate bandwidth bottlenecks, this plan proposes splitting the NoC into two independent physical planes (Virtual Channels implemented as separate networks).

- **NoC-0 (Control & Push Plane)**: Handles write-only traffic for data feeding (PS, PD) and system control.
- **NoC-1 (Local Network Plane)**: Handles read/write traffic for inter-PE communication (PLI, PLO).

## 2. Architecture Specification

### 2.1 NoC-0: Control & Push Plane
*   **Traffic Types**:
    *   **PS (Port Static)**: Weight/Constant data loading (Write Only).
    *   **PD (Port Dynamic)**: Input activation streaming (Write Only).
    *   **Control Commands**: PE Reset, Start, Stop, Program Load (Write Only).
*   **Characteristics**:
    *   **Direction**: Host -> PE (Unidirectional).
    *   **Response**: None (Fire-and-forget).
    *   **Bandwidth**: High (for burst data loading).

### 2.2 NoC-1: Local Network Plane
*   **Traffic Types**:
    *   **PLI (Port Local Input)**: Injecting data into the systolic/local ring (Write Only).
    *   **PLO (Port Local Output)**: Retrieving results from the systolic/local ring (Read Only).
*   **Characteristics**:
    *   **Direction**: Bidirectional (Host <-> PE).
    *   **Response**: Required for PLO Read operations.
    *   **Bandwidth**: Moderate to High (depending on reduction/output frequency).

## 3. Module Modifications

### 3.1 NetworkOnChip (Top Level)
The top-level module will instantiate two separate router networks and MBus arrays.

*   **Ports**:
    *   Remove: `req_in`, `resp_out`
    *   Add:
        *   `req0_in` (`VRDIF<noc::router_req_t>`): Input for NoC-0.
        *   `req1_in` (`VRDIF<noc::router_req_t>`): Input for NoC-1.
        *   `resp1_out` (`VRDOF<noc::router_resp_t>`): Output for NoC-1 responses.
*   **Internal Structure**:
    *   **Routers**: Instantiate `router0` and `router1`.
    *   **MBus**: Instantiate `mbus0` (connected to `router0`) and `mbus1` (connected to `router1`).
    *   **Scan Chain**: The scan chain configuration must be applied to **both** `mbus0` and `mbus1` to ensure they both know the PE IDs. This can be done by daisy-chaining them or feeding them in parallel if the config format allows. (Recommendation: Daisy-chain `mbus0` then `mbus1` or parallel feed if identical config).

### 3.2 MBUS

### Design Updates
1. **Dual-Channel Architecture**:
   - The MBUS module will support two distinct channels:
     - **PS/PD Channel**: Write-only, utilizing combinational logic for multicast data transmission.
     - **PLI/PLO Channel**: Supports both read and write operations, leveraging the existing pipelined MBUS design.
2. **Internal Structure**:
   - **Coexistence of Dual Channels**: Both PS/PD and PLI/PLO channels are integrated within the same MBUS module but are controlled by separate logic and registers.
   - **Combinational Logic for PS/PD**: Since PS/PD is write-only and requires no response, simplified combinational logic is used for multicast.
   - **Pipelined Design for PLI/PLO**: The PLI/PLO channel supports bidirectional operations, retaining the current pipelined design to ensure synchronized and efficient data transmission.
3. **Scan-Chain Configuration**:
   - **Single Scan-Chain**: The MBUS retains a single scan-chain configuration that includes all channel settings.
   - **ID Configuration**:
     - PS/PD Channel: Only references the IDs for PS/PD.
     - PLI/PLO Channel: Read/write operations reference the IDs for PLI/PLO.

### 3.3 ProcessElement (PE)
The PE interface needs to be updated to accept two request streams.

*   **Ports**:
    *   Remove: `noc_req`, `noc_resp`
    *   Add:
        *   `noc0_req` (`VRDIF<noc_request_t>`): From MBus-0.
        *   `noc1_req` (`VRDIF<noc_request_t>`): From MBus-1.
        *   `noc1_resp` (`VRDOF<noc_response_t>`): To MBus-1.

### 3.4 PErouter
The PE Router requires significant logic updates to split the traffic handling.

*   **Ports**:
    *   Update inputs/outputs to match PE's new ports (`noc0_req_in`, `noc1_req_in`, `noc1_resp_out`).
*   **Logic - NoC-0 Path**:
    *   Listens to `noc0_req_in`.
    *   Decodes Address:
        *   If `Command Address` (0x100): Route to Internal Control (Reset, Start, etc.).
        *   If `PS Address`: Route to `pe_ps_out`.
        *   If `PD Address`: Route to `pe_pd_out`.
    *   No response generation.
*   **Logic - NoC-1 Path**:
    *   Listens to `noc1_req_in`.
    *   Decodes Address:
        *   If `PLI Address`: Route to `pe_pli_out` (Write).
        *   If `PLO Address`: Route to `pe_plo_in` (Read).
    *   **Response Handling**:
        *   Collects data from `pe_plo_in` (for Read ops).
        *   Generates response packets on `noc1_resp_out`.

## 4. Addressing & Protocol Updates

### 4.1 Address Space Optimization
Since traffic is physically separated, the 2-bit Channel ID (`[7:6]`) in the address can be simplified or repurposed.

*   **Old Mapping**:
    *   `00`: PS
    *   `01`: PD
    *   `10`: PLI
    *   `11`: PLO

*   **New Mapping (Per Plane)**:
    *   **NoC-0**:
        *   Bit `[6]` (or LSB of channel field) selects PS vs PD.
        *   `0`: PS
        *   `1`: PD
        *   (Command address `0x100` remains special case or mapped separately).
    *   **NoC-1**:
        *   Bit `[6]` selects PLI vs PLO.
        *   `0`: PLI
        *   `1`: PLO

### 4.2 Control Flow
*   **Side-band Control**: All global control signals (Reset, Start, Stop) are strictly routed through **NoC-0**.
*   **Data Flow**:
    *   Weights/Inputs -> NoC-0.
    *   Partial Sums/Results <-> NoC-1.

## 5. Implementation Plan

1.  **Header Updates (`utils.hpp`)**:
    *   Review `NOC_CHANNELS` enum. It may remain for compatibility, but logic using it will change.
2.  **PE Router Update**:
    *   Modify `PErouter.hpp` to add new ports.
    *   Split the `process` loop into two independent threads/methods: one for NoC-0 (Push) and one for NoC-1 (Transaction).
3.  **PE Update**:
    *   Update `ProcessElement.hpp` to expose new ports and bind them to the updated `PErouter`.
4.  **NetworkOnChip Update**:
    *   Duplicate `NoCRouter` and `MBUS` instances.
    *   Update wiring in `NetworkOnChip.hpp`.
    *   Update `scan_chain` wiring to configure both MBUS sets.
5.  **Cluster/Testbench Update**:
    *   Update `pe_wrapper` and testbenches to drive the split interfaces.

---

# NoC 優化計劃：雙平面架構

## 1. 概述
目前的 NoC 架構使用單一通道來處理四種不同類型的流量：Port Static (PS)、Port Dynamic (PD)、Port Local Input (PLI) 和 Port Local Output (PLO)，以及控制命令。為了緩解頻寬瓶頸，本計劃提出將 NoC 分為兩個獨立的物理平面（虛擬通道作為獨立網路實現）。

- **NoC-0（控制與推送平面）**：處理數據加載（PS、PD）和系統控制的寫入流量。
- **NoC-1（本地網路平面）**：處理處理單元間通信的讀/寫流量（PLI、PLO）。

## 2. 架構規範

### 2.1 NoC-0：控制與推送平面
*   **流量類型**：
    *   **PS（Port Static）**：權重/常數數據加載（僅寫入）。
    *   **PD（Port Dynamic）**：輸入激活流（僅寫入）。
    *   **控制命令**：PE 重置、啟動、停止、程序加載（僅寫入）。
*   **特性**：
    *   **方向**：主機 -> PE（單向）。
    *   **回應**：無（即發即棄）。
    *   **頻寬**：高（用於突發數據加載）。

### 2.2 NoC-1：本地網路平面
*   **流量類型**：
    *   **PLI（Port Local Input）**：將數據注入到 systolic/local 環（僅寫入）。
    *   **PLO（Port Local Output）**：從 systolic/local 環中檢索結果（僅讀取）。
*   **特性**：
    *   **方向**：雙向（主機 <-> PE）。
    *   **回應**：PLO 讀取操作需要回應。
    *   **頻寬**：中等到高（取決於數據減少/輸出頻率）。

## 3. 模組修改

### 3.1 NetworkOnChip（頂層）
頂層模組將實例化兩個獨立的路由器網路和 MBus 陣列。

*   **端口**：
    *   移除： `req_in`、`resp_out`
    *   新增：
        *   `req0_in` （`VRDIF<noc::router_req_t>`）：NoC-0 的輸入。
        *   `req1_in` （`VRDIF<noc::router_req_t>`）：NoC-1 的輸入。
        *   `resp1_out` （`VRDOF<noc::router_resp_t>`）：NoC-1 回應的輸出。
*   **內部結構**：
    *   **路由器**：實例化 `router0` 和 `router1`。
    *   **MBus**：實例化 `mbus0` （連接到 `router0`）和 `mbus1` （連接到 `router1`）。
    *   **掃描鏈**：掃描鏈配置必須應用於 **mbus0** 和 **mbus1** 以確保它們都知道 PE ID。這可以通過串接它們或並行輸入來完成，如果配置格式允許的話。（建議：先串接 `mbus0` 然後是 `mbus1`，或者如果配置相同則並行輸入）。

### 3.2 MBUS

### 設計更新
1. **雙通道架構**：
   - MBUS 模組將支持兩組通道：
     - **PS/PD 通道**：單向寫入，使用組合邏輯進行多播資料傳輸。
     - **PLI/PLO 通道**：支持讀寫操作，沿用現有的 MBUS 管線化設計。
2. **模組內部結構**：
   - **雙通道共存**：PS/PD 和 PLI/PLO 通道位於同一個 MBUS 模組中，但由不同的邏輯與暫存器控制。
   - **組合邏輯處理 PS/PD**：由於 PS/PD 僅需單向寫入，無需回應，使用簡化的組合邏輯進行多播。
   - **管線化處理 PLI/PLO**：PLI/PLO 通道需要支持讀寫操作，沿用現有的管線化設計，確保數據傳輸的同步性與效率。
3. **Scan-Chain 配置**：
   - **單一 Scan-Chain**：MBUS 內部僅保留一組 Scan-Chain，包含所有通道的配置。
   - **ID 配置**：
     - PS/PD 通道：僅需參考 PS/PD 的 ID。
     - PLI/PLO 通道：讀寫操作需參考 PLI/PLO 的 ID。

### 3.3 ProcessElement（PE）
PE 介面需要更新以接受兩個請求流。

*   **端口**：
    *   移除： `noc_req`、`noc_resp`
    *   新增：
        *   `noc0_req` （`VRDIF<noc_request_t>`）：來自 MBus-0。
        *   `noc1_req` （`VRDIF<noc_request_t>`）：來自 MBus-1。
        *   `noc1_resp` （`VRDOF<noc_response_t>`）：發送到 MBus-1。

### 3.4 PErouter
PE 路由器需要顯著的邏輯更新以分離流量處理。

*   **端口**：
    *   更新輸入/輸出以匹配 PE 的新端口（`noc0_req_in`、`noc1_req_in`、`noc1_resp_out`）。
*   **邏輯 - NoC-0 路徑**：
    *   監聽 `noc0_req_in`。
    *   解碼地址：
        *   如果是 `Command Address` （0x100）：路由到內部控制（重置、啟動等）。
        *   如果是 `PS Address`：路由到 `pe_ps_out`。
        *   如果是 `PD Address`：路由到 `pe_pd_out`。
    *   無需生成回應。
*   **邏輯 - NoC-1 路徑**：
    *   監聽 `noc1_req_in`。
    *   解碼地址：
        *   如果是 `PLI Address`：路由到 `pe_pli_out` （寫入）。
        *   如果是 `PLO Address`：路由到 `pe_plo_in` （讀取）。
    *   **回應處理**：
        *   從 `pe_plo_in` 收集數據（用於讀取操作）。
        *   在 `noc1_resp_out` 上生成回應封包。

## 4. 地址與協議更新

### 4.1 地址空間優化
由於流量已物理分離，地址中的 2 位通道 ID（`[7:6]`）可以簡化或重新分配。

*   **舊映射**：
    *   `00`： PS
    *   `01`： PD
    *   `10`： PLI
    *   `11`： PLO

*   **新映射（每個平面）**：
    *   **NoC-0**：
        *   位 `[6]` （或通道字段的 LSB）選擇 PS 與 PD。
        *   `0`： PS
        *   `1`： PD
        *   （命令地址 `0x100` 仍然是特殊情況或單獨映射）。
    *   **NoC-1**：
        *   位 `[6]` 選擇 PLI 與 PLO。
        *   `0`： PLI
        *   `1`： PLO

### 4.2 控制流
*   **旁路控制**：所有全局控制信號（重置、啟動、停止）嚴格通過 **NoC-0** 路由。
*   **數據流**：
    *   權重/輸入 -> NoC-0。
    *   部分和/結果 <-> NoC-1。

## 5. 實現計劃

1.  **標頭更新（`utils.hpp`）**：
    *   檢查 `NOC_CHANNELS` 枚舉。為了兼容性，它可能保持不變，但使用它的邏輯將更改。
2.  **PE 路由器更新**：
    *   修改 `PErouter.hpp` 以添加新端口。
    *   將 `process` 循環拆分為兩個獨立的線程/方法：一個用於 NoC-0（推送），一個用於 NoC-1（事務）。
3.  **PE 更新**：
    *   更新 `ProcessElement.hpp` 以暴露新端口並將其綁定到更新的 `PErouter`。
4.  **NetworkOnChip 更新**：
    *   複製 `NoCRouter` 和 `MBUS` 實例。
    *   更新 `NetworkOnChip.hpp` 中的布線。
    *   更新 `scan_chain` 布線以配置兩組 MBUS。
5.  **集群/測試平台更新**：
    *   更新 `pe_wrapper` 和測試平台以驅動分離的介面。
