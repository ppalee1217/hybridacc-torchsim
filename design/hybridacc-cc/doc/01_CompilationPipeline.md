# HybridAcc-CC：編譯管線詳細規格

文件樹： [../../../doc/index.md](../../../doc/index.md) -> [00_Overview.md](00_Overview.md) -> 本頁。

> 前置閱讀：[00_Overview.md](00_Overview.md)

---

## 1. 管線總覽

```
  Workload YAML ─┐
                  v
        ┌────────────────────┐
        │  Stage 0: Frontend │    Parse YAML → WorkloadIR
        └────────┬───────────┘
                 │ WorkloadIR
                 v
        ┌────────────────────┐
        │ Stage 1: Lowering  │    WorkloadIR → HardwareIR
        └────────┬───────────┘
                 │ HardwareIR
                 v
   ┌─────────────┴─────────────┐
   │                           │
   v                           v
┌──────────────────┐  ┌───────────────────┐
│ Stage 2: CodeGen │  │ Stage 3: PE Prep  │   (可並行)
│  HwIR → C source │  │  HwIR → .pkg/.h   │
└────────┬─────────┘  └────────┬──────────┘
         │                     │
         └──────────┬──────────┘
                    v
        ┌────────────────────┐
        │ Stage 4: ELF Link  │    GCC cross-compile + link
        └────────┬───────────┘
                 │
                 v
            output.elf
```

Stage 2（C code generation）與 Stage 3（PE payload preparation）之間沒有資料依賴，可以並行執行。Stage 4 必須等待兩者都完成。

---

## 2. Stage 0：Frontend（YAML Parse）

### 2.1 目的

將使用者撰寫的 Workload YAML 解析為型別安全的 `WorkloadIR` 資料結構。此階段執行：

1. YAML 語法驗證
2. Schema 驗證（必填欄位、型別檢查）
3. 張量形狀合法性檢查
4. 算子參數合法性檢查
5. 硬體參數合法性檢查

### 2.2 輸入格式：Workload YAML

```yaml
# workload.yaml
name: "resnet_block_1"

hardware:
  num_clusters: 4
  num_pes: 48              # per cluster
  num_bus: 3                # per cluster
  spm_banks_per_group: 3   # SPM banks per group（參照 SPM.md）
  spm_bank_depth: 8192     # words per bank（每 word = 8 bytes）
  # 衍生值：group_capacity = 3 × 8192 × 8 = 192 KB/group
  dram_base: 0x80000000

ops:
  - name: "conv1"
    type: "conv2d_3x3"
    inputs:
      - name: "input0"
        shape: [1, 14, 14, 16]   # [N, H, W, C]
        dtype: "fp16"
    weights:
      - name: "weight0"
        shape: [16, 3, 3, 16]    # [OC, KH, KW, IC]
        dtype: "fp16"
    outputs:
      - name: "output0"
        shape: [1, 14, 14, 16]   # [N, H_out, W_out, OC]
        dtype: "fp16"
    attrs:
      stride: 1
      padding: 1

  - name: "conv2"
    type: "conv2d_1x1"
    inputs:
      - name: "output0"          # 參照前一 op 的輸出
        shape: [1, 14, 14, 16]
        dtype: "fp16"
    weights:
      - name: "weight1"
        shape: [16, 1, 1, 16]
        dtype: "fp16"
    outputs:
      - name: "output1"
        shape: [1, 14, 14, 16]
        dtype: "fp16"
    attrs:
      stride: 1
      padding: 0

  - name: "fc1"
    type: "gemm"
    inputs:
      - name: "A"
        shape: [1, 256]          # [M, K]
        dtype: "fp16"
      - name: "B"
        shape: [256, 64]         # [K, N]
        dtype: "fp16"
    outputs:
      - name: "C"
        shape: [1, 64]           # [M, N]
        dtype: "fp16"
    attrs: {}
```

### 2.3 Schema 驗證規則

#### 頂層

| 欄位 | 型別 | 必填 | 驗證規則 |
|------|------|------|---------|
| `name` | string | 是 | 非空，合法 C identifier |
| `hardware` | object | 是 | 見下方 |
| `ops` | list | 是 | 至少一個 op |

