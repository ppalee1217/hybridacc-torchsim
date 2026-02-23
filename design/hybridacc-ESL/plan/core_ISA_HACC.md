# HybridAcc Core Controller 規格（HACC Native Core-ISA）

> 目標：在 **不使用 RISC-V CPU core** 的前提下，定義一套可直接驅動 HybridAcc 的精簡 Core-ISA。
>
> 核心方向：**少數控制指令 + 硬體微序列執行 + descriptor 資料驅動**。

---

## 0. 設計目標與邊界

### 0.1 目標

1. 定義可獨立實作的 **HACC Native ISA**（非 RV32I 相容需求）。
2. Core 只負責任務提交與例外處理；資料搬移與 cluster 控制由硬體單元完成。
3. 支援數千至數萬 wave 的穩定執行，且 core 程式大小維持 O(1)。

### 0.2 邊界

1. 不變更 PE ALU 指令語意。
2. 不變更 HDDU/AGU/Cluster/DMA 的既有 MMIO register map。
3. 不導入多程序 OS 模型；MVP 為單 context、單 command queue。

### 0.3 設計原則

1. ISA 指令語意只到「工作提交/同步/查狀態」層級。
2. wave 內部 prefetch/apply/start/sync/drain 全部由硬體狀態機完成。
3. 所有可觀測狀態（busy/error/wave_id）必須可由 CSR 讀取。

---

## 1. 架構總覽

### 1.1 總體架構

```text
+--------------------+        +------------------------------+
| HACC Core Frontend | -----> | HACC Command Queue           |
| (Native ISA)       |        +------------------------------+
|  - PC/Decoder      |                    |
|  - 16 GPR          |                    v
+--------------------+        +------------------------------+
                              | HACC Execution Complex        |
                              |  - Descriptor Fetch Unit      |
                              |  - Wave Scheduler             |
                              |  - Profile Apply Unit         |
                              |  - Cluster Broadcast Unit     |
                              |  - DMA Range Unit             |
                              |  - Sync/Timeout Unit          |
                              |  - Error/IRQ Unit             |
                              +------------------------------+
                                            |
                                +-----------+-----------+
                                | MMIO Bus / Desc Memory |
                                +------------------------+
```

### 1.2 執行模型

1. Core Frontend 取指後發送高階命令至 Command Queue。
2. Execution Complex 依 JobDesc/WaveDesc 自主執行，不需每波軟體介入。
3. Core 可用 blocking 指令等待完成，或 polling 讀狀態。

### 1.3 可程式化粒度

1. **Job 粒度**：設定 table base、policy、wave range。
2. **Batch 粒度**：一次提交多個 wave。
3. **Wave 粒度**：由硬體 scheduler 自動展開微流程。

---

## 2. 位址模型與 MMIO 版圖

### 2.1 固定基底位址

- `CLUSTER_MMIO_BASE = 0x0000_0000`
- `CLUSTER_STRIDE    = 0x0001_0000`
- `DMA_BASE          = 0x8000_0000`
- `HACC_CSR_BASE     = 0x9000_0000`
- `DESC_MEM_BASE     = 0xA000_0000`

### 2.2 Cluster 位址換算

```text
cluster_addr = CLUSTER_MMIO_BASE + cluster_id * CLUSTER_STRIDE + local_offset
```

local window（沿用）：
1. `0x0000~0x00FF`：SPM config
2. `0x1000~0x1FFF`：HDDU passthrough
3. `0x2000~0x20FF`：NoC command

### 2.3 HACC CSR 空間（Core 可見）

1. `0x000 HACC_CTRL`
   - bit0 `START`
   - bit1 `ABORT`
   - bit2 `RESET`
2. `0x004 HACC_STATUS`
   - bit0 `BUSY`
   - bit1 `DONE`
   - bit2 `ERROR`
   - bit3 `IRQ_PENDING`
3. `0x008 HACC_JOB_BASE_LO`
4. `0x00C HACC_JOB_BASE_HI`
5. `0x010 HACC_WAVE_BEGIN`
6. `0x014 HACC_WAVE_COUNT`
7. `0x018 HACC_ERR_CODE`
8. `0x01C HACC_ERR_AUX`
9. `0x020 HACC_LAST_WAVE_ID`
10. `0x024 HACC_PERF_CYCLE`
11. `0x028 HACC_PERF_MMIO_CNT`
12. `0x02C HACC_PERF_DMA_CNT`

