# HACC Core Controller 硬體架構規格書

本文件定義 HACC Core Controller 在第二版架構下的正式 micro architecture。本文不是概念提案，而是供 RTL 切模、韌體開發、compiler handoff、驗證規劃與 reviewer 審查使用的工程規格。

本版最重要的設計變更是：runtime control 從舊的 descriptor execution complex 改成由 `cc_core_mcu` 直接掌控，且 `cc_core_mcu` 正式具體化為一個 32-bit 5-stage pipelined RV32I_Zmmul_Zicsr core（machine-mode only）。`cc_section_loader` 只做 local section copy；對 cluster、DMA、NLU、PLIC 的 runtime side effect 都由 core 透過標準 load/store MMIO 顯式觸發。

---

## 1. 正式結論

### 1.1 架構主結論

本版 controller 採用以下主結構：

1. 一個 32-bit 5-stage pipelined `cc_core_mcu`（RV32I_Zmmul_Zicsr, machine-mode only）作為唯一 runtime orchestrator。
2. 一個 `cc_section_loader`，只負責將 host/boot firmware 提供的 manifest section 複製到 local memories。
3. 一個 `cc_cmd_fabric`，承接 core LSU 發出的 MMIO access，並路由到 local CSR / DMA / per-cluster AHB / per-NLU AHB / PLIC。
4. 一個 `cc_dma_engine`，作為由 MCU 透過 MMIO 與 stream 方式控制的 data mover。
5. 一個 `cc_plic`，統整 cluster、DMA、NLU 與 local timer 的 level IRQ，對 core 提供 machine external interrupt。
6. 一個 `cc_cluster_data_fabric`，負責協調 DMA engine 與 NLU data master 對 cluster SPM data path 的存取。

正式原則：cluster config、HDDU 設定、NoC sideband、DMA 啟動、NLU 啟動都必須由 firmware 以 load/store MMIO 完成；硬體不得再藏任何 profile apply engine、wave expander 或隱式 scheduler。

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
7. 支援 deterministic MMIO ordering 與可驗證的 side effect 邊界。
8. 支援所有 cluster config 以軟體透過 MMIO 完成，不依賴專用硬體 sequencer。

### 2.2 非目標

1. 不在 controller 內放大型 tensor SRAM。
2. 不把 NLU 內部 SRAM 納入 DMA engine ownership。
3. 不在 loader 中實作完整通用 ELF parser。
4. 不再把 runtime wave orchestration 交給隱含的 execution complex 黑盒。
5. 不在 controller 內加入專用 cluster-profile apply micro-engine。

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

### 3.5 IRQ / PLIC 模型

所有外部完成/錯誤事件都以 level signal 輸入 `cc_plic`。正式規則：

1. signal 經同步器後進入 pending latch。
2. pending latch 為 sticky bit，直到 firmware 透過 PLIC claim/complete 完成對應 source。
3. `cc_plic` 對 core 拉起 machine external interrupt（`MEIP`）。
4. 若來源 level 在 complete 後仍為 asserted，pending 必須重新置位。
5. 此規則同時適用 cluster IRQ、DMA IRQ、NLU IRQ 與 local timer/event source。

### 3.6 Core CSR 與軟體模型

本版 core 採以下正式約束：

1. privilege level 僅實作 machine mode。
2. architected CSR 以 RV32I_Zmmul_Zicsr 標準 machine CSR 為主：`mstatus/misa/mie/mtvec/mscratch/mepc/mcause/mtval/mip/mcycle/minstret`。
3. cluster、DMA、NLU、PLIC、loader、host coordination 一律使用 MMIO，不以 custom CSR opcode 暴露。
4. custom CSR range `0x7C0 ~ 0x7FF` 保留給未來 debug/perf 擴充，本版不放 runtime control 定址。

---

## 4. Top-level 架構

### 4.1 方塊圖

