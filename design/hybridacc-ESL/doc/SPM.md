# Scratchpad Memory (SPM) 系統規格書

## 1. 系統概述 (System Overview)

本 SPM 系統旨在為 AI 加速器提供高吞吐量、低延遲的資料快取。系統由 12 個實體 SRAM Bank 組成，切分為 4 個 SRAM Group，並採用可配置 4x4 Group-level Crossbar：4 個 NoC Port 可一對一映射到不重複的 Group 並擁有優先權；DMA 為次級優先權，但可透過全域位址存取所有 Group。

## 2. 硬體架構規格 (Hardware Architecture)

### 2.1 儲存資源劃分

* **總實體 Bank 數:** 12 Banks。
* **分組配置 (Group Configuration):** 每 3 個 Bank 組合成一個 **Logical Group**，共 4 組 (Group 0-3)。
* **Bank 規格:**
* **寬度:** 64-bit。
* **深度:**  (可根據需求擴展)。
* **技術:** 單埠 (Single-port) 或 偽雙埠 (Pseudo Dual-port) SRAM。



### 2.2 外部介面 (External Interfaces)

| 介面名稱 | 數量 | 寬度 | 讀寫權限 | 說明 |
| --- | --- | --- | --- | --- |
| **NoC Read Port** | 3 | 192-bit | Read-only | 提供運算單元 (PE) 權重或特徵圖讀取。 |
| **NoC Write Port** | 1 | 192-bit | Write-only | 接收運算後的 Partial Sum 或結果。 |
| **DMA Port** | 1 | 64-bit | Read/Write | 銜接 DRAM，負責資料載入與回寫。 |

---

## 3. 定址模式與映射 (Addressing & Mapping)

### 3.1 雙模式定址邏輯 (Dual-Mode Addressing)

每一組 (Group) 根據位址範圍自動切換存取行為：

* **Mode 1: 64-bit 串接模式 (Linear Mode)**
* **位址範圍:** `0x0000` ~ `3D - 1`
* **行為:** 將 3 個 Bank 的位址垂直串聯，當作一個深度為  的連續空間。
* **用途:** DMA 初始化、精細資料操作。


* **Mode 2: 192-bit 交織並接模式 (Parallel Mode)**
* **位址範圍:** `3D` ~ `4D - 1`
* **行為:** 同時致能 Group 內 3 個 Bank。資料為  的拼接。
* **用途:** AI 核心運算、高頻寬資料交付。



### 3.2 可配置映射與優先權仲裁 (Configurable Mapping & Priority Arbitration)

系統採用 **可配置 Group-to-Port Mapping Crossbar**，支援在 Wave 邊界更新映射：

* **一對一不重複綁定:** 4 個 NoC Port 必須映射到 4 個互不重複的 Group（Permutation Constraint）。
* **映射控制:** 由 `config_map_i` 設定 Port-to-Group 對應，於 `config_update_i` 觸發時生效。
* **優先權機制:** 每個 Group 皆有 `NoC > DMA` 的仲裁規則。
* **DMA 全域可見:** DMA 請求攜帶全域位址，可指定任一 Group 與其組內地址。

---

## 4. 低延遲實作技術 (Latency Optimization)

### 4.1 Crossbar 實作

* **4x4 可配置 Crossbar:** 每個 NoC Port 經 4-to-1 選擇器連到單一 Group。
* **唯一性約束檢查:** 設定階段檢查 4 個 Port 的目標 Group 不可重複，違規配置不生效。
* **分散式仲裁器:** 每個 Group 前端使用輕量 2-to-1 仲裁（NoC / DMA），縮短關鍵路徑。
* **位元切片 (Bit-Slicing):** 將 192-bit 拆分為 3 組 64-bit 物理 Slice 佈局，降低線路擁塞與 RC 延遲。

### 4.2 流量與競爭控制

* **固定優先權:** 同一 Group 同週期發生 NoC 與 DMA 競爭時，永遠由 NoC 先服務。
* **DMA 次級服務:** DMA 在目標 Group 無 NoC 需求時取得服務，避免影響計算資料路徑。
* **Valid/Ready 輕量協定:** 放棄 AXI4，改用基於 Credit-based 的 Valid/Ready 握手，實現單週期資料交付。

---

## 5. 建模與驗證 (SystemC ESL Modeling)

### 5.1 Cycle-Accurate 模型要點

* **Bank 模型:** 實作單週期讀/寫延遲。
* **Crossbar 模型:** 模擬可配置 Port-to-Group 映射，並檢查一對一不重複約束。
* **Mapping Switch:** 模擬在 `config_update_i` 觸發後於 Wave 邊界切換映射表。
* **仲裁模型:** 每個 Group 模擬 `NoC > DMA` 的優先權決策（可選擇是否加入公平性機制）。

### 5.2 效能指標 (KPIs)

* **NoC Read Latency:** 目標 1~2 Cycles (含 Group 前端仲裁)。
* **NoC Throughput:** 192 bits/cycle per port。
* **Area Overhead:** 仲裁與控制邏輯面積預計佔整體 SPM 系統 < 5%。

---

## 6. 設計注意事項 (Caveats)

