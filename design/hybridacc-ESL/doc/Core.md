# HACC Core Controller 硬體架構規格書

本文件定義 HACC Core Controller 在第二版架構下的正式 micro architecture。本文不是概念提案，而是供 RTL 切模、韌體開發、compiler handoff、驗證規劃與 reviewer 審查使用的工程規格。

本版最重要的設計變更是：runtime control 從舊的 descriptor execution complex 改成由 `cc_core_mcu` 直接掌控，且 `cc_core_mcu` 正式具體化為一個 32-bit 5-stage pipelined RV32I_Zmmul_Zicsr core（machine-mode only）。`cc_section_loader` 只做 local section copy；對 cluster、DMA、NLU、PLIC 的 runtime side effect 都由 core 透過標準 load/store MMIO 顯式觸發。

本次修訂另外明確關閉先前版本中的幾個主要歧義：

1. `cc_boot_host_if` 的 MMIO 是 host-visible control plane，不屬於 core flat address space。
2. `cc_plic` 補上 compact single-hart MMIO register map。
3. `cc_dma_engine` 改採 architected MMIO staging register + doorbell submit 模型；`dma_stream_*` 不再是 architected 介面。
4. core local MMIO 與 host-visible CSR 的關係、鏡射與寫入仲裁規則正式定義。
5. local timer / software interrupt 與 PLIC 的職責切開，避免 `MTIP/MSIP` 與 `MEIP` 混疊。

---

## 0. 文件定位與術語

本文件使用以下術語：

1. `MUST`：RTL 與 firmware 必須遵守的正式約束。
2. `SHOULD`：強烈建議的實作方式；若偏離，需在整合文件額外註明。
3. `host-visible MMIO`：只透過 `s_ctrl_axi_*` 對 host 開放的 control plane，不屬於 core 的 flat address space。
4. `core-visible MMIO`：`cc_core_mcu` 透過標準 load/store 可見的 local control、DMA、PLIC、cluster、NLU 位址空間。
5. `architected CSR`：RV32I_Zmmul_Zicsr 定義的標準 machine CSR；controller-specific state 不得偽裝成 custom CSR opcode。
6. `mirror`：同一份實體狀態同時對 host 與 core 暴露兩個地址視圖；本版只允許極少數 shared mirror。
7. `snapshot`：由 host control plane 只讀觀測的狀態複本；snapshot 不代表 host 擁有該狀態的 runtime write ownership。

## 1. 正式結論

### 1.1 架構主結論

本版 controller 採用以下主結構：

1. 一個 32-bit 5-stage pipelined `cc_core_mcu`（RV32I_Zmmul_Zicsr, machine-mode only）作為唯一 runtime orchestrator。
2. 一個 `cc_boot_host_if`，只對 host 提供 boot、manifest、trace、snapshot 與 halt/resume 的 host-visible control plane。
3. 一個 `cc_section_loader`，只負責將 host/boot firmware 提供的 manifest section 複製到 local memories。
4. 一個 `cc_cmd_fabric`，承接 core LSU 發出的 MMIO access，並路由到 core local control / DMA / per-cluster AHB / per-NLU AHB / PLIC。
5. 一個 `cc_dma_engine`，作為由 MCU 透過 architected MMIO register bank 與 submit doorbell 控制的 data mover。
6. 一個 `cc_plic`，統整 cluster、DMA、NLU 與少數 controller-local shared external event source 的 level IRQ，對 core 提供 machine external interrupt。
7. 一個 `cc_core_local_irq`，提供 machine timer interrupt 與 software interrupt；此路徑不經 PLIC。
8. 一個 `cc_cluster_data_fabric`，負責協調 DMA engine 與 NLU data master 對 cluster SPM data path 的存取。

正式原則：cluster config、HDDU 設定、NoC sideband、DMA 啟動、NLU 啟動都必須由 firmware 以 load/store MMIO 完成；硬體不得再藏任何 profile apply engine、wave expander、descriptor parser、hidden stream micro-protocol 或隱式 scheduler。

### 1.2 被移除的 primary runtime 模組

下列模組不再作為 primary runtime control path：

1. `cc_job_manager`
2. `cc_block_fetch`
3. `cc_block_expander`
4. `cc_wave_scheduler`

若未來為了效能加入輔助式 prefetch/assist 硬體，仍不得改變 MCU 為唯一 architecturally visible control master 的原則。

另外，本版也正式移除下列 architected 假設：

1. `dma_stream_*` 作為 firmware 可見控制協定的假設。
2. loader 直接對 cluster、DMA、NLU 產生 runtime side effect 的假設。
3. local timer 經由 PLIC 進入 `MEIP` 的假設。

### 1.3 DRAM AXI4 共享結論

Controller top-level 只對外暴露一組 `m_mem_axi_*` DRAM AXI4 master。此介面由以下兩個內部 requester 共用：

1. `cc_section_loader`
2. `cc_dma_engine`

正式規則：

1. reset/load phase 預設由 loader 擁有 DRAM master，`load_phase=1`。
2. run phase 預設由 DMA 擁有 DRAM master，`load_phase=0`。
3. 若系統不需要 runtime reload，top-level 可採單純 owner mux，不必實作通用 arbiter。
4. 若實作需要在 runtime 再次載入 section，則必須加入 `cc_mem_axi_arbiter`，但外部介面仍只有一組 `m_mem_axi_*`。
5. runtime reload 的建議預設是：先 halt core、等待 DMA idle，再把 owner 從 DMA 切回 loader。若不採此限制，就必須提供可驗證的仲裁公平性與 deadlock-free 保證。
6. 不允許 loader 與 DMA 直接各自對外暴露獨立 DRAM master port。

### 1.4 本版已關閉的主要規格歧義

本版文件正式關閉以下先前未定義或互相衝突的點：

1. DMA MMIO window 與 DMA stream window 的地址重疊問題：stream window 已移除，不再存在重疊。
2. `cc_plic` 缺 register map 的問題：本文件定義 compact single-hart PLIC MMIO map。
3. `cc_dma_engine` 缺 register map 的問題：本文件定義 staging register、doorbell、status/error/tag register。
4. `cc_boot_host_if` 與 core local MMIO 的所有權不清問題：本文件定義 host-only、core-only、shared-mirror 三種狀態類型。
5. local timer / software interrupt 是否經 PLIC 的衝突：本文件定義 `MTIP/MSIP` 走 `cc_core_local_irq`，`MEIP` 才由 `cc_plic` 提供。

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

1. `0x0000 ~ 0x00FF`：SPM config / PMU window。
2. `0x1000 ~ 0x1FFF`：HDDU MMIO passthrough window。
3. `0x2000 ~ 0x20FF`：NoC command sideband window。

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

`NLU_CFG_PAYLOAD_WINDOW` 的語意是 repeated MMIO store window；controller 不為 NLU 定義額外 hidden stream protocol。

### 3.5 IRQ 模型

本版將 shared external interrupt 與 core-local interrupt 明確分離：

1. cluster、DMA、NLU、loader fault、fabric fault 以 level signal 輸入 `cc_plic`。
2. `cc_plic` 負責 sticky pending、priority、enable、threshold、claim/complete，並對 core 產生 `MEIP`。
3. `MTIP` 與 `MSIP` 由 `cc_core_local_irq` 直接對 core 提供，不經 PLIC。
4. 若 shared external source level 在 complete 後仍為 asserted，pending 必須重新置位。
5. `cc_plic` 的 source 類型是 shared external event；本版不把 local timer/software interrupt 混入 PLIC source map。