```text
                         +-----------------------+
                         | cc_boot_host_if       |
                         | - host CSR bank       |
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
                        | - RV32I_Zmmul_Zicsr  |
                        | - IF/ID/EX/MEM/WB     |
                        | - hazard/bypass/irq   |
                        | - local load/store    |
                         +-----------+-----------+
                                     |
                                     v
                         +-----------------------+
                         | cc_cmd_fabric         |
                        | - MMIO decode         |
                        | - DMA route           |
                        | - cluster AHB route   |
                        | - NLU AHB route       |
                        | - PLIC route          |
                        | - mask-broadcast      |
                         +----+------------+-----+
                              |            |
                              v            v
                      +----------------+  +----------------+
                      | cc_dma_engine  |  | cc_plic        |
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
3. Runtime control layer：`cc_core_mcu`、`cc_cmd_fabric`、`cc_plic`。
4. Data movement layer：`cc_dma_engine`、`cc_cluster_data_fabric`。
5. External target layer：cluster command/data ports 與 NLU MMIO/data requester。

---

## 5. 參數化規格

| Parameter | 建議值 | 說明 |
|---|---:|---|
| `NUM_CLUSTERS` | 1, 2, 4, 8, 16 | cluster 數量 |
| `NUM_NLU` | 0, 1, 2, 4 | NLU 數量 |
| `CORE_XLEN` | 32 | MCU 資料寬度 |
| `CORE_GPR_NUM` | 32 | RV32I GPR 數量 |
| `CORE_PIPE_STAGES` | 5 | IF/ID/EX/MEM/WB |
| `CORE_ISA` | RV32I_Zmmul_Zicsr | 含 Zmmul（MUL 系列），不含 DIV/A/F/D/C |
| `CORE_PRIV_MODES` | M only | 不實作 S/U mode |
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
| `CMD_FABRIC_OUTSTANDING` | 1 | 本版 MMIO path 採 blocking 語意 |
| `PLIC_NUM_SOURCES` | `NUM_CLUSTERS + NUM_NLU + 8` | cluster/dma/nlu/timer/error sources |

### 5.1 Core pipeline 正式要求

1. pipeline stage 固定為 IF / ID / EX / MEM / WB。
2. 至少需支援 EX/MEM 與 MEM/WB bypass。
3. load-use hazard 允許 interlock 1 cycle 或更多，但語意必須正確。
4. CSR read-modify-write 需維持 RV32I_Zmmul_Zicsr 單指令原子語意。
5. MMIO load/store 不得被亂序執行；對同一 hart 必須維持 program order。

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

### 6.4 Core-visible address space 規劃

下表為 core 的 32-bit load/store address map。正式原則是：core 以單一 flat address space 執行；local memory 與所有 runtime control 都透過標準 load/store 完成。

| Base | End | Size | Region | 說明 |
|---|---|---:|---|---|
| `0x0000_0000` | `0x0000_3FFF` | 16 KB | I-SRAM | `.hacc.core`，instruction fetch only |
| `0x1000_0000` | `0x1000_FFFF` | 64 KB | Data-SRAM | descriptor/payload/event/debug ABI |
| `0x2000_0000` | `0x2000_0FFF` | 4 KB | Local control MMIO | core local control、cluster mask、error/status |
| `0x2000_1000` | `0x2000_1FFF` | 4 KB | DMA MMIO | DMA register bank |
| `0x2000_1800` | `0x2000_18FF` | 256 B | DMA stream window | write-only push FIFO，供 firmware 寫入 stream payload |
| `0x2000_2000` | `0x2000_2FFF` | 4 KB | Core local timer/MSIP | optional，供 machine timer/software interrupt |
| `0x0C00_0000` | `0x0C00_FFFF` | 64 KB | PLIC MMIO | claim/complete、enable、priority、pending |
| `0x4000_0000` | `0x400F_FFFF` | 1 MB | Cluster unicast MMIO | 每 cluster stride `0x0001_0000` |
| `0x5000_0000` | `0x5000_FFFF` | 64 KB | Cluster masked-broadcast MMIO | 依 `CLUSTER_MASK_LO/HI` fan-out |
| `0x6000_0000` | `0x6000_FFFF` | 64 KB | NLU MMIO | 每 NLU stride `0x0000_1000` |

#### 6.4.1 Cluster unicast window

正式公式：

`cluster_n_base = 0x4000_0000 + n * 0x0001_0000`

cluster 內部 offset 維持現有契約：

1. `+0x0000 ~ +0x00FF`：SPM config / PMU
2. `+0x1000 ~ +0x1FFF`：HDDU MMIO
3. `+0x2000 ~ +0x20FF`：NoC command sideband

#### 6.4.2 Cluster masked-broadcast window

正式公式：

`cluster_bcast_addr = 0x5000_0000 + cluster_local_offset`

規則：

1. firmware 先寫 `CLUSTER_MASK_LO/HI`，再對 broadcast window 做 store。
2. fabric 以遞增 cluster id 順序複製同一筆 transaction 到所有命中的 cluster。
3. 只有全部命中 target 都完成後，該 store 才算完成。
4. broadcast window 預設只允許 write；read 僅在 mask popcount=1 時合法，否則回 fault。
5. 此機制只負責 transaction fan-out，不承擔任何 profile merge、descriptor parse 或 wave schedule。

#### 6.4.3 NLU MMIO window

正式公式：

`nlu_n_base = 0x6000_0000 + n * 0x0000_1000`

其最小 register contract 需符合本文件 3.4 所列 NLU MMIO window。

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
5. Host 對 core boot/halt/resume 與 cluster mask 的可觀測控制。

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
3. host-visible CSR 是 host control plane；不直接取代 core 自己的 RV32 CSR。

#### Host-visible CSR bank 規劃

下表定義 `s_ctrl_axi_*` 至少要提供的 host-visible CSR offset。

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x0000` | `HOST_CAP0` | R | capability bitmap：RV32I_Zmmul_Zicsr、DMA、PLIC、broadcast support |
| `0x0004` | `HOST_CAP1` | R | `NUM_CLUSTERS`、`NUM_NLU`、memory size encode |
| `0x0008` | `HOST_CTRL` | R/W | bit0=`core_en`, bit1=`core_haltreq`, bit2=`core_resume`, bit3=`sw_reset` |
| `0x000C` | `HOST_STATUS` | R | bit0=`loader_busy`, bit1=`core_halted`, bit2=`core_running`, bit3=`faulted` |
| `0x0010` | `CORE_BOOT_ADDR` | R/W | reset/boot PC |
| `0x0014` | `CORE_TRAP_VECTOR` | R/W | firmware 預設 trap vector mirror |
| `0x0018` | `CORE_PC_SNAPSHOT` | R | 最近一次可觀測 PC |
| `0x001C` | `CORE_CAUSE_SNAPSHOT` | R | 最近一次 trap/abort cause |
| `0x0020` | `MANIFEST_ADDR_LO` | R/W | manifest base low |
| `0x0024` | `MANIFEST_ADDR_HI` | R/W | manifest base high |
| `0x0028` | `MANIFEST_SIZE` | R/W | manifest bytes |
| `0x002C` | `MANIFEST_KICK` | W | push loader start pulse |
| `0x0030` | `LOADER_STATUS` | R | loader 狀態 |
| `0x0034` | `LOADER_ERR_CODE` | R/W1C | loader error code |
| `0x0040` | `IRQ_SUMMARY` | R | 對 host 可見的 summary pending |
| `0x0044` | `IRQ_FORCE_ACK` | W1C | host debug 用 force ack |
| `0x0050` | `CLUSTER_MASK_LO` | R/W | 與 core local MMIO mirror |
| `0x0054` | `CLUSTER_MASK_HI` | R/W | 與 core local MMIO mirror |
| `0x0058` | `LAST_MMIO_TARGET` | R | 最近一次 MMIO 錯誤 target id |
| `0x005C` | `LAST_MMIO_ADDR` | R | 最近一次 MMIO fault address |
| `0x0060` | `TRACE_BASE` | R/W | trace buffer base |
| `0x0064` | `TRACE_SIZE` | R/W | trace buffer size |
| `0x0068` | `TRACE_CTRL` | R/W | trace enable/flush |

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