#### `hardware` 區塊

| 欄位 | 型別 | 必填 | 預設值 | 驗證規則 |
|------|------|------|--------|---------|
| `num_clusters` | int | 是 | — | 1 ≤ n ≤ 16 |
| `num_pes` | int | 否 | 48 | 1 ≤ n ≤ 256，必須能被 num_bus 整除 |
| `num_bus` | int | 否 | 3 | 1 ≤ n ≤ 16 |
| `spm_banks_per_group` | int | 否 | 3 | 1 ≤ n ≤ 8，SPM 每 Group 的 bank 數（參照 SPM.md） |
| `spm_bank_depth` | int | 否 | 8192 | ≥ 1024，必須為 2 的冪，每 bank 的 word 數 |
| `dram_base` | int | 否 | 0x80000000 | 4KB 對齊 |

#### 每個 `ops[]` 項目

| 欄位 | 型別 | 必填 | 驗證規則 |
|------|------|------|---------|
| `name` | string | 是 | 非空，唯一 |
| `type` | string | 是 | 必須為 `"conv2d_3x3"`, `"conv2d_1x1"`, `"gemm"` 之一 |
| `inputs` | list | 是 | 張量描述，見下方 |
| `weights` | list | conv2d 必填 | 權重張量描述 |
| `outputs` | list | 是 | 張量描述 |
| `attrs` | object | 否 | 算子特定屬性 |

#### 張量描述

| 欄位 | 型別 | 必填 | 驗證規則 |
|------|------|------|---------|
| `name` | string | 是 | 非空 |
| `shape` | list[int] | 是 | 所有元素 > 0 |
| `dtype` | string | 是 | 必須為 `"fp16"` |

#### 算子特定 `attrs` 驗證

**conv2d_3x3 / conv2d_1x1**：

| 欄位 | 型別 | 必填 | 預設值 | 驗證規則 |
|------|------|------|--------|---------|
| `stride` | int | 否 | 1 | ≥ 1 |
| `padding` | int | 否 | 0 | ≥ 0 |

**gemm**：

無額外必填屬性。

### 2.4 語意驗證

Frontend 除了 schema 驗證外，還必須執行以下語意檢查：

1. **形狀一致性**：conv2d 的 weight `IC` 維度必須等於 input `C` 維度。
2. **通道對齊**：conv2d_3x3 的 `C_in` 必須為 4 的倍數；conv2d_1x1 的 `C_in` 必須為 12 的倍數。
3. **輸出形狀**：`H_out = (H_in + 2*padding - KH) / stride + 1`，必須為正整數。
4. **GEMM 維度**：`A.shape[1]` 必須等於 `B.shape[0]`；`C.shape = [A.shape[0], B.shape[1]]`。
5. **SPM 容量（強制 ping-pong 檢查）**：各 layer 經 tiling 後的 wave tile，其 PS/PD/PLI/PLO 各自的 buffer size 不得超過 SPM Group 的半容量（`half_group_capacity = spm_banks_per_group × spm_bank_depth × 8 / 2`）。所有 Group 強制使用 ping-pong 雙緩衝，不存在降級模式。檢查失敗為 hard error（`E_SPM_HALF_CAP_OVERFLOW`）。
6. **SPM/AGU 模式一致性**：
   - Conv2D 3×3：所有 group 一律使用 SPM Linear Mode + AGU Normal Mode（`CTRL.bit3=0`）。
   - Conv2D 1×1：PS/PLI/PLO 一律使用 SPM Parallel Mode + AGU Ultra Mode（`CTRL.bit3=1`），PD 一律使用 SPM Linear Mode + AGU Normal Mode。SPM/AGU 模式並不依「normal vs ultra path」而切換；該 path 區分只影響 scan-chain 拓樸與 tiling，詳見 §3.2。
   - GEMM：同 Conv2D 1×1（PS/PLI/PLO Parallel + Ultra，PD Linear + Normal）。
   不匹配為 hard error（`E_SPM_ULTRA_MODE_MISMATCH`）。

### 2.4.1 Conv2D 1×1 Path 選擇與 Scan-chain Truth Table

