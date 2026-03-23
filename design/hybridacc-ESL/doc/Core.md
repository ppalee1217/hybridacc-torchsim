# HACC Core Controller 硬體架構規格書

本文件定義 HACC Core Controller 在第二版架構下的正式 micro architecture。本文不是概念提案，而是供 RTL 切模、韌體開發、compiler handoff、驗證規劃與 reviewer 審查使用的工程規格。

本版最重要的設計變更是：runtime control 從舊的 descriptor execution complex 改成由 `cc_core_mcu` 直接掌控。`cc_section_loader` 只做 local section copy；對 cluster、DMA、NLU 的 runtime side effect 都由 MCU 透過 command fabric 與 stream path 顯式觸發。

---

## 1. 正式結論

### 1.1 架構主結論

本版 controller 採用以下主結構：

1. 一個 32-bit `cc_core_mcu` 作為唯一 runtime orchestrator。
2. 一個 `cc_section_loader`，只負責將 host/boot firmware 提供的 manifest section 複製到 local memories。
3. 一個 `cc_cmd_fabric`，承接 MCU 發出的 local MMIO / per-cluster AHB / per-NLU AHB / broadcast command。
4. 一個 `cc_dma_engine`，作為由 MCU 透過 MMIO 與 stream 方式控制的 data mover。
5. 一個 `cc_irq_router`，統整 cluster、DMA、NLU 與 local timer 的 level IRQ，對 MCU 提供 sticky pending + explicit ack 介面。
6. 一個 `cc_cluster_data_fabric`，負責協調 DMA engine 與 NLU data master 對 cluster SPM data path 的存取。

### 1.2 被移除的 primary runtime 模組

下列模組不再作為 primary runtime control path：

1. `cc_job_manager`
2. `cc_block_fetch`
3. `cc_block_expander`
4. `cc_wave_scheduler`

若未來為了效能加入輔助式 prefetch/assist 硬體，仍不得改變 MCU 為唯一 architecturally visible control master 的原則。

### 1.3 DRAM AXI4 共享結論

Controller top-level 只對外暴露一組 `m_mem_axi_*` DRAM AXI4 master。此介面由以下兩個內部 requester 共用：

1. `cc_section_loader`
2. `cc_dma_engine`

正式規則：

1. reset/load phase 可採 phase ownership，優先由 loader 使用。
2. run phase 由 DMA 使用。
3. 若實作需要在 runtime 再次載入 section，則必須加入 `cc_mem_axi_arbiter`，但外部介面仍只有一組 `m_mem_axi_*`。
4. 不允許 loader 與 DMA 直接各自對外暴露獨立 DRAM master port。

---

## 2. 設計目標與非目標

### 2.1 目標

1. 支援 1 到 N 個 ComputeCluster。
2. 支援 0 到 M 個 NLU。
3. 支援 firmware-driven block/wave runtime control。
4. 支援 `.hacc.core/.job/.block/.profile/.dma/.agu/.nlu/.pe/.scan/.patch` 的載入與執行。
5. 支援 level IRQ、sticky pending、explicit ack。
6. 支援對多 cluster 的 mask-based broadcast 控制，且完成語意一致。
7. 支援 deterministic command ordering 與可驗證的 side effect 邊界。

### 2.2 非目標

1. 不在 controller 內放大型 tensor SRAM。
2. 不把 NLU 內部 SRAM 納入 DMA engine ownership。
3. 不在 loader 中實作完整通用 ELF parser。
4. 不再把 runtime wave orchestration 交給隱含的 execution complex 黑盒。

---

## 3. 系統假設與外部契約

### 3.1 Cluster 契約

每個 cluster 必須對 controller 提供：

1. 一組 AXI4-Lite 64-bit data slave port，用於 SPM data access。
2. 一組 AHB-Lite 32-bit command slave port，用於 SPM config、HDDU MMIO passthrough 與 NoC sideband。
3. 一條 interrupt output signal。

### 3.2 NLU 契約

每個 NLU 必須對 controller 提供：

1. 一組 AHB-Lite 32-bit MMIO slave port。
2. 一組 AXI4-Lite data master requester 或等價抽象 request/response 介面，用於主動讀 cluster SPM。
3. 一條 interrupt output signal。