1. 執行 RV32I_Zmmul_Zicsr machine-mode firmware。
2. 讀取 local descriptor 與 payload tables。
3. 以標準 load/store 對 local memory 與 MMIO fabric 進行存取。
4. 接收 `cc_plic` 的 machine external interrupt 與 local timer interrupt。

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
| `mmio_req_valid` | out | 1 | MMIO request valid |
| `mmio_req_write` | out | 1 | MMIO write strobe |
| `mmio_req_addr` | out | 32 | MMIO address |
| `mmio_req_wdata` | out | 32 | MMIO write data |
| `mmio_req_wstrb` | out | 4 | MMIO byte enable |
| `mmio_resp_valid` | in | 1 | MMIO complete |
| `mmio_resp_rdata` | in | 32 | MMIO read data |
| `irq_meip` | in | 1 | machine external interrupt pending |
| `irq_mtip` | in | 1 | machine timer interrupt pending |

#### 內部子模組

1. IF stage
2. ID stage + 32-entry GPR file
3. EX stage + ALU/branch/CSR execute
4. MEM stage
5. WB stage
6. hazard detect + bypass network
7. interrupt/trap entry-return control

#### RV32I_Zmmul_Zicsr architected CSR 規劃

| CSR Addr | Name | RW | 說明 |
|---:|---|---|---|
| `0x301` | `misa` | R | 固定回報 RV32I + Zmmul + Zicsr capability |
| `0x300` | `mstatus` | R/W | machine interrupt enable 等控制 |
| `0x304` | `mie` | R/W | `MEIE/MTIE/MSIE` enable |
| `0x305` | `mtvec` | R/W | trap vector base |
| `0x340` | `mscratch` | R/W | firmware scratch |
| `0x341` | `mepc` | R/W | trap return PC |
| `0x342` | `mcause` | R/W | trap cause |
| `0x343` | `mtval` | R/W | badaddr/fault info |
| `0x344` | `mip` | R | interrupt pending view |
| `0xB00` | `mcycle` | R/W | cycle counter |
| `0xB02` | `minstret` | R/W | retired instruction counter |

