# HybridAcc 全系統模擬分析報告 — Conv2D 3×3 端到端驗證

> **日期**：2026-07
> **工作負載**：`conv2d_3x3_sim` — 3×3 Conv2D, stride=1, padding=0
> **模擬器**：`hybridacc-sim` (SystemC ESL, 全系統 SoC 模擬)
> **最終結果**：✅ **PASS** — 模擬完成 cycle 4563，cosine similarity = 0.99999982

---

## 1. 工作負載規格

| 參數 | 值 |
|---|---|
| Input (NHWC) | [1, 16, 16, 4] fp16 |
| Weight (OIHW) | [16, 3, 3, 4] fp16 |
| Output (NHWC) | [1, 14, 14, 16] fp16 |
| Kernel | 3×3, stride=1, pad=0 |
| 硬體配置 | 1 cluster, 3 bus × 16 PE = 48 PE (42 active) |
| SPM | 4 groups, 3 banks/group, bank depth=8192 words, pipeline depth=3 |
| Tiling | 1 oc × 1 h × 1 w × 1 ic tile（單 wave） |

---

## 2. 模擬結果摘要

### 2.1 模擬完成

```
[TB] EBREAK at cycle 4563, IRQ_SUMMARY=0x1d
[SIM] Simulation ended at 254320 ns
```

模擬在 4563 cycles 內正常完成，firmware 執行到 `ebreak` 指令。

### 2.2 輸出驗證

| 指標 | 值 | 門檻 |
|---|---|---|
| Cosine Similarity | 0.99999982 | ≥ 0.99 |
| MSE | 3.864 × 10⁻⁷ | — |
| Max Absolute Diff | 0.003906 (1 ULP) | — |
| Exact Match | 1092 / 3136 (34.8%) | — |
| Close Match (< 5%) | 3117 / 3136 (99.4%) | — |
| 非零輸出 | 3134 / 3136 | — |

> 差異完全源於 fp16 乘累加運算順序不同（硬體 PE vs Python reference），屬正常數值誤差。

### 2.3 關於 Simulator 報告 "FAIL" 的說明

Simulator 輸出 `Total: 1 Pass: 0 Fail: 1 First-fail: 15` 為**假陽性**。

原因：Simulator 從 DSRAM[0x00–0x0C] 讀取 firmware test counters，但 linker 將 `.rodata`（LayerConfig 結構）放在 DSRAM 起始位址 0x10000000。因此前 4 個 word 被誤讀為：

| DSRAM Offset | Test 框架解讀 | 實際值 (LayerConfig field) |
|---|---|---|
| 0x00 | Total = 1 | `cluster_mask_lo = 0x00000001` |
| 0x04 | Pass = 0 | `cluster_mask_hi = 0x00000000` |
| 0x08 | Fail = 1 | `num_clusters = 1` |
| 0x0C | First-fail = 15 | `hddu_plane_en = 0x0F` |

此 firmware 不使用 `TEST_CHECK` 巨集（僅執行 `run_layer()` + `ebreak`），因此 DSRAM 測試區域未被初始化為 sentinel 值 `0xCAFECAFE`。

---

## 3. test_cluster_sim_advanced 與 hybridacc_sim 差異分析

### 3.1 架構差異

| 面向 | test_cluster_sim_advanced | hybridacc_sim |
|---|---|---|
| **驅動方式** | C++ testbench 直接透過 sc_signal 寫入 | RISC-V firmware 透過 MMIO 寫入 |
| **資料搬運** | Testbench 直接寫入 SPM | DMA Engine: DRAM → SPM → DRAM |
| **Scan chain 寫入順序** | `send_scan_chain_words_reverse()` (反序) | Forward (chain[0] → chain[47]) |
| **Scan chain mapping** | index 0 = Bus 0 PE[0] | chain[0] = Bus 2 PE[15] (shift register 末端) |
| **HDDU 啟動** | `cfg_hddu_global()` 含 CTRL_START，再由 `start_all()` 再次 CTRL_START | firmware 先 CTRL_RESET (bit 0)，再 CTRL_START (bit 1) |
| **SPM pipeline depth** | 3 (硬編碼) | 原為 1 → **已修正為 3** |
| **PE 程式載入** | cluster_gen.py 產生 | hybridacc-cc/lowering.py 產生 |
| **輸出驗證** | 直接從 SPM 讀回 + golden fp16 比對 | DRAM dump 比對 |

