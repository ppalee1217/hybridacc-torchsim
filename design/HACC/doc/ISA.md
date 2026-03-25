# HybridAcc Core Controller 規格（HACC Core MCU ISA）

> 目標：定義一套不依賴通用 CPU、可直接實作為 HACC 控制 MCU 的精簡 Native ISA，並把 local memory、descriptor access、MMIO broadcast、payload stream、IRQ 與 subroutine 語意一次定清楚。

---

## 0. 核心結論

本版 ISA 與舊版最大的差異只有一件事：控制主體從「提交 block range 給 execution complex」改為「由 core MCU 直接讀 descriptor、展開 wave、透過 MMIO 與 stream 控制 DMA / cluster / NLU」。

正式結論如下：

1. `cc_core_mcu` 是唯一的 runtime control master。
2. 舊版 `cc_job_manager`、`cc_block_fetch`、`cc_block_expander`、`cc_wave_scheduler` 不再是 primary runtime module。
3. `.hacc.job/.hacc.block/.hacc.profile/.hacc.dma/.hacc.agu/.hacc.nlu/.hacc.pe/.hacc.scan/.hacc.patch` 全部先由 loader 載入 local memory，再由 MCU 以 load / MMIO / stream 指令消費。
4. `cc_data_sram` 的 descriptor ABI region MCU-visible 基本讀取粒度固定為 32-bit word。
5. AHB-Lite broadcast 的完成語意固定為「所有 target transaction 都完成後，該指令才算完成」。
6. IRQ 採 level input + sticky pending bitmap + explicit ack 模型。
7. ISA 必須原生支援 subroutine，因此需有 `CALL`、`RET`、`IRET` 與堆疊操作能力。

---

## 1. 設計目標與邊界

### 1.1 目標

1. 定義可獨立實作的 32-bit HACC Core MCU ISA。
2. 讓 MCU 可直接讀取 descriptor、執行 block-level 控制流程、展開 logical wave。
3. 讓 MCU 可透過單一 command path 控制 cluster、DMA、NLU 與 local CSRs。
4. 保留足夠的控制能力以支援 bring-up、debug、polling、IRQ-driven completion 與例外處理。
5. 讓 compiler 輸出的 `.hacc.core` 可維持小而穩定，但不是 O(1) 到完全失去流程表達能力。

### 1.2 非目標

1. 不把 tensor 算術放進控制 MCU。
2. 不在 ISA 中定義 PE ALU instruction。
3. 不在 ISA 中定義通用作業系統、虛擬記憶體或快取一致性語意。
4. 不要求 MCU 支援 out-of-order、speculation 或多發射。
5. 不要求 loader 在載入期直接對 cluster 或 NLU 發送 runtime program/config stream。

### 1.3 設計原則

1. 程式控制簡單，但不偷渡隱藏硬體狀態機去替 MCU 做 wave orchestration。
2. 所有 runtime side effect 都必須能回溯到一條明確的 ISA 指令。
3. 所有對外 transaction 的完成條件都要在文件中定義，不保留「實作自決」灰色地帶。
4. Local descriptor 資料是 MCU 的可程式資源，不是 execution complex 私有資源。

---

## 2. 架構模型

### 2.1 Programmer-visible 執行模型

```text
+-------------------+        +------------------------+
| HACC Core MCU     | -----> | cc_cmd_fabric          |
| - PC/Decoder      |        | - local CSR decode     |
| - GPR/SP/LR       |        | - DMA MMIO route       |
| - load/store      |        | - cluster AHB route    |
| - mmio/stream     |        | - NLU AHB route        |
| - irq/branch/call |        | - broadcast sequencer  |
+-------------------+        +------------------------+
         |                                  |
         v                                  v
+-------------------+        +-------------------------------+
| I-SRAM            |        | DMA / Cluster / NLU targets   |
| Data-SRAM         |        +-------------------------------+
| - descriptor ABI  |
| - event/debug ABI |
+-------------------+
```

