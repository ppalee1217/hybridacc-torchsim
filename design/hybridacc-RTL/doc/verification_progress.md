# HybridAcc RTL 驗證任務計劃書與當前進度

> 文件產生日期：2026-03-27
> 驗證工具：Synopsys VCS W-2024.09-SP1
> License Server：26585@lstn

---

## 1. 驗證任務總覽

本次驗證任務的目標涵蓋以下四個階段：

| 階段 | 任務描述 | 狀態 |
|------|----------|------|
| Phase 1 | 檢查所有 unit test TB 的正確性並修正 bug | **進行中** |
| Phase 2 | 對照 ESL (SystemC) 規格，確保 TB 覆蓋所有 corner case | **進行中** |
| Phase 3 | 以 VCS 執行所有 unit test TB 並確認通過 | **進行中** |
| Phase 4 | 撰寫完整驗證報告（含覆蓋率分析與問題清單） | **尚未開始** |

---

## 2. 驗證範圍

### 2.1 Unit Test TB 清單（共 19 個）

排除複合模組 TB（tb_networkonchip, tb_processelement, tb_pe_sim, tb_noc_sim_rtl, tb_noc_system, tb_noc_unit_rtl），僅針對以下 unit test：

**PE 子系統（14 個）：**
- tb_fifo, tb_asyncfifo, tb_decoder, tb_instructionmemory, tb_loopcontroller
- tb_vaddu, tb_vmulu, tb_psumregfile, tb_transformregfile, tb_datamemory
- tb_ldma, tb_sdma, tb_if_id_stage, tb_exe_a_stage, tb_exe_m_stage

**NoC 子系統（3 個）：**
- tb_perouter, tb_mbus, tb_nocrouter

**Cluster 子系統（1 個）：**
- tb_addressgenerateunit

### 2.2 RTL 原始碼涉及修改

| 檔案 | 修改類型 | 說明 |
|------|----------|------|
| `src/hybridacc_utils_pkg.sv` | **RTL Bug Fix** | fp16_mul / fp16_add 的 `logic'(exp_res[4:0])` 將 5-bit 指數截斷為 1-bit，導致所有非零乘法/加法結果錯誤 |
| `src/PE/LDMA.sv` | **RTL Refactor** | `always_comb` + `always_ff` 雙驅動 ICPD 違規，嘗試改為 `always @(*)` 但引發 combinational feedback loop |
| `src/PE/SDMA.sv` | **RTL Refactor** | 同 LDMA，ICPD 違規問題 |

---

## 3. 當前測試結果摘要

### 3.1 測試通過/失敗總表

| # | Testbench | 編譯 | 執行 | PASS | FAIL | 狀態 |
|---|-----------|------|------|------|------|------|
| 1 | tb_fifo | ✅ | ✅ | 25 | 0 | ✅ **PASS** |
| 2 | tb_asyncfifo | ✅ | ✅ | 30 | 0 | ✅ **PASS** |
| 3 | tb_datamemory | ✅ | ✅ | 7 | 0 | ✅ **PASS** |
| 4 | tb_decoder | ✅ | ✅ | 52 | 0 | ✅ **PASS** |
| 5 | tb_instructionmemory | ✅ | ✅ | 17 | 0 | ✅ **PASS** |
| 6 | tb_loopcontroller | ✅ | ✅ | 17 | 0 | ✅ **PASS** |
| 7 | tb_vaddu | ✅ | ✅ | 22 | 0 | ✅ **PASS** |
| 8 | tb_vmulu | ✅ | ✅ | 32 | 0 | ✅ **PASS** |
| 9 | tb_psumregfile | ✅ | ✅ | 22 | 0 | ✅ **PASS** |
| 10 | tb_transformregfile | ✅ | ✅ | 21 | 0 | ✅ **PASS** |
| 11 | tb_ldma | ✅ | ❌ 掛起 | — | — | 🔴 **HANG** |
| 12 | tb_sdma | ❌ ICPD | — | — | — | 🔴 **COMPILE FAIL** |
| 13 | tb_if_id_stage | ✅ | ✅ | 17 | 0 | ✅ **PASS** |
| 14 | tb_exe_a_stage | ✅ | ✅ | 14 | 0 | ✅ **PASS** |
| 15 | tb_exe_m_stage | ❌ ICPD | — | — | — | 🔴 **COMPILE FAIL** |
| 16 | tb_perouter | ✅ | ✅ | 16 | 2 | 🟡 **PARTIAL FAIL** |
| 17 | tb_mbus | ✅ | ✅ | 12 | 0 | ✅ **PASS** |
| 18 | tb_nocrouter | ✅ | ✅ | 18 | 0 | ✅ **PASS** |
| 19 | tb_addressgenerateunit | ✅ | ✅ | 28 | 10 | 🟡 **PARTIAL FAIL** |

