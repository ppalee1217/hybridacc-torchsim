# HybridAcc Core Controller 規格（RV32I Base + XHACC 擴充）

> 目標：將原本自定義 Core-ISA 收斂為 **RV32I 標準指令集**，僅針對 HybridAcc 的 cluster/MMIO/DMA/wave 控制加入少量擴充（XHACC）。
>
> 核心方向：**通用控制邏輯走 RV32I**、**裝置控制走 MMIO**、**高頻控制路徑以少量擴充指令加速**。

---

## 0. 設計目標與邊界

### 0.1 問題背景

目前系統可執行單一 wave 的 cluster 控制，但若要達成高吞吐、連續 wave pipeline（prefetch/compute/drain 重疊），需要更標準、可維護的控制 ISA。

既有自定義 ISA 雖可用，但在以下面向成本較高：
1. 工具鏈不通用（assembler/debugger/反組譯工具需要客製）
2. 程式語意與 ABI 不易重用
3. 長期維護與多人協作成本高

### 0.2 本版策略

1. **Core ISA 採 RV32I**（最小可行，先不依賴 M/F/D/C）
2. **裝置互動維持 MMIO**（cluster/HDDU/DMA 以 load/store 存取）
3. **僅加少量 XHACC 擴充**，處理 broadcast、wave、DMA range 這類高頻控制路徑
4. **保留現有硬體位址語意**（不改 ComputeCluster/HDDU/DMA register map）
5. **程式封裝維持 section 化**（header / core / pe / scan-chain / dma / wave）

### 0.3 非目標

1. 不改 PE ALU ISA
2. 不改 HDDU/AGU 寄存器欄位定義
3. 不強制一次導入完整作業系統或完整 RISC-V 特權層

---

## 1. 架構總覽：RV32I + XHACC

### 1.1 執行模型

- 單一 hart，in-order（M-mode only 即可）
- 32-bit 指令寬度，little-endian
- 指令來源：`SEC_CORE_PROGRAM`（可為裸機 ELF 或純 binary）
- 裝置透過 memory-mapped I/O 操作：
  - Cluster MMIO（SPM/HDDU/NoC）
  - DMA MMIO
  - （可選）Descriptor memory window

### 1.2 軟硬體分工

#### 軟體（RV32I 程式）負責

1. loop/control-flow、條件分支、錯誤處理
2. 依 wave descriptor 發送控制命令
3. timeout / retry / trap policy

#### 硬體（XHACC 擴充）負責（可選）

1. cluster mask broadcast 加速
2. DMA descriptor range 啟動
3. wave execute micro-sequencing

> 原則：即使沒有 XHACC，純 RV32I + MMIO 仍可完整執行（只是 code size 與執行開銷較高）。

---

## 2. 位址模型與 MMIO 版圖

### 2.1 Cluster 位址模型

```text
Global MMIO Address = CLUSTER_MMIO_BASE + CLUSTER_STRIDE * cluster_id + local_mmio_addr
```

建議常數：
- `CLUSTER_MMIO_BASE = 0x0000_0000`
- `CLUSTER_STRIDE    = 0x0001_0000`
- `cluster_id        = 0 .. cluster_count-1`

Cluster local window（沿用現有文件）：
- `0x0000 ~ 0x00FF`：SPM config
- `0x1000 ~ 0x1FFF`：HDDU passthrough
- `0x2000 ~ 0x20FF`：NoC command

### 2.2 DMA 位址模型

- `DMA_BASE      = 0x8000_0000`
- `DMA_CH_STRIDE = 0x0000_0100`

建議 MMIO：
- `DMA_CHn_CTRL`
- `DMA_CHn_STATUS`
- `DMA_CHn_DESC_LO/HI`
- `DMA_INT_STATUS / DMA_INT_CLR`

### 2.3 Descriptor 區域（建議）

- `WAVE_DESC_BASE`：wave 描述表
- `PROFILE_TABLE_BASE`：HDDU profile 表
- `DMA_DESC_BASE`：DMA descriptor 表

描述表可放在共享 memory，由 core 以普通 `lw/sw` 存取。

---

## 3. RV32I 基準與 ABI

