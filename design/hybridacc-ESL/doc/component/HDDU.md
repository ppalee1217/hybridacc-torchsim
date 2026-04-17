# HybridDataDeliverUnit (HDDU) 規格書 v4

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
- `FIFO_DEPTH = 16`（所有內部 FIFO 固定深度）

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
| `0x800` | `HDDU_CTRL` | R/W | bit0: soft-reset（清 FIFO/錯誤）；bit1: start_all；bit2: stop_all |
| `0x804` | `HDDU_STATUS` | R | bit1: any_busy；bit2: done（!any_busy）；bit3: 本 cycle stall；bit4: err |
| `0x808` | `PLANE_EN` | R/W | bit0~3: PS/PD/PLI/PLO enable（預設全開 0xF） |
| `0x80C` | `PLANE_MODE` | R/W | 軟體定義模式旗標（硬體不直接解碼） |
| `0x810` | `NUM_PORTS` | R | 固定 4（相容欄位） |
| `0x814` | `PORT_WIDTH` | R | `DATA_BITS / 4`（相容欄位） |
| `0x818` | `ARB_POLICY` | R/W | 仲裁策略保留欄位 |
| `0x81C` | `ERR_CODE` | R/W | 錯誤碼（`HdduErrorCode`：0=NONE, 1=AGU, 2=NOC, 3=SPM） |
| `0x820` | `ERR_INFO0` | R/W | 錯誤資訊0（受影響通道 bitmask） |
| `0x824` | `ERR_INFO1` | R/W | 錯誤資訊1（錯誤位址） |
| `0x828` | `COUNTER_TX_PKT` | R | PS/PD/PLI/PLO 出口握手封包累計數 |
| `0x82C` | `COUNTER_TX_BYTE` | R | PS/PD/PLI 送出位元組累計（每包 +`DATA_BYTES`） |
| `0x830` | `COUNTER_RX_BYTE` | R | PLO 寫回位元組累計（每筆 +`DATA_BYTES`） |
| `0x834` | `COUNTER_STALL` | R | 各 AGU 有效但未 ready 之 stall cycle 累計 |

> **注意**：`MAX_OUTSTANDING` 在程式碼中不存在，FIFO 深度固定為 `FIFO_DEPTH = 16`。

---

## 6. 微架構與詳細運作流程

HDDU 採用四通道獨立平行運作的設計，每個平面 (Plane) 都有獨立的資料路徑與緩衝區。

### 6.1 全域控制與 MMIO 路由

- **設定寫入**：MMIO 寫入位址會被解碼。若位址落在 `0x000~0x3FF`，則寫入特定 AGU 的設定暫存器；若落在 `0x800~0x8FF`，則更新 HDDU 全域控制暫存器 (如 `global_ctrl`, `plane_en`)。
- **狀態更新**：每個時脈週期統計所有 AGU 的 `busy` 狀態，更新 `global_status`。
- **平面啟用**：根據 `plane_en` register 決定是否驅動對應的 AGU 與資料路徑。

### 6.2 發送平面 (PS, PD, PLI) 運作流程

PS、PD、PLI 三個平面負責將 SPM 中的資料讀出並發送至 NoC。每個平面維護以下兩個 FIFO：
- `read_noc_addr_wait_fifo[i]`：存放已發出 SPM 讀取請求、等待回應的 **編碼 NoC 位址**（`encode_noc_addr(tag, ultra)` = `{ultra, tag[NOC_TAG_BITS-1:0]}`）。
- `noc_req_fifo[i]`：存放已組裝好的完整 NoC 請求封包（`{data, addr}`），等待送出。

所有控制信號為純組合邏輯，無 Cooldown 機制。

#### Stage 0: 請求生成與 SPM 讀取
**觸發條件**（`comb_spm_read_req`）：
1. 對應平面被啟用 (`PLANE_EN[i]=1`)。
2. AGU 產生有效位址 (`agu_gen_valid=1`)。
3. `read_noc_addr_wait_fifo[i]` 未滿。

**動作**：
- 對 SPM 發出讀取請求（`spm_req_valid=1`, `wen=0`, `addr=agu_gen_addr`）。
- 若 SPM 接受（`spm_req_ready=1`）且以上條件成立（`comb_spm_read_req_push_wait_tag_fifo_agu_ready`）：
  - 回應 AGU `gen_ready=1`。
  - 將編碼後之 `encode_noc_addr(tag, ultra)` 推入 `read_noc_addr_wait_fifo[i]`。
- AGU 有效但 `gen_ready=0` 時，`COUNTER_STALL` 累加。

