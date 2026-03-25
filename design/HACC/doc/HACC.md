# HACC AI Compiler 與 Runtime 整合規格

本文件定義 HACC compiler、package、loader、runtime firmware 與 controller hardware 之間的正式契約。目標不是只描述編譯流程，而是把「compiler 產生什麼」、「loader 載入什麼」、「MCU 如何消費」、「RTL 要提供什麼」一次對齊。

本版以 MCU-driven runtime 為唯一正式模型。任何仍假設 block fetch / block expander / wave scheduler 由專用硬體黑盒處理的敘述，都視為舊版內容，不屬於本文件。

---

## 0. 核心結論

### 0.1 系統主結論

1. HACC compiler 的輸出主體不是「讓 execution complex 自主執行的一堆 descriptor」，而是「讓 MCU 韌體可直接解譯並控制 DMA / cluster / NLU 的 local tables 與 payload」。
2. `.hacc.core` 是 MCU 韌體程式。
3. `.hacc.job` 提供 job root metadata。
4. `.hacc.block` 提供 block-level 壓縮控制描述。
5. `.hacc.profile/.hacc.dma/.hacc.agu/.hacc.nlu/.hacc.pe/.hacc.scan/.hacc.patch` 都是 MCU runtime 消費的 rule/payload tables。
6. loader 只負責把 sections 載入 local memories，不直接對 cluster/NLU/DMA 產生 runtime side effect。

### 0.2 這代表什麼

1. Compiler 必須把資料組織成 MCU 容易順序讀取與 stream 的形式。
2. 韌體必須負責 block 展開、wave loop、例外 patch 套用、DMA payload 下發、profile 套用與完成收斂。
3. 硬體不再隱含一個巨大的 runtime scheduler 去替軟體做這些事。

---

## 1. 角色分工

### 1.1 Compiler 的責任

1. 將 graph/op lowering 成 block-level execution plan。
2. 決定 tile、cluster mapping、DMA 需求、AGU/profile/NLU/PE/scan 配置。
3. 生成 compact table 與 payload，使 MCU 可以 deterministic 地順序消費。
4. 生成 `.hacc.core` 韌體或韌體所需的 patchable constants。
5. 輸出可 review 的 debug artifact。

### 1.2 Loader 的責任

1. 驗證 package 基本格式。
2. 根據 manifest 將各 section 載入 I-SRAM 或 `cc_data_sram`，並由 ABI 區分 descriptor/payload 與 event/debug 用途。
3. 不做 wave expansion。
4. 不直接寫 cluster HDDU/NoC/NLU/DMA runtime window。

### 1.3 MCU firmware 的責任

1. 讀 `.hacc.job` 與 `.hacc.block`。
2. 逐 block 展開 wave loop。
3. 根據 `.hacc.profile/.hacc.agu` 套用 cluster runtime 狀態。
4. 根據 `.hacc.dma` 產生 DMA stream payload。
5. 根據 `.hacc.pe/.hacc.scan` 產生 NoC command stream。
6. 根據 `.hacc.nlu` 設定與啟動 NLU。
7. 處理中斷、錯誤與同步。

### 1.4 Hardware 的責任

1. 提供 local memories 與 deterministic command path。
2. 提供 DMA、cluster command、NLU MMIO、IRQ router。
3. 不偷藏一個與 compiler 契約不一致的 wave scheduler。

---

## 2. 編譯鏈分層

### 2.1 正式分層

```text
Frontend Graph / Op Parser
  -> Semantic Validator
  -> Unified HACC IR Builder
  -> Schedule & Resource Planner
  -> Cluster Lowering Backend
  -> NLU Lowering Backend
  -> Block Builder
  -> Runtime Payload Builder
  -> MCU Firmware Emitter / Patcher
  -> HACC-ELF Packager
```

### 2.2 各層責任摘要

1. Frontend：解析 Conv/GEMM 或未來 graph IR。
2. Planner：決定 tile、cluster 使用、memory partition、phase dependency。
3. Cluster lowering：產生 PE、AGU、profile、scan 相關結果。
4. NLU lowering：產生 non-linear phase 與其 source/destination references。
5. Block builder：把大量 wave 聚合為 block 與 formula/patch 表示。
6. Runtime payload builder：把 cluster/NLU/DMA 需要的 payload 轉成 MCU 易消費格式。
7. MCU firmware emitter：生成或 patch `.hacc.core`。
8. Packager：輸出 HACC-ELF 與 review artifact。