### 2.2 控制責任分界

1. `cc_section_loader` 只把 section payload 複製到 local memories。
2. `cc_core_mcu` 讀 job/block/rule tables，決定要對哪些 cluster/NLU 做哪些操作。
3. `cc_dma_engine` 不再主動從 `cc_data_sram` 的 descriptor ABI region fetch `.hacc.dma`；DMA 規則與 payload 由 MCU 透過 MMIO/stream 寫入 DMA。
4. `.hacc.pe`、`.hacc.scan`、`.hacc.profile`、`.hacc.nlu`、`.hacc.dma` 都是 MCU runtime 消費的 local payload/table，而不是 loader 直接轉發的 side effect。

### 2.3 Job / Block / Wave 的 ISA 觀點

1. Job：一個工作單位的靜態根描述，包含各 section/table 的 base、count、capability 與 runtime policy。
2. Block：一段可由 MCU 反覆展開的壓縮控制片段，通常描述一個 tile region、其 loop shape、對應 profile/PE/NLU/DMA 規則索引與 patch 覆寫規則。
3. Wave：Block 在某個 loop index 組合下的單次 runtime instance。Wave 不一定單獨存成 table，可由 MCU 根據 block formula 即時計算。

因此，ISA 的提交粒度不是「把一批 block 丟給黑盒 execution complex」，而是「MCU 自己執行 block control program，必要時對每一個 logical wave 顯式地下 MMIO/stream 指令」。

---

## 3. Programmer-visible 狀態

### 3.1 基本狀態

1. `PC`：32-bit program counter。
2. `GPR[0..15]`：16 個 32-bit 通用暫存器。
3. `SP`：別名 `r13`，作為 stack pointer。
4. `LR`：別名 `r14`，作為 link register。
5. `SR`：status register，不直接當成 GPR 使用。

### 3.2 固定暫存器別名

| Register | Alias | 用途 |
|---|---|---|
| `r0` | `zero` | 固定為 0 |
| `r13` | `sp` | stack pointer |
| `r14` | `lr` | link register |
| `r15` | `tmp` | 保留給 call veneer / IRQ prologue 使用 |

### 3.3 Status Register 定義

| Bit | Name | 說明 |
|---|---|---|
| 0 | `Z` | 比較或算術結果為零 |
| 1 | `N` | 比較或算術結果為負 |
| 2 | `C` | 進位或借位旗標 |
| 3 | `V` | overflow |
| 4 | `IRQ_EN` | global IRQ enable |
| 5 | `IN_ISR` | 目前位於 IRQ handler 內 |
| 6 | `FAULT` | 進入同步 fault 狀態 |
| 31:7 | reserved | 保留 |

### 3.4 IRQ shadow 狀態

MCU 除了 `SR` 之外，還必須能觀測以下 shadow state：

1. `IRQ_PENDING_BITMAP_LO/HI`
2. `IRQ_ENABLE_BITMAP_LO/HI`
3. `IRQ_CAUSE_ID`
4. `EPC`：exception/interrupt return PC
5. `ESR`：exception/interrupt saved SR

這些資訊可由 CSR 方式讀寫，不映射到一般 GPR。

---

## 4. 記憶體與位址模型

### 4.1 Local address space

建議 MCU 使用固定 32-bit flat address map：

| Base | Region | 說明 |
|---|---|---|
| `0x0000_0000` | I-SRAM | instruction fetch only；data load/store 是否允許由 implementation 決定 |
| `0x1000_0000` | Data-SRAM | MCU-visible unified local data SRAM；由 ABI 區分 descriptor/payload 與 event/debug 區 |
| `0x2000_0000` | Local CSR window | core/irq/timer/state/local peripheral CSR |
| `0x3000_0000` | DMA MMIO window | `cc_dma_engine` command/status window |
| `0x4000_0000` | Cluster command window | 經 `cc_cmd_fabric` decode 到 per-cluster AHB-Lite |
| `0x5000_0000` | NLU MMIO window | 經 `cc_cmd_fabric` decode 到 per-NLU AHB-Lite |