### 3.2 關鍵參數差異（修正前）

cluster_gen.py 與 lowering.py 在以下參數存在差異：

| 參數 | cluster_gen (正確) | lowering.py (修正前) | 影響 |
|---|---|---|---|
| PLI/PLO tag_ctrl | 1 | 2 | Tag 跟隨 iter2 而非 iter1 → 前 56 beats 全指向同一 PE |
| spm_map_odd | 0xB4 | 0xD8 | Group swap 映射錯誤（多 IC tile 時會出問題） |
| SPM pipeline depth | 3 | 1 | 額外 2 cycle latency 但不致死鎖 |
| PE OUTPUT_WINDOW_CNT | tile_w_out - 1 = 13 | tile_h_out × tile_w_out - 1 = 195 | **ROOT CAUSE** — PE 迴圈跑 196 次但只有 14 列的 PLI 資料 |

### 3.3 Scan Chain 驗證

逐一比對 48 筆 scan chain 命令，firmware (forward order) 經過 shift register 後與 cluster testbench (reverse order) 映射至完全相同的 PE 配置：

- **Bus 0**: ps_id=0, route_mode=PLI_FROM_BUS_PLO_TO_LN, 14 active PEs (pd_id=0..13, pli_id=0..13)
- **Bus 1**: ps_id=1, route_mode=PLI_FROM_LN_PLO_TO_LN, 14 active PEs (pd_id=2..15)
- **Bus 2**: ps_id=2, route_mode=PLI_FROM_LN_PLO_TO_BUS, 14 active PEs (pd_id=0..13, plo_id=0..13)

---

## 4. Bug 修復紀錄

### Bug #1: PLI/PLO tag_ctrl 錯誤

- **檔案**: `python/hybridacc_cc/lowering.py` (lines 329, 340)
- **症狀**: tag 跟隨 iter2 (output column) 而非 iter1 (output row)，導致前 56 beats (4 channels × 14 columns) 全指向同一 PE
- **修正**: `tag_ctrl=2` → `tag_ctrl=1`
- **風險**: 單獨修正此 bug 不足以解決 deadlock

### Bug #2: spm_map_odd 編碼錯誤

- **檔案**: `python/hybridacc_cc/lowering.py` (line 43)
- **症狀**: Group mapping swap 在 ic_tile=odd 時編碼錯誤
- **修正**: `_SPM_MAP_ODD = 0xD8` → `0xB4`
- **風險**: 潛在 bug，在 num_ic_tiles=1 時不觸發

### Bug #3: SPM SRAM Bank Pipeline Depth 不匹配

- **檔案**: `design/hybridacc-ESL/simulator/include/ComputeCluster.hpp`
- **症狀**: 全系統模擬預設 pipeline_depth=1，cluster testbench 預設 3
- **修正**: `SPM_SRAM_BANK_PIPELINE_DEPTH = 1` → `3`
- **風險**: 不致 deadlock，但影響 timing 一致性

### Bug #4: PE 程式 OUTPUT_WINDOW_CNT_MINUS_ONE 計算錯誤 — **ROOT CAUSE**

- **檔案**: `python/hybridacc_cc/lowering.py` (line ~348)
- **修正**: `OUTPUT_WINDOW_CNT_MINUS_ONE = tile_h_out * tile_w_out - 1` → `tile_w_out - 1`

#### 4.1 詳細根因分析

**背景**：Conv2D 3×3 在 HybridAcc 上的執行模型為，HDDU 透過 AGU tag routing 將 14 個 output rows 分配至 14 個 PE，每個 PE 負責 **一行** (`tile_w_out = 14` columns) 的 conv1d 運算。

**錯誤**：lowering.py 設定 `OUTPUT_WINDOW_CNT_MINUS_ONE = tile_h_out × tile_w_out - 1 = 195`，意圖讓每個 PE 處理整個 14×14 空間。但 tag routing 已經將 rows 分散至不同 PE，每個 PE 實際只會收到 **1 row × 14 columns** 的 PLI/PD 資料。