---

## 2.1 CLI 輸入規格

目前 Python package `hacc` 的 CLI 入口為：

```bash
hacc compile <spec.yaml|spec.json> -o <output_prefix>
```

CLI 目前接受單一 op spec，並直接對應到 frontend 的 `create_conv2d(...)` 或 `create_gemm(...)`。根節點必須是 mapping；若為其他型別，CLI 會直接拒絕。

### 2.1.1 共同欄位

所有 spec 都支援以下欄位：

1. `name`：op 名稱，用於輸出訊息與 debug artifact。
2. `op_type`：目前支援 `conv2d`、`gemm`。
3. `dtype`：目前支援 `int8`、`int16`、`fp16`、`fp32`；CLI 會轉成 `DataType` enum。
4. `with_nlu`：布林值；若為 `true`，compiler 可產生 NLU phase。預設 `false`。

### 2.1.2 `conv2d` 欄位

`conv2d` spec 需要：

1. `n`
2. `ic`
3. `ih`
4. `iw`
5. `oc`
6. `kh`
7. `kw`

以下欄位可省略，CLI 會補預設值：

1. `stride_h`：預設 `1`
2. `stride_w`：預設 `1`
3. `pad_h`：預設 `0`
4. `pad_w`：預設 `0`
5. `with_nlu`：預設 `false`

輸出 shape 由 frontend 根據下式計算：

$$
OH = \left\lfloor \frac{IH + 2 \cdot pad_h - KH}{stride_h} \right\rfloor + 1
$$

$$
OW = \left\lfloor \frac{IW + 2 \cdot pad_w - KW}{stride_w} \right\rfloor + 1
$$

若算出的 `OH` 或 `OW` 非正數，frontend validation 會失敗。

範例：

```yaml
name: conv2d_basic
op_type: conv2d
dtype: int16
n: 1
ic: 16
ih: 8
iw: 8
oc: 32
kh: 3
kw: 3
stride_h: 1
stride_w: 1
pad_h: 1
pad_w: 1
with_nlu: false
```

直接編譯：

```bash
hacc compile design/HACC/examples/conv2d_basic.yaml -o /tmp/conv2d_basic
```

### 2.1.3 `gemm` 欄位

`gemm` spec 需要：

1. `M`
2. `N`
3. `K`

以下欄位可省略，CLI 會補預設值：

1. `trans_a`：預設 `false`
2. `trans_b`：預設 `false`
3. `with_nlu`：預設 `false`

目前 frontend 會將 shape 固定映射為：

1. input = `(1, 1, M, K)`
2. weight = `(1, 1, K, N)`
3. output = `(1, 1, M, N)`

範例：

```yaml
name: gemm_basic
op_type: gemm
dtype: int16
M: 64
N: 64
K: 128
trans_a: false
trans_b: false
with_nlu: false
```

直接編譯：

```bash
hacc compile design/HACC/examples/gemm_basic.yaml -o /tmp/gemm_basic
```

### 2.1.4 範例檔位置

repository 內建可直接拿來驗證 CLI 的範例檔：

1. `design/HACC/examples/conv2d_basic.yaml`
2. `design/HACC/examples/gemm_basic.yaml`

這兩份範例的目的不是覆蓋所有 compiler 行為，而是提供一個最小、合法、可直接編譯的輸入格式基準，讓 packaging、CLI、ELF 產物與 debug artifact 可以快速 smoke test。

---

## 3. Job / Block / Wave 的正式定義

### 3.1 Job

Job 是單次工作執行的靜態根節點，至少包含：

1. 各 section/table 的 base 與 count
2. 目標 capability
3. cluster / NLU 使用限制
4. block 範圍資訊
5. error / sync / overlap policy

### 3.2 Block

Block 是一段由 MCU 重複展開的壓縮控制片段。Block 不應只是「舊版 wave scheduler 的前置資料」，而是直接給 MCU 韌體使用的 compact program fragment。

Block 建議至少包含：

1. loop rank 與各維度上界
2. repeat / step 規則
3. profile / dma / agu / pe / scan / nlu rule index
4. rule stride：每個外層迴圈步進對應的 rule index 增量（`rule_stride`、`nlu_rule_stride`），使 compiler 可將多個結構相同但 rule offset 構成等差數列的 block 合併為單一壓縮 block
5. total waves：該 block 的 loop extents 乘積（預先計算，避免 MCU 在 runtime 做乘法）
6. cluster mask
7. block flags
8. patch table reference

