# HybridAcc-CC：算子 Lowering 詳細規格

> 前置閱讀：[00_Overview.md](00_Overview.md)、[01_CompilationPipeline.md](01_CompilationPipeline.md)
> SPM 硬體規格：[SPM.md](../../hybridacc-ESL/doc/SPM.md)
> 參考圖示：[conv2D_image.png](conv2D_image.png)、[GEMM_image.png](GEMM_image.png)

---

## 1. 總覽

Operator Lowering 是 Stage 1 的核心任務，負責將每個高層算子 `OpDesc` 轉變為可直接驅動硬體的 `LayerHwConfig`。本文件詳細描述三種支援算子的 lowering 演算法：

1. **Conv2D 3×3**（`conv2d_3x3`）→ PE template `conv1d_k3c4s1`
2. **Conv2D 1×1**（`conv2d_1x1`）→ PE template `conv1d_k1c12s1`
3. **GEMM**（`gemm`）→ PE template `gemm`

每個算子的 lowering 包含以下子步驟：

```
a) Tiling 策略決定（wave tile 維度）
b) 精確 SPM per-group 容量驗證
c) SPM per-group address layout 計算（含 ping-pong 雙緩衝）
d) PE template 選擇與參數計算（基於 tiled 維度）
e) AGU bank 配置計算（PS/PD/PLI/PLO）
f) Scan-chain（PE router 拓撲）計算
g) Multi-cluster mapping 決定
h) DMA descriptor 生成
i) Temporal schedule 生成
```

> **重要變更**：Tiling 必須在 PE template 參數計算之前完成，因為 PE 看到的是 wave-level 的 tensor shape，而非原始的完整 tensor shape。SPM 分配以 per-group 容量為準（參照 SPM.md），不得使用 flat 總容量估算。

---

## 2. 共通概念

### 2.1 術語

| 符號 | 定義 |
|------|------|
| `C_in` | 輸入通道數 |
| `OC` | 輸出通道數 |
| `KH`, `KW` | 卷積核高度、寬度 |
| `H_in`, `W_in` | 輸入特徵圖高度、寬度 |
| `H_out`, `W_out` | 輸出特徵圖高度、寬度 |
| `pkt_size` | 單一 AGU transaction 的位元組數（SPM 64-bit port → 8 bytes） |
| `in_ch_pack` | `ceil(C_in / channels_per_cycle)`，每個 channel group 對應的 transaction 數 |
| `out_ch_pack` | `ceil(OC / 4)`，輸出通道分組數 |
| `num_pes` | 每個 cluster 的 PE 數量 |
| `num_bus` | 每個 cluster 的 bus 數量 |
| **`wave`** | **一次完整的硬體執行週期（AGU start → compute done → stop），處理 tiled 子問題** |
| **`wave_tile`** | **一個 wave 所處理的 tensor 子區塊維度** |
| **`temporal_loop`** | **遍歷所有 wave tiles 的外層迴圈（由 firmware 控制）** |
| **`group_capacity`** | **SPM 單一 Group 的可用容量（bytes）** |
| **`ping_buf` / `pong_buf`** | **同一 Group 內的兩組地址分區，交替供 compute 和 DMA 使用** |

### 2.2 Packing 規則

HybridAcc PE 使用 fp16（16-bit half-precision）資料格式。SPM 的 data port 為 64-bit（8 bytes），因此：

- 一個 SPM transaction = 8 bytes = 4 個 fp16 elements
- `pkt_size = 8`（固定常數）

### 2.3 SPM 物理架構

> 以 [SPM.md](../../hybridacc-ESL/doc/SPM.md) 為準進行空間規劃。

SPM 為每個 cluster 內的多 Bank 分組記憶體，物理結構如下：

- **SRAM Banks 總數**：`TOTAL_BANKS = NUM_GROUPS × BANKS_PER_GROUP = 4 × 3 = 12`
- **分組**：12 個 Bank 劃分為 **4 個 Group**（Group 0 ~ Group 3），每組 3 個 Bank
- **單一 Bank**：深度 `BANK_DEPTH` words，每 word `BANK_DATA_WIDTH = 64` bits = 8 bytes
- **NoC 介面**：4 組 NoC Slave Port（192-bit），每個 Port 固定映射到一個 Group
- **DMA 介面**：1 組 AXI4-Lite（64-bit），僅支援 Linear 模式存取

```
SPM 物理架構圖：

              NoC Port 0 (PS)    NoC Port 1 (PD)    NoC Port 2 (PLI)   NoC Port 3 (PLO)
                   │                  │                   │                  │
                   v                  v                   v                  v
            ┌─────────────┐   ┌─────────────┐    ┌─────────────┐   ┌─────────────┐
  Group 0   │ Bank 0      │   │ Bank 3      │    │ Bank 6      │   │ Bank 9      │
  (PS)      │ Bank 1      │   │ Bank 4      │    │ Bank 7      │   │ Bank 10     │
            │ Bank 2      │   │ Bank 5      │    │ Bank 8      │   │ Bank 11     │
            └─────────────┘   └─────────────┘    └─────────────┘   └─────────────┘
              Group 0           Group 1             Group 2           Group 3
              (Weight)         (Activation)        (PSum In)         (Output)
                                                                         ^
                                                                         │
                                                          AXI4-Lite DMA (64-bit, any group)
```

### 2.4 SPM Per-Group 容量計算

```python
# 每 Group 包含的 SRAM 容量
group_linear_words = BANKS_PER_GROUP * BANK_DEPTH      # 3 × 8192 = 24576 words (BANK_DEPTH=8192)
group_capacity     = group_linear_words * BYTES_PER_BANK_WORD  # 24576 × 8 = 196608 bytes = 192 KB

# 每 Group 的地址空間（含 Parallel region）
group_span_words   = (BANKS_PER_GROUP + 1) * BANK_DEPTH  # 4 × 8192 = 32768 words
```

> **關鍵**：Lowering 階段的 SPM 空間規劃以 **per-group `group_capacity`** 為單位，而非 SPM 總容量。
> 4 個 AGU bank（PS/PD/PLI/PLO）分別映射到 4 個獨立 Group，各自擁有 `group_capacity` bytes 的可用空間。

| Port | AGU Bank | 方向 | Group 映射 | 可用容量 |
|------|----------|------|-----------|---------|
| 0 | PS (bank 0) | Read | Group 0 | `group_capacity` |
| 1 | PD (bank 1) | Read | Group 1 | `group_capacity` |
| 2 | PLI (bank 2) | Read | Group 2 | `group_capacity` |
| 3 | PLO (bank 3) | Write | Group 3 | `group_capacity` |

```python
spm_config_map = 0xE4  # port0→group0, port1→group1, port2→group2, port3→group3（1:1 映射）
```

### 2.5 SPM Per-Group Address Layout（強制 Ping-Pong）

每個 Group 內部的地址空間獨立規劃。一個 Group 有兩種存取模式（見 SPM.md §3.1）：

- **Linear Mode**（`0 ≤ addr < group_linear_words`）：存取單一 Bank，64-bit 寬
- **Parallel Mode**（`group_linear_words ≤ addr < group_span_words`）：同時存取 Group 內 3 個 Bank，192-bit 寬

HDDU/AGU 存取模式依據算子需求選擇：

| 算子 | PS (Group 0) | PD (Group 1) | PLI (Group 2) | PLO (Group 3) |
|------|-------------|-------------|---------------|---------------|
| Conv2D 3×3 | Linear | Linear | Linear | Linear |
| Conv2D 1×1 | **Parallel** | Linear | **Parallel** | **Parallel** |
| GEMM | **Parallel** | Linear | **Parallel** | **Parallel** |

> 使用 Parallel Mode 的 Group 同時設定 AGU **ultra** 模式（`CTRL.bit3=1`），使 NoC 將 192-bit 資料拆分給 3 個 port（每 port 64-bit），而非 broadcast。
>
> Conv2D 1×1 需要 ultra 是因為 `channels_per_cycle=12`，須透過 3 port × 4 channels 同時傳輸；Conv2D 3×3 的 `channels_per_cycle=4` 僅需 broadcast 即可。

DMA 一律使用 **Linear Mode** 寫入 SPM（AXI4-Lite 64-bit 介面只能存取單一 Bank）。

> **強制規則**：**所有 Group 一律使用 Ping-Pong 雙緩衝**，以隱藏 DMA 搬運延遲、加速運算。不存在 single buffer 降級模式。因此每個 Group 的有效可用容量為 `half_group_capacity = group_capacity // 2`。

#### Linear Mode Ping-Pong Layout

```
Group X Linear Layout（Conv2D 3×3 全部 Group / Conv2D 1×1・GEMM 的 PD Group）：
┌──────────────────────┐ 0x0000
│  PING buffer         │  wave_tile_bytes     ← compute 目前讀/寫此區
├──────────────────────┤ half_group_capacity (98304)
│  PONG buffer         │  wave_tile_bytes     ← DMA 同時搬運下一波資料到此區
├──────────────────────┤ group_capacity (196608)
└──────────────────────┘

AGU BASE_ADDR：PING = 0, PONG = half_group_capacity
約束：wave_tile_bytes ≤ half_group_capacity
```

#### Parallel Mode Ping-Pong Layout

```python
# Parallel mode 地址常數
parallel_base     = group_linear_words * BYTES_PER_BANK_WORD  # 24576 × 8 = 196608
bank_depth_bytes  = BANK_DEPTH * BYTES_PER_BANK_WORD          # 8192 × 8 = 65536
half_parallel     = bank_depth_bytes // 2                      # 32768

parallel_ping_base = parallel_base                   # 196608
parallel_pong_base = parallel_base + half_parallel   # 229376
```

```
Group X Parallel Layout（Conv2D 1×1・GEMM 的 PS/PLI/PLO Group）：

Linear Region（DMA 寫入用，每個 Bank 獨立定址）：
┌──────────────────────┐ 0x00000  (byte addr 0)
│  Bank 0 PING         │  ← DMA 寫入 port 0 的資料
├──────────────────────┤ half_parallel (32768)
│  Bank 0 PONG         │
├──────────────────────┤ bank_depth_bytes (65536)
│  Bank 1 PING         │  ← DMA 寫入 port 1 的資料
├──────────────────────┤ bank_depth_bytes + half_parallel (98304)
│  Bank 1 PONG         │
├──────────────────────┤ 2 × bank_depth_bytes (131072)
│  Bank 2 PING         │  ← DMA 寫入 port 2 的資料
├──────────────────────┤ 2 × bank_depth_bytes + half_parallel (163840)
│  Bank 2 PONG         │
├──────────────────────┤ group_capacity (196608)
└──────────────────────┘

Parallel Region（AGU/HDDU 存取用，3 Banks 同時讀寫 192-bit）：
┌──────────────────────┐ parallel_base (196608)
│  PING buffer         │  ← AGU ultra mode: 每個 addr 讀/寫 3 Banks
├──────────────────────┤ parallel_pong_base (229376)
│  PONG buffer         │
├──────────────────────┤ parallel_base + bank_depth_bytes (262144)
└──────────────────────┘

AGU BASE_ADDR：PING = parallel_ping_base (196608), PONG = parallel_pong_base (229376)
每個 addr 提供 3 × 64-bit = 192-bit → 3 port 各得 64-bit
half_parallel 內可容納的 AGU 迭代數 = half_parallel / pkt_size = 4096
等效資料容量 = 4096 × 3 × 8 = 98304 bytes = half_group_capacity（與 Linear mode 相同）
```

> **容量等價性**：Parallel mode 的 half_parallel (32768 地址空間) 與 Linear mode 的 half_group_capacity (98304 地址空間) 可容納的**實際資料量相同**（均為 96 KB）。因此 tiling 搜尋的 `wave_tile_bytes ≤ half_cap` 約束在兩種模式下等價，搜尋演算法不需修改。

> **DMA 與 Compute 不得重疊**：Compute 使用 PING buffer 地址範圍時，DMA 只能寫入 PONG buffer 地址範圍，反之亦然。每個 wave 結束後交換 PING/PONG 角色（透過更新 AGU `BASE_ADDR`）。
> 對於 Parallel mode 的 Group，DMA 需分別寫入 3 個 Bank 的 PING 或 PONG 半段（見各算子的 DMA Descriptor 章節）。

### 2.6 Ping-Pong 雙緩衝策略（強制啟用）

> **設計決策**：為了有效隱藏 DMA 搬運延遲，SPM **強制**對所有 Group 規劃 ping-pong buffer。Tiling 大小必須確保每個 wave tile 能 fit 進 **半個 Group 容量**。

```python
half_group_capacity = group_capacity // 2  # 每個 buffer（ping 或 pong）的容量上限

def compute_pingpong_layout(wave_tile_bytes: int, group_capacity: int) -> dict:
    """為指定 Group 計算 ping-pong 地址配置（強制雙緩衝）"""
    half_cap = group_capacity // 2
    if wave_tile_bytes > half_cap:
        raise TilingFailed(
            f"Wave tile ({wave_tile_bytes} bytes) exceeds "
            f"half group capacity ({half_cap} bytes). "
            f"Reduce tile dimensions."
        )
    return {
        "ping_base": 0,
        "pong_base": half_cap,  # PONG 固定在 group 後半段
        "buffer_size": wave_tile_bytes,
    }
```

> **PONG 地址固定為 `half_group_capacity`**（而非緊鄰 PING buffer 之後），這保證 PING 和 PONG 地址空間完全不重疊，即使 wave_tile_bytes 遠小於 half_group_capacity 也確保安全。

**多 tile 預載**：若 wave tile 遠小於 half group capacity，可在同一 half 內預載多個連續 tile（N-buffering），但初版只支援 double buffering（N=2）。

### 2.7 Tiling 框架

#### 2.7.1 Tiling 是必要步驟

> **所有算子都必須經過 tiling**（即使完整張量可 fit 進 SPM）。原因：
> 1. SPM 強制 ping-pong → tiling budget 為 `half_group_capacity`，而非 `group_capacity`
> 2. Conv2D 的 `tile_ic` 和 `tile_oc` 是 PE template 的固有限制，**一定要切**
> 3. Tiling 決定 PE template 參數值

#### 2.7.2 固定 Tile 維度 vs. 可變 Tile 維度

Conv2D 有四個 tiling 維度，其中兩個是由 PE template 固定的：