**PE 程式行為** (conv1d_k3c4s1_template)：
```
LOOPIN 迴圈封裝 = OUTPUT_WINDOW_CNT_MINUS_ONE - 1 = 194 (修正前)
→ 迴圈跑 195 次 + 1 初始 iteration = 196 total windows
→ 每 window 消耗 4 PLI beats (out_ch_pack)
→ 共需消耗 196 × 4 = 784 PLI beats
```

但每個 PE 只能從 PLI 收到 14 columns × 4 beats = **56 beats**。

**死鎖機制**：

```
PE 消耗完 56 beats PLI → 等待更多 PLI → PLI 永遠不來
                    │
                    ▼ 同時
NoCRouter PLO path: head-of-line blocking
  → 發出 PLO read request 到 PE[tag]
  → PERouter 要求 plo_fifo 非空才 accept
  → PE 卡在等 PLI → 無法產出新的 PLO
  → NoCRouter PLO request stall
  → 所有後續 PE 的 PLO 也無法被讀取
  → 其他 PE 的 plo_fifo 滿 → 反壓到 PS/PD/PLI
  → 全系統 DEADLOCK
```

**修正後**：`OUTPUT_WINDOW_CNT_MINUS_ONE = tile_w_out - 1 = 13`
- LOOPIN 封裝值 = 12 → 迴圈跑 13 次 + 1 初始 = 14 windows
- 每 PE 消耗 14 × 4 = 56 PLI beats ✓
- 每 PE 產出 14 × 4 = 56 PLO beats ✓
- 14 PE × 56 = 784 total PLO beats = AGU iter total ✓

### 4.2 排除的假設

| 假設 | 驗證方式 | 結果 |
|---|---|---|
| FIFO depth 不足導致 cascading backpressure | 將 pe_fifo_depth 從 4 增至 64 | ❌ Deadlock 仍存在 |
| HDDU 啟動時序差異 | 比對 firmware vs testbench START/STOP sequence | ❌ 非根因 |
| Scan chain 配置錯誤 | 48 筆逐一比對 | ❌ 配置正確 |
| NoCRouter resp_fifo → HDDU 寫入路徑問題 | 追蹤 17 筆 PLO 回應 | ❌ 路徑正確，問題在 PE 端 |

---

## 5. 修改文件清單

| 文件 | 修改內容 |
|---|---|
| `python/hybridacc_cc/lowering.py` | PLI/PLO tag_ctrl 2→1; spm_map_odd 0xD8→0xB4; OUTPUT_WINDOW_CNT_MINUS_ONE tile_h*tile_w-1→tile_w-1 |
| `design/hybridacc-ESL/simulator/include/ComputeCluster.hpp` | SPM_SRAM_BANK_PIPELINE_DEPTH 1→3 |
| `python/hybridacc_cc/config/conv2d_3x3_sim.yaml` | 新建正確的 workload 配置 YAML |

---

## 6. 模擬時間軸 (修正後)

```
Phase 1: Boot & Init          [0 ~ 40 µs]     RISC-V boot, AGU/HDDU 配置
Phase 2: Scan Chain Config     [40.9 ~ 48.4 µs] 48 筆 PE 配置載入
Phase 3: DMA 資料搬運          [70.5 ~ 226 µs]  Weight(144) + Input(256) 搬入 SPM
Phase 4: HDDU/PE 計算          [226.9 ~ 228.7 µs] 42 PE parallel compute
Phase 5: DMA 輸出回寫 + EBREAK [228.7 ~ 254.3 µs] SPM→DRAM 784 beats
```

Total: 4563 cycles / 254,320 ns

---

## 7. 已知限制與後續工作

### 7.1 Simulator Test Framework 假陽性

DSRAM test counter 區域與 firmware `.rodata` 重疊。建議修正方案：
- **方案 A**：Linker script 預留 DSRAM[0x00–0x10] 給 test counters
- **方案 B**：Conv2D firmware 加入 `TEST_CHECK` 巨集進行自驗證
- **方案 C**：Simulator 加入 `--golden` 選項，直接在 post-sim 比對 DRAM output

### 7.2 尚未驗證的場景

| 場景 | 狀態 | 依賴 |
|---|---|---|
| 多 IC tile (num_ic_tiles > 1) | 未測試 | spm_map_odd 修正已備妥 |
| 多 OC tile (num_oc_tiles > 1) | 未測試 | 需新 test case |
| Ping/pong 雙緩衝切換 | 未測試 | 需多 wave 配置 |
| Conv1x1 operator | 未測試 | 獨立 lowering 路徑 |
| GEMM operator | 未測試 | 獨立 lowering 路徑 |
| 多 cluster | 未測試 | 需修改 cluster_mask |