### 3.6 Core CSR 與軟體模型

本版 core 採以下正式約束：

1. privilege level 僅實作 machine mode。
2. architected CSR 以 RV32I_Zmmul_Zicsr 標準 machine CSR 為主：`mstatus/misa/mie/mtvec/mscratch/mepc/mcause/mtval/mip/mcycle/minstret`。
3. `mip.MEIP` 由 `cc_plic` 驅動；`mip.MTIP`/`mip.MSIP` 由 `cc_core_local_irq` 驅動。
4. cluster、DMA、NLU、PLIC、loader、host coordination 一律使用 MMIO，不以 custom CSR opcode 暴露。
5. custom CSR range `0x7C0 ~ 0x7FF` 保留給未來 debug/perf 擴充，本版不放 runtime control 定址。

### 3.7 Manifest 格式契約

`cc_section_loader` 不解析 section 名稱字串，也不解析通用 ELF。host 或 compiler handoff 必須提供固定長度 manifest entry 陣列。

#### 3.7.1 Manifest entry 格式

每筆 manifest entry 固定為 32 bytes，共 8 個 32-bit word：

| Word | Field | 說明 |
|---:|---|---|
| 0 | `section_kind[15:0]`, `flags[31:16]` | section 類型與 route/control flag |
| 1 | `dram_addr_lo` | payload DRAM byte address low |
| 2 | `dram_addr_hi` | payload DRAM byte address high |
| 3 | `local_addr` | local destination byte address |
| 4 | `size_bytes` | payload bytes |
| 5 | `crc32` | 可選 CRC32；若 `flags.verify_crc=1` 必須驗證 |
| 6 | `attr0` | implementation-defined；本版保留給未來屬性 |
| 7 | `reserved` | 必須寫 0 |

#### 3.7.2 `section_kind` 編碼

| Kind | Section |
|---:|---|
| `0x0001` | `.hacc.core` |
| `0x0002` | `.hacc.job` |
| `0x0003` | `.hacc.block` |
| `0x0004` | `.hacc.profile` |
| `0x0005` | `.hacc.dma` |
| `0x0006` | `.hacc.agu` |
| `0x0007` | `.hacc.nlu` |
| `0x0008` | `.hacc.pe` |
| `0x0009` | `.hacc.scan` |
| `0x000A` | `.hacc.patch` |
| `0x000B` | `.hacc.debug` |

#### 3.7.3 Manifest 規則

1. `MANIFEST_SIZE` 必須是 32 bytes 的整數倍。
2. loader 依 entry 順序消費 `MANIFEST_SIZE / 32` 筆 entry。
3. `local_addr` 必須落在該 section 類型允許的 local memory window。
4. loader 不得根據 `section_kind` 直接對 cluster、DMA、NLU 產生 side effect。

### 3.8 `load_phase` 與 DRAM owner 狀態

1. `load_phase` 由 top-level boot/load control FSM 產生，而不是由 memory 本身自行推斷。
2. `load_phase=1` 期間，`cc_section_loader` 取得 DRAM owner，`cc_inst_ram` 與 `cc_data_ram` 接受 loader write。
3. `load_phase=0` 期間，`cc_core_mcu` 正式進入 runtime，DRAM master owner 預設切換到 `cc_dma_engine`。

---

## 4. Top-level 架構

### 4.1 方塊圖

```text
host s_ctrl_axi_*            +-----------------------+
---------------------------->| cc_boot_host_if       |
                             | - host CSR bank       |
                             | - manifest registers  |
                             | - trace/snapshot      |
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
                   +---------------------+---------------------+
                   |                                           |
                   v                                           v
           +---------------+                           +----------------+
           | cc_inst_ram   |                           | cc_data_ram    |
           +-------+-------+                           +--------+-------+
                   ^                                            ^
                   |                                            |
                   +---------------------+----------------------+
                                         |
                                         v
                             +-----------------------+
                             | cc_core_mcu           |
                             | - RV32I_Zmmul_Zicsr   |
                             | - IF/ID/EX/MEM/WB     |
                             | - hazard/bypass/irq   |
                             +-----+-----------+-----+
                                   |           |
                                   |           v
                                   |   +-------------------+
                                   |   | cc_core_local_irq |
                                   |   | - MSIP / MTIP     |
                                   |   +-------------------+
                                   v
                             +-----------------------+
                             | cc_cmd_fabric         |
                             | - local ctrl MMIO     |
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
                         +-----------------+   cluster_irq[*]
                         | cc_cluster_data |   nlu_irq[*]
                         | _fabric         |   loader/fabric fault
                         +--------+--------+
                                  |
                                  v
                     cluster AXI4-Lite data / NLU AXI requester
```

### 4.2 分層原則

1. Host/boot layer：`cc_boot_host_if`、manifest 管理、boot/debug/halt control。
2. Load layer：`cc_section_loader` 與 local memory load path。
3. Runtime control layer：`cc_core_mcu`、`cc_cmd_fabric`、`cc_plic`、`cc_core_local_irq`。
4. Data movement layer：`cc_dma_engine`、`cc_cluster_data_fabric`。
5. External target layer：cluster command/data ports 與 NLU MMIO/data requester。

### 4.3 Control-plane 分離原則

1. host control plane 只經由 `cc_boot_host_if` 的 `s_ctrl_axi_*`。
2. firmware runtime control 只經由 `cc_core_mcu` 的 core-visible flat address space。
3. host 不是 runtime orchestrator；host 只做 boot、halt/debug、snapshot 與整體協調。
4. 除了文件明確列出的 shared mirror 之外，不允許 host 與 core 同時對同一 architected register bank 擁有 runtime write ownership。

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
| `HAS_BOOT_ROM` | 0 或 1 | 是否存在 optional `cc_boot_rom` |
| `BOOT_ROM_BYTES` | 4096 或 8192 | boot ROM 容量 |
| `CORE_ISRAM_BYTES` | 8192 或 16384 | MCU instruction SRAM |
| `DATA_SRAM_BYTES` | 65536 或以上 | unified local data SRAM |
| `DATA_DESC_ABI_BYTES` | 49152 或以上 | descriptor/payload ABI region 保留容量 |
| `DATA_STACK_BYTES` | 4096 或 8192 | firmware software stack 保留容量 |
| `DATA_EVENT_ABI_BYTES` | 4096 或以上 | event/debug ABI region 保留容量 |
| `DATA_WORD_W` | 32 | MCU-visible data SRAM word width |
| `CL_AXI_DATA_WIDTH` | 64 | cluster data port width |
| `CL_AHB_DATA_WIDTH` | 32 | cluster command port width |
| `MEM_AXI_DATA_WIDTH` | 128 或 256 | external DRAM AXI4 width |
| `DMA_CMD_FIFO_DEPTH` | 8 或 16 | DMA command FIFO 深度 |
| `CMD_FABRIC_OUTSTANDING` | 1 | 本版 MMIO path 採 blocking 語意 |
| `HAS_LOCAL_TIMER` | 0 或 1 | 是否實作 core-local timer/MSIP block |
| `PLIC_NUM_SOURCES` | `NUM_CLUSTERS + NUM_NLU + 3` | cluster/dma/nlu/loader_fault/fabric_fault |

### 5.1 Core pipeline 正式要求

