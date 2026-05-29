# AddressGenerateUnit (AGU) 規格書

文件樹： [../../../../doc/index.md](../../../../doc/index.md) -> [../index.md](../index.md) -> [README.md](README.md) -> 本頁。

## 1. 目的與範圍

本文件定義 HybridAcc 在 NoC 資料分發路徑使用的 AGU（Address Generate Unit）行為，目標是把 `test/test_noc_sim.cpp` 的軟體 loop/index/tag 規律，轉成可硬體化的可配置規格。

AGU 負責：
- 產生來源位址（SRAM / DMA 線性位址）
- 產生目的標籤（NoC `addr[5:0]` tag）
- 產生平面旗標（NoC `addr[6]` ultra mode）
- 產生封包有效 lane mask（16-bit lane mask）
- 以多層迴圈（wave / tile / element）控制輸出節奏

不負責：
- NoC ready/valid 握手仲裁
- Scan-chain 寫入
- PE 內部路由執行

---

## 2. 與 NoC 封包對應

依據目前 NoC/Router 實作，AGU 需輸出以下欄位：

- `pkt.data[255:0]`: 單拍最大 256-bit（3 port x 64-bit 有效使用 192-bit）
- `pkt.addr[15:0]`：
	- `addr[5:0]` = tag
	- `addr[6]` = ultra flag（1: SIMD 分 port，0: broadcast）
	- `addr[7]` 目前保留（Router 會轉換成 command base bit 行為）
- `pkt.mask[15:0]`: lane valid mask

NoC Router decode 規則：
- `is_ultra = addr[6]`
- ultra 時每個 port 取 `data[64*i+63:64*i]`
- normal 時所有 port 共用 `data[63:0]`

---

## 3. AGU 輸入/輸出介面

## 3.1 Port 定義（建議）

| 名稱 | 方向 | 寬度 | 說明 |
|---|---|---:|---|
| `clk` | in | 1 | 系統時脈 |
| `reset_n` | in | 1 | 低有效重置 |
| `cfg_valid` | in | 1 | config 寫入觸發 |
| `cfg_addr` | in | 12 | AGU MMIO 子位址 |
| `cfg_wdata` | in | 32 | AGU MMIO 寫入資料 |
| `cfg_write` | in | 1 | AGU MMIO 寫入使能 |
| `cfg_rdata` | out | 32 | AGU MMIO 讀出 |
| `start` | in | 1 | 啟動本 AGU |
| `stop` | in | 1 | 停止本 AGU |
| `busy` | out | 1 | AGU 執行中 |
| `done` | out | 1 | AGU 完成脈波 |
| `pkt_valid` | out | 1 | 輸出封包有效 |
| `pkt_ready` | in | 1 | 下游可接受 |
| `pkt_data` | out | 256 | 封包資料 |
| `pkt_addr` | out | 16 | Tag + ultra |
| `pkt_mask` | out | 16 | lane mask |

## 3.2 時序語義

- `pkt_valid && pkt_ready` 才前進一筆 loop iteration。
- `busy=1` 期間，除非 `cfg_live_update=1`，否則 config latch 不可改動。
- `done` 在最後一筆傳輸 handshake 後 1 cycle 拉高。
- `CTRL.bit0(start)` 與 `CTRL.bit1(stop)` 為 one-shot command（被 AGU 接受後會自動清零）。
- 若同拍同時出現 start/stop，`stop` 優先，避免停機後立即重啟。

---

## 4. Loop Controller 規格

AGU 使用 4 層巢狀 loop（L0 最內層）：

- `iter[i]`：第 i 層迭代次數
- `stride[i]`：第 i 層位址步進（單位：element）
- `tag_stride[i]`：第 i 層 tag 步進
- `tag_update_level`：指定 tag 在哪一層更新

位址公式（抽象）：

`addr_elem = base_addr + Σ(loop_idx[i] * stride[i]) + lane_offset`

tag 公式（抽象）：

`tag = tag_base + loop_idx[tag_update_level] * tag_stride[tag_update_level] + tag_bias`

其中：
- `lane_offset` 由 packetizer 決定（4-lane 或 12-lane）
- `tag_bias` 用於 wave-local 映射

---

## 5. 與 test_noc_sim 對應的行為模板

## 5.1 Conv2D 模式

使用 wave 維度：
- `wave_out_h`
- `wave_out_ch`
- `wave_in_ch`

### PS（Weight）
- 索引：`weight_idx(och, kh, kw, ich)`
- 封包：normal 4 lane；ultra 為 3 port x 4 lane
- tag：`tag = kh`
- mask：有效 `ich` lane 置 1

