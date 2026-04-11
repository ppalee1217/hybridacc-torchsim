# HybridAcc CoreMCU Firmware Test Report

## 概述

本報告記錄 `cc_core_mcu` 5-stage RISC-V pipeline (RV32I\_Zmmul\_Zicsr) 的韌體驗證結果，涵蓋基本指令、記憶體存取、CSR、pipeline hazard、中斷、MMIO 外設操作以及複合指令序列。

**Toolchain**: `riscv32-unknown-elf-gcc -march=rv32im_zicsr -mabi=ilp32 -Os -nostdlib -ffreestanding`

---

## 測試摘要

| 測試名稱 | 測試數 | 通過 | 失敗 | 狀態 | 說明 |
|:---------|:------:|:----:|:----:|:----:|:-----|
| empty | 0 | 0 | 0 | ✅ PASS | EBREAK 自停機制驗證 |
| test\_alu | 28 | 28 | 0 | ✅ PASS | RV32I ALU 所有算術/邏輯指令 |
| test\_branch | 22 | 22 | 0 | ✅ PASS | 條件分支 (BEQ/BNE/BLT/BGE/BLTU/BGEU) |
| test\_compound | 25 | 25 | 0 | ✅ PASS | 排序、memcpy/set、CRC32、矩陣乘法、dot product、GCD |
| test\_csr | 19 | 19 | 0 | ✅ PASS | CSR 讀寫 (Zicsr): mscratch/misa/mtvec/mcycle/minstret/mstatus/mie |
| test\_diag | 2 | 2 | 0 | ✅ PASS | 診斷指令 (stack frame / 回傳值) |
| test\_fabric | 19 | 19 | 0 | ✅ PASS | CmdFabric MMIO 路徑 (LocalCtrl/Timer/DMA) |
| test\_hazard | 15 | 15 | 0 | ✅ PASS | Pipeline data/control hazard (forwarding/stalling) |
| test\_jump | 9 | 9 | 0 | ✅ PASS | JAL / JALR / 函式呼叫 |
| test\_loadstore | 25 | 25 | 0 | ✅ PASS | LW/LH/LB/LHU/LBU/SW/SH/SB + sign/zero extension |
| test\_mul | 19 | 19 | 0 | ✅ PASS | MUL/MULH/MULHSU/MULHU |
| test\_plic | 17 | 17 | 0 | ✅ PASS | PLIC 中斷控制器 (priority/enable/claim/complete) |
| test\_sram\_timing | 3 | 3 | 0 | ✅ PASS | DSRAM store→load 時序 / load-use hazard |
| test\_stack | 15 | 15 | 0 | ✅ PASS | 堆疊操作 / push-pop / 遞迴 (Fibonacci, sum) |
| test\_trap | 10 | 10 | 0 | ✅ PASS | Trap/MRET 流程 / ecall / 中斷向量 |
| test\_dma | 5 | 5 | 0 | ✅ PASS | DMA DRAM↔Cluster SPM 線性搬移 + PLIC 中斷等待 |

**整體**: **16/16 test suites 全數通過，233 assertions 全部成功 (100%)**

---

## RV32I\_Zmmul\_Zicsr 指令覆蓋率

### RV32I 基本指令集 (37/39 = 94.9%)