| 維度 | Conv2D 3×3 | Conv2D 1×1 | 性質 |
|------|-----------|-----------|------|
| `tile_ic` | `4`（channels_per_cycle） | `12`（channels_per_cycle） | **固定**，由 PE template 決定 |
| `tile_oc` | `min(OC, 16)` | `min(OC, 16)` | **固定**，由 PE 暫存器限制決定 |
| `tile_h` | 由 SPM 反推 | 由 SPM 反推 | **可變**，最大化以減少 DMA |
| `tile_w` | 由 SPM 反推 | 由 SPM 反推 | **可變**，最大化以減少 DMA |

GEMM 的三個維度均為可變：

| 維度 | 性質 |
|------|------|
| `M_tile`, `N_tile`, `K_tile` | **可變**，由 `half_group_capacity` 反推 |

#### 2.7.3 Tile 維度反推（從 SPM 半容量）

可變 tile 維度（`tile_h`, `tile_w`）由 **half_group_capacity** 反推：

```python
half_cap = group_capacity // 2

# 對 Conv2D，各 Group 的 wave tile bytes 表示為 tile_h, tile_w 的函數：
# PS:  f(tile_oc, tile_ic)          — 不含 tile_h/tile_w，只受限 tile_oc/tile_ic
# PD:  f(tile_h_in, tile_w_in, tile_ic)   — 受 tile_h, tile_w 影響
# PLI: f(tile_h_out, tile_w_out, tile_oc) — 受 tile_h, tile_w 影響
# PLO: 同 PLI

# 約束：max(ps_bytes, pd_bytes, pli_bytes, plo_bytes) ≤ half_cap
# 搜尋：最大化 tile_h × tile_w
```

#### 2.7.4 Wave = 一個硬體執行週期

一個 **wave** 代表一次完整的硬體計算，其生命週期為：

```
1. DMA 搬運 wave tile 資料到 SPM 的 PONG buffer（與前一 wave 的 compute 重疊）
2. 配置 HDDU/AGU（BASE_ADDR 指向本 wave 的 buffer：ping 或 pong）
3. 配置 NoC scan-chain + 載入 PE program（首 wave 必做，後續若不變可跳過）
4. 啟動 HDDU start_all
5. 等待 HDDU_STATUS done
6. DMA 搬出結果到 DRAM（與下一 wave 的 compute 重疊）
```

PE template 看到的張量形狀 = wave tile 維度（而非原始完整張量維度）。

#### 2.7.5 Temporal Loop 結構

Conv2D 的 temporal loop 必須有 **4 個維度**（oc, h, w, ic）：

```python
# Conv2D temporal loop（通用 4 維）
for oc_tile_idx in range(num_oc_tiles):         # OC tiling
    for h_tile_idx in range(num_h_tiles):        # spatial H tiling
        for w_tile_idx in range(num_w_tiles):    # spatial W tiling
            for ic_tile_idx in range(num_ic_tiles):  # IC tiling（partial sum accumulation）
                # 首個 ic_tile: PLI 初始為 0
                # 後續 ic_tile: PLI 讀取前一輪 PLO 的結果（cumulative partial sum）

                dma_load_wave(oc_tile_idx, h_tile_idx, w_tile_idx, ic_tile_idx, buf)
                configure_agu(buf)
                start_hddu()
                wait_hddu_done()

                if ic_tile_idx == num_ic_tiles - 1:
                    # IC 累加完成，DMA store output
                    dma_store_wave(oc_tile_idx, h_tile_idx, w_tile_idx, buf)

                buf = 1 - buf  # swap ping/pong
```

> **IC 累加**：與 GEMM 的 K 累加相同原理。IC 維度的多個 tile 產生 partial sum，需逐步累加。PLI/PLO base address 在 ic_tile 之間交替（ping ↔ pong）實現就地累加。

GEMM temporal loop 為 3 維 (n, m, k)：

```python
# GEMM temporal loop
for n_tile_idx in range(num_n_tiles):
    for m_tile_idx in range(num_m_tiles):
        for k_tile_idx in range(num_k_tiles):
            dma_load_wave(m_tile_idx, k_tile_idx, n_tile_idx, buf)
            configure_agu(buf)
            start_hddu()
            wait_hddu_done()
            if k_tile_idx == num_k_tiles - 1:
                dma_store_wave(m_tile_idx, n_tile_idx, buf)
            buf = 1 - buf
```

#### 2.7.6 Tiling 搜尋策略

搜尋目標：**在所有 Group 的 wave tile ≤ `half_group_capacity` 的約束下，最大化 tile_h × tile_w**（或 GEMM 的各 tile 維度）。

```python
half_cap = group_capacity // 2

# 搜尋步驟：
# 1. 固定 tile_ic, tile_oc（由 PE template 決定）
# 2. 計算 PS 的 wave tile bytes（只含 tile_oc, tile_ic）→ 驗證 ≤ half_cap
# 3. 從最大 tile_h (= H_out) 向下搜尋
# 4.   從最大 tile_w (= W_out) 向下搜尋
# 5.     計算 PD, PLI, PLO 的 wave tile bytes
# 6.     若全部 ≤ half_cap → 找到解
# 7. 若搜尋窮盡 → 報 TILING_FAILED
```

具體的搜尋演算法於 §3、§4、§5 各算子章節詳述。

### 2.8 Multi-Cluster Mapping

多個 cluster 可平行處理不同的 tile，實現 data parallelism。

#### 2.8.1 核心原則：避免跨 cluster 的 partial sum 同步

> **強制規則**：不同 cluster **不得同時計算同一個 output 位置的 partial sum**。
> 若多個 cluster 分別處理同一 output pixel 的不同 IC tile（或 GEMM 的不同 K tile），則需要跨 cluster 做 reduction（加總），這在硬體上極為昂貴且不支援。

因此，multi-cluster mapping 必須沿 **不產生 partial sum 依賴** 的維度分割：

| 算子 | 可分割維度 | 不可分割維度 | 理由 |
|------|-----------|-------------|------|
| Conv2D | `oc_tile`, `h_tile`, `w_tile` | `ic_tile` | IC 維度產生 partial sum，需累加 |
| GEMM | `n_tile`, `m_tile` | `k_tile` | K 維度產生 partial sum，需累加 |

#### 2.8.2 資料共用策略（減少 DMA 搬運）

不同分割維度對應不同的資料共用模式：

| 分割維度 | 共用的 tensor | 各 cluster 獨立的 tensor | DMA 節省 |
|---------|--------------|------------------------|---------|
| `h_tile` / `w_tile`（spatial） | Weight（全部 cluster 相同） | Input（不同 spatial 區域）、Output | Weight 可 broadcast，只搬一次 |
| `oc_tile` | Input（全部 cluster 相同） | Weight（不同 OC slice）、Output | Input 可 broadcast，只搬一次 |
| `n_tile`（GEMM） | A matrix（全部 cluster 相同） | B matrix（不同 N slice）、C | A 可 broadcast |
| `m_tile`（GEMM） | B matrix（全部 cluster 相同） | A matrix（不同 M slice）、C | B 可 broadcast |

> **建議**：優先沿能共用較大 tensor 的維度分割。例如 Conv2D 中若 weight 遠小於 input，沿 spatial 維度分割（共用 weight）效益較大。

#### 2.8.3 Mapping 演算法

```python
def compute_cluster_mapping(
    num_clusters: int,
    tiling: TilingResult,
    op_type: str,
) -> ClusterMapping:
    """
    將 output tiles 分配到 clusters。
    策略：沿不產生 partial sum 依賴的最大 tile 維度平均分配。
    IC/K 維度必須由同一 cluster 完整處理。
    """
    if op_type in ("conv2d_3x3", "conv2d_1x1"):
        # 可分割的 tile 維度：oc, h, w（不含 ic）
        # 優先沿最多 tiles 的維度分割
        spatial_tiles = tiling.num_h_tiles * tiling.num_w_tiles
        oc_tiles = tiling.num_oc_tiles

        # 選擇哪個維度切割最有效率
        if oc_tiles >= num_clusters:
            # 沿 OC 分割 → 各 cluster 共用 input activation
            split_dim = "oc_tile"
            total_parallel_tiles = oc_tiles
        elif spatial_tiles >= num_clusters:
            # 沿 spatial 分割 → 各 cluster 共用 weight
            split_dim = "spatial"
            total_parallel_tiles = spatial_tiles
        else:
            total_parallel_tiles = max(oc_tiles, spatial_tiles)
            split_dim = "oc_tile" if oc_tiles >= spatial_tiles else "spatial"

    elif op_type == "gemm":
        # 可分割：n, m（不含 k）
        nm_tiles = tiling.num_n_tiles * tiling.num_m_tiles
        total_parallel_tiles = nm_tiles
        split_dim = "n_tile" if tiling.num_n_tiles >= tiling.num_m_tiles else "m_tile"

    active_clusters = min(num_clusters, total_parallel_tiles)
    tiles_per_cluster = (total_parallel_tiles + active_clusters - 1) // active_clusters
    cluster_mask = (1 << active_clusters) - 1

    return ClusterMapping(
        active_clusters=active_clusters,
        cluster_mask=cluster_mask,
        split_dim=split_dim,
        tiles_per_cluster=tiles_per_cluster,
        tile_assignments=assign_tiles_round_robin(total_parallel_tiles, active_clusters),
        shared_tensor=get_shared_tensor(split_dim, op_type),
    )
```

#### 2.8.4 Code-Gen 中的 Loop Unrolling

後續 Stage 2（Code Generation）可透過 loop unrolling 處理平行運算：

```c
// 概念：多 cluster 平行處理不同 output tile
// 各 cluster 獨立完成自己的 IC/K 累加迴圈，無需跨 cluster 同步
//
// 若 N_clusters = 4, num_oc_tiles = 8（沿 OC 分割）
// → 每個 cluster 處理 2 OC tile passes
// → input activation broadcast 給所有 cluster

for (int wave = 0; wave < tiles_per_cluster; wave++) {
    int tile_idx = cluster_id * tiles_per_cluster + wave;
    if (tile_idx >= total_parallel_tiles) break;

    // broadcast 共用的 tensor（weight 或 input）
    // unicast 各 cluster 的 AGU base address / DMA（不同 tile 範圍）
    configure_cluster_unicast(cluster_id, tile_idx);

    // 每個 cluster 獨立執行完整的 IC/K 累加迴圈
    for (int ic = 0; ic < num_ic_tiles; ic++) {
        start_hddu_broadcast();
        wait_cluster_done(cluster_id);
    }
}
```

> **關鍵**：IC（或 K）迴圈 **在每個 cluster 內部完整執行**，不跨 cluster 切割。code-gen 使用 broadcast mode 共用 scan-chain / PE program，unicast mode 配置各 cluster 的 AGU base address 和 DMA descriptors。

### 2.9 精確 SPM 容量驗證

Lowering 必須對每個 Group **獨立**進行精確的容量檢查。由於強制 ping-pong，所有 Group 的 wave tile 必須 ≤ `half_group_capacity`：

```python
def validate_spm_capacity(
    wave_tile_sizes: Dict[str, int],  # {"ps": bytes, "pd": bytes, "pli": bytes, "plo": bytes}
    group_capacity: int,
) -> None:
    """
    精確驗證 SPM 容量。強制 ping-pong → 每個 wave tile ≤ half_group_capacity。
    """
    half_cap = group_capacity // 2
    group_names = {"ps": "Group 0 (Weight/PS)",
                   "pd": "Group 1 (Activ./PD)",
                   "pli": "Group 2 (PSum/PLI)",
                   "plo": "Group 3 (Output/PLO)"}

    for key in ["ps", "pd", "pli", "plo"]:
        needed = wave_tile_sizes[key]
        if needed > half_cap:
            raise TilingFailed(
                f"{group_names[key]}: wave tile {needed} bytes "
                f"exceeds half group capacity {half_cap} bytes. "
                f"Reduce tile dimensions."
            )
```

驗證時機：
1. **Tiling 搜尋內層**：每次候選 tile 維度都需通過此驗證
2. **Lowering 輸出後**：最終 `LayerHwConfig` 產出前再次驗證（防禦性檢查）

### 2.10 Scan-chain 通用邏輯

Scan-chain 定義了每個 PE 的 router 連線，決定 PS/PD/PLI/PLO 各走哪條資料路徑。

PE array 的拓撲為 `num_pes` 個 PE 串联在 `num_bus` 條 bus 上。每條 bus 上有 `num_pes / num_bus` 個 PE。

Scan-chain 以**反序（最後一個 PE 先 shift）**送入 NoC command window。

每個 PE 的 scan-chain entry 需要依據算子類型設定 `route_mode`：

| 算子 | 典型 route_mode | 理由 |
|------|----------------|------|
| conv2d（標準） | `PLI_FROM_LN_PLO_TO_LN` (0) | PLI/PLO 走 local neighbor |
| conv2d（首 PE） | `PLI_FROM_BUS_PLO_TO_LN` (1) | 首 PE 的 PLI 從 bus 讀入（初始 psum） |
| conv2d（末 PE） | `PLI_FROM_LN_PLO_TO_BUS` (2) | 末 PE 的 PLO 送回 bus |
| gemm | 視 tiling 而定 | 可能為 mode 0~3 的混合 |

### 2.11 Cluster Mask 決策

Cluster mask 依據 multi-cluster mapping 計算結果決定：

```python
def compute_cluster_mask(mapping: ClusterMapping) -> int:
    """啟用 mapping 中使用到的 cluster"""
    return mapping.cluster_mask  # e.g., 0xF for 4 clusters active
```

若 spatial tile 總數少於 cluster 數（例如只有 2 個 tile 但有 4 個 cluster），只啟用所需數量的 cluster，節省功耗。

---

## 3. Conv2D 3×3 Lowering

### 3.1 算子語義

計算 2D 卷積，kernel size = 3×3，stride 與 padding 可配置。

輸入：
- `input[N, H_in, W_in, C_in]`（NHWC layout，fp16）
- `weight[OC, 3, 3, C_in]`（OKHKWIC layout，fp16）

輸出：
- `output[N, H_out, W_out, OC]`（NHWC layout，fp16）

其中：
- `H_out = (H_in + 2*padding - 3) / stride + 1`
- `W_out = (W_in + 2*padding - 3) / stride + 1`

### 3.2 PE Template 選擇

使用 `conv1d_k3c4s1` template。此 template 的特性：
- Kernel size = 3，每 cycle 處理 channels_per_cycle = 4 個通道
- 支援 sliding window convolution
- 使用雙緩衝（KERNEL_LOOP_INNER 控制）

### 3.3 前置條件