1. **同 Group 競爭:** 若 NoC 與 DMA 同時請求同一 Group，DMA 會被延後（NoC 優先）。
2. **DMA 飢餓風險:** 在 NoC 長時間滿載場景下，需評估 DMA 延遲上界（可加入 Aging/Round-Robin 緩解）。
3. **Timing Closure:** 建議將每組仲裁器靠近對應 3 個 Bank 佈局，縮短 192-bit 資料路徑。

---

## 7. Pin-out 定義 (Interface Specification)

### 7.1 全域訊號 (Global Signals)

| 信號名稱 | 方向 | 寬度 | 說明 |
| --- | --- | --- | --- |
| `clk` | Input | 1 | 系統主時鐘 |
| `rst_n` | Input | 1 | 非同步低電位重置 |
| `config_map_i` | Input | 8 | Crossbar 映射設定 (4 Ports * 2-bit Group ID) |
| `config_update_i` | Input | 1 | 更新 Mapping Table 的觸發訊號（建議於 Wave 邊界） |
| `arb_policy_i` | Input | 1 | 仲裁策略控制（0: 固定優先 NoC>DMA, 1: 保留擴展） |

### 7.2 NoC 數據介面 (Ports 0-3)

*Port 0-2 為 Read-only, Port 3 為 Write-only。以下為通用的單一 Port 定義：*

| 信號名稱 | 方向 | 寬度 | 說明 |
| --- | --- | --- | --- |
| `noc_req_vld_i` | Input | 1 | Request Valid: NoC 發起存取請求 |
| `noc_req_rdy_o` | Output | 1 | Request Ready: SPM 緩衝區準備好接收請求 |
| `noc_addr_i` | Input |  | 依 `config_map_i` 映射後之目標 Group 組內地址 (0 ~ 4D-1) |
| `noc_mode_i` | Input | 1 | 0: 64-bit 模式, 1: 192-bit 模式 |
| `noc_rdata_o` | Output | 192 | 讀取數據 (Port 0-2 適用) |
| `noc_wdata_i` | Input | 192 | 寫入數據 (Port 3 適用) |
| `noc_resp_vld_o` | Output | 1 | Response Valid: 數據已送出/寫入完成 |

### 7.3 DMA 介面 (Port 4)

*專門銜接 DRAM Controller，採用 64-bit 存取：*

| 信號名稱 | 方向 | 寬度 | 說明 |
| --- | --- | --- | --- |
| `dma_req_vld_i` | Input | 1 | DMA 請求有效 |
| `dma_req_rdy_o` | Output | 1 | SPM 可接受 DMA 請求 |
| `dma_addr_i` | Input |  | 全域地址（含 Group ID + 組內地址），可存取 Group 0~3 |
| `dma_rw_i` | Input | 1 | 讀寫控制 (0: Read, 1: Write) |
| `dma_wdata_i` | Input | 64 | 來自 DRAM 的資料 |
| `dma_rdata_o` | Output | 64 | 送往 DRAM 的資料 |
| `dma_done_o` | Output | 1 | 單筆或 Burst 傳輸完成 |

---

## 8. 內部控制邏輯與位址解碼 (Internal Decoder Logic)

為了達成最低延遲，NoC 請求先經 Crossbar 映射到目標 Group，再與 DMA 請求在 Group 前端完成仲裁，最後進入一級組合解碼邏輯：

1. **Crossbar 映射與約束檢查：**
* `config_map_i` 定義 4 個 NoC Port 的目標 Group。
* 僅接受一對一不重複映射；若設定重複 Group，維持前一版有效映射。

2. **Group 仲裁 (Arbitration)：**
* 每個 Group 獨立仲裁，規則為 `NoC > DMA`。
* DMA 以 `dma_addr_i` 的 Group ID 選定目標 Group，僅在該 Group 無 NoC 請求時被服務。

3. **Bank Enable (CEN) 產生：**
* 若 `mode == 1` (192-bit)：直接同時致能該 Group 的 3 個 Bank。
* 若 `mode == 0` (64-bit)：根據組內位址 LSB 解碼出單一 Bank 的 CEN。


4. **Byte Mask (WEN) 處理：**
* 由於是 192-bit 併接，寫入時需確保對齊。在 64-bit 模式下，僅開啟選中 Bank 的寫入遮罩。



---

## 9. SystemC Modeling 實作建議 (Cycle-Accurate)

在 SystemC 中，您可以將上述 Pin-out 封裝為 `sc_port` 或使用 TLM2.0 的 `simple_target_socket`。若要模擬可配置映射與優先權仲裁，建議實作如下：

```cpp
// 模擬可配置 Crossbar 映射 + Group 仲裁 + SPM 存取延遲
void spm_main_process() {
    // config_map_i 在 update 時載入為 active_map[4]
    // active_map[p] = group_id, 且需滿足 group_id 互不重複
    for (uint32_t p = 0; p < 4; ++p) {
        uint32_t group = active_map[p];
        bool noc_req = noc_req_vld_i[p].read();
        bool dma_req_same_group = dma_req_vld_i.read() && (dma_group_id == group);

        if (noc_req) {
            // NoC 高優先權
            wait(1, SC_NS);
            service_noc(group, p);
        } else if (dma_req_same_group) {
            // DMA 次級優先權
            wait(1, SC_NS);
            service_dma(group);
        }
    }
}

```
---