| 指令 | 覆蓋 | 測試來源 |
|:-----|:----:|:---------|
| `lui` | ✅ | 全部 15 個測試 |
| `auipc` | ✅ | 全部 15 個測試 |
| `jal` | ✅ | 全部（直接 + `j` 偽指令） |
| `jalr` | ✅ | test\_jump（直接 + `jr`），其餘以 `ret` 形式 |
| `beq` | ✅ | test\_branch, test\_compound, test\_diag, test\_hazard |
| `bne` | ✅ | 幾乎全部（直接 + `bnez`） |
| `blt` | ✅ | test\_branch |
| `bge` | ✅ | 幾乎全部（直接 + `bgez`） |
| `bltu` | ✅ | test\_branch, test\_compound, test\_csr, test\_stack |
| `bgeu` | ✅ | test\_branch, test\_csr, test\_fabric, test\_plic |
| `lb` | ✅ | test\_loadstore |
| `lh` | ✅ | test\_loadstore |
| `lw` | ✅ | 多數測試 |
| `lbu` | ✅ | test\_compound, test\_loadstore |
| `lhu` | ✅ | test\_loadstore |
| `sb` | ✅ | test\_compound, test\_loadstore |
| `sh` | ✅ | test\_loadstore |
| `sw` | ✅ | 全部 15 個測試 |
| `addi` | ✅ | 全部（直接 + `li`/`mv`/`nop`） |
| `slti` | ✅ | test\_alu |
| `sltiu` | ✅ | test\_alu（直接 + `seqz`） |
| `xori` | ✅ | test\_alu（直接 + `not`） |
| `ori` | ✅ | test\_alu |
| `andi` | ✅ | test\_alu, test\_compound, test\_csr, test\_fabric, test\_stack |
| `slli` | ✅ | 多數測試 |
| `srli` | ✅ | test\_alu, test\_compound, test\_stack, test\_trap |
| `srai` | ✅ | test\_alu |
| `add` | ✅ | 多數測試 |
| `sub` | ✅ | test\_alu, test\_compound, test\_diag, test\_jump（+ `neg`） |
| `sll` | ✅ | test\_alu |
| `slt` | ✅ | test\_alu |
| `sltu` | ✅ | test\_alu |
| `xor` | ✅ | test\_alu, test\_compound |
| `srl` | ✅ | test\_alu |
| `sra` | ✅ | test\_alu |
| `or` | ✅ | test\_alu |
| `and` | ✅ | test\_alu, test\_compound |
| `fence` | ❌ | 未覆蓋（硬體支援 stall\_fence，但無測試行使） |
| `ecall` | ✅ | test\_trap |
| `ebreak` | ✅ | 全部 15 個測試（停機機制） |

### Zmmul 乘法擴展 (4/4 = 100%)

| 指令 | 覆蓋 | 測試來源 |
|:-----|:----:|:---------|
| `mul` | ✅ | test\_mul, test\_compound, test\_loadstore, test\_stack |
| `mulh` | ✅ | test\_mul |
| `mulhsu` | ✅ | test\_mul |
| `mulhu` | ✅ | test\_mul |

### Zicsr CSR 擴展 (6/6 = 100%)

| 指令 | 覆蓋 | 測試來源 |
|:-----|:----:|:---------|
| `csrrw` | ✅ | test\_csr（直接 + `csrw` 偽指令） |
| `csrrs` | ✅ | test\_csr, test\_trap（`csrr`/`csrs`） |
| `csrrc` | ✅ | test\_csr, test\_trap（`csrc`） |
| `csrrwi` | ✅ | test\_csr |
| `csrrsi` | ✅ | test\_csr（`csrsi`） |
| `csrrci` | ✅ | test\_csr（`csrci`） |

### 特權指令 (1/2 = 50%)

| 指令 | 覆蓋 | 測試來源 |
|:-----|:----:|:---------|
| `mret` | ✅ | test\_trap |
| `wfi` | ❌ | 未覆蓋（硬體支援 stall\_wfi，但無測試行使） |

### 覆蓋率彙總

| 類別 | 覆蓋 / 總數 | 比率 |
|:-----|:----------:|:----:|
| RV32I | 37 / 39 | 94.9% |
| Zmmul | 4 / 4 | 100% |
| Zicsr | 6 / 6 | 100% |
| Privileged | 1 / 2 | 50% |
| **總計** | **48 / 51** | **94.1%** |

**未覆蓋指令 (2)**：`fence`（記憶體屏障）、`wfi`（Wait For Interrupt）— 硬體均已實作對應 stall 邏輯，僅缺少專屬測試韌體。

---

## 已發現並修正的 Bug

### 1. MemoryStage DM\_stall 與 stall\_local 順序問題

**檔案**: `MemoryStage.hpp` `compute_comb()`

