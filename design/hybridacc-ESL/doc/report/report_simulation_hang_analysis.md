# HybridAcc 模擬器「假性卡住」分析報告

## 1. 問題描述

| 項目 | 說明 |
|------|------|
| **症狀** | 執行 `conv2d_k3c4ich4och16.yaml`（input W=200）模擬時，終端無輸出、看似卡死 |
| **對照組** | `conv2d_k3c4ich4och16s.yaml`（input W=16）模擬可正常在數秒內完成 |
| **唯一差異** | Input tensor shape：`[1,16,16,4]` → `[1,16,200,4]`，Output：`[1,14,14,16]` → `[1,14,198,16]` |
| **使用者操作** | `hybridacc_sim.sh run-task output/test_conv2d` → 等待後手動 Ctrl+C（exit code 130） |

## 2. 調查結論

### 模擬並未卡死 — 只是耗時較長

將 max-cycles 提高到 2,000,000 後重新執行模擬，結果如下：

| 測資 | Input W | 模擬週期 | 牆鐘時間 | 結果 |
|------|---------|----------|---------|------|
| `conv2d_k3c4ich4och16s` (W=16) | 16 | **6,617 cycles** | **5.6 s** | ALL TESTS PASSED |
| `conv2d_k3c4ich4och16` (W=200) | 200 | **74,074 cycles** | **53.8 s** | ALL TESTS PASSED |
| 比例 | 12.5× | **11.2×** | **9.6×** | — |

**模擬器在 cycle 74,074 正常結束於 EBREAK**，核心並未陷入無窮迴圈或死鎖。

### Golden 比對結果

```
Output shape: [1, 14, 198, 16] = 44,352 fp16 words
Cosine similarity: 0.99999983
MSE:              3.64e-07
Max abs diff:     0.003906
Exact matches:    15,652 / 44,352 (35.3%)
RESULT: PASS (threshold: cosine_sim >= 0.99)
```

**運算結果完全正確！**

## 3. 根因分析

模擬看似「卡死」涉及 **兩個因素**：

### 3.1 工作量線性增長 ── 無可避免

W 從 16 增至 200，造成以下硬體迭代量變化：

| 元件 | W=16 | W=200 | 倍率 |
|------|------|-------|------|
| AGU-PD（輸入讀取） | 256 beats | 3,200 beats | 12.5× |
| AGU-PLI/PLO（累加器 R/W） | 784 beats | 11,088 beats | 14.1× |
| AGU-PS（權重讀取） | 144 beats | 144 beats | 1.0× |
| DMA 偏誤載入 | 784 beats | 11,088 beats | 14.1× |
| DMA 輸出寫回 | 784 beats | 11,088 beats | 14.1× |

其中 DMA SPM-DRAM 傳輸走 AXI-Lite Cluster Data Fabric（每 beat 約 4-5 cycle），佔了大部分模擬時間。實際計算（HDDU compute phase）反而不是瓶頸。

> 模擬器速度約 **1,300 cycles/sec**（SystemC cycle-accurate 模擬，含完整 SPM/HDDU/NoC/DMA/AXI，屬正常水準），因此 74K cycles ≈ 57 秒。

### 3.2 使用者介面欠缺進度回報 ── 造成誤判

`hybridacc_sim.sh` wrapper 的 `run_task()` 將模擬器輸出完全導向 `sim.log`：

```bash
"${cmd[@]}" > "$log" 2>&1 || rc=$?
```

使用者終端在模擬期間**完全沒有輸出**，只看到：
```
[INFO]  Running test_conv2d  →  …/sim.log
```
然後 53 秒的沉默。W=16 只需 5 秒就出結果，使用者習慣了近即時回應，因此 W=200 的 53 秒沉默被誤判為「卡住」。

### 3.3 Debug Build 加劇（次要因素）

調查時發現模擬器之前以 **Debug + ENABLE_DEBUG_UTILS=ON** 組態編譯：

```
CMAKE_BUILD_TYPE:STRING=Debug
ENABLE_DEBUG_UTILS:BOOL=ON
```