### 3.3 Cluster AHB command window

controller 對 cluster command path 的假設維持不變：

1. `0x0000 ~ 0x00FF`：SPM config / PMU window
2. `0x1000 ~ 0x1FFF`：HDDU MMIO passthrough window
3. `0x2000 ~ 0x20FF`：NoC command sideband window

### 3.4 NLU MMIO window

controller 對 NLU 的最小要求：

1. `NLU_CTRL`
2. `NLU_STATUS`
3. `NLU_OP_KIND`
4. `NLU_CLUSTER_MASK`
5. `NLU_SRC_SECTION_REF`
6. `NLU_DST_SECTION_REF`
7. `NLU_AUX_PARAM[0..3]`
8. `NLU_ERR_CODE`
9. `NLU_ERR_AUX`
10. `NLU_IRQ_ACK`
11. `NLU_CFG_PAYLOAD_WINDOW`

### 3.5 IRQ 模型

所有外部完成/錯誤事件都以 level signal 輸入 `cc_irq_router`。正式規則：

1. signal 經同步器後進入 pending latch。
2. pending latch 為 sticky bit。
3. MCU 需經由 `IRQ_ACK` CSR 明確 ack。
4. 若來源 level 在 ack 後仍為 asserted，pending 必須重新置位。
5. 此規則同時適用 cluster IRQ、DMA IRQ 與 NLU IRQ。

---

## 4. Top-level 架構

### 4.1 方塊圖

```text
                         +-----------------------+
                         | cc_boot_host_if       |
                         | - host CSR            |
                         | - manifest queue      |
                         | - controller IRQ      |
                         +-----------+-----------+
                                     |
                                     v
                         +-----------------------+
                         | cc_section_loader     |
                         | - manifest consumer   |
                         | - DRAM reader         |
                         | - local section copy  |
                         +-----------+-----------+
                                     |
                +--------------------+--------------------+
                |                                         |
                v                                         v
       +-------------------+                    +-------------------+
     | cc_isram          |                    | cc_data_sram      |
       +-------------------+                    +-------------------+
                ^                                         ^
                |                                         |
                +--------------------+--------------------+
                                     |
                                     v
                         +-----------------------+
                         | cc_core_mcu           |
                         | - fetch/decode/ALU    |
                         | - call/ret/irq        |
                         | - local load/store    |
                         | - mmio/stream issue   |
                         +-----------+-----------+
                                     |
                                     v
                         +-----------------------+
                         | cc_cmd_fabric         |
                         | - local CSR decode    |
                         | - DMA route           |
                         | - cluster AHB route   |
                         | - NLU AHB route       |
                         | - broadcast sequencer |
                         +----+------------+-----+
                              |            |
                              v            v
                    +----------------+  +----------------+
                    | cc_dma_engine  |  | cc_irq_router  |
                    +--------+-------+  +--------+-------+
                             |                   |
                             v                   v
                    +----------------+  cluster_irq[*]
                    | cc_cluster_data |  dma_irq
                    | _fabric         |  nlu_irq[*]
                    +--------+-------+
                             |
                             v
                 cluster AXI4-Lite data / NLU AXI requester
```

### 4.2 分層原則

1. Host/boot layer：`cc_boot_host_if` 與 manifest 管理。
2. Load layer：`cc_section_loader` 與 local memory load path。
3. Runtime control layer：`cc_core_mcu`、`cc_cmd_fabric`、`cc_irq_router`。
4. Data movement layer：`cc_dma_engine`、`cc_cluster_data_fabric`。
5. External target layer：cluster command/data ports 與 NLU MMIO/data requester。

---

## 5. 參數化規格