#### Stage 0.5: SPM 讀取回應處理
**觸發條件**（`comb_spm_read_resp_ready`）：
1. `read_noc_addr_wait_fifo[i]` 非空（表示有 in-flight 請求）。
2. `noc_req_fifo[i]` 未滿。

**動作**：
- 向 SPM assert `spm_resp_ready=1`。
- 若收到 SPM 回應（`spm_resp_valid=1`）（`comb_spm_read_resp_pop_wait_tag_fifo_and_push_noc_req`）：
  - 從 `read_noc_addr_wait_fifo[i]` pop 出對應的 NoC 位址。
  - 將 `{spm_resp.rdata, noc_addr}` 組裝成 `noc_req_payload_t` 推入 `noc_req_fifo[i]`。

#### Stage 1: NoC 封包發送
**觸發條件**（`comb_noc_req_valid`）：
1. `noc_req_fifo[i]` 非空。

**動作**：
- 將 FIFO 頂端資料送往對應 NoC 介面（`noc_ps/pd/pli_out.data_out`），assert `valid_out=1`。
- 若 NoC 接受（`ready_in=1`）（`comb_noc_req_pop`）：
  - Pop `noc_req_fifo[i]`。
  - 累加 `COUNTER_TX_PKT`（+1）與 `COUNTER_TX_BYTE`（+`DATA_BYTES`）。

### 6.3 接收平面 (PLO) 運作流程

PLO 負責向 NoC 發送地址請求索取資料，並將回傳的資料寫入 SPM。
PLO 維護以下兩個 FIFO（均深度 `FIFO_DEPTH=16`）：
- `write_addr_fifo`：存放已發出 NoC 請求、等待 NoC 回傳資料的 **SPM 寫入位址**（`agu_gen_addr`）。
- `spm_req_fifo`：暫存已組裝好的 SPM 寫入請求（`{addr, wdata, wen=true}`），在 NoC 回應到達並配對後推入。

無獨立 mode FIFO（ultra bit 已在 NoC 位址封包中編碼）。
無 pending 計數器，改以 `spm_req_fifo` 緩衝未完成的寫入。

#### Stage 0: NoC 請求發送 (Request Address)
**觸發條件**（`comb_noc_plo_req_valid`）：
1. `PLANE_EN[PLO]=1`。
2. AGU 產生有效位址 (`agu_gen_valid=1`)。
3. `write_addr_fifo` 未滿。

**動作**：
- 組合 NoC 請求封包（僅含位址/Tag）：`req.addr = encode_noc_addr(tag, ultra)`。
- 對 NoC 介面 (`noc_plo_out`) assert `valid_out=1`（繞過 FIFO，降低延遲）。
- 若 NoC 接受（`ready_in=1`）（`comb_noc_plo_req_push_wait_addr_fifo_agu_ready`）：
  - 回應 AGU `gen_ready=1`。
  - 將 `agu_gen_addr` 推入 `write_addr_fifo`。
- AGU 有效但 `gen_ready=0` 時，`COUNTER_STALL` 累加。
- **注意**：PLO 的 NoC 請求握手也會遞增 `COUNTER_TX_PKT`（+1），但不累加 `COUNTER_TX_BYTE`。

#### Stage 1: NoC 資料接收與 SPM 寫入
**觸發條件**（`comb_noc_plo_resp_ready`）：
1. `write_addr_fifo` 非空（表示有對應的等待位址）。
2. `spm_req_fifo` 未滿。

**動作**：
- 向 NoC assert `noc_plo_in.ready_out=1`。
- 若收到 NoC 回應（`valid_in=1`）（`comb_noc_plo_resp_pop_wait_addr_fifo_and_push_spm_req`）：
  - 從 `write_addr_fifo` pop 出 SPM 位址。
  - 將 `{addr=pop_addr, wdata=noc_plo_in.data, wen=1}` 推入 `spm_req_fifo`。
  - 累加 `COUNTER_RX_BYTE`（+`DATA_BYTES`）。

#### Stage 2: SPM 寫入
**觸發條件**（`comb_spm_write_req_valid`）：
1. `spm_req_fifo` 非空。

**動作**（`comb_spm_write_req_pop`）：
- 對 SPM port 3 assert 寫入請求 (`spm_req_valid=1`, `wen=1`)。
- 若 SPM 接受（`spm_req_ready=1`）：
  - Pop `spm_req_fifo`。

#### Stage 2.5: SPM 寫入確認
在 `seq_process` 中監聽 `spm_resp_valid[PLO]`：
- 若 SPM 回應碼非 `SPM_OK`：設定 `ERR_CODE=SPM_ERROR`，並記錄 `ERR_INFO0`（bitmask）與 `ERR_INFO1`（錯誤位址）。