**問題**: `stall_local = stall.read() || DM_stall.read()` 讀取了 DM\_stall 自身輸出的舊值。DM\_stall 在同一 SC\_METHOD 末端才被更新，但 SC\_METHOD 不會因自身輸出變化而重新觸發，導致 dm\_valid 已到達時 MEM\_inst\_valid 仍為 false。

**修正**: 將 DM\_stall 的計算移到 `compute_comb()` 最前面，使用 local variable `dm_stall` 供 `stall_local` 讀取。

### 2. stall\_DH 偵測 rs2 使用了錯誤的 control signal

**檔案**: `ExecuteStage.hpp`

**問題**: `dh_rs2 = ... && exe_ctrl[kCtrlRs2] && ...` 檢查的是 EXE 指令（如 LW）的 rs2 使用位元，而 LW 不使用 rs2，因此 rs2 load-use hazard 永遠不會被偵測到。

**修正**: 改為 `ID_controll_sel.read()[kCtrlRs1]` 和 `ID_controll_sel.read()[kCtrlRs2]`，檢查 **ID 階段指令** 的暫存器使用。

### 3. IF 指令讀取資料未暫存

**檔案**: `CoreMcu.hpp`

**問題**: ISRAM 在 `im_valid_reg_` deassert 時輸出 0，導致 pipeline 下一 cycle 收到錯誤的指令資料。

**修正**: 新增 `if_rdata_reg_` 暫存器，在 posedge 捕捉 `if_rdata_i`，`im_DO_int_` 從暫存器驅動。

### 4. DM 讀取資料未暫存

**檔案**: `CoreMcu.hpp`

**問題**: DataSram 讀取透過 sc\_signal 傳遞有 delta-cycle 延遲，導致 load 資料在 MemoryStage 讀取時不穩定。

**修正**: 新增 `dm_rdata_reg_` 暫存器，在 posedge 透過 `sram_read_cb_` 直接讀取 SRAM 陣列。

### 5. DM 位址未做 word-aligned

**檔案**: `CoreMcu.hpp`, `DataSram.hpp`

**問題**: DataSram 的 `comb_rdata_process` 使用 byte 位址存取 sram\_.mem[]，未做 4-byte 對齊。

**修正**: 在 bridge callback 和 DataSram 讀取中使用 `(addr & ~3u) & (SRAM_BYTES - 1)`。

### 6. CSR 模組缺少 mscratch 暫存器

**檔案**: `CSR.hpp`

**問題**: CSR read switch 和 write-back logic 均未處理 `mscratch` (0x340)，CSRW/CSRR mscratch 讀寫無效。

**修正**: 新增 `mscratch_reg_` 訊號與 `mscratch_next_` 變數，read switch 與 write-back 中加入對應 case。

### 7. CSR misa 未實作

**檔案**: `CSR.hpp`

**問題**: CSR read switch 無 `case kCsrMisa:`，讀取 misa 回傳 0，test\_csr T009/T010 失敗。

**修正**: 新增 `case kCsrMisa: csr_rdata = 0x40001100u; break;`（RV32I + M 擴展）。

### 8. CSR mcycle/minstret 不可寫入

**檔案**: `CSR.hpp`

**問題**: CSR write-back logic 只處理 mstatus/mie/mtvec/mepc，寫入 mcycle/minstret 無效。

**修正**: 在 `csr_wb_en` 區段加入 mcycle/mcycleh/minstret/minstreth 的寫入處理，覆蓋 `cycle_next_`/`instret_next_` 的對應 32-bit 半字。

### 9. Data port 無法存取 ISRAM (instruction RAM)

**檔案**: `CoreMcu.hpp`, `CoreController.hpp`, `Isram.hpp`

**問題**: 韌體的 `.rodata` 常數嵌入在 `.text` 段（ISRAM 位址空間 0x0-0x3FFF）。當 `memcpy` 經由 data port 讀取 `.rodata` 時，bridge 將其路由到 MMIO，CmdFabric 無法解碼導致回傳 0，所有依賴常數的測試均失敗。