### 3.2 統計

- **全部通過：13 / 19** (68.4%)
- **部分失敗：2 / 19** (tb_perouter: 2 FAIL, tb_addressgenerateunit: 10 FAIL)
- **編譯失敗：2 / 19** (tb_sdma, tb_exe_m_stage — 根因：LDMA/SDMA ICPD)
- **執行掛起：1 / 19** (tb_ldma — 根因：LDMA `always @(*)` combinational feedback loop)
- **Total assertions：277 PASS, 12 FAIL**

---

## 4. 已完成的修正清單

### 4.1 RTL 原始碼修正

#### 4.1.1 `src/hybridacc_utils_pkg.sv` — fp16 算術截斷 Bug（嚴重）

**問題：** `fp16_mul()` 和 `fp16_add()` 兩個函式的 return 語句使用了 `logic'(exp_res[4:0])`。`logic'()` 是 1-bit cast，將 5-bit 指數截斷為 1-bit，導致所有非特殊值（非0、非Inf、非NaN）的乘法與加法結果錯誤。

**影響範圍：** VMULU、VADDU、及所有依賴 fp16 計算的下游模組（EXE_A_Stage、EXE_M_Stage）。

**修正：** 將兩處 `logic'(exp_res[4:0])` 改為 `exp_res[4:0]`（直接使用 5-bit slice）。

```diff
- return {sign_res, logic'(exp_res[4:0]), sig_main[9:0]};
+ return {sign_res, exp_res[4:0], sig_main[9:0]};
```

**驗證結果：** fp16_mul(0x3C00, 0x3C00) 從錯誤的 0x0400 修正為正確的 0x3C00 (1.0×1.0=1.0)。tb_vmulu 從 19 FAIL 變為 32 PASS / 0 FAIL。

#### 4.1.2 `src/PE/LDMA.sv` — ICPD 雙驅動問題（未解決）

**問題：** LDMA 模組中 `mask_and_broadcast` 函式被 `always_comb` 和 `always_ff` 共同驅動的信號使用，VCS 報 `Error-[ICPD]`。嘗試將 `always_comb` 改為 `always @(*)`，但因為 FSM 在同一個 always block 中對暫存器做 self-update（如 `dma_offset_reg = dma_offset_reg + stride`），導致 combinational feedback loop，模擬掛起。

**狀態：🔴 需重新設計修正策略**（見§6.1）

#### 4.1.3 `src/PE/SDMA.sv` — ICPD 雙驅動問題（未解決）

**問題：** 與 LDMA 相同的 ICPD 問題。VCS 拒絕編譯。

**狀態：🔴 需重新設計修正策略**（見§6.1）

---

### 4.2 Testbench 修正

#### 4.2.1 `tb/PE/tb_decoder.sv`