Conv2D 1×1 的 PE-lane 對應到 local output H row，而非 `H × W` flatten；W 維由 PE template 內部 `OUTPUT_WINDOW_CNT_MINUS_ONE` 全程 temporal 處理。Lowering 依 `H_out` 與 PE budget 自動選 path：

| 條件 | path | active_buses | 每 bus active row | 單列 Scan-chain `route_mode` |
| --- | --- | --- | --- | --- |
| `H_out ≤ pes_per_bus` | normal | 1 | `H_out` | `(IB, OB)` = `PLI_FROM_BUS_PLO_TO_BUS` (3) |
| `pes_per_bus < H_out ≤ num_pes` | ultra | `ceil(H_out / pes_per_bus)` | `pes_per_bus` | `(IB, OB)` (3) |
| `H_out > num_pes` | ultra（多 wave） | `num_bus` | `pes_per_bus` | `(IB, OB)` (3) |

對 conv2d_1x1，scan-chain `route_mode` 永遠是 `(IB, OB)`：每個 active PE 直接從 MBUS 取 PLI/初值，計算完直接寫回 MBUS，沒有跨列 LN 累積（因為 kernel height = 1）。

**Single-row 開放問題決議**：當 conv non-ultra 出現 `split_kh = 1` 時，唯一那一列同時是「first row」與「last row」。其唯一語意自洽的 `route_mode` 即 `(IB, OB)`，因為此時既沒有上游 LN 鄰居能提供 PLI，也沒有下游 LN 鄰居能接 PLO。compiler 與 verification (`noc_gen.get_route_mode`) 都採此規則。

### 2.4.2 GEMM Plane 對應與 K-chain

| 平面 | DRAM 對應 | AGU group |
| --- | --- | --- |
| PS  | B `[K, N]` | Group 0（Parallel + Ultra） |
| PD  | A `[M, K]` | Group 1（Linear  + Normal） |
| PLI | C `[M, N]`（讀回作 partial sum） | Group 2（Parallel + Ultra） |
| PLO | C `[M, N]`（寫回 final sum） | Group 3（Parallel + Ultra） |

GEMM scan-chain 為 K-chain 拓樸：bus `i` 承擔 K-stage `i`。`grid_k_per_wave = min(grid_k, num_bus)`：

| 階段 | `route_mode` |
| --- | --- |
| `grid_k_per_wave == 1`（single K-stage） | `(IB, OB)` (3) |
| 第一 K-stage（multi-stage） | `(IB, OL)` (1) |
| 中間 K-stage | `(IL, OL)` (0) |
| 最後 K-stage | `(IL, OB)` (2) |

ID 規則（與 `noc_gen.generate_gemm_test` 對齊）：

- ultra：`ps_id = n_idx`，`pd_id = m_idx`（tag 在 K-stage 間共用）。
- normal：`ps_id = k_idx * grid_n + n_idx`，`pd_id = k_idx * grid_m + m_idx`。
- `pli_id` 僅在第一 K-stage 有效，其餘為 63；`plo_id` 僅在最後 K-stage 有效，其餘為 63。
- `grid_k_per_wave == 1` 時，bus 0 既是 first 也是 last，`pli_id` / `plo_id` 都有效。

PE-grid metadata 會以 `GRID_M / GRID_N / GRID_K / GRID_M_PER_WAVE / GRID_N_PER_WAVE / GRID_K_PER_WAVE` 等 key 寫入 `pe_program.params`，可從 `hardware_ir.json` 直接驗證。

### 2.5 輸出

`WorkloadIR` 物件，結構定義見 [00_Overview.md](00_Overview.md) §6.1。

### 2.6 錯誤處理

所有驗證失敗都以 `CompilationError` exception 報告，包含：

- 錯誤類別（schema / semantic / shape_mismatch）
- 出錯的 YAML 路徑（e.g., `ops[0].inputs[0].shape`）
- 人類可讀的錯誤訊息

---

## 3. Stage 1：Operator Lowering

### 3.1 目的

將每個 `OpDesc`（高層運算描述）轉換為 `LayerHwConfig`（硬體配置，含 AGU 參數、scan-chain、PE program 參照）。

