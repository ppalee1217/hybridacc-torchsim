# HybridAcc-CC：算子 Lowering 詳細規格

> 前置閱讀：[00_Overview.md](00_Overview.md)、[01_CompilationPipeline.md](01_CompilationPipeline.md)

---

## 1. 總覽

Operator Lowering 是 Stage 1 的核心任務，負責將每個高層算子 `OpDesc` 轉變為可直接驅動硬體的 `LayerHwConfig`。本文件詳細描述三種支援算子的 lowering 演算法：

1. **Conv2D 3×3**（`conv2d_3x3`）→ PE template `conv1d_k3c4s1`
2. **Conv2D 1×1**（`conv2d_1x1`）→ PE template `conv1d_k1c12s1`
3. **GEMM**（`gemm`）→ PE template `gemm`

每個算子的 lowering 包含以下子步驟：

```
a) PE template 選擇與參數計算
b) SPM address layout 計算
c) AGU bank 配置計算（PS/PD/PLI/PLO）
d) Scan-chain（PE router 拓撲）計算
e) HDDU global 配置
f) Cluster mask 決定
g) DMA descriptor 生成（optional）
```

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

### 2.2 Packing 規則

HybridAcc PE 使用 fp16（16-bit half-precision）資料格式。SPM 的 data port 為 64-bit（8 bytes），因此：

- 一個 SPM transaction = 8 bytes = 4 個 fp16 elements
- `pkt_size = 8`（固定常數）

### 2.3 SPM Address Layout 通用原則

SPM 為每個 cluster 內的 local SRAM，由 4 個 port 共用，分配給 4 個 AGU bank：

| Port | AGU Bank | 方向 | 用途 |
|------|----------|------|------|
| 0 | PS (bank 0) | Read | Weight / Matrix A |
| 1 | PD (bank 1) | Read | Activation / Matrix B |
| 2 | PLI (bank 2) | Read | Partial sum input |
| 3 | PLO (bank 3) | Write | Partial sum / output |

SPM 地址空間以 byte 為單位，由 lowering 階段進行靜態分配：

```
SPM Layout (概念圖):
┌──────────────────────┐ 0x0000
│  PS region (weight)  │
├──────────────────────┤ ps_size
│  PD region (activ.)  │
├──────────────────────┤ ps_size + pd_size
│  PLI region (psum_in)│
├──────────────────────┤ ps_size + pd_size + pli_size
│  PLO region (output) │
├──────────────────────┤ ps_size + pd_size + pli_size + plo_size
│  (unused / stack)    │
└──────────────────────┘ spm_size_bytes
```

### 2.4 Scan-chain 通用邏輯

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

### 2.5 Cluster Mask 決策

初版策略：所有 `num_clusters` 個 cluster 都參與每個 layer 的運算。

```python
def compute_cluster_mask(num_clusters: int) -> int:
    return (1 << num_clusters) - 1
```

未來可依 layer 大小做 adaptive 分配。

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
| `OC ≤ 16` | PE 暫存器限制 | 若不滿足，需進行 OC tiling |
| `N == 1` | 初版限制 | 若不滿足，外層 batch loop 由多次 layer 呼叫實現 |

### 3.4 Template 參數計算

