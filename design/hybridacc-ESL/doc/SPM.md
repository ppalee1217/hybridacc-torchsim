# Scratchpad Memory (SPM) 系統硬體規格書

## 1. 系統概述

SPM 是一個高效能、低延遲的片上存取記憶體，旨在為多核處理器或加速器提供高頻寬數據交換能力。ESL 實作以 SystemC SC_MODULE 呈現，採 RTL 風格設計：所有狀態元素皆為 `sc_signal<T>`（加 `_reg` 後綴），並以獨立的 `SC_CTHREAD`（`seq_process`）集中更新；各輸出信號由對應的 `SC_METHOD`（`comb_*`）驅動，避免組合邏輯回授。

### 核心組成與特性

* **組成**：12 個獨立 64-bit SRAM Banks，劃分為 4 個 Groups（每組 3 Banks），共 768 KiB（預設 `BANK_DEPTH=8192`）。
* **介面**：支援 4 組 NoC Slave Ports（192-bit，使用 `spm_req_t`/`spm_resp_t` 封包）與 1 組 AXI4-Lite DMA（64-bit）。
* **存取模式**：支援 Linear（單 Bank，64-bit）與 Parallel（同 Group 全 3 Banks，192-bit 聚合）模式，由地址範圍自動判斷。
* **管線化**：6-stage pipeline（S0–S5），目標在無衝突條件下維持 $II=1$。
* **握手切斷**：所有 NoC 輸入口整合 Skid Buffer（`skid_valid_reg` / `skid_data_reg`），`spm_req_ready_o` 由暫存器驅動，切斷 `valid/ready` 組合相依。
* **流控機制**：Per-port Credit-based outstanding 控制，`credit_cnt_reg[p]` 初始值 = `MAX_OUTSTANDING`（預設 8）。
* **DMA FIFO 緩衝**：AW/W/AR/B/R 各通道皆以獨立的 `hybridacc::FIFO<T>` 模組實作，深度 = `DMA_MAX_OUTSTANDING`（預設 8）。
* **可觀測性**：內建 PMU 計數器並以 Perfetto-compatible trace_process 輸出 JSON 事件。

---

## 2. 參數與介面定義 (Parameters & Signal Interfaces)

### 2.1 主要參數

#### 模板參數（Template Parameters）

| 參數名稱 | 預設值 | 描述 |
| --- | --- | --- |
| `NUM_NOC_PORTS` | 4 | NoC 請求接口數量 |
| `BANKS_PER_GROUP` | 3 | 每個 Group 包含的 SRAM Bank 數 |
| `BANK_DATA_WIDTH` | 64 | 單一 Bank 資料寬度 (bits) |
| `BANK_DEPTH` | 8192 | 單一 Bank 深度 (Words) |
| `SRAM_BANK_LATENCY` | 1 | SRAM Bank 讀取延遲週期數（傳入 SRAM submodule） |
| `SRAM_BANK_PIPELINE_DEPTH` | 1 | SRAM Bank 管線深度（傳入 SRAM submodule） |
| `ADDR_WIDTH` | 32 | 位址寬度 (bits) |
| `MAX_OUTSTANDING` | 8 | 每個 Port 最大 in-flight Read 交易數 |
| `DMA_MAX_OUTSTANDING` | 8 | DMA 最大 in-flight Read/Write 交易數 |

#### 衍生常數（Derived `static constexpr`）

| 常數名稱 | 計算式 | 預設值 | 描述 |
| --- | --- | --- | --- |
| `NUM_GROUPS` | `= NUM_NOC_PORTS` | 4 | 物理 Bank 分組數量（固定對應 NoC Port 數） |
| `TOTAL_BANKS` | `= NUM_GROUPS × BANKS_PER_GROUP` | 12 | SRAM Bank 總數 |
| `NOC_DATA_WIDTH` | `= BANKS_PER_GROUP × BANK_DATA_WIDTH` | 192 | NoC 資料寬度 (bits) |
| `GROUP_LINEAR_WORDS` | `= BANKS_PER_GROUP × BANK_DEPTH` | 24576 | 每 Group Linear 區段字數 |
| `GROUP_SPAN_WORDS` | `= (BANKS_PER_GROUP + 1) × BANK_DEPTH` | 32768 | 每 Group 總位址跨度（字數）|
| `BYTES_PER_BANK_WORD` | `= BANK_DATA_WIDTH / 8` | 8 | 每個 Bank Word 的位元組數 |
| `BANK_BYTE_MASK` | `= (1ULL << BYTES_PER_BANK_WORD) - 1` | 0xFF | 全欄寫入遮罩 |

#### FIFO 深度常數

| 常數名稱 | 值 | 描述 |
| --- | --- | --- |
| `DMA_AW_FIFO_DEPTH` | `= DMA_MAX_OUTSTANDING` | DMA AW channel FIFO 深度 |
| `DMA_W_FIFO_DEPTH` | `= DMA_MAX_OUTSTANDING` | DMA W data/strb FIFO 深度 |
| `DMA_WRITE_REQ_FIFO_DEPTH` | `= DMA_MAX_OUTSTANDING` | DMA 合併寫入請求 FIFO 深度 |
| `DMA_READ_REQ_FIFO_DEPTH` | `= DMA_MAX_OUTSTANDING` | DMA AR read-request FIFO 深度 |
| `DMA_READ_RESP_FIFO_DEPTH` | `= DMA_MAX_OUTSTANDING` | DMA R read-response FIFO 深度 |
| `GROUP_META_FIFO_DEPTH` | `= MAX_OUTSTANDING` | 每 Group 讀取 Meta FIFO 深度 |
| `PORT_RESP_FIFO_DEPTH` | `= MAX_OUTSTANDING + 2` | 每 Port 回應 FIFO 深度（+2 headroom：Write resp 與 Read resp 可同週期 push） |

