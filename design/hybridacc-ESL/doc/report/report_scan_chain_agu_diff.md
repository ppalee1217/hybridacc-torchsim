# hybridacc-cc vs noc_gen/cluster_gen 差異分析報告

## 摘要

本報告深度比對 **hybridacc-cc compiler**（`python/hybridacc_cc/lowering.py`）與 **驗證環境 testbench data generator**（`python/hybridacc_verify/gen/noc_gen.py`、`cluster_gen.py`）在 NoC scan chain 配置與 AGU 參數產生上的差異。

分析結果：**hybridacc-cc 存在 5 個嚴重 (Critical) 級別的 bug**，導致 PE 陣列完全無法正確接收或產出資料。

---

## 1. Scan Chain ID 分配（CRITICAL）

### 1.1 問題描述

Scan chain 中每個 PE 的 `ps_id`、`pd_id`、`pli_id`、`plo_id` 決定了該 PE 接收 NoC 封包的 tag 匹配規則。當 AGU 送出一筆 NoC 封包帶有 `tag=T`，只有 scan chain 中對應 port 的 ID 等於 T 的 PE 才會接受該封包。

### 1.2 noc_gen / cluster_gen（正確）

Conv2D Normal Mode 下，每個 bus 對應一個 kernel height 位置，bus 內的 PE 各處理不同的 output row：

```python
# cluster_gen.py / noc_gen.py
for i in range(num_bus):          # i = bus_idx → kernel_row (0..KH-1)
    for j in range(num_pes_per_bus):  # j = pe_local → output_row
        enable = (i < split_kh and j < out_h_final)
        ps_id  = i                              # 同一 bus 上所有 PE 共享 → weight broadcast
        pd_id  = (i + j) * stride               # 每個 PE 對應不同的 input row
        pli_id = j if (i == 0) else 63          # 只有第一個 bus 的 PE 讀 PLI (from bus)
        plo_id = j if (i == split_kh - 1) else 63  # 只有最後一個 bus 的 PE 寫 PLO (to bus)
```

**語義：**
| ID | 含義 | 值域 | 功能 |
|---|---|---|---|
| `ps_id` | kernel row index | `0..KH-1` | **同 bus 共享同一個 ps_id** → weight broadcast 至全 bus |
| `pd_id` | input row position | `(bus+pe)*stride` | **每個 PE 唯一** → 每 PE 接收不同 input row |
| `pli_id` | output row index | `pe_local` or `63` | 僅 bus 0 有效，匹配 output row tag |
| `plo_id` | output row index | `pe_local` or `63` | 僅 最後一個 bus 有效，匹配 output row tag |

### 1.3 hybridacc-cc（錯誤）

```python
# lowering.py compute_scan_chain()
for bus_idx in range(num_bus):
    for pe_local in range(pes_per_bus):
        gid = bus_idx * pes_per_bus + pe_local
        ps_id = gid    # ← 全局唯一！每個 PE 期望不同的 weight tag
        pd_id = gid    # ← 全局唯一！但跟 input row 無對應關係
```

### 1.4 影響

| 欄位 | 正確值 | hybridacc-cc 值 | 錯誤後果 |
|---|---|---|---|
| `ps_id` | bus_idx (e.g. 0,0,0,...,1,1,1,...,2,2,2) | gid (0,1,2,3,4,...,N-1) | **Weight 無法 broadcast**：AGU PS 送出 tag=0 的封包只會配發給 PE0，其他 PE 收不到 weight |
| `pd_id` | (bus+pe)*stride (e.g. 0,1,2,...) | gid (0,1,2,...) | **Input row mapping 錯誤**：pd_id=gid 沒有考慮 stride 和 bus offset |
| `pli_id` | pe_local if bus==0, else 63 | bus_idx if first_pe, else gid | **PLI routing 完全錯亂**：非 bus0 的 PE 也有有效 pli_id |
| `plo_id` | pe_local if bus==last, else 63 | bus_idx if last_pe, else gid | **PLO routing 完全錯亂**：非最後一個 bus 的 PE 也有有效 plo_id |

### 1.5 示例（3 bus × 4 PE/bus, KH=3, out_h=4, stride=1）