### 3.1 RV32I 必備指令群

至少需支援：
1. 算術/邏輯：`add/sub/and/or/xor/sll/srl/sra` + immediate 版本
2. 控制流：`jal/jalr/beq/bne/blt/bge/bltu/bgeu`
3. 記憶體：`lb/lh/lw/sb/sh/sw`
4. 上下文：`lui/auipc`
5. 系統：`ecall/ebreak`（可用於 debug/trap）

### 3.2 暫存器與呼叫慣例

採用標準 RISC-V ABI：
- `x0`：zero
- `ra(x1)`：return address
- `sp(x2)`：stack pointer
- `gp/tp`：可選
- `t0~t6`：caller-saved
- `s0~s11`：callee-saved（`s0` 兼 fp）
- `a0~a7`：參數/回傳

### 3.3 CSR（最小需求）

MVP 建議：
1. `mstatus`
2. `mie/mip`
3. `mtvec`
4. `mepc/mcause/mtval`

若不做完整中斷，也至少保留 trap 入口處理 timeout/error。

---

## 4. XHACC 擴充指令（少量）

本章定義「可選但強烈建議」的最小擴充。若硬體暫未實作，可由 assembler/compiler 以 RV32I + MMIO script 展開。

### 4.1 擴充設計原則

1. 不取代 RV32I，一律是加速器友善語法糖
2. 每條擴充都必須有可等價展開路徑
3. opcode 使用 RISC-V custom 區間（`custom-0..3`）

### 4.2 指令清單（XHACC v1）

#### A) Cluster Mask / Broadcast

1. `hacc.clset rd, rs1`
   - 語意：`CLMASK[rd] = rs1`
   - 用途：設定 cluster mask 暫存器（硬體影子寄存）

2. `hacc.clw rs1, rs2, imm12`
   - 語意：對 `mask_id=rs1` 指向的 cluster mask，將 `rs2` 寫入 `local_mmio=imm12`
   - 等價展開：for each cluster in mask -> `sw`

3. `hacc.clwi rs1, imm12, imm32`（pseudo）
   - 實作可展開成 `li t0, imm32; hacc.clw rs1, t0, imm12`

#### B) DMA Range

4. `hacc.dstart rs1, rs2`
   - 語意：啟動 `dma_desc_id = rs2` 於 channel=`rs1`

5. `hacc.drange rs1, rs2, rs3`
   - 語意：啟動 descriptor `[begin=rs2, count=rs3]`，channel policy 由 desc 自帶或全域設定

6. `hacc.dwait rs1`
   - 語意：等待 channel mask `rs1` 完成（含 error 返回狀態）

#### C) Wave

7. `hacc.wload rs1`
   - 語意：載入 `WaveDesc* = rs1` 到 wave shadow register

8. `hacc.wexec`
   - 語意：執行當前 wave（prefetch -> apply -> run -> sync -> drain）

9. `hacc.wnext`
   - 語意：`wave_idx++` 並刷新必要動態欄位

### 4.3 MVP 實作順序

1. 先做 `hacc.clw` + `hacc.drange`
2. 再做 `hacc.wload/wexec/wnext`
3. `hacc.wexec` 初版可不做 profile cache（固定 full write）

---

## 5. 指令語意與一致性規範

### 5.1 MMIO 可見性

1. `sw` 到 MMIO 預設視為 posted write
2. 對同目標做 `lw` 時，需隱含先前 write 的可見性同步
3. 需要全域順序時，使用 `fence iorw, iorw`

### 5.2 DMA 語意

1. `hacc.dstart/drange` 只保證 descriptor 被接受
2. 完成判定須由 `hacc.dwait` 或輪詢 `DMA_STATUS`
3. error 必須可回報至 trap 或狀態碼

### 5.3 中斷/等待

可選兩種模式：
1. **polling mode**：核心輪詢狀態位元
2. **irq mode**：`wfi` 等待 `cluster_done/dma_done/error`

MVP 可先實作 polling，保留 IRQ 擴充。

---

## 6. `.hacc` 可讀格式（RV32I 導向）

本章維持原本 `.hacc` 設計精神，但 `core_flow` 改為「可 lowering 到 RV32I（+XHACC）」。