### 2.4 Descriptor 記憶體佈局

1. `JOB_DESC_TABLE`
2. `WAVE_DESC_TABLE`
3. `PROFILE_TABLE`
4. `DMA_DESC_TABLE`
5. `CONST_TABLE`（可選）

---

## 3. HACC 指令（詳細 ISA 規格）

> 本章定義「非 RISC-V」的原生 HACC 指令格式與語意。

### 3.1 架構狀態

1. `PC`：32-bit 程式計數器。
2. `GPR[0..15]`：16 個 32-bit 通用暫存器（`r0` 固定為 0）。
3. `PRED`：條件旗標（`Z/E`）。
4. `SYS`：系統狀態（halt/exception mask）。

### 3.2 指令長度與對齊

1. 固定 32-bit 指令寬度。
2. 指令位址需 4-byte 對齊。
3. Little-endian 指令儲存。

### 3.3 編碼格式

#### Format-R（雙來源 + 目的）

```text
[31:26] OPC   [25:22] RD   [21:18] RS1   [17:14] RS2   [13:0] FUNC/IMM14
```

#### Format-I（來源 + 立即數）

```text
[31:26] OPC   [25:22] RD   [21:18] RS1   [17:0] IMM18
```

#### Format-J（控制流）

```text
[31:26] OPC   [25:0]  IMM26 (signed, <<2)
```

### 3.4 Opcode 類別

1. `0x00~0x07`：Core 控制（MOVI/MOV/ADD/CMP/JMP/BR）
2. `0x08~0x0F`：CSR 讀寫（CSR_RD/CSR_WR）
3. `0x10~0x1F`：HACC 控制（CFG/LAUNCH/KICK/WAIT/POLL/ERR/ABORT）
4. `0x20~0x2F`：除錯與追蹤（STEP/TRACE/BREAK）

### 3.5 MVP 指令表

#### A) 基礎控制（為了不依賴外部 CPU）

1. `MOVI rd, imm18`
   - `rd = sign_ext(imm18)`
2. `MOV rd, rs1`
   - `rd = rs1`
3. `ADD rd, rs1, rs2`
   - `rd = rs1 + rs2`
4. `AND rd, rs1, rs2`
   - `rd = rs1 & rs2`
5. `CMP rs1, rs2`
   - 設定 `PRED.Z = (rs1 == rs2)`
6. `JMP imm26`
   - `PC = PC + (sign_ext(imm26) << 2)`
7. `BRZ imm26`
   - 若 `PRED.Z==1`，分支

> 說明：這 7 條僅用於最小控制流程，非資料面主力運算 ISA。

#### B) CSR 存取

8. `CSR_WR csr_id, rs1`
   - 將 `rs1` 寫入 CSR
9. `CSR_RD rd, csr_id`
   - 將 CSR 讀入 `rd`

#### C) HACC 任務控制

10. `HCFG rs1`
   - `rs1` 指向 JobConfig base；等效寫入 `HACC_JOB_BASE_*`
11. `HLAUNCH rs1, rs2`
   - `wave_begin=rs1`、`wave_count=rs2`，僅入 queue（non-blocking）
12. `HKICK`
   - 觸發 scheduler doorbell
13. `HWAIT rs1`
   - `rs1` 為 wait mask：bit0 done、bit1 error、bit2 timeout
   - blocking，直到任一條件成立
14. `HPOLL rd`
   - `rd = HACC_STATUS`
15. `HERR rd`
   - `rd = (ERR_CODE[15:0] | (ERR_AUX[31:16] << 16))`
16. `HABORT`
   - 中止執行並清空 inflight
17. `HBREAK`
   - 進入 debug halt

### 3.6 指令時序語意

1. `HCFG/HLAUNCH/HKICK` 為順序一致：提交順序等於執行觀察順序。
2. `HWAIT` 為強同步點；返回時 `STATUS/ERR/LAST_WAVE_ID` 必須可見。
3. `HABORT` 完成條件：Execution Complex idle + MMIO sequencer drained。

### 3.7 例外與錯誤碼

`ERR_CODE` 建議：
1. `0x01`：Descriptor bounds error
2. `0x02`：DMA channel error
3. `0x03`：Cluster MMIO timeout
4. `0x04`：Illegal sync policy
5. `0x05`：Queue overflow
6. `0x06`：Unsupported capability

