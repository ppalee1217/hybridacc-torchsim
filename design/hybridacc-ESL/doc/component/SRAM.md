# SRAM 模組設計規格

文件樹： [../../../../doc/index.md](../../../../doc/index.md) -> [../index.md](../index.md) -> [README.md](README.md) -> 本頁。

**檔案**: `design/hybridacc-ESL/simulator/include/Cluster/SRAM.hpp`
**命名空間**: `hybridacc::cluster`
**實作風格**: RTL-style ESL（SystemC SC_MODULE）
**日期**: 2026-03-08

---

## 目錄

1. [模組概覽](#1-模組概覽)
2. [Template 參數](#2-template-參數)
3. [建構子參數](#3-建構子參數)
4. [埠口 (Ports)](#4-埠口-ports)
   - 4.1 [時脈與重置](#41-時脈與重置)
   - 4.2 [讀取請求介面](#42-讀取請求介面)
   - 4.3 [讀取回應介面](#43-讀取回應介面)
   - 4.4 [同步寫入介面](#44-同步寫入介面)
5. [設定參數 (Configuration)](#5-設定參數-configuration)
6. [內部記憶體](#6-內部記憶體)
7. [內部暫存器 (Registers)](#7-內部暫存器-registers)
   - 7.1 [管線 FIFO 暫存器](#71-管線-fifo-暫存器array-大小--pipeline_depth)
   - 7.2 [FIFO 控制暫存器](#72-fifo-控制暫存器)
   - 7.3 [輸出級暫存器](#73-輸出級暫存器)
8. [Process 說明](#8-process-說明)
   - 8.1 [seq_process（SC_CTHREAD）](#81-seq_processsc_cthread)
   - 8.2 [comb_req_ready（SC_METHOD）](#82-comb_req_readysc_method)
   - 8.3 [comb_resp_out（SC_METHOD）](#83-comb_resp_outsc_method)
   - 8.4 [trace_process（SC_METHOD）](#84-trace_processsc_method)
9. [管線時序行為](#9-管線時序行為)
10. [Back-pressure 處理](#10-back-pressure-處理)
11. [同步寫入行為](#11-同步寫入行為)
12. [重置行為](#12-重置行為)
13. [Trace 狀態機](#13-trace-狀態機)
14. [公開工具函式](#14-公開工具函式)
15. [私有輔助函式](#15-私有輔助函式)
16. [設計約束與限制](#16-設計約束與限制)

---

## 1. 模組概覽

`SRAM<DATA_WIDTH_BITS, ADDR_WIDTH>` 是一個可配置的靜態隨機存取記憶體模組，採用 RTL 風格的 ESL（Electronic System Level）實作。

**核心設計原則**：

- 所有架構狀態皆以 `sc_signal<T>` 暫存器（後綴 `_reg`）表示，確保 RTL 語意正確性。
- 一個 `SC_CTHREAD`（`seq_process`）負責計算並提交所有次態（next-state）。
- 兩個 `SC_METHOD`（`comb_req_ready`、`comb_resp_out`）純粹由暫存器值驅動輸出埠，不存在組合邏輯回饋迴路。

**功能摘要**：

| 功能 | 說明 |
|------|------|
| 讀取 | 具可配置延遲（latency）的管線化讀取，支援多筆飛行中請求（in-flight） |
| 寫入 | 同步寫入，附帶 byte-enable 遮罩 |
| 壓力控制 | 讀取請求端及回應端均支援 valid/ready 握手（back-pressure） |

---

## 2. Template 參數

```cpp
template <unsigned DATA_WIDTH_BITS = 64, unsigned ADDR_WIDTH = 32>
SC_MODULE(SRAM)
```

| 參數 | 型別 | 預設值 | 說明 |
|------|------|--------|------|
| `DATA_WIDTH_BITS` | `unsigned` | `64` | 資料匯流排寬度（位元），**必須為 8 的倍數** |
| `ADDR_WIDTH` | `unsigned` | `32` | 位址欄位寬度（位元），**必須 ≤ 32** |

**靜態斷言**：

```cpp
static_assert(DATA_WIDTH_BITS % 8 == 0, "DATA_WIDTH_BITS must be a multiple of 8");
static_assert(ADDR_WIDTH     <= 32,     "ADDR_WIDTH must be <= 32");
```

---

## 3. 建構子參數

```cpp
SRAM(sc_module_name name,
     size_t size_bytes = (1u << 16),  // 預設 64 KiB
     size_t latency    = 1,
     size_t pip_depth  = 3)
```

| 參數 | 型別 | 預設值 | 說明 |
|------|------|--------|------|
| `name` | `sc_module_name` | — | SystemC 模組名稱 |
| `size_bytes` | `size_t` | `65536`（64 KiB） | 記憶體陣列的總位元組容量 |
| `latency` | `size_t` | `1` | 讀取延遲（時脈周期數），必須 ≥ 1 |
| `pip_depth` | `size_t` | `3` | 管線最大飛行中請求數量，範圍 1～255 |

---

## 4. 埠口 (Ports)

### 4.1 時脈與重置

| 埠口名稱 | 方向 | 型別 | 說明 |
|----------|------|------|------|
| `clk` | `sc_in` | `bool` | 系統時脈，所有時序邏輯在正緣觸發 |
| `reset_n` | `sc_in` | `bool` | 低準位有效同步重置（Active-Low Synchronous Reset） |

### 4.2 讀取請求介面

採用 valid/ready 握手協議（AXI-like handshake）。

| 埠口名稱 | 方向 | 型別 | 說明 |
|----------|------|------|------|
| `req_addr` | `sc_in` | `sc_uint<ADDR_WIDTH>` | 讀取請求的位元組位址 |
| `req_valid` | `sc_in` | `bool` | 請求有效旗標，`1` 表示有效請求 |
| `req_ready` | `sc_out` | `bool` | 模組準備好接受請求，`1` 表示可接受 |

**握手條件**：`req_valid && req_ready` 同為 `1` 時，本週期成功傳輸一筆請求。

### 4.3 讀取回應介面

採用 valid/ready 握手協議。

| 埠口名稱 | 方向 | 型別 | 說明 |
|----------|------|------|------|
| `resp_data` | `sc_out` | `sc_biguint<DATA_WIDTH_BITS>` | 讀取回應資料，小端序（Little-Endian byte packing） |
| `resp_valid` | `sc_out` | `bool` | 回應有效旗標，`1` 表示 `resp_data` 有效 |
| `resp_ready` | `sc_in` | `bool` | 下游準備好接收，`1` 表示可接收 |

**握手條件**：`resp_valid && resp_ready` 同為 `1` 時，本週期成功傳輸一筆回應。

### 4.4 同步寫入介面

寫入在 `clk` 正緣且 `write_en` 有效時執行，**不走管線**，直接作用於記憶體陣列。

| 埠口名稱 | 方向 | 型別 | 說明 |
|----------|------|------|------|
| `write_en` | `sc_in` | `bool` | 寫入致能，`1` 表示本週期執行寫入 |
| `write_addr` | `sc_in` | `sc_uint<ADDR_WIDTH>` | 寫入目標的位元組位址 |
| `write_data` | `sc_in` | `sc_biguint<DATA_WIDTH_BITS>` | 待寫入資料 |
| `write_mask` | `sc_in` | `sc_uint<DATA_WIDTH_BITS/8>` | Byte-enable 遮罩，位元 `i` 為 `1` 時才寫入第 `i` 個位元組 |

---

## 5. 設定參數 (Configuration)

這些常數於建構時初始化，執行期間不可修改。

| 成員名稱 | 型別 | 計算方式 | 說明 |
|----------|------|----------|------|
| `data_width_bytes` | `const size_t` | `DATA_WIDTH_BITS / 8` | 資料寬度（位元組） |
| `default_latency` | `const size_t` | = 建構子 `latency` | 讀取管線延遲週期數 |
| `pipeline_depth` | `const size_t` | = 建構子 `pip_depth` | 管線 FIFO 最大深度 |

---

## 6. 內部記憶體

```cpp
std::vector<uint8_t> mem;
```

| 屬性 | 說明 |
|------|------|
| **型別** | `std::vector<uint8_t>`（位元組陣列） |
| **大小** | 建構時由 `size_bytes` 決定，可透過 `resize()` 調整 |
| **初始值** | 全部初始化為 `0` |
| **存取** | 位址以模數（modulo）位元組大小包覆一，避免越界 (`addr % mem.size()`) |
| **位元組序** | Little-Endian：位元組 `i` 儲存在 `[8*i+7 : 8*i]` 位元位置 |

---

## 7. 內部暫存器 (Registers)

所有暫存器皆為 `sc_signal<T>`，唯有 `seq_process` 對其寫入，確保 RTL 語意。

### 7.1 管線 FIFO 暫存器（Array 大小 = `pipeline_depth`）

每個陣列元素代表管線中一個飛行中的讀取請求（slot）。FIFO 以循環緩衝區（circular buffer）實作。

#### `pipe_valid_reg[i]`

```cpp
std::vector< sc_signal<bool> > pipe_valid_reg;
```

| 欄位 | 說明 |
|------|------|
| **型別** | `sc_signal<bool>` |
| **大小** | `pipeline_depth` 個元素 |
| **語意** | Slot `i` 是否被一筆飛行中請求佔用 |
| **重置值** | `false`（全部清除） |
| **寫入時機** | Push：目標 tail slot 設為 `true`；Pop：head slot 設為 `false` |

#### `pipe_addr_reg[i]`

```cpp
std::vector< sc_signal< sc_uint<ADDR_WIDTH> > > pipe_addr_reg;
```

| 欄位 | 說明 |
|------|------|
| **型別** | `sc_signal< sc_uint<ADDR_WIDTH> >` |
| **大小** | `pipeline_depth` 個元素 |
| **語意** | Slot `i` 所對應的讀取位元組位址 |
| **重置值** | `0` |
| **寫入時機** | Push 時，將 `req_addr` 存入 tail slot |

#### `pipe_cycles_reg[i]`

```cpp
std::vector< sc_signal< sc_uint<8> > > pipe_cycles_reg;
```

| 欄位 | 說明 |
|------|------|
| **型別** | `sc_signal< sc_uint<8> >` |
| **大小** | `pipeline_depth` 個元素 |
| **語意** | Slot `i` 剩餘的倒數週期數（countdown），最大值 255 |
| **重置值** | `0` |
| **寫入時機** | Push：設為 `default_latency`；每週期所有有效 slot 遞減 1 |
| **完成條件** | 值為 `1`（本週期遞減後變為 `0`，即完成） |

### 7.2 FIFO 控制暫存器

#### `head_reg`

```cpp
sc_signal< sc_uint<8> > head_reg;
```

| 欄位 | 說明 |
|------|------|
| **型別** | `sc_signal< sc_uint<8> >` |
| **語意** | 循環緩衝區中最舊請求（head/oldest entry）的索引 |
| **範圍** | `[0, pipeline_depth - 1]` |
| **重置值** | `0` |
| **寫入時機** | Pop 時遞增：`(head + 1) % pipeline_depth` |

#### `count_reg`

```cpp
sc_signal< sc_uint<8> > count_reg;
```

| 欄位 | 說明 |
|------|------|
| **型別** | `sc_signal< sc_uint<8> >` |
| **語意** | FIFO 中目前被佔用的 slot 數量（佔用率） |
| **範圍** | `[0, pipeline_depth]` |
| **重置值** | `0` |
| **寫入時機** | Pop 時 -1，Push 時 +1；同週期可同時發生 Push+Pop（淨值不變或 +1/-1） |
| **驅動組合邏輯** | `comb_req_ready` 以此判斷 FIFO 是否已滿 |

### 7.3 輸出級暫存器

輸出級（output holding register）作為管線末端到埠口之間的緩衝，提供 back-pressure 機制。

#### `resp_data_reg`

```cpp
sc_signal< sc_biguint<DATA_WIDTH_BITS> > resp_data_reg;
```

| 欄位 | 說明 |
|------|------|
| **型別** | `sc_signal< sc_biguint<DATA_WIDTH_BITS> >` |
| **語意** | 輸出暫存器，保存最近一次從記憶體讀出的資料 |
| **重置值** | `0` |
| **寫入時機** | Pop 時，執行 `read_word(pipe_addr_reg[head])` 並將結果寫入 |

#### `resp_valid_reg`

```cpp
sc_signal<bool> resp_valid_reg;
```

| 欄位 | 說明 |
|------|------|
| **型別** | `sc_signal<bool>` |
| **語意** | 輸出暫存器有效旗標，`true` 表示 `resp_data_reg` 持有待傳資料 |
| **重置值** | `false` |
| **寫入時機（設為 true）** | Pop 成功時 |
| **寫入時機（設為 false）** | 下游接收（`resp_valid && resp_ready`）且管線頭部尚未完成（無法立即補充新資料）時 |

---

## 8. Process 說明

### 8.1 `seq_process`（SC_CTHREAD）

```cpp
SC_CTHREAD(seq_process, clk.pos());
reset_signal_is(reset_n, false);
```

- **觸發條件**：`clk` 正緣
- **重置信號**：`reset_n`（低準位有效）
- **職責**：所有次態計算，是唯一修改 `_reg` 信號的 process

**執行順序（每個時脈週期）**：

```
1. [可選] 同步寫入記憶體（若 write_en 有效）
2. 對所有有效 slot 的 pipe_cycles_reg 遞減
3. 判斷 Pop 條件（head_done && out_free → do_pop）
4. 若 do_pop：讀取記憶體並更新 resp_data_reg / resp_valid_reg / head_reg / count_reg
5. 若 !do_pop 且 (rv && rr)：清除 resp_valid_reg
6. 計算 effective_count（考慮 pop）
7. 判斷 Push 條件（req_valid && effective_count < pipeline_depth → do_push）
8. 若 do_push：填入 tail slot，更新 pipe_valid_reg / pipe_addr_reg / pipe_cycles_reg / count_reg
9. wait()（等待下一個正緣）
```

**RTL 紀律**：
- 所有 `.read()` 反映前一個時脈邊緣的值。
- 所有 `.write()` 排程於下一個時脈邊緣生效。
- 同一個 between-wait 視窗內，不在 `.write()` 後再 `.read()` 同一信號。

### 8.2 `comb_req_ready`（SC_METHOD）

```cpp
SC_METHOD(comb_req_ready);
sensitive << count_reg;
```

- **觸發條件**：`count_reg` 改變
- **職責**：純組合邏輯，驅動 `req_ready` 埠口

```cpp
req_ready = (count_reg < pipeline_depth)
```

當 FIFO 未滿（佔用數 < 最大深度），允許接受新請求。

### 8.3 `comb_resp_out`（SC_METHOD）

```cpp
SC_METHOD(comb_resp_out);
sensitive << resp_valid_reg << resp_data_reg;
```

- **觸發條件**：`resp_valid_reg` 或 `resp_data_reg` 改變
- **職責**：透明轉發輸出級暫存器到埠口

```cpp
resp_valid = resp_valid_reg
resp_data  = resp_data_reg
```

### 8.4 `trace_process`（SC_METHOD）

```cpp
SC_METHOD(trace_process);
sensitive << clk.pos();
```

- **觸發條件**：`clk` 正緣
- **職責**：產生 Perfetto/Chrome-trace 格式的事件追蹤紀錄
- **啟用條件**：需呼叫 `set_trace_context()` 設定有效的 `trace_id`（≥ 0）

---

## 9. 管線時序行為

以 `default_latency = L` 為例，讀取時序如下：

```
週期    動作
────    ──────────────────────────────────────────────
  0     req_valid & req_ready → Push，pipe_cycles_reg[tail] = L
  1     pipe_cycles_reg[tail] = L-1（開始遞減）
  …
 L-1    pipe_cycles_reg[head] = 1（即將完成）
  L     head_done = true，do_pop，resp_data_reg ← read_word(addr)
        resp_valid_reg = true → resp_valid 輸出有效
```

**結論**：從請求接受到回應有效，延遲恰為 **L 個時脈週期**。

**同週期 Push + Pop**：當 `do_pop` 為 `true` 時，`effective_count = count - 1`，因此在 FIFO 僅剩一個可用空間的臨界情況下，仍可同週期接受新請求。

---

## 10. Back-pressure 處理

### 請求端 Back-pressure

```
req_ready = 0  ←  count_reg == pipeline_depth（FIFO 滿）
```

當 FIFO 已滿，`req_ready` 拉低，傳送方必須保持 `req_valid` 及 `req_addr` 不變直到 `req_ready` 回到 `1`。

### 回應端 Back-pressure

```
條件：resp_valid = 1 且 resp_ready = 0
```

- `resp_data_reg` 和 `resp_valid_reg` **保持不變**（RTL 暫存器自然保持）。
- 管線頭部的 Pop 操作被阻塞：`out_free = !rv || rr`，當 `rv=1, rr=0` 時 `out_free=0`，`do_pop=false`。
- 管線中其他 slot 繼續倒數，但頭部不會被彈出，有效地凍結了整個管線的輸出。

---

## 11. 同步寫入行為

```cpp
if (write_en.read())
    write_word(write_addr, write_data, write_mask);
```

- 寫入優先於讀取管線的 Pop 動作執行（在 `seq_process` 中排列在前）。
- **Write-before-Read 語意**：若同一週期同一位址同時有寫入和讀取管線完成，讀取到的是**舊值**（寫入在下一週期才真正生效於 `mem`，但因為 `read_word` 直接存取 `mem[]`，而 `write_word` 也直接寫入 `mem[]`，實際上在同一 `seq_process` 活化中，寫入發生在 Pop 讀取之前，故讀到**新值**—取決於實作細節，請注意）。

> **注意**：`write_word` 直接操作 `std::vector<uint8_t> mem`（非 `sc_signal`），因此寫入立即生效，管線 Pop 在同週期讀取同位址時將讀到已寫入的新值。

### Byte-enable 遮罩

```
write_mask 位元 i = 1 → 寫入第 i 個位元組（位址 base + i）
write_mask 位元 i = 0 → 保留原值
```

遮罩寬度為 `DATA_WIDTH_BITS / 8` 位元，對應每個資料字組的每個位元組。

---

## 12. 重置行為

重置信號：`reset_n`（低準位有效），由 SystemC 的 `reset_signal_is(reset_n, false)` 宣告。

重置期間（`reset_n = 0`），`seq_process` 執行以下初始化：

| 暫存器 | 重置值 |
|--------|--------|
| `pipe_valid_reg[i]`（所有 i） | `false` |
| `pipe_addr_reg[i]`（所有 i） | `0` |
| `pipe_cycles_reg[i]`（所有 i） | `0` |
| `head_reg` | `0` |
| `count_reg` | `0` |
| `resp_data_reg` | `0` |
| `resp_valid_reg` | `false` |

> **注意**：重置不清除 `mem` 記憶體陣列，若需清除請呼叫 `mem_reset()`。

---

## 13. Trace 狀態機

`trace_process` 以 valid/ready 信號組合定義以下互斥狀態，優先級由高至低：

| 狀態名稱 | 條件 | 說明 |
|----------|------|------|
| `RESET` | `!reset_n` | 模組處於重置中 |
| `RESP_BACKPRESSURE` | `resp_valid && !resp_ready` | 回應端被下游阻塞 |
| `REQ_BACKPRESSURE` | `req_valid && !req_ready` | 請求端 FIFO 已滿 |
| `REQ_RESP_XFER` | `req_valid && req_ready && resp_valid && resp_ready` | 同週期請求與回應均傳輸 |
| `REQ_XFER` | `req_valid && req_ready` | 僅請求傳輸 |
| `RESP_XFER` | `resp_valid && resp_ready` | 僅回應傳輸 |
| `BUSY` | `count_reg > 0 \|\| resp_valid_reg` | 管線中有飛行中請求或輸出級有資料 |
| `IDLE` | 其餘情況 | 閒置 |

狀態發生轉換時，產生 `TRACE_END`（前一狀態）和 `TRACE_BEGIN`（新狀態）事件，附帶 JSON payload 包含 `req_v`、`req_r`、`resp_v`、`resp_r`、`count` 欄位。

---

## 14. 公開工具函式

| 函式 | 說明 |
|------|------|
| `mem_reset()` | 將記憶體陣列全部清零（不影響管線或輸出暫存器） |
| `resize(bytes)` | 調整記憶體陣列大小，新增位元組初始化為 `0` |
| `size_bytes_total()` | 回傳記憶體陣列目前的位元組大小 |
| `dump(start, len)` | 回傳 hex dump 字串，用於除錯，每行 16 bytes |
| `set_trace_context(pid, tid_base)` | 設定 trace 上下文（PID 及 thread ID） |
| `get_trace_num()` | 回傳此模組的 trace thread 數量（固定為 `1`） |

---

## 15. 私有輔助函式

### `read_word(byte_addr)`

```cpp
sc_biguint<DATA_WIDTH_BITS> read_word(uint32_t byte_addr) const
```

從 `mem[]` 讀取一個對齊字組，採小端序組裝：

$$
\text{out}[8i+7 : 8i] = \text{mem}[(byte\_addr + i) \bmod \text{mem.size()}]
\quad \forall i \in [0, \text{data\_width\_bytes})
$$

位址以 `mem.size()` 取模，自動包覆處理越界。

### `write_word(byte_addr, value, byte_mask)`

```cpp
void write_word(uint32_t byte_addr,
                sc_biguint<DATA_WIDTH_BITS> value,
                uint64_t byte_mask)
```

以 byte-enable 遮罩為條件，逐位元組寫入 `mem[]`：

$$
\text{if}\ \text{byte\_mask}[i] = 1:\ \text{mem}[(byte\_addr + i) \bmod \text{mem.size()}] \leftarrow \text{value}[8i+7 : 8i]
$$

---

## 16. 設計約束與限制

| 限制項目 | 說明 |
|----------|------|
| `DATA_WIDTH_BITS` 必須為 8 的倍數 | 由靜態斷言保證 |
| `ADDR_WIDTH` 最大 32 位元 | 由靜態斷言保證 |
| `pipeline_depth` 範圍 1～255 | 受 `sc_uint<8>` 的 `count_reg` / `head_reg` 限制 |
| `default_latency` 最大 255 週期 | 受 `sc_uint<8>` 的 `pipe_cycles_reg` 限制 |
| 記憶體陣列以模數包覆 | 越界位址進行 `addr % mem.size()` 包覆，而非報錯 |
| 不支援讀寫排序保證 | 同週期同位址讀寫時，讀取到新值（write-first 語意） |
| `mem` 不是 `sc_signal` | `mem` 為 `std::vector`，`write_word` 立即生效，不遵循 delta-cycle 語意 |
