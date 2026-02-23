# HybridDataDeliverUnit (HDDU) 規格書 v3

> 適用檔案：`simulator/include/Cluster/HybridDataDeliverUnit.hpp`（目前實作）

## 1. 模組定位

`HybridDataDeliverUnit`（HDDU）為 Cluster 內的資料搬運核心，負責：

1. 管理 4 組 AGU（PS/PD/PLI/PLO）
2. 驅動 4 個 SPM 連接埠（0/1/2 讀、3 寫）
3. 透過型別化 valid-ready 介面與 NoC 交換資料
4. 提供 MMIO 控制、狀態與統計計數器

本版本重點為「四通道獨立平行運作」，不再使用舊版共享仲裁輸出路徑。

---

## 2. Template 與常數

```cpp
template <int SPM_ADDR_BITS = 32, int NOC_TAG_BITS = 6, int DATA_BITS = 192>
class HybridDataDeliverUnit;
```

- `SPM_ADDR_BITS`：SPM address 位寬
- `NOC_TAG_BITS`：tag 位寬
- `DATA_BITS`：PS/PD/PLI/PLO 單筆 payload 位寬（例如 192）

內部常數：

- `NUM_AGU = 4`
- `NUM_SPM = 4`
- `NUM_SEND_PLANES = 3`（PS/PD/PLI）
- `RECV_PLANE = 3`（PLO）
- `NOC_ADDR_BITS = NOC_TAG_BITS + 1`
- `DATA_BYTES = DATA_BITS / 8`

NoC 地址編碼（送出與 PLO request 共用）：

- `addr[NOC_TAG_BITS-1:0] = tag`
- `addr[NOC_TAG_BITS] = ultra`

---

## 3. 介面定義

## 3.1 Clock/Reset 與中斷

| 名稱 | 方向 | 說明 |
|---|---|---|
| `clk` | in | 系統時脈 |
| `reset_n` | in | 低有效 reset |
| `interrupt` | out | `status.err` 或 `status.done` 時為 1 |

## 3.2 MMIO

| 名稱 | 方向 | 位寬 | 說明 |
|---|---|---:|---|
| `mmio_addr` | in | 32 | MMIO 位址 |
| `mmio_write` | in | 1 | 1=寫入 |
| `mmio_wdata` | in | 32 | 寫入資料 |
| `mmio_rdata` | out | 32 | 讀回資料 |

## 3.3 SPM 介面（4 ports）

訊號群：

- `spm_addr[i]`
- `spm_req[i]`
- `spm_we[i]`
- `spm_wdata[i]`
- `spm_rdata[i]`
- `spm_ready[i]`

| Port | Plane | 方向 | 用途 |
|---:|---|---|---|
| 0 | PS | R | stage0 讀取 PS payload |
| 1 | PD | R | stage0 讀取 PD payload |
| 2 | PLI | R | stage0 讀取 PLI payload |
| 3 | PLO | W | stage1 寫回 PLO response |

## 3.4 NoC 型別化 valid-ready 介面

HDDU 直接暴露 VR 模組，避免上層手工拆接 bit-level 訊號。

```cpp
VRDOF<request_t<sc_biguint<DATA_BITS>, uint16_t>> noc_ps_out;
VRDOF<request_t<sc_biguint<DATA_BITS>, uint16_t>> noc_pd_out;
VRDOF<request_t<sc_biguint<DATA_BITS>, uint16_t>> noc_pli_out;
VRDOF<noc_addr_req_t> noc_plo_out;
VRDIF<response_t<sc_biguint<DATA_BITS>>> noc_plo_in;
```

| 介面 | 方向 | payload |
|---|---|---|
| `noc_ps_out` | HDDU -> NoC | `request_t<data, addr>` |
| `noc_pd_out` | HDDU -> NoC | `request_t<data, addr>` |
| `noc_pli_out` | HDDU -> NoC | `request_t<data, addr>` |
| `noc_plo_out` | HDDU -> NoC | `noc_addr_req_t` |
| `noc_plo_in` | NoC -> HDDU | `response_t<data>` |

---

## 4. AGU Bank Address Map

| 區間 | 對應 AGU |
|---|---|
| `0x000 ~ 0x0FF` | AGU0 (PS) |
| `0x100 ~ 0x1FF` | AGU1 (PD) |
| `0x200 ~ 0x2FF` | AGU2 (PLI) |
| `0x300 ~ 0x3FF` | AGU3 (PLO) |