此階段是整個編譯器最核心的演算法階段，負責：

1. 決定每個 layer 的 PE kernel template 選擇
2. 計算 tiling 參數（wave tile 維度，受 per-group SPM 容量約束）
3. 計算 SPM per-group address layout（含 ping-pong 雙緩衝地址配置）
4. 精確驗證每個 SPM Group 的容量（hard error，非粗估 warning）
5. 計算 multi-cluster mapping（spatial tile → cluster 的分配策略）
6. 計算 PE template patch 參數（基於 tiled 維度，而非原始張量維度）
7. 計算 4 個 AGU bank 的完整暫存器配置（per-group base address + ping/pong 交替）
8. 計算 scan-chain 配置（PE router 連線拓撲）
9. 計算 temporal schedule（wave loop 結構 + DMA/compute overlap 流水線）
10. 決定 cluster mask（依 multi-cluster mapping 決定）
11. 生成 DMA 描述子（配合 ping-pong 目標地址）

### 3.2 演算法概述

```python
def lower_op(op: OpDesc, hw: HardwareDesc) -> LayerHwConfig:
    # 1. 根據 op_type 選擇 PE kernel template
    template = select_pe_template(op.op_type)

    # 2. 計算 SPM per-group 容量
    group_capacity = hw.spm_banks_per_group * hw.spm_bank_depth * 8

    # 3. 計算 tiling（wave tile 維度，受 per-group 容量約束）
    tiling = compute_tiling(op, hw, group_capacity)

    # 4. 計算 SPM per-group layout（含 ping-pong 地址配置）
    spm_layout = compute_spm_layout(op, hw, tiling, group_capacity)

    # 5. 精確驗證 per-group SPM 容量
    validate_per_group_capacity(spm_layout, group_capacity)

    # 6. 計算 multi-cluster mapping（spatial tile → cluster 分配）
    cluster_mapping = compute_cluster_mapping(op, hw, tiling)

    # 7. 計算 PE template patch 參數（基於 tiled 維度）
    pe_params = compute_pe_params(op, tiling, hw)

    # 8. 計算 4 個 AGU bank 配置（per-group base address + ping/pong）
    agu_configs = compute_agu_configs(op, spm_layout, tiling)

    # 9. 計算 scan-chain（PE router 拓撲）
    scan_chain = compute_scan_chain(hw.num_pes, hw.num_bus, op.op_type)

    # 10. 計算 temporal schedule（wave loop + DMA/compute overlap）
    schedule = compute_temporal_schedule(tiling, cluster_mapping)

    # 11. 決定 cluster mask
    cluster_mask = compute_cluster_mask(cluster_mapping)

    # 12. 組裝完整 LayerHwConfig
    return LayerHwConfig(...)
```

詳細的 lowering 演算法請參閱 [02_OperatorLowering.md](02_OperatorLowering.md)。

### 3.3 輸出

`HardwareIR` 物件，包含所有 layer 的 `LayerHwConfig`。

### 3.4 錯誤處理

| 錯誤 | 原因 | 處理 |
|------|------|------|
| `UnsupportedOp` | op_type 不在支援列表 | 中斷編譯 |
| `TilingFailed` | 無論如何 tiling 都無法 fit SPM | 中斷編譯，報告最小所需 SPM |
| `ChannelAlignError` | 通道數不符 PE kernel 要求 | 中斷編譯，建議 padding |

---

## 4. Stage 2：Code Generation

### 4.1 目的

將 `HardwareIR` 轉換為可編譯的 C 原始碼。生成的 C 程式碼將在 `cc_core_mcu`（RV32I_zicsr core）上執行，透過 MMIO load/store 完成所有硬體配置。

### 4.2 生成的檔案

| 檔案 | 內容 |
|------|------|
| `firmware_main.c` | `_start` 入口、layer 迴圈呼叫、trap handler |
| `firmware_data.c` | Per-layer config struct table（`.rodata`）：AGU, HDDU, TilingParams compile-time 常數（O(1) per layer） |
| `firmware_ops.c` | 共用 runtime 函式：`run_layer()`, `run_loop_tiling()`, `cfg_agu_bank()`, DMA 控制 |
| `firmware_hw.h` | MMIO 位址常數、helper inline function（含 DMA MMIO） |
| `firmware_payload.h` | PE template binary（未 patch）、pre-encoded scan chain、patch 描述表 |