### 2.2 全域與配置信號

| 信號名稱 | 方向 | 位元寬 | SC 類型 | 描述 |
| --- | --- | --- | --- | --- |
| `clk` | I | 1 | `sc_in<bool>` | 系統主時鐘（目標 1 GHz） |
| `reset_n` | I | 1 | `sc_in<bool>` | 同步重置（低準位有效），`SC_CTHREAD` 的 reset signal |
| `pmu_rst_i` | I | 1 | `sc_in<bool>` | PMU 專用重置（高準位有效，僅清除 PMU 計數器） |
| `config_map_i` | I | 8 | `sc_in<sc_uint<8>>` | 4 ports × 2-bit Group ID 靜態映射設定 |
| `config_update_i` | I | 1 | `sc_in<bool>` | 配置更新脈衝，於 Wave Boundary 生效 |
| `arb_policy_i` | I | 1 | `sc_in<bool>` | 仲裁策略保留欄位；目前固定為 `0`（SPM req > DMA req） |

### 2.3 NoC Slave Port（x`NUM_NOC_PORTS`）

| 信號名稱 | 方向 | 位元寬 | SC 類型 | 描述 |
| --- | --- | --- | --- | --- |
| `spm_req_valid_i[p]` | I | 1 | `sc_vector<sc_in<bool>>` | 請求有效 |
| `spm_req_ready_o[p]` | O | 1 | `sc_vector<sc_out<bool>>` | **Skid Buffer 輸出**，`= !skid_valid_reg[p]` |
| `spm_req_i[p]` | I | – | `sc_vector<sc_in<spm_req_t>>` | 請求封包（含 `addr`, `wdata`, `wen`） |
| `spm_resp_valid_o[p]` | O | 1 | `sc_vector<sc_out<bool>>` | 回應有效，直接由輸出暫存器 `port_resp_out_valid_reg[p]` 驅動 |
| `spm_resp_ready_i[p]` | I | 1 | `sc_vector<sc_in<bool>>` | 上游接收 Ready |
| `spm_resp_o[p]` | O | – | `sc_vector<sc_out<spm_resp_t>>` | 回應封包（含 `rdata`, `code`） |

NoC 在 ESL 內部以封包型態表示：

* **Request payload** `spm_req_t` = `spm_request_t<ADDR_WIDTH, NOC_DATA_WIDTH>`：`{addr[ADDR_WIDTH-1:0], wdata[NOC_DATA_WIDTH-1:0], wen}`
* **Response payload** `spm_resp_t` = `spm_response_t<NOC_DATA_WIDTH>`：`{rdata[NOC_DATA_WIDTH-1:0], code}`，其中 `code ∈ {SPM_OK, SPM_ERROR}`

> `spm_req_ready_o[p]` 由 `comb_spm_req_ready` 純組合驅動：`spm_req_ready_o[p] = !skid_valid_reg[p]`。

### 2.4 AXI4-Lite Slave Interface（DMA）

符合標準 AXI4-Lite 規格，data width = `BANK_DATA_WIDTH`（預設 64-bit），包含 `aw`, `w`, `b`, `ar`, `r` 五個通道。

| 信號名稱 | 方向 | 位元寬 | SC 類型 | 描述 |
| --- | --- | --- | --- | --- |
| `s_axi_awvalid_i` | I | 1 | `sc_in<bool>` | AW channel 有效 |
| `s_axi_awready_o` | O | 1 | `sc_out<bool>` | `= !dma_aw_fifo_full` |
| `s_axi_awaddr_i` | I | `ADDR_WIDTH` | `sc_in<sc_uint<ADDR_WIDTH>>` | 寫入全域位元組位址 |
| `s_axi_wvalid_i` | I | 1 | `sc_in<bool>` | W channel 有效 |
| `s_axi_wready_o` | O | 1 | `sc_out<bool>` | `= !dma_w_data_fifo_full` |
| `s_axi_wdata_i` | I | `BANK_DATA_WIDTH` | `sc_in<sc_biguint<BANK_DATA_WIDTH>>` | 寫入資料 |
| `s_axi_wstrb_i` | I | `BANK_DATA_WIDTH/8` | `sc_in<sc_uint<BANK_DATA_WIDTH/8>>` | Byte strobe |
| `s_axi_bvalid_o` | O | 1 | `sc_out<bool>` | `= (dma_b_pending_cnt_reg > 0)` |
| `s_axi_bready_i` | I | 1 | `sc_in<bool>` | 上游 B ready |
| `s_axi_bresp_o` | O | 2 | `sc_out<sc_uint<2>>` | 固定 `2'b00`（OKAY） |
| `s_axi_arvalid_i` | I | 1 | `sc_in<bool>` | AR channel 有效 |
| `s_axi_arready_o` | O | 1 | `sc_out<bool>` | `= !dma_read_req_fifo_full && !dma_read_resp_fifo_full && (inflight < DMA_MAX_OUTSTANDING)` |
| `s_axi_araddr_i` | I | `ADDR_WIDTH` | `sc_in<sc_uint<ADDR_WIDTH>>` | 讀取全域位元組位址 |
| `s_axi_rvalid_o` | O | 1 | `sc_out<bool>` | `= !dma_read_resp_fifo_empty` |
| `s_axi_rready_i` | I | 1 | `sc_in<bool>` | 上游 R ready |
| `s_axi_rdata_o` | O | `BANK_DATA_WIDTH` | `sc_out<sc_biguint<BANK_DATA_WIDTH>>` | 讀取回傳資料 |
| `s_axi_rresp_o` | O | 2 | `sc_out<sc_uint<2>>` | 固定 `2'b00`（OKAY） |