| Parameter | 建議值 | 說明 |
|---|---:|---|
| `NUM_CLUSTERS` | 1, 2, 4, 8, 16 | cluster 數量 |
| `NUM_NLU` | 0, 1, 2, 4 | NLU 數量 |
| `CORE_XLEN` | 32 | MCU 資料寬度 |
| `CORE_GPR_NUM` | 16 | GPR 數量 |
| `CORE_ISRAM_BYTES` | 8192 或 16384 | MCU instruction SRAM |
| `DATA_SRAM_BYTES` | 65536 或以上 | unified local data SRAM |
| `DATA_DESC_ABI_BYTES` | 49152 或以上 | descriptor/payload ABI region 保留容量 |
| `DATA_EVENT_ABI_BYTES` | 4096 或以上 | event/debug ABI region 保留容量 |
| `DATA_WORD_W` | 32 | MCU-visible data SRAM word width |
| `CL_AXI_DATA_WIDTH` | 64 | cluster data port width |
| `CL_AHB_DATA_WIDTH` | 32 | cluster command port width |
| `MEM_AXI_DATA_WIDTH` | 128 或 256 | external DRAM AXI4 width |
| `DMA_CMD_FIFO_DEPTH` | 8 或 16 | DMA command FIFO 深度 |
| `DMA_STREAM_FIFO_DEPTH` | 16 或 32 | DMA stream FIFO 深度 |
| `CMD_FABRIC_OUTSTANDING` | 1 | 本版 command path 採 blocking 語意 |

---

## 6. Local memories

### 6.1 `cc_isram`

#### 功能

1. 存放 `.hacc.core`。
2. 提供 MCU instruction fetch。
3. 接受 section loader 寫入。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `core_if_req_valid` | in | 1 | MCU fetch request |
| `core_if_addr` | in | 32 | byte address |
| `core_if_rdata` | out | 32 | instruction word |
| `loader_wr_valid` | in | 1 | loader write valid |
| `loader_wr_addr` | in | 32 | byte address |
| `loader_wr_data` | in | 32 或 128 | write data |
| `loader_wr_strb` | in | 4 或 16 | byte strobes |

#### 規則

1. Loader 與 MCU fetch 不可在同 cycle 對同一位址產生未定義衝突。
2. run phase 覆寫 active instruction range 屬於非法行為，必須由 host/firmware 避免，或由 implementation 回報 fault。

### 6.2 `cc_data_sram`

#### 功能

1. 作為單一實體 local data SRAM，承載所有非 `.hacc.core` 的 local runtime data。
2. 由軟體 ABI 在其中區分 descriptor/payload region 與 event/debug region。
3. 提供 section loader 寫入。
4. 提供 MCU 32-bit load/store 存取。

#### ABI 分區

| ABI region | 主要內容 | 主要消費者 |
|---|---|---|
| descriptor/payload ABI region | `.hacc.job/.hacc.block/.hacc.profile/.hacc.dma/.hacc.agu/.hacc.nlu/.hacc.pe/.hacc.scan/.hacc.patch` | MCU load / MMIO / stream |
| event/debug ABI region | completion bitmap、trace、debug scratch、optional software stack spill、`.hacc.debug` | MCU / trace / debug |

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `loader_req_valid` | in | 1 | loader request |
| `loader_req_write` | in | 1 | write enable |
| `loader_req_addr` | in | `DATA_ADDR_W` | byte 或 word 位址，依實作定義 |
| `loader_req_wdata` | in | implementation-defined | loader write data |
| `loader_req_wstrb` | in | implementation-defined | byte enable |
| `loader_resp_ready` | in | 1 | loader response ready |
| `mcu_req_valid` | in | 1 | MCU access request |
| `mcu_req_write` | in | 1 | `1=write`, `0=read` |
| `mcu_req_addr` | in | 32 | byte address |
| `mcu_req_wdata` | in | 32 | 32-bit store data |
| `mcu_req_wstrb` | in | 4 | byte strobes |
| `mcu_resp_valid` | out | 1 | access complete |
| `mcu_resp_rdata` | out | 32 | 32-bit read data |

#### 規則

1. MCU-visible 基本讀取粒度固定為 32-bit。
2. `.hacc.pe/.hacc.scan` 也必須可作為 local payload 存在 `cc_data_sram` 的 descriptor/payload ABI region 中，不得要求 loader 直接 side-effect 到 cluster。
3. 若實作採用更寬 memory line，對 MCU 仍需維持正確 32-bit 語意。
4. descriptor/payload ABI region 與 event/debug ABI region 是軟體 ABI 邊界，不要求硬體分成兩個實體 SRAM instance。

### 6.3 `cc_data_sram` event/debug ABI 用途

用途：