#### 規則

1. MCU 是 architecturally visible 的唯一 runtime control master。
2. 所有外部 side effect 都必須經由 MMIO store 或 local memory store 產生。
3. cluster config、HDDU config、NoC sideband、DMA config、NLU config 都必須由 firmware 顯式發出 store sequence。
4. 本版不增加自訂 runtime control 指令；firmware 只依賴 RV32I_Zmmul_Zicsr + MMIO。

### 8.5 `cc_cmd_fabric`

#### 功能

1. 解碼 core LSU 的 MMIO request。
2. 將 local CSR、DMA MMIO、cluster AHB、NLU AHB、PLIC MMIO 分流。
3. 支援 cluster masked-broadcast alias window。
4. 提供 deterministic blocking completion。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `core_mmio_req_valid` | in | 1 | core MMIO valid |
| `core_mmio_req_write` | in | 1 | MMIO write |
| `core_mmio_req_addr` | in | 32 | MMIO address |
| `core_mmio_req_wdata` | in | 32 | write data |
| `core_mmio_req_wstrb` | in | 4 | byte enable |
| `core_mmio_resp_valid` | out | 1 | MMIO complete |
| `core_mmio_resp_rdata` | out | 32 | read data |
| `dma_mmio_*` | out/in | implementation-defined | DMA local route |
| `m_cl_ahb_*` | out/in | AHB-Lite | cluster route |
| `m_nlu_ahb_*` | out/in | AHB-Lite | NLU route |
| `plic_mmio_*` | out/in | implementation-defined | PLIC route |