### PD（Activation）
- 索引：`act_idx(ih, iw, ich)`；ultra 用 `base_h = port * loop_in_height + ih`
- tag：`tag = ih - ih_start`
- channels_per_packet：normal 12、ultra 4（每 port）

### PLI（Partial Sum In）
- 只在 `wave_ic == 0` 送入
- 索引：`ps_idx(oh, ow, och)`；ultra 用 `base_h = port * loop_out_height + oh`
- tag：`tag = oh - oh_range.start`

### PLO（Partial Sum Out Read）
- 只在 `wave_ic == wave_in_ch-1` 發 request
- request tag：`tag = oh - oh_range.start`
- 回寫 base index：`base_idx = ps_idx(oh, ow, och)`
- ultra response 的 per-port stride：`loop_out_height * out_width * out_ch`

## 5.2 GEMM 模式

張量定義：
- A: `M x K`
- B: `K x N`
- D/C: `M x N`

tile：
- `PE_M`, `PE_N`, `PE_K`
- grid：`grid_m`, `grid_n`, `grid_k`

wave 可由 `wave_m/n/k` 或 `grid_*_per_wave[]` 控制。

### PS（Weight=B）

normal：
- 索引：`weight_idx(k, n)`
- tag：`tag = k_idx * grid_n + n_idx`

ultra：
- 每 port 對應不同 `k_tile`
- tag：`tag = (n_base - n_start) / PE_N`

### PD（Activation=A）

normal：
- 索引：`act_idx(m, k_global)`
- tag：`tag = k_idx * grid_m + m_idx`

ultra：
- 每 port 對應不同 `k_tile`
- tag：`tag = m_idx_local`

### PLI（Input D）

normal：
- tag：`tag = m_idx * grid_n + n_idx`

ultra + `grid_k > 1`（K-split）：
- 僅使用 port0 寫入（mask=0x1）
- tag：`tag = m_idx_local * n_tiles + n_idx_local`

ultra + `grid_k == 1`：
- 逐 port 寫入各自 row region
- tag：`tag = m_idx_local * n_tiles + n_idx_local`

### PLO（Output C read）

只在最後 `wave_k` 發 request。

normal：
- tag：`tag = m_idx_local * n_tiles + n_idx_local`

ultra + `grid_k > 1`：
- 走 normal response（Port2 路徑）
- tag 同上

ultra + `grid_k == 1`：
- tag：`tag = n_idx_local`
- per_port_stride：`rows_per_port * N`

---

## 6. AGU MMIO Register Map

以每個 AGU bank `0x100` bytes 為單位：

- Weight AGU: `0x000~0x0FF`
- Input AGU: `0x100~0x1FF`
- PLI AGU: `0x200~0x2FF`
- PLO AGU: `0x300~0x3FF`

| Offset | 名稱 | 欄位 | 說明 |
|---:|---|---|---|
| `0x00` | `BASE_ADDR` | `[31:0] base_addr` | element base（對應 SRAM address space） |
| `0x04` | `BASE_ADDR_H` | `[31:0]` | >32-bit 位址擴充（可選） |
| `0x08` | `ITER01` | `[15:0] iter0`, `[31:16] iter1` | loop 次數 |
| `0x0C` | `ITER23` | `[15:0] iter2`, `[31:16] iter3` | loop 次數 |
| `0x10` | `STRIDE0` | `[31:0]` | loop0 stride |
| `0x14` | `STRIDE1` | `[31:0]` | loop1 stride |
| `0x18` | `STRIDE2` | `[31:0]` | loop2 stride |
| `0x1C` | `STRIDE3` | `[31:0]` | loop3 stride |
| `0x20` | `CTRL` | `bit0 start`, `bit1 stop`, `bit2 soft_reset`, `bit3 ultra`, `bit4 circular`, `bit5 cfg_live_update` | 控制 |
| `0x24` | `STATUS` | `bit0 busy`, `bit1 done`, `bit2 error`, `bit3 stalled` | 狀態 |
| `0x28` | `LANE_CFG` | `[4:0] lanes_per_pkt`, `[9:5] lanes_per_port`, `[15:10] ports` | 封包 lane 設定 |
| `0x2C` | `WAVE_CFG` | `[7:0] wave_h`, `[15:8] wave_c0`, `[23:16] wave_c1` | wave 次數 |
| `0x30` | `WAVE_TILE0` | `[15:0] tile_count0`, `[31:16] tile_count1` | per-wave tile（可覆蓋） |
| `0x34` | `WAVE_TILE1` | 同上 | per-wave tile |
| `0x38` | `ADDR_CLAMP` | `[15:0] min`, `[31:16] max` | 邊界保護 |
| `0x40` | `TAG_BASE` | `[5:0] base` | tag base |
| `0x44` | `TAG_STRIDE0` | `[7:0] stride` | tag stride level0 |
| `0x48` | `TAG_STRIDE1` | `[7:0] stride` | tag stride level1 |
| `0x4C` | `TAG_CTRL` | `[1:0] tag_update_level`, `[4:2] tag_mode` | tag 更新規則 |
| `0x50` | `ULTRA_STRIDE` | `[31:0] per_port_stride` | response mapping 使用 |
| `0x54` | `MASK_CFG` | `[15:0] default_mask` | lane mask 缺省值 |
| `0x58` | `ERR_CODE` | `[31:0]` | 錯誤碼 |
| `0x5C` | `DBG_LAST_TAG` | `[15:0]` | 最後輸出 tag |
| `0x60` | `DBG_LAST_ADDR` | `[31:0]` | 最後輸出位址 |