每個 bank 內部欄位沿用 `AddressGenerateUnit` 規格。

## 4.1 AGU Register 完整對照（bank 內 offset）

下表為 `AddressGenerateUnit::RegOffset` 實際可用欄位：

| Offset | Name | RW | 欄位/用途 |
|---:|---|---|---|
| `0x00` | `REG_BASE_ADDR` | R/W | base address low 32-bit |
| `0x04` | `REG_BASE_ADDR_H` | R/W | base address high（目前保留，建議寫 0） |
| `0x08` | `REG_ITER01` | R/W | `iter0[15:0]`, `iter1[31:16]` |
| `0x0C` | `REG_ITER23` | R/W | `iter2[15:0]`, `iter3[31:16]` |
| `0x10` | `REG_STRIDE0` | R/W | stride0 |
| `0x14` | `REG_STRIDE1` | R/W | stride1 |
| `0x18` | `REG_STRIDE2` | R/W | stride2 |
| `0x1C` | `REG_STRIDE3` | R/W | stride3 |
| `0x20` | `REG_CTRL` | R/W | bit0=start, bit1=stop, bit2=soft-reset, bit3=ultra |
| `0x24` | `REG_STATUS` | R | bit0=busy, bit1=done, bit2=error, bit3=stalled |
| `0x28` | `REG_LANE_CFG` | R/W | addr/tag lane config（目前可先用預設） |
| `0x40` | `REG_TAG_BASE` | R/W | tag base（實際取低位） |
| `0x44` | `REG_TAG_STRIDE0` | R/W | tag stride for inner level |
| `0x48` | `REG_TAG_STRIDE1` | R/W | tag stride for outer levels |
| `0x4C` | `REG_TAG_CTRL` | R/W | tag level/source control |
| `0x54` | `REG_MASK_CFG` | R/W | descriptor mask（bit0~15） |
| `0x58` | `REG_ERR_CODE` | R/W | AGU 本地錯誤碼 |
| `0x5C` | `REG_DBG_TAG` | R | 最後輸出 tag |
| `0x60` | `REG_DBG_ADDR` | R | 最後輸出 addr |

## 4.2 位址換算（Cluster 視角）

若透過 `ComputeCluster` 存取 HDDU：

- `HDDU_BASE = 0x1000`
- `bank_base = HDDU_BASE + bank * 0x100`
- `reg_addr = bank_base + reg_offset`

範例：

- 設定 `AGU_PD` 的 `REG_BASE_ADDR`：`0x1000 + 0x100 + 0x00 = 0x1100`
- 啟動 `AGU_PLO`：寫 `REG_CTRL` 於 `0x1000 + 0x300 + 0x20 = 0x1320`

## 4.3 建議最小設定序列（每個 AGU）

至少建議寫入：

1. `REG_BASE_ADDR`
2. `REG_ITER01`、`REG_ITER23`
3. `REG_STRIDE0`（必要時再加 `STRIDE1/2/3`）
4. `REG_TAG_BASE`、`REG_TAG_STRIDE0`
5. `REG_MASK_CFG`
6. `REG_CTRL`（`bit0=1` 啟動，必要時 `bit3=ultra`）

---

## 5. Global MMIO Register Map（`0x800 ~ 0x8FF`）

| Offset | Name | RW | 說明 |
|---:|---|---|---|
| `0x800` | `HDDU_CTRL` | R/W | bit1: clear error/fifo；bit2: start_all；bit3: stop_all |
| `0x804` | `HDDU_STATUS` | R | bit0: any_busy；bit1: !any_busy；bit2: err；bit3: 本 cycle stall |
| `0x808` | `PLANE_EN` | R/W | bit0~3: PS/PD/PLI/PLO enable |
| `0x80C` | `PLANE_MODE` | R/W | 軟體定義模式旗標（硬體不直接解碼） |
| `0x810` | `NUM_PORTS` | R | 固定 4（相容欄位） |
| `0x814` | `PORT_WIDTH` | R | `DATA_BITS / 4`（相容欄位） |
| `0x818` | `MAX_OUTSTANDING` | R/W | 每通道 FIFO 限制（0 視為 1） |
| `0x81C` | `ARB_POLICY` | R/W | 保留欄位 |
| `0x820` | `ERR_CODE` | R/W | 錯誤碼（目前主要供軟體維護） |
| `0x824` | `ERR_INFO0` | R/W | 錯誤資訊0 |
| `0x828` | `ERR_INFO1` | R/W | 錯誤資訊1 |
| `0x82C` | `COUNTER_TX_PKT` | R | PS/PD/PLI 成功握手送出封包數 |
| `0x830` | `COUNTER_TX_BYTE` | R | 送出位元組累計（每包 +`DATA_BYTES`） |
| `0x834` | `COUNTER_RX_BYTE` | R | PLO 寫回位元組累計（每筆 +`DATA_BYTES`） |
| `0x838` | `COUNTER_STALL` | R | 各通道背壓/資源不足 stall 累計 |