### 4.3 MMIO 指令序列概要

`run_layer()` 執行以下兩級 MMIO 序列：

**Layer 級別（一次性設定）**：
```
1. 設定 CLUSTER_MASK_LO/HI
2. HDDU soft-reset
3. AGU 4 bank iter/stride/tag 設定（data-driven loop，base_addr 由 wave 更新）
4. HDDU PLANE_EN / PLANE_MODE
5. NoC CMD_RESET + CMD_INIT
6. NoC scan-chain（pre-encoded uint32_t array）
7. Runtime PE patch + CMD_LOAD_PROGRAM
```

**Wave 級別（每個 wave 重複）**：
```
W1. 確認 DMA prefetch 完成（首 wave 同步載入，後續由前一輪 async 完成）
W2. SPM_CONFIG_MAP 更新（PLI/PLO group 交換）
W3. AGU BASE_ADDR 更新（ping/pong 切換）
W4. NOC_CMD_START_PE + HDDU start_all
W5. DMA prefetch 下一 wave（async，overlap with compute）
W6. Poll HDDU_STATUS 直到完成
W7. NOC_CMD_STOP_PE
W8. DMA writeback（僅 IC/K 累加最後一筆 tile）
```

詳細的 code generation 規則請參閱 [03_CodeGeneration.md](03_CodeGeneration.md)。

### 4.4 輸出

生成的 `.c` / `.h` 檔案存放於 build 目錄的 `gen/` 子目錄。

---

## 5. Stage 3：PE Payload Preparation

### 5.1 目的

為 `HardwareIR` 中引用的所有 PE kernel templates 準備可嵌入韌體的 binary payload。

### 5.2 處理流程

```
1. 收集 HardwareIR 中所有唯一的 PeProgramRef
2. 對每個 template：
   a. 載入對應的 JSON metadata (from kernel/json/)
   b. 從 JSON 中取出 instructions[] (base machine code)
   c. 根據 params 與 patches[] 進行 runtime patching
      - 讀取 patch.offset 與 patch.param_index
      - 用新的 param value 替換 instruction word 的 payload 欄位
   d. 輸出 patched instruction array (uint16_t[])
3. 將所有 patched program 打包為 C header
   (可選：也可使用 ha-package 產生 .pkg + .h)
```

### 5.3 Patch 演算法

PE instruction 的 patch 規則如下：

```python
def patch_instruction(word: int, new_payload_value: int) -> int:
    """
    PE instruction format:
      [15:6] = payload (10 bits)
      [5]    = func1
      [4:3]  = func2
      [2:1]  = opcode
      [0]    = LE (loop end)

    Patch 只替換 payload[15:6]，保留其餘欄位不變。
    但需考慮編碼規則：
    - LOOPIN / LDMA.LEN / SDMA.LEN 等使用 N-1 編碼
    - 因此 new_payload_value 應已經是 N-1 後的值
    """
    mask_payload = 0xFFC0  # bits [15:6]
    mask_rest    = 0x003F  # bits [5:0]
    return ((new_payload_value << 6) & mask_payload) | (word & mask_rest)
```

具體範例：若 `conv1d_k3c4s1` template 的 `KERNEL_COUNT` 需設為 8（而非預設 16）：

1. 找到 `patches` 中 `param_index` 對應 `KERNEL_COUNT`（index 2）的 patch 項目
2. Patch 項目指示 `offset: 17` 和 `offset: 26`
3. 對 `instructions[17]` 和 `instructions[26]`，將 payload 替換為 `(8 - 1) = 7`
4. `instructions[17]` 原為 `0x03C4`（`LOOPIN 16`），patched 後為 `(7 << 6) | (0x03C4 & 0x3F) = 0x01C4`（`LOOPIN 8`）

### 5.4 Payload 編碼格式