`ERR_AUX`：
- [7:0] channel_id
- [15:8] cluster_id
- [31:16] wave_id

---

## 4. `.hacc` 可讀格式

### 4.1 設計重點

`.hacc` 以「描述資料 + 執行策略」為主，避免手寫 per-wave 指令碼。

### 4.2 檔案骨架

```text
program <name> {
  meta { ... }
  hardware { ... }
  tensors { ... }
  pe_programs { ... }
  scan_chain { ... }
  hddu_profiles { ... }
  dma_plan { ... }
  wave_graph { ... }
  runtime_policy { ... }
}
```

### 4.3 必填欄位

1. `cluster_count`
2. `deploy_mask`
3. `cluster_stride`
4. `dma_base`
5. `wave_count`
6. `scheduler_policy`（`in_order` / `overlap_prefetch` / `aggressive`）
7. `timeout_cycle`

### 4.4 `wave_graph` 語意

每筆 wave 需包含：
1. `cluster_mask`
2. `profile_id`
3. `pe_program_id`
4. `prefetch_begin/prefetch_count`
5. `drain_begin/drain_count`
6. `sync_policy`

壓縮語法：
1. `repeat(n)`
2. `tile_loop(i,j,k)`
3. `delta_rule(field, base, step)`

### 4.5 `runtime_policy`

1. `error_action = trap|skip|abort`
2. `retry_max`
3. `profile_mode = full|delta|auto`
4. `completion_bitmap = on|off`

---

## 5. Compiler/Packager 流程

### 5.1 編譯管線

```text
.hacc
  -> Parser(AST)
  -> Semantic Check
  -> Wave DAG & Policy IR
  -> Descriptor Lowering
       - JOB_CONFIG
       - WAVE_DESC
       - PROFILE_TABLE
       - DMA_DESC
  -> Core Stub Emit (HACC Native ISA)
  -> Packager (.hap)
```

### 5.2 Semantic Check（必做）

1. `deploy_mask` 不超出 `cluster_count`
2. wave DAG 無循環
3. DMA 覆蓋 tensor byte range
4. profile/pe_program 引用合法
5. sync policy 與 capability 相容

### 5.3 Core Stub Emit（非 RISC-V）

編譯器只輸出固定流程：
1. `HCFG`
2. `HLAUNCH`
3. `HKICK`
4. `HWAIT`
5. `HPOLL/HERR/HABORT`

### 5.4 Packager

1. section 對齊與版本資訊
2. descriptor 壓縮（RLE/delta/formula）
3. 產生 debug map（wave_id -> source）

---

## 6. 封裝格式

### 6.1 Header

```cpp
struct HaccPkgHeader {
  uint32_t magic;          // 'HACP'
  uint16_t version_major;
  uint16_t version_minor;
  uint32_t section_count;
  uint32_t entry_offset;   // Native ISA entry
  uint32_t flags;          // bit0: little-endian, bit1: desc-compressed
  uint32_t target_caps;    // capability bitmap
};
```

### 6.2 Section Type

```cpp
enum SectionType : uint32_t {
  SEC_META           = 0,
  SEC_CORE_STUB      = 1,
  SEC_JOB_CONFIG     = 2,
  SEC_WAVE_DESC      = 3,
  SEC_PROFILE_TABLE  = 4,
  SEC_DMA_DESC       = 5,
  SEC_PE_PROGRAM     = 6,
  SEC_SCAN_CHAIN     = 7,
  SEC_DEBUG_MAP      = 8,
  SEC_SYMBOL         = 9
};
```

### 6.3 Loader 順序

1. 驗證 `magic/version/target_caps`
2. 載入 PE/scan-chain/profile/dma/wave descriptors
3. 設定 `PC = SEC_CORE_STUB.entry`
4. 啟動 HACC Native Core 執行

---

## 7. Core 程式範本（HACC Native ISA）

> 不使用 `li/lw` 等 RISC-V 指令，僅使用本規格定義指令。