**修正**: 在 CoreMcu 新增 `isram_read_cb_` 和 `is_isram_range()` 函式，bridge 對 ISRAM 位址使用 ISRAM 讀取回路而非 MMIO。CoreController 安裝 callback 透過 `u_isram.read_byte()` 讀取。

### 10. 韌體 inline assembly 缺少 early-clobber 約束

**檔案**: `test_hazard/main.c`

**問題**: T002/T014/T015 的 inline assembly 輸出用 `=r` 而非 `=&r`，GCC 將輸入與輸出分配到同一暫存器，導致寫入覆蓋尚未讀取的輸入。

**修正**: 將 `=r` 改為 `=&r`（early-clobber constraint）。

### 11. DMA Cluster→DRAM 最後 beat 資料遺失

**檔案**: `Core/DmaEngine.hpp`

**問題**: `CL_RESP_WAIT` 狀態中，當 `bytes_remaining == 0`（最後一個 beat）時，FSM 直接進入 `DONE` 而未將已讀取的 cluster 資料寫入 DRAM，導致 cluster→DRAM 搬移最後 8 bytes 丟失。

**修正**: 在 `bytes_remaining == 0` 時檢查是否為 cluster→DRAM 路徑，若是則先發出 DRAM 寫入（進入 `DRAM_AW_WAIT`），由 `DRAM_B_WAIT` 完成後再進入 `DONE`。

### 12. DMA DRAM 寫入資料未跨 cycle 保留

**檔案**: `Core/DmaEngine.hpp`

**問題**: FSM 主迴圈每 cycle 開頭呼叫 `reset_axi_outputs()` 清除所有 AXI 輸出，但 `DRAM_AW_WAIT` 和 `DRAM_W_WAIT` 狀態未重新設定 `w_data` / `w_strb`，導致 DRAM slave 接收到全零資料。

**修正**: 新增 `dram_wr_data`、`dram_wr_addr`、`dram_wr_strb` 本地變數在 submit 時快照，`DRAM_AW_WAIT` 和 `DRAM_W_WAIT` 狀態從這些本地變數重新驅動 AXI 輸出。

### 13. DMA DRAM_AW_WAIT 使用已推進的目標位址

**檔案**: `Core/DmaEngine.hpp`

**問題**: `DRAM_AW_WAIT` 使用 `dst_addr` 作為 AXI AW 位址，但 `dst_addr` 在 `CL_RESP_WAIT` 中已被推進（`dst_addr += beat`），導致寫入位址偏移一個 beat。

**修正**: 改用 `dram_wr_addr`（在 `CL_RESP_WAIT` 中以 `dst_addr - beat` 快照）。

---

## 修改檔案列表

| 檔案 | 修改內容 |
|:-----|:---------|
| `rv32i_mcu/CoreMcu.hpp` | 完整重寫：IF/DM bridge 暫存器、ISRAM data-port 路徑、debug trace |
| `rv32i_mcu/component/CSR.hpp` | 新增 mscratch/misa/mcycle-write/minstret-write, software\_interrupt port |
| `rv32i_mcu/ExecuteStage.hpp` | stall\_DH rs2 修正、software\_interrupt passthrough |
| `rv32i_mcu/MemoryStage.hpp` | DM\_stall compute 順序修正 |
| `Core/CoreController.hpp` | 安裝 ISRAM read callback |
| `Core/DataSram.hpp` | word-aligned `comb_rdata_process` |
| `Core/Isram.hpp` | 新增 `read_byte()` public method |
| `test/firmware/test_hazard/main.c` | inline asm early-clobber fix |
| `Core/DmaEngine.hpp` | 修正 cluster→DRAM last-beat 遺失、DRAM 寫入資料/位址 buffer |
| `test/core_tb_utils.hpp` | 新建：FakeDram / FakeClusterSpm / ManifestBuilder / BootTestDriver |
| `test/test_core_sim.cpp` | 新建：DMA DRAM↔Cluster SPM 整合測試 testbench |
| `test/firmware/test_dma/main.c` | 新建：DMA 搬移 + PLIC 中斷等待韌體測試 |
| `scripts/fast_entry/core_mcu_sim.sh` | 新建 build/run/test 腳本 |