### 3.3 Wave

Wave 是 block 在某個 loop index 組合下的單次 runtime instance。Wave 可以是邏輯概念，不一定需要在 package 中 fully materialize。

MCU 只要依 block 公式算出：

1. 這次要用哪個 profile
2. 這次要用哪個 DMA payload/rule
3. 這次要不要發 PE/scan/NLU config
4. 這次要對哪些 cluster 啟動

就已經完成 wave orchestration。

### 3.4 三者關係總結

1. Job 管全局 tables 與政策。
2. Block 管可壓縮的重複控制流程。
3. Wave 是 MCU 在 runtime 根據 Block 動態展開的實例。

---

## 4. Package 與 Section 契約

### 4.1 強制 section 集合

1. `.hacc.meta`
2. `.hacc.core`
3. `.hacc.job`
4. `.hacc.block`
5. `.hacc.profile`
6. `.hacc.dma`
7. `.hacc.agu`
8. `.hacc.nlu`
9. `.hacc.pe`
10. `.hacc.scan`
11. `.hacc.patch`
12. `.hacc.debug` 可選

### 4.2 Section route 正式定義

| Section | 載入位置 | runtime 消費方式 |
|---|---|---|
| `.hacc.core` | I-SRAM | MCU fetch |
| `.hacc.job` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.block` | Data-SRAM descriptor/payload ABI region | MCU load |
| `.hacc.profile` | Data-SRAM descriptor/payload ABI region | MCU 讀 table 後，以 MMIO 或 stream 套用到 cluster HDDU |
| `.hacc.dma` | Data-SRAM descriptor/payload ABI region | MCU 讀 table 後，串流 payload 到 DMA engine |
| `.hacc.agu` | Data-SRAM descriptor/payload ABI region | MCU 套用到 cluster HDDU |
| `.hacc.nlu` | Data-SRAM descriptor/payload ABI region | MCU 套用到 NLU MMIO / payload window |
| `.hacc.pe` | Data-SRAM descriptor/payload ABI region | MCU 串流到 cluster NoC command path |
| `.hacc.scan` | Data-SRAM descriptor/payload ABI region | MCU 串流到 cluster NoC command path |
| `.hacc.patch` | Data-SRAM descriptor/payload ABI region | MCU 讀取並覆寫 block/wave 派生欄位 |
| `.hacc.debug` | Data-SRAM event/debug ABI region / DRAM | debug/replay only |

### 4.3 `cc_data_sram` 軟體 ABI 分區

本版不再要求硬體提供獨立的 `cc_desc_sram` 與 `cc_event_sram`。取而代之，compiler、loader 與 firmware 共同遵守單一 `cc_data_sram` 軟體 ABI：

1. descriptor/payload ABI region：存放 job/block/rule/payload tables
2. event/debug ABI region：存放 completion bitmap、trace、debug scratch、optional spill area

這個分區可以由固定 ABI offset 或 job metadata 公布，但對 software contract 而言必須是 deterministic 且可 review 的。

### 4.4 兩個明確限制

1. `.hacc.dma` 不是供 DMA engine 自行 random access 的 autonomous rule heap；其所有有效 runtime payload 都由 MCU 下發。
2. `.hacc.pe` 與 `.hacc.scan` 不在 loader 階段直接寫入 cluster，而是由 MCU 在適當 phase 串流。

---

## 5. 建議的描述資料格式

### 5.1 `.hacc.job`

建議至少包含：

1. `block_table_base`
2. `block_count`
3. `profile_table_base` / `profile_count`
4. `dma_table_base` / `dma_count`
5. `agu_table_base` / `agu_count`
6. `nlu_table_base` / `nlu_count`
7. `pe_table_base` / `pe_count`
8. `scan_table_base` / `scan_count`
9. `patch_table_base` / `patch_count`
10. `required_cluster_mask`
11. `required_caps`
12. `job_flags`

### 5.2 `.hacc.block`

建議 block header 至少包含：

1. `loop_rank`
2. `loop_extent[0..3]`
3. `repeat_count`
4. `cluster_mask`
5. `profile_rule_idx`
6. `dma_rule_idx`
7. `agu_rule_idx`
8. `pe_payload_idx`
9. `scan_payload_idx`
10. `nlu_rule_idx`
11. `rule_stride` — 每個 d0（最外層迴圈）步進的 rule index 增量（= d1 × d2）。MCU 可用 `base + sequential_wave_counter` 做線性存取，或在需要 per-dimension 索引（例如 NLU barrier）時用 stride 推算
12. `nlu_rule_stride` — 每個 d0 步進的 NLU rule index 增量（= d1），供 MCU 在 NLU phase 計算正確 rule index
13. `total_waves` — loop extents 的乘積，預先計算以避免 MCU runtime 乘法（MCU ISA 不含硬體乘法指令）
14. `patch_begin`
15. `patch_count`
16. `block_flags`

### 5.3 `.hacc.patch`

Patch 應只描述少量例外覆寫，而不是取代 block 壓縮。建議欄位：

1. compare key：`logical_wave_id` 或 `loop_idx tuple`
2. valid mask：指明覆寫哪些欄位
3. override payload：profile/dma/cluster mask 等被覆寫值

### 5.4 `.hacc.profile`

`.hacc.profile` 可由兩層結構組成：

1. rule header：描述要套用到哪些 register window、word count、target mask 規則
2. payload words：要實際串流的 32-bit words

### 5.5 `.hacc.dma`

`.hacc.dma` 也建議拆為：

1. rule header：描述 mode、word count、cluster mask、start policy
2. payload words：實際要送進 DMA engine 的 32-bit stream

如此 MCU 韌體可直接以 header + `STRM` 方式執行，不需在 runtime 重組複雜 blob。

### 5.6 `.hacc.pe` 與 `.hacc.scan`

建議格式同樣採：

1. payload header：字數、對應 cluster mask、是否需先清空/同步
2. word stream：直接對應 NoC command path 的 32-bit words

---

## 6. Firmware execution model

### 6.1 正常流程

1. MCU 讀 job root。
2. MCU 視需要載入 PE program 與 scan-chain config。
3. MCU 迴圈走訪 blocks。
4. 對每個 block，MCU 依 loop/patch 規則生成 wave instance。
5. 對每個 wave，MCU 套用 profile/AGU、下發 DMA、啟動 cluster。
6. 若 block 含 NLU phase，則在適當 barrier 後設定並啟動 NLU。
7. MCU 以 polling 或 IRQ 模式等待完成。

### 6.2 為什麼保留 block 壓縮

改成 MCU-driven runtime，不代表 compiler 要 fully expand wave table。仍應保留 block 壓縮，理由如下：

1. fully expanded wave table 會讓 package 體積暴增。
2. 大量重複的 rule/payload 由 block + formula + patch 表示更自然。
3. MCU 具備子程序與迴圈能力，正好適合解譯這種結構。

### 6.2.1 Block 合併壓縮策略

當多個 block 僅在 rule index 上構成等差數列（`base, base+stride, base+2*stride, …`），且其他欄位（loop extent, cluster mask, flags 等）完全一致時，compiler 應將它們合併為一個 block，並透過 `rule_stride` 欄位記錄步進量。

合併前後對照：

| 指標 | 合併前 | 合併後 |
|---|---|---|
| block 數量 | d1 個 | 1 個 |
| `.hacc.block` size | d1 × BLOCK_WORDS | BLOCK_WORDS |
| MCU 行為 | 外層迴圈 d1 次 block dispatch | 單一 block 內含 d0×d1×d2 三層迴圈 |

合併後的 block 將所有維度收進 `loop_extent[0..2]`，MCU 以 sequential wave counter 線性走訪 profile/DMA/AGU table，無需 runtime 乘法。需要 per-dimension 索引（如 NLU barrier）時，MCU 可利用 `rule_stride` 與 `nlu_rule_stride` 還原維度。

### 6.3 韌體與 compiler 的切分線

Compiler 應負責把重複模式壓縮得足夠好；韌體應負責 runtime 依序解譯與下發。兩者中間不應再假設有一個硬體 expander 幫忙補語意。

---

## 7. Loader 契約

### 7.1 輸入

Loader 接受：

1. manifest entries
2. 每個 section 的 DRAM base、byte size、dst kind、dst base
3. optional checksum

### 7.2 行為

1. 驗證基本格式與邊界。
2. 把 `.hacc.core` 載入 I-SRAM。
3. 把其餘 runtime sections 載入 `cc_data_sram` 的對應 ABI region。
4. 在 local load 完成前，不釋放 MCU 進入正式執行。

### 7.3 不做的事

1. 不做 relocation-heavy ELF 處理。
2. 不做 PE/scan/program stream emission。
3. 不做 DMA command emission。
4. 不做 NLU config emission。

---

## 8. Compiler 輸出與 review artifact

### 8.1 建議輸出層次

1. Stage0：可讀 JSON，供 review、diff、sim replay
2. Stage1：binary-friendly tables
3. Stage2：HACC-ELF

### 8.2 Stage0 至少要有的資訊

1. job summary
2. block list
3. loop extents
4. cluster mapping
5. DMA payload summary
6. PE/scan payload summary
7. NLU phases
8. patch coverage

### 8.3 為什麼 reviewer 需要 Stage0

Gemini 或任何 reviewer 如果只看 ELF binary，很難確認：

1. block 壓縮是否合理
2. 某些 wave 是否因 patch 被特殊覆寫
3. profile / dma / nlu payload 是否與 block index 對得上

因此 compiler 應保留可讀 review artifact，而不只輸出最終 package。

---

## 9. Capability 與版本相容

### 9.1 capability bitmap 建議

至少包含：

1. cluster NoC stream 支援
2. DMA payload stream 支援
3. NLU path 支援
4. broadcast write 支援
5. IRQ router 支援
6. subroutine ISA 支援

### 9.2 版本升級規則

1. section header 必須帶 version。
2. unknown section type 應在 load 或 compile review 階段報錯。
3. capability mismatch 不應等到 runtime 深處才發現。

---

## 10. Example：Conv block 的編譯與執行

### 10.1 編譯期

以一個 convolution 為例，compiler 應：

1. 把 output tile 切成若干 blocks。
2. 為每個 block 決定 cluster mask、profile、AGU、DMA 規則。
3. 生成 `.hacc.pe`、`.hacc.scan` payload。
4. 若有 post-op softmax 或 layernorm，生成 `.hacc.nlu` rule。
5. 產生 MCU 韌體可用的 tables 與 `.hacc.core`。

### 10.2 執行期

1. loader 載入所有 sections。
2. MCU 讀 job root。
3. MCU 先串流 `.hacc.pe/.hacc.scan`，完成 cluster 程式載入。
4. MCU 對 block 做 loop 展開。
5. MCU 套用 `.hacc.profile/.hacc.agu`。
6. MCU 串流 `.hacc.dma` payload 到 DMA engine。
7. MCU 啟動 cluster。
8. MCU 在需要時設定 NLU 並等待其 IRQ。

這裡沒有任何一步依賴硬體 block expander 自行去讀 `.hacc.dma` 或 `.hacc.profile`。

---

## 11. 驗證與審稿檢查點

### 11.1 Compiler 端

1. block 壓縮後是否仍可由 MCU 線性解譯
2. `.hacc.dma` payload 是否已經整理成可直接 stream 的 32-bit word 序列
3. `.hacc.pe/.hacc.scan` 是否不含 loader-time side effect 假設
4. patch 是否只處理例外，不侵蝕 block 壓縮價值

### 11.2 Runtime/RTL 端

1. `cc_data_sram` descriptor/payload ABI region 的 32-bit word access 是否正確
2. broadcast 是否 all-target complete 才 retire
3. IRQ 是否遵守 level + sticky + ack reassert
4. DMA 是否完全由 MCU command/stream 驅動
5. loader 是否只做 local copy

### 11.3 文件一致性檢查

只要下列敘述仍出現在任何文件中，就代表規格尚未定乾淨：

1. 「execution complex 會自行展開 wave」
2. 「section loader 直接發 `.hacc.pe/.hacc.scan`」
3. 「DMA engine 自己讀 `.hacc.dma`」

---

## 12. 總結

本版 HACC 整合規格把整個系統重新對齊到一個更直接也更可驗證的模型：

1. Compiler 產生 MCU 可消費的 block/rule/payload tables。
2. Loader 只做 local memory load。
3. MCU 韌體直接控制 DMA、cluster、NLU。
4. Hardware 提供 deterministic command/stream/irq primitive。

這個模型保留 block 壓縮的優勢，同時消除舊版 execution complex 黑盒帶來的規格模糊區，適合作為後續 RTL、模擬器與 compiler 的共同基準。