```asm
# r0 固定 0
# r1: job_cfg_ptr
# r2: wave_begin
# r3: wave_count
# r4: wait_mask
# r5: status
# r6: err

start:
  MOVI   r1, JOBCFG_PTR_LO
  HCFG   r1

  MOVI   r2, 0
  MOVI   r3, WAVE_COUNT
  HLAUNCH r2, r3
  HKICK

wait_done:
  MOVI   r4, 0b011       # done or error
  HWAIT  r4
  HPOLL  r5

  MOVI   r6, 0x4         # ERROR bit mask
  AND    r5, r5, r6      # 若實作 AND 指令；MVP 可改 CSR 判斷流程
  CMP    r5, r0
  BRZ    finish

on_error:
  HERR   r6
  HABORT
  HBREAK

finish:
  HBREAK
```

### 7.1 MVP 指令子集（可執行上例）

若要執行上例，MVP 至少需有：
1. `MOVI`
2. `CMP`
3. `BRZ`
4. `HCFG`
5. `HLAUNCH`
6. `HKICK`
7. `HWAIT`
8. `HPOLL`
9. `HERR`
10. `HABORT`
11. `HBREAK`

---

## 8. 大量 Wave（數千～數萬）策略

### 8.1 核心策略

1. Code 固定，資料擴張：`code O(1), data O(Nwave)`。
2. Wave 以 chunk 提交（建議 256~2048 waves/批次）。
3. Descriptor Fetch Unit 預抓下一批，降低記憶體停頓。

### 8.2 描述壓縮

1. `delta profile`
2. `run-length wave`
3. `formula wave`

```text
addr = base0 + i*delta_i + j*delta_j + k*delta_k
```

### 8.3 排程模式

1. `in_order`：最穩定、易除錯
2. `overlap_prefetch`：預設，適度重疊
3. `aggressive`：高吞吐，需更嚴格 buffer 管理

### 8.4 錯誤恢復

1. wave 完成 bitmap 記錄已完成項目
2. 支援從 `wave_begin = last_success+1` 續跑
3. timeout wave 可標記 bad-wave 並依 policy skip/abort

---

## 9. 客製化 Unit 硬體功能與行為（詳細）

### 9.1 Descriptor Fetch Unit (DFU)

**功能**
1. 依 `JOB_CONFIG` 取得各 descriptor table base。
2. 以 `wave_id` 抓取 WaveDesc 與關聯 profile/dma 索引。
3. 提供 prefetch FIFO 給 Wave Scheduler。

**狀態機**
1. `IDLE`：等待 scheduler request
2. `REQ`：發出 desc memory 讀請求
3. `WAIT`：等待回應
4. `PUSH`：推入 FIFO
5. `ERR`：地址越界/回應錯誤

**錯誤條件**
1. table 越界
2. desc 格式非法
3. memory timeout

### 9.2 Wave Scheduler Unit (WSU)

**功能**
1. 從 DFU FIFO 取 wave task。
2. 依 `sync_policy`/`scheduler_policy` 決定發送時機。
3. 維護 inflight wave window。

**關鍵行為**
1. `in_order`：下一波需等上一波完成。
2. `overlap_prefetch`：允許 prefetch 與 compute 重疊。
3. `aggressive`：允許多波 prefetch + drain pipeline。

**狀態機**
1. `S_IDLE`
2. `S_DISPATCH_PREFETCH`
3. `S_DISPATCH_APPLY`
4. `S_DISPATCH_START`
5. `S_TRACK_SYNC`
6. `S_DISPATCH_DRAIN`
7. `S_DONE`
8. `S_ERR`

### 9.3 Profile Apply Unit (PAU)

**功能**
1. 將 profile 寫入目標 cluster 的 HDDU/AGU register。
2. 支援 `full` 與 `delta` 兩模式。

**行為規範**
1. `full`：寫入 profile 全欄位。
2. `delta`：僅寫變動欄位，需保證前一版 profile cache 命中。
3. 寫入順序固定：`iter -> stride -> base -> mask -> ctrl`。

**完成條件**
- 全目標 cluster 寫入完成且 MMIO ack 可見。

### 9.4 Cluster Broadcast Unit (CBU)

**功能**
1. 將 `cluster_mask + local_offset + value` 展開為多筆 MMIO write。
2. 保證單命令內 cluster 寫入保序。

**行為規範**
1. bitmap 從低位 cluster_id 到高位。
2. 可選 `strict_ack`：每筆都需 ack 才進下一筆。
3. timeout 產生 `ERR_CODE=0x03`。

### 9.5 DMA Range Unit (DRU)

**功能**
1. 依 `prefetch_begin/count` 或 `drain_begin/count` 掃描 DMA_DESC。
2. 配置 channel 並發送 DMA start。
3. 追蹤 channel inflight counter。