1. pipeline stage 固定為 IF / ID / EX / MEM / WB。
2. 至少需支援 EX/MEM 與 MEM/WB bypass。
3. load-use hazard 允許 interlock 1 cycle 或更多，但語意必須正確。
4. CSR read-modify-write 需維持 RV32I_Zmmul_Zicsr 單指令原子語意。
5. MMIO load/store 不得被亂序執行；對同一 hart 必須維持 program order。
6. LSU 必須在 core 內或 LSU boundary 明確區分 local memory access 與 MMIO access；不允許把 core-visible MMIO access 錯送到 `cc_data_ram`。

---

## 6. 位址空間與 local memories

### 6.1 Host-visible control space

`cc_boot_host_if` 透過 `s_ctrl_axi_*` 對 host 暴露 host-visible control plane。此空間有以下正式屬性：

1. 它是 host bus address space，不是 core flat address space 的一部分。
2. core 不得透過一般 load/store 存取 `cc_boot_host_if` CSR。
3. host-visible space 主要用於 boot、manifest、halt/resume、trace 與 snapshot。
4. 本版只有明確定義的 shared mirror 會同時對 host 與 core 可見。

### 6.2 `cc_inst_ram`

#### 功能

1. 存放 `.hacc.core`。
2. 提供 MCU instruction fetch。
3. 接受 section loader 寫入。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `clock` | in | 1 | 時脈 |
| `reset_n` | in | 1 | active-low reset |
| `mcu_im_valid` | in | 1 | MCU fetch request |
| `mcu_im_addr` | in | 32 | byte address |
| `mcu_im_rdata` | out | 32 | instruction word |
| `loader_wr_valid` | in | 1 | loader write valid |
| `loader_wr_addr` | in | 32 | byte address |
| `loader_wr_data` | in | 32 或 64 | write data |
| `loader_wr_strb` | in | 4 或 8 | byte strobes |
| `loader_wr_ready` | out | 1 | loader write ready |
| `load_phase` | in | 1 | load phase indicator |

#### 規則

1. MCU fetch 以 32-bit word 為基本粒度，地址需對齊。
2. section loader 可採更寬寫入，但對 MCU 仍需維持正確 32-bit instruction word 語意。
3. `load_phase=1` 期間允許 loader 寫入；run phase 覆寫 active instruction range 屬於非法行為。
4. load delay 預設為 1 cycle。

### 6.3 `cc_data_ram`

#### 功能

1. 作為單一實體 local data SRAM，承載所有非 `.hacc.core` 的 local runtime data。
2. 由軟體 ABI 在其中區分 descriptor/payload region、software stack region 與 event/debug region。
3. 提供 section loader 寫入。
4. 提供 MCU 32-bit load/store 存取。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `clock` | in | 1 | 時脈 |
| `reset_n` | in | 1 | active-low reset |
| `mcu_dm_valid` | in | 1 | MCU access request |
| `mcu_dm_write` | in | 1 | `1=write`, `0=read` |
| `mcu_dm_addr` | in | 32 | byte address |
| `mcu_dm_wdata` | in | 32 | 32-bit store data |
| `mcu_dm_wstrb` | in | 4 | byte strobes |
| `mcu_dm_rdata` | out | 32 | 32-bit read data |
| `loader_wr_valid` | in | 1 | loader write valid |
| `loader_wr_addr` | in | 32 | byte address |
| `loader_wr_data` | in | 32 或 64 | write data |
| `loader_wr_strb` | in | 4 或 8 | byte strobes |
| `loader_wr_ready` | out | 1 | loader write ready |
| `load_phase` | in | 1 | load phase indicator |

#### 規則

1. MCU-visible 基本讀取粒度固定為 32-bit。
2. `.hacc.pe/.hacc.scan` 必須以 local payload 形式存在 `cc_data_ram`，不得要求 loader 直接對 cluster side-effect。
3. 若實作採用更寬 memory line，對 MCU 仍需維持正確 32-bit 語意。
4. `load_phase=1` 期間允許 loader 寫入；run phase 以 MCU 為主。
5. load delay 預設為 1 cycle。

### 6.4 Data-RAM 軟體 ABI 分區

`cc_data_ram` 不要求硬體切成多個實體 SRAM，但軟體 ABI 必須至少保留以下邊界：

| ABI region | 建議容量 | 主要內容 | 主要消費者 |
|---|---:|---|---|
| descriptor/payload region | `DATA_DESC_ABI_BYTES` | `.hacc.job/.hacc.block/.hacc.profile/.hacc.dma/.hacc.agu/.hacc.nlu/.hacc.pe/.hacc.scan/.hacc.patch` | MCU load / MMIO write sequence |
| software stack region | `DATA_STACK_BYTES` | firmware stack、callee-save spill、trap frame | MCU |
| event/debug region | `DATA_EVENT_ABI_BYTES` | completion bitmap、trace、debug scratch、`.hacc.debug` | MCU / host debug |

正式要求：software stack region 是 mandatory，不再視為 optional scratch。

### 6.5 Core-visible address space 規劃

下表為 core 的 32-bit load/store address map。正式原則是：core 以單一 flat address space 執行；local memory 與所有 runtime control 都透過標準 load/store 完成。

| Base | End | Size | Region | 說明 |
|---|---|---:|---|---|
| `0x0000_0000` | `0x0000_3FFF` | 16 KB | Inst-RAM | `.hacc.core`，instruction fetch only |
| `0x0001_0000` | `0x0001_0FFF` | 4 KB | Boot-ROM | optional，僅 `HAS_BOOT_ROM=1` 時存在 |
| `0x1000_0000` | `0x1000_FFFF` | 64 KB | Data-RAM | descriptor/payload/stack/event/debug ABI |
| `0x2000_0000` | `0x2000_0FFF` | 4 KB | Local control MMIO | cluster mask、fabric fault、boot reason |
| `0x2000_1000` | `0x2000_17FF` | 2 KB | DMA MMIO | DMA register bank |
| `0x2000_1800` | `0x2000_1FFF` | 2 KB | Reserved | 保留給未來 DMA debug/queue extension；本版不得使用 |
| `0x2000_2000` | `0x2000_20FF` | 256 B | Core local timer/MSIP | optional，供 `MTIP/MSIP` |
| `0x0C00_0000` | `0x0C00_FFFF` | 64 KB | PLIC MMIO | claim/complete、enable、priority、pending |
| `0x4000_0000` | `0x400F_FFFF` | 1 MB | Cluster unicast MMIO | 每 cluster stride `0x0001_0000` |
| `0x5000_0000` | `0x5000_FFFF` | 64 KB | Cluster masked-broadcast MMIO | 依 `CLUSTER_MASK_LO/HI` fan-out |
| `0x6000_0000` | `0x6000_FFFF` | 64 KB | NLU MMIO | 每 NLU stride `0x0000_1000` |

### 6.6 Address ownership 與 mirror 規則

1. host-only：`cc_boot_host_if` CSR bank 由 host 擁有，不對 core 暴露。
2. core-only：DMA MMIO、PLIC MMIO、cluster/NLU MMIO、core local timer/MMIO 由 core 擁有。
3. shared mirror：本版只允許 `CLUSTER_MASK_LO/HI` 同時對 host 與 core 可見，且實體上必須只有一份 storage。
4. snapshot：`CORE_PC_SNAPSHOT`、`CORE_CAUSE_SNAPSHOT`、`LAST_MMIO_TARGET`、`LAST_MMIO_ADDR` 是 host 只讀 snapshot，不代表 host 擁有這些 runtime state 的寫入權。
5. host 對 shared mirror 的寫入只允許在 `core_running=0` 或 `core_halted=1` 時進行；若在 core 正在執行期間寫入，host slave 必須回 error。

### 6.7 Cluster unicast window

正式公式：