#### 內部邏輯

1. address decoder
2. cluster masked-broadcast sequencer
3. completion collector
4. error encoder
5. local register bank（含 `CLUSTER_MASK_LO/HI`、`LAST_FAULT_ADDR`）

#### Broadcast 規則

1. `mask=0` 時不對外發 transaction，並立即返回完成。
2. `mask!=0` 時，fabric 必須對每個命中 target 執行相同 transfer。
3. 只有全部 target 都完成後才回應該次 MMIO store/load 完成。
4. 任一 target 出錯，fabric 必須帶回錯誤 target id 與 local address。
5. 針對 broadcast read，若 `popcount(mask)!=1`，fabric 必須回 fault。

### 8.6 `cc_plic`

#### 功能

1. 同步所有外部 level IRQ。
2. 建立 sticky pending bitmap。
3. 依 priority/enable 決定是否對 core 產生 machine external interrupt。
4. 提供 claim/complete 介面。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `cluster_irq` | in | `NUM_CLUSTERS` | cluster level IRQ |
| `nlu_irq` | in | `NUM_NLU` | NLU level IRQ |
| `dma_irq` | in | 1 | DMA level IRQ |
| `irq_pending_lo/hi` | out | 32 | pending bitmap |
| `meip` | out | 1 | 對 core 產生 machine external interrupt |
| `plic_mmio_*` | in/out | implementation-defined | priority/enable/claim/complete |

#### 規則

1. claim 只回傳目前選到的 source id，不代表來源已解除。
2. complete 才是 firmware 對該 source 的顯式 ack。
3. source level 未解除時，pending 必須重新置位。
4. 本版可採簡化 single-hart PLIC，但 MMIO 語意必須維持 claim/complete 相容。

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
5. core 從 `CORE_BOOT_ADDR` 開始取指，並以 RV32I trap model 進入初始化程式。

### 10.2 Load 後初始化

1. MCU 初始化 stack、`mtvec`、`mie/mstatus`、cluster mask 與 global state。
2. MCU 讀 `.hacc.job`，取得各 table base/count 與 capability。
3. MCU 視需要把 `.hacc.pe`、`.hacc.scan` 先載入 cluster，或在 block 執行前按需載入。

### 10.3 Block / Wave 執行

1. MCU 從 `.hacc.block` 讀 block descriptor。
2. MCU 依 loop shape、profile index、DMA index、patch 等規則計算 wave instance。
3. MCU 以 store sequence 對 cluster unicast 或 masked-broadcast window 套用 profile/AGU 或下 start。
4. MCU 對 DMA MMIO 與 DMA stream window 寫入 `.hacc.dma` 派生 payload。
5. MCU 若遇到 NLU phase，則對 NLU MMIO window 下發 config/start。
6. MCU 以 polling 或 IRQ 模式等待完成。

### 10.4 Error / Abort

1. DMA/cluster/NLU 任一單元出錯，`cc_plic` 置位 pending。
2. MCU 進入 handler 或 polling path 收斂錯誤。
3. MCU 必須負責發 abort/cleanup sequence。
4. controller 對 host 的 `controller_irq` 可做 summary interrupt。

---

## 11. Core local MMIO / CSR 規劃