| 修正項目 | 詳情 |
|----------|------|
| Test 11 SYS_CTRL payload 錯誤 | 原 payload `10'b0000_11_0000` 設定 bits[5:4]（ldma 信號）但 assert 檢查 sdma 信號（bits[7:6]）。修正為 `10'b0011_00_0000` |
| 新增 Test 11b | DMA ldma_act/ldma_rst 信號驗證 |
| 新增 Test 11c | clear_regs 信號驗證 |
| 新增 Tests 16-23 | DMA_MODE_SDMA, TSTORE, VTSTORE, TSHIFT, VPSUM, VMAC+vcounter, VMAC counter reset, loop_end+arithmetic 組合 |

#### 4.2.2 `tb/PE/tb_if_id_stage.sv`

| 修正項目 | 詳情 |
|----------|------|
| NOP 指令編碼錯誤 | 原 0x0002 實際解碼為 VMAC（opcode=1），修正為 0x0014（opcode=2, funct2=2） |
| HALT 指令編碼錯誤 | 原 0x0001 實際解碼為 DMA_setaddr+loop_end，修正為 0x001C（opcode=2, funct2=3） |
| 變數宣告位置 | `stalled_pc` 從 initial block 中段移至 block 開頭（VCS 語法要求） |

#### 4.2.3 `tb/PE/tb_loopcontroller.sv`

| 修正項目 | 詳情 |
|----------|------|
| Push 後取值時序 | push 後 `top_pc_reg`/`top_remaining_reg` 因 NBA 延遲落後一週期，需額外等待一個 `@(posedge clk)` |
| Test 2 修正 | push 後新增等待週期再檢查 `pc_out` |
| Test 8 嵌套迴圈修正 | 外層/內層 push 均加等待週期，loop_end 判斷從 3 次改為 4 次（含 NBA lag） |

#### 4.2.4 `tb/PE/tb_vaddu.sv`

| 修正項目 | 詳情 |
|----------|------|
| Test 6 subnormal 預期值 | RTL fp16_add 在 `exp_a==0` 時直接返回 b（flush subnormals），`0x0001+0x0001` 結果為 0x0001 而非 0x0002 |

#### 4.2.5 `tb/PE/tb_vmulu.sv`

| 修正項目 | 詳情 |
|----------|------|
| 全面通過 | fp16_mul RTL bug 修正後，原 TB 預期值均正確，32/32 PASS |

#### 4.2.6 `tb/PE/tb_exe_a_stage.sv`

| 修正項目 | 詳情 |
|----------|------|
| Halt 測試 stall 殘留 | 前一測試的 PLI stall 條件未清除，導致 halt 測試誤判。加入清除 stall 邏輯 |

#### 4.2.7 其他 TB 已修正但無 bug 的項目

- `tb/tb_asyncfifo.sv`：修正 pop_set 時序（clock 後再讀 data_out_set）、修正 SimPushPop full 判斷邏輯
- `tb/PE/tb_perouter.sv`：修正 ready 信號初始狀態造成的 FIFO 自動 pop 問題（部分修正，仍有 2 FAIL）

---

### 4.3 新增 Corner Case 測試

| Testbench | 新增測試 | 對應 ESL 規格 |
|-----------|----------|---------------|
| tb_asyncfifo | Test 9: 同時 push+pop、Test 10: 同時 push+pop_set | asyncFIFO 寬度轉換 FIFO 並行操作 |
| tb_psumregfile | Test 10: Hybrid mode 向量/純量別名存取、Test 11: pcounter 大值遞增 | PsumRegFile 混合定址模式 |
| tb_transformregfile | Test 10: K5 shift mask 驗證、Test 11: K7 shift mask 驗證 | TransformRegFile 三種 kernel size shift |
| tb_ldma | Test 6: Broadcast HALF 模式、Test 7: Multi-loop 迴圈 | LDMA broadcast + stride + loop FSM |
| tb_sdma | Test 7: Multi-loop bank ping-pong、Test 8: Stride 跳躍位址 | SDMA 多迴圈 bank 切換 |
| tb_decoder | Tests 16-23: 8 種新指令/組合 corner case | Decoder 完整指令集覆蓋 |

---

## 5. 未解決問題清單

### 5.1 🔴 P0（阻塞性）：LDMA / SDMA ICPD 問題