`cluster_n_base = 0x4000_0000 + n * 0x0001_0000`

cluster 內部 offset 維持現有契約：

1. `+0x0000 ~ +0x00FF`：SPM config / PMU。
2. `+0x1000 ~ +0x1FFF`：HDDU MMIO。
3. `+0x2000 ~ +0x20FF`：NoC command sideband。

### 6.8 Cluster masked-broadcast window

正式公式：

`cluster_bcast_addr = 0x5000_0000 + cluster_local_offset`

規則：

1. firmware 先寫 `CLUSTER_MASK_LO/HI`，再對 broadcast window 做 store。
2. fabric 以遞增 cluster id 順序複製同一筆 transaction 到所有命中的 cluster。
3. 只有全部命中 target 都完成後，該 store 才算完成。
4. broadcast window 預設只允許 write；read 僅在 mask popcount=1 時合法，否則回 fault。
5. 此機制只負責 transaction fan-out，不承擔任何 profile merge、descriptor parse 或 wave schedule。

### 6.9 NLU MMIO window

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
| `s_ctrl_axi_*` | in/out | platform-defined | host 存取 host-visible controller CSR / manifest queue；此空間不屬於 core address map |

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
| `controller_irq` | out | 1 | controller 對 host 的總中斷 |

---

## 8. 模組切分與詳細規格

### 8.1 `cc_boot_host_if`

#### 功能

1. 提供 host-visible control plane。
2. 提供 manifest base/size/kick 與 loader control/status。
3. 提供 selected core state snapshot；不是直接對 host 暴露 RV CSR opcode 空間。
4. 提供 host-visible interrupt/status summary。
5. 提供 host 對 core boot/halt/resume 與 shared `CLUSTER_MASK` mirror 的可觀測控制。

#### 主要 I/O

| Port | Dir | 說明 |
|---|---|---|
| `s_ctrl_axi_*` | in/out | host control bus |
| `host_irq_status` | in | 來自 local status/irq summary |
| `controller_irq` | out | 對 host 中斷 |


#### 規則

1. active load 期間不得覆寫正在消費的 manifest entry。
2. completion 與 error 狀態採 write-1-to-clear，不可 read-to-clear。
3. host-visible CSR 是 host control plane；不直接取代 core 自己的 RV32 CSR。
4. host 不得透過此介面直接操控 DMA、PLIC、cluster、NLU 的 runtime register bank。
5. 對 shared mirror 的 host write 只允許在 core 未執行或已 halt 的狀態。

#### Host-visible CSR bank 規劃