```python
def compute_conv2d_3x3_pe_params(
    C_in: int, OC: int, H_out: int, W_out: int
) -> Dict[str, int]:
    channels_per_cycle = 4
    in_ch_pack = C_in // channels_per_cycle  # 每 cycle 幾個 pkt

    # vectors_per_kernel = KH * in_ch_pack = 3 * in_ch_pack
    vectors_per_kernel = 3 * in_ch_pack

    # KERNEL_DMA_LEN: 一次加載所有 kernel 的 weight vector 數
    # = OC * vectors_per_kernel
    kernel_dma_len = OC * vectors_per_kernel

    # OUTPUT_WINDOW_CNT_MINUS_ONE: sliding window 次數
    # conv2d 的 output pixels 排成 1D 序列
    output_window_cnt = H_out * W_out
    output_window_cnt_minus_one = output_window_cnt - 1

    # KERNEL_COUNT: 每個 window 內的 kernel loop 次數
    # = OC (每個 output pixel 需遍歷所有 output channels)
    kernel_count = OC

    # KERNEL_LOOP_INNER: 雙緩衝內層迴圈次數
    # 初版固定為 1 (不做雙緩衝)
    kernel_loop_inner = 1

    # KERNEL_LOOP_OUTER: 外層迴圈（重複整個計算）
    # 初版固定為 1
    kernel_loop_outer = 1

    return {
        "KERNEL_DMA_LEN": kernel_dma_len,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": output_window_cnt_minus_one,
        "KERNEL_COUNT": kernel_count,
        "KERNEL_LOOP_INNER": kernel_loop_inner,
        "KERNEL_LOOP_OUTER": kernel_loop_outer,
    }
```

**範例**：`C_in=16, OC=16, H_out=14, W_out=14`
- `in_ch_pack = 16/4 = 4`
- `vectors_per_kernel = 3 * 4 = 12`
- `kernel_dma_len = 16 * 12 = 192`
- `output_window_cnt = 14 * 14 = 196`
- `output_window_cnt_minus_one = 195`
- `kernel_count = 16`

### 3.5 SPM Layout 計算

```python
def compute_conv2d_3x3_spm_layout(
    C_in: int, OC: int,
    H_in: int, W_in: int,
    H_out: int, W_out: int,
    spm_size_bytes: int
) -> Dict[str, int]:
    pkt_size = 8
    in_ch_pack = C_in // 4
    out_ch_pack = max(1, (OC + 3) // 4)

    # PS (weight): OC * KH(3) * KW(3) * in_ch_pack * pkt_size
    ps_size = OC * 3 * 3 * in_ch_pack * pkt_size

    # PD (activation): H_in * W_in * in_ch_pack * pkt_size
    pd_size = H_in * W_in * in_ch_pack * pkt_size

    # PLI (partial sum input): H_out * W_out * out_ch_pack * pkt_size
    pli_size = H_out * W_out * out_ch_pack * pkt_size

    # PLO (output): same as PLI
    plo_size = pli_size

    total = ps_size + pd_size + pli_size + plo_size
    if total > spm_size_bytes:
        raise TilingFailed(f"Required SPM {total} > available {spm_size_bytes}")

    ps_base = 0
    pd_base = ps_size
    pli_base = ps_size + pd_size
    plo_base = ps_size + pd_size + pli_size

    return {
        "ps_base": ps_base,
        "pd_base": pd_base,
        "pli_base": pli_base,
        "plo_base": plo_base,
        "ps_size": ps_size,
        "pd_size": pd_size,
        "pli_size": pli_size,
        "plo_size": plo_size,
    }
```

### 3.6 AGU 配置計算

#### 3.6.1 AGU PS（Weight，Bank 0）

權重在 SPM 中的存放順序為 `[OC, KH, KW, IC_pack]`，每個 element 為一個 8-byte packet。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `spm.ps_base` | Weight 區起始 |
| `ITER01` | `(KW << 16) \| in_ch_pack` | iter0 = IC_pack, iter1 = KW |
| `ITER23` | `(OC << 16) \| KH` | iter2 = KH, iter3 = OC |
| `STRIDE0` | `pkt_size` | 每個 IC pack +8 |
| `STRIDE1` | `in_ch_pack * pkt_size` | 跨 KW: IC_pack * 8 |
| `STRIDE2` | `KW * in_ch_pack * pkt_size` | 跨 KH |
| `STRIDE3` | `KH * KW * in_ch_pack * pkt_size` | 跨 OC |
| `TAG_BASE` | `0` | Tag 從 0 開始 |
| `TAG_STRIDE0` | `1` | 每個 inner loop tag +1 |
| `TAG_STRIDE1` | `0` | |
| `TAG_CTRL` | `1` | Tag tied to loop 1 (KH) |
| `MASK_CFG` | `0xF` | 全部 lane 啟用 |
| `LANE_CFG` | `0` | 預設 |
| `CTRL` | `0x0` | (start bit 由 HDDU CTRL 統一控制) |