### 4.2 Data-SRAM ABI 分區與存取粒度

`cc_data_sram` 是單一實體 local SRAM，軟體 ABI 在其中保留至少兩個邏輯區段：

1. descriptor/payload ABI region：存放 `.hacc.job/.hacc.block/.hacc.profile/.hacc.dma/.hacc.agu/.hacc.nlu/.hacc.pe/.hacc.scan/.hacc.patch`
2. event/debug ABI region：存放 completion bitmap、trace、debug scratch、optional software stack spill

這兩個區段由固定 ABI offset 或 job metadata 公布的 base/limit 定義；ISA 只要求軟體可 deterministic 地區分兩者，並不強制實作成兩塊獨立 SRAM。

正式規則：

1. MCU 對 `cc_data_sram` 的基本存取寬度固定為 32-bit。
2. 對齊要求為 4-byte。
3. 若實作提供 64-bit 或 128-bit 內部記憶體巨集，對 MCU 仍必須表現為正確的 32-bit load/store 語意。
4. 任何跨界讀寫都必須由程式分成多筆指令，不允許隱含 unaligned merge。
5. descriptor/payload ABI region 與 event/debug ABI region 的使用邊界由軟體 ABI 負責維護；硬體不再提供獨立 `cc_desc_sram` 或 `cc_event_sram` 模組名稱給軟體依賴。

### 4.3 Cluster command 位址模型

```text
cluster_addr = CLUSTER_CMD_BASE + cluster_id * CLUSTER_STRIDE + local_offset
```

建議值：

1. `CLUSTER_CMD_BASE = 0x4000_0000`
2. `CLUSTER_STRIDE   = 0x0001_0000`

`local_offset` 必須與 ComputeCluster AHB-Lite 合約一致：

1. `0x0000 ~ 0x00FF`：SPM config / PMU window
2. `0x1000 ~ 0x1FFF`：HDDU MMIO passthrough window
3. `0x2000 ~ 0x20FF`：NoC command sideband window

### 4.4 NLU 位址模型

```text
nlu_addr = NLU_MMIO_BASE + nlu_id * NLU_STRIDE + local_offset
```

建議值：

1. `NLU_MMIO_BASE = 0x5000_0000`
2. `NLU_STRIDE    = 0x0000_1000`

### 4.5 DMA 位址模型

DMA engine 是 local peripheral，因此其 register 與 stream sink 對 MCU 表現為 local window：

1. `DMA_MMIO_BASE = 0x3000_0000`
2. `DMA_STREAM_DATA = DMA_MMIO_BASE + 0x0100`
3. `DMA_STREAM_CTRL = DMA_MMIO_BASE + 0x0104`

正式規則：DMA 對 DRAM 的 AXI4 master 與 section loader 共用 top-level `m_mem_axi_*`，ISA 不直接暴露 DRAM transaction，而是透過 DMA MMIO/stream 控制。

---

## 5. CSR 契約

### 5.1 Core-local CSR window

| Offset | Name | Access | 說明 |
|---|---|---|---|
| `0x000` | `CORE_CTRL` | RW | bit0=`RUN`, bit1=`HALT_REQ`, bit2=`SSTEP`, bit3=`CLR_FAULT` |
| `0x004` | `CORE_STATUS` | RO | bit0=`RUNNING`, bit1=`HALTED`, bit2=`FAULT`, bit3=`IN_ISR` |
| `0x008` | `IRQ_PENDING_LO` | RO | sticky pending bitmap low |
| `0x00C` | `IRQ_PENDING_HI` | RO | sticky pending bitmap high |
| `0x010` | `IRQ_ENABLE_LO` | RW | irq enable low |
| `0x014` | `IRQ_ENABLE_HI` | RW | irq enable high |
| `0x018` | `IRQ_ACK_LO` | WO | write-1-to-ack；若 source level 仍為 1，pending 會重新置位 |
| `0x01C` | `IRQ_ACK_HI` | WO | write-1-to-ack |
| `0x020` | `IRQ_CAUSE_ID` | RO | 目前仲裁後的最高優先權 cause id |
| `0x024` | `EPC` | RW | exception/interrupt return PC |
| `0x028` | `ESR` | RW | exception/interrupt saved SR |
| `0x02C` | `FAULT_CODE` | RO | fault code |
| `0x030` | `FAULT_AUX` | RO | auxiliary info |
| `0x034` | `CYCLE_CNT_LO` | RO | cycle counter low |
| `0x038` | `CYCLE_CNT_HI` | RO | cycle counter high |
| `0x03C` | `INSTRET_CNT_LO` | RO | retired instruction low |
| `0x040` | `INSTRET_CNT_HI` | RO | retired instruction high |