### 6.1 檔案結構

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
  temporal_waves { ... }
  core_flow { ... }   ; optional
}
```

### 6.2 必填欄位

1. `cluster_count`
2. `deploy_mask`
3. `cluster_stride`
4. `dma_base`

### 6.3 `core_flow` 的 RV32I lowering 原則

1. 無 `core_flow`：compiler 自動生成 wave loop 範本
2. 有 `core_flow`：先轉高階 IR，再產生 RV32I block
3. `hddu_apply/pe_start/dma_start/wait_*` 先轉 runtime API call，再由 backend 決定：
   - 純 RV32I + MMIO
   - RV32I + XHACC 指令

---

## 7. Compiler/Packager 流程（更新版）

### 7.1 編譯管線

```text
.hacc
  -> Parser(AST)
  -> Semantic Check
  -> IR (typed + scheduled)
  -> Lowering
       - PE asm      -> pe_program.bin
       - core flow   -> RV32I asm / object
       - wave table  -> SEC_WAVE_DESC
       - dma plan    -> SEC_DMA_DESC
       - profile     -> SEC_PROFILE_TABLE
  -> Link / Pack
       - .hap (or ELF + side sections)
```

### 7.2 Semantic Check 必做項目

1. `deploy_mask` 不可超出 `cluster_count`
2. grouping 不可重疊 cluster
3. AGU base/stride 不可越界 SPM
4. scan-chain words 數量符合硬體需求
5. wave 所需 DMA 覆蓋 tensor bytes
6. PE deploy 範圍不可越界

### 7.3 IR 結構（建議）

```cpp
struct DeploymentInfo {
  uint32_t cluster_count;
  uint32_t deploy_mask;
  uint32_t cluster_stride;
};

struct WaveDesc {
  uint32_t cluster_mask;
  uint16_t profile_id;
  uint16_t pe_program_id;
  uint16_t prefetch_begin;
  uint16_t prefetch_count;
  uint16_t drain_begin;
  uint16_t drain_count;
  uint8_t  sync_policy;
  uint8_t  flags;
  uint16_t reserved;
};
```

---

## 8. 封裝格式（HAP / ELF 混合）

### 8.1 Header

```cpp
struct CorePkgHeader {
  uint32_t magic;          // 'HACP'
  uint16_t version_major;
  uint16_t version_minor;
  uint32_t section_count;
  uint32_t cluster_count;
  uint32_t deploy_mask;
  uint32_t entry_section;  // 通常 CORE_PROGRAM
  uint32_t flags;          // bit0: little-endian
};
```

### 8.2 Section Type（更新）

```cpp
enum SectionType : uint32_t {
  SEC_META           = 0,
  SEC_CORE_PROGRAM   = 1,   // RV32I text/data or ELF payload
  SEC_PE_PROGRAM     = 2,
  SEC_SCAN_CHAIN     = 3,
  SEC_CLUSTER_CFG    = 4,
  SEC_DMA_DESC       = 5,
  SEC_WAVE_DESC      = 6,
  SEC_PROFILE_TABLE  = 7,
  SEC_SYMBOL         = 8,
  SEC_DEBUG_MAP      = 9
};
```

### 8.3 Loader 執行順序

1. 驗證 header/section
2. 檢查 `cluster_count` 與目標平台資源
3. 套用 `SEC_CLUSTER_CFG` 靜態設定
4. 載入 `SEC_PE_PROGRAM` + `SEC_SCAN_CHAIN`
5. 載入 `SEC_DMA_DESC` + `SEC_WAVE_DESC` + `SEC_PROFILE_TABLE`
6. 啟動 `SEC_CORE_PROGRAM`（RV32I entry）

---

## 9. Core 程式範本（RV32I 風格）

以下展示「不展開每個 wave 指令碼」的核心模式。

### 9.1 Wave Loop 範本

```asm
  li   s0, 0                  # wave_idx
  li   s1, WAVE_COUNT
  la   s2, wave_desc_base