#### 3.6.2 AGU PD（Activation，Bank 1）

Activation 在 SPM 中的存放順序為 `[H_in, W_in, IC_pack]`。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `spm.pd_base` | Activation 區起始 |
| `ITER01` | `(H_in << 16) \| in_ch_pack` | iter0 = IC_pack, iter1 = H_in |
| `ITER23` | `(1 << 16) \| W_in` | iter2 = W_in, iter3 = 1 |
| `STRIDE0` | `pkt_size` | 每個 IC pack +8 |
| `STRIDE1` | `W_in * in_ch_pack * pkt_size` | 跨 H: W*IC*8 |
| `STRIDE2` | `in_ch_pack * pkt_size` | 跨 W: IC*8 |
| `STRIDE3` | `0` | 不使用 |
| `TAG_BASE` | `0` | |
| `TAG_STRIDE0` | `1` | |
| `TAG_STRIDE1` | `0` | |
| `TAG_CTRL` | `1` | Loop 1 (H) drives tag |
| `MASK_CFG` | `0xF` | |
| `LANE_CFG` | `0` | |
| `CTRL` | `0x0` | |

#### 3.6.3 AGU PLI（Partial Sum In，Bank 2）

Partial sum 在 SPM 中的排列為 `[H_out, W_out, OC_pack]`。

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `spm.pli_base` | Partial sum 輸入區 |
| `ITER01` | `(H_out << 16) \| out_ch_pack` | iter0 = OC_pack, iter1 = H_out |
| `ITER23` | `(1 << 16) \| W_out` | iter2 = W_out, iter3 = 1 |
| `STRIDE0` | `pkt_size` | 每個 OC pack +8 |
| `STRIDE1` | `W_out * out_ch_pack * pkt_size` | 跨 H |
| `STRIDE2` | `out_ch_pack * pkt_size` | 跨 W |
| `STRIDE3` | `0` | |
| `TAG_BASE` | `0` | |
| `TAG_STRIDE0` | `1` | |
| `TAG_STRIDE1` | `0` | |
| `TAG_CTRL` | `1` | Loop 1 (H) drives tag |
| `MASK_CFG` | `0xF` | |
| `LANE_CFG` | `0` | |
| `CTRL` | `0x0` | |

#### 3.6.4 AGU PLO（Output，Bank 3）

與 PLI 完全相同的地址模式，但 base address 指向 output 區：

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `spm.plo_base` | Output 區 |
| 其餘 | 與 PLI 相同 | 相同大小與 stride |

### 3.7 HDDU Global 配置

| Register | Value | 說明 |
|----------|-------|------|
| `PLANE_EN` | `0xF` | PS/PD/PLI/PLO 全開 |
| `PLANE_MODE` | `0x1` | Conv mode |

### 3.8 Scan-chain 計算

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

### 3.9 SPM Config Map

SPM 有 4 個 port，需要映射到 group（決定仲裁行為）：

```python
spm_config_map = 0xE4  # port0→group0, port1→group1, port2→group2, port3→group3
```

這是標準 1:1 映射。

### 3.10 完整 Lowering 範例

**輸入**：
- `conv2d_3x3`：`input[1,14,14,16]`, `weight[16,3,3,16]`, `output[1,14,14,16]`, stride=1, padding=1
- Hardware：4 clusters, 64 PEs, 4 bus, 32KB SPM

**計算過程**：

1. `in_ch_pack = 16/4 = 4`
2. `out_ch_pack = 16/4 = 4`
3. `H_out = (14+2-3)/1+1 = 14`, `W_out = 14`

SPM Layout：
- `ps_size = 16 * 3 * 3 * 4 * 8 = 4608`
- `pd_size = 14 * 14 * 4 * 8 = 6272`
- `pli_size = 14 * 14 * 4 * 8 = 6272`
- `plo_size = 6272`
- Total = 23424 < 32768 ✓