### 5.2 DMA MMIO window

| Offset | Name | Access | 說明 |
|---|---|---|---|
| `0x000` | `DMA_CTRL` | RW | bit0=`START`, bit1=`ABORT`, bit2=`RESET` |
| `0x004` | `DMA_STATUS` | RO | bit0=`BUSY`, bit1=`DONE`, bit2=`ERROR`, bit3=`STREAM_READY` |
| `0x008` | `DMA_MODE` | RW | rule mode / payload kind |
| `0x00C` | `DMA_TARGET_CLUSTER_MASK` | RW | 目標 cluster mask |
| `0x010` | `DMA_WORD_COUNT` | RW | 本次 stream word count |
| `0x014` | `DMA_ERR_CODE` | RO | DMA error code |
| `0x018` | `DMA_ERR_AUX` | RO | DMA error aux |
| `0x100` | `DMA_STREAM_DATA` | WO | 32-bit stream data |
| `0x104` | `DMA_STREAM_CTRL` | WO | bit0=`LAST`, bit1=`SOP`, bit2=`EOP` |

DMA 如何解釋 stream payload 由 DMA programming model 定義，但 ISA 層要求：

1. 所有由 `.hacc.dma` 派生的 runtime payload 都必須能以 32-bit word stream 送入 DMA。
2. 指令完成不得僅以 `DMA_STREAM_DATA` 被寫進 skid buffer 作為準則；若目的指令語意是 blocking，則必須等待 DMA side 接收完成。

### 5.3 目標 peripheral common status

Cluster 與 NLU 的狀態寄存器由其各自文件定義；本 ISA 只規定兩件事：

1. MCU 對 cluster/NLU 的控制必須走 `cc_cmd_fabric`。
2. 若使用 broadcast write，完成條件是所有被 mask 命中的 target 都完成相同 AHB-Lite transfer。

---

## 6. 指令格式與編碼類別

### 6.1 基本規格

1. 固定 32-bit instruction。
2. Little-endian。
3. 4-byte aligned fetch。

### 6.2 格式

#### Format-R

```text
[31:26] OPC   [25:22] RD   [21:18] RS1   [17:14] RS2   [13:0] FUNC
```

#### Format-I

```text
[31:26] OPC   [25:22] RD   [21:18] RS1   [17:0] IMM18
```

#### Format-S

```text
[31:26] OPC   [25:22] RS2  [21:18] RS1   [17:0] IMM18
```

#### Format-B

```text
[31:26] OPC   [25:22] RS1  [21:18] RS2   [17:0] IMM18 (signed, <<2)
```

#### Format-J

```text
[31:26] OPC   [25:0] IMM26 (signed, <<2)
```

#### Format-X（extended control）

```text
[31:26] OPC   [25:22] RD/RS [21:18] RS1   [17:14] RS2   [13:0] SUBOP/IMM14
```

`Format-X` 用於 CSR、MMIO、stream、IRQ 控制等較特殊指令。

---

## 7. 指令集

### 7.1 基礎整數與控制流

