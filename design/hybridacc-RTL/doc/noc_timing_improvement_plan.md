# NoC 合成時序改善計劃

## 1. 時序分析總覽

### 合成環境
| 項目 | 設定 |
|------|------|
| 工藝 | TSMC N16 ADFP |
| Operating Condition | ss0p72vm40c (worst-case) |
| 時脈週期 | 1.0 ns (1 GHz) |
| 工具 | Synopsys DC W-2024.09-SP2 |
| Wire Load Model | ZeroWireload |

### 各模組時序狀態

| 模組 | Slack (ns) | 路徑延遲 (ns) | 狀態 |
|------|-----------|--------------|------|
| **NetworkOnChip** | **-0.1054** | 1.2995 | :red_circle: VIOLATED |
| NoCRouter | 0.0000 | 1.1955 | :yellow_circle: 臨界 |
| MBUS | 0.0014 | 1.0986 | :green_circle: 安全 |

---

## 2. 關鍵路徑深度分析

### 2.1 NetworkOnChip 關鍵路徑 (Slack = -0.1054 ns)

**起點:** `gen_ports[2].gen_pe[12].pe / exe_a_stage / state_reg_reg[2]` (D-FF)
**終點:** `gen_ports[2].gen_pe[12].pe / exe_a_stage / PR / vp64_regs_reg[12].lanes[0][9]` (D-FF)

#### 路徑拆解

```
[0.200 ns] Clock network delay
    │
    ▼
[0.070 ns] state_reg FF → QN output (DFCNQND2)
    │
    ▼  ──── EXE_A_Stage 狀態解碼 ────
[0.057 ns] IND3D4 → AN2D4 → MUX2D2 (FSM decode + mux select)
    │
    ▼  ──── PsumRegFile 讀取 ────
[0.191 ns] pid[7] → NR4D1 → AOI32D1 → IAO21D1 → AN3D1 → AN2D2 →
           CKND2D4 → ND3D1 → BUFFD4 → CKND8 → AN2D4 →
           AOI222D1 → OAI211D1 → NR4D1 → IND4D1 → vp_out[10]
    │
    ▼  ──── VADDU FP16 加法 (組合邏輯) ────
[0.015 ns] MOAI22D1 (operand mux)
    │
[0.070 ns] DW_cmp (指數比較器): OAI31D1 → INVD1 → ND2D1 → AOI31D1 →
           INVD2 → AOI31D1 → IND2D4
    │
[0.234 ns] 尾數對齊 + 加法 (DW_fp_addsub):
           INVD4 → CKND8 → MUX2D2 → ND4D4 → NR2D2 → AOI21D2 →
           ND3D2 → INVD4 → IND2D4 → INVD4 → IND2D1 → INVD1 →
           ND2D1 → IND3D1 → XNR2D2
    │
[0.197 ns] ★ DW01_add 尾數加法器 (Ripple-Carry): 10 級 FA1D4 串接
           FA1D2 → FA1D4 × 9 → XNR2D4
    │
[0.033 ns] DW_norm 正規化: OR3D2
    │
[0.152 ns] ★ DW01_inc 進位鏈 (Ripple-Carry): 8 級 HA1D1 串接
           HA1D4 → HA1D1 × 7 → XOR2D1
    │
[0.023 ns] 輸出選擇: ND2D1 → IOAI21D2
    │
    ▼  ──── PsumRegFile 寫入 ────
[0.055 ns] AN2D2 → CKND2D8 → CKND12 → MUX2D1 → D-FF setup

Total: 1.2995 ns (超出 budget 0.1054 ns)
```

#### 延遲分佈

| 路徑段 | 延遲 (ns) | 佔比 | 說明 |
|--------|----------|------|------|
| Clock → FF output | 0.070 | 5.4% | FF CK-to-Q |
| EXE_A FSM decode | 0.057 | 4.4% | 狀態組合邏輯 |
| **PsumRegFile 讀取** | **0.191** | **14.7%** | 暫存器檔案 MUX 解碼 |
| **VADDU FP16 加法** | **0.727** | **55.9%** | :star: **主要瓶頸** |
| ├ DW_cmp 指數比較 | 0.070 | 5.4% | |
| ├ 尾數對齊 + 邏輯 | 0.234 | 18.0% | |
| ├ **DW01_add ripple** | **0.197** | **15.2%** | :warning: Ripple-carry |
| ├ DW_norm 正規化 | 0.033 | 2.5% | |
| └ **DW01_inc ripple** | **0.152** | **11.7%** | :warning: Ripple-carry |
| PsumRegFile 寫入 | 0.055 | 4.2% | 寫入 MUX + setup |

**根因:** VADDU 的 FP16 加法路徑佔了 55.9% 的延遲，其中 `DW01_add` (ripple-carry FA 串接) 和 `DW01_inc` (ripple-carry HA 串接) 兩條進位鏈加起來佔 26.9%，是最大的時序瓶頸。