### 7.3 效能觀察

- 計算密度較低：4563 cycles 中大部分時間用於 DMA 搬運和 boot initialization
- 單 wave (1 tile) 無法充分利用 DMA/compute overlap
- 真實工作負載（多 tile）才能體現 prefetch pipeline 效益

---

## 附錄 A: 完整 Bug 修復 Diff

### lowering.py — tag_ctrl 修正

```python
# PLI AGU（修正前 tag_ctrl=2，修正後 tag_ctrl=1）
agu_pli = AguBankConfig(
    ...
    tag_ctrl=1,   # was: 2
    ...
)

# PLO AGU（同上）
agu_plo = AguBankConfig(
    ...
    tag_ctrl=1,   # was: 2
    ...
)
```

### lowering.py — spm_map_odd 修正

```python
_SPM_MAP_ODD = 0xB4   # was: 0xD8
```

### lowering.py — PE 程式參數修正

```python
pe_params = {
    ...
    "OUTPUT_WINDOW_CNT_MINUS_ONE": tile_w_out - 1,  # was: tile_h_out * tile_w_out - 1
    ...
}
```

### ComputeCluster.hpp — SPM pipeline depth

```cpp
static constexpr unsigned SPM_SRAM_BANK_PIPELINE_DEPTH = 3;  // was: 1
```

---

## 附錄 B: 驗證命令

```bash
# 編譯 firmware
cd /home/yoyo/work/MasterResearch/HybridAcc/python
uv run python -m hybridacc_cc.cli

# 編譯 simulator
cd /home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-ESL/simulator/build
cmake .. && make -j$(nproc)

# 執行模擬
SIM=./hybridacc-sim
FW=../../output/hacc_conv3x3_test_build/firmware.elf
DRAM=../../output/hacc_conv3x3_test_build/dram_init.bin
timeout 30 "$SIM" --mirror "$DRAM" --max-cycles 500000 "$FW"

# 比對 golden output (Python)
python3 compare_golden.py  # 見附錄 C
```

## 附錄 C: Golden 比對腳本

```python
import struct, math

meta = {}
with open('golden_meta.txt') as f:
    for line in f:
        k, v = line.strip().split('=')
        meta[k] = v

dram_output_base = int(meta['dram_output_base'], 16)
golden_bytes = int(meta['golden_output_bytes'])

with open('golden_output.bin', 'rb') as f:
    golden = f.read()

with open('dram_init.bin.out', 'rb') as f:
    f.read(8)  # skip sparse header
    records = []
    while True:
        addr = struct.unpack('<I', f.read(4))[0]
        length = struct.unpack('<I', f.read(4))[0]
        if addr == 0 and length == 0:
            break
        data = f.read(length)
        records.append((addr, length, data))

actual = bytearray(golden_bytes)
for addr, length, data in records:
    s = max(dram_output_base, addr)
    e = min(dram_output_base + golden_bytes, addr + length)
    if s < e:
        actual[s - dram_output_base : e - dram_output_base] = data[s - addr : e - addr]

def fp16(h):
    s = (h >> 15) & 1; exp = (h >> 10) & 0x1F; f = h & 0x3FF
    if exp == 0: return (-1)**s * 2**-14 * (f / 1024)
    if exp == 31: return float('inf') if f == 0 else float('nan')
    return (-1)**s * 2**(exp - 15) * (1 + f / 1024)

dot = ng = na = mse = mx = 0; total = golden_bytes // 2
for i in range(total):
    g = fp16(struct.unpack('<H', golden[i*2:i*2+2])[0])
    a = fp16(struct.unpack('<H', actual[i*2:i*2+2])[0])
    dot += g*a; ng += g*g; na += a*a
    d = abs(g-a); mse += d*d; mx = max(mx, d)

cos = dot / (math.sqrt(ng) * math.sqrt(na))
print(f"Cosine: {cos:.8f}, MSE: {mse/total:.2e}, MaxDiff: {mx:.6f}")
print("PASS" if cos >= 0.99 else "FAIL")
```