下表定義 `s_ctrl_axi_*` 至少要提供的 host-visible CSR offset。

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x0000` | `HACC_CAP0` | R | capability bitmap：RV32I_Zmmul_Zicsr、DMA、PLIC、broadcast support |
| `0x0004` | `HACC_CAP1` | R | `NUM_CLUSTERS`、`NUM_NLU`、memory size encode |
| `0x0008` | `HACC_CTRL` | R/W | bit0=`core_en`, bit1=`core_haltreq`, bit2=`core_resume`(W1P), bit3=`sw_reset`(W1P), bit4=`loader_abort`(W1P), bit5=`auto_boot_after_load` |
| `0x000C` | `HACC_STATUS` | R | bit0=`loader_busy`, bit1=`loader_done`, bit2=`core_halted`, bit3=`core_running`, bit4=`faulted`, bit5=`load_phase`, bit6=`mem_axi_owner` |
| `0x0010` | `CORE_BOOT_ADDR` | R/W | reset/boot PC |
| `0x0014` | `CORE_TRAP_VECTOR` | R/W | firmware 預設 `mtvec` reset value；不是 runtime 強制 mirror |
| `0x0018` | `CORE_PC_SNAPSHOT` | R | 最近一次可觀測 PC |
| `0x001C` | `CORE_CAUSE_SNAPSHOT` | R | 最近一次 trap/abort cause |
| `0x0020` | `MANIFEST_ADDR_LO` | R/W | manifest base low |
| `0x0024` | `MANIFEST_ADDR_HI` | R/W | manifest base high |
| `0x0028` | `MANIFEST_SIZE` | R/W | manifest bytes |
| `0x002C` | `MANIFEST_KICK` | W1P | push loader start pulse |
| `0x0030` | `LOADER_STATUS` | R | loader 狀態 |
| `0x0034` | `LOADER_ERR_CODE` | R/W1C | loader error code |
| `0x0038` | `LOADER_ERR_INFO` | R | loader 補充錯誤資訊 |
| `0x0040` | `IRQ_SUMMARY` | R | 對 host 可見的 summary pending |
| `0x0044` | `IRQ_FORCE_ACK` | W1C | host debug 用 force ack |
| `0x0050` | `CLUSTER_MASK_LO` | R/W | 與 core local MMIO mirror；host write 只允許在 core 未執行或已 halt 時 |
| `0x0054` | `CLUSTER_MASK_HI` | R/W | 與 core local MMIO mirror；host write 只允許在 core 未執行或已 halt 時 |
| `0x0058` | `LAST_MMIO_TARGET` | R | 最近一次 MMIO 錯誤 target id |
| `0x005C` | `LAST_MMIO_ADDR` | R | 最近一次 MMIO fault address |
| `0x0060` | `TRACE_BASE` | R/W | trace buffer base |
| `0x0064` | `TRACE_SIZE` | R/W | trace buffer size |
| `0x0068` | `TRACE_CTRL` | R/W | trace enable/flush |
| `0x006C` | `TRACE_STATUS` | R | trace running / watermark / overflow 狀態 |

`IRQ_SUMMARY` 的最低需求 bit 分配如下：

1. bit0=`loader_done`
2. bit1=`loader_error`
3. bit2=`core_fault`
4. bit3=`core_halted`
5. bit4=`plic_pending`
6. bit5=`trace_watermark`

`LOADER_STATUS` 的最低需求欄位如下：

1. bit0=`busy`
2. bit1=`done`
3. bit2=`err`
4. bits[7:4]=current FSM state code（對應 `LD_IDLE/LD_FETCH/LD_ROUTE/LD_WRITE_LOCAL/LD_VERIFY/LD_DONE/LD_ERR`）
5. bits[31:16]=目前正在處理的 manifest entry index

`IRQ_FORCE_ACK` 是 host debug escape hatch，只允許清除 `IRQ_SUMMARY` 中由 host summary aggregator 生成的 sticky bit；它不取代 PLIC 的 `claim/complete` 正式語意。

### 8.2 `cc_boot_rom`（可選）

本版允許 `cc_boot_rom` 為 optional。若實作存在，其責任僅限於：

1. reset vector
2. boot mode 決策
3. 跳轉到 I-SRAM entry 或 host-assisted boot stub

它不是 runtime scheduler，也不承擔 descriptor 解析邏輯。

`cc_boot_rom` 若存在，建議映射到本文件 6.5 節所定義的 optional Boot-ROM window，並允許 `CORE_BOOT_ADDR` 指向該窗口。

### 8.3 `cc_section_loader`

#### 功能

1. 消費 manifest。
2. 從 DRAM 讀 section payload。
3. 將 payload 寫入 `cc_inst_ram` 或 `cc_data_ram` 的對應 ABI region。
4. 做基礎 bounds/checksum 驗證。
5. 在 load 完成或失敗時更新 `LOADER_STATUS` / `LOADER_ERR_*`。

#### 明確不做的事

1. 不對 cluster 發送 NoC program stream。
2. 不對 cluster 發送 profile apply MMIO。
3. 不對 DMA engine 發送 runtime command。
4. 不對 NLU 發送 config stream。
5. 不從 section payload 中隱含解譯任何 runtime action。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `manifest_pop_valid` | in | 1 | manifest entry valid |
| `manifest_pop_data` | in | manifest width | section metadata |
| `manifest_pop_ready` | out | 1 | 可接收 entry |
| `m_mem_axi_*` | out/in | AXI4 | DRAM read master |
| `inst_loader_*` | out | implementation-defined | 寫 `cc_inst_ram` |
| `data_loader_*` | out | implementation-defined | 寫 `cc_data_ram` |

#### Section route 正式定義

| Section | 載入位置 | runtime 消費者 |
|---|---|---|
| `.hacc.core` | INST-RAM | MCU fetch |
| `.hacc.job` | Data-RAM descriptor/payload ABI region | MCU load |
| `.hacc.block` | Data-RAM descriptor/payload ABI region | MCU load |
| `.hacc.profile` | Data-RAM descriptor/payload ABI region | MCU load + MMIO store sequence |
| `.hacc.dma` | Data-RAM descriptor/payload ABI region | MCU load + DMA MMIO staging/submit |
| `.hacc.agu` | Data-RAM descriptor/payload ABI region | MCU load + MMIO store sequence |
| `.hacc.nlu` | Data-RAM descriptor/payload ABI region | MCU load + MMIO store sequence |
| `.hacc.pe` | Data-RAM descriptor/payload ABI region | MCU load + repeated cluster NoC MMIO store |
| `.hacc.scan` | Data-RAM descriptor/payload ABI region | MCU load + repeated cluster NoC MMIO store |
| `.hacc.patch` | Data-RAM descriptor/payload ABI region | MCU load |
| `.hacc.debug` | Data-RAM event/debug ABI region | debug/runtime optional |

#### 狀態機

1. `LD_IDLE`
2. `LD_FETCH`
3. `LD_ROUTE`
4. `LD_WRITE_LOCAL`
5. `LD_VERIFY`
6. `LD_DONE`
7. `LD_ERR`

#### Loader 錯誤碼最小要求

| Code | Name | 說明 |
|---:|---|---|
| `0` | `LD_ERR_NONE` | 無錯誤 |
| `1` | `LD_ERR_MANIFEST_ALIGN` | `MANIFEST_SIZE` 或 manifest base 對齊錯誤 |
| `2` | `LD_ERR_BAD_SECTION_KIND` | 未知 `section_kind` |
| `3` | `LD_ERR_LOCAL_ADDR_OOB` | local address 超出允許範圍 |
| `4` | `LD_ERR_SIZE_OOB` | size 超出 local region |
| `5` | `LD_ERR_CRC_MISMATCH` | CRC 驗證失敗 |
| `6` | `LD_ERR_AXI` | DRAM AXI 回應錯誤 |
| `7` | `LD_ERR_OVERLAP` | manifest entry 對 active local range 發生不合法重疊 |

### 8.4 `cc_core_mcu`

#### 功能

1. 執行 RV32I_Zmmul_Zicsr machine-mode firmware。
2. 讀取 local descriptor 與 payload tables。
3. 以標準 load/store 對 local memory 與 MMIO fabric 進行存取。
4. 接收 `cc_plic` 的 machine external interrupt 與 `cc_core_local_irq` 的 machine timer/software interrupt。

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
| `ls_resp_rdata` | in | 32 | load data |
| `mmio_req_valid` | out | 1 | MMIO request valid |
| `mmio_req_write` | out | 1 | MMIO write strobe |
| `mmio_req_addr` | out | 32 | MMIO address |
| `mmio_req_wdata` | out | 32 | MMIO write data |
| `mmio_req_wstrb` | out | 4 | MMIO byte enable |
| `mmio_resp_valid` | in | 1 | MMIO complete |
| `mmio_resp_rdata` | in | 32 | MMIO read data |
| `irq_meip` | in | 1 | machine external interrupt pending |
| `irq_msip` | in | 1 | machine software interrupt pending |
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
5. core LSU 必須明確分流 local memory 與 MMIO access；對 `0x2000_0000` 以上與 PLIC/cluster/NLU windows 的 access 不得送到 `cc_data_ram`。

### 8.5 `cc_cmd_fabric`

#### 功能

1. 解碼 core LSU 的 MMIO request。
2. 將 core local control MMIO、DMA MMIO、cluster AHB、NLU AHB、PLIC MMIO 分流。
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
5. local control register bank（含 `CLUSTER_MASK_LO/HI`、`LAST_FAULT_ADDR`）

#### Core local control MMIO 規劃

下表定義 core 在 `0x2000_0000 ~ 0x2000_0FFF` 可見的 local control MMIO。這些是 firmware 使用的 controller-local register，不是 RV32 CSR opcode 空間。

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x000` | `CLUSTER_MASK_LO` | R/W | cluster broadcast mask low；與 host-visible CSR mirror 同一份 storage |
| `0x004` | `CLUSTER_MASK_HI` | R/W | cluster broadcast mask high；與 host-visible CSR mirror 同一份 storage |
| `0x008` | `MMIO_ERR_STATUS` | R/W1C | bit0=`pending`, bit1=`decode_fault`, bit2=`broadcast_read_fault`, bit3=`target_bus_fault`, bit4=`timeout` |
| `0x00C` | `LAST_TARGET_ID` | R | 最近一次 fault target |
| `0x010` | `LAST_FAULT_ADDR` | R | 最近一次 fault address |
| `0x014` | `LAST_FAULT_INFO` | R | bus response / target-local error code |
| `0x018` | `BOOT_REASON` | R | boot/trap resume source；由 host/boot ROM 提供 |
| `0x01C` | `FABRIC_CAP0` | R | fabric capability bitmap：broadcast、timeout detect、mirror support |

#### Broadcast 規則

1. `mask=0` 時不對外發 transaction，並立即返回完成。
2. `mask!=0` 時，fabric 必須對每個命中 target 執行相同 transfer。
3. 只有全部 target 都完成後才回應該次 MMIO store/load 完成。
4. 任一 target 出錯，fabric 必須帶回錯誤 target id 與 local address。
5. 針對 broadcast read，若 `popcount(mask)!=1`，fabric 必須回 fault。

### 8.6 `cc_core_local_irq`

#### 功能

1. 提供 machine software interrupt（`MSIP`）。
2. 提供 machine timer interrupt（`MTIP`）。
3. 將 `mtime` / `mtimecmp` / `msip` 對映到 core-visible local timer/MMIO window。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `core_timer_mmio_*` | in/out | implementation-defined | local timer register access |
| `irq_msip` | out | 1 | 對 core 產生 machine software interrupt |
| `irq_mtip` | out | 1 | 對 core 產生 machine timer interrupt |