**正確（noc_gen）：**
```
Bus 0: PE(ps=0, pd=0, pli=0, plo=63, mode=IB→OL)  ← 第一行
        PE(ps=0, pd=1, pli=1, plo=63, mode=IL→OL)
        PE(ps=0, pd=2, pli=2, plo=63, mode=IL→OL)
        PE(ps=0, pd=3, pli=3, plo=63, mode=IL→OL)
Bus 1: PE(ps=1, pd=1, pli=63, plo=63, mode=IL→OL)  ← 中間行
        PE(ps=1, pd=2, pli=63, plo=63, mode=IL→OL)
        PE(ps=1, pd=3, pli=63, plo=63, mode=IL→OL)
        PE(ps=1, pd=4, pli=63, plo=63, mode=IL→OL)
Bus 2: PE(ps=2, pd=2, pli=63, plo=0, mode=IL→OB)  ← 最後行
        PE(ps=2, pd=3, pli=63, plo=1, mode=IL→OB)
        PE(ps=2, pd=4, pli=63, plo=2, mode=IL→OB)
        PE(ps=2, pd=5, pli=63, plo=3, mode=IL→OB)
```

**hybridacc-cc（錯誤）：**
```
Bus 0: PE(ps=0, pd=0, pli=0, plo=0, mode=IB→OL)
        PE(ps=1, pd=1, pli=1, plo=1, mode=IL→OL)
        PE(ps=2, pd=2, pli=2, plo=2, mode=IL→OL)
        PE(ps=3, pd=3, pli=3, plo=0, mode=IL→OB)  ← last PE
Bus 1: PE(ps=4, pd=4, pli=1, plo=4, mode=IB→OL)
        PE(ps=5, pd=5, pli=5, plo=5, mode=IL→OL)
        PE(ps=6, pd=6, pli=6, plo=6, mode=IL→OL)
        PE(ps=7, pd=7, pli=7, plo=1, mode=IL→OB)
Bus 2: PE(ps=8, pd=8, pli=2, plo=8, mode=IB→OL)
        PE(ps=9, pd=9, pli=9, plo=9, mode=IL→OL)
        PE(ps=10,pd=10,pli=10,plo=10,mode=IL→OL)
        PE(ps=11,pd=11,pli=11,plo=2, mode=IL→OB)
```

---

## 2. Route Mode 分配（CRITICAL）

### 2.1 問題描述

Route mode 決定 PLI/PLO 資料的流向，為 NoC 中 partial sum 的累積鏈建立路徑。

### 2.2 正確行為 vs hybridacc-cc

**正確（noc_gen/cluster_gen）— bus 級別的路由：**

```
                   Bus 0 (kernel row 0)
                   ┌──────────────────────┐
         PLI ─────►│ PE0  PE1  PE2  PE3   │──── PLO to LN (mode: IB→OL)
         (from bus)│                      │     ↓
                   └──────────────────────┘
                   Bus 1 (kernel row 1)         ↓ (through LN = Local Neighbor)
                   ┌──────────────────────┐
         PLI ─────►│ PE4  PE5  PE6  PE7   │──── PLO to LN (mode: IL→OL)
         (from LN) │                      │     ↓
                   └──────────────────────┘
                   Bus 2 (kernel row 2)         ↓
                   ┌──────────────────────┐
         PLI ─────►│ PE8  PE9  PE10 PE11  │──── PLO to BUS (mode: IL→OB)
         (from LN) │                      │     → DMA store back to DRAM
                   └──────────────────────┘
```

每個 **bus 整體** 扮演一個路由節點，partial sum 從 bus 0 流到 bus 2 做 kernel 高度方向累積。

**hybridacc-cc — PE 級別的路由（錯誤）：**

```
Bus 0:  PE0 (IB→OL) → PE1 (IL→OL) → PE2 (IL→OL) → PE3 (IL→OB)
Bus 1:  PE4 (IB→OL) → PE5 (IL→OL) → PE6 (IL→OL) → PE7 (IL→OB)
Bus 2:  PE8 (IB→OL) → PE9 (IL→OL) → PE10(IL→OL) → PE11(IL→OB)
```

這把 partial sum 的累積鏈建在了每個 bus **內部** PE 之間，而非 bus 之間。
Conv2D 3×3 需要 KH=3 個 bus 做垂直累積，但 hybridacc-cc 的鏈是在 bus 內水平推進，完全不會跨 bus 累積。

### 2.3 影響

- **Conv2D 3×3 的 kernel height 維度累積完全失效**
- 每個 bus 內部 PE 做了無意義的 chain，PLO 輸出是錯誤的 partial sum
- 最終 DMA store 讀到的 PLO 資料是單一 kernel row 的結果，而非 KH 行累積結果