**狀態機**
1. `D_IDLE`
2. `D_FETCH_DESC`
3. `D_START_DMA`
4. `D_WAIT_DMA`
5. `D_DONE`
6. `D_ERR`

**錯誤來源**
1. DMA status error
2. channel deadlock（watchdog）
3. descriptor 非法（長度 0、地址未對齊）

### 9.6 Sync/Timeout Unit (STU)

**功能**
1. 實作 `wait_cluster_done` / `wait_dma_done` / `wait_both`。
2. 對每 wave 與每 batch 提供 watchdog。

**行為規範**
1. timeout 到期：置 `ERROR=1`、記錄 `ERR_AUX.wave_id`。
2. 若 policy=`skip`：標記壞波後交由 scheduler 繼續。
3. 若 policy=`abort`：向全系統發出 stop request。

### 9.7 MMIO Sequencer Unit (MSU)

**功能**
1. 串行化各單元輸出的 MMIO 交易。
2. 處理 write posting 與 read-after-write 可見性。

**一致性規則**
1. 同一 target register 寫入不可重排。
2. 對 status read 前，必須 flush 同目標 pending write。
3. `HWAIT` 返回前，所有「該 wait 條件所需」狀態已一致可見。

### 9.8 Error/IRQ Unit (EIU)

**功能**
1. 匯總各單元錯誤。
2. 產生統一 `ERR_CODE/ERR_AUX`。
3. 產生 `IRQ_PENDING` 與 debug break 事件。

**優先級（高到低）**
1. descriptor 越界
2. DMA 致命錯誤
3. MMIO timeout
4. policy 非法

---

## 10. 版本化與 capability

### 10.1 Capability Bitmap

1. bit0：CBU（broadcast）
2. bit1：DRU（dma range）
3. bit2：WSU overlap
4. bit3：PAU delta
5. bit4：completion bitmap
6. bit5：debug step mode

### 10.2 編譯器降級策略

1. 無 bit3 -> 強制 profile full write
2. 無 bit2 -> 強制 `in_order`
3. 無 bit4 -> 不輸出可恢復執行資訊

---

## 11. 驗證與可觀測性

### 11.1 最低 trace 事件

1. `EV_WAVE_DISPATCH(wave_id)`
2. `EV_WAVE_DONE(wave_id)`
3. `EV_DMA_START(desc_id,ch)`
4. `EV_DMA_DONE(desc_id,ch)`
5. `EV_ERROR(code,aux)`

### 11.2 建議覆蓋測試

1. 單波 smoke（full profile）
2. 多波 overlap（含 prefetch/drain 重疊）
3. timeout/error injection
4. 續跑（resume from last_success）

---

## 12. Boot/Load Ownership 與啟動時序

### 12.1 Ownership（誰負責載入）

本規格採「外部 Loader 負責載入，HACC Core 負責執行」：

1. **外部 Loader（Host Driver / Bootloader / Runtime）**
    - 解析 `.hap`
    - 配置目標記憶體
    - 載入 `SEC_PE_PROGRAM`、`SEC_SCAN_CHAIN`、`SEC_JOB_CONFIG`、`SEC_WAVE_DESC`、`SEC_PROFILE_TABLE`、`SEC_DMA_DESC`
    - 寫入啟動前 CSR（job base、wave range）

2. **HACC Native Core**
    - 執行 `HCFG/HLAUNCH/HKICK/HWAIT`
    - 監控 `HACC_STATUS`
    - 錯誤時 `HERR/HABORT`

3. **HACC Execution Complex（DFU/WSU/PAU/DRU...）**
    - 讀 descriptor
    - 展開 MMIO/DMA 微流程
    - 回報 done/error

### 12.2 載入流程（標準順序）

1. Loader 讀檔與檢查：`magic/version/target_caps`。
2. Loader 分配並搬運 section 到目標位址。
3. Loader 寫入 `HACC_JOB_BASE_LO/HI`、`HACC_WAVE_BEGIN`、`HACC_WAVE_COUNT`。
4. Loader 設定 `PC = SEC_CORE_STUB.entry` 並釋放 core reset。
5. Core 執行：`HCFG -> HLAUNCH -> HKICK -> HWAIT`。
6. done：回報完成；error：讀 `HERR` 後 `HABORT`。