1. trace buffer
2. completion bitmap
3. debug scratch
4. optional software stack spill area

---

## 7. 外部介面總表

### 7.1 Global

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `clk` | in | 1 | 主時脈 |
| `rst_n` | in | 1 | active-low reset |

### 7.2 Host control slave

| Port Group | Dir | Width | 說明 |
|---|---|---:|---|
| `s_ctrl_axi_*` | in/out | platform-defined | host 存取 controller CSR / manifest queue |

### 7.3 DRAM master interface

| Port Group | Dir | Width | 說明 |
|---|---|---:|---|
| `m_mem_axi_aw*` | out | AXI4 | write address |
| `m_mem_axi_w*` | out | AXI4 | write data |
| `m_mem_axi_b*` | in | AXI4 | write response |
| `m_mem_axi_ar*` | out | AXI4 | read address |
| `m_mem_axi_r*` | in | AXI4 | read data |

### 7.4 Per-cluster AXI4-Lite data masters

controller 對每個 cluster 暴露一組 data master port，供 DMA engine 或 NLU fabric arbitration 後使用。

### 7.5 Per-cluster AHB-Lite command masters

controller 對每個 cluster 暴露一組 command master port，供 `cc_cmd_fabric` 下發 control/MMIO/NoC transaction。

### 7.6 Per-NLU AHB-Lite MMIO masters

controller 對每個 NLU 暴露一組 AHB-Lite command master port，供 `cc_cmd_fabric` 設定與啟動 NLU。

### 7.7 Interrupts

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `cluster_irq[NUM_CLUSTERS-1:0]` | in | N | cluster done/error irq |
| `nlu_irq[NUM_NLU-1:0]` | in | M | NLU done/error irq |
| `dma_irq` | in | 1 | DMA done/error irq |
| `controller_irq` | out | 1 | controller 對 host 的總中斷 |

---

## 8. 模組切分與詳細規格

### 8.1 `cc_boot_host_if`

#### 功能

1. Host CSR bank。
2. Manifest queue。
3. Loader control/status。
4. Host-visible interrupt/status summary。

#### 主要 I/O

| Port | Dir | 說明 |
|---|---|---|
| `s_ctrl_axi_*` | in/out | host control bus |
| `manifest_push_valid` | out | 新 manifest entry |
| `manifest_push_data` | out | manifest payload |
| `manifest_push_ready` | in | loader 可接收 |
| `host_irq_status` | in | 來自 local status/irq summary |
| `controller_irq` | out | 對 host 中斷 |

#### 規則

1. active load 期間不得覆寫正在消費的 manifest entry。
2. completion 與 error 狀態採 write-1-to-clear，不可 read-to-clear。

### 8.2 `cc_boot_rom`（可選）

本版允許 `cc_boot_rom` 為 optional。若實作存在，其責任僅限於：

1. reset vector
2. boot mode 決策
3. 跳轉到 I-SRAM entry 或 host-assisted boot stub

它不是 runtime scheduler，也不承擔 descriptor 解析邏輯。

### 8.3 `cc_section_loader`

#### 功能

1. 消費 manifest。
2. 從 DRAM 讀 section payload。
3. 將 payload 寫入 I-SRAM 或 `cc_data_sram` 的對應 ABI region。
4. 做基礎 bounds/checksum 驗證。

#### 明確不做的事

1. 不對 cluster 發送 NoC program stream。
2. 不對 cluster 發送 profile apply MMIO。
3. 不對 DMA engine 發送 runtime command。
4. 不對 NLU 發送 config stream。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `manifest_pop_valid` | in | 1 | manifest entry valid |
| `manifest_pop_data` | in | manifest width | section metadata |
| `manifest_pop_ready` | out | 1 | 可接收 entry |
| `m_mem_axi_*` | out/in | AXI4 | DRAM read master |
| `isram_loader_*` | out | implementation-defined | 寫 I-SRAM |
| `data_loader_*` | out | implementation-defined | 寫 `cc_data_sram` |

#### Section route 正式定義