PE Params：
- `KERNEL_DMA_LEN = 16 * 12 = 192`
- `OUTPUT_WINDOW_CNT_MINUS_ONE = 196 - 1 = 195`
- `KERNEL_COUNT = 16`
- `KERNEL_LOOP_INNER = 1`
- `KERNEL_LOOP_OUTER = 1`

AGU PS：
- BASE=0, ITER01=(3<<16)|4=0x30004, ITER23=(16<<16)|3=0x100003
- STRIDE0=8, STRIDE1=32, STRIDE2=96, STRIDE3=288

AGU PD：
- BASE=4608, ITER01=(14<<16)|4=0xE0004, ITER23=(1<<16)|14=0x1000E
- STRIDE0=8, STRIDE1=896, STRIDE2=32, STRIDE3=0

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
| `OC ≤ 16` | PE 暫存器限制 |

### 4.4 Template 參數計算

```python
def compute_conv2d_1x1_pe_params(
    C_in: int, OC: int, H_out: int, W_out: int
) -> Dict[str, int]:
    channels_per_cycle = 12
    in_ch_pack = C_in // channels_per_cycle

    # KH=KW=1，vectors_per_kernel = 1 * in_ch_pack
    vectors_per_kernel = in_ch_pack

    kernel_dma_len = OC * vectors_per_kernel
    output_window_cnt_minus_one = H_out * W_out - 1
    kernel_count = OC
    kernel_loop_inner = 1
    kernel_loop_outer = 1

    return {
        "KERNEL_DMA_LEN": kernel_dma_len,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": output_window_cnt_minus_one,
        "KERNEL_COUNT": kernel_count,
        "KERNEL_LOOP_INNER": kernel_loop_inner,
        "KERNEL_LOOP_OUTER": kernel_loop_outer,
    }
```

**與 conv2d_3x3 的差異**：
- `vectors_per_kernel` 為 `1 * in_ch_pack`（而非 `3 * in_ch_pack`），因為 kernel spatial size = 1
- `channels_per_cycle = 12`（而非 4）

### 4.5 SPM Layout 計算

```python
def compute_conv2d_1x1_spm_layout(
    C_in: int, OC: int,
    H_in: int, W_in: int,
    H_out: int, W_out: int,
    spm_size_bytes: int
) -> Dict[str, int]:
    pkt_size = 8
    in_ch_pack = C_in // 12
    out_ch_pack = max(1, (OC + 3) // 4)

    # PS (weight): OC * 1 * 1 * in_ch_pack * pkt_size
    ps_size = OC * in_ch_pack * pkt_size

    # PD (activation): H_in * W_in * in_ch_pack * pkt_size
    pd_size = H_in * W_in * in_ch_pack * pkt_size

    # PLI/PLO: H_out * W_out * out_ch_pack * pkt_size
    pli_size = H_out * W_out * out_ch_pack * pkt_size
    plo_size = pli_size

    total = ps_size + pd_size + pli_size + plo_size
    if total > spm_size_bytes:
        raise TilingFailed(...)

    return {
        "ps_base": 0,
        "pd_base": ps_size,
        "pli_base": ps_size + pd_size,
        "plo_base": ps_size + pd_size + pli_size,
        "ps_size": ps_size,
        "pd_size": pd_size,
        "pli_size": pli_size,
        "plo_size": plo_size,
    }
```

### 4.6 AGU 配置

#### AGU PS（Weight）

1×1 conv 的 weight shape 為 `[OC, 1, 1, IC_pack]`，展平為 `[OC, IC_pack]`：

| Register | Value |
|----------|-------|
| `BASE_ADDR` | `spm.ps_base` |
| `ITER01` | `(1 << 16) \| in_ch_pack` |
| `ITER23` | `(OC << 16) \| 1` |
| `STRIDE0` | `pkt_size` |
| `STRIDE1` | `0` |
| `STRIDE2` | `0` |
| `STRIDE3` | `in_ch_pack * pkt_size` |
| `TAG_BASE` | `0` |
| `TAG_CTRL` | `0` |
| `MASK_CFG` | `0xF` |