---

## 3. AGU Stride 單位（CRITICAL）

### 3.1 問題描述

AGU 硬體的 stride 單位為 **word64**（64-bit word = 8 bytes），由 `AddressGenerateUnit.hpp` 明確定義：

```cpp
REG_STRIDE0 = 0x10, // 32-bit stride for idx0 (unit: word64)
REG_STRIDE1 = 0x14, // 32-bit stride for idx1 (unit: word64)
REG_STRIDE2 = 0x18, // 32-bit stride for idx2 (unit: word64)
REG_STRIDE3 = 0x1C, // 32-bit stride for idx3 (unit: word64)
```

地址計算公式：
$$\text{addr} = \text{base\_addr} + \sum_{i=0}^{3} \text{idx}[i] \times \text{stride}[i]$$
全部以 word64 為單位。

### 3.2 比對

| 項目 | cluster_gen（正確） | hybridacc-cc（錯誤） |
|---|---|---|
| stride 單位 | word64（stride0=1 = 步進 1 個 64-bit word） | bytes（stride0=PKT_SIZE=8 = 步進 8 個 64-bit word = 64 bytes）|
| base_addr 單位 | word64（透過 `_to_group_local_word_addr()` 轉換）| bytes（直接用 `half_cap` 等 byte 值）|
| 計算方式 | `stride0 = 1` | `stride0 = PKT_SIZE = 8` |

### 3.3 cluster_gen 的轉換函式

```python
def _to_group_local_word_addr(addr_bytes: int) -> int:
    spm_word_bytes = 8
    spm_group_span_words = 8192 * 4
    return (int(addr_bytes) // spm_word_bytes) % spm_group_span_words
```

### 3.4 hybridacc-cc 的 AGU 配置

```python
PKT_SIZE = 8  # bytes per SPM transaction (64-bit)

# conv3x3 AGU PS
agu_ps = AguBankConfig(
    base_addr=0,                        # ← 碰巧正確（byte 0 = word 0）
    stride0=PKT_SIZE,                   # ← 8 word64 = 64 bytes ≠ 正確的 1 word64
    stride1=in_ch_pack * PKT_SIZE,      # ← 8 word64 ≠ 正確的 in_ch_pack word64
    stride2=KW * in_ch_pack * PKT_SIZE, # ← 24 word64 ≠ 正確的 KW * in_ch_pack word64
    stride3=KH * KW * in_ch_pack * PKT_SIZE, # ← ...
    ...
)
```

### 3.5 影響

- **所有 AGU 地址計算偏移量都是正確值的 8 倍**
- SPM 讀取位置完全錯位，讀到的是錯誤資料或空白區域
- base_addr 相關的 ping/pong 切換（`agu_pong = [half_cap, half_cap, 0, 0]`）也使用了 byte 值，但 AGU 期望 word64
- 此 bug 影響全部 4 個 AGU bank（PS、PD、PLI、PLO）的全部 operator（conv3x3、conv1x1、gemm）

---

## 4. AGU tag_ctrl 配置（CRITICAL for PS）

### 4.1 問題描述

AGU 的 `tag_ctrl` 決定了 NoC 封包的 tag 跟隨哪一層 loop index 更新。Tag 用來匹配 scan chain 中 PE 的 `ps_id`/`pd_id`/`pli_id`/`plo_id`。

Tag 公式：
$$\text{tag} = \text{tag\_base} + \text{idx}[\text{tag\_ctrl}] \times \text{tag\_stride}[\text{tag\_ctrl}]$$

### 4.2 比對（Conv2D 3×3）

| AGU | Loop Order (iter0→3) | cluster_gen tag_ctrl | hybridacc-cc tag_ctrl | 對應的 tag 維度 |
|---|---|---|---|---|
| **PS** (weight) | [ic_pack, **KW**, **KH**, tile_oc] | **2** → tag 跟 KH（kernel height）| **1** → tag 跟 KW（kernel width）| cluster_gen: tag=kernel_row → 匹配 ps_id=bus_idx ✓ |
| PD (input) | [ic_pack, H_in, W_in, 1] | 1 → tag 跟 H_in | 1 → tag 跟 H_in | 兩者皆以 input height 為 tag ✓ |
| PLI | [oc_pack, H_out, W_out, 1] | 1 → tag 跟 H_out | 1 → tag 跟 H_out | 兩者皆以 output height 為 tag ✓ |
| PLO | [oc_pack, H_out, W_out, 1] | 1 → tag 跟 H_out | 1 → tag 跟 H_out | 兩者皆以 output height 為 tag ✓ |