wave_loop:
  beq  s0, s1, finish

  li   t0, WAVE_DESC_SIZE
  mul  t1, s0, t0             # 若無 M extension，改成 shift/add sequence
  add  t2, s2, t1             # t2 = desc_ptr

  # option A: 純 RV32I runtime call
  mv   a0, t2
  jal  ra, run_one_wave

  # option B: XHACC
  # hacc.wload t2
  # hacc.wexec
  # hacc.wnext

  addi s0, s0, 1
  j    wave_loop

finish:
  ebreak
```

### 9.2 `run_one_wave`（純 RV32I + MMIO）

```asm
run_one_wave:
  # a0 = desc_ptr
  # 1) 讀 desc
  # 2) DMA prefetch
  # 3) apply profile (full or delta)
  # 4) PE start + HDDU start
  # 5) sync policy
  # 6) DMA drain
  ret
```

> 若啟用 XHACC，可把 2~6 收斂為一到三條擴充指令。

---

## 10. 大量 Wave（數千～數萬）策略

### 10.1 關鍵結論

不要展開每波 core asm。改成：

1. 小型固定 RV32I 控制程式（O(1) code size）
2. 大型 wave 描述資料（O(Nwave) data size）
3. profile/delta 更新減少 MMIO transaction

### 10.2 Delta Profile 策略

將 profile 拆成：
1. static part：iter/stride/tag_ctrl/mask
2. dynamic part：base_addr/tag_base

首波 full write，後續波只更新 dynamic 欄位。

### 10.3 進一步壓縮

1. `SEC_WAVE_DESC` 做 RLE / delta encoding
2. 規律 workload 以 nest-loop descriptor 取代 flat table
3. 以公式 runtime 計算 base：

$$
base = base_0 + oc_t\Delta_{oc} + ic_t\Delta_{ic} + h_t\Delta_h + w_t\Delta_w
$$

---

## 11. 相容性與遷移計畫

### 11.1 舊版自定義 ISA -> RV32I 對照

1. `CALL/RET` -> `jal/jalr`
2. `BEQ/BNE/...` -> RV32I branch
3. `MMIO.W/MMIO.R` -> `sw/lw`
4. `CL.MMIO.*` -> `for each cluster` 展開，或 `hacc.clw`
5. `DMA.START/WAIT` -> MMIO script，或 `hacc.dstart/dwait`
6. `WAVE.*` -> runtime function，或 `hacc.w*`

### 11.2 落地步驟（建議）

1. **Phase 1**：先把 compiler backend 改成輸出 RV32I asm（不依賴 XHACC）
2. **Phase 2**：加入 `hacc.clw` 與 `hacc.drange` 兩條擴充
3. **Phase 3**：導入 `hacc.wload/wexec/wnext`
4. **Phase 4**：加入 profile cache + delta MMIO 最佳化

---

## 12. MVP 規格（可直接開工）

### 必做

1. RV32I core 可執行基本 loop/branch/load/store
2. MMIO 可操作 cluster/HDDU/DMA
3. `.hacc -> SEC_WAVE_DESC + SEC_DMA_DESC + RV32I core program`
4. loader 可載入並啟動 core

### 可選但建議

1. XHACC：`hacc.clw`, `hacc.drange`
2. polling + timeout trap
3. debug map（wave idx 對應 source）

### 第二版

1. `hacc.wexec`
2. IRQ + `wfi`
3. descriptor cache / profile cache

---

## 13. 與既有文件對齊

本文件不改變既有硬體寄存器語意，只改 Core Controller 指令與軟體模型：

1. AGU/HDDU register 欄位：對齊 `doc/AGU.md`、`doc/HDDU.md`
2. Cluster MMIO window：對齊 `doc/ComputeCluster.md`
3. SPM 行為：對齊 `doc/SPM.md`

新增的是：
1. RV32I-based core programming model
2. XHACC minimal extension set
3. wave descriptor 導向的 compiler/packaging/runtime 流程

---

## 14. 總結

將 Core-ISA 改為 RV32I + 少量 XHACC 擴充，可同時達成：

1. 工具鏈標準化（assembler/debugger/ELF 生態）
2. 與既有 MMIO 架構相容
3. 適合數千至數萬 wave 的可擴充執行模型
4. 保留硬體加速空間（broadcast/range/wave macro）

最重要的架構決策是：

**固定小型 RV32I 控制程式 + 可壓縮 wave/profile/dma 描述資料**，而不是展開每一波的巨量 core asm。

---

## 15. 附錄 A：Conv2D RV32I Core ASM 範本（可給 compiler 產生）

> 對應：`conv2d_k3c4.hacc`。
>
> 目的：展示「小型迴圈 + 描述表」的 RV32I 程式骨架，不展開每一波 MMIO 寫入。

### 15.1 符號假設

```asm
  .equ WAVE_DESC_SIZE,      16
  .equ WAVE_SYNC_WAIT_CL,   0
  .equ WAVE_SYNC_WAIT_BOTH, 1
  .equ WAVE_SYNC_OVERLAP,   2

  # wave desc offsets
  .equ OFF_CL_MASK,         0
  .equ OFF_PROFILE_PE,      4
  .equ OFF_PREFETCH,        8
  .equ OFF_DRAIN_SYNC,      12