```
有兩種策略可供選擇（實作時二擇一）：

compile-time patch + runtime apply 策略：
   → firmware_payload.h 嵌入未 patch 的 PE template（同一 template 共用一份）
   → firmware_payload.h 嵌入 per-layer 的 PePatchEntry[] 描述表
   → firmware 在 runtime 呼叫 pe_patch_runtime() 套用 patch
   → 再逐 word 送出 CMD_LOAD_PROGRAM
```

此策略的優勢：
1. 同一 template 只嵌入一次，多 layer 共用
2. Patch 描述表極小（每個 entry 僅 3 bytes）
3. N-1 encoding 在 compile-time 完成，runtime 不需判斷指令類型

### 5.5 輸出

`firmware_payload.h`，包含 PE template binary + patch 描述表 + pre-encoded scan chain。

---

## 6. Stage 4：ELF Compilation & Linking

### 6.1 目的

將 Stage 2 生成的 C 原始碼 + Stage 3 生成的 payload header，透過 `riscv32-unknown-elf-gcc` 交叉編譯並連結為最終 ELF。

### 6.2 處理流程

```bash
# 1. 編譯 firmware
riscv32-unknown-elf-gcc \
    -march=rv32i_zicsr -mabi=ilp32 \
    -O2 -nostdlib -ffreestanding \
    -I ${GEN_DIR} \
    -c ${GEN_DIR}/firmware_main.c -o ${BUILD_DIR}/firmware_main.o

riscv32-unknown-elf-gcc \
    -march=rv32i_zicsr -mabi=ilp32 \
    -O2 -nostdlib -ffreestanding \
    -I ${GEN_DIR} \
    -c ${GEN_DIR}/firmware_data.c -o ${BUILD_DIR}/firmware_data.o

riscv32-unknown-elf-gcc \
    -march=rv32i_zicsr -mabi=ilp32 \
    -O2 -nostdlib -ffreestanding \
    -I ${GEN_DIR} \
    -c ${GEN_DIR}/firmware_ops.c -o ${BUILD_DIR}/firmware_ops.o

# 2. 連結
riscv32-unknown-elf-ld \
    -T ${GEN_DIR}/linker.ld \
    -o ${BUILD_DIR}/output.elf \
    ${BUILD_DIR}/firmware_main.o \
    ${BUILD_DIR}/firmware_data.o \
    ${BUILD_DIR}/firmware_ops.o
```

### 6.3 Linker Script 概要

```ld
MEMORY {
    ISRAM  (rx)  : ORIGIN = 0x00000000, LENGTH = 16K
    DSRAM  (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

SECTIONS {
    .text : {
        *(.text.start)    /* _start entry point */
        *(.text*)
    } > ISRAM

    .rodata : {
        *(.rodata*)       /* PE payload arrays, constants */
    } > DSRAM

    .data : {
        *(.data*)
    } > DSRAM

    .bss : {
        *(.bss*)
    } > DSRAM

    .stack (NOLOAD) : {
        . = ALIGN(16);
        . = . + 4K;
        _stack_top = .;
    } > DSRAM
}

ENTRY(_start)
```

詳細的 ELF layout 請參閱 [04_ELF_Layout.md](04_ELF_Layout.md)。

### 6.4 輸出

最終的 `output.elf`，可由 `cc_section_loader` 載入。

### 6.5 錯誤處理

| 錯誤 | 原因 | 處理 |
|------|------|------|
| GCC not found | 交叉編譯器未安裝 | 中斷編譯，提示安裝路徑 |
| Link overflow | 生成的 firmware 超過 ISRAM 16KB 限制 | 中斷編譯，報告大小 |
| Data overflow | payload + data 超過 DSRAM 64KB | 中斷編譯，報告大小 |
| Undefined symbol | 生成的 C code 有語法/邏輯錯誤 | 中斷，dump GCC stderr |

---

## 7. 編譯報告

每次編譯完成後，產生一份 JSON 格式的編譯報告，供開發者檢視與除錯：