### 2.2 NoCRouter 關鍵路徑 (Slack = 0.0000 ns)

**起點:** `command_data[3]` (input port)
**終點:** `u_ps_fifo/mem_reg[1][106]` (FIFO 記憶體)

路徑: input external delay (0.6 ns) → command decode (INR4/INR2/NR2) → pop control (AN4/OAI31/CKND1) → FIFO 非同步邏輯 (AOI31/CKBD4/NR2/BUFFD1) → 寫入 MUX (AO22D1)

**根因:** 外部輸入延遲設為 0.6 ns (佔 budget 的 60%)，FIFO 非同步控制邏輯約 0.21 ns。

### 2.3 MBUS 關鍵路徑 (Slack = 0.0014 ns)

**起點:** `pe_to_bus_plo_resp_valid[4]` (input port)
**終點:** `bus_to_noc_plo_resp_data[1]` (output port)

路徑: input → PLO response arbitration → output mux
**根因:** 純組合路徑 (input-to-output), 受外部延遲約束限制。

---

## 3. 改善方案

### 方案 A: VADDU FP16 加法器 Pipeline 分割 (`推薦 — 收益最高`)

**問題:** VADDU 使用 DesignWare `DW_fp_add` 實例化時 `num_stages=0` (純組合邏輯)，FP16 加法的完整路徑 (指數比較 → 尾數對齊 → 尾數加法 → 正規化 → 進位) 全部在一個 cycle 內完成。

**方案:**
1. **修改 VADDU.sv:** 在 `DW_fp_add` 中插入 pipeline register
   - 將 `DW_fp_add` 的 `num_cyc_reg` 參數改為 2 (2-stage pipeline)
   - 或手動將 FP16 加法拆為 2 階段:
     - Stage 1: 指數比較 + 尾數對齊 + 尾數加法 (~0.50 ns)
     - Stage 2: 正規化 + 進位修正 + 結果選擇 (~0.22 ns)

2. **修改 EXE_A_Stage FSM:** 增加等待週期
   - `S_NORMAL_MODE` 拆為 `S_NORMAL_S1` (發出操作數) 和 `S_NORMAL_S2` (接收結果)，增加 1 cycle latency
   - `S_VMAC_S1~S3` 的串接加法同理需要調整 FSM 時序

**預估效果:**
- VADDU 路徑從 0.727 ns ⇒ ~0.40 ns (最長 stage), 釋放約 0.33 ns
- Slack 改善: -0.1054 ns ⇒ +0.22 ns
- **代價:** 每次 VADDU 操作增加 1 cycle latency, 對 throughput 影響需評估 (Pipeline 可保持 throughput)

**修改檔案:** `VADDU.sv`, `EXE_A_Stage.sv`

---

### 方案 B: DesignWare 加法器架構替換 (Carry-Lookahead)

**問題:** DC 將 `DW01_add` 合成為 ripple-carry (10 級 FA 串接, ~0.197 ns)，`DW01_inc` 也是 ripple-carry (8 級 HA 串接, ~0.152 ns)。

**方案:**
1. **在 DC 腳本中指定加法器實現方式:**
   ```tcl
   set_implementation cla [find cell -hierarchical *DW01_add*]
   set_implementation cla [find cell -hierarchical *DW01_inc*]
   ```
   或使用 DesignWare 的 `impl` 參數切換為 Carry-Lookahead (CLA) 或 Brent-Kung 架構。

2. **提高 compile 優化等級:**
   ```tcl
   compile_ultra -timing_high_effort_script
   set_max_delay 0.8 -from [all_registers] -to [all_registers]
   ```

3. **對 VADDU 模組設定區域性高優化:**
   ```tcl
   set_critical_range 0.2 [get_designs VADDU*]
   ```

**預估效果:**
- CLA 加法器比 ripple-carry 快約 40-50%: 0.197 ns ⇒ ~0.12 ns, 0.152 ns ⇒ ~0.09 ns
- 總節省約 0.14 ns, Slack: -0.1054 ns ⇒ ~+0.03 ns
- **代價:** 面積增加約 10-15% (CLA 使用更多 gate), 功耗略增
- **注意:** 若 DC 已在 compile_ultra 模式下仍選擇 ripple-carry，可能是受到面積約束限制

**修改檔案:** `script/synthesis_*.tcl`

---

### 方案 C: PsumRegFile 讀取路徑優化

**問題:** PsumRegFile 的讀取路徑佔 0.191 ns (14.7%), 使用 24-entry × 64-bit 的暫存器陣列 + 組合 MUX，解碼邏輯深度達 14 級 gate。