| Section | 載入位置 | runtime 消費者 |
|---|---|---|
| `.hacc.core` | I-SRAM | MCU fetch |
| `.hacc.job` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.block` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.profile` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO |
| `.hacc.dma` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO -> DMA |
| `.hacc.agu` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO |
| `.hacc.nlu` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO -> NLU |
| `.hacc.pe` | Data-SRAM descriptor/payload ABI region | MCU stream -> cluster NoC |
| `.hacc.scan` | Data-SRAM descriptor/payload ABI region | MCU stream -> cluster NoC |
| `.hacc.patch` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.debug` | Data-SRAM event/debug ABI region | debug/runtime optional |

#### 狀態機

1. `LD_IDLE`
2. `LD_FETCH`
3. `LD_ROUTE`
4. `LD_WRITE_LOCAL`
5. `LD_VERIFY`
6. `LD_DONE`
7. `LD_ERR`

### 8.4 `cc_core_mcu`

#### 功能

1. 執行 HACC Core MCU ISA。
2. 讀取 local descriptor 與 payload tables。
3. 對 `cc_cmd_fabric` 發出 blocking MMIO/broadcast/stream 操作。
4. 接收 `cc_irq_router` 的 pending 狀態與 interrupt trap。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `if_req_valid` | out | 1 | instruction fetch request |
| `if_addr` | out | 32 | instruction address |
| `if_rdata` | in | 32 | instruction data |
| `ls_req_valid` | out | 1 | local load/store request |
| `ls_req_write` | out | 1 | `1=store`, `0=load` |
| `ls_req_addr` | out | 32 | local address |
| `ls_req_wdata` | out | 32 | store data |
| `ls_req_wstrb` | out | 4 | byte enable |
| `ls_resp_valid` | in | 1 | local access done |
| `ls_resp_rdata` | in | 32 | load data |
| `cmd_req_valid` | out | 1 | command fabric request |
| `cmd_req` | out | `mcu_cmd_req_t` | MMIO/broadcast/stream command |
| `cmd_req_ready` | in | 1 | fabric accept |
| `cmd_resp_valid` | in | 1 | command complete |
| `cmd_resp` | in | `mcu_cmd_resp_t` | read data or error |
| `irq_taken` | in | 1 | interrupt trap taken |
| `irq_vector` | in | 32 | irq handler address |

#### 內部子模組

1. fetch/decode
2. 16-entry GPR file
3. ALU/branch unit
4. CSR unit
5. stack/call-return support
6. interrupt entry/return control

#### 規則

1. MCU 是 architecturally visible 的唯一 runtime control master。
2. 所有外部 side effect 都必須經由 `cmd_req` 路徑或 local CSR/store 路徑產生。
3. `CALL` / `RET` / `IRET` 必須為硬體原生支援，不允許編譯器用巨量 template 展開替代。

### 8.5 `cc_cmd_fabric`

#### 功能

1. 解碼 MCU 的 command request。
2. 將 local CSR、DMA MMIO、cluster AHB、NLU AHB 分流。
3. 支援 mask-based broadcast。
4. 提供 deterministic blocking completion。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `mcu_cmd_req_valid` | in | 1 | MCU command valid |
| `mcu_cmd_req` | in | `mcu_cmd_req_t` | request payload |
| `mcu_cmd_req_ready` | out | 1 | 可接收 |
| `mcu_cmd_resp_valid` | out | 1 | command complete |
| `mcu_cmd_resp` | out | `mcu_cmd_resp_t` | response/error |
| `dma_mmio_*` | out/in | implementation-defined | DMA local route |
| `m_cl_ahb_*` | out/in | AHB-Lite | cluster route |
| `m_nlu_ahb_*` | out/in | AHB-Lite | NLU route |

#### 內部邏輯

1. target decoder
2. broadcast sequencer
3. completion collector
4. error encoder

#### Broadcast 規則

1. `mask=0` 時不對外發 transaction，並立即返回完成。
2. `mask!=0` 時，fabric 必須對每個命中 target 執行相同 transfer。
3. 只有全部 target 都完成後才回 `cmd_resp_valid`。
4. 任一 target 出錯，fabric 必須帶回錯誤 target id 與 local address。

### 8.6 `cc_irq_router`

#### 功能

1. 同步所有外部 level IRQ。
2. 建立 sticky pending bitmap。
3. 與 `IRQ_ENABLE` 遮罩相與後決定是否對 MCU 產生 trap。
4. 提供 ack 介面與 cause 選擇。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `cluster_irq` | in | `NUM_CLUSTERS` | cluster level IRQ |
| `nlu_irq` | in | `NUM_NLU` | NLU level IRQ |
| `dma_irq` | in | 1 | DMA level IRQ |
| `irq_pending_lo/hi` | out | 32 | pending bitmap |
| `irq_ack_lo/hi` | in | 32 | ack bitmap |
| `irq_enable_lo/hi` | in | 32 | enable bitmap |
| `mcu_irq_req` | out | 1 | 對 MCU 產生 interrupt trap |
| `mcu_irq_id` | out | 8 或更多 | cause id |

#### 規則

1. ack 只清 sticky latch，不改變來源硬體狀態。
2. source level 未解除時，pending 必須重新置位。

### 8.7 `cc_dma_engine`

#### 功能

1. 接受 MCU 寫入的 MMIO 設定。
2. 接受 MCU 串流進來的 `.hacc.dma` 派生 payload。
3. 透過 DRAM AXI 與 cluster data fabric 搬運 tensor data。
4. 回報 done/error status 與 irq。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `dma_mmio_req_valid` | in | 1 | MMIO request |
| `dma_mmio_req` | in | `mmio_req_t` | reg access |
| `dma_mmio_resp_valid` | out | 1 | MMIO complete |
| `dma_stream_valid` | in | 1 | stream word valid |
| `dma_stream_data` | in | 32 | stream word |
| `dma_stream_ctrl` | in | control bits | SOP/EOP/LAST |
| `dma_stream_ready` | out | 1 | sink ready |
| `m_mem_axi_*` | out/in | AXI4 | DRAM requester |
| `cluster_data_req` | out | `cluster_data_req_t` | cluster SPM request |
| `cluster_data_req_valid` | out | 1 | request valid |
| `cluster_data_req_ready` | in | 1 | fabric ready |
| `cluster_data_resp_valid` | in | 1 | response valid |
| `cluster_data_resp` | in | implementation-defined | data/error |
| `dma_irq` | out | 1 | done/error irq |

#### 正式語意

1. DMA engine 不得自行從 `cc_data_sram` 的 descriptor/payload ABI region 讀 `.hacc.dma`。
2. DMA engine 只對已由 MCU 下發的 command/stream payload 負責。
3. stream payload 基本粒度為 32-bit。
4. cluster 端因為是 AXI4-Lite slave，DMA 寫 cluster SPM 時不得假設 burst。

### 8.8 `cc_cluster_data_fabric`

#### 功能

1. 協調 DMA engine 與 NLU AXI requester 對 cluster SPM 的存取。
2. 依 cluster id 將 request 路由到對應 cluster AXI4-Lite data master。
3. 可選支援 round-robin 或固定優先權仲裁。

#### 規則

1. DMA 與 NLU request 不得直接同時驅動外部 cluster data port。
2. 仲裁策略需 deterministic，並在 PMU 或 trace 可觀測。

---

## 9. Section 與 local memory 規則

### 9.1 正式 section route

| Section | 載入位置 | runtime path |
|---|---|---|
| `.hacc.core` | I-SRAM | MCU fetch |
| `.hacc.job` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.block` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.profile` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO -> cluster HDDU |
| `.hacc.dma` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO -> DMA engine |
| `.hacc.agu` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO -> cluster HDDU |
| `.hacc.nlu` | Data-SRAM descriptor/payload ABI region | MCU stream/MMIO -> NLU MMIO |
| `.hacc.pe` | Data-SRAM descriptor/payload ABI region | MCU stream -> cluster NoC |
| `.hacc.scan` | Data-SRAM descriptor/payload ABI region | MCU stream -> cluster NoC |
| `.hacc.patch` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.debug` | Data-SRAM event/debug ABI region | debug/runtime optional |