| Mnemonic | 語意 |
|---|---|
| `MOVI rd, imm18` | load small immediate |
| `MOVHI rd, imm18` | 寫入高位 immediate，常與 `MOVI` 搭配形成 32-bit 常數 |
| `MOV rd, rs1` | register move |
| `ADD rd, rs1, rs2` | 加法 |
| `ADDI rd, rs1, imm18` | 加法立即數 |
| `SUB rd, rs1, rs2` | 減法 |
| `AND rd, rs1, rs2` | bitwise and |
| `OR rd, rs1, rs2` | bitwise or |
| `XOR rd, rs1, rs2` | bitwise xor |
| `SHL rd, rs1, imm5` | logical left shift |
| `SHR rd, rs1, imm5` | logical right shift |
| `CMP rs1, rs2` | 更新 `Z/N/C/V` |
| `CMPI rs1, imm18` | compare immediate |
| `B imm26` | unconditional branch |
| `BEQ rs1, rs2, imm18` | equal branch |
| `BNE rs1, rs2, imm18` | not-equal branch |
| `BLT rs1, rs2, imm18` | signed less-than branch |
| `BGE rs1, rs2, imm18` | signed greater/equal branch |
| `CALL imm26` | `lr = next_pc`，跳到 `PC + imm26<<2` |
| `CALLR rs1` | `lr = next_pc`，跳到 `rs1` |
| `RET` | 跳到 `lr` |
| `NOP` | no operation |
| `HLT` | halt until debug or reset |

### 7.2 Local memory access

| Mnemonic | 語意 |
|---|---|
| `LDW rd, [rs1 + imm18]` | 讀取 32-bit word |
| `STW rs2, [rs1 + imm18]` | 寫入 32-bit word |
| `LDB rd, [rs1 + imm18]` | 讀取 byte，zero-extend |
| `STB rs2, [rs1 + imm18]` | 寫入 byte |
| `PUSH rs1` | `sp -= 4; [sp] = rs1` |
| `POP rd` | `rd = [sp]; sp += 4` |

正式規則：

1. `LDW/STW` 對 `cc_data_sram` 的 descriptor ABI region、event/debug ABI region 與 local peripheral window 均必須有效。
2. 對 command fabric 視窗做 `LDW/STW` 時，效果等價於 blocking read/write MMIO。
3. 對 write-only 或 read-only window 執行不合法方向操作時，必須產生同步 fault。

### 7.3 CSR 存取

| Mnemonic | 語意 |
|---|---|
| `CSRRD rd, csr_id` | read CSR |
| `CSRWR csr_id, rs1` | write CSR |
| `CSRSI csr_id, imm18` | set bits |
| `CSRCL csr_id, imm18` | clear bits |

### 7.4 MMIO 指令

| Mnemonic | 語意 |
|---|---|
| `MMIOW [rs1 + imm18], rs2` | blocking MMIO write |
| `MMIOR rd, [rs1 + imm18]` | blocking MMIO read |
| `MMIOWB mask_rs, [rs1 + imm14], rs2` | broadcast blocking write |
| `MMIORD mask_rs, rd, [rs1 + imm14]` | broadcast read，僅允許所有 target 回傳值必須相同的場景 |

`MMIOWB` 的正式語意：

1. `mask_rs` 指定 cluster 或 NLU target mask。
2. fabric 必須把同一筆 32-bit write 對所有命中的 target 逐一或平行送出。
3. 只有當所有 target transaction 都完成且無錯誤時，指令才 retire。
4. 任一 target 出錯，整筆指令以 fault / error completion 結束，不允許部分成功但 ISA 視為完成。

### 7.5 Stream 指令

| Mnemonic | 語意 |
|---|---|
| `STRM dst_sel, rs_base, rs_count` | 從 local memory 連續輸出 `rs_count` 個 32-bit words 到目標 stream sink |
| `STRMI dst_sel, rs_data` | 立即送出一個 32-bit word 到目標 stream sink |
| `STRMC dst_sel, imm14` | 發送 control token，例如 `SOP/EOP/LAST` |