**方案:**
1. **改用 SRAM 替換暫存器陣列:**
   - 將 `vp64_regs[0:23]` 替換為單埠 SRAM (64-bit × 24-word)
   - SRAM 讀取通常 < 0.15 ns (TSMC N16 SRAM compiler)
   - 需修改 read 為 registered output (增加 1 cycle read latency)

2. **改用 Latch-based 設計 + read-pipeline:**
   - 在 PsumRegFile 的 `vp_out` 加一級 output register
   - Read address 提前 1 個 cycle 送入, 下一個 cycle 讀出 registered data
   - FSM 相應提前發出 read address

3. **減少 MUX 深度:**
   - 使用 one-hot decoded write enable 而非 binary-to-one-hot 解碼
   - 對 read MUX 做 2-level 分層: 先 4:1 再 6:1

**預估效果:**
- MUX 優化: 0.191 ns ⇒ ~0.12 ns (節省 ~0.07 ns)
- SRAM 替換: 0.191 ns ⇒ ~0.10 ns (但需增加 read pipeline)
- **代價:** SRAM 方案需增加 1 cycle latency; MUX 優化代價較小

**修改檔案:** `PsumRegFile.sv`, `EXE_A_Stage.sv`

---

### 方案 D: NoCRouter FIFO 非同步邏輯優化

**問題:** NoCRouter 的 `u_ps_fifo` 非同步控制邏輯佔了 0.21 ns, 尤其 `AOI31D1 → CKBD4 → NR2D1` 的 buffer chain 延遲大。

**方案:**
1. **FIFO pop 訊號註冊化:**
   - 將 `command_data` 解碼→FIFO pop 的控制路徑加一級 register
   - 需更新 FIFO 介面為 registered pop

2. **FIFO 結構改用 Gray-code pointer:**
   - 若目前使用 binary 指標比較, 改為 Gray-code 可減少比較器延遲

3. **外部約束調整:**
   - 若 `input_external_delay = 0.6 ns` 過於保守，可放寬到 0.4 ns

**預估效果:**
- 註冊化 pop: 節省 ~0.08 ns, Slack 0.00 ⇒ +0.08 ns
- **代價:** FIFO 變為 1-cycle pop latency

**修改檔案:** `FIFO.sv`, `NoCRouter.sv`, `script/synthesis_*.tcl` (約束)

---

## 4. 改善方案比較

| 方案 | 預估 Slack 改善 | 修改複雜度 | 性能影響 | 面積影響 | 推薦優先度 |
|------|----------------|-----------|---------|---------|-----------|
| **A: VADDU Pipeline** | **+0.33 ns** | 中 (FSM 調整) | +1 cycle latency | 少量 reg | :star: **最高** |
| **B: CLA 加法器** | +0.14 ns | 低 (只改 TCL) | 無 | +10~15% | :star: **次高** |
| **C: PsumRegFile 優化** | +0.07 ns | 中 | +1 cycle latency | 減少 (若用 SRAM) | 中 |
| **D: FIFO 優化** | +0.08 ns | 低~中 | +1 cycle latency | 少量 | 低 (NoCRouter only) |

---

## 5. 建議執行順序

### Phase 1: 低風險快速收益
1. **先嘗試方案 B** — 只需修改合成腳本, 不動 RTL, 驗證 CLA 是否能解決 violation
2. 若 Slack 仍不足, 進入 Phase 2

### Phase 2: RTL 微調
3. **實施方案 A** — VADDU pipeline 分割, 同時解決最大瓶頸
4. 修改 EXE_A_Stage FSM 配合 pipeline delay
5. 重新跑 pre-sim 驗證功能正確性

### Phase 3: 進階優化 (若仍有需要)
6. **實施方案 C** — PsumRegFile 讀取優化
7. **實施方案 D** — NoCRouter FIFO 優化 (僅在 NoCRouter 單獨使用時需要)
8. 考慮降頻到 900 MHz 作為 fallback

---

## 附錄: 完整關鍵路徑數據

### NetworkOnChip Timing Report 關鍵數據
```
Clock Period:  1.0 ns
Slack:        -0.1054 ns (VIOLATED)
Path Delay:    1.2995 ns (data arrival) - 0.2000 ns (clock delay) = 1.0995 ns effective
Required:      1.1940 ns (data required)
```

### NoCRouter Timing Report 關鍵數據
```
Clock Period:  1.0 ns
Slack:         0.0000 ns (MET, zero margin)
Startpoint:    command_data[3] (input port)
Endpoint:      u_ps_fifo/mem_reg[1][106] (FIFO register)
Input Delay:   0.6 ns
```

### MBUS Timing Report 關鍵數據
```
Clock Period:  1.0 ns
Slack:         0.0014 ns (MET)
Startpoint:    pe_to_bus_plo_resp_valid[4] (input port)
Endpoint:      bus_to_noc_plo_resp_data[1] (output port)
Input Delay:   0.6 ns, Output Delay: -0.1 ns
```