```json
{
  "workload": "resnet_block_1",
  "status": "success",
  "elf_path": "build/output.elf",
  "elf_size": {
    "text": 2048,
    "rodata": 1024,
    "data": 256,
    "bss": 0,
    "total": 3328
  },
  "layers": [
    {
      "name": "conv1",
      "op_type": "conv2d_3x3",
      "pe_template": "conv1d_k3c4s1_template",
      "pe_params": {
        "KERNEL_DMA_LEN": 48,
        "OUTPUT_WINDOW_CNT_MINUS_ONE": 13,
        "KERNEL_COUNT": 16,
        "KERNEL_LOOP_INNER": 1,
        "KERNEL_LOOP_OUTER": 1
      },
      "cluster_mask": "0x0000000F",
      "agu_config": {
        "ps": { "base": "0x0000", "iter": [4, 3, 3, 16], "stride": [8, 32, 96, 288] },
        "pd": { "base": "0x0900", "iter": [4, 14, 14, 1], "stride": [8, 896, 64, 0] },
        "pli": { "base": "0x3200", "iter": [4, 14, 14, 1], "stride": [8, 896, 64, 0] },
        "plo": { "base": "0x4B00", "iter": [4, 14, 14, 1], "stride": [8, 896, 64, 0] }
      },
      "scan_chain_entries": 64,
      "pe_program_size": 36,
      "estimated_cycles": null
    }
  ],
  "compilation_time_ms": 150
}
```

---

## 8. 中間產物摘要

每個 Stage 都會在 build 目錄下產生可檢視的中間產物：

| Stage | 產物 | 位置 | 格式 |
|-------|------|------|------|
| 0 | 解析後的 WorkloadIR | `build/ir/workload_ir.json` | JSON |
| 1 | 各 layer HardwareIR | `build/ir/hardware_ir.json` | JSON |
| 2 | 生成的 C 原始碼 | `build/gen/firmware_*.c, firmware_*.h` | C source |
| 3 | Patched PE payload | `build/gen/firmware_payload.h` | C header |
| 3 | PE patch 報告 | `build/ir/pe_patches.json` | JSON |
| 4 | 物件檔 | `build/obj/*.o` | ELF object |
| 4 | 最終 ELF | `build/output.elf` | ELF |
| 4 | 編譯報告 | `build/report.json` | JSON |

所有中間產物都可透過 `hacc-compile --dump-ir` 強制輸出（即使在正常模式下也會產出 ELF 和 report）。

---

## 9. 錯誤碼一覽

| 碼 | 名稱 | 階段 | 說明 |
|----|------|------|------|
| E001 | `YAML_PARSE_ERROR` | Stage 0 | YAML 語法錯誤 |
| E002 | `SCHEMA_VALIDATION_ERROR` | Stage 0 | 必填欄位缺失或型別錯誤 |
| E003 | `SHAPE_MISMATCH` | Stage 0 | 張量形狀不一致 |
| E004 | `UNSUPPORTED_OP` | Stage 1 | 不支援的算子類型 |
| E005 | `CHANNEL_ALIGN_ERROR` | Stage 1 | 通道數不符 PE kernel 要求 |
| E006 | `TILING_FAILED` | Stage 1 | 無法找到任何 fit SPM per-group 容量的 tile 組合 |
| E012 | `SPM_HALF_CAP_OVERFLOW` | Stage 1 | SPM 半容量不足（wave tile > half_group_capacity，無法滿足強制 ping-pong） |
| E013 | `CLUSTER_REDUCTION_SPLIT` | Stage 1 | Multi-cluster 沿 reduction 維度切分（IC/K），會導致跨 cluster partial sum |
| E014 | `CLUSTER_MAPPING_FAILED` | Stage 1 | 無法將 spatial tiles 分配到 clusters |
| E015 | `AGU_ADDR_OVERLAP` | Stage 1 | Ping/pong buffer 地址空間重疊 |
| E007 | `PE_TEMPLATE_NOT_FOUND` | Stage 3 | 找不到指定的 PE kernel JSON |
| E008 | `GCC_NOT_FOUND` | Stage 4 | 交叉編譯器缺失 |
| E009 | `ISRAM_OVERFLOW` | Stage 4 | Firmware text 超過 I-SRAM 容量 |
| E010 | `DSRAM_OVERFLOW` | Stage 4 | Data + payload 超過 Data-SRAM 容量 |
| E011 | `LINK_ERROR` | Stage 4 | 連結失敗 |