---

## 6. 微架構與時序語意

## 6.1 PS/PD/PLI：兩級獨立管線（平行）

每個 send plane 各自維護兩個 FIFO：

- `tx_tag_fifo[lane]`
- `tx_data_fifo[lane]`

### Stage 0（AGU -> SPM read -> FIFO push）

觸發條件（lane = 0/1/2）：

1. `PLANE_EN[lane] = 1`
2. `agu_gen_valid[lane] = 1`
3. `spm_ready[lane] = 1`
4. `tx_tag_fifo` 與 `tx_data_fifo` 未達 `MAX_OUTSTANDING`

成功後行為：

- 發出 `spm_req[lane]=1, spm_we[lane]=0`
- `spm_addr[lane] = agu_gen_addr`
- 對 AGU 回覆 `agu_gen_ready[lane]=1`
- push `encode(tag, ultra)` 到 `tx_tag_fifo`
- push `spm_rdata[lane]` 到 `tx_data_fifo`

### Stage 1（FIFO pop -> NoC request）

當兩個 FIFO 皆非空，輸出：

- `noc_*_out.valid_out = 1`
- `noc_*_out.data_out = {data=fifo_front_data, addr=fifo_front_tag, mask=all_ones}`

若 `ready_in=1`：

- pop 兩個 FIFO
- `COUNTER_TX_PKT += 1`
- `COUNTER_TX_BYTE += DATA_BYTES`

## 6.2 PLO：兩級獨立管線（平行）

PLO 使用一個地址 FIFO：`plo_addr_fifo`。

### Stage 0（AGU -> NoC PLO request -> addr fifo push）

觸發條件：

1. `PLANE_EN[PLO] = 1`
2. `agu_gen_valid[PLO] = 1`

輸出：

- `noc_plo_out.valid_out = 1`
- `noc_plo_out.data_out.addr = encode(tag, ultra)`

當 `noc_plo_out.ready_in=1` 且 `plo_addr_fifo` 未滿：

- `agu_gen_ready[PLO] = 1`
- push `agu_gen_addr` 到 `plo_addr_fifo`

### Stage 1（NoC PLO response -> SPM write）

當 `noc_plo_in.valid_in=1` 時：

- 若 `plo_addr_fifo` 非空且 `spm_ready[3]=1`，則
  - `noc_plo_in.ready_out=1`
  - pop `addr`
  - 發出 `spm_req[3]=1, spm_we[3]=1`
  - `spm_addr[3]=addr`
  - `spm_wdata[3]=noc_plo_in.data_in.data`
  - `COUNTER_RX_BYTE += DATA_BYTES`
- 否則視為 stall（`COUNTER_STALL` 累加）

## 6.3 Stall 定義

以下任一情況會計入 stall：

- send stage0：SPM not ready 或 FIFO 已滿
- PLO stage0：`noc_plo_out.ready_in=0` 或地址 FIFO 已滿
- PLO stage1：有 response 但無地址可配對，或 SPM3 not ready

`HDDU_STATUS.bit3` 反映「本 cycle 是否出現 stall」。

## 6.4 中斷語意

`interrupt = HDDU_STATUS.bit2 || HDDU_STATUS.bit1`

- `bit2=1`：error 狀態
- `bit1=1`：所有 AGU 非 busy（視為 done 條件）

---

## 7. 驅動流程（建議）