```

### 15.2 主程式骨架

```asm
  .section .text
  .globl _start

_start:
  # init: SPM map / scan chain / PE program
  jal  ra, conv_init

  li   s0, 0                      # wave_idx
  la   s1, conv_wave_table
  lw   s2, conv_wave_count        # wave_count

conv_wave_loop:
  beq  s0, s2, conv_finish

  slli t0, s0, 4                  # *16 bytes
  add  a0, s1, t0                 # a0 = desc_ptr
  jal  ra, run_one_wave_conv

  addi s0, s0, 1
  j    conv_wave_loop

conv_finish:
  ebreak
```

### 15.3 `run_one_wave_conv`（純 RV32I + MMIO runtime）

```asm
run_one_wave_conv:
  # a0 = &WaveDesc
  # decode fields
  lw   t0, OFF_CL_MASK(a0)        # cluster_mask
  lw   t1, OFF_PROFILE_PE(a0)     # [15:0]=profile_id, [31:16]=pe_program_id
  lw   t2, OFF_PREFETCH(a0)       # [15:0]=pref_begin, [31:16]=pref_count
  lw   t3, OFF_DRAIN_SYNC(a0)     # [15:0]=drain_begin, [23:16]=drain_count, [31:24]=sync

  # 1) DMA prefetch range
  mv   a1, t2
  jal  ra, dma_prefetch_range

  # 2) apply profile (full/delta)
  mv   a1, t0
  mv   a2, t1
  jal  ra, hddu_apply_profile

  # 3) start PE + HDDU
  mv   a1, t0
  jal  ra, start_pe_and_hddu

  # 4) sync policy + drain
  mv   a1, t0
  mv   a2, t3
  jal  ra, sync_and_drain
  ret
```

### 15.4 若啟用 XHACC 的等價替代

```asm
  # 在 wave loop 內可直接改成：
  #   hacc.wload a0
  #   hacc.wexec
  #   hacc.wnext
```

---

## 16. 附錄 B：GEMM RV32I Core ASM 範本（可給 compiler 產生）

> 對應：`gemm_m128_n128_k64.hacc`。

### 16.1 主程式骨架

```asm
  .section .text
  .globl _start_gemm

_start_gemm:
  jal  ra, gemm_init

  li   s0, 0
  la   s1, gemm_wave_table
  lw   s2, gemm_wave_count

gemm_wave_loop:
  beq  s0, s2, gemm_finish

  slli t0, s0, 4
  add  a0, s1, t0
  jal  ra, run_one_wave_gemm

  addi s0, s0, 1
  j    gemm_wave_loop

gemm_finish:
  ebreak
```

### 16.2 `run_one_wave_gemm`（偏向 overlap policy）

```asm
run_one_wave_gemm:
  lw   t0, OFF_CL_MASK(a0)
  lw   t1, OFF_PROFILE_PE(a0)
  lw   t2, OFF_PREFETCH(a0)
  lw   t3, OFF_DRAIN_SYNC(a0)

  # prefetch A/B
  mv   a1, t2
  jal  ra, dma_prefetch_range

  # apply profile + start compute
  mv   a1, t0
  mv   a2, t1
  jal  ra, hddu_apply_profile

  mv   a1, t0
  jal  ra, start_pe_and_hddu

  # wait both / overlap 由 desc.sync_policy 決定
  mv   a1, t0
  mv   a2, t3
  jal  ra, sync_and_drain
  ret