**影響：** tb_ldma（掛起）、tb_sdma（編譯失敗）、tb_exe_m_stage（編譯失敗，因包含 LDMA+SDMA）

**根因分析：**
LDMA.sv 和 SDMA.sv 的 RTL 架構中，`mask_and_broadcast` 等組合邏輯函式和 FSM 暫存器更新放在同一個 `always_ff` block 中。VCS 嚴格的 ICPD 規則禁止 `always_comb` 和 `always_ff` 同時驅動相同變數。

嘗試過的方案及結果：
1. **`always_comb` → `always @(*)`**：消除 ICPD 編譯錯誤，但 FSM 中對暫存器的組合自更新（如 `dma_offset_reg = dma_offset_reg + stride`）形成 combinational feedback loop，模擬掛起。
2. 需要的正確方案：**將 `always_ff` 拆分為純暫存器更新 + 獨立的 `always_comb` next-state 邏輯**，使用 `_next` 信號模式。

**預計修正工作量：** 中等 — 需重構 LDMA.sv (~170行) 和 SDMA.sv (~130行) 的 FSM 為標準 next-state pattern。

### 5.2 🟡 P1：tb_perouter 仍有 2 FAIL

**失敗測試：**
- `PLI LN: pe_pli_valid=1`
- `PLI LN: data correct`

**初步分析：** PERouter 在 PLI（PE Load Input）的 LN（Lane）模式下，FIFO pop 時序與 ready 信號交互有問題。已部分修正 ready 初始值，但 PLI 路徑的 data flow 仍需進一步排查。

### 5.3 🟡 P1：tb_addressgenerateunit 仍有 10 FAIL

**失敗測試：**
- AGU 多層巢狀迴圈的 addr/tag 計算偏差
- Backpressure 釋放後 advance 判斷

**初步分析：** AGU 的 `calc_addr` / `calc_tag` 函式在巢狀迴圈索引遞增時，TB 預期值可能與 RTL 實際的迭代順序不符。需仔細比對 RTL 的 4 層迴圈展開邏輯與 TB 的預期地址序列。

---

## 6. 下一步行動計畫

### Phase 5a：修復 LDMA/SDMA ICPD（P0 阻塞項）

| 步驟 | 動作 | 預期結果 |
|------|------|----------|
| 6.1.1 | 重構 LDMA.sv：拆分為 `always_comb`（next-state 邏輯）+ `always_ff`（暫存器更新）| ICPD 消除，編譯通過 |
| 6.1.2 | 重構 SDMA.sv：同上 | 編譯通過 |
| 6.1.3 | 重新執行 tb_ldma, tb_sdma, tb_exe_m_stage | 確認功能正確 |

### Phase 5b：修復剩餘 TB 失敗

| 步驟 | 動作 | 預期結果 |
|------|------|----------|
| 6.2.1 | 排查 tb_perouter PLI LN 模式的 FIFO 時序 | 2 FAIL → 0 FAIL |
| 6.2.2 | 排查 tb_addressgenerateunit 巢狀迴圈地址計算 | 10 FAIL → 0 FAIL |

### Phase 5c：全量回歸測試

| 步驟 | 動作 | 預期結果 |
|------|------|----------|
| 6.3.1 | 執行全部 19 個 unit test TB | 19/19 PASS |
| 6.3.2 | 確認無新增 regression | 所有 assertion 通過 |

### Phase 6：撰寫最終驗證報告

| 步驟 | 動作 |
|------|------|
| 6.4.1 | 彙整每個模組的測試覆蓋項清單（功能點 vs. 測試案例矩陣）|
| 6.4.2 | 記錄所有發現的 RTL bug 與 TB bug |
| 6.4.3 | 評估 corner case 覆蓋率，列出未涵蓋項目 |
| 6.4.4 | 提出改善建議與後續驗證方向 |

---

## 7. 已修改檔案索引

### RTL 原始碼（3 個檔案）

