# HybridDataDeliverUnit (HDDU) 規格書 v2

## 1. 概述

HDDU 負責在 MMIO 控制下協調 4 組 AGU、4 個 SPM 連接埠與 4 個 NoC 連接埠。
本版本移除 DMA 介面，資料路徑改為：

- `SPM[0/1/2]` 讀出資料 -> `NoC[0/1/2]` 送出
- `NoC[3]` 接收資料 -> `SPM[3]` 寫入

---

## 2. Template 參數

```cpp
template <int SPM_ADDR_BITS = 32, int NOC_TAG_BITS = 6, int DATA_BITS = 256>
class HybridDataDeliverUnit;
```

- `SPM_ADDR_BITS`: SPM 位址寬度
- `NOC_TAG_BITS`: NoC tag 寬度（實際 NoC 地址寬 = `NOC_TAG_BITS + 1`）
- `DATA_BITS`: SPM 與 NoC 資料寬度

NoC 地址編碼規則：

- `addr[NOC_TAG_BITS-1:0] = tag`
- `addr[NOC_TAG_BITS] = ultra`

---

## 3. Top-level Port 定義

## 3.1 時序/控制

| 名稱 | 方向 | 寬度 | 說明 |
|---|---|---:|---|
| `clk` | in | 1 | 系統時脈 |
| `reset_n` | in | 1 | 低有效重置 |
| `interrupt` | out | 1 | 中斷輸出（done 或 err） |

## 3.2 MMIO 介面

| 名稱 | 方向 | 寬度 | 說明 |
|---|---|---:|---|
| `mmio_addr` | in | 32 | MMIO 位址 |
| `mmio_write` | in | 1 | 寫使能 |
| `mmio_wdata` | in | 32 | 寫資料 |
| `mmio_rdata` | out | 32 | 讀資料 |

## 3.3 SPM 介面（4 ports）

> `spm_addr[i] / spm_req[i] / spm_we[i] / spm_wdata[i] / spm_rdata[i] / spm_ready[i]`

| port | 用途 | 行為 |
|---:|---|---|
| 0 | PS | read |
| 1 | PD | read |
| 2 | PLI | read |
| 3 | PLO | write |

## 3.4 NoC 介面（4 ports）

### 送出（port 0/1/2）

> `noc_out_data[i] / noc_out_addr[i] / noc_out_valid[i] / noc_out_ready[i]`

| port | plane | 方向 |
|---:|---|---|
| 0 | PS | HDDU -> NoC |
| 1 | PD | HDDU -> NoC |
| 2 | PLI | HDDU -> NoC |

### 接收（port 3）

> `noc_in3_data / noc_in3_addr / noc_in3_valid / noc_in3_ready`

| port | plane | 方向 |
|---:|---|---|
| 3 | PLO | NoC -> HDDU |

---

## 4. AGU 對映

HDDU 內建 4 組 AGU，bank 對映如下：

- `0x000~0x0FF`: AGU_PS
- `0x100~0x1FF`: AGU_PD
- `0x200~0x2FF`: AGU_PLI
- `0x300~0x3FF`: AGU_PLO

每個 bank 內部欄位沿用 AGU 規格（`AddressGenerateUnit`）。

---

## 5. MMIO Register Map

## 5.1 全域區 `0x800~0x8FF`

| Offset | 名稱 | 欄位 | 說明 |
|---:|---|---|---|
| `0x800` | `HDDU_CTRL` | `bit0 en`, `bit1 soft_reset`, `bit2 start_all`, `bit3 stop_all` | 全域控制 |
| `0x804` | `HDDU_STATUS` | `bit0 busy`, `bit1 done`, `bit2 err`, `bit3 stall` | 全域狀態 |
| `0x808` | `PLANE_EN` | `bit0 PS`, `bit1 PD`, `bit2 PLI`, `bit3 PLO` | 平面使能 |
| `0x80C` | `PLANE_MODE` | `bit0 conv2d`, `bit1 gemm`, `bit2 ultra`, `bit3 k_split` | 模式 |
| `0x810` | `NUM_PORTS` | `[7:0]` | 固定 4 |
| `0x814` | `PORT_WIDTH` | `[15:0]` | `DATA_BITS / 4` |
| `0x818` | `MAX_OUTSTANDING` | `[7:0]` | 保留 |
| `0x81C` | `ARB_POLICY` | `[1:0] rr/fixed` | 仲裁策略 |
| `0x820` | `ERR_CODE` | `[31:0]` | 第一個錯誤 |
| `0x824` | `ERR_INFO0` | `[31:0]` | 錯誤資訊 0 |
| `0x828` | `ERR_INFO1` | `[31:0]` | 錯誤資訊 1 |
| `0x82C` | `COUNTER_TX_PKT` | `[31:0]` | 累積送出封包 |
| `0x830` | `COUNTER_TX_BYTE` | `[31:0]` | 累積送出 bytes |
| `0x834` | `COUNTER_RX_BYTE` | `[31:0]` | 累積接收 bytes |
| `0x838` | `COUNTER_STALL` | `[31:0]` | 背壓 stall cycle |

---

## 6. 行為規範

## 6.1 仲裁

- 預設 round-robin 在 4 個 AGU 間選擇來源。
- `PLANE_EN` 關閉的 plane 不參與仲裁。

## 6.2 PS/PD/PLI（AGU0/1/2）

- AGU `gen_valid=1` 且 `spm_ready[plane]=1` 時發出 `SPM read`。
- 同 cycle 擷取 `spm_rdata[plane]`，封裝成對應 `noc_out[plane]`。
- `noc_out_ready=0` 時資料需維持（valid hold）。

## 6.3 PLO（AGU3）

- AGU3 `gen_valid=1`、`noc_in3_valid=1`、`spm_ready[3]=1` 時：
  - 發出 `spm_req[3]=1`、`spm_we[3]=1`
  - `spm_wdata[3] = noc_in3_data`
  - `noc_in3_ready=1`

## 6.4 中斷

- `interrupt = (HDDU_STATUS.err == 1) || (HDDU_STATUS.done == 1)`

---

## 7. 驗證重點

`test_hddu_unit.cpp` 應覆蓋：

1. MMIO 讀寫與全域寄存器預設值
2. AGU bank passthrough
3. SPM read -> NoC send（含 tag/ultra 編碼）
4. NoC backpressure hold/clear
5. NoC3 recv -> SPM3 write
6. 多平面混合 + 多 cycle 背壓壓力測試