註：`test_hddu_unit.cpp` 已使用子集合：`BASE_ADDR(0x00)`, `ITER01(0x08)`, `STRIDE0(0x10)`, `CTRL(0x20)`, `TAG_BASE(0x40)`, `TAG_STRIDE(0x44)`, `TAG_CTRL(0x4C)`。

`CTRL` 命令語意補充：
- `bit0` 與 `bit1` 定義為命令位元（command bit），不是 level-enable。
- 軟體可用「寫 1」發命令，硬體在命令生效後自行清位。

---

## 7. 錯誤與例外行為

- `iter[i]==0`：在 MMIO 寫入 `ITER01/ITER23` 時正規化為 `1`，避免空 loop 歧義（不使用每拍 `safe_iter()`）。
- tag 溢位：只取低 6 bit，並設 `STATUS.error=1`。
- address 超界：封包不發送，`STATUS.error=1`，`ERR_CODE=ADDR_OOB`。
- `pkt_ready` 長時間為 0：`STATUS.stalled=1`。

---

## 8. 時序與硬體化實作（Critical Path 優化）

以下為 AGU 在硬體時序上的實作建議與目前對齊方向，目標是在維持既有邏輯語意下，降低 critical path：

### 8.1 三階段管線（Stage0/1/2）

- Stage 0：鎖住 `idx` 與控制欄位（`base/stride/tag_ctrl/mask/ultra/last`）
- Stage 1：執行乘法 `idx[i] * stride[i]`，同時完成 tag 乘法項 `tag_mul = idx[level] * tag_stride(level)`
- Stage 2：執行加總與輸出（`addr/tag/mask/ultra`）
- 以 `valid/ready` 反壓機制串接各 stage，確保下游阻塞時資料不遺失

時序語意：
- 只有在最終輸出 `valid && ready` handshake 成功時，該筆 descriptor 才算消耗
- `done` 在最後一筆（`last=1`）於輸出端 handshake 後產生 pulse

### 8.2 `iter==0` 正規化前移（MMIO write-time normalize）

- `ITER01/ITER23` 寫入時即做 `0 -> 1` 正規化
- 避免把 `safe_iter()` 放在每拍 loop 計算路徑，降低控制路徑延遲

### 8.3 `calc_addr` 使用平衡加法樹

位址計算由線性累加改為平衡加法樹：

`addr = base + (p0+p1) + (p2+p3)`，其中 `p{i}=idx[i]*stride[i]`

- 地址樹切為 `n0` 三級子管線：
	- `n0_s0`: `s01=(p0+p1)`, `s23=(p2+p3)`
	- `n0_s1`: `total=base+s01+s23`
	- `n0_out`: `addr=total[31:0]`
- 在管線為空且下游可收時，允許啟動期直通（startup bypass）以維持原有可觀測啟動延遲
- 可對應 DSP + pipeline register 的硬體 mapping

### 8.5 Tag 計算路徑調整

- 原先 `calc_tag(tag_base, index, stride)` 中的乘法，已前移至 `s0->s1` 轉移時完成
- Stage2 僅做 `tag = tag_base + tag_mul` 與 low 6-bit 截位
- 這樣可降低輸出 stage 的乘法負擔，縮短關鍵路徑

### 8.4 loop 前進改為 next-state 預計算 + 註冊更新

- 當拍只計算 `next_idx` 與 `all_done`
- 下一拍才把 `idx_reg <= next_idx`
- 避免「連鎖 carry + 狀態輸出控制」在同拍形成過長組合路徑

---

## 9. 驗證重點（對齊 test_noc_sim）

- Conv2D：`kh` 與 `oh` 對應 tag 的可重現性。
- GEMM normal：`k_idx*grid_n+n_idx` / `k_idx*grid_m+m_idx`。
- GEMM ultra：`grid_k==1` 與 `grid_k>1` 的 tag 切換規則。
- PLO response mapping：normal/ultra 寫回 index 必須與 `base_idx + port*stride + lane_offset` 一致。