#### AGU PD / PLI / PLO

與 conv2d_3x3 完全相同的模式（只是 `in_ch_pack` 的基數不同）。

### 4.7 Scan-chain

與 conv2d_3x3 共用相同的 `compute_conv2d_scan_chain()` 函數。scan-chain 拓撲不因 kernel size 改變。

### 4.8 HDDU 配置

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

GEMM 需要 3 維 tiling：

```
for n_tile in range(num_n_tiles):
    for m_tile in range(num_m_tiles):
        for k_tile in range(num_k_tiles):
            C_tile[m_tile, n_tile] += A_tile[m_tile, k_tile] × B_tile[k_tile, n_tile]
```

Tiling 參數由 SPM 容量決定：

```python
def compute_gemm_tiling(
    M: int, N: int, K: int,
    num_pes: int, spm_size_bytes: int
) -> Dict[str, int]:
    pkt_size = 8
    elements_per_pkt = 4  # fp16

    # K tile：取決於 PE DM (data memory) 大小
    # PE DM = 256 words (16-bit) = 512 bytes
    # K_tile 受限於 PE local storage
    k_tile_dim = min(K, 32)  # 初版固定 32

    # N tile：取決於 bus 上的 PE 分配
    # 每條 bus 上的 PE 共享 N 維度的 slice
    n_tile_dim = min(N, num_pes // 4)  # 初版簡化

    # M tile：取決於 VPSUM 暫存器數
    m_tile_dim = min(M, 8)

    # 驗證 SPM 容量
    # A tile: m_tile * k_tile * pkt_size / elements_per_pkt
    a_tile_bytes = m_tile_dim * k_tile_dim * pkt_size // elements_per_pkt
    # B tile: k_tile * n_tile * pkt_size / elements_per_pkt
    b_tile_bytes = k_tile_dim * n_tile_dim * pkt_size // elements_per_pkt
    # C tile: m_tile * n_tile * pkt_size / elements_per_pkt
    c_tile_bytes = m_tile_dim * n_tile_dim * pkt_size // elements_per_pkt

    total = a_tile_bytes + b_tile_bytes + 2 * c_tile_bytes  # PLI + PLO
    if total > spm_size_bytes:
        raise TilingFailed(...)

    num_m_tiles = (M + m_tile_dim - 1) // m_tile_dim
    num_n_tiles = (N + n_tile_dim - 1) // n_tile_dim
    num_k_tiles = (K + k_tile_dim - 1) // k_tile_dim

    return {
        "k_tile_dim": k_tile_dim,
        "n_tile_dim": n_tile_dim,
        "m_tile_dim": m_tile_dim,
        "num_m_tiles": num_m_tiles,
        "num_n_tiles": num_n_tiles,
        "num_k_tiles": num_k_tiles,
    }
```

### 5.4 Template 參數計算

```python
def compute_gemm_pe_params(
    M: int, N: int, K: int,
    tiling: Dict[str, int],
    num_pes: int
) -> Dict[str, int]:
    k_tile = tiling["k_tile_dim"]
    n_tile = tiling["n_tile_dim"]
    m_tile = tiling["m_tile_dim"]

    # KERNEL_DMA_STORE_LEN: C tile output 的 DMA 長度
    # = m_tile * n_tile / elements_per_pkt
    kernel_dma_store_len = m_tile * n_tile // 4

    # KERNEL_DMA_LOAD_LEN: A/B tile load 的 DMA 長度
    kernel_dma_load_len = k_tile * max(n_tile, m_tile) // 4

    # INPUT_DIM: K tile 維度
    input_dim = k_tile

    # OUTPUT_DIM: output tile 較小維度
    output_dim = m_tile

    # PSUM_COUNT: partial sum 累加次數
    psum_count = m_tile * n_tile // 4

    # NUM_OF_KERNEL_SETS: K 維度的 tile 數
    num_of_kernel_sets = tiling["num_k_tiles"]

    # NUM_OF_N_TILES / NUM_OF_M_TILES
    num_of_n_tiles = tiling["num_n_tiles"]
    num_of_m_tiles = tiling["num_m_tiles"]

    return {
        "KERNEL_DMA_STORE_LEN": kernel_dma_store_len,
        "KERNEL_DMA_LOAD_LEN": kernel_dma_load_len,
        "INPUT_DIM": input_dim,
        "OUTPUT_DIM": output_dim,
        "PSUM_COUNT": psum_count,
        "NUM_OF_KERNEL_SETS": num_of_kernel_sets,
        "NUM_OF_N_TILES": num_of_n_tiles,
        "NUM_OF_M_TILES": num_of_m_tiles,
        "K_TILE_DIM": k_tile,
    }
```