### 12.3 載入責任邊界（必須遵守）

1. HACC Core **不解析 `.hap`**。
2. HACC Core **不負責大量 section 搬運**（避免控制面膨脹）。
3. DFU 只讀 descriptor table，不承擔 package parser 功能。

---

## 13. readable program text file 格式（Native ISA 版）

### 13.1 檔案結構

```text
program <name> {
   meta { ... }
   hardware { ... }
   grouping { ... }
   tensors { ... }
   pe_programs { ... }
   scan_chain { ... }
   hddu_profiles { ... }
   dma_plan { ... }
   wave_graph { ... }
   runtime_policy { ... }
   launch_plan { ... }   # optional
}
```

### 13.2 區塊語意

1. `meta`：版本、目標平台、cluster_count、deploy_mask。
2. `hardware`：cluster stride/base、dma base、硬體限制參數。
3. `grouping`：cluster 分群別名。
4. `tensors`：邏輯 tensor 到記憶體映射。
5. `pe_programs`：PE 程式來源與部署範圍。
6. `scan_chain`：scan chain word 序列。
7. `hddu_profiles`：profile 靜態/動態欄位。
8. `dma_plan`：DMA descriptor 定義。
9. `wave_graph`：wave 資料依賴與執行策略。
10. `runtime_policy`：timeout/retry/error_action。
11. `launch_plan`（可選）：切批策略（每批 wave 數、重試策略）。

### 13.3 最小欄位約束

1. `cluster_count`, `deploy_mask`, `cluster_stride`, `dma_base` 必填。
2. `wave_graph` 每筆必含 `cluster_mask/profile_id/pe_program_id/prefetch/drain/sync_policy`。
3. `runtime_policy.timeout_cycle` 必填。

---

## 14. readable text file 的編譯與轉換流程（Native ISA 版）

### 14.1 編譯管線

```text
.hacc text
   -> Parser(AST)
   -> Semantic Check
   -> Wave DAG + Schedule IR
   -> Lowering
          - SEC_PE_PROGRAM
          - SEC_SCAN_CHAIN
          - SEC_PROFILE_TABLE
          - SEC_DMA_DESC
          - SEC_WAVE_DESC
          - SEC_JOB_CONFIG
   -> Core Stub Emit (HACC Native ISA)
   -> Packager (.hap)
```

### 14.2 Semantic Check 必做項目

1. `deploy_mask` 不超出 `cluster_count`。
2. grouping 不重疊（除非明確允許重疊策略）。
3. wave DAG 無循環。
4. DMA bytes 覆蓋 tensor 需求。
5. profile、pe_program、dma descriptor 引用合法。
6. `sync_policy` 與 `target_caps` 相容。

### 14.3 主要輸出 Section

1. `SEC_CORE_STUB`
2. `SEC_JOB_CONFIG`
3. `SEC_WAVE_DESC`
4. `SEC_PROFILE_TABLE`
5. `SEC_DMA_DESC`
6. `SEC_PE_PROGRAM`
7. `SEC_SCAN_CHAIN`
8. `SEC_DEBUG_MAP`

---

## 15. readable text file 範例（Native ISA 導向）

### 15.1 Conv2D 範例（節錄）

```text
program conv2d_k3c4 {
   meta { version="1.0" target="sim" cluster_count=2 deploy_mask=0b0011 }
   hardware { cluster_stride=0x00010000 dma_base=0x80000000 }

   grouping {
      group g_conv clusters=[0,1]
   }

   pe_programs {
      pe_program conv_main {
         format="pe-asm-v1"
         deploy={group:g_conv, pe_range:"0..47"}
         source_file="pe/conv_main.peasm"
      }
   }

   scan_chain {
      order=reverse
      words=[0x40100208,0x40100208,0x40100208,0x40100208]
   }

   hddu_profiles {
      profile conv_wave { plane_en=0xF plane_mode=0x1 max_outstanding=16 }
   }

   dma_plan {
      desc load_w   { ch=0 src="dram://W.bin"  dst=0x00002000 bytes=0x9000 }
      desc load_x0  { ch=1 src="dram://X0.bin" dst=0x00020000 bytes=0x24000 }
      desc store_y0 { ch=2 src=0x00070000 dst="dram://Y0.bin" bytes=0x80000 }
   }

   wave_graph {
      wave 0 {
         cluster_mask=0b0011
         profile_id=conv_wave
         pe_program_id=conv_main
         prefetch=[load_w,load_x0]
         drain=[store_y0]
         sync_policy="wait_cluster_then_drain"
      }
   }

   runtime_policy { timeout_cycle=1000000 retry_max=0 error_action=abort }
}
```