| 條件 | 要求 | 處理 |
|------|------|------|
| `C_in % 4 == 0` | 通道對齊 | 若不滿足，報 `CHANNEL_ALIGN_ERROR` |
| `N == 1` | 初版限制 | 若不滿足，外層 batch loop 由多次 layer 呼叫實現 |

> **注意**：OC > 16 和 C_in > 4 的情況由 tiling 處理（`tile_oc`、`tile_ic` 維度切割），不再是前置條件限制。

### 3.4 Tiling 策略

> 參照 [conv2D_image.png](conv2D_image.png)

Conv2D 3×3 使用 **4 維 tiling**（oc, h, w, ic），所有維度都必須切割。SPM 強制 ping-pong，因此 tile 大小以 `half_group_capacity` 為上限反推。

#### 3.4.1 Wave Tile 維度定義

| 符號 | 定義 | 約束 | 性質 |
|------|------|------|------|
| `tile_ic` | 一個 wave 處理的 input channels | `= channels_per_cycle = 4` | **固定** |
| `tile_oc` | 一個 wave 產出的 output channels | `= min(OC, 16)` | **固定** |
| `tile_h_out` | 一個 wave 產出的 output H 維度 | ≥ 1，由 SPM 反推 | **可變** |
| `tile_w_out` | 一個 wave 產出的 output W 維度 | ≥ 1，由 SPM 反推 | **可變** |
| `tile_h_in` | 一個 wave 的 input H 維度 | `= tile_h_out + KH - 1`（含 halo） | 衍生 |
| `tile_w_in` | 一個 wave 的 input W 維度 | `= tile_w_out + KW - 1`（含 halo） | 衍生 |
| `num_ic_tiles` | IC 維度 tile 數 | `= C_in / tile_ic` | |
| `num_oc_tiles` | OC 維度 tile 數 | `= ceil(OC / tile_oc)` | |
| `num_h_tiles` | H 維度 tile 數 | `= ceil(H_out / tile_h_out)` | |
| `num_w_tiles` | W 維度 tile 數 | `= ceil(W_out / tile_w_out)` | |

> **Halo overlap**：3×3 convolution 在 stride=1 時，相鄰 H tile 之間需要 2 行 halo（上下各 1 行）。具體而言，tile i 處理 output rows `[r_start, r_end)` 時，其 input 行範圍為 `[r_start - padding, r_end - padding + KH - 1]`。

> **IC 累加**：當 `num_ic_tiles > 1` 時（即 C_in > tile_ic），每個 (oc, h, w) 組合需要跑 `num_ic_tiles` 次 wave，逐步累加 partial sum。第一個 ic_tile 的 PLI 初始為 0，後續 ic_tile 的 PLI 讀取前一輪 PLO 結果。

#### 3.4.2 Per-Group SPM 需求（使用 tiled 維度）

每個 wave tile 在各 Group 中的空間需求（**皆使用 tiled 維度，不是完整張量**）：

```python
def compute_conv2d_3x3_wave_sizes(
    tile_ic: int, tile_oc: int,
    tile_h_in: int, tile_w_in: int,
    tile_h_out: int, tile_w_out: int,
) -> Dict[str, int]:
    pkt_size = 8  # bytes per SPM transaction
    in_ch_pack = tile_ic // 4       # = 1（固定）
    out_ch_pack = tile_oc // 4      # = min(OC, 16) // 4

    # Group 0 (PS): Weight[tile_oc, KH, KW, in_ch_pack]
    ps_wave_bytes = tile_oc * 3 * 3 * in_ch_pack * pkt_size

    # Group 1 (PD): Activation[tile_h_in, tile_w_in, in_ch_pack]
    pd_wave_bytes = tile_h_in * tile_w_in * in_ch_pack * pkt_size

    # Group 2 (PLI): Partial-sum[tile_h_out, tile_w_out, out_ch_pack]
    pli_wave_bytes = tile_h_out * tile_w_out * out_ch_pack * pkt_size

    # Group 3 (PLO): Output — 與 PLI 相同大小
    plo_wave_bytes = pli_wave_bytes

    return {
        "ps": ps_wave_bytes,
        "pd": pd_wave_bytes,
        "pli": pli_wave_bytes,
        "plo": plo_wave_bytes,
    }
```

#### 3.4.3 Tiling 搜尋演算法

```python
def compute_conv2d_3x3_tiling(
    C_in: int, OC: int,
    H_in: int, W_in: int,
    stride: int, padding: int,
    group_capacity: int,
) -> Conv2DTiling:
    """
    搜尋滿足 per-group SPM 容量（半容量，強制 ping-pong）的最大 wave tile 維度。
    固定維度：tile_ic=4, tile_oc=min(OC,16)
    可變維度：tile_h_out, tile_w_out（從最大值向下搜尋）
    """
    half_cap = group_capacity // 2
    H_out = (H_in + 2 * padding - 3) // stride + 1
    W_out = (W_in + 2 * padding - 3) // stride + 1
    KH, KW = 3, 3
    halo = KH - 1  # = 2 for 3x3

    # 固定 tile 維度
    tile_ic = 4  # channels_per_cycle
    tile_oc = min(OC, 16)

    num_ic_tiles = C_in // tile_ic
    num_oc_tiles = (OC + tile_oc - 1) // tile_oc

    # 驗證 PS（weight）先 — 不含 spatial 維度
    in_ch_pack = tile_ic // 4  # = 1
    ps_wave = tile_oc * KH * KW * in_ch_pack * 8
    if ps_wave > half_cap:
        raise TilingFailed("Weight tile exceeds half group capacity")

    # 搜尋 tile_h_out, tile_w_out（最大化）
    for tile_h_out in range(H_out, 0, -1):
        tile_h_in = tile_h_out + halo
        for tile_w_out in range(W_out, 0, -1):
            tile_w_in = tile_w_out + halo

            sizes = compute_conv2d_3x3_wave_sizes(
                tile_ic, tile_oc,
                tile_h_in, tile_w_in,
                tile_h_out, tile_w_out)

            # 強制 ping-pong：所有 Group 的 wave tile ≤ half_cap
            if sizes["ps"]  > half_cap: continue
            if sizes["pd"]  > half_cap: continue
            if sizes["pli"] > half_cap: continue
            if sizes["plo"] > half_cap: continue

            num_h_tiles = (H_out + tile_h_out - 1) // tile_h_out
            num_w_tiles = (W_out + tile_w_out - 1) // tile_w_out

            return Conv2DTiling(
                tile_ic=tile_ic, tile_oc=tile_oc,
                tile_h_in=tile_h_in, tile_w_in=tile_w_in,
                tile_h_out=tile_h_out, tile_w_out=tile_w_out,
                num_ic_tiles=num_ic_tiles,
                num_oc_tiles=num_oc_tiles,
                num_h_tiles=num_h_tiles,
                num_w_tiles=num_w_tiles,
            )

    raise TilingFailed("Cannot tile conv2d_3x3 to fit half group capacity")
```

#### 3.4.4 Temporal Loop 結構（4 維）

```python
# Conv2D 3×3 temporal loop（firmware 層級，4 維）
buf = 0
for oc_tile_idx in range(num_oc_tiles):
    oc_start = oc_tile_idx * tile_oc
    for h_tile_idx in range(num_h_tiles):
        for w_tile_idx in range(num_w_tiles):
            for ic_tile_idx in range(num_ic_tiles):
                ic_start = ic_tile_idx * tile_ic

                # DMA load weight[oc_start:+tile_oc, :, :, ic_start:+tile_ic] → SPM Group 0
                # DMA load input[h_start:+tile_h_in, w_start:+tile_w_in, ic_start:+tile_ic] → SPM Group 1
                # 若 ic_tile_idx > 0: PLI 指向前一輪 PLO（累加 partial sum）
                # 若 ic_tile_idx == 0: PLI 填 0（初始 partial sum）
                dma_load_wave(oc_tile_idx, h_tile_idx, w_tile_idx, ic_tile_idx, buf)

                configure_agu(buf)
                start_hddu()
                wait_hddu_done()

                if ic_tile_idx == num_ic_tiles - 1:
                    # IC 維度完整累加完成，DMA store output
                    dma_store_output(oc_tile_idx, h_tile_idx, w_tile_idx, buf)

                buf = 1 - buf  # swap ping/pong
```

> **Total waves** = `num_oc_tiles × num_h_tiles × num_w_tiles × num_ic_tiles`

#### 3.4.5 Conv2D 3×3 Tiling 範例（參照 conv2D_image.png）

**輸入**：`input[1, 16, 200, 4]`, `weight[16, 3, 3, 4]`, stride=1, padding=0
- `C_in=4`, `OC=16`, `H_in=16`, `W_in=200`
- `H_out=14`, `W_out=198`
- `group_capacity = 192 KB = 196608 bytes`
- `half_group_capacity = 96 KB = 98304 bytes`

**固定 tile 維度**：`tile_ic=4`, `tile_oc=16`
- `num_ic_tiles = 4/4 = 1`
- `num_oc_tiles = 16/16 = 1`

**搜尋 tile_h_out, tile_w_out（最大化，≤ half_cap）**：

嘗試 `tile_h_out=14, tile_w_out=198`（完整空間）：

| Group | Buffer | 計算 | Size | ≤ 96KB? |
|-------|--------|------|------|---------|
| 0 (PS) | Weight | `16 × 3 × 3 × 1 × 8 = 1152` | 1.125 KB | ✓ |
| 1 (PD) | Input | `16 × 200 × 1 × 8 = 25600` | 25 KB | ✓ |
| 2 (PLI) | PSum | `14 × 198 × 4 × 8 = 88704` | 86.625 KB | ✓ |
| 3 (PLO) | Output | `14 × 198 × 4 × 8 = 88704` | 86.625 KB | ✓ |

所有 Group ≤ 96 KB ✓ → 完整空間 fit，不需要 spatial tiling。

**結論**：
- `tile_h_out=14, tile_w_out=198, tile_h_in=16, tile_w_in=200`
- `num_h_tiles = ceil(14/14) = 1`, `num_w_tiles = 1`
- `num_oc_tiles = 1`, `num_ic_tiles = 1`
- **Total waves = 1 × 1 × 1 × 1 = 1**
- 所有 Group 皆 ping-pong ✓

**SPM layout（per Group）**：
- Group 0 (PS): PING=0, PONG=98304, buffer=1152 bytes
- Group 1 (PD): PING=0, PONG=98304, buffer=25600 bytes
- Group 2 (PLI): PING=0, PONG=98304, buffer=88704 bytes
- Group 3 (PLO): PING=0, PONG=98304, buffer=88704 bytes

**Multi-cluster mapping**：
- 1 spatial tile → 1 cluster active
- 剩餘 3 clusters idle

**DMA 節省分析**：僅 1 cluster active，不需要 broadcast。

### 3.5 SPM Per-Group Layout 計算

> 強制 ping-pong：PONG base 固定在 `half_group_capacity`，所有 Group 皆雙緩衝。

```python
def compute_conv2d_3x3_spm_layout(
    tiling: Conv2DTiling,
    group_capacity: int,
) -> SpmPerGroupLayout:
    """
    計算 conv2d 3×3 的 per-group SPM layout。
    強制 ping-pong：PONG base 固定在 half_group_capacity。
    """
    half_cap = group_capacity // 2
    pkt_size = 8
    in_ch_pack = tiling.tile_ic // 4   # = 1
    out_ch_pack = tiling.tile_oc // 4

    # 各 Group 的 wave tile byte 大小（使用 tiled 維度）
    ps_size  = tiling.tile_oc * 3 * 3 * in_ch_pack * pkt_size
    pd_size  = tiling.tile_h_in * tiling.tile_w_in * in_ch_pack * pkt_size
    pli_size = tiling.tile_h_out * tiling.tile_w_out * out_ch_pack * pkt_size
    plo_size = pli_size

    # 強制 ping-pong 驗證：所有 wave tile ≤ half_cap
    for name, size in [("PS", ps_size), ("PD", pd_size),
                       ("PLI", pli_size), ("PLO", plo_size)]:
        if size > half_cap:
            raise TilingFailed(
                f"{name} wave ({size} bytes) > half_group_capacity ({half_cap})")

    return SpmPerGroupLayout(
        ps=GroupBufferLayout(
            wave_size=ps_size, ping_base=0,
            pong_base=half_cap, pingpong=True),
        pd=GroupBufferLayout(
            wave_size=pd_size, ping_base=0,
            pong_base=half_cap, pingpong=True),
        pli=GroupBufferLayout(
            wave_size=pli_size, ping_base=0,
            pong_base=half_cap, pingpong=True),
        plo=GroupBufferLayout(
            wave_size=plo_size, ping_base=0,
            pong_base=half_cap, pingpong=True),
    )
```

```
Per-Group Layout 範例（conv2d 3×3, tile_h_out=14, tile_w_out=198,
                       tile_ic=4, tile_oc=16, group_capacity=192KB）：

Group 0 (PS - Weight):                    Group 1 (PD - Activation):
┌──────────────────┐ 0                    ┌──────────────────┐ 0
│ PING: 1.125 KB   │                      │ PING: 25 KB      │
├──────────────────┤ 1152                  ├──────────────────┤ 25600
│ (unused 94.9 KB) │                      │ (unused 71 KB)   │
╞══════════════════╡ 98304 (half_cap)     ╞══════════════════╡ 98304 (half_cap)
│ PONG: 1.125 KB   │                      │ PONG: 25 KB      │
├──────────────────┤ 99456                 ├──────────────────┤ 123904
│ (unused 94.9 KB) │                      │ (unused 71 KB)   │
└──────────────────┘ 196608                └──────────────────┘ 196608

Group 2 (PLI - PSum In):                  Group 3 (PLO - Output):
┌──────────────────┐ 0                    ┌──────────────────┐ 0
│ PING: 86.6 KB    │                      │ PING: 86.6 KB    │
├──────────────────┤ 88704                 ├──────────────────┤ 88704
│ (unused 9.4 KB)  │                      │ (unused 9.4 KB)  │
╞══════════════════╡ 98304 (half_cap)     ╞══════════════════╡ 98304 (half_cap)
│ PONG: 86.6 KB    │                      │ PONG: 86.6 KB    │
├──────────────────┤ 187008                ├──────────────────┤ 187008
│ (unused 9.4 KB)  │                      │ (unused 9.4 KB)  │
└──────────────────┘ 196608                └──────────────────┘ 196608
```

### 3.6 Template 參數計算（基於 Tiled 維度）