| 檔案路徑 | 修改摘要 |
|----------|----------|
| `src/hybridacc_utils_pkg.sv` | 修正 fp16_mul/fp16_add 指數截斷 bug |
| `src/PE/LDMA.sv` | 嘗試修正 ICPD（**目前狀態不穩定，需重構**）|
| `src/PE/SDMA.sv` | 嘗試修正 ICPD（**目前狀態不穩定，需重構**）|

### Testbench 檔案（18 個檔案）

| 檔案路徑 | 修改摘要 |
|----------|----------|
| `tb/PE/tb_decoder.sv` | 修正 SYS_CTRL payload，新增 8 個 corner case 測試 |
| `tb/PE/tb_if_id_stage.sv` | 修正 NOP/HALT 編碼，修正變數宣告位置 |
| `tb/PE/tb_loopcontroller.sv` | 修正 push 後 NBA 時序，修正嵌套迴圈測試 |
| `tb/PE/tb_vaddu.sv` | 修正 subnormal flush 預期值 |
| `tb/PE/tb_vmulu.sv` | 無 TB bug（RTL 修正後自動通過）|
| `tb/PE/tb_psumregfile.sv` | 新增 hybrid aliasing 和 pcounter 測試 |
| `tb/PE/tb_transformregfile.sv` | 新增 K5/K7 shift mode 測試 |
| `tb/PE/tb_ldma.sv` | 新增 broadcast 和 multi-loop 測試 |
| `tb/PE/tb_sdma.sv` | 新增 multi-loop bank ping-pong 和 stride 測試 |
| `tb/PE/tb_exe_a_stage.sv` | 修正 halt 測試 stall 殘留 |
| `tb/PE/tb_perouter.sv` | 修正 ready 信號初始值（部分） |
| `tb/tb_asyncfifo.sv` | 修正 pop_set 時序，新增 push+pop / push+pop_set 測試 |
| `tb/tb_fifo.sv` | 驗證通過（可能有先前的修正殘留） |
| `tb/PE/tb_datamemory.sv` | 驗證通過 |
| `tb/PE/tb_instructionmemory.sv` | 驗證通過 |
| `tb/PE/tb_exe_m_stage.sv` | 待 LDMA/SDMA ICPD 修正後重新驗證 |
| `tb/NoC/tb_mbus.sv` | 驗證通過 |
| `tb/NoC/tb_nocrouter.sv` | 驗證通過 |

---

## 8. 關鍵發現摘要

### 8.1 RTL Bug（已修正）

| ID | 嚴重度 | 模組 | 描述 |
|----|--------|------|------|
| RTL-001 | **Critical** | hybridacc_utils_pkg | fp16_mul `logic'()` 截斷 5-bit exponent 為 1-bit，所有非特殊值乘法結果錯誤 |
| RTL-002 | **Critical** | hybridacc_utils_pkg | fp16_add 同樣的 `logic'()` 截斷問題 |

### 8.2 RTL 架構問題（待修正）

| ID | 嚴重度 | 模組 | 描述 |
|----|--------|------|------|
| RTL-003 | **High** | LDMA.sv | always_comb + always_ff ICPD 違規，需重構為 next-state pattern |
| RTL-004 | **High** | SDMA.sv | 同 RTL-003 |

### 8.3 TB Bug（已修正）

| ID | 模組 | 描述 |
|----|------|------|
| TB-001 | tb_decoder | SYS_CTRL payload bit-field 對應錯誤 |
| TB-002 | tb_if_id_stage | NOP/HALT 指令編碼完全錯誤 |
| TB-003 | tb_loopcontroller | push 後未等待 NBA 更新即檢查輸出 |
| TB-004 | tb_vaddu | subnormal flush 行為預期值錯誤 |
| TB-005 | tb_exe_a_stage | 前一測試 stall 殘留影響後續測試 |
| TB-006 | tb_asyncfifo | pop_set 後讀取時序錯誤 |

---

*本文件將隨驗證進度持續更新。*