```

### 16.3 建議 runtime API（給 compiler lowering target）

```c
void conv_init(void);
void gemm_init(void);

void dma_prefetch_range(uint32_t packed_begin_count);
void hddu_apply_profile(uint32_t cluster_mask, uint32_t packed_profile_pe);
void start_pe_and_hddu(uint32_t cluster_mask);
void sync_and_drain(uint32_t cluster_mask, uint32_t packed_drain_sync);
```

> 上述 API 在 backend 可有兩個實作：
> 1) 純 RV32I + MMIO；
> 2) RV32I + XHACC（以 custom opcode 內聯）。

---

## 17. 附錄 C：XHACC 客製化硬體模組規格與 RV32I CPU 組織

> 本章定義 XHACC 在 RTL 層級的最小可實作規格（MVP+），並說明如何掛接到 RV32I pipeline。

### 17.1 設計目標

1. 保持 RV32I 核心通用性，不破壞標準指令流程。
2. 將高頻控制操作（cluster broadcast / DMA range / wave execute）下沉到硬體狀態機。
3. 所有 XHACC 指令都可回退為純 RV32I + MMIO（功能等價）。

### 17.2 模組分解（MVP）

#### A) XHACC Decoder

- 輸入：ID stage instruction（opcode/funct3/funct7/rs1/rs2/rd/imm）
- 功能：辨識 `custom-0..3` 區間，輸出 `xhacc_uop`
- 輸出欄位建議：
  - `uop_kind`（CLSET/CLW/DSTART/DRANGE/DWAIT/WLOAD/WEXEC/WNEXT）
  - `src0/src1/imm/rd`

#### B) XHACC Command FIFO + Scoreboard

- 功能：
  1. 解耦 CPU issue 與後端 MMIO/DMA 延遲
  2. 追蹤 inflight command
  3. 提供 `busy/done/error`
- 建議深度：4~8 entries（MVP）

#### C) Cluster Broadcast Engine

- 功能：把單一 `hacc.clw` 展開成多筆 cluster MMIO write
- 內部狀態：
  - `mask_bank[0..3]`（每組 32-bit cluster mask）
  - `iter_cluster_id`
- 完成條件：mask 中所有 cluster 寫入成功或 timeout/error

#### D) DMA Range Engine

- 功能：
  1. 依 `begin/count` 讀 `SEC_DMA_DESC`
  2. 發送 `DMA.START`
  3. 維護 channel inflight counter
- 建議支援：
  - `hacc.dstart`（單筆）
  - `hacc.drange`（連續區間）
  - `hacc.dwait`（channel mask 完成等待）

#### E) Wave Engine

- 功能：
  1. `hacc.wload` 載入 WaveDesc 到 shadow registers
  2. `hacc.wexec` 依 policy 執行 prefetch/apply/start/sync/drain
  3. `hacc.wnext` 更新 wave index 與動態欄位
- 建議子單元：
  - `Profile Apply Unit`（初版 full write）
  - `Sync Policy Unit`（wait_cluster_then_drain / wait_both / overlap）

#### F) MMIO Sequencer

- 功能：
  - 將 XHACC uop 轉換成有序 MMIO transaction
  - 控制 posted write drain 與 read-after-write 可見性
- 規則：
  - `lw`/poll 前需確保同目標 write 可見
  - `fence iorw, iorw` 需等待 sequencer 清空

#### G) Sync/IRQ Aggregator

- 來源：`cluster_done/error`、`dma_done/error`、timeout watchdog
- 輸出：
  - `xhacc_irq`
  - `xhacc_error_code`
  - trap request（可選）

### 17.3 與 RV32I Pipeline 的組織方式

#### 建議 CPU 架構

- 基本：5-stage in-order（IF/ID/EX/MEM/WB）
- XHACC 接點：ID/EX 之間加入 `XHACC issue port`

#### 指令流程

1. ID stage 發現 custom opcode -> 送 `xhacc_req`
2. 若 `xhacc_req_ready=1`：
   - non-blocking uop（如 `clset/clw/drange`）可繼續 retire
   - blocking uop（如 `dwait/wexec`）進入 stall until done
3. 若 `xhacc_req_ready=0`：ID stage backpressure（stall）

#### blocking / non-blocking 建議

- non-blocking：`clset`, `clw`, `dstart`, `drange`, `wload`, `wnext`
- blocking：`dwait`, `wexec`

> 若要簡化硬體，MVP 可先全部 blocking，再逐步放寬。

### 17.4 CPU-XHACC 介面訊號規格（建議）

```text
cpu -> xhacc
  req_valid
  req_ready
  req_uop[7:0]
  req_rs1[31:0]
  req_rs2[31:0]
  req_imm[31:0]
  req_rd[4:0]