### 4.3 影響

- PS AGU 的 tag 跟著 KW（kernel width）遞增而非 KH（kernel height）
- 即使 ps_id 分配正確（假設第 1 個 bug 被修），tag 也無法正確匹配到對應 bus 的 PE
- Weight 資料會被送到完全錯誤的 PE

---

## 5. 非活躍 PE 處理（MODERATE）

### 5.1 問題描述

當 PE 數量超過實際需要（例如 12 PE 但只需 3 bus × 4 PE = 12 中的 3×4=12 active），多餘的 PE 應被停用。

### 5.2 比對

| 項目 | noc_gen / cluster_gen | hybridacc-cc |
|---|---|---|
| Excess PE 處理 | `enable=False`, 所有 ID 設為 `63`（null routing）| **所有 PE 都 enable=True** |
| 停用判定 | `i < split_kh and j < out_h_final` | 無判定邏輯 |

### 5.3 影響

- 超出有效範圍的 PE 仍嘗試接收 NoC 封包（但 ID=gid 可能不會匹配到任何 tag）
- 若碰巧匹配上，會消耗 NoC 資料並汙染累積鏈
- 更重要的是，如果 out_h < pes_per_bus，部分 PE 不應參與計算

---

## 6. HDDU 配置

### 6.1 比對

| 項目 | cluster_gen | hybridacc-cc |
|---|---|---|
| Conv2D Normal | `plane_en=0xF, plane_mode=0x1` | `plane_en=0xF, plane_mode=0x1` |
| Conv2D Ultra | `plane_en=0xF, plane_mode=0x2` | 尚未實作 |
| GEMM | `plane_en depends on k-split` | `plane_en=0xB, plane_mode=0x2` |

**結論：Conv2D Normal mode 的 HDDU 配置正確。**

---

## 7. AGU Iteration Dimensions — 結構比對

### 7.1 Conv2D 3×3 PS (Weight) AGU

| 維度 | cluster_gen | hybridacc-cc | 匹配 |
|---|---|---|---|
| iter0 | ic_pack (`ceil(IC/4)`) | in_ch_pack (`IC/4`) | ✓ 語義相同 |
| iter1 | kernel_size (KW) | KW | ✓ |
| iter2 | kernel_size (KH) | KH | ✓ |
| iter3 | count_oc | tile_oc | ✓ |
| **stride0** | **1** | **8 (PKT_SIZE)** | **✗ 8x 偏差** |
| **stride1** | **ic_pack** | **ic_pack × 8** | **✗ 8x 偏差** |
| **stride2** | **KW × ic_pack** | **KW × ic_pack × 8** | **✗ 8x 偏差** |
| **stride3** | **KH × KW × ic_pack** | **KH × KW × ic_pack × 8** | **✗ 8x 偏差** |

### 7.2 Conv2D 3×3 PD (Input) AGU

| 維度 | cluster_gen | hybridacc-cc | 匹配 |
|---|---|---|---|
| iter0 | ic_pack | in_ch_pack | ✓ |
| iter1 | ih_end - ih_start (tile_h_in) | tile_h_in | ✓ |
| iter2 | in_w (tile_w_in) | tile_w_in | ✓ |
| iter3 | 1 | 1 | ✓ |
| **stride0** | **1** | **8** | **✗** |
| **stride1** | **in_w × ic_pack** | **tile_w_in × ic_pack × 8** | **✗** |
| **stride2** | **ic_pack** | **ic_pack × 8** | **✗** |

### 7.3 Conv2D 3×3 PLI/PLO AGU

| 維度 | cluster_gen | hybridacc-cc | 匹配 |
|---|---|---|---|
| iter0 | oc_pack (`ceil(OC/4)`) | out_ch_pack (`OC/4`) | ✓ |
| iter1 | out_h | tile_h_out | ✓ |
| iter2 | out_w | tile_w_out | ✓ |
| **stride0** | **1** | **8** | **✗** |
| **stride1** | **out_w × oc_pack** | **tile_w_out × oc_pack × 8** | **✗** |
| **stride2** | **oc_pack** | **oc_pack × 8** | **✗** |