#### Core local timer/MSIP MMIO 規劃

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x000` | `MSIP` | R/W | bit0=`msip`; 1 表示 pending，寫 0 清除 |
| `0x004` | `MTIMECMP_LO` | R/W | timer compare low |
| `0x008` | `MTIMECMP_HI` | R/W | timer compare high |
| `0x00C` | `MTIME_LO` | R/W | free-running counter low；debug/testing 允許寫入 |
| `0x010` | `MTIME_HI` | R/W | free-running counter high |
| `0x014` | `TIMER_CTRL` | R/W | bit0=`timer_en` |

#### 規則

1. `MTIP` 在 `timer_en=1` 且 `mtime >= mtimecmp` 時 assert。
2. `MSIP` 不經 PLIC，直接反映到 `mip.MSIP`。
3. 若 `HAS_LOCAL_TIMER=0`，則對此 window 的存取必須回 fault。

### 8.7 `cc_plic`

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

#### Source id 配置

| Source ID | 說明 |
|---:|---|
| `0` | reserved，claim 時表示無 pending source |
| `1 .. NUM_CLUSTERS` | `cluster_irq[0 .. NUM_CLUSTERS-1]` |
| `NUM_CLUSTERS + 1` | `dma_irq` |
| `NUM_CLUSTERS + 2 .. NUM_CLUSTERS + NUM_NLU + 1` | `nlu_irq[0 .. NUM_NLU-1]` |
| `NUM_CLUSTERS + NUM_NLU + 2` | `loader_fault` |
| `NUM_CLUSTERS + NUM_NLU + 3` | `fabric_fault` |

#### PLIC MMIO 規劃

本版採 compact single-hart map，仍維持 RISC-V PLIC claim/complete 語意。

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x0000 + 4*s` | `PRIORITY[s]` | R/W | source `s` priority，`s=1..PLIC_NUM_SOURCES` |
| `0x0800` | `PENDING_LO` | R | pending bitmap low |
| `0x0804` | `PENDING_HI` | R | pending bitmap high |
| `0x1000` | `ENABLE_LO` | R/W | enable bitmap low |
| `0x1004` | `ENABLE_HI` | R/W | enable bitmap high |
| `0x1800` | `THRESHOLD` | R/W | interrupt threshold |
| `0x1804` | `CLAIM_COMPLETE` | R/W | read=claim、write=complete |
| `0x1808` | `MAX_SOURCE_ID` | R | 最大合法 source id |

#### 規則

1. claim 只回傳目前選到的 source id，不代表來源已解除。
2. complete 才是 firmware 對該 source 的顯式 ack。
3. source level 未解除時，pending 必須重新置位。
4. 本版可採簡化 single-hart PLIC，但 MMIO 語意必須維持 claim/complete 相容。
5. `priority=0` 的 source 視為不會產生中斷；同優先權時，以較小 source id 勝出。
6. 寫 `CLAIM_COMPLETE` 時若 id=0 或超出合法範圍，必須回 fault。

### 8.8 `cc_dma_engine`

#### 功能

1. 接受 MCU 寫入的 MMIO 設定。
2. 將 staging register 在 `submit` 時原子地快照進 internal command FIFO。
3. 透過 DRAM AXI 與 cluster data fabric 搬運 tensor data。
4. 回報 done/error status 與 irq。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `dma_mmio_req_valid` | in | 1 | MMIO request |
| `dma_mmio_req` | in | `mmio_req_t` | reg access |
| `dma_mmio_resp_valid` | out | 1 | MMIO complete |
| `m_mem_axi_*` | out/in | AXI4 | DRAM requester |
| `cluster_data_req` | out | `cluster_data_req_t` | cluster SPM request |
| `cluster_data_req_valid` | out | 1 | request valid |
| `cluster_data_req_ready` | in | 1 | fabric ready |
| `cluster_data_resp_valid` | in | 1 | response valid |
| `cluster_data_resp` | in | implementation-defined | data/error |
| `dma_irq` | out | 1 | done/error irq |