`dst_sel` 必須至少支援：

1. `0`：DMA stream sink
2. `1`：Cluster NoC command sink
3. `2`：Cluster HDDU/profile payload sink
4. `3`：NLU config payload sink

`STRM` 的正式規則：

1. 來源資料從 local address space 取出，基本粒度為 32-bit。
2. `rs_count = 0` 時不做任何 side effect。
3. 指令必須依照 word 順序送出；不得重排。
4. 目標 sink 若 backpressure，指令必須停住直到完成。
5. `STRM` 對應的 blocking 完成定義是：最後一個 word 已被目標 sink 接收。
6. 若目標是 broadcast-capable sink，則每一個 word 也必須遵守 all-target completion。

這個定義是為了明確支援以下場景：

1. `.hacc.dma` 由 MCU 逐字串流到 DMA engine。
2. `.hacc.pe` 與 `.hacc.scan` 由 MCU 串流到 cluster NoC command path。
3. `.hacc.profile` 與 `.hacc.agu` 由 MCU 串流到 cluster HDDU/MMIO path。
4. `.hacc.nlu` 由 MCU 串流到 NLU config window。

### 7.6 IRQ / 同步指令

| Mnemonic | 語意 |
|---|---|
| `WFI rs1` | wait-for-interrupt；`rs1` 指定允許喚醒的 pending mask policy |
| `WAIT rd, rs1` | 等待 local event/IRQ/timeout 條件成立，返回狀態碼到 `rd` |
| `ACKIRQ rs1` | 對 `IRQ_ACK_*` 寫入 ack mask |
| `EI` | enable global irq |
| `DI` | disable global irq |
| `IRET` | 從 interrupt handler 返回，`PC = EPC`、`SR = ESR` |

IRQ 正式模型：

1. 外部輸入是 level-sensitive signal。
2. `cc_irq_router` 先同步化，再把有效 level 置位到 sticky pending bitmap。
3. 軟體寫 `ACKIRQ` 後，只是清 pending latch；若來源 signal 仍維持 asserted，pending 必須重新置位。
4. 因此驅動程式應先處理來源、使其 deassert，再做 ack；否則會立刻再中斷。

### 7.7 陷阱與例外

同步例外至少包含：

1. illegal opcode
2. unaligned word access
3. MMIO protection fault
4. broadcast target fault
5. local memory bounds fault

非同步中斷至少包含：

1. cluster done/error pending
2. DMA done/error pending
3. NLU done/error pending
4. timer pending

---

## 8. 指令時序與一致性規則

### 8.1 基本 retire 規則

1. 一般 ALU 指令在單個 architectural step 內完成。
2. `LDW/STW` 對 local SRAM 的延遲可實作為多 cycle，但 retire 前必須保證結果已生效。
3. `MMIOW/MMIOR/MMIOWB/MMIORD/STRM` 都是 blocking 指令，不允許 architecturally fire-and-forget。

### 8.2 MMIO broadcast 規則

Broadcast 不是單一總線 transaction，而是 fabric-level sequenced multi-target operation。ISA 層要求：

1. 發送順序必須 deterministic。
2. 所有 target 都要完成，指令才完成。
3. 錯誤回報需帶 target id。
4. 除非未來另定 relaxed variant，本版不定義「部分 target 完成即可繼續」模式。

### 8.3 Stream 規則

1. 同一條 `STRM` 指令內的 payload word 順序不得改變。
2. 不同 `STRM` 指令之間也必須保持程式順序，除非軟體自己建立雙 buffer 並依狀態 CSR 控制。
3. 若目標 peripheral 需要 `START` 位元，應由 `STRM` 前後配合 `MMIOW` 顯式控制，不允許隱含 side effect。

### 8.4 Subroutine 與 IRQ 交互

1. `CALL` 只更新 `lr`，不自動保存其他暫存器。
2. IRQ entry 由硬體自動保存 `PC -> EPC`、`SR -> ESR`，但一般 GPR 由軟體在 handler 中決定是否 `PUSH/POP`。
3. `IRET` 只能在 `IN_ISR=1` 時合法執行。