> **重要**：PE template 參數使用的是 **wave tile 維度**（tile_ic, tile_oc, tile_h_out, tile_w_out），而非原始完整張量維度。PE 只看到一個 wave 的子問題。

```python
def compute_conv2d_3x3_pe_params(
    tiling: Conv2DTiling,
) -> Dict[str, int]:
    """
    由 tiled wave dimensions 計算 PE template 參數。
    PE 看到的是 tile_h_out × tile_w_out 大小的 output，
    tile_ic 個 input channels，tile_oc 個 output channels。
    """
    in_ch_pack = tiling.tile_ic // 4  # = 1（tile_ic=4 for 3×3）

    # vectors_per_kernel = KH * in_ch_pack = 3 × 1 = 3
    vectors_per_kernel = 3 * in_ch_pack

    # KERNEL_DMA_LEN: 一次加載所有 kernel weights 的 vector 數
    kernel_dma_len = tiling.tile_oc * vectors_per_kernel

    # OUTPUT_WINDOW_CNT_MINUS_ONE: 使用 wave tile 的 output 維度
    output_window_cnt = tiling.tile_h_out * tiling.tile_w_out
    output_window_cnt_minus_one = output_window_cnt - 1

    # KERNEL_COUNT: 每個 output pixel 的 output channel 遍歷次數
    kernel_count = tiling.tile_oc

    return {
        "KERNEL_DMA_LEN": kernel_dma_len,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": output_window_cnt_minus_one,
        "KERNEL_COUNT": kernel_count,
        "KERNEL_LOOP_INNER": 1,
        "KERNEL_LOOP_OUTER": 1,
    }
```

**範例**（tile_ic=4, tile_oc=16, tile_h_out=14, tile_w_out=198）：
- `in_ch_pack = 4/4 = 1`
- `vectors_per_kernel = 3 × 1 = 3`
- `KERNEL_DMA_LEN = 16 × 3 = 48`
- `OUTPUT_WINDOW_CNT_MINUS_ONE = 14 × 198 - 1 = 2771`
- `KERNEL_COUNT = 16`

### 3.7 AGU 配置計算

> Conv2D 3×3 全部 4 個 AGU 均使用 **Linear 地址空間 + Normal mode**（`channels_per_cycle=4`，broadcast 即可）。
> AGU `BASE_ADDR` 指向**各自 Group 內部**的 ping 或 pong offset。PONG base 固定在 `half_group_capacity`。
> 所有 iteration count 使用 **tiled 維度**（tile_ic, tile_oc, tile_h, tile_w），不是完整張量維度。

#### 3.7.1 AGU PS（Weight，Group 0）

權重在 SPM Group 0 中的存放順序為 `[tile_oc, KH, KW, in_ch_pack]`，in_ch_pack = tile_ic // 4 = 1。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `0`（ping）或 `half_cap`（pong） | 隨 wave 交替 |
| `ITER01` | `(KW << 16) \| in_ch_pack` | iter0 = in_ch_pack(=1), iter1 = KW(=3) |
| `ITER23` | `(tile_oc << 16) \| KH` | iter2 = KH(=3), iter3 = tile_oc |
| `STRIDE0` | `pkt_size` | 每個 IC pack +8 |
| `STRIDE1` | `in_ch_pack * pkt_size` | 跨 KW: 1 × 8 = 8 |
| `STRIDE2` | `KW * in_ch_pack * pkt_size` | 跨 KH: 3 × 1 × 8 = 24 |
| `STRIDE3` | `KH * KW * in_ch_pack * pkt_size` | 跨 OC: 3 × 3 × 1 × 8 = 72 |
| `TAG_BASE` | `0` | Tag 從 0 開始 |
| `TAG_STRIDE0` | `1` | 每個 inner loop tag +1 |
| `TAG_STRIDE1` | `0` | |
| `TAG_CTRL` | `1` | Tag tied to loop 1 (KH) |
| `MASK_CFG` | `0xF` | 全部 lane 啟用 |
| `LANE_CFG` | `0` | 預設 |
| `CTRL` | `0x0` | (start bit 由 HDDU CTRL 統一控制) |

> **IC tiling 注意**：每個 ic_tile 載入不同的 weight IC slice `weight[oc_start:+tile_oc, :, :, ic_start:+tile_ic]`。AGU 內部地址模式不變（總是遍歷 in_ch_pack=1 的 weight tile），差異在 DMA 載入的 DRAM 源位址。

#### 3.7.2 AGU PD（Activation，Group 1）

Activation 在 SPM Group 1 中的存放順序為 `[tile_h_in, tile_w_in, in_ch_pack]`，in_ch_pack = 1。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `0`（ping）或 `half_cap`（pong） | 隨 wave 交替 |
| `ITER01` | `(tile_h_in << 16) \| in_ch_pack` | iter0 = in_ch_pack(=1), iter1 = tile_h_in |
| `ITER23` | `(1 << 16) \| tile_w_in` | iter2 = tile_w_in, iter3 = 1 |
| `STRIDE0` | `pkt_size` | 每個 IC pack +8 |
| `STRIDE1` | `tile_w_in * in_ch_pack * pkt_size` | 跨 H: tile_w_in × 1 × 8 |
| `STRIDE2` | `in_ch_pack * pkt_size` | 跨 W: 1 × 8 = 8 |
| `STRIDE3` | `0` | 不使用 |
| `TAG_BASE` | `0` | |
| `TAG_STRIDE0` | `1` | |
| `TAG_STRIDE1` | `0` | |
| `TAG_CTRL` | `1` | Loop 1 (H) drives tag |
| `MASK_CFG` | `0xF` | |
| `LANE_CFG` | `0` | |
| `CTRL` | `0x0` | |

#### 3.7.3 AGU PLI（Partial Sum In，Group 2）

Partial sum 在 SPM Group 2 中的排列為 `[tile_h_out, tile_w_out, out_ch_pack]`，out_ch_pack = tile_oc // 4。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `0`（ping）或 `half_cap`（pong） | 隨 wave 交替 |
| `ITER01` | `(tile_h_out << 16) \| out_ch_pack` | iter0 = out_ch_pack, iter1 = tile_h_out |
| `ITER23` | `(1 << 16) \| tile_w_out` | iter2 = tile_w_out, iter3 = 1 |
| `STRIDE0` | `pkt_size` | 每個 OC pack +8 |
| `STRIDE1` | `tile_w_out * out_ch_pack * pkt_size` | 跨 H |
| `STRIDE2` | `out_ch_pack * pkt_size` | 跨 W |
| `STRIDE3` | `0` | |
| `TAG_BASE` | `0` | |
| `TAG_STRIDE0` | `1` | |
| `TAG_STRIDE1` | `0` | |
| `TAG_CTRL` | `1` | Loop 1 (H) drives tag |
| `MASK_CFG` | `0xF` | |
| `LANE_CFG` | `0` | |
| `CTRL` | `0x0` | |

> **IC 累加**：當 `ic_tile_idx == 0` 時，PLI 區域應填 0（初始 partial sum）。當 `ic_tile_idx > 0` 時，PLI 讀取前一輪 PLO 的結果。可透過 PLI/PLO 的 ping/pong 交替實現就地累加：ic_tile 0 → PLI=PING(0), PLO=PONG(half_cap)；ic_tile 1 → PLI=PONG(half_cap), PLO=PING(0)，以此交替。

#### 3.7.4 AGU PLO（Output，Group 3）

與 PLI 完全相同的地址模式，但指向 Group 3 的 ping/pong buffer：

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `0`（ping）或 `half_cap`（pong） | 隨 wave 交替 |
| 其餘 | 與 PLI 相同 | 相同大小與 stride |

### 3.8 DMA Descriptor 生成

每個 wave 需要生成 DMA descriptor 用於：
1. **Load**：DRAM → SPM（weight, activation）
2. **Load PLI**：僅 `ic_tile_idx > 0` 時，載入前一輪 PLO 結果作為 partial sum
3. **Store**：SPM → DRAM（output，僅 IC 累加完成後）

```python
def compute_conv2d_3x3_dma_descriptors(
    tiling: Conv2DTiling,
    spm_layout: SpmPerGroupLayout,
    dram_base: int,
    oc_tile_idx: int, h_tile_idx: int, w_tile_idx: int, ic_tile_idx: int,
    current_buffer: int,  # 0=ping, 1=pong
    half_cap: int,
) -> List[DmaDescriptor]:
    """生成單一 wave 的 DMA descriptors（4D tiling）"""
    descs = []

    # 選擇目標 buffer offset
    buf_base = half_cap if current_buffer else 0

    # 1. Weight DMA (Group 0)
    #    來源：weight[oc_start:+tile_oc, :, :, ic_start:+tile_ic]
    oc_start = oc_tile_idx * tiling.tile_oc
    ic_start = ic_tile_idx * tiling.tile_ic
    descs.append(DmaDescriptor(
        src_addr=dram_base + weight_dram_offset(oc_start, ic_start),
        dst_group=0,
        dst_addr=buf_base,
        length=spm_layout.ps.wave_size,
        direction="DRAM_TO_SPM",
    ))

    # 2. Activation DMA (Group 1)
    #    來源：input[h_start:+tile_h_in, w_start:+tile_w_in, ic_start:+tile_ic]
    h_start = h_tile_idx * tiling.tile_h_out  # output 座標（input = output + halo）
    w_start = w_tile_idx * tiling.tile_w_out
    descs.append(DmaDescriptor(
        src_addr=dram_base + activation_dram_offset(h_start, w_start, ic_start),
        dst_group=1,
        dst_addr=buf_base,
        length=spm_layout.pd.wave_size,
        direction="DRAM_TO_SPM",
    ))

    # 3. PLI DMA (Group 2) — 僅 ic_tile_idx > 0 時
    #    來源：前一輪 PLO 結果（已在 SPM 中，PLI/PLO ping-pong 交替）
    #    ic_tile_idx == 0: PLI 填 0（由 firmware clear 或硬體 zero-init）
    #    此處不需額外 DMA — 透過 PLI/PLO base address 交替實現就地累加

    # 4. Output DMA (Group 3) — 僅 IC 累加完成後
    #    條件：ic_tile_idx == num_ic_tiles - 1
    if ic_tile_idx == tiling.num_ic_tiles - 1:
        plo_base = half_cap if current_buffer else 0
        descs.append(DmaDescriptor(
            src_group=3,
            src_addr=plo_base,
            dst_addr=dram_base + output_dram_offset(oc_start, h_start, w_start),
            length=spm_layout.plo.wave_size,
            direction="SPM_TO_DRAM",
        ))

    return descs
```

> **IC 累加的 PLI/PLO 交替**：不需要額外 DMA 搬運 partial sum。利用 SPM Group 2 (PLI) 和 Group 3 (PLO) 的 ping/pong buffer 交替：
> - `ic_tile 0`: PLI=PING(0) [filled with 0], PLO=PONG(half_cap)
> - `ic_tile 1`: PLI=PONG(half_cap) [前輪 PLO 輸出], PLO=PING(0)
> - 依此交替，直到所有 IC tiles 累加完成
>
> 注意：此交替邏輯適用於 PLI/PLO 在**不同 Group** 的情況（Group 2 vs Group 3）。每個 Group 獨立的 ping/pong 空間讓 PE 可以同時讀 PLI 和寫 PLO 而不衝突。

### 3.9 HDDU Global 配置

| Register | Value | 說明 |
|----------|-------|------|
| `PLANE_EN` | `0xF` | PS/PD/PLI/PLO 全開 |
| `PLANE_MODE` | `0x1` | Conv mode |

### 3.10 Scan-chain 計算

```python
def compute_conv2d_scan_chain(
    num_pes: int,
    num_bus: int
) -> List[ScanChainEntry]:
    """
    為 conv2d 生成 scan-chain。
    PE 在每條 bus 上形成 chain：
    - 首 PE：PLI from bus, PLO to local-neighbor
    - 中間 PE：PLI from local, PLO to local-neighbor
    - 末 PE：PLI from local, PLO to bus

    ps_id / pd_id 為循序遞增（每個 PE 對映到唯一的 PS/PD stream）。
    pli_id / plo_id 的連線形成 chain（前一個 PE 的 plo 連到後一個 PE 的 pli）。
    """
    entries = []
    pes_per_bus = num_pes // num_bus

    for bus_idx in range(num_bus):
        for pe_local_idx in range(pes_per_bus):
            global_pe_id = bus_idx * pes_per_bus + pe_local_idx

            ps_id = global_pe_id    # 每個 PE 一個 PS stream
            pd_id = global_pe_id    # 每個 PE 一個 PD stream

            if pe_local_idx == 0:
                # 首 PE: PLI from bus
                route_mode = 1  # PLI_FROM_BUS_PLO_TO_LN
                pli_id = bus_idx  # bus input channel
                plo_id = global_pe_id
            elif pe_local_idx == pes_per_bus - 1:
                # 末 PE: PLO to bus
                route_mode = 2  # PLI_FROM_LN_PLO_TO_BUS
                pli_id = global_pe_id
                plo_id = bus_idx  # bus output channel
            else:
                # 中間 PE: local chain
                route_mode = 0  # PLI_FROM_LN_PLO_TO_LN
                pli_id = global_pe_id
                plo_id = global_pe_id

            entries.append(ScanChainEntry(
                ps_id=ps_id,
                pd_id=pd_id,
                pli_id=pli_id,
                plo_id=plo_id,
                route_mode=route_mode,
                enable=True,
            ))

    return entries
```

**重要**：scan-chain 必須以**反序**（最後一個 PE 先）送入 NoC command window。firmware 在 runtime 會從 `entries[-1]` 開始逐一 shift。

### 3.11 完整 Lowering 範例

**輸入**（參照 conv2D_image.png）：
- `conv2d_3x3`：`input[1, 16, 200, 4]`, `weight[16, 3, 3, 4]`, `output[1, 14, 198, 16]`, stride=1, padding=0
- Hardware：4 clusters, 48 PEs, 3 bus, SPM(BANKS_PER_GROUP=3, BANK_DEPTH=8192) → group_capacity=192KB, half_cap=96KB