### 5.5 SPM Layout 計算

```python
def compute_gemm_spm_layout(
    tiling: Dict[str, int],
    spm_size_bytes: int
) -> Dict[str, int]:
    pkt_size = 8
    ep = 4  # elements_per_pkt
    m = tiling["m_tile_dim"]
    n = tiling["n_tile_dim"]
    k = tiling["k_tile_dim"]

    # PS region: A tile [M_tile, K_tile]
    ps_size = (m * k // ep) * pkt_size

    # PD region: B tile [K_tile, N_tile]
    pd_size = (k * n // ep) * pkt_size

    # PLI region: C tile accumulator [M_tile, N_tile]
    pli_size = (m * n // ep) * pkt_size

    # PLO region: C tile output [M_tile, N_tile]
    plo_size = pli_size

    total = ps_size + pd_size + pli_size + plo_size
    if total > spm_size_bytes:
        raise TilingFailed(...)

    return {
        "ps_base": 0,
        "pd_base": ps_size,
        "pli_base": ps_size + pd_size,
        "plo_base": ps_size + pd_size + pli_size,
        "ps_size": ps_size,
        "pd_size": pd_size,
        "pli_size": pli_size,
        "plo_size": plo_size,
    }
```

### 5.6 AGU 配置

#### AGU PS（Matrix A，Bank 0）

A tile shape `[M_tile, K_tile]`：

| Register | Value | 說明 |
|----------|-------|------|
| `BASE_ADDR` | `spm.ps_base` | A tile 起始 |
| `ITER01` | `(K_tile/ep << 16) \| (M_tile/ep)` | iter0, iter1 |
| `ITER23` | `0x10001` | iter2=1, iter3=1 |
| `STRIDE0` | `pkt_size` | |
| `STRIDE1` | `M_tile/ep * pkt_size` | 跨 K |
| `STRIDE2` | `0` | |
| `STRIDE3` | `0` | |
| `TAG_BASE` | `0` | |
| `TAG_CTRL` | `1` | |
| `MASK_CFG` | `0xF` | |

#### AGU PD（Matrix B，Bank 1）

B tile shape `[K_tile, N_tile]`：

| Register | Value |
|----------|-------|
| `BASE_ADDR` | `spm.pd_base` |
| `ITER01` | `(N_tile/ep << 16) \| (K_tile/ep)` |
| `STRIDE0` | `pkt_size` |
| `STRIDE1` | `K_tile/ep * pkt_size` |
| 其餘 | 與 PS 類似 |

#### AGU PLI（Unused or Zero-init，Bank 2）

GEMM 的 PLI 通常不使用（partial sum 從 0 開始），或指向 zero-init 區域：

| Register | Value |
|----------|-------|
| `BASE_ADDR` | `spm.pli_base` |
| `ITER01` | `(1 << 16) \| 1` |
| 其餘 STRIDE | `0` |
| `CTRL` | `0x0` |

#### AGU PLO（Matrix C，Bank 3）

C tile shape `[M_tile, N_tile]`：

| Register | Value |
|----------|-------|
| `BASE_ADDR` | `spm.plo_base` |
| `ITER01` | `(N_tile/ep << 16) \| (M_tile/ep)` |
| `STRIDE0` | `pkt_size` |
| `STRIDE1` | `M_tile/ep * pkt_size` |
| 其餘 | 類似 PS |