**DMA FIFO 流程概覽**：

```
AW ──> dma_aw_fifo ──┐
                     ├──(comb_dma_aw_w_merge)──> dma_write_req_fifo ──> comb_bank_req_arb ──> SRAM write
W  ──> dma_w_data_fifo ──┘                                                                └──> dma_b_pending_cnt_reg ──> B channel

AR ──> dma_read_req_fifo ──> comb_bank_req_arb ──> SRAM read ──> comb_resp_merge ──> dma_read_resp_fifo ──> R channel
```

### 2.5 PMU 監控輸出介面

| 信號名稱 | 方向 | 位元寬 | SC 類型 | 描述 |
| --- | --- | --- | --- | --- |
| `pmu_cycle_cnt_o` | O | 64 | `sc_out<sc_uint<64>>` | 對外輸出 `CYCLE_CNT` |
| `pmu_port_txn_cnt_o[p]` | O | 64 | `sc_vector<sc_out<sc_uint<64>>>` | 對外輸出 `PORT_TXN_CNT[p]`（p=0..NUM_NOC_PORTS-1） |
| `pmu_arb_stall_cnt_o` | O | 64 | `sc_out<sc_uint<64>>` | 對外輸出 `ARB_STALL_CNT` |
| `pmu_credit_stall_cnt_o` | O | 64 | `sc_out<sc_uint<64>>` | 對外輸出 `CREDIT_STALL_CNT` |

所有 PMU 輸出由 `pmu_output_process`（`SC_METHOD`）直接從 `_reg` 暫存器 pass-through，無額外延遲。

---

### 2.6 內部資料型別（Internal SC-compatible Struct Types）

#### ReqMode（列舉）

```cpp
enum class ReqMode : uint8_t { LINEAR = 0, PARALLEL = 1 };
```

#### ReadMeta（讀取 In-flight 元資訊）

記錄每筆進入 SRAM pipeline 的讀取請求，存放於 `group_meta_fifo[g]`：

| 欄位 | 型別 | 描述 |
| --- | --- | --- |
| `is_dma` | `bool` | `true` = 來自 DMA AR，`false` = 來自 NoC Port |
| `port_id` | `uint8_t` | NoC Port 編號（DMA 時為 0） |
| `mode` | `ReqMode` | LINEAR 或 PARALLEL |
| `addr` | `uint32_t` | Group-local word address（用於 `comb_resp_merge` 決定 bank index） |

#### DmaWriteReq（DMA 寫入合併請求）

AW + W channel 合併後存放於 `dma_write_req_fifo`：

| 欄位 | 型別 | 描述 |
| --- | --- | --- |
| `addr` | `sc_uint<ADDR_WIDTH>` | 全域位元組位址 |
| `data` | `sc_biguint<BANK_DATA_WIDTH>` | 寫入資料 |
| `strb` | `sc_uint<BANK_DATA_WIDTH/8>` | Byte strobe |

---

### 2.7 FIFO 模組清單

所有 FIFO 均為 `hybridacc::FIFO<T>` 模組實例，以明確的 `sc_signal` 信號線（port-based interface）連接，替代原有 `std::deque`：

| FIFO 名稱 | 資料型別 | 深度 | 說明 |
| --- | --- | --- | --- |
| `dma_aw_fifo` | `sc_uint<ADDR_WIDTH>` | `DMA_MAX_OUTSTANDING` | DMA AW channel 位址緩衝 |
| `dma_w_data_fifo` | `sc_biguint<BANK_DATA_WIDTH>` | `DMA_MAX_OUTSTANDING` | DMA W channel 資料緩衝 |
| `dma_w_strb_fifo` | `sc_uint<BANK_DATA_WIDTH/8>` | `DMA_MAX_OUTSTANDING` | DMA W channel strobe 緩衝 |
| `dma_write_req_fifo` | `DmaWriteReq` | `DMA_MAX_OUTSTANDING` | AW+W 合併後的寫入請求 |
| `dma_read_req_fifo` | `sc_uint<ADDR_WIDTH>` | `DMA_MAX_OUTSTANDING` | DMA AR channel 讀取位址緩衝 |
| `dma_read_resp_fifo` | `sc_biguint<BANK_DATA_WIDTH>` | `DMA_MAX_OUTSTANDING` | DMA R channel 讀取回應緩衝 |
| `group_meta_fifo[g]` | `ReadMeta` | `MAX_OUTSTANDING` | 每 Group 的讀取 meta 資訊（tracking in-flight reads） |
| `port_resp_fifo[p]` | `spm_resp_t` | `MAX_OUTSTANDING + 2` | 每 Port 的回應緩衝（輸出暫存器前緩衝） |