### 6.4 Stall 與效能計數

`COUNTER_STALL` 在 `seq_process` 中統計，條件為 **每個 AGU 有 `gen_valid=1` 但 `gen_ready=0`**（涵蓋所有 4 個平面），每 cycle 將所有 stall 次數相加後寫入暫存器。

各平面造成 stall 的原因：
- **PS/PD/PLI**：`read_noc_addr_wait_fifo` 已滿，或 SPM port 未 ready（使 `gen_ready=0`）。
- **PLO**：`write_addr_fifo` 已滿，或 NoC `noc_plo_out.ready_in=0`（使 `gen_ready=0`）。

**`COUNTER_TX_PKT`** 計入：PS/PD/PLI 及 PLO 的 NoC 出口握手次數。

**`COUNTER_TX_BYTE`** 僅計入：PS/PD/PLI 成功送出封包的位元組數（每包 +`DATA_BYTES`）。

**`COUNTER_RX_BYTE`** 計入：PLO pipeline 接收 NoC response 並送往 SPM 的位元組數（每筆 +`DATA_BYTES`）。

### 6.5 中斷語意

`interrupt = HDDU_STATUS.bit4 || HDDU_STATUS.bit2`

- `bit4=1`：error 狀態（`err_code_reg != 0`）
- `bit2=1`：所有 AGU 非 busy（`!any_busy`，視為 done 條件）

完整 `HDDU_STATUS` bit 定義：

| Bit | 名稱 | 說明 |
|---:|---|---|
| 1 | BUSY | 至少一個 AGU busy |
| 2 | DONE | 所有 AGU 非 busy（`!any_busy`） |
| 3 | STALL | 本 cycle 發生 stall |
| 4 | ERROR | `ERR_CODE != 0` |

---

## 7. 驅動流程（建議）

1. 設定 AGU bank（PS/PD/PLI/PLO）
2. 設定 `PLANE_EN`（視情況設定 `PLANE_MODE`）
3. 以 `HDDU_CTRL.bit1=1`（`CTRL_START`）啟動全部 AGU
4. 輪詢 `HDDU_STATUS` 與 counters，或等 `interrupt`
   - `bit2=1`：done；`bit4=1`：error
5. 完成後讀取統計結果（`0x828~0x834`）

> **注意**：程式碼中無 `MAX_OUTSTANDING` 軟體可設定欄位；FIFO 深度固定為 `FIFO_DEPTH=16`。

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
hddu_wr(H, 0x808, 0xF);   // enable all planes (default is already 0xF)
hddu_wr(H, 0x80C, 0x1);   // mode flag (conv2d, software-defined)

// start all AGUs from global CTRL (bit1 = CTRL_START)
hddu_wr(H, 0x800, (1u << 1));

while (true) {
    uint32_t st = hddu_rd(H, 0x804);
    if (st & (1u << 4)) { /* bit4 = err */ break; }
    if (st & (1u << 2)) { /* bit2 = done */ break; }
}

uint32_t tx_pkt   = hddu_rd(H, 0x828); // COUNTER_TX_PKT
uint32_t tx_bytes = hddu_rd(H, 0x82C); // COUNTER_TX_BYTE
uint32_t rx_bytes = hddu_rd(H, 0x830); // COUNTER_RX_BYTE
uint32_t stall    = hddu_rd(H, 0x834); // COUNTER_STALL
```

## 8.3 Stop/clear 範例

```cpp
// stop all AGUs (bit2 = CTRL_STOP)
hddu_wr(H, 0x800, (1u << 2));

// soft-reset: clear FIFO & error info (bit0 = CTRL_RESET)
hddu_wr(H, 0x800, (1u << 0));
```

---

## 9. 驗證重點清單

1. bank MMIO passthrough 正確（0x000/0x100/0x200/0x300）
2. `tag + ultra` 編碼符合預期（`encode_noc_addr`：`{ultra, tag[NOC_TAG_BITS-1:0]}`）
3. send channel 在 `ready=0` 能 hold，不遺失封包
4. PLO response 與 `write_addr_fifo` 對齊寫回 SPM port 3，SPM 寫入錯誤能觸發 `ERR_CODE`
5. FIFO 深度 `FIFO_DEPTH=16` 造成 backpressure 時 stall 計數正確累加
6. `COUNTER_TX_PKT`（含 PLO NoC req）/ `TX_BYTE` / `RX_BYTE` / `STALL` 與 traffic 相符
7. 多通道同時運作時互不阻塞（除共享 SPM/NoC backpressure 外）
8. `HDDU_CTRL` bit0/1/2 分別觸發 reset/start/stop；`HDDU_STATUS` bit2/4 觸發中斷