此組態使 SPM 模組在每個 cycle 輸出大量 `[Debug]` 訊息。雖然這些訊息被導向 `sim.log`，但生成字串本身仍消耗 CPU。實測 Debug build 與 Release build 的速度差距約 5-8%（57.9s vs 53.8s），不是主因，但仍應避免非必要的 debug 組建。

## 4. 已執行修正

### 4.1 修正 `hybridacc_sim.sh` Release Build 定義

**問題**：`do_build()` 僅設 `CMAKE_BUILD_TYPE=Release`，未顯式關閉 `ENABLE_DEBUG_UTILS`。若先前曾執行 `build_debug`，CMake cache 會殘留 `ENABLE_DEBUG_UTILS=ON`，導致 release build 仍啟用 debug 輸出。

**修正**：

```diff
- cmake "$SIM_MODEL_DIR" -DCMAKE_BUILD_TYPE=Release
+ cmake "$SIM_MODEL_DIR" -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG_UTILS=OFF
```

此修正確保 release 模式永遠不受 cache 殘留影響。

### 4.2 重新以 Release 模式編譯模擬器

```bash
./scripts/fast_entry/hybridacc_sim.sh build
```

確認編譯結果：
```
CMAKE_BUILD_TYPE:STRING=Release
ENABLE_DEBUG_UTILS:BOOL=OFF
```

## 5. 驗證結果

以 Release build 執行完整流程：

```bash
# 1. 編譯韌體
uv run hacc-compile testbench/core/conv2d_k3c4ich4och16.yaml -o output/test_conv2d --dump-ir

# 2. 產生 DRAM 測試資料
uv run python scripts/gen/gen_conv3x3_test_dram.py \
    --ir output/test_conv2d/hardware_ir.json \
    --workload testbench/core/conv2d_k3c4ich4och16.yaml \
    --output-dir output/test_conv2d

# 3. 執行模擬（~54 秒，正常完成）
./scripts/fast_entry/hybridacc_sim.sh run-task output/test_conv2d

# 4. 驗證結果
./scripts/fast_entry/hybridacc_sim.sh verify output/test_conv2d
# → RESULT: PASS (cosine_sim = 0.99999983)
```

## 6. 編譯器 Tiling 正確性分析

確認編譯器的 SPM 容量檢查邏輯對此測資是正確的：

```
SPM group capacity = 3 banks × 8192 words × 8 bytes = 196,608 bytes
half_group_capacity = 98,304 bytes
```

| Plane | 計算公式 | 大小 (bytes) | ≤ half_cap? |
|-------|---------|-------------|-------------|
| PS (weight) | 16 × 3 × 3 × 1 × 8 | 1,152 | ✅ |
| PD (input) | 16 × 200 × 1 × 8 | 25,600 | ✅ |
| PLI/PLO (accum) | 14 × 198 × 4 × 8 | 88,704 | ✅ (≤ 98,304) |

所有 plane 資料均在 SPM half-group 容量內，**不需要 tiling**（`num_*_tiles = 1`）。

## 7. 建議

| 優先度 | 建議 | 說明 |
|--------|------|------|
| 🔴 高 | 日常開發使用 `build`（Release），debug 調查時才用 `build_debug` | 避免不必要的性能損失和大型 log 檔 |
| 🟡 中 | 為 `run-task` 加入預估進度提示 | 例如：顯示經過時間或 sim.log 檔案大小增長 |
| 🟢 低 | 考慮 DMA Engine 的 burst 模式最佳化 | 目前 Cluster Data Fabric 為 single-outstanding AXI-Lite，每 beat 佔 4-5 cycles。改為 burst/pipelined 可大幅減少 DMA 週期數 |

## 8. 數據摘要

```
┌──────────┬───────────┬────────────┬──────────┬──────────────┐
│  測資     │  Input W  │  Sim Cycles│ 牆鐘時間  │  Golden 比對  │
├──────────┼───────────┼────────────┼──────────┼──────────────┤
│  W=16    │    16     │    6,617   │   5.6 s  │  PASS (0.999)│
│  W=200   │   200     │   74,074   │  53.8 s  │  PASS (0.999)│
└──────────┴───────────┴────────────┴──────────┴──────────────┘
Simulator throughput: ~1,300 cycles/sec (SystemC cycle-accurate)
```