**Step 1 — Tiling（4D: oc, h, w, ic）**：
1. 固定維度：`tile_ic=4`, `tile_oc=16`
2. `num_ic_tiles = 4/4 = 1`, `num_oc_tiles = 16/16 = 1`
3. 搜尋 tile_h_out × tile_w_out ≤ half_cap / (out_ch_pack × 8) = 98304 / (4 × 8) = 3072
4. 嘗試 `tile_h_out=14, tile_w_out=198`：14 × 198 = 2772 ≤ 3072 ✓
5. 驗證所有 Group ≤ half_cap（參照 §3.4.5）✓
6. `num_h_tiles = ceil(14/14) = 1`, `num_w_tiles = 1`
7. **Total waves = 1 × 1 × 1 × 1 = 1**

**Step 2 — SPM Per-Group Layout（強制 ping-pong）**：
- Group 0 (PS): PING=0, PONG=98304, wave=1152B
- Group 1 (PD): PING=0, PONG=98304, wave=25600B
- Group 2 (PLI): PING=0, PONG=98304, wave=88704B
- Group 3 (PLO): PING=0, PONG=98304, wave=88704B

**Step 3 — PE Template 參數**（使用 tiled 維度 tile_oc=16, tile_h_out=14, tile_w_out=198, tile_ic=4）：
- `in_ch_pack = 4/4 = 1`
- `KERNEL_DMA_LEN = 16 × 3 = 48`
- `OUTPUT_WINDOW_CNT_MINUS_ONE = 14 × 198 - 1 = 2771`
- `KERNEL_COUNT = 16`

**Step 4 — AGU**（以 PING buffer 為例）：
- AGU PS: BASE=0, ITER01=(3<<16)|1=0x30001, ITER23=(16<<16)|3=0x100003, STRIDE0=8, STRIDE1=8, STRIDE2=24, STRIDE3=72
- AGU PD: BASE=0, ITER01=(16<<16)|1=0x100001, ITER23=(1<<16)|200=0x100C8, STRIDE0=8, STRIDE1=1600, STRIDE2=8, STRIDE3=0
- AGU PLI: BASE=0, ITER01=(14<<16)|4=0xE0004, ITER23=(1<<16)|198=0x100C6, STRIDE0=8, STRIDE1=6336, STRIDE2=32, STRIDE3=0
- AGU PLO: BASE=0（同 PLI 模式，指向 Group 3）

**Step 5 — Multi-cluster mapping**：
- 1 spatial tile → 1 cluster active
- 剩餘 3 clusters idle

**Step 6 — Temporal Loop（single cluster, 4D）**：
```
# Single cluster processes entire spatial region
buf = 0
for oc_tile in [0]:       # num_oc_tiles=1
  for h_tile in [0]:       # num_h_tiles=1
    for w_tile in [0]:     # num_w_tiles=1
      for ic_tile in [0]:  # num_ic_tiles=1
        DMA_load(weight, input_tile) → SPM[buf]
        configure_AGU(buf)
        start_HDDU()
        wait_done()
        DMA_store(output_tile) ← SPM[buf]
        buf = 1 - buf
```

**DMA 轉移量**：
- Weight: 1152/8 = 144 transfers
- Input: 25600/8 = 3200 transfers
- Output: 88704/8 = 11088 transfers

---

## 4. Conv2D 1×1 Lowering

### 4.1 算子語義

Pointwise (1×1) convolution：
- `input[N, H_in, W_in, C_in]`
- `weight[OC, 1, 1, C_in]`
- `output[N, H_out, W_out, OC]`
- `H_out = H_in`（stride=1, padding=0 的典型情況）

### 4.2 PE Template 選擇

使用 `conv1d_k1c12s1` template。特性：
- Kernel size = 1，每 cycle 處理 channels_per_cycle = 12 個通道
- 同樣使用 sliding window 結構，但無 kernel spatial dimension

### 4.3 前置條件

| 條件 | 要求 |
|------|------|
| `C_in % 12 == 0` | 通道對齊（12 channels/cycle） |

> **注意**：OC > 16 和 C_in > 12 的情況由 tiling 處理（`tile_oc`、`tile_ic` 維度切割），不再是前置條件限制。

### 4.4 Tiling 策略

> 參照 [conv2D_image.png](conv2D_image.png) Conv2D 1x1 部分

Conv2D 1×1 使用 **4 維 tiling**（oc, h, w, ic），所有維度都必須切割。SPM 強制 ping-pong，tile 大小以 `half_group_capacity` 為上限。

#### 4.4.0 Spatial / PE 對應規則（H-lane + W-time）

PE 平行軸只承載 output `H` row：每個 PE 對應一個輸出列。`W` 維由 PE template 內部 `LOOPIN $(OUTPUT_WINDOW_CNT_MINUS_ONE)` 完整 temporal 處理。換言之：

- `tile_w` 永遠等於 `W_out`（不在 W 上切分），`num_w_tiles = 1`。
- `tile_h_per_bus = min(H_out, pes_per_bus)`，最多每 bus 16 PE。
- 跨 bus 平行的能力依 `H_out` 自動決定 path：
  | 條件 | path | `active_buses` | `tile_h` (= active_buses × tile_h_per_bus) |
  | --- | --- | --- | --- |
  | `H_out ≤ pes_per_bus` (16) | normal | 1 | `H_out` |
  | `pes_per_bus < H_out ≤ num_pes` (48) | ultra | `ceil(H_out / pes_per_bus)` | `≤ num_pes` |
  | `H_out > num_pes` | ultra (multi-wave) | `num_bus` (3) | `num_pes`，多 wave 切 H |

normal 與 ultra 兩條 path 的 SPM/AGU mode 相同（PS/PLI/PLO Parallel + Ultra，PD Linear + Normal），差異只在 scan-chain 拓樸與 PE / bus 數量。

#### 4.4.1 Wave Tile 維度定義

| 符號 | 定義 | 約束 | 性質 |
|------|------|------|------|
| `tile_ic` | 一個 wave 處理的 input channels | `= channels_per_cycle = 12` | **固定** |
| `tile_oc` | 一個 wave 產出的 output channels | `= min(OC, 16)` | **固定** |
| `tile_h` | 一個 wave 處理的 H 維度 | ≥ 1，由 SPM 反推 | **可變** |
| `tile_w` | 一個 wave 處理的 W 維度 | ≥ 1，由 SPM 反推 | **可變** |
| `num_ic_tiles` | IC 維度 tile 數 | `= C_in / tile_ic` | |
| `num_oc_tiles` | OC 維度 tile 數 | `= ceil(OC / tile_oc)` | |
| `num_h_tiles` | H 維度 tile 數 | `= ceil(H_in / tile_h)` | |
| `num_w_tiles` | W 維度 tile 數 | `= ceil(W_in / tile_w)` | |

> **IC 累加**：當 `num_ic_tiles > 1` 時（即 C_in > 12），每個 (oc, h, w) 組合需要跑 `num_ic_tiles` 次 wave，逐步累加 partial sum。機制與 Conv2D 3×3 相同。

#### 4.4.2 Per-Group SPM 需求

```python
def compute_conv2d_1x1_wave_sizes(
    tile_ic: int, tile_oc: int,
    tile_h: int, tile_w: int,
) -> Dict[str, int]:
    pkt_size = 8
    in_ch_pack = tile_ic // 12      # = 1（固定）
    out_ch_pack = tile_oc // 4

    # Group 0 (PS): Weight — tile_oc × 1 × 1 × in_ch_pack
    ps_wave_bytes = tile_oc * in_ch_pack * pkt_size

    # Group 1 (PD): Activation — tile_h × tile_w × in_ch_pack
    pd_wave_bytes = tile_h * tile_w * in_ch_pack * pkt_size

    # Group 2 (PLI): Partial-sum — tile_h × tile_w × out_ch_pack
    pli_wave_bytes = tile_h * tile_w * out_ch_pack * pkt_size

    # Group 3 (PLO): Output — 與 PLI 相同
    plo_wave_bytes = pli_wave_bytes

    return {"ps": ps_wave_bytes, "pd": pd_wave_bytes,
            "pli": pli_wave_bytes, "plo": plo_wave_bytes}
```

#### 4.4.3 Tiling 搜尋演算法

```python
def compute_conv2d_1x1_tiling(
    C_in: int, OC: int,
    H_in: int, W_in: int,
    group_capacity: int,
) -> Conv2D1x1Tiling:
    """
    搜尋滿足 per-group SPM 半容量（強制 ping-pong）的最大 wave tile 維度。
    固定維度：tile_ic=12, tile_oc=min(OC,16)
    可變維度：tile_h, tile_w（從最大值向下搜尋）
    """
    half_cap = group_capacity // 2
    tile_ic = 12  # channels_per_cycle
    tile_oc = min(OC, 16)

    num_ic_tiles = C_in // tile_ic
    num_oc_tiles = (OC + tile_oc - 1) // tile_oc

    # 驗證 PS（weight）先
    ps_wave = tile_oc * (tile_ic // 12) * 8
    if ps_wave > half_cap:
        raise TilingFailed("Weight tile exceeds half group capacity")

    for tile_h in range(H_in, 0, -1):
        for tile_w in range(W_in, 0, -1):
            sizes = compute_conv2d_1x1_wave_sizes(
                tile_ic, tile_oc, tile_h, tile_w)

            # 強制 ping-pong：所有 Group 的 wave tile ≤ half_cap
            if all(sizes[k] <= half_cap for k in ["ps", "pd", "pli", "plo"]):
                num_h_tiles = (H_in + tile_h - 1) // tile_h
                num_w_tiles = (W_in + tile_w - 1) // tile_w

                return Conv2D1x1Tiling(
                    tile_ic=tile_ic, tile_oc=tile_oc,
                    tile_h=tile_h, tile_w=tile_w,
                    num_ic_tiles=num_ic_tiles,
                    num_oc_tiles=num_oc_tiles,
                    num_h_tiles=num_h_tiles,
                    num_w_tiles=num_w_tiles,
                )

    raise TilingFailed("Cannot tile conv2d_1x1 to fit half group capacity")
```

#### 4.4.4 Temporal Loop 結構（4 維）

```python
# Conv2D 1x1 temporal loop（firmware 層級，4 維）
buf = 0
for oc_tile_idx in range(num_oc_tiles):
    oc_start = oc_tile_idx * tile_oc
    for h_tile_idx in range(num_h_tiles):
        for w_tile_idx in range(num_w_tiles):
            for ic_tile_idx in range(num_ic_tiles):
                ic_start = ic_tile_idx * tile_ic

                # DMA load weight[oc_start:+tile_oc, ic_start:+tile_ic] → SPM Group 0
                # DMA load input[h_start:+tile_h, w_start:+tile_w, ic_start:+tile_ic] → SPM Group 1
                # 若 ic_tile_idx > 0: PLI ← 前一輪 PLO（累加 partial sum）
                # 若 ic_tile_idx == 0: PLI 填 0
                dma_load_wave(oc_tile_idx, h_tile_idx, w_tile_idx, ic_tile_idx, buf)

                configure_agu(buf)
                start_hddu()
                wait_hddu_done()

                if ic_tile_idx == num_ic_tiles - 1:
                    # IC 累加完成，DMA store output
                    dma_store_output(oc_tile_idx, h_tile_idx, w_tile_idx, buf)

                buf = 1 - buf
```

> **Total waves** = `num_oc_tiles × num_h_tiles × num_w_tiles × num_ic_tiles`

#### 4.4.5 Conv2D 1×1 Tiling 範例（參照 conv2D_image.png）

**輸入**：`input[1, 16, 64, 12]`, `weight[48, 1, 1, 12]`, `output[1, 16, 64, 48]`
- `C_in=12`, `OC=48`, `H_in=16`, `W_in=64`
- `group_capacity = 192 KB`, `half_cap = 96 KB = 98304 bytes`

**固定 tile 維度**：`tile_ic=12`, `tile_oc=16`
- `num_ic_tiles = 12/12 = 1`
- `num_oc_tiles = 48/16 = 3`

**搜尋 tile_h, tile_w（最大化，≤ half_cap）**：

嘗試 `tile_h=16, tile_w=64`（完整空間）：
- `out_ch_pack = 16/4 = 4`, `in_ch_pack = 12/12 = 1`

| Group | Buffer | 計算 | Size | ≤ 96KB? |
|-------|--------|------|------|---------|
| 0 (PS) | Weight | `16 × 1 × 8 = 128` | 0.125 KB | ✓ |
| 1 (PD) | Input | `16 × 64 × 1 × 8 = 8192` | 8 KB | ✓ |
| 2 (PLI) | PSum | `16 × 64 × 4 × 8 = 32768` | 32 KB | ✓ |
| 3 (PLO) | Output | `16 × 64 × 4 × 8 = 32768` | 32 KB | ✓ |

所有 Group ≤ 96 KB ✓ → 不需要 spatial tiling。

**結論**：
- `tile_h=16, tile_w=64`（完整空間）
- `num_h_tiles=1, num_w_tiles=1`
- `num_oc_tiles=3, num_ic_tiles=1`
- **Total waves = 3 × 1 × 1 × 1 = 3**（3 passes for OC dimension）

**SPM layout（per Group, 強制 ping-pong, PS/PLI/PLO = Parallel mode）**：
- Group 0 (PS): **Parallel** — PING=196608, PONG=229376, buffer=128B
- Group 1 (PD): Linear — PING=0, PONG=98304, buffer=8192B
- Group 2 (PLI): **Parallel** — PING=196608, PONG=229376, buffer=32768B
- Group 3 (PLO): **Parallel** — PING=196608, PONG=229376, buffer=32768B

**Multi-cluster mapping**：
- 3 OC tiles → 3 clusters active（各處理 1 OC tile）
- 共用 input activation（broadcast），各 cluster 搬不同 OC slice 的 weight
- 第 4 cluster idle

**DMA 節省分析**：Input（8 KB）broadcast 一次即可，不需各 cluster 重複搬運。

### 4.5 SPM Per-Group Layout 計算

Conv2D 1×1 與 3×3 的主要差異在於 **PS/PLI/PLO 使用 Parallel Mode 地址空間 + AGU ultra mode**，而 PD 維持 Linear Mode。

- `in_ch_pack` 的基數為 12（tile_ic=12，in_ch_pack = tile_ic // 12 = 1）
- Weight shape 為 `[tile_oc, 1, 1, in_ch_pack]`，展平為 `[tile_oc, in_ch_pack]`

| Group | Buffer | SPM Mode | AGU Mode | Ping Base | Pong Base |
|-------|--------|----------|----------|-----------|-----------|
| 0 (PS) | Weight | **Parallel** | **Ultra** | `parallel_ping_base` (196608) | `parallel_pong_base` (229376) |
| 1 (PD) | Activation | Linear | Normal | `0` | `half_cap` (98304) |
| 2 (PLI) | Partial Sum | **Parallel** | **Ultra** | `parallel_ping_base` (196608) | `parallel_pong_base` (229376) |
| 3 (PLO) | Output | **Parallel** | **Ultra** | `parallel_ping_base` (196608) | `parallel_pong_base` (229376) |