### 9.2 重要限制

1. loader 不得直接對 cluster 發 `.hacc.pe/.hacc.scan` side effect。
2. loader 不得直接啟動 DMA。
3. `.hacc.dma` 不是 DMA engine 自動 fetch 的 descriptor heap，而是 MCU runtime 解譯後送入 DMA 的 payload/rule source。

---

## 10. Runtime 流程

### 10.1 Reset / Boot

1. `rst_n` deassert 後，host 或 boot ROM 決定 boot mode。
2. `cc_boot_host_if` 接收 manifest。
3. `cc_section_loader` 將 `.hacc.core` 載入 I-SRAM，將其餘 sections 載入 `cc_data_sram` 的對應 ABI region。
4. loader 完成後釋放 MCU 執行。

### 10.2 Load 後初始化

1. MCU 初始化 stack、IRQ mask、global state。
2. MCU 讀 `.hacc.job`，取得各 table base/count 與 capability。
3. MCU 視需要把 `.hacc.pe`、`.hacc.scan` 先載入 cluster，或在 block 執行前按需載入。

### 10.3 Block / Wave 執行

1. MCU 從 `.hacc.block` 讀 block descriptor。
2. MCU 依 loop shape、profile index、DMA index、patch 等規則計算 wave instance。
3. MCU 透過 `cc_cmd_fabric` 對 cluster 套用 profile/AGU 或下 start。
4. MCU 對 DMA stream `.hacc.dma` 派生 payload。
5. MCU 若遇到 NLU phase，則對 NLU MMIO/stream 下發 config/start。
6. MCU 以 polling 或 IRQ 模式等待完成。