每個 FIFO 的信號命名規則為 `<fifo_name>_{empty,full,din,dout,push,pop,clear}`。

---

## 3. 地址解碼與映射 (Memory Mapping)

### 3.1 模式判定與索引規則

設單一 Bank 深度為 $D = \texttt{BANK\_DEPTH}$（Words），每個 Group 有 $B = \texttt{BANKS\_PER\_GROUP}$ 個 Banks。

定義：

* $G_{lin} = B \times D = \texttt{GROUP\_LINEAR\_WORDS}$（每 Group 的 Linear 區段字數）
* $G_{span} = (B+1) \times D = \texttt{GROUP\_SPAN\_WORDS}$（每 Group 總位址跨度）

NoC 請求使用 Group-local 位址 $A_{local}$（Word address，由 `active_map_reg[p]` 指派至目標 Group）：

* **Linear Mode**（$0 \le A_{local} < G_{lin}$）
  * 映射至 Group 內單一實體 Bank。
  * 實際 Bank global index：$\text{bidx} = \lfloor A_{local} / D \rfloor + g \times B$
  * SRAM row（byte address）：$\text{row} = (A_{local} \bmod D) \times \texttt{BYTES\_PER\_BANK\_WORD}$
  * 回應時讀取 `bank_resp_data_sig[bidx]`，置入 `rdata[BANK_DATA_WIDTH-1:0]`，高位補 0。

* **Parallel Mode**（$G_{lin} \le A_{local} < G_{span}$）
  * 啟動目標 Group 內全部 $B$ 個 Banks（192-bit 聚合存取）。
  * SRAM row：$\text{row} = (A_{local} - G_{lin}) \times \texttt{BYTES\_PER\_BANK\_WORD}$
  * 三個 Bank 同時驅動相同 row，使用全 1 byte mask。
  * 回應時拼接：$\text{rdata}[(k{+}1) \times 64 - 1 : k \times 64] = \text{bank\_resp\_data\_sig}[g \times B + k]$，$k=0,1,2$。

DMA 使用全域位元組位址 $A_{g}$，處理流程：

1. 轉換為全域 Word address：$\text{gwaddr} = A_g / \texttt{BYTES\_PER\_BANK\_WORD}$
2. 對應 Group：$\text{grp} = \lfloor \text{gwaddr} / G_{span} \rfloor$
3. Group-local Word address：$\text{lidx} = \text{gwaddr} \bmod G_{span}$
4. Bank index 與 Row（與 Linear 相同規則）：$\text{bidx} = \lfloor \text{lidx} / D \rfloor + \text{grp} \times B$；$\text{row} = (\text{lidx} \bmod D) \times \texttt{BYTES\_PER\_BANK\_WORD}$

> **注意**：DMA 僅支援 Linear 模式存取（`ReqMode::LINEAR`，meta 的 `mode` 固定為 LINEAR）。

### 3.2 物理容量視圖

* **總容量**：$\texttt{TOTAL\_BANKS} \times D \times \texttt{BANK\_DATA\_WIDTH}$ bits。預設（$D=8192$）= $12 \times 8192 \times 64 = 6{,}291{,}456$ bits = 786,432 bytes = **768 KiB**。
* **Linear 位址範圍**：每 Group $G_{lin} = 24576$ Words；**Parallel 位址範圍**：每 Group $D = 8192$ rows（位址 $G_{lin}$ 至 $G_{span}-1$）。
* **Parallel 寬度**：每個 Group 單週期可提供 $B \times 64 = 192$ bits 存取寬度。

---

## 4. 管線架構 (Pipeline Architecture)

本設計採 6 級流水線以達成高頻操作與高吞吐：

1. **S0 (Ingress - Skid Buffer)**
    * 每個 Port 配置一個暫存槽（`skid_valid_reg[p]` + `skid_data_reg[p]`）。
    * `spm_req_ready_o[p]` 由 FF 暫存器輸出（`comb_spm_req_ready`：`= !skid_valid_reg[p]`），確保不依賴後級即時組合結果。
    * 當 Port 請求被仲裁器直接消耗（`port_req_fire_sig[p]` 且未使用 skid）：skid 不填入。
    * 當請求未能被本週期服務（stall）：填入 `skid_valid_reg[p]` = `true`，`skid_data_reg[p]` = `spm_req_i[p]`。
    * 當 skid buffer 中的請求被服務後：`skid_valid_reg[p]` 清零。

2. **S1 (Decode & Route)**
    * 地址解碼（Linear/Parallel 模式判定）、Group mapping（由 `active_map_reg[p]` 決定）。
    * **Config Update 規則**：當 `config_update_i` 置高時，`seq_process` 解析 `config_map_i[7:0]`（每 Port 2 bits）；若任意 Group ID 重複則整次更新**忽略**，維持原 `active_map_reg`。
    * Read 請求執行 Credit 檢查（`credit_cnt_reg[p] > 0`）。