#### DMA MMIO 規劃

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x000` | `DMA_CAP0` | R | capability bitmap：linear copy、2D copy、DRAM endpoint、cluster SPM endpoint |
| `0x004` | `DMA_STATUS` | R | bit0=`idle`, bit1=`busy`, bit2=`cmd_fifo_full`, bit3=`done_pending`, bit4=`err_pending`, bit5=`fatal` |
| `0x008` | `DMA_CTRL` | R/W | bit0=`submit`(W1P), bit1=`abort`(W1P), bit2=`soft_reset`(W1P), bit3=`irq_en` |
| `0x00C` | `DMA_OP_KIND` | R/W | `0=linear_copy`, `1=strided_2d_copy` |
| `0x010` | `DMA_SRC_KIND` | R/W | `0=DRAM`, `1=CLUSTER_SPM` |
| `0x014` | `DMA_DST_KIND` | R/W | `0=DRAM`, `1=CLUSTER_SPM` |
| `0x018` | `DMA_SRC_ADDR_LO` | R/W | source byte address low |
| `0x01C` | `DMA_SRC_ADDR_HI` | R/W | source byte address high |
| `0x020` | `DMA_DST_ADDR_LO` | R/W | destination byte address low |
| `0x024` | `DMA_DST_ADDR_HI` | R/W | destination byte address high |
| `0x028` | `DMA_SRC_CLUSTER_ID` | R/W | 當 `SRC_KIND=CLUSTER_SPM` 時有效 |
| `0x02C` | `DMA_DST_CLUSTER_ID` | R/W | 當 `DST_KIND=CLUSTER_SPM` 時有效 |
| `0x030` | `DMA_BYTES` | R/W | linear copy bytes |
| `0x034` | `DMA_LINE_BYTES` | R/W | 2D copy 每列 bytes |
| `0x038` | `DMA_LINE_COUNT` | R/W | 2D copy 列數 |
| `0x03C` | `DMA_SRC_STRIDE` | R/W | source line stride bytes |
| `0x040` | `DMA_DST_STRIDE` | R/W | destination line stride bytes |
| `0x044` | `DMA_CMD_TAG` | R/W | firmware 指定 command tag |
| `0x048` | `DMA_DONE_TAG` | R | 最近完成 command tag |
| `0x04C` | `DMA_ERR_CODE` | R/W1C | DMA 錯誤碼 |
| `0x050` | `DMA_ERR_INFO` | R | 補充錯誤資訊 |
| `0x054` | `DMA_DEBUG_STATE` | R | internal FSM / outstanding state snapshot |

#### DMA 錯誤碼最小要求

| Code | Name | 說明 |
|---:|---|---|
| `0` | `DMA_ERR_NONE` | 無錯誤 |
| `1` | `DMA_ERR_SUBMIT_WHEN_FULL` | submit 時 command FIFO 已滿 |
| `2` | `DMA_ERR_BAD_OP_KIND` | 不支援的 `DMA_OP_KIND` |
| `3` | `DMA_ERR_BAD_ENDPOINT_KIND` | 不支援的 source/destination kind |
| `4` | `DMA_ERR_ADDR_ALIGN` | 地址或長度對齊錯誤 |
| `5` | `DMA_ERR_ZERO_LENGTH` | 非法零長度搬移 |
| `6` | `DMA_ERR_CLUSTER_RESP` | cluster data path 回 error |
| `7` | `DMA_ERR_DRAM_AXI` | DRAM AXI 回應錯誤 |
| `8` | `DMA_ERR_ABORTED` | 命令被 abort |

#### 正式語意

1. DMA engine 不得自行從 `cc_data_ram` 的 descriptor/payload ABI region 讀 `.hacc.dma`。
2. DMA engine 只對已由 MCU 經由 MMIO staging register + `submit` 明確下發的 command 負責。
3. firmware 必須先完整寫入 command staging register，再寫 `DMA_CTRL.submit=1`。
4. `submit` 成功時，DMA engine 必須把該組 staging register 原子快照進 internal command FIFO。
5. 若 command FIFO 已滿，對 `submit` 的 MMIO store 必須回 fault，且不得部分接受該命令。
6. `DMA_OP_KIND=linear_copy` 時，`DMA_BYTES` 有效，2D fields 忽略；`DMA_OP_KIND=strided_2d_copy` 時，總搬移量為 `DMA_LINE_BYTES * DMA_LINE_COUNT`。
7. 當 endpoint kind 是 `CLUSTER_SPM` 時，`ADDR_LO` 視為 cluster local byte address，對應的 `*_CLUSTER_ID` 必須有效。
8. cluster 端因為是 AXI4-Lite slave，DMA 對 cluster SPM 的存取不得假設 burst。
9. 本版不把 `dma_stream_*` 視為 architected 介面；若 implementation 內部仍用 write queue 或 wider push bus，必須完全隱藏在 DMA MMIO/doorbell 之後。
10. `DMA_DONE_TAG` 必須回報最近一筆進入 terminal state（done 或 error）的 command tag，方便 firmware 對應 completion source。

### 8.9 `cc_cluster_data_fabric`

#### 功能

1. 協調 DMA engine 與 NLU AXI requester 對 cluster SPM 的存取。
2. 依 cluster id 將 request 路由到對應 cluster AXI4-Lite data master。
3. 可選支援 round-robin 或固定優先權仲裁。

#### 主要 I/O

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `clock` | in | 1 | 時脈 |
| `reset_n` | in | 1 | active-low reset |
| **DMA requester interface** | | | |
| `dma_req_valid` | in | 1 | DMA data request valid |
| `dma_req_write` | in | 1 | `1=write`, `0=read` |
| `dma_req_cluster_id` | in | `ceil(log2(NUM_CLUSTERS))` | 目標 cluster 編號 |
| `dma_req_addr` | in | 32 | cluster-local byte address |
| `dma_req_wdata` | in | `CL_AXI_DATA_WIDTH` | write data |
| `dma_req_wstrb` | in | `CL_AXI_DATA_WIDTH/8` | byte strobes |
| `dma_req_ready` | out | 1 | fabric 可接受 DMA request |
| `dma_resp_valid` | out | 1 | DMA response valid |
| `dma_resp_rdata` | out | `CL_AXI_DATA_WIDTH` | read data |
| `dma_resp_error` | out | 1 | response error flag |
| **NLU requester interface** | | | |
| `nlu_req_valid` | in | 1 | NLU data request valid |
| `nlu_req_write` | in | 1 | `1=write`, `0=read` |
| `nlu_req_cluster_id` | in | `ceil(log2(NUM_CLUSTERS))` | 目標 cluster 編號 |
| `nlu_req_addr` | in | 32 | cluster-local byte address |
| `nlu_req_wdata` | in | `CL_AXI_DATA_WIDTH` | write data |
| `nlu_req_wstrb` | in | `CL_AXI_DATA_WIDTH/8` | byte strobes |
| `nlu_req_ready` | out | 1 | fabric 可接受 NLU request |
| `nlu_resp_valid` | out | 1 | NLU response valid |
| `nlu_resp_rdata` | out | `CL_AXI_DATA_WIDTH` | read data |
| `nlu_resp_error` | out | 1 | response error flag |
| **Per-cluster AXI4-Lite data master (×NUM_CLUSTERS)** | | | |
| `m_cl_data_aw_valid[n]` | out | 1 | AXI4-Lite write address valid |
| `m_cl_data_aw_ready[n]` | in | 1 | AXI4-Lite write address ready |
| `m_cl_data_aw_addr[n]` | out | 32 | write address |
| `m_cl_data_w_valid[n]` | out | 1 | write data valid |
| `m_cl_data_w_ready[n]` | in | 1 | write data ready |
| `m_cl_data_w_data[n]` | out | `CL_AXI_DATA_WIDTH` | write data |
| `m_cl_data_w_strb[n]` | out | `CL_AXI_DATA_WIDTH/8` | write strobes |
| `m_cl_data_b_valid[n]` | in | 1 | write response valid |
| `m_cl_data_b_ready[n]` | out | 1 | write response ready |
| `m_cl_data_b_resp[n]` | in | 2 | write response code |
| `m_cl_data_ar_valid[n]` | out | 1 | read address valid |
| `m_cl_data_ar_ready[n]` | in | 1 | read address ready |
| `m_cl_data_ar_addr[n]` | out | 32 | read address |
| `m_cl_data_r_valid[n]` | in | 1 | read data valid |
| `m_cl_data_r_ready[n]` | out | 1 | read data ready |
| `m_cl_data_r_data[n]` | in | `CL_AXI_DATA_WIDTH` | read data |
| `m_cl_data_r_resp[n]` | in | 2 | read response code |

#### 內部邏輯

1. **Arbiter**：round-robin 仲裁，一次只允許一個 requester（DMA 或 NLU）佔用一個 cluster port。初始 pointer reset 為 0（DMA 優先）。
2. **Cluster router**：依 `cluster_id` 選擇目標 cluster AXI4-Lite master port。
3. **Transaction tracker**：追蹤 outstanding transaction，確保 response 回到正確 requester。
4. **Error encoder**：將 AXI4-Lite response code 非 OKAY 轉換為 `resp_error=1`。

#### 仲裁與路由 FSM

```text
IDLE ──(dma_req_valid || nlu_req_valid)──> GRANT
GRANT ──(selected requester)──> ADDR_PHASE
ADDR_PHASE ──(aw_ready/ar_ready)──> DATA_PHASE (write) / WAIT_RESP (read)
DATA_PHASE ──(w_ready)──> WAIT_RESP
WAIT_RESP ──(b_valid/r_valid)──> COMPLETE
COMPLETE ──> IDLE
```

#### 規則

1. DMA 與 NLU request 不得直接同時驅動外部 cluster data port。
2. 仲裁策略需 deterministic，並在 PMU 或 trace 可觀測。
3. 對同一 cluster port 的仲裁結果必須可重現；若採 round-robin，初始 pointer reset 值必須固定。
4. cluster_id 超出 `NUM_CLUSTERS` 範圍時，fabric 必須立刻回 error response，不得對外發 transaction。
5. 一個 requester 的 transaction 完成前，arbiter 不得切換到另一 requester（non-preemptive）。
6. DMA 與 NLU 同時對不同 cluster 發 request 的 interleave 本版不支援；fabric 一次只服務一筆 transaction。

---

## 9. Section 與 local memory 規則

### 9.1 正式 section route

| Section | 載入位置 | runtime path |
|---|---|---|
| `.hacc.core` | I-SRAM | MCU fetch |
| `.hacc.job` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.block` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.profile` | Data-SRAM descriptor/payload ABI region | MCU load + cluster HDDU MMIO store sequence |
| `.hacc.dma` | Data-SRAM descriptor/payload ABI region | MCU load + DMA MMIO staging/submit |
| `.hacc.agu` | Data-SRAM descriptor/payload ABI region | MCU load + cluster HDDU MMIO store sequence |
| `.hacc.nlu` | Data-SRAM descriptor/payload ABI region | MCU load + NLU MMIO store sequence |
| `.hacc.pe` | Data-SRAM descriptor/payload ABI region | MCU load + repeated cluster NoC MMIO store |
| `.hacc.scan` | Data-SRAM descriptor/payload ABI region | MCU load + repeated cluster NoC MMIO store |
| `.hacc.patch` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.debug` | Data-SRAM event/debug ABI region | debug/runtime optional |

### 9.2 重要限制

1. loader 不得直接對 cluster 發 `.hacc.pe/.hacc.scan` side effect。
2. loader 不得直接啟動 DMA。
3. `.hacc.dma` 不是 DMA engine 自動 fetch 的 descriptor heap，而是 MCU runtime 解譯後寫入 DMA staging register 的 payload/rule source。
4. 本版沒有 architected `dma_stream_*`，因此 `.hacc.dma` 不得被描述成 DMA stream payload。