> **為什麼 Conv 1×1 需要 Parallel + Ultra**：`channels_per_cycle = 12`，需要 3 個 NoC port 各送 4 channels 的不同資料（weight 12 channels 拆分到 3 port）。若使用 broadcast（normal mode），所有 port 只能取得相同的 4 channels，無法滿足 12 channels/cycle 的需求。PLI/PLO 同理：3 port 各處理不同的空間區段（見 AGU.md §5.1）。

```python
def compute_conv2d_1x1_spm_layout(
    tiling: Conv2D1x1Tiling,
    group_capacity: int,
) -> SpmPerGroupLayout:
    half_cap = group_capacity // 2
    pkt_size = 8
    in_ch_pack = tiling.tile_ic // 12  # = 1
    out_ch_pack = tiling.tile_oc // 4

    # wave_bytes = per-port 資料量 = AGU parallel 地址空間使用量
    # 實際 3 banks 總資料 = wave_bytes × 3（但 tiling 約束等價，見 §2.5）
    ps_size  = tiling.tile_oc * in_ch_pack * pkt_size
    pd_size  = tiling.tile_h * tiling.tile_w * in_ch_pack * pkt_size
    pli_size = tiling.tile_h * tiling.tile_w * out_ch_pack * pkt_size
    plo_size = pli_size

    # 強制 ping-pong：所有 Group wave tile ≤ half_cap
    for name, size in [("PS", ps_size), ("PD", pd_size),
                       ("PLI", pli_size), ("PLO", plo_size)]:
        if size > half_cap:
            raise TilingFailed(
                f"{name} wave ({size} bytes) > half_group_capacity ({half_cap})")

    # PS/PLI/PLO: Parallel mode → ping/pong 在 parallel 地址範圍
    # PD: Linear mode → ping/pong 在 linear 地址範圍
    parallel_ping = parallel_ping_base  # 196608
    parallel_pong = parallel_pong_base  # 229376

    return SpmPerGroupLayout(
        ps=GroupBufferLayout(wave_size=ps_size, ping_base=parallel_ping, pong_base=parallel_pong,
                             pingpong=True, spm_mode="parallel"),
        pd=GroupBufferLayout(wave_size=pd_size, ping_base=0, pong_base=half_cap,
                             pingpong=True, spm_mode="linear"),
        pli=GroupBufferLayout(wave_size=pli_size, ping_base=parallel_ping, pong_base=parallel_pong,
                              pingpong=True, spm_mode="parallel"),
        plo=GroupBufferLayout(wave_size=plo_size, ping_base=parallel_ping, pong_base=parallel_pong,
                              pingpong=True, spm_mode="parallel"),
    )
```

### 4.6 Template 參數計算（基於 Tiled 維度）

```python
def compute_conv2d_1x1_pe_params(
    tiling: Conv2D1x1Tiling,
) -> Dict[str, int]:
    in_ch_pack = tiling.tile_ic // 12  # = 1

    # KH=KW=1，vectors_per_kernel = 1 * in_ch_pack
    vectors_per_kernel = in_ch_pack

    kernel_dma_len = tiling.tile_oc * vectors_per_kernel
    # 使用 wave tile 的 spatial 維度
    output_window_cnt_minus_one = tiling.tile_h * tiling.tile_w - 1
    kernel_count = tiling.tile_oc

    return {
        "KERNEL_DMA_LEN": kernel_dma_len,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": output_window_cnt_minus_one,
        "KERNEL_COUNT": kernel_count,
        "KERNEL_LOOP_INNER": 1,
        "KERNEL_LOOP_OUTER": 1,
    }
```

**與 conv2d_3×3 的差異**：
- `vectors_per_kernel` 為 `1 * in_ch_pack`（而非 `3 * in_ch_pack`），因為 kernel spatial size = 1
- `channels_per_cycle = 12`（而非 4）→ `tile_ic = 12`
- `KERNEL_COUNT = tile_oc`（≤16）

### 4.7 AGU 配置

> Conv2D 1×1 的 PS/PLI/PLO 使用 **Parallel 地址空間 + Ultra mode**，PD 維持 **Linear + Normal**。
> 此 SPM/AGU 模式組合在 conv1x1 normal path 與 ultra path 都成立；path 區分只決定 scan-chain 拓樸與 H tiling，並不切換 SPM/AGU mode（見 §4.6.1）。
> 詳細的 per-port 迭代拆分與 tag 編碼規則請參考 [AGU.md](../../hybridacc-ESL/doc/AGU.md) §5.1。

#### AGU PS（Weight，Group 0 — Parallel / Ultra）

1×1 conv 的 weight shape 為 `[tile_oc, 1, 1, tile_ic=12]`。Ultra mode 下 12 channels 拆分給 3 port（每 port 4 channels）：
- Bank 0（port 0）：channels 0–3
- Bank 1（port 1）：channels 4–7
- Bank 2（port 2）：channels 8–11

每個 AGU 迭代讀取 parallel 地址 → 同時獲得 3 banks × 64-bit = 192-bit = 12 channels。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `parallel_ping_base`（196608）或 `parallel_pong_base`（229376） | Parallel 地址範圍，隨 wave 交替 |
| `ITER01` | `(1 << 16) \| in_ch_pack` | iter0 = in_ch_pack(=1), iter1 = 1 |
| `ITER23` | `(tile_oc << 16) \| 1` | iter2 = 1, iter3 = tile_oc |
| `STRIDE0` | `pkt_size` | 每個 IC pack +8（跨 parallel row） |
| `STRIDE1` | `0` | |
| `STRIDE2` | `0` | |
| `STRIDE3` | `in_ch_pack * pkt_size` | 跨 OC: 1 × 8 = 8 |
| `TAG_BASE` | `0` | |
| `TAG_STRIDE0` | `1` | |
| `TAG_CTRL` | `0` | |
| `MASK_CFG` | `0xF` | 4 lanes per port |
| `CTRL` | `0x8` | **bit3=1：ultra mode** |

> **DMA 載入**：DMA 需將 weight 資料拆分寫入 3 個 Bank：
> - Bank 0（linear addr 0）：port 0 的 weight（channels 0–3 for each OC）
> - Bank 1（linear addr `bank_depth_bytes`=65536）：port 1 的 weight（channels 4–7）
> - Bank 2（linear addr `2×bank_depth_bytes`=131072）：port 2 的 weight（channels 8–11）
>
> Ping-pong 交替時，各 Bank 的 PING 半段 base = bank_base，PONG 半段 base = bank_base + `half_parallel`。

> **IC tiling 注意**：每個 ic_tile 載入不同的 weight IC slice。AGU 內部不變（in_ch_pack=1），差異在 DMA 的 DRAM 源位址。

#### AGU PD（Activation，Group 1 — Linear / Normal）

PD 維持 Linear mode + Normal broadcast。Activation 在一個 wave 內對所有 port 相同（broadcast 即可）。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `0`（ping）或 `half_cap`（pong） | Linear 地址範圍 |
| `ITER01` | `(tile_h << 16) \| in_ch_pack` | iter0 = in_ch_pack(=1), iter1 = tile_h |
| `ITER23` | `(1 << 16) \| tile_w` | iter2 = tile_w, iter3 = 1 |
| `STRIDE0` | `pkt_size` | |
| `STRIDE1` | `tile_w * in_ch_pack * pkt_size` | 跨 H |
| `STRIDE2` | `in_ch_pack * pkt_size` | 跨 W |
| `STRIDE3` | `0` | |
| `TAG_BASE` | `0` | |
| `TAG_CTRL` | `1` | Loop 1 (H) drives tag |
| `MASK_CFG` | `0xF` | |
| `CTRL` | `0x0` | Normal mode（bit3=0） |

#### AGU PLI（Partial Sum In，Group 2 — Parallel / Ultra）

Ultra mode 下 3 port 各處理不同的 output 空間區段（見 AGU.md §5.1：`base_h = port × loop_out_height + oh`）。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `parallel_ping_base`（196608）或 `parallel_pong_base`（229376） | Parallel 地址範圍 |
| `ITER01` | `(tile_h_out << 16) \| out_ch_pack` | 參考 AGU.md per-port 拆分 |
| `ITER23` | `(1 << 16) \| tile_w_out` | |
| `STRIDE0` | `pkt_size` | |
| `STRIDE1` | `tile_w_out * out_ch_pack * pkt_size` | 跨 H |
| `STRIDE2` | `out_ch_pack * pkt_size` | 跨 W |
| `STRIDE3` | `0` | |
| `TAG_BASE` | `0` | |
| `TAG_CTRL` | `1` | Loop 1 (H) drives tag |
| `MASK_CFG` | `0xF` | |
| `CTRL` | `0x8` | **bit3=1：ultra mode** |

> **IC 累加**：當 `ic_tile_idx == 0` 時，PLI 區域填 0；`ic_tile_idx > 0` 時讀取前一輪 PLO 結果。在 parallel mode 下，PLI/PLO 的 ping/pong 交替同樣在 parallel 地址範圍內進行：`parallel_ping_base` ↔ `parallel_pong_base`。

#### AGU PLO（Output，Group 3 — Parallel / Ultra）

與 PLI 相同的 parallel + ultra 配置，但指向 Group 3。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `parallel_ping_base` 或 `parallel_pong_base` | Parallel 地址範圍 |
| 其餘 ITER/STRIDE | 與 PLI 相同 | 相同 output tile shape |
| `ULTRA_STRIDE` | `rows_per_port * tile_w_out * out_ch_pack * pkt_size` | PLO response per-port stride（見 AGU.md） |
| `CTRL` | `0x8` | **bit3=1：ultra mode** |

> **DMA store**：IC 累加完成後，output 需從 3 個 Bank 分別讀回（DMA 使用 Linear 地址）並組裝存回 DRAM。

### 4.8 Scan-chain

Conv2D 1×1 使用獨立的 `compute_scan_chain_conv1x1(num_pes, num_bus, tile_h_per_bus, active_buses, stride)`，**不**與 conv2d_3x3 共用。每個 active PE 對應一個 H row：

- enable: `bus_idx < active_buses` 且 `pe_local < tile_h_per_bus`
- `route_mode`：永遠是 `(IB, OB)` (3)，因為 1×1 沒有跨列累積（kernel height = 1），每個 active PE 直接從 MBUS 取 PLI/初值並寫回 MBUS。
- IDs：`ps_id = 0`、`pd_id = pe_local * stride`、`pli_id = plo_id = pe_local`。

> **與 conv3x3 的差別**：conv3x3 的 scan-chain 用 `(IL, OL)` 串接相鄰 row 的 line-buffer 累積；conv1x1 不需要也沒有此鏈路。

### 4.9 HDDU 配置

| Register | Value |
|----------|-------|
| `PLANE_EN` | `0xF` |
| `PLANE_MODE` | `0x1` (conv mode) |

---

## 5. GEMM Lowering

### 5.1 算子語義

General Matrix Multiplication：`C[M, N] = A[M, K] × B[K, N]`

所有矩陣以 fp16 格式存放。PE array 以 tiled 方式處理。

### 5.2 PE Template 選擇

使用 `gemm` template。此 template 的特性：
- 支援 M/N/K 三維 tiling
- 使用 `VMULR`/`VMULRN` 指令（乘法 + stride 控制）
- 輸出使用 `VPSUMR`（累加 partial sum）
- DMA 使用 `LDMA.LHB`（fp16 broadcast load）

### 5.3 Tiling 策略

> 參照 [GEMM_image.png](GEMM_image.png)

GEMM 需要 3 維 tiling（M, N, K），每個 wave 處理一個 `A_tile[M_tile, K_tile] × B_tile[K_tile, N_tile] → C_tile[M_tile, N_tile]` 子問題。K 維度的多個 tile 需要累加（partial sum）。

#### 5.3.1 Wave Tile 維度定義

| 符號 | 定義 | 約束 |
|------|------|------|
| `M_tile` | A 矩陣的行數 tile | 由 per-group SPM 容量決定 |
| `N_tile` | B 矩陣的列數 tile | 由 per-group SPM 容量決定 |
| `K_tile` | 共用的 reduction 維度 tile | 由 per-group SPM 容量決定 |
| `num_m_tiles` | M 維度 tile 數 | `ceil(M / M_tile)` |
| `num_n_tiles` | N 維度 tile 數 | `ceil(N / N_tile)` |
| `num_k_tiles` | K 維度 tile 數 | `ceil(K / K_tile)` |

#### 5.3.2 Per-Group SPM 需求

```python
def compute_gemm_wave_sizes(
    M_tile: int, N_tile: int, K_tile: int,
) -> Dict[str, int]:
    pkt_size = 8
    ep = 4  # elements_per_pkt (fp16)

    # Group 0 (PS): A tile [M_tile, K_tile]
    ps_wave_bytes = (M_tile * K_tile // ep) * pkt_size

    # Group 1 (PD): B tile [K_tile, N_tile]
    pd_wave_bytes = (K_tile * N_tile // ep) * pkt_size

    # Group 2 (PLI): C tile accumulator [M_tile, N_tile] — partial sum input
    pli_wave_bytes = (M_tile * N_tile // ep) * pkt_size

    # Group 3 (PLO): C tile output [M_tile, N_tile]
    plo_wave_bytes = pli_wave_bytes

    return {"ps": ps_wave_bytes, "pd": pd_wave_bytes,
            "pli": pli_wave_bytes, "plo": plo_wave_bytes}
```

#### 5.3.3 Tiling 搜尋演算法