3. **S2 (Group Arbiter)**（由 `comb_bank_req_arb` 實作）
    * 每個 Group 設置 `group_busy[]` 旗標，每週期至多服務一個請求。
    * **固定來源優先順序**（由程式碼迴圈順序決定）：
      1. Port 0（最高）→ Port 1 → Port 2 → Port 3
      2. DMA Write（次低，需 `dma_b_pending_cnt_reg < DMA_MAX_OUTSTANDING`）
      3. DMA Read（最低，需 `dma_rd_inflight_cnt_reg < DMA_MAX_OUTSTANDING`）
    * Parallel 模式讀取需同時對 Group 內全部 3 Banks 發出 `bank_req_valid_sig`。
    * Write 請求直接驅動 `bank_write_en_sig`（不進 read pipeline），並立即產生 `wr_resp_push_sig`（寫入回應）。
    * Read 請求將 `ReadMeta` push 至 `group_meta_fifo[g]`。

4. **S3 (SRAM Issue)**
    * 對 SRAM 傳送 `req_valid`、`req_addr`（位元組位址）或 write 信號。
    * SRAM 模組（`hybridacc::SRAM<BANK_DATA_WIDTH, ADDR_WIDTH>`）接受後，於 `SRAM_BANK_LATENCY` 週期後輸出 `resp_valid` + `resp_data`。

5. **S4 (SRAM Access)**
    * SRAM 執行內部存取（延遲 $L_{sram} = \texttt{SRAM\_BANK\_LATENCY}$；管線深度 `SRAM_BANK_PIPELINE_DEPTH`）。
    * `bank_resp_ready_sig` 由 `comb_bank_resp_ready` 固定驅動為 `true`（meta FIFO 吸收排序）。

6. **S5 (Egress / Merge)**（由 `comb_resp_merge` 實作）
    * 當 `group_meta_fifo[g]` 非空且所有對應 Bank `resp_valid` 皆為 high，執行合併：
      * **NoC Linear**：取 `bank_resp_data_sig[bidx]`，置入 `rdata[63:0]`，高位清零。
      * **NoC Parallel**：拼接 3 Banks 資料。
      * **DMA**：取單 Bank 資料推入 `dma_read_resp_fifo`。
    * 回應先推入 `port_resp_fifo[p]`，再由 `seq_process` 在下一週期載入輸出暫存器（`port_resp_out_valid_reg`, `port_resp_out_data_reg`）。
    * 輸出暫存器透過 `comb_spm_resp_output` 驅動 `spm_resp_valid_o` / `spm_resp_o`；握手成功後由 `seq_process` 清除 `port_resp_out_valid_reg`。

---

## 5. RTL 風格設計方法論

### 5.1 設計原則

ESL 實作遵循以下 RTL 風格目標：

1. **狀態集中於 `_reg`**：所有狀態元素以 `sc_signal<T>`（加 `_reg` 後綴）宣告，**唯一**在 `seq_process`（`SC_CTHREAD`，clk 正緣觸發）中更新。
2. **輸出由 `comb_*` 驅動**：每組輸出信號由專屬 `SC_METHOD` 純組合驅動，與 `seq_process` 解耦，避免 delta-cycle 迴路。
3. **中間 fire/next 信號**：`comb_*` 方法經由 `sc_signal` 中繼信號（`port_req_fire_sig`, `wr_resp_push_sig`, `dma_write_fire_sig` 等）與 `seq_process` 溝通，不直接讀取 `_reg` 後寫回同一週期的輸出。
4. **FIFO 模組化**：所有 `std::deque` 替換為 `hybridacc::FIFO<T>` 模組實例，通過明確 `sc_signal` 連接。

### 5.2 狀態暫存器清單

| 暫存器名稱 | 型別 | 初始值 | 描述 |
| --- | --- | --- | --- |
| `skid_valid_reg[p]` | `sc_signal<bool>` | `false` | Port p 的 skid buffer 有效旗標 |
| `skid_data_reg[p]` | `sc_signal<spm_req_t>` | `{}` | Port p 的 skid buffer 資料 |
| `credit_cnt_reg[p]` | `sc_signal<sc_uint<8>>` | `MAX_OUTSTANDING` | Port p 的 Read credit 計數 |
| `active_map_reg[p]` | `sc_signal<sc_uint<2>>` | `p % NUM_GROUPS` | Port p 對應的 Group ID |
| `port_resp_out_valid_reg[p]` | `sc_signal<bool>` | `false` | Port p 輸出暫存器有效旗標 |
| `port_resp_out_data_reg[p]` | `sc_signal<spm_resp_t>` | `{}` | Port p 輸出暫存器資料 |
| `dma_b_pending_cnt_reg` | `sc_signal<sc_uint<8>>` | `0` | DMA 已完成寫入但 B response 尚未發送的計數 |
| `dma_rd_inflight_cnt_reg` | `sc_signal<sc_uint<8>>` | `0` | DMA 讀取 in-flight 計數（已入 AR FIFO，未收到 merge） |
| `pmu_cycle_cnt_reg` | `sc_signal<sc_uint<64>>` | `0` | 週期計數器 |
| `pmu_port_txn_cnt_reg[p]` | `sc_signal<sc_uint<64>>` | `0` | Port p 成功交易計數 |
| `pmu_arb_stall_cnt_reg` | `sc_signal<sc_uint<64>>` | `0` | 仲裁 Stall 累積計數 |
| `pmu_credit_stall_cnt_reg` | `sc_signal<sc_uint<64>>` | `0` | Credit Stall 累積計數 |