---

## 10. Runtime 流程

### 10.1 Reset / Boot

1. `rst_n` deassert 後，host 或 boot ROM 決定 boot mode。
2. `cc_boot_host_if` 接收 manifest。
3. `cc_section_loader` 將 `.hacc.core` 載入 `cc_inst_ram`，將其餘 sections 載入 `cc_data_ram` 的對應 ABI region。
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
4. MCU 將 `.hacc.dma` 解譯為 DMA staging register 值，寫入 DMA MMIO，最後以 `DMA_CTRL.submit` 提交命令。
5. MCU 若遇到 NLU phase，則對 NLU MMIO window 下發 config/start。
6. MCU 以 polling 或 IRQ 模式等待完成。

### 10.4 Error / Abort

1. DMA/cluster/NLU 任一單元出錯，`cc_plic` 置位 pending。
2. MCU 進入 handler 或 polling path 收斂錯誤。
3. MCU 必須負責發 abort/cleanup sequence。
4. controller 對 host 的 `controller_irq` 可做 summary interrupt。

---

## 11. Core local MMIO 與 `cc_boot_host_if` 的關係

### 11.1 為什麼兩套空間都需要存在

這兩套空間的存在目的不同，不能混為同一件事：

1. `cc_boot_host_if` 是 host control plane，讓 host 在 core 尚未執行前完成 boot、manifest、halt/resume、trace 與 snapshot。
2. core local MMIO 是 firmware runtime control plane，讓 `cc_core_mcu` 在不污染標準 CSR opcode 空間的前提下，以普通 load/store 存取 controller-local 狀態。
3. 兩者若完全合併，會導致 host 與 core 對同一組 runtime register 競爭所有權，破壞「MCU 是唯一 runtime control master」的原則。

### 11.2 正式關係與所有權表

| 類型 | Host 視圖 | Core 視圖 | 實體 storage | 存在意義 |
|---|---|---|---|---|
| host-only | `HACC_CTRL`、manifest、trace | 無 | `cc_boot_host_if` | boot/debug/control plane |
| core-only | 無 | DMA/PLIC/cluster/NLU/local timer/local control | 各 runtime 模組 | firmware runtime control |
| shared mirror | `CLUSTER_MASK_LO/HI` | `CLUSTER_MASK_LO/HI` | `cc_cmd_fabric` 單一份 storage | host 可在 boot/halt 狀態預先設定 broadcast mask |
| host snapshot | `CORE_PC_SNAPSHOT`、`LAST_MMIO_ADDR` 等 | 原始狀態位於 core 或 fabric | snapshot flop / status mux | 方便 host debug，不改變 runtime ownership |

### 11.3 本版刻意移除的重複 register

為了避免語意重疊，本版明確不再提供下列 mirror：

1. 不提供 DMA runtime mirror 到 core local MMIO；firmware 直接讀 DMA MMIO bank。
2. 不提供 `mcycle/minstret` 的 MMIO mirror；firmware 直接用標準 CSR。
3. 不提供 trace control 的 core/local mirror；trace control 屬於 host debug plane。
4. 不提供 `CORE_STATUS/CORE_CTRL` 的 core local MMIO 版本；host 已有 `HACC_STATUS/HACC_CTRL`，firmware 則應依 CSR 與一般程式流控制自己。

### 11.4 Shared mirror 的寫入規則

1. `CLUSTER_MASK_LO/HI` 實體上只能有一份 storage，位於 `cc_cmd_fabric`。
2. core 可於 runtime 任意寫入該 mask。
3. host 只可在 core 尚未執行或已 halt 時寫入該 mask。
4. host 與 core 不得同時擁有 runtime write ownership；若 host 在 core 正執行期間寫 shared mirror，host slave 必須回 error。

---

## 12. RTL 落地規則

### 12.1 模組介面風格

1. 所有一級模組需明確列出 RTL I/O，不以 C++ method 或隱藏 side effect 表示介面。
2. 所有 blocking MMIO path 都必須有 `req_valid/req_ready/resp_valid` 或等價握手。
3. host-visible 與 core-visible register bank 必須有獨立 address decode，不得在 RTL 中把兩者混成同一個「誰都可寫」的未定義 bus mux。
4. 本版不得暴露 architected `dma_stream_*` 介面到 top-level 或 firmware contract。

### 12.2 狀態機規則

1. 模組狀態需可在 trace 中觀測。
2. error path 必須有獨立 `ERR` 狀態，而不是直接回 `IDLE`。
3. masked-broadcast sequencer 必須有可觀測的 current target id。
4. DMA command accept/reject、abort、fifo full 狀態必須可觀測。
5. `load_phase` 與 DRAM owner 狀態必須可觀測。

## 13. 驗證規則

至少需要以下驗證點：

1. loader 只做 local copy，不對 cluster/NLU/DMA 產生 runtime side effect。
2. `cc_data_ram` descriptor/payload/stack/event ABI region 的 32-bit word access correctness。
3. broadcast all-target completion semantics。
4. broadcast read 在 `popcount(mask)!=1` 時確實回 fault。
5. DMA command 由 MCU 經 MMIO staging/submit 下發，而非 DMA 自取 descriptor。
6. DMA 在 `cmd_fifo_full` 時對 `submit` 會 reject 並回 fault，不得部分接受命令。
7. PLIC claim/complete 與 level reassert semantics。
8. `MTIP/MSIP` 不經 PLIC，而是直接進入 core local interrupt path。
9. host 與 core 對 shared `CLUSTER_MASK` mirror 的寫入仲裁規則正確。
10. RV32I_Zmmul_Zicsr 5-stage pipeline 的 branch/load-use/CSR hazard correctness。
11. cluster config 完全經由 firmware MMIO sequence 完成，無隱式硬體 side effect。

## 14. 仍需由整合方固定的項目與建議

以下項目不影響本文件的 architected contract，但整合時必須明確選定，否則仍會留下工程風險：

1. 是否支援 runtime reload。
建議：預設不支援；若支援，至少要求 core halted 且 DMA idle，或加入正式 `cc_mem_axi_arbiter`。
2. trace buffer 匯出路徑。
建議：優先由 host debug path 或專用 debug/DMA 機制讀取 event/debug region，不要讓 trace 匯出改變 runtime ownership。
3. NLU data requester 的 controller boundary 形態。
建議：在 controller 內部統一轉為 request/response 抽象介面，再由 wrapper 對接 AXI4-Lite 或實體 NLU bus。
4. cluster data fabric 仲裁策略。
建議：若無明確效能需求，先採固定 reset 起始的 round-robin，因為最容易驗證 starvation 與 trace 行為。

## 15. 總結

本版 HACC Core Controller 的正式定義是：

1. Loader 負責把東西載進 local memory。
2. RV32I_Zmmul_Zicsr 5-stage core 負責 runtime control。
3. `cc_boot_host_if` 是 host-visible control plane，不是 core runtime MMIO。
4. `cc_cmd_fabric` 負責 deterministic MMIO、masked-broadcast 與 local control register。
5. DMA 只執行 MCU 已經明確經由 MMIO staging/submit 下發的搬運命令。
6. shared external IRQ 由 PLIC 以 claim/complete + level reassert 收斂；`MTIP/MSIP` 則走 core-local path。

只要這六個原則不被打破，controller 的 RTL、韌體與 compiler handoff 就會維持一致，且不再依賴舊版 execution complex 的隱含行為。