### 15.2 GEMM 範例（節錄）

```text
program gemm_m128_n128_k64 {
   meta { version="1.0" target="sim" cluster_count=4 deploy_mask=0b1111 }
   hardware { cluster_stride=0x00010000 dma_base=0x80000000 }

   grouping {
      group g_all clusters=[0,1,2,3]
   }

   pe_programs {
      pe_program gemm_main {
         format="pe-asm-v1"
         deploy={group:g_all, pe_range:"0..47"}
         source_file="pe/gemm_main.peasm"
      }
   }

   hddu_profiles {
      profile gemm_wave { plane_en=0xB plane_mode=0x2 max_outstanding=16 }
   }

   dma_plan {
      desc load_a0  { ch=0 src="dram://A0.bin" dst=0x00010000 bytes=0x2000 }
      desc load_b0  { ch=1 src="dram://B0.bin" dst=0x00020000 bytes=0x2000 }
      desc store_c0 { ch=2 src=0x00040000 dst="dram://C0.bin" bytes=0x10000 }
   }

   wave_graph {
      wave 0 {
         cluster_mask=0b1111
         profile_id=gemm_wave
         pe_program_id=gemm_main
         prefetch=[load_a0,load_b0]
         drain=[store_c0]
         sync_policy="wait_both"
      }
   }

   runtime_policy { timeout_cycle=1000000 retry_max=1 error_action=abort }
}
```

---

## 16. 兩個範例經 core-compiler 後的預期 Core ASM（完整 Core Flow）

> 本規格為 Native ISA + descriptor-driven，因此「完整 core flow」在 Core ASM 中應保持精簡；per-wave 細節由硬體單元展開。

### 16.1 Conv2D 預期 Core ASM

```asm
# core_stub.conv2d.native.s
# r1=job_cfg_ptr, r2=wave_begin, r3=wave_count, r4=wait_mask, r5=status, r6=tmp

start:
   MOVI   r1, JOBCFG_CONV_PTR
   HCFG   r1

   MOVI   r2, 0
   MOVI   r3, CONV_WAVE_COUNT
   HLAUNCH r2, r3
   HKICK

wait_loop:
   MOVI   r4, 0b011
   HWAIT  r4
   HPOLL  r5

   MOVI   r6, 0x4
   AND    r5, r5, r6
   CMP    r5, r0
   BRZ    finish

error:
   HERR   r6
   HABORT
   HBREAK

finish:
   HBREAK
```

### 16.2 GEMM 預期 Core ASM

```asm
# core_stub.gemm.native.s

start:
   MOVI   r1, JOBCFG_GEMM_PTR
   HCFG   r1

   MOVI   r2, 0
   MOVI   r3, GEMM_WAVE_COUNT
   HLAUNCH r2, r3
   HKICK

wait_loop:
   MOVI   r4, 0b011
   HWAIT  r4
   HPOLL  r5

   MOVI   r6, 0x4
   AND    r5, r5, r6
   CMP    r5, r0
   BRZ    finish

error:
   HERR   r6
   HABORT
   HBREAK

finish:
   HBREAK
```

### 16.3 對應關係說明

1. Conv2D/GEMM 差異主要存在 `JOB_CONFIG`、`WAVE_DESC`、`PROFILE_TABLE`、`DMA_DESC` 內容。
2. Core ASM 模板可共用，僅更換 `JOBCFG_*_PTR` 與 `*_WAVE_COUNT`。
3. 這是本規格的核心目標：以資料差異取代指令膨脹。

---

## 17. 總結

這份規格將 Core Controller 明確定義為 **HACC Native ISA 控制前端**，不再依賴 RISC-V 指令語意；同時把主要複雜度放在可觀測的硬體單元（DFU/WSU/PAU/CBU/DRU/STU/MSU/EIU）。

關鍵成果：
1. ISA 可獨立實作（含編碼、狀態、例外）。
2. 每個客製化 Unit 皆有明確功能與狀態行為。
3. 能以 descriptor 驅動大規模 wave，維持高吞吐與可維護性。