### 5.3 組合邏輯方法（SC_METHOD）清單

| 方法名稱 | Sensitivity | 輸出目標 | 說明 |
| --- | --- | --- | --- |
| `comb_spm_req_ready` | `skid_valid_reg[p]` | `spm_req_ready_o[p]` | `= !skid_valid_reg[p]` |
| `comb_dma_chan_ready` | `dma_*_fifo_full`, `dma_rd_inflight_cnt_reg` | `s_axi_awready_o`, `s_axi_wready_o`, `s_axi_arready_o` | 各通道 Ready 由對應 FIFO 滿/inflight 計數決定 |
| `comb_dma_aw_w_merge` | AW/W FIFO empty/dout, write_req FIFO full | `dma_aw_fifo_pop`, `dma_w_data/strb_fifo_pop`, `dma_write_req_fifo_push/din` | 當 AW 與 W 均有資料且 merged FIFO 有空間時合併 |
| `comb_bank_req_arb` | skid/credit/map regs, NoC req, DMA FIFO, meta full | Bank 驅動信號, `port_req_fire_sig`, `wr_resp_push_sig`, `dma_*_fire_sig`, stall sigs | 核心仲裁邏輯 |
| `comb_resp_merge` | `group_meta_fifo_*`, `bank_resp_*_sig`, resp FIFO full | `rd_resp_push/data_sig`, `dma_read_merge_*_sig`, `group_meta_fifo_pop` | 合併 SRAM 讀取回應 |
| `comb_port_resp_fifo_ctrl` | `wr/rd_resp_push/data_sig` | `port_resp_fifo_push/din` | Write/Read 回應二選一推入 port resp FIFO（assert 兩者不可同時） |
| `comb_spm_resp_output` | `port_resp_out_valid/data_reg` | `spm_resp_valid_o`, `spm_resp_o` | 輸出暫存器直送 NoC 輸出埠 |
| `comb_port_resp_fifo_pop` | `port_resp_out_valid_reg`, `port_resp_fifo_empty` | `port_resp_fifo_pop` | 輸出暫存器閒置且 FIFO 非空時 pop |
| `comb_dma_resp_b` | `dma_b_pending_cnt_reg` | `s_axi_bvalid_o`, `s_axi_bresp_o` | B 通道 valid = (`pending > 0`)，bresp 固定 OKAY |
| `comb_dma_resp_r` | `dma_read_resp_fifo_empty/dout` | `s_axi_rvalid_o`, `s_axi_rdata_o`, `s_axi_rresp_o` | R 通道 valid = FIFO 非空，rresp 固定 OKAY |
| `comb_dma_read_resp_fifo_push` | `dma_read_merge_fire/data_sig` | `dma_read_resp_fifo_push/din` | 轉發 merge 結果進 DMA R FIFO |
| `comb_dma_read_resp_fifo_pop` | `s_axi_rvalid/rready` | `dma_read_resp_fifo_pop` | R 握手成功時 pop |
| `comb_bank_resp_ready` | （無，常數） | `bank_resp_ready_sig[b]` | 固定為 `true`，SRAM 輸出永遠接受 |
| `pmu_output_process` | PMU `_reg` 信號 | `pmu_*_o` | PMU 暫存器直接 pass-through 至輸出埠 |

### 5.4 Skid Buffer 握手機制（程式碼級說明）

```
每週期 seq_process 中，對每個 Port p：
  fire       = port_req_fire_sig[p]      // 本週期仲裁器成功消耗一個請求
  from_skid  = skid_valid_reg[p]         // 本週期消耗的是 skid buffer 中的請求
  incoming   = spm_req_valid_i[p] && !from_skid

  if (fire && from_skid)   → skid_valid_reg[p] = false   // skid 消耗完畢
  else if (incoming && !fire) → skid_valid_reg[p] = true,
                                skid_data_reg[p]  = spm_req_i[p]  // 儲存 stall 的請求
  // incoming && fire：請求直接消耗，skid 保持 false
```

相較於原本的「two-slot」描述，ESL 實作採用單一暫存器（one-slot skid）；`ready_o = !skid_valid_reg[p]`，確保 back-pressure 切斷組合迴圈。

### 5.5 仲裁與流控

#### Credit Counter 更新規則

```
每週期 seq_process 中，對每個 Port p：
  rd_issue = port_req_fire_sig[p] && !port_req_is_write_sig[p]  // 本週期送出 Read
  rd_done  = rd_resp_push_sig[p]                                  // 本週期收到 Read 回應

  if  (rd_issue && !rd_done) → credit_cnt_reg[p] -= 1
  if  (!rd_issue && rd_done) → credit_cnt_reg[p] += 1
  // 同時 issue 與 done：信用不變（最佳情況，理論上因管線延遲不會同週期出現）
```

#### Stall 條件

1. `credit_cnt_reg[p] == 0`：`credit_stall_sig` 置高，累積至 `pmu_credit_stall_cnt_reg`。
2. 目標 Group 的 `group_busy[g]` 已被其他 Port/DMA 搶先：`arb_stall_sig` 置高，累積至 `pmu_arb_stall_cnt_reg`。
3. `group_meta_fifo_full[g]`（Read 情況）。
4. `port_resp_fifo_full[p]`（`comb_resp_merge` 背壓）。