---

## 9. 最低軟體慣例

### 9.1 呼叫慣例建議

建議 ABI：

1. `r0`：常數零
2. `r1-r4`：argument / scratch
3. `r5-r8`：caller-saved temporary
4. `r9-r12`：callee-saved
5. `r13`：`sp`
6. `r14`：`lr`
7. `r15`：scratch / veneer temporary

### 9.2 Block runtime 建議流程

MCU 韌體對一個 block 的典型流程：

1. 從 `.hacc.block` 載入 block header。
2. 根據 loop shape 與 rule index 計算當前 wave 參數。
3. 以 `MMIOWB` 或 `STRM` 套用 `.hacc.profile/.hacc.agu`。
4. 以 `STRM` 將 `.hacc.dma` 派生 payload 送進 DMA engine。
5. 以 `MMIOWB` 寫 cluster start。
6. 若需要 NLU phase，則用 `MMIOW/STRM` 設定 NLU 並等待 IRQ。
7. 處理 wave/block 完成與錯誤收斂。

### 9.3 Busy-wait 與 IRQ-driven 兩種模式

1. Bring-up 可使用 `MMIOR` 或 `WAIT` 輪詢 status。
2. 正式 runtime 建議開啟 `EI`，使用 `WFI + IRQ handler + ACKIRQ`。

---

## 10. 最低範例

### 10.1 範例目的

下例示範 MCU 如何：

1. 呼叫子程序載入一段 profile payload。
2. 對 cluster mask 做 broadcast write。
3. 對 DMA engine stream 一段 `.hacc.dma` payload。
4. 等待 cluster IRQ 完成。

```asm
# r1 = profile_base
# r2 = profile_words
# r3 = cluster_base
# r4 = cluster_mask
# r5 = dma_base
# r6 = dma_words

start:
  CALL   load_profile
  CALL   launch_dma
  EI
  WFI    r0
  DI
  HLT

load_profile:
  STRM   2, r1, r2
  MMIOWB r4, [r3 + 0x1000], r0
  RET

launch_dma:
  MMIOW  [DMA_MMIO_BASE + 0x008], r0
  STRM   0, r5, r6
  MOVI   r7, 1
  MMIOW  [DMA_MMIO_BASE + 0x000], r7
  RET

irq_handler:
  CSRRD  r8, IRQ_PENDING_LO
  ACKIRQ r8
  IRET
```

這個範例刻意保持極小，但已經覆蓋本版 ISA 的三個核心能力：subroutine、broadcast、stream。

---

## 11. 與舊版 ISA 的不相容變更

1. `HCFG`、`HRUN`、`HWAIT`、`HPOLL`、`HERR`、`HABORT` 不再是 primary control model。
2. 舊版「execution complex 讀 `.hacc.dma`」改為「MCU 讀 table 後把 payload stream 給 DMA」。
3. 舊版 loader 直接轉送 `.hacc.pe/.hacc.scan` 的模式被移除。
4. 舊版把 wave orchestration 隱藏在硬體狀態機中的假設被取消。

若某些 bring-up 程式仍需保留舊 stub，可透過 compatibility shim 在編譯期轉換成新 MCU control flow，但文件本身不再定義舊語意。

---

## 12. 總結

本版 HACC Core MCU ISA 的正式定位如下：

1. 它不是通用 CPU ISA。
2. 它也不是只會 `HRUN` 的極簡 doorbell ISA。
3. 它是一套專門用來讀 local descriptor、呼叫子程序、對多 target 發 MMIO/broadcast、對 DMA/NLU/NoC 發 payload stream、並以 level+sticky+ack IRQ 模型收斂完成與錯誤的控制 ISA。

只要 RTL、compiler、loader 與 firmware 都依照這個模型實作，整個 HACC runtime 的控制流就不再依賴隱含的 execution complex 黑盒行為。