```python
def compute_gemm_tiling(
    M: int, N: int, K: int,
    num_pes: int,
    group_capacity: int,
) -> GemmTiling:
    """
    搜尋最大化 M_tile × N_tile × K_tile 的 wave tile 維度。
    強制 ping-pong：所有 Group wave tile ≤ half_group_capacity。

    搜尋策略：
    1. M_tile 從 M 向下搜尋（步長 8，PE 的 VPSUM 暫存器對齊）
    2. N_tile 從 N 向下搜尋（步長 12，匹配 NoC bus 寬度）
    3. K_tile 從 K 向下搜尋（步長 32，匹配 PE DM 對齊）
    """
    half_cap = group_capacity // 2

    for M_tile in range(min(M, 32), 0, -8):  # M ≤ 32, 步長 8
        if M_tile <= 0: continue
        for N_tile in range(min(N, 48), 0, -12):  # N ≤ 48, 步長 12
            if N_tile <= 0: continue
            for K_tile in range(min(K, 96), 0, -32):  # K ≤ 96, 步長 32
                if K_tile <= 0: continue

                sizes = compute_gemm_wave_sizes(M_tile, N_tile, K_tile)

                # 強制 ping-pong：所有 Group ≤ half_cap
                if all(sizes[k] <= half_cap
                       for k in ["ps", "pd", "pli", "plo"]):

                    num_m_tiles = (M + M_tile - 1) // M_tile
                    num_n_tiles = (N + N_tile - 1) // N_tile
                    num_k_tiles = (K + K_tile - 1) // K_tile

                    return GemmTiling(
                        M_tile=M_tile, N_tile=N_tile, K_tile=K_tile,
                        num_m_tiles=num_m_tiles,
                        num_n_tiles=num_n_tiles,
                        num_k_tiles=num_k_tiles,
                    )

    raise TilingFailed("Cannot tile GEMM to fit half group capacity")
```

#### 5.3.4 Temporal Loop 結構

```python
# GEMM temporal loop（firmware 層級）
for n_tile_idx in range(num_n_tiles):
    for m_tile_idx in range(num_m_tiles):
        # 初始化 C_tile partial sum = 0
        clear_pli_buffer()

        for k_tile_idx in range(num_k_tiles):
            # DMA load A_tile[m_tile, k_tile] → SPM Group 0
            # DMA load B_tile[k_tile, n_tile] → SPM Group 1
            # 若 k>0: DMA load previous C_tile partial sum → SPM Group 2
            dma_load_gemm_wave(m_tile_idx, k_tile_idx, n_tile_idx, buf)

            configure_agu(buf)
            start_hddu()
            wait_hddu_done()

            buf = 1 - buf  # swap ping/pong

        # K 維度累加完成，DMA store C_tile → DRAM
        dma_store_c_tile(m_tile_idx, n_tile_idx, buf)
```

> **K 累加**：K 維度的多個 tile 需要逐步累加 partial sum。第一個 K tile 的 PLI 輸入為 0，後續 K tile 的 PLI 輸入為前一輪的 PLO 輸出。Code-gen 可透過 PLI/PLO base address 交替實現就地累加（in-place accumulation）。

#### 5.3.5 Ping-Pong 多 Tile 預載約束（參照 GEMM_image.png）

根據 GEMM_image.png，SPM 各 Group 強制 ping-pong，每半 Group 可容納多個 wave tile 用於 DMA/compute 流水線：

| Group | Buffer | Wave tile size | 最大 tiles (half_cap=96KB) | 約束 |
|-------|--------|---------------|---------------------------|------|
| 0 (PS) | A tiles | 6 KB | 16 | `num_tiles × 6KB ≤ half_cap` |
| 1 (PD) | B tiles | 9 KB | 10 | `num_tiles × 9KB ≤ half_cap` |
| 3 (PLO) | C tiles | 3 KB | 32 | `num_tiles × 3KB ≤ half_cap` |

> 注：以上數值為 M_tile=32, K_tile=96, N_tile=48 時的具體範例。實際由 tiling 搜尋決定。

#### 5.3.6 GEMM Tiling 範例（參照 GEMM_image.png）

**輸入**：`A[32, 96]`, `B[96, 48]`, `C[32, 48]`
- `group_capacity = 192 KB = 196608 bytes`, `half_cap = 96 KB = 98304 bytes`

**Wave tile 維度**：`M_tile=32, K_tile=96, N_tile=48`（完整矩陣 fit 進一個 wave）

**Per-group wave sizes（≤ half_cap 驗證）**：

| Group | Buffer | 計算 | Size | ≤ 96KB? |
|-------|--------|------|------|---------|
| 0 (PS) | A tile | `32 × 96 / 4 × 8 = 6144` | 6 KB | ✓ |
| 1 (PD) | B tile | `96 × 48 / 4 × 8 = 9216` | 9 KB | ✓ |
| 2 (PLI) | C accum | `32 × 48 / 4 × 8 = 3072` | 3 KB | ✓ |
| 3 (PLO) | C out | `32 × 48 / 4 × 8 = 3072` | 3 KB | ✓ |

**SPM layout（per Group, 強制 ping-pong, PS/PLI/PLO = Parallel mode）**：
- Group 0 (PS): **Parallel** — PING=196608, PONG=229376
- Group 1 (PD): Linear — PING=0, PONG=98304
- Group 2 (PLI): **Parallel** — PING=196608, PONG=229376
- Group 3 (PLO): **Parallel** — PING=196608, PONG=229376

**DMA transfers**：
- A: `6144 / 8 = 768` transfers
- B: `9216 / 8 = 1152` transfers
- C: `3072 / 8 = 384` transfers

**Wave Compute cycle**：`256 + 826 + 24 = 1106`（PE template 固有延遲）

**若矩陣更大（e.g. M=128, K=384, N=192）**：
- `num_m_tiles = 128/32 = 4`
- `num_k_tiles = 384/96 = 4`
- `num_n_tiles = 192/48 = 4`
- Total waves = 4 × 4 × 4 = 64 waves

**Multi-cluster mapping**：
- 可沿 N 或 M 維度切分（非 reduction 維度）
- **禁止沿 K 維度切分**（K 是 reduction 維度，會產生跨 cluster 的 partial sum 同步需求）
- 4 clusters 各處理 num_n_tiles/4 = 1 個 N tile → 有效 waves/cluster = 16
- 共用 A 矩陣（broadcast），各 cluster 搬不同 N slice 的 B 矩陣

### 5.4 SPM Per-Group Layout 計算

GEMM 的 **PS/PLI/PLO 使用 Parallel Mode 地址空間 + AGU ultra mode**，PD 維持 Linear Mode。
（原因與 Conv2D 1×1 類似：3 port 需傳送不同的 weight/partial sum 資料。）

| Group | Buffer | SPM Mode | AGU Mode | Ping Base | Pong Base |
|-------|--------|----------|----------|-----------|-----------|
| 0 (PS) | A tile | **Parallel** | **Ultra** | `parallel_ping_base` (196608) | `parallel_pong_base` (229376) |
| 1 (PD) | B tile | Linear | Normal | `0` | `half_cap` (98304) |
| 2 (PLI) | C accum | **Parallel** | **Ultra** | `parallel_ping_base` (196608) | `parallel_pong_base` (229376) |
| 3 (PLO) | C out | **Parallel** | **Ultra** | `parallel_ping_base` (196608) | `parallel_pong_base` (229376) |

```python
def compute_gemm_spm_layout(
    tiling: GemmTiling,
    group_capacity: int,
) -> SpmPerGroupLayout:
    half_cap = group_capacity // 2
    pkt_size = 8
    ep = 4  # elements_per_pkt

    ps_size  = (tiling.M_tile * tiling.K_tile // ep) * pkt_size
    pd_size  = (tiling.K_tile * tiling.N_tile // ep) * pkt_size
    pli_size = (tiling.M_tile * tiling.N_tile // ep) * pkt_size
    plo_size = pli_size

    # 強制 ping-pong
    for name, size in [("PS", ps_size), ("PD", pd_size),
                       ("PLI", pli_size), ("PLO", plo_size)]:
        if size > half_cap:
            raise TilingFailed(
                f"{name} wave ({size} bytes) > half_group_capacity ({half_cap})")

    # PS/PLI/PLO: Parallel mode → ping/pong 在 parallel 地址範圍
    # PD: Linear mode → ping/pong 在 linear 地址範圍
    parallel_ping = parallel_ping_base  # 196608
    parallel_pong = parallel_pong_base  # 229376

    return SpmPerGroupLayout(
        ps=GroupBufferLayout(wave_size=ps_size, ping_base=parallel_ping, pong_base=parallel_pong,
                             pingpong=True, spm_mode="parallel"),
        pd=GroupBufferLayout(wave_size=pd_size, ping_base=0, pong_base=half_cap,
                             pingpong=True, spm_mode="linear"),
        pli=GroupBufferLayout(wave_size=pli_size, ping_base=parallel_ping, pong_base=parallel_pong,
                              pingpong=True, spm_mode="parallel"),
        plo=GroupBufferLayout(wave_size=plo_size, ping_base=parallel_ping, pong_base=parallel_pong,
                              pingpong=True, spm_mode="parallel"),
    )
```

### 5.5 Template 參數計算（基於 Tiled 維度）

> PE template 參數完全由 tiling 結果決定。

```python
def compute_gemm_pe_params(
    tiling: GemmTiling,
) -> Dict[str, int]:
    M = tiling.M_tile
    N = tiling.N_tile
    K = tiling.K_tile
    ep = 4  # elements_per_pkt

    return {
        # C tile output 的 DMA 長度
        "KERNEL_DMA_STORE_LEN": M * N // ep,

        # A/B tile load 的 DMA 長度
        "KERNEL_DMA_LOAD_LEN": K * max(N, M) // ep,

        # K tile 維度
        "INPUT_DIM": K,

        # output tile 較小維度
        "OUTPUT_DIM": M,

        # partial sum 累加次數
        "PSUM_COUNT": M * N // ep,

        # K 維度的 tile 數（reduction loop 在 PE 內部 or firmware 控制）
        "NUM_OF_KERNEL_SETS": tiling.num_k_tiles,

        # spatial tile 數（用於 firmware temporal loop）
        "NUM_OF_N_TILES": tiling.num_n_tiles,
        "NUM_OF_M_TILES": tiling.num_m_tiles,

        # K tile dimension
        "K_TILE_DIM": K,
    }
```

### 5.6 AGU 配置

> GEMM 的 PS/PLI/PLO 使用 **Parallel 地址空間 + Ultra mode**，PD 維持 **Linear + Normal**。
> Plane → matrix 對應：**PS = B `[K, N]`、PD = A `[M, K]`、PLI/PLO = C `[M, N]`**（與 `noc_gen.generate_gemm_test` 的 `gen_dram_image_gemm` 對齊）。`hardware_ir.json.tiling_params` 中 `dram_input_base` 指 A、`dram_weight_base` 指 B、`dram_output_base` 指 C。
> 詳細的 per-port 迭代拆分與 tag 編碼規則請參考 [AGU.md](../../hybridacc-ESL/doc/AGU.md) §5.2。

#### AGU PS（Matrix B，Group 0 — Parallel / Ultra）

B tile shape `[K_tile, N_tile]`。Ultra mode 下 3 port 各取得不同的 N 區段資料。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `parallel_ping_base`（196608）或 `parallel_pong_base`（229376） | **Parallel** 地址範圍，隨 wave 交替 |
| `ITER01` | `(K_tile/ep << 16) \| (M_tile/ep)` | iter0, iter1 |
| `ITER23` | `0x10001` | iter2=1, iter3=1 |
| `STRIDE0` | `pkt_size` | |
| `STRIDE1` | `M_tile/ep * pkt_size` | 跨 K |
| `STRIDE2` | `0` | |
| `STRIDE3` | `0` | |
| `TAG_BASE` | `0` | |
| `TAG_CTRL` | `1` | |
| `MASK_CFG` | `0xF` | |
| `CTRL` | `0x8` | **bit3=1：ultra mode** |

> **DMA 載入**：DMA 需將 A tile 資料拆分寫入 3 個 Bank（各 Bank 存放分配給對應 port 的資料段落），使用 linear 地址：Bank 0 = addr 0, Bank 1 = addr 65536, Bank 2 = addr 131072。Ping-pong 半段偏移 = `half_parallel` (32768)。

#### AGU PD（Matrix A，Group 1 — Linear / Normal）

A tile shape `[M_tile, K_tile]`。PD 維持 Linear mode + Normal broadcast。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `0`（ping）或 `half_cap`（pong） | **Linear** 地址範圍 |
| `ITER01` | `(N_tile/ep << 16) \| (K_tile/ep)` | |
| `STRIDE0` | `pkt_size` | |
| `STRIDE1` | `K_tile/ep * pkt_size` | |
| 其餘 | 與 PS 類似 | |
| `CTRL` | `0x0` | Normal mode（bit3=0） |

#### AGU PLI（Partial Sum / Zero-init，Group 2 — Parallel / Ultra）

GEMM 的 PLI 在 K=0 時輸入 0（initial partial sum），K>0 時指向前一輪 PLO 的結果。
Ultra mode 下 3 port 各讀取不同 M 區段的 partial sum（見 AGU.md §5.2）。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `parallel_ping_base`（196608）或 `parallel_pong_base`（229376） | **Parallel** 地址範圍 |
| `ITER01` | `(N_tile/ep << 16) \| (M_tile/ep)` | 參考 AGU.md per-port 拆分 |
| `STRIDE0` | `pkt_size` | |
| `STRIDE1` | `M_tile/ep * pkt_size` | |
| 其餘 STRIDE | `0` | |
| `CTRL` | `0x8` | **bit3=1：ultra mode** |

> **K 累加就地交替**：K tile 0 時 PLI 區域填 0，PLO 寫結果到 parallel pong。K tile 1 時 PLI 讀 parallel pong（前輪結果），PLO 寫到 parallel ping。如此交替實現 reduction accumulation，在 parallel 地址空間內：`parallel_ping_base` ↔ `parallel_pong_base`。

#### AGU PLO（Matrix C，Group 3 — Parallel / Ultra）

C tile shape `[M_tile, N_tile]`。Ultra mode 下 3 port 各寫入不同 M 區段。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `parallel_ping_base` 或 `parallel_pong_base` | **Parallel** 地址範圍 |
| `ITER01` | `(N_tile/ep << 16) \| (M_tile/ep)` | |
| `STRIDE0` | `pkt_size` | |
| `STRIDE1` | `M_tile/ep * pkt_size` | |
| `ULTRA_STRIDE` | `rows_per_port * N_tile / ep * pkt_size` | PLO response per-port stride（見 AGU.md §5.2） |
| `CTRL` | `0x8` | **bit3=1：ultra mode** |

> **DMA store**：K 累加完成後，C tile 需從 3 個 Bank 分別讀回（DMA 使用 Linear 地址）並組裝存回 DRAM。

### 5.7 HDDU 配置

| Register | Value | 說明 |
|----------|-------|------|
| `PLANE_EN` | `0xB` (0b1011) | PS/PD/PLO 開啟，PLI 視 K 累加需求 |
| `PLANE_MODE` | `0x2` | GEMM mode |