1. 設定 AGU bank（PS/PD/PLI/PLO）
2. 設定 `PLANE_EN`、`MAX_OUTSTANDING`
3. 視情況設定 `PLANE_MODE`
4. 以 `HDDU_CTRL.bit2=1` 啟動全部 AGU
5. 輪詢 `HDDU_STATUS` 與 counters，或等 `interrupt`
6. 完成後讀取統計與結果記憶體

---

## 8. 驅動範例

## 8.1 基本 helper（C-like pseudo code）

```cpp
void hddu_wr(uint32_t base, uint32_t off, uint32_t val) {
    mmio_write(base + off, val);
}

uint32_t hddu_rd(uint32_t base, uint32_t off) {
    return mmio_read(base + off);
}

void agu_wr(uint32_t hddu_base, int bank, uint32_t reg_off, uint32_t val) {
    mmio_write(hddu_base + bank * 0x100 + reg_off, val);
}
```

## 8.2 Conv2D-like 資料流（PS/PD/PLI send + PLO recv）

```cpp
const uint32_t H = 0x1000; // ComputeCluster 中的 HDDU base

// bank0: PS
agu_wr(H, 0, 0x00, ps_base);
agu_wr(H, 0, 0x08, (iter1 << 16) | iter0);
agu_wr(H, 0, 0x10, iter2);
agu_wr(H, 0, 0x40, ps_tag);
agu_wr(H, 0, 0x44, 1);
agu_wr(H, 0, 0x54, 0xF);
agu_wr(H, 0, 0x20, 0x1); // start

// bank1: PD
agu_wr(H, 1, 0x00, pd_base);
agu_wr(H, 1, 0x08, pd_iter01);
agu_wr(H, 1, 0x10, pd_iter2);
agu_wr(H, 1, 0x40, pd_tag);
agu_wr(H, 1, 0x44, 1);
agu_wr(H, 1, 0x54, 0xF);
agu_wr(H, 1, 0x20, 0x1);

// bank2: PLI
agu_wr(H, 2, 0x00, pli_base);
agu_wr(H, 2, 0x08, pli_iter01);
agu_wr(H, 2, 0x10, pli_iter2);
agu_wr(H, 2, 0x40, pli_tag);
agu_wr(H, 2, 0x44, 1);
agu_wr(H, 2, 0x54, 0xF);
agu_wr(H, 2, 0x20, 0x1);

// bank3: PLO write-back address producer
agu_wr(H, 3, 0x00, plo_base);
agu_wr(H, 3, 0x08, plo_iter01);
agu_wr(H, 3, 0x10, plo_iter2);
agu_wr(H, 3, 0x40, plo_req_tag);
agu_wr(H, 3, 0x44, 1);
agu_wr(H, 3, 0x54, 0xF);
agu_wr(H, 3, 0x20, 0x1);

// global config
hddu_wr(H, 0x808, 0xF);   // enable all planes
hddu_wr(H, 0x80C, 0x1);   // mode flag (conv2d)
hddu_wr(H, 0x818, 16);    // max outstanding

// optional: start all AGU from global ctrl
hddu_wr(H, 0x800, (1u << 2));

while (true) {
    uint32_t st = hddu_rd(H, 0x804);
    if (st & (1u << 2)) { /* err */ break; }
    if (st & (1u << 1)) { /* done */ break; }
}

uint32_t tx_pkt   = hddu_rd(H, 0x82C);
uint32_t tx_bytes = hddu_rd(H, 0x830);
uint32_t rx_bytes = hddu_rd(H, 0x834);
uint32_t stall    = hddu_rd(H, 0x838);
```

## 8.3 Stop/clear 範例

```cpp
// stop all AGUs
hddu_wr(H, 0x800, (1u << 3));

// clear fifo/error info
hddu_wr(H, 0x800, (1u << 1));
```

---

## 9. 驗證重點清單

1. bank MMIO passthrough 正確（0x000/0x100/0x200/0x300）
2. `tag + ultra` 編碼符合預期
3. send channel 在 `ready=0` 能 hold，不遺失封包
4. PLO response 與地址 FIFO 對齊寫回 SPM3
5. `MAX_OUTSTANDING` 對 FIFO 深度限制生效
6. `COUNTER_TX_PKT / TX_BYTE / RX_BYTE / STALL` 與 traffic 相符
7. 多通道同時運作時互不阻塞（除共享資源 backpressure 外）