**結論：Iteration count 維度正確，但所有 stride 值都有 8× 的系統性偏差。**

---

## 8. SPM 相關配置

### 8.1 SPM_CONFIG_MAP

| 項目 | cluster_gen | hybridacc-cc | 匹配 |
|---|---|---|---|
| Even map | 0xE4 | 0xE4 | ✓ |
| Odd map | 0xD8 | 0xD8 | ✓ |
| 切換依據 | ic & 1 | ic & 1 | ✓ |

### 8.2 DMA SPM 地址

| 項目 | cluster_gen | hybridacc-cc | 備註 |
|---|---|---|---|
| Group base | 透過 `_build_spm_dma_plan` 計算 | `hw.spm_dma_group_base(N)` | ✓ 已修正 |
| Ping/pong | 由 wave_id 奇偶決定 section | 由 `wave_count & 1` 和 `ic & 1` 決定 | ✓ 概念相同 |

---

## 9. 問題嚴重度總覽

| # | 問題 | 嚴重度 | 影響範圍 | 修復複雜度 |
|---|---|---|---|---|
| 1 | Scan chain ID 分配錯誤 | **CRITICAL** | 全部 operator | 需重寫 `compute_scan_chain()` |
| 2 | Route mode 分配錯誤 | **CRITICAL** | conv2d_3x3 | 需改為 bus-level routing |
| 3 | AGU stride 使用 byte 而非 word64 | **CRITICAL** | 全部 operator | 移除所有 `× PKT_SIZE` |
| 4 | PS AGU tag_ctrl 錯誤 | **CRITICAL** | conv2d_3x3 | 改 tag_ctrl: 1→2 |
| 5 | 非活躍 PE 未停用 | MODERATE | 當 PE > needed 時 | 加入 enable/disable 邏輯 |
| 6 | AGU base_addr 單位為 byte（應為 word64） | **CRITICAL** | ping/pong 切換 | 除以 8 |

---

## 10. 修復建議

### 10.1 Scan Chain（Bug #1, #2）

重寫 `compute_scan_chain()`，接受 operator 語義參數：

```python
def compute_scan_chain_conv2d(
    num_pes: int, num_bus: int,
    kernel_height: int,      # 使用幾個 bus
    out_h: int,              # PE per bus 中有幾個活躍
    stride: int = 1,
) -> List[ScanChainEntry]:
    pes_per_bus = num_pes // num_bus
    entries = []
    for bus_idx in range(num_bus):
        for pe_local in range(pes_per_bus):
            active = (bus_idx < kernel_height and pe_local < out_h)
            if active:
                ps_id = bus_idx                        # weight: bus-level broadcast
                pd_id = (bus_idx + pe_local) * stride  # input: per-PE unique row
                if bus_idx == 0:
                    route_mode = 1  # PLI_FROM_BUS_PLO_TO_LN
                    pli_id = pe_local
                    plo_id = pe_local  # (pass to LN)
                elif bus_idx == kernel_height - 1:
                    route_mode = 2  # PLI_FROM_LN_PLO_TO_BUS
                    pli_id = pe_local  # (from LN)
                    plo_id = pe_local
                else:
                    route_mode = 0  # PLI_FROM_LN_PLO_TO_LN
                    pli_id = pe_local
                    plo_id = pe_local
            else:
                ps_id = pd_id = pli_id = plo_id = 63
                route_mode = 3  # PLI_FROM_BUS_PLO_TO_BUS (passthrough)
            entries.append(ScanChainEntry(
                ps_id=ps_id, pd_id=pd_id,
                pli_id=pli_id, plo_id=plo_id,
                route_mode=route_mode, enable=active,
            ))
    return entries
```

### 10.2 AGU Stride（Bug #3）

移除所有 `× PKT_SIZE`，改用 word64 單位（stride=1 表示一個 64-bit word）：

```python
# Before (WRONG):
stride0 = PKT_SIZE                           # 8
stride1 = in_ch_pack * PKT_SIZE              # 8
stride2 = KW * in_ch_pack * PKT_SIZE         # 24

# After (CORRECT):
stride0 = 1                                  # 1 word64
stride1 = in_ch_pack                         # 1 word64
stride2 = KW * in_ch_pack                    # 3 word64
```

### 10.3 AGU base_addr（Bug #6）

將所有 byte 地址轉換為 word64：