> 注意：GEMM 模式下 PLI 可能不啟用（首個 K tile 時 partial sum = 0），此處依 ComputeCluster.md 範例為 `0xB`。K > 0 時可能需切為 `0xF` 啟用 PLI。

### 5.8 Scan-chain

GEMM 走 **K-chain** 拓樸：M × N grid 在每條 bus 上重複，bus index 對應 K-stage：bus `i` 承擔第 `i` 個 K-stage 的部分和，跨 bus 透過 LN 串接形成 reduction chain。

常數：`PE_M = 12, PE_N = 8, PE_K = 32`，`grid_m = ceil(M/PE_M)`, `grid_n = ceil(N/PE_N)`, `grid_k = ceil(K/PE_K)`。`grid_k_per_wave = min(grid_k, num_bus)` 決定每 wave 同時跑幾個 K-stage。

對 bus `k_idx` 上的 PE `j`：
- `enable = (k_idx < grid_k_per_wave) and (j < grid_m_per_wave * grid_n_per_wave)`
- `m_idx = j // grid_n_per_wave`, `n_idx = j  % grid_n_per_wave`

| 條件 | `route_mode` | `pli_id` | `plo_id` |
| --- | --- | --- | --- |
| `grid_k_per_wave == 1` | `(IB, OB)` (3) | `pe_local`（讀 C / 0） | `pe_local`（寫 C） |
| `k_idx == 0`（first stage, multi） | `(IB, OL)` (1) | `pe_local`（讀 C / 0） | 63（傳給下一 K-stage） |
| `0 < k_idx < grid_k_per_wave - 1` | `(IL, OL)` (0) | 63 | 63 |
| `k_idx == grid_k_per_wave - 1`（last） | `(IL, OB)` (2) | 63 | `pe_local`（寫 C） |

ID 規則（與 `noc_gen.generate_gemm_test` 對齊）：
- ultra path：`ps_id = n_idx`、`pd_id = m_idx`（K-stage 間共用）。
- normal path：`ps_id = k_idx * grid_n + n_idx`、`pd_id = k_idx * grid_m + m_idx`。

```python
def compute_scan_chain_gemm(num_pes, num_bus, grid_m, grid_n, grid_k,
                            use_ultra=True):
    """GEMM K-chain scan chain — see lowering.compute_scan_chain_gemm."""
    ...  # 見 python/hybridacc_cc/lowering.py
```

> **與 Conv2D 的差別**：Conv2D 1×1 用 H-lane scan-chain（PE 對應 output H row，沒有跨列累積，永遠 `(IB, OB)`）。Conv2D 3×3 用 line-buffer chain 累積 3 列 input，PE chain 內 `(IL, OL)`。GEMM 改以 bus 軸做 K reduction，chain 方向與 Conv 完全不同。

> **PE-grid metadata**：lowering 會將 `GRID_M / GRID_N / GRID_K / GRID_M_PER_WAVE / GRID_N_PER_WAVE / GRID_K_PER_WAVE` 寫入 `pe_program.params`，方便從 `hardware_ir.json` 直接驗證 wave 分割。

---

## 6. Temporal Schedule 生成

### 6.1 Wave Loop 結構

Lowering 的最終產出包含一個 **Temporal Schedule**，描述在 firmware 層級如何迭代所有 wave tiles。共通結構如下：

```python
@dataclass
class TemporalSchedule:
    """Temporal loop schedule for firmware code generation."""
    loop_dims: List[str]          # e.g. ["oc_tile", "h_tile", "w_tile", "ic_tile"]
    loop_bounds: Dict[str, int]   # e.g. {"oc_tile": 3, "h_tile": 2, "w_tile": 1, "ic_tile": 4}
    total_waves: int              # product of all loop bounds
    pingpong: bool = True         # 強制 ping-pong（always True）

    # per-wave AGU base address 計算函式（code-gen 使用）
    def get_agu_base_addrs(self, wave_indices: Dict[str, int], buf: int,
                           half_cap: int) -> Dict[str, int]:
        """Return per-group base addresses for this wave.
        buf=0 → PING(base=0), buf=1 → PONG(base=half_cap)."""
        base = half_cap if buf else 0
        return {"ps": base, "pd": base, "pli": base, "plo": base}
```

#### Conv2D 3×3 Temporal Loop（4 維：oc, h, w, ic）

```
for oc_tile in range(num_oc_tiles):
    for h_tile in range(num_h_tiles):
        for w_tile in range(num_w_tiles):
            for ic_tile in range(num_ic_tiles):
                DMA_load(weight[oc,ic], input_tile[h,w,ic]) → SPM[buf]
                # ic_tile==0: PLI=0; ic_tile>0: PLI ← prev PLO
                configure_AGU(buf)
                start_HDDU()
                wait_done()
                if ic_tile == num_ic_tiles - 1:
                    DMA_store(output_tile[oc,h,w]) ← SPM[buf]
                buf = 1 - buf
```

#### Conv2D 1×1 Temporal Loop（4 維：oc, h, w, ic）

```
for oc_tile in range(num_oc_tiles):
    for h_tile in range(num_h_tiles):
        for w_tile in range(num_w_tiles):
            for ic_tile in range(num_ic_tiles):
                DMA_load(weight[oc,ic], input_tile[h,w,ic]) → SPM[buf]
                # ic_tile==0: PLI=0; ic_tile>0: PLI ← prev PLO
                configure_AGU(buf)
                start_HDDU()
                wait_done()
                if ic_tile == num_ic_tiles - 1:
                    DMA_store(output_tile[oc,h,w]) ← SPM[buf]
                buf = 1 - buf
```

#### GEMM Temporal Loop（3 維：n, m, k）

```
for n_tile in range(num_n_tiles):
    for m_tile in range(num_m_tiles):
        clear_pli_buffer()
        for k_tile in range(num_k_tiles):
            DMA_load(A_tile[m,k], B_tile[k,n]) → SPM[buf]
            if k_tile > 0: PLI ← prev PLO（K 累加）
            configure_AGU(buf)
            start_HDDU()
            wait_done()
            buf = 1 - buf  # PLI/PLO ping-pong for K accumulation
        DMA_store(C_tile[m,n]) ← SPM[buf]
```

### 6.2 DMA/Compute Overlap Schedule

強制 ping-pong 雙緩衝，firmware 實現 DMA 與 compute 的流水線重疊：

```
Timeline:
  Wave 0: [DMA_load_0 → Compute_0]
  Wave 1:                [DMA_load_1 → Compute_1]    ← DMA_load_1 與 Compute_0 重疊
  Wave 2:                              [DMA_load_2 → Compute_2]
  ...
```

流水線控制：

```python
def run_temporal_loop_pipelined(schedule, cluster_id):
    """Pipelined temporal loop with DMA/compute overlap.
    強制 ping-pong — 所有 wave 皆使用雙緩衝流水線。"""
    buf = 0

    # Wave 0: DMA load only
    dma_load_wave(wave_idx=0, buf=buf)
    dma_wait_done()

    for wave_idx in range(schedule.total_waves):
        # Start compute for current wave
        configure_agu(buf, wave_idx)
        start_hddu()

        # While computing, DMA load next wave into opposite buffer
        next_buf = 1 - buf
        if wave_idx + 1 < schedule.total_waves:
            dma_load_wave(wave_idx + 1, next_buf)

        # Wait compute done
        wait_hddu_done()

        # DMA store current wave result（僅在 IC/K 累加完成後）
        if is_reduction_complete(schedule, wave_idx):
            dma_store_wave(wave_idx, buf)

        buf = next_buf

    dma_wait_done()  # final store
```

> **IC/K 累加期間的 store 策略**：Conv2D 的 IC 累加和 GEMM 的 K 累加期間，中間結果保留在 SPM 的 PLI/PLO 中（ping-pong 交替），不需要 DMA store。只有在累加完成時才 store 最終結果到 DRAM。

### 6.3 Multi-Cluster Barrier

多 cluster 平行執行時，firmware 的控制邏輯：

```python
def run_multi_cluster(schedule, cluster_mapping, hw):
    """
    Multi-cluster execution:
    1. 各 cluster 收到自己的 spatial tile 區間
    2. Broadcast: scan-chain config, PE program (相同)
    3. Unicast: AGU config, DMA descriptors (per-cluster 不同)
    4. 各 cluster 獨立執行 temporal loop
    5. Barrier: 等待所有 cluster 完成
    """
    # Broadcast common config
    for cluster_id in cluster_mapping.active_clusters:
        load_scan_chain(cluster_id, schedule.scan_chain)
        load_pe_program(cluster_id, schedule.pe_program)

    # Unicast per-cluster config and start
    for cluster_id in cluster_mapping.active_clusters:
        tile_range = cluster_mapping.get_tile_range(cluster_id)
        agu_cfg = compute_agu_for_tile_range(schedule, tile_range)
        configure_cluster_agu(cluster_id, agu_cfg)
        start_cluster(cluster_id)

    # Barrier: wait all clusters done
    for cluster_id in cluster_mapping.active_clusters:
        wait_cluster_done(cluster_id)
```

Code-gen 層級可使用 **loop unrolling** 將多 cluster 的平行迭代展開為獨立指令序列：

```python
# Example: 4 clusters processing H tiles [0..3]
# Unrolled code-gen output:
cluster_0_start(h_range=[0, H//4))
cluster_1_start(h_range=[H//4, H//2))
cluster_2_start(h_range=[H//2, 3*H//4))
cluster_3_start(h_range=[3*H//4, H))
barrier_all()
```

---

## 7. Lowering 產出驗證

每個 lowering 結果都必須通過以下**精確驗證**。SPM 強制 ping-pong，所有 wave tile 必須 ≤ half_group_capacity。

```python
def validate_layer_hw_config(cfg: LayerHwConfig, hw: HardwareDesc):
    group_capacity = compute_group_capacity(hw)
    half_cap = group_capacity // 2

    # ── 1. Per-Group SPM 容量精確檢查（強制 ping-pong，≤ half_cap）──
    for group_name, agu in [("PS", cfg.agu_ps), ("PD", cfg.agu_pd),
                            ("PLI", cfg.agu_pli), ("PLO", cfg.agu_plo)]:
        region_size = compute_agu_region_size(agu)
        assert region_size <= half_cap, (
            f"SPM Group {group_name}: wave tile requires {region_size} bytes, "
            f"but half_group_capacity is {half_cap} bytes"
        )

    # ── 2. Ping/Pong 地址驗證 ──
    for group_name in ["ps", "pd", "pli", "plo"]:
        layout = cfg.spm_layout.groups[group_name]
        assert layout.pingpong is True
        if layout.spm_mode == "parallel":
            assert layout.ping_base == parallel_ping_base  # 196608
            assert layout.pong_base == parallel_pong_base  # 229376
        else:
            assert layout.ping_base == 0
            assert layout.pong_base == half_cap

    # ── 3. PE program 存在 ──
    assert cfg.pe_program.template_name in KNOWN_TEMPLATES

    # ── 4. Scan-chain 長度 ──
    assert len(cfg.scan_chain) == hw.num_pes

    # ── 5. Cluster mask 合法 ──
    assert cfg.target_cluster_mask != 0
    assert cfg.target_cluster_mask < (1 << hw.num_clusters)

    # ── 6. HDDU plane enable ──
    assert cfg.hddu.plane_en in [0xF, 0xB, 0x7]

    # ── 7. AGU iter 非零 ──
    for agu in [cfg.agu_ps, cfg.agu_pd, cfg.agu_pli, cfg.agu_plo]:
        assert (agu.iter0 >= 1) or not is_plane_enabled(agu)

    # ── 8. Tiling 一致性 ──
    if cfg.tiling is not None:
        total_waves = 1
        for dim in cfg.tiling.loop_dims:
            total_waves *= cfg.tiling.loop_bounds[dim]
        assert total_waves == cfg.tiling.total_waves

    # ── 9. Multi-cluster mapping 一致性 ──
    if cfg.cluster_mapping is not None:
        active = bin(cfg.target_cluster_mask).count('1')
        assert active == len(cfg.cluster_mapping.active_clusters)
        assert active <= hw.num_clusters

        # 驗證不沿 reduction 維度切分
        if cfg.cluster_mapping.split_dim is not None:
            assert cfg.cluster_mapping.split_dim not in cfg.tiling.reduction_dims, (
                f"Multi-cluster split along reduction dim "
                f"'{cfg.cluster_mapping.split_dim}' would require "
                f"cross-cluster partial sum sync"
            )

    # ── 10. AGU ultra / SPM parallel mode 一致性 ──
    for group_name, agu in [("ps", cfg.agu_ps), ("pd", cfg.agu_pd),
                            ("pli", cfg.agu_pli), ("plo", cfg.agu_plo)]:
        layout = cfg.spm_layout.groups[group_name]
        if layout.spm_mode == "parallel":
            assert agu.ultra is True, (
                f"Group {group_name} uses parallel SPM but AGU ultra=False"
            )
        if agu.ultra:
            assert layout.spm_mode == "parallel", (
                f"AGU {group_name} has ultra=True but SPM mode is linear"
            )

    print(f"✓ Validation passed: {cfg.op_type} "
          f"({cfg.tiling.total_waves} waves, "
          f"{bin(cfg.target_cluster_mask).count('1')} clusters)")
```

### 7.1 錯誤代碼

| 代碼 | 名稱 | 說明 |
|------|------|------|
| `E_SPM_HALF_CAP_OVERFLOW` | SPM 半容量不足 | wave tile > half_group_capacity，無法滿足強制 ping-pong |
| `E_TILING_FAILED` | Tiling 搜尋失敗 | 無法找到任何 fit 進 SPM half_cap 的 tile 組合 |
| `E_CLUSTER_MAPPING_FAILED` | Cluster 映射失敗 | spatial tiles < num_clusters，無法分配 |
| `E_CLUSTER_REDUCTION_SPLIT` | 沿 reduction 維度切分 | Multi-cluster 切分了 IC（Conv2D）或 K（GEMM），會導致跨 cluster partial sum |
| `E_SPM_ULTRA_MODE_MISMATCH` | SPM/AGU 模式不一致 | AGU ultra mode 與 SPM parallel/linear mode 不匹配 |
| `E_PE_TEMPLATE_MISMATCH` | PE Template 參數不匹配 | tiled 維度超出 PE template 的暫存器限制 |
| `E_AGU_ADDR_OVERLAP` | AGU 地址重疊 | ping/pong buffer 地址空間重疊 |