xhacc -> cpu
  rsp_valid
  rsp_ready
  rsp_rd_we
  rsp_rd[4:0]
  rsp_data[31:0]
  xhacc_busy
  xhacc_irq
  xhacc_error
```

```text
xhacc -> mmio bus
  mmio_req_valid / mmio_req_ready
  mmio_addr[31:0]
  mmio_wdata[31:0]
  mmio_we
  mmio_rdata[31:0]
  mmio_rsp_valid
```

```text
xhacc <-> descriptor memory
  desc_req_valid / desc_req_ready
  desc_addr[31:0]
  desc_rdata[31:0]
  desc_rsp_valid
```

### 17.5 CSR/狀態映射（建議）

建議以 memory-mapped CSR 或 custom CSR 暴露：

1. `XHACC_STATUS`
   - bit0: busy
   - bit1: error
   - bit2: irq_pending
2. `XHACC_ERRCODE`
   - [7:0] error class
   - [31:8] aux（desc_id/cluster_id/channel）
3. `XHACC_WIDX`
   - 當前 wave index
4. `XHACC_PERF_*`（可選）
   - mmio_count / dma_count / stall_cycle

### 17.6 錯誤模型與 Trap 行為

#### 錯誤來源

1. MMIO timeout
2. DMA channel error
3. illegal descriptor（越界/格式錯誤）
4. wave policy 非法組合

#### 行為建議

1. 設定 `XHACC_STATUS.error=1`
2. 寫入 `XHACC_ERRCODE`
3. 觸發 `xhacc_irq` 或直接 raise machine trap
4. trap handler 讀狀態後決定 retry/abort

### 17.7 一致性與排序規範

1. `hacc.clw` 對單一指令內展開的多 cluster 寫入，需保序完成。
2. `hacc.wexec` 內部步驟需遵循：prefetch -> apply -> start -> sync -> drain。
3. `fence iorw, iorw` 前所有 XHACC 發出的 MMIO 交易必須完成。
4. `hacc.dwait` 返回時，對應 channel inflight 必須歸零，且 error 已可見。

### 17.8 MVP RTL 實作順序

#### Phase A（最小可用）

1. Decoder + FIFO + Broadcast Engine
2. `clset/clw` 指令可用

#### Phase B（DMA）

1. DMA Range Engine
2. `dstart/drange/dwait` 可用

#### Phase C（Wave）

1. Wave Engine（full profile write）
2. `wload/wexec/wnext` 可用

#### Phase D（優化）

1. profile delta writer
2. descriptor prefetch cache
3. IRQ + `wfi` 深度整合

### 17.9 與軟體工具鏈對接重點

1. assembler：支援 custom opcode 與 fallback pseudo 展開。
2. compiler backend：
   - 有 XHACC 時優先輸出 custom 指令
   - 無 XHACC 時轉 runtime MMIO call
3. simulator：
   - 必須可追蹤每筆 XHACC uop 的展開 MMIO/DMA 行為
   - 提供 wave 級別 trace（wave_idx / desc_id / sync_policy）

> 結論：XHACC 應被視為 RV32I 的「協同控制引擎」，而非取代 CPU 的第二套 ISA。這樣可同時保留標準工具鏈與硬體加速能力。
