# HybridAcc-CC：編譯管線詳細規格

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
  num_pes: 64         # per cluster
  num_bus: 4           # per cluster
  spm_size_bytes: 32768  # 32 KB per cluster
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
| `num_pes` | int | 否 | 64 | 1 ≤ n ≤ 256，必須為 2 的冪 |
| `num_bus` | int | 否 | 4 | 1 ≤ n ≤ 16 |
| `spm_size_bytes` | int | 否 | 32768 | ≥ 4096，必須為 2 的冪 |
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
5. **SPM 容量**：各 layer 所需的 working set 不超過 `spm_size_bytes`（初版不做精確檢查，只做粗估 warning）。

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
2. 計算 template patch 參數
3. 計算 4 個 AGU bank 的完整暫存器配置
4. 計算 scan-chain 配置（PE router 連線拓撲）
5. 決定 cluster mask（哪些 cluster 參與此 layer）
6. 計算 SPM 地址分配（weight / activation / partial-sum / output 在 SPM 中的 base address）
7. 生成 DMA 描述子（若需從 DRAM 搬運資料）

### 3.2 演算法概述

```python
def lower_op(op: OpDesc, hw: HardwareDesc) -> LayerHwConfig:
    # 1. 根據 op_type 選擇 PE kernel template
    template = select_pe_template(op.op_type)

    # 2. 計算 tiling 參數（若張量超過 SPM 容量）
    tiling = compute_tiling(op, hw)

    # 3. 計算 SPM layout（各 buffer 的 base address）
    spm_layout = compute_spm_layout(op, hw, tiling)

    # 4. 計算 4 個 AGU bank 配置
    agu_configs = compute_agu_configs(op, spm_layout, tiling)

    # 5. 計算 scan-chain（PE router 拓撲）
    scan_chain = compute_scan_chain(hw.num_pes, hw.num_bus, op.op_type)

    # 6. 計算 PE template patch 參數
    pe_params = compute_pe_params(op, tiling, hw)

    # 7. 決定 cluster mask
    cluster_mask = compute_cluster_mask(hw.num_clusters)

    # 8. 組裝完整 LayerHwConfig
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
| `firmware_main.c` | `_start` 入口、layer 呼叫序列、trap handler |
| `firmware_layers.c` | 每個 layer 的配置函數實作 |
| `firmware_hw.h` | MMIO 位址常數、helper macro/inline function |
| `firmware_payload.h` | PE program binary 的 `#include`（由 Stage 3 生成） |

### 4.3 MMIO 指令序列概要

每個 layer 的配置函數生成以下 MMIO 序列：

```
1. 設定 CLUSTER_MASK_LO/HI
2. Broadcast: SPM config (config_map + update)
3. Broadcast: HDDU soft-reset
4. Broadcast: 4 個 AGU bank 的完整 register 寫入
5. Broadcast: HDDU PLANE_EN / PLANE_MODE
6. Broadcast: NoC CMD_RESET
7. Broadcast: NoC CMD_INIT
8. Broadcast: NoC scan-chain (反序)
9. Broadcast: NoC CMD_LOAD_PROGRAM × N_instructions
10. Broadcast: NoC CMD_START_PE
11. Broadcast: HDDU CTRL start_all
12. Poll HDDU_STATUS 直到完成
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

策略 A：直接嵌入 C array
   → 在 firmware_payload.h 中生成 static const uint16_t pe_prog_XXX[] = { ... };
   → firmware 在 runtime 逐 word 送出 CMD_LOAD_PROGRAM

策略 B：使用 ha-package
   → 呼叫 ha-package 將所有 patched templates 打包成 .pkg + .h
   → firmware 引用 ha-package 生成的 C API
```

本版建議使用 **策略 A**（直接嵌入 C array），因為：
1. 不需要 runtime 解 package 格式
2. 每個 layer 可能使用不同 patch 參數，package 無法直接支援
3. 實作最簡單

### 5.5 輸出

`firmware_payload.h`，包含每個 layer 的 patched PE program array。

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
    -c ${GEN_DIR}/firmware_layers.c -o ${BUILD_DIR}/firmware_layers.o

# 2. 連結
riscv32-unknown-elf-ld \
    -T ${GEN_DIR}/linker.ld \
    -o ${BUILD_DIR}/output.elf \
    ${BUILD_DIR}/firmware_main.o \
    ${BUILD_DIR}/firmware_layers.o
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
| E006 | `TILING_FAILED` | Stage 1 | 無法 fit SPM |
| E007 | `PE_TEMPLATE_NOT_FOUND` | Stage 3 | 找不到指定的 PE kernel JSON |
| E008 | `GCC_NOT_FOUND` | Stage 4 | 交叉編譯器缺失 |
| E009 | `ISRAM_OVERFLOW` | Stage 4 | Firmware text 超過 I-SRAM 容量 |
| E010 | `DSRAM_OVERFLOW` | Stage 4 | Data + payload 超過 Data-SRAM 容量 |
| E011 | `LINK_ERROR` | Stage 4 | 連結失敗 |