#### DMA B-channel Pending 機制

`dma_b_pending_cnt_reg` 追蹤已完成 bank write 但 B response 尚未握手完成的筆數：

```
wr_fire = dma_write_fire_sig        // 本週期 DMA 寫入成功送至 bank
b_fire  = s_axi_bvalid_o && s_axi_bready_i

if  (wr_fire && !b_fire) → dma_b_pending_cnt_reg += 1
if  (!wr_fire && b_fire && pending > 0) → dma_b_pending_cnt_reg -= 1
```

---

## 6. 時序要求 (Timing Specification)

### 6.1 寫入時序 (Write Transaction)

| 週期 | 事件 |
| --- | --- |
| `T0` | NoC：`spm_req_valid_i[p]` 置高，`wen=1` |
| `T1` | S0：`spm_req_ready_o[p]` 高（= `!skid_valid_reg[p]`），請求進入 skid 或直接被仲裁器讀取 |
| `T1` | S2：`comb_bank_req_arb` 驅動 `bank_write_en_sig`，產生 `wr_resp_push_sig[p]` |
| `T1` | S2：`comb_port_resp_fifo_ctrl` push 寫入回應至 `port_resp_fifo[p]` |
| `T2` | S5（output reg）：`seq_process` 載入 `port_resp_out_valid_reg[p]` |
| `T2` | `spm_resp_valid_o[p]` 置高（write ack），`spm_resp_o[p].code = SPM_OK` |

### 6.2 讀取時序 (Read Transaction)

| 週期 | 事件 |
| --- | --- |
| `T0` | NoC：`spm_req_valid_i[p]` 置高，`wen=0` |
| `T1` | S2：`comb_bank_req_arb` 驅動 `bank_req_valid_sig`，push `ReadMeta` 至 `group_meta_fifo[g]` |
| `T1..T1+Lsram` | S3–S4：SRAM 存取（`SRAM_BANK_LATENCY` = $L_{sram}$ 週期） |
| `T1+Lsram` | S4：`bank_resp_valid_sig[bidx]` 置高 |
| `T1+Lsram` | S5：`comb_resp_merge` 合併資料，push 至 `port_resp_fifo[p]`，pop `group_meta_fifo[g]`，觸發 credit 回補 |
| `T2+Lsram` | `seq_process` 載入 `port_resp_out_valid_reg[p]` |
| `T2+Lsram` | `spm_resp_valid_o[p]` 置高，`spm_resp_o[p]` 含讀取資料 |

> **讀取總延遲**：$2 + L_{sram}$ 週期（仲裁無 stall 情況下）。若有仲裁或 credit stall 則 $+N$ 週期。

### 6.3 DMA 寫入時序

| 步驟 | 說明 |
| --- | --- |
| AW+W handshake | `s_axi_awvalid` && `awready` → push `dma_aw_fifo`；`s_axi_wvalid` && `wready` → push `dma_w_data/strb_fifo` |
| AW+W merge | `comb_dma_aw_w_merge`：AW FIFO 與 W FIFO 均非空時 pop 並 push `dma_write_req_fifo` |
| Bank write | `comb_bank_req_arb`（最低優先）：pop `dma_write_req_fifo`，驅動 SRAM write |
| B response | `dma_b_pending_cnt_reg` += 1；當 `s_axi_bready_i` 握手後遞減 |

### 6.4 DMA 讀取時序

| 步驟 | 說明 |
| --- | --- |
| AR handshake | `s_axi_arvalid` && `arready`（inflight < limit）→ push `dma_read_req_fifo`，`dma_rd_inflight_cnt_reg` += 1 |
| Bank read | `comb_bank_req_arb`（最低優先）：pop `dma_read_req_fifo`，push `ReadMeta`（`is_dma=true`）至 `group_meta_fifo[g]` |
| Merge | `comb_resp_merge`：bank resp valid 後 push 資料至 `dma_read_resp_fifo`，`dma_rd_inflight_cnt_reg` -= 1 |
| R response | `s_axi_rvalid` = FIFO 非空；`s_axi_rready_i` 握手後 pop FIFO |

---

## 7. 效能監控器 (PMU) 規格

PMU 提供以下 64-bit 計數器：

| 暫存器名稱 | 功能 | 觸發條件 | 應用 |
| --- | --- | --- | --- |
| `CYCLE_CNT` | 系統週期計數 | 每週期 +1（`pmu_rst_i=0` 時） | 時間基準 |
| `PORT_TXN_CNT[0:N-1]` | 各 Port 成功交易數 | `port_req_fire_sig[p]` 為 high | 計算 Throughput |
| `ARB_STALL_CNT` | 仲裁衝突 Stall 次數 | `arb_stall_sig` 為 high | 診斷 Bank Conflict |
| `CREDIT_STALL_CNT` | Credit 不足 Stall 次數 | `credit_stall_sig` 為 high | 診斷 Read Latency 過大 |

> `arb_stall_sig` 和 `credit_stall_sig` 由 `comb_bank_req_arb` 驅動，並在 `seq_process` 中每週期累積。

PMU 計數器同步連接至模組輸出 IO（`pmu_*_o`），由 `pmu_output_process`（`SC_METHOD`）直接 pass-through，可由上層即時觀測。

PMU 重置規則：