```python
WORD64_BYTES = 8

# Before (WRONG):
agu_pong = [half_cap, half_cap, 0, 0]  # half_cap in bytes

# After (CORRECT):
agu_pong = [half_cap // WORD64_BYTES, half_cap // WORD64_BYTES, 0, 0]
```

### 10.4 PS tag_ctrl（Bug #4）

Conv2D 3×3 的 PS AGU tag_ctrl 改為 2（跟隨 KH 維度）：

```python
# Before (WRONG):
agu_ps = AguBankConfig(..., tag_ctrl=1, ...)  # tag follows KW

# After (CORRECT):
agu_ps = AguBankConfig(..., tag_ctrl=2, ...)  # tag follows KH (kernel height = bus index)
```

---

## 附錄 A：完整 AGU 參數對照表（Conv2D 3×3, IC=4, OC=16, KH=3, KW=3, tileH=7, tileW=7）

### PS (Weight) AGU

| Register | cluster_gen 值 | hybridacc-cc 值 | 正確值 |
|---|---|---|---|
| base_addr | word64 地址 | 0 | 0（word64）|
| iter0 | 1 (ic_pack) | 1 | 1 |
| iter1 | 3 (KW) | 3 | 3 |
| iter2 | 3 (KH) | 3 | 3 |
| iter3 | 16 (OC) | 16 | 16 |
| stride0 | 1 | 8 | **1** |
| stride1 | 1 | 8 | **1** |
| stride2 | 3 | 24 | **3** |
| stride3 | 9 | 72 | **9** |
| tag_base | 0 | 0 | 0 |
| tag_stride0 | 1 | 1 | 1 |
| tag_stride1 | 1 | 0 | **1** |
| tag_ctrl | 2 | 1 | **2** |
| ctrl | ultra-dependent | 0x0 | 0x0 (linear) |

### PD (Input) AGU

| Register | cluster_gen 值 | hybridacc-cc 值 | 正確值 |
|---|---|---|---|
| base_addr | word64 地址 | 0 | 0（word64）|
| iter0 | 1 | 1 | 1 |
| iter1 | 9 (tile_h_in) | 9 | 9 |
| iter2 | 9 (tile_w_in) | 9 | 9 |
| stride0 | 1 | 8 | **1** |
| stride1 | 9 | 72 | **9** |
| stride2 | 1 | 8 | **1** |
| tag_base | 0 | 0 | 0 |
| tag_stride0 | 0 | 1 | **0** |
| tag_stride1 | 1 | 0 | ※ tag_ctrl=1, 所以此值決定 tag stride |
| tag_ctrl | 1 | 1 | 1 |

### PLI / PLO AGU

| Register | cluster_gen 值 | hybridacc-cc 值 | 正確值 |
|---|---|---|---|
| base_addr | word64 地址 | 0 | 0（word64）|
| iter0 | 4 (oc_pack) | 4 | 4 |
| iter1 | 7 (tile_h_out) | 7 | 7 |
| iter2 | 7 (tile_w_out) | 7 | 7 |
| stride0 | 1 | 8 | **1** |
| stride1 | 28 | 224 | **28** |
| stride2 | 4 | 32 | **4** |
| tag_ctrl | 1 | 1 | 1 |

---

## 附錄 B：Tag Matching 流程圖

```
┌─────────────┐      tag = idx[tag_ctrl]      ┌──────────────────────┐
│  AGU PS     │ ──────────────────────────────►│ NoC Packet (PS port) │
│  tag_ctrl=2 │                                │  tag = kernel_row    │
│  iter2 = KH │                                └──────────┬───────────┘
└─────────────┘                                           │
                                                          ▼
                    ┌─────────────────────────────────────────────────┐
                    │ NoC Router: 比對 pkt.tag vs PE.scan_chain.ps_id │
                    │                                                 │
                    │  Bus 0: PE(ps_id=0) → match tag=0 → 接收 ✓     │
                    │  Bus 1: PE(ps_id=1) → match tag=1 → 接收 ✓     │
                    │  Bus 2: PE(ps_id=2) → match tag=2 → 接收 ✓     │
                    └─────────────────────────────────────────────────┘
```

---

*報告產生時間：2026-04-09*
*分析範圍：hybridacc-cc lowering.py (`compute_scan_chain`, `_lower_conv2d_3x3`) vs hybridacc_verify cluster_gen.py / noc_gen.py*