### 5.7 HDDU 配置

| Register | Value | 說明 |
|----------|-------|------|
| `PLANE_EN` | `0xB` (0b1011) | PS/PD/PLO 開啟，PLI 關閉 |
| `PLANE_MODE` | `0x2` | GEMM mode |

> 注意：GEMM 模式下 PLI 可能不啟用（因為 partial sum 累加在 PE 內部完成），此處依 ComputeCluster.md §9.3 範例為 `0xB`。實際值視 PE kernel 行為而定。

### 5.8 Scan-chain

GEMM 的 scan-chain 拓撲可能與 Conv2D 不同，取決於 tiling 的 N/M 維度如何映射到 PE array：

```python
def compute_gemm_scan_chain(
    num_pes: int,
    num_bus: int,
    tiling: Dict[str, int]
) -> List[ScanChainEntry]:
    """
    GEMM scan chain:
    - 每條 bus 處理 N 維度的一個 slice
    - bus 上的 PE 串成 chain，處理 M 維度
    - route_mode 取決於 PE 在 chain 中的位置
    """
    entries = []
    pes_per_bus = num_pes // num_bus

    for bus_idx in range(num_bus):
        for pe_local_idx in range(pes_per_bus):
            global_pe_id = bus_idx * pes_per_bus + pe_local_idx
            ps_id = global_pe_id
            pd_id = global_pe_id

            if pe_local_idx == 0:
                route_mode = 1  # PLI_FROM_BUS_PLO_TO_LN
                pli_id = bus_idx
                plo_id = global_pe_id
            elif pe_local_idx == pes_per_bus - 1:
                route_mode = 2  # PLI_FROM_LN_PLO_TO_BUS
                pli_id = global_pe_id
                plo_id = bus_idx
            else:
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

---

## 6. Tiling 策略（通用）

### 6.1 何時需要 Tiling

當算子的 working set（所有 SPM buffer 加總）超過單一 cluster 的 SPM 容量時，需要將計算分割為多個 tile，逐一執行。

### 6.2 初版策略

初版採用**保守固定 tiling**策略：

- Conv2D：假設整個 layer 可 fit 進 SPM（若不行則報錯）
- GEMM：K 維度固定 tile 為 32，M/N 維度依 SPM 容量調整

### 6.3 未來擴充

未來版本可加入：

1. **Auto-tiling search**：遍歷合法 tile 組合，選擇最小化 DMA overhead 的方案
2. **Multi-layer fusion**：連續 Conv → Conv 或 Conv → GEMM 的 tiling 融合
3. **Double buffering**：利用 `KERNEL_LOOP_INNER > 1` 實現 compute/DMA overlap

---

## 7. Lowering 產出驗證

每個 lowering 結果都必須通過以下 assertion：

```python
def validate_layer_hw_config(cfg: LayerHwConfig, hw: HardwareDesc):
    # 1. SPM 容量
    total_spm = sum([
        cfg.agu_ps.base_addr + compute_agu_region_size(cfg.agu_ps),
        # ... 對 pd, pli, plo 同理
    ])
    assert total_spm <= hw.spm_size_bytes

    # 2. PE program 存在
    assert cfg.pe_program.template_name in KNOWN_TEMPLATES

    # 3. Scan-chain 長度
    assert len(cfg.scan_chain) == hw.num_pes

    # 4. Cluster mask 合法
    assert cfg.target_cluster_mask != 0
    assert cfg.target_cluster_mask < (1 << hw.num_clusters)

    # 5. HDDU plane enable
    assert cfg.hddu.plane_en in [0xF, 0xB, 0x7]  # 已知合法組合

    # 6. AGU iter 非零（iter0 必須 ≥ 1）
    for agu in [cfg.agu_ps, cfg.agu_pd, cfg.agu_pli, cfg.agu_plo]:
        assert (agu.iter0 >= 1) or not is_plane_enabled(agu)
```