下表定義 core 在 `0x2000_0000 ~ 0x2000_0FFF` 可見的 local control MMIO。這些是 firmware 使用的 controller-local register，不是 RV32 CSR opcode 空間。

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x000` | `CORE_STATUS` | R | bit0=`running`, bit1=`in_trap`, bit2=`faulted` |
| `0x004` | `CORE_CTRL` | R/W | bit0=`haltreq`, bit1=`single_step`, bit2=`resume` |
| `0x008` | `CLUSTER_MASK_LO` | R/W | cluster broadcast mask low |
| `0x00C` | `CLUSTER_MASK_HI` | R/W | cluster broadcast mask high |
| `0x010` | `MMIO_ERR_STATUS` | R/W1C | fabric error summary |
| `0x014` | `LAST_TARGET_ID` | R | 最近一次 fault target |
| `0x018` | `LAST_FAULT_ADDR` | R | 最近一次 fault address |
| `0x01C` | `LAST_FAULT_INFO` | R | error code / bus response |
| `0x020` | `DMA_STATUS` | R | DMA 狀態 mirror |
| `0x024` | `DMA_ERR_CODE` | R/W1C | DMA error mirror |
| `0x028` | `TRACE_CTRL` | R/W | trace 控制 |
| `0x02C` | `TRACE_WPTR` | R | trace write pointer |
| `0x030` | `CYCLE_CNT_LO` | R | cycle counter low |
| `0x034` | `CYCLE_CNT_HI` | R | cycle counter high |
| `0x038` | `INSTRET_LO` | R | retired inst low |
| `0x03C` | `INSTRET_HI` | R | retired inst high |
| `0x040` | `SW_IRQ_SET` | W | software event trigger |
| `0x044` | `SW_IRQ_CLR` | W | software event clear |
| `0x048` | `BOOT_REASON` | R | boot/trap resume source |

---

## 12. RTL 落地規則

### 12.1 模組介面風格

1. 所有一級模組需明確列出 RTL I/O，不以 C++ method 或隱藏 side effect 表示介面。
2. 所有 blocking MMIO path 都必須有 `req_valid/req_ready/resp_valid` 或等價握手。
3. stream path 必須有 `valid/ready/data/control`。

### 12.2 狀態機規則

1. 模組狀態需可在 trace 中觀測。
2. error path 必須有獨立 `ERR` 狀態，而不是直接回 `IDLE`。
3. masked-broadcast sequencer 必須有可觀測的 current target id。

### 12.3 驗證規則

至少需要以下驗證點：

1. loader 只做 local copy，不對 cluster/NLU/DMA 產生 runtime side effect。
2. `cc_data_sram` descriptor/payload ABI region 的 32-bit word access correctness。
3. broadcast all-target completion semantics。
4. DMA stream 由 MCU 下發，而非 DMA 自取 descriptor。
5. PLIC claim/complete 與 level reassert semantics。
6. RV32I_Zmmul_Zicsr 5-stage pipeline 的 branch/load-use/CSR hazard correctness。
7. cluster config 完全經由 firmware MMIO sequence 完成，無隱式硬體 side effect。

---

## 13. 與舊版架構的差異摘要

1. 舊版的 `execution complex` 主敘事已移除。
2. 舊版 `cc_section_loader` 直接發 NoC command 的模式已移除。
3. 舊版由硬體 block expander 展開 wave 的假設已移除。
4. DMA engine 的 descriptor ownership 改由 MCU 掌控。
5. 本版改以 RV32I_Zmmul_Zicsr 5-stage pipeline + MMIO software control 為正式控制模型。

這些差異是本版規格的核心，不是實作選項。

---

## 14. 總結

本版 HACC Core Controller 的正式定義是：

1. Loader 負責把東西載進 local memory。
2. RV32I_Zmmul_Zicsr 5-stage core 負責 runtime control。
3. MMIO fabric 負責 deterministic MMIO/masked-broadcast/stream routing。
4. DMA 只執行 MCU 已經明確下發的搬運命令。
5. IRQ 以 PLIC claim/complete + level reassert 模型收斂。

只要這五個原則不被打破，controller 的 RTL、韌體與 compiler handoff 就會維持一致，且不再依賴舊版 execution complex 的隱含行為。