* 當 `pmu_rst_i=1` 時（在 `seq_process` 主迴圈中優先處理），`CYCLE_CNT`、`PORT_TXN_CNT[0:N-1]`、`ARB_STALL_CNT`、`CREDIT_STALL_CNT` 全部同步清零。本週期不累積任何計數。
* `pmu_rst_i` 不影響資料路徑、FIFO 狀態與一般記憶體狀態；全域重置仍由 `reset_n`（低準位）負責。

---

## 8. 異常處理與可觀察性 (Debug & Error Handling)

* **Address Out-of-Range**：當 DMA 計算的 `grp >= NUM_GROUPS` 時，`comb_bank_req_arb` 不驅動 SRAM 任何信號（跳過該 DMA 請求），不回傳錯誤（目前 ESL 以 silent drop 處理）。NoC 請求的 group 來自 `active_map_reg[p]`，理論上已在配置時驗證，不應出現越界。
* **Response Code 對齊**：ESL 實作回應碼為 `SPM_OK`（寫入立即回應、讀取 merge 後回應）；目前無 `SPM_ERROR` 回傳路徑（越界視為 silent drop）。
* **SRAM OOB 行為**：由 `hybridacc::SRAM` 子模組處理；讀取越界回傳 0 並以 `std::cout` 記錄訊息；寫入越界忽略並記錄訊息。
* **wr/rd conflict assert**：`comb_port_resp_fifo_ctrl` 有 `assert(!(wr && rd))` 結構性檢查，確保同一 Port 同週期不同時出現 write response 與 read response push。
* **Counter Snapshot**：建議於 Wave Boundary 或軟體觸發點讀取 PMU，避免跨波段統計混疊。
* **可追蹤欄位**：`ReadMeta` 保留 `port_id`, `mode`, `addr`, `is_dma` 於 meta FIFO，用於 `comb_resp_merge` 精確路由回應。

---

### 8.1 Perfetto Trace 機制（`trace_process`）

`trace_process` 是一個 `SC_METHOD`（`sensitive << clk.pos()`），在每個 clk 正緣觸發，輸出 Perfetto-compatible JSON trace 事件（需呼叫 `set_trace_context` 或 `enable_perffeto_trace` 初始化）。

#### Trace Thread 分配（`get_trace_num()` 回傳 `3 + TOTAL_BANKS`）

| Thread | 用途 |
| --- | --- |
| `trace_id + 0` | SPM 模組本身（保留） |
| `trace_id + 1` | `SPM_State`：整體狀態機 |
| `trace_id + 2` | `SPM_NoC`：NoC 請求/回應狀態 |
| `trace_id + 3` | `SPM_DMA`：DMA 通道狀態 |
| `trace_id + 4..3+TOTAL_BANKS` | 各 SRAM Bank（由子模組自行輸出） |

#### SPM_State 狀態機

| 狀態 | 條件（優先序由上至下） |
| --- | --- |
| `RESET` | `!reset_n` |
| `PMU_RESET` | `pmu_rst_i` |
| `DMA_ACTIVE` | DMA FIFO 非空或有 DMA pending（讀/寫） |
| `NOC_ACTIVE` | 任一 Port 有 valid req 或 valid resp |
| `IDLE` | 以上皆否 |

#### SPM_NoC 狀態機

| 狀態 | 條件 |
| --- | --- |
| `XFER` | 本週期有 NoC req 握手 **且** 有 resp 握手 |
| `REQ` | 本週期有 NoC req 握手 |
| `RESP` | 本週期有 NoC resp 握手 |
| `BACKPRESSURE` | 有 valid req/resp 但無握手 |
| `IDLE` | 無 active |

#### SPM_DMA 狀態機

| 狀態 | 條件 |
| --- | --- |
| `RESP` | 本週期有 B 或 R 握手 |
| `REQ` | 本週期有 AW/W/AR 握手 |
| `PENDING` | DMA FIFO 非空或有 inflight/pending |
| `IDLE` | 無 active |

---

## 9. 效能目標 (Performance Targets)

* **時脈頻率**：1 GHz（目標）。
* **峰值頻寬（Peak BW）**：
  * 單 Port Parallel：$192 \text{ bits/cycle} = 24 \text{ B/cycle}$
  * 單 Port Linear：$64 \text{ bits/cycle} = 8 \text{ B/cycle}$
  * `NUM_NOC_PORTS` Ports 理論總和（Parallel）：$\texttt{NUM\_NOC\_PORTS} \times 192 = 768 \text{ bits/cycle} = 96 \text{ B/cycle}$
  * 於 1 GHz：理論峰值 $96 \text{ GB/s}$（未扣除衝突與背壓）。
* **讀取延遲（Read Latency）**：
  * 無 stall：$2 + L_{sram}$ 週期（$L_{sram} = \texttt{SRAM\_BANK\_LATENCY}$，預設 1），即從請求到輸出暫存器 valid 共 **3 週期**。
  * 有仲裁 stall：$+N$ 週期。
  * 有 Credit stall：$+M$ 週期。
* **吞吐量（Throughput）**：理想無衝突條件下 Initiation Interval（$II$）= 1（每週期每 Group 可服務 1 筆請求）。
* **DMA 最大 Outstanding**：讀寫各 `DMA_MAX_OUTSTANDING`（預設 8）筆 in-flight。