### 10.4 Error / Abort

1. DMA/cluster/NLU 任一單元出錯，`cc_irq_router` 置位 pending。
2. MCU 進入 handler 或 polling path 收斂錯誤。
3. MCU 必須負責發 abort/cleanup sequence。
4. controller 對 host 的 `controller_irq` 可做 summary interrupt。

---

## 11. 建議的 local CSR 與狀態摘要

建議 controller 至少提供：

1. `LOADER_STATUS`
2. `LOADER_ERR_CODE`
3. `CORE_STATUS`
4. `IRQ_PENDING_LO/HI`
5. `IRQ_ENABLE_LO/HI`
6. `IRQ_ACK_LO/HI`
7. `DMA_STATUS`
8. `DMA_ERR_CODE`
9. `LAST_TARGET_ID`
10. `LAST_FAULT_ADDR`
11. `CYCLE_CNT`
12. `CMD_COUNT`

---

## 12. RTL 落地規則

### 12.1 模組介面風格

1. 所有一級模組需明確列出 RTL I/O，不以 C++ method 或隱藏 side effect 表示介面。
2. 所有 blocking command path 都必須有 `req_valid/req_ready/resp_valid` 或等價握手。
3. stream path 必須有 `valid/ready/data/control`。

### 12.2 狀態機規則

1. 模組狀態需可在 trace 中觀測。
2. error path 必須有獨立 `ERR` 狀態，而不是直接回 `IDLE`。
3. broadcast sequencer 必須有可觀測的 current target id。

### 12.3 驗證規則

至少需要以下驗證點：

1. loader 只做 local copy，不對 cluster/NLU/DMA 產生 runtime side effect。
2. `cc_data_sram` descriptor/payload ABI region 的 32-bit word access correctness。
3. broadcast all-target completion semantics。
4. DMA stream 由 MCU 下發，而非 DMA 自取 descriptor。
5. level IRQ + sticky pending + ack reassert semantics。
6. subroutine 與 IRQ nesting 基本流程正確。

---

## 13. 與舊版架構的差異摘要

1. 舊版的 `execution complex` 主敘事已移除。
2. 舊版 `cc_section_loader` 直接發 NoC command 的模式已移除。
3. 舊版由硬體 block expander 展開 wave 的假設已移除。
4. DMA engine 的 descriptor ownership 改由 MCU 掌控。

這些差異是本版規格的核心，不是實作選項。

---

## 14. 總結

本版 HACC Core Controller 的正式定義是：

1. Loader 負責把東西載進 local memory。
2. MCU 負責 runtime control。
3. Command fabric 負責 deterministic MMIO/broadcast/stream routing。
4. DMA 只執行 MCU 已經明確下發的搬運命令。
5. IRQ 以 level + sticky + ack 模型收斂。

只要這五個原則不被打破，controller 的 RTL、韌體與 compiler handoff 就會維持一致，且不再依賴舊版 execution complex 的隱含行為。
