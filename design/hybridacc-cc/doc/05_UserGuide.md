# HybridAcc-CC：使用者操作指南

> 前置閱讀：[00_Overview.md](00_Overview.md)

---

## 1. 環境準備

### 1.1 系統需求

| 項目 | 要求 |
|------|------|
| OS | Linux x86_64（Ubuntu 20.04+ 或同等） |
| Python | 3.8+ |
| RISC-V GCC | `riscv32-unknown-elf-gcc` 12.x+（含 `rv32i_zicsr` support） |
| PE ISA Toolchain | `ha-asm`, `ha-objdump`, `ha-package`（來自 `hybridacc_tools`） |

### 1.2 安裝 hybridacc-cc

hybridacc-cc 作為 `hybridacc_tools` Python monorepo 的一部分，使用 pip 安裝：

```bash
cd /path/to/HybridAcc
pip install -e .
```

安裝後，`hacc-compile` CLI 命令即可使用：

```bash
hacc-compile --help
```

### 1.3 確認 RISC-V GCC

```bash
riscv32-unknown-elf-gcc --version
# 輸出示例：
# riscv32-unknown-elf-gcc (GCC) 12.2.0
# ...

# 確認支援 rv32i_zicsr
riscv32-unknown-elf-gcc -march=rv32i_zicsr -mabi=ilp32 -E -x c /dev/null
# 無錯誤即為支援
```

若未安裝，參考 [RISC-V GNU Toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain) 建置。

### 1.4 確認 PE ISA Toolchain

```bash
ha-asm --help
ha-package --help
```

若未安裝，同樣透過 `pip install -e .` 在 HybridAcc 根目錄安裝。

---

## 2. 撰寫 Workload YAML

### 2.1 YAML 格式總覽

```yaml
# workload.yaml
hardware:
  num_clusters: 4
  pes_per_cluster: 64
  spm_size_kb: 32
  isram_size_kb: 16
  dsram_size_kb: 64

ops:
  - name: conv1
    type: conv2d_3x3
    input:
      shape: [1, 14, 14, 16]   # [N, H, W, C_in]
      dtype: fp16
    weight:
      shape: [16, 3, 3, 16]    # [OC, KH, KW, C_in]
      dtype: fp16
    output:
      shape: [1, 12, 12, 16]   # [N, OH, OW, OC]
      dtype: fp16
    attrs:
      stride: 1
      padding: valid

  - name: conv2
    type: conv2d_1x1
    input:
      shape: [1, 12, 12, 16]
      dtype: fp16
    weight:
      shape: [32, 1, 1, 16]
      dtype: fp16
    output:
      shape: [1, 12, 12, 32]
      dtype: fp16
    attrs:
      stride: 1
      padding: valid

  - name: fc1
    type: gemm
    input:
      shape: [1, 128]          # [M, K]
      dtype: fp16
    weight:
      shape: [128, 64]         # [K, N]
      dtype: fp16
    output:
      shape: [1, 64]           # [M, N]
      dtype: fp16
    attrs: {}
```

### 2.2 `hardware` 區塊

| 欄位 | 型別 | 必填 | 說明 |
|------|------|------|------|
| `num_clusters` | int | ✅ | Compute Cluster 數量 |
| `pes_per_cluster` | int | ✅ | 每個 cluster 的 PE 數量 |
| `spm_size_kb` | int | ✅ | 每個 cluster 的 SPM 容量（KB） |
| `isram_size_kb` | int | ✅ | Core I-SRAM 容量（KB），預設 16 |
| `dsram_size_kb` | int | ✅ | Core Data-SRAM 容量（KB），預設 64 |

### 2.3 `ops` 區塊

每個 op entry 定義一個要執行的算子。

#### 共通欄位

| 欄位 | 型別 | 必填 | 說明 |
|------|------|------|------|
| `name` | string | ✅ | 唯一識別名稱（生成函數名用） |
| `type` | string | ✅ | `conv2d_3x3`、`conv2d_1x1`、`gemm` |
| `input` | object | ✅ | 輸入 tensor 描述 |
| `weight` | object | ✅ | 權重 tensor 描述 |
| `output` | object | ✅ | 輸出 tensor 描述 |
| `attrs` | object | ✅ | 算子特有屬性 |

#### Tensor 描述

| 欄位 | 型別 | 必填 | 說明 |
|------|------|------|------|
| `shape` | list[int] | ✅ | 維度列表 |
| `dtype` | string | ✅ | 資料型態（目前僅 `fp16`） |

#### Conv2D attrs

| 欄位 | 型別 | 必填 | 說明 |
|------|------|------|------|
| `stride` | int | ✅ | 卷積步進 |
| `padding` | string | ✅ | `valid`（無 padding） |

#### GEMM attrs

GEMM 目前不需要額外屬性，傳入空 dict `{}` 即可。

### 2.4 Shape 規則

**Conv2D 3×3：**
- `input.shape` = `[N, H, W, C_in]`（NHWC）
- `weight.shape` = `[OC, 3, 3, C_in]`
- `output.shape` = `[N, H-2, W-2, OC]`（valid padding, stride=1）
- `C_in` 必須為 4 的倍數（packing 要求）
- `OC ≤ 16`（scan-chain 限制）

**Conv2D 1×1：**
- `weight.shape` = `[OC, 1, 1, C_in]`
- `output.shape` = `[N, H, W, OC]`（no spatial reduction）
- `C_in` 必須為 12 的倍數

**GEMM：**
- `input.shape` = `[M, K]`
- `weight.shape` = `[K, N]`
- `output.shape` = `[M, N]`
- `K` 必須為 4 的倍數（packing）

---

## 3. 執行編譯

### 3.1 基本用法

```bash
hacc-compile workload.yaml -o output_dir/
```

### 3.2 完整 CLI 參數

```bash
hacc-compile [OPTIONS] WORKLOAD_YAML

位置引數:
  WORKLOAD_YAML          Workload YAML 檔案路徑

選項:
  -o, --output DIR       輸出目錄（預設: ./build/）
  --dump-ir              Dump 中間 IR（WorkloadIR, HardwareIR）為 JSON
  --dump-c               保留生成的 .c/.h 原始碼（預設: 保留）
  --no-compile           僅生成 C 原始碼，不執行 GCC 編譯
  --gcc PATH             指定 riscv32-unknown-elf-gcc 路徑
  --stack-size BYTES     Stack 保留大小（預設: 4096）
  --opt-level {0,1,2,s}  GCC 最佳化等級（預設: 2）
  --verbose, -v          顯示詳細 log
  --version              顯示版本號
  -h, --help             顯示幫助訊息
```

### 3.3 輸出目錄結構

```
output_dir/
├── firmware.elf               # 最終 ELF 韌體映像
├── firmware_main.c            # 主程式原始碼（可審查）
├── firmware_layers.c          # Layer 函數原始碼
├── firmware_hw.h              # 硬體抽象 header
├── firmware_payload.h         # PE payload 數據 header
├── linker.ld                  # Linker script
├── workload_ir.json           # (--dump-ir) WorkloadIR
├── hardware_ir.json           # (--dump-ir) HardwareIR
└── compile_report.json        # 編譯報告
```

---

## 4. 端對端範例

### 4.1 範例 1：單層 Conv2D 3×3

#### Step 1：建立 YAML

```yaml
# simple_conv.yaml
hardware:
  num_clusters: 1
  pes_per_cluster: 16
  spm_size_kb: 32
  isram_size_kb: 16
  dsram_size_kb: 64

ops:
  - name: conv1
    type: conv2d_3x3
    input:
      shape: [1, 14, 14, 16]
      dtype: fp16
    weight:
      shape: [16, 3, 3, 16]
      dtype: fp16
    output:
      shape: [1, 12, 12, 16]
      dtype: fp16
    attrs:
      stride: 1
      padding: valid
```

#### Step 2：編譯

```bash
hacc-compile simple_conv.yaml -o build/ --dump-ir -v
```

#### Step 3：預期輸出

```
[INFO] Stage 0: Parsing workload YAML ...
[INFO]   Found 1 operations: [conv1(conv2d_3x3)]
[INFO] Stage 1: Lowering to HardwareIR ...
[INFO]   conv1: template=conv1d_k3c4s1, num_pes=16, spm_total=8960 B
[INFO] Stage 2: Generating C firmware ...
[INFO]   Generated 4 files: firmware_main.c, firmware_layers.c, firmware_hw.h, firmware_payload.h
[INFO] Stage 3: Preparing PE payloads ...
[INFO]   conv1: 36 instructions, 5 patches applied
[INFO] Stage 4: Compiling ELF ...
[INFO]   riscv32-unknown-elf-gcc -march=rv32i_zicsr -mabi=ilp32 ...
[INFO] ELF Size Report:
[INFO]   .text   =   1852 / 16384 B (11.3%)
[INFO]   .data   =     72 B
[INFO]   .bss    =      0 B
[INFO]   D-SRAM  =     72 / 61440 B (0.1%)
[INFO]   stack   =   4096 B (reserved)
[INFO] Build successful: build/firmware.elf
```

#### Step 4：驗證中間產物

```bash
# 查看生成的 C 程式碼
cat build/firmware_layers.c

# 查看 HardwareIR
cat build/hardware_ir.json | python -m json.tool

# 反組譯 ELF
riscv32-unknown-elf-objdump -d build/firmware.elf | head -60

# 查看 section 大小
riscv32-unknown-elf-size build/firmware.elf
```

### 4.2 範例 2：多層混合網路

#### Step 1：建立 YAML

```yaml
# mixed_network.yaml
hardware:
  num_clusters: 4
  pes_per_cluster: 64
  spm_size_kb: 32
  isram_size_kb: 16
  dsram_size_kb: 64

ops:
  - name: conv3x3_layer
    type: conv2d_3x3
    input:  { shape: [1, 14, 14, 16], dtype: fp16 }
    weight: { shape: [16, 3, 3, 16],  dtype: fp16 }
    output: { shape: [1, 12, 12, 16], dtype: fp16 }
    attrs: { stride: 1, padding: valid }

  - name: conv1x1_layer
    type: conv2d_1x1
    input:  { shape: [1, 12, 12, 12], dtype: fp16 }
    weight: { shape: [16, 1, 1, 12],  dtype: fp16 }
    output: { shape: [1, 12, 12, 16], dtype: fp16 }
    attrs: { stride: 1, padding: valid }

  - name: gemm_layer
    type: gemm
    input:  { shape: [16, 128], dtype: fp16 }
    weight: { shape: [128, 64], dtype: fp16 }
    output: { shape: [16, 64],  dtype: fp16 }
    attrs: {}
```

#### Step 2：編譯

```bash
hacc-compile mixed_network.yaml -o build_mixed/ --dump-ir -v
```

#### Step 3：預期韌體行為

生成的 `firmware_main.c` 的 `main()` 函數依序呼叫：

```c
void main(void) {
    REG_CORE_STATUS = 0x00000001u;

    layer_conv3x3_layer();   /* Conv2D 3x3 on all 4 clusters */
    layer_conv1x1_layer();   /* Conv2D 1x1 on all 4 clusters */
    layer_gemm_layer();      /* GEMM on all 4 clusters */

    REG_CORE_STATUS = 0x00000002u;
    while (1) { __asm__ volatile("wfi"); }
}
```

每個 layer 函數完成自己的 cluster 配置、PE program 載入、啟動、等待。Layer 之間的 data movement（DMA）在 v0.1 中由使用者在 YAML 中額外定義（未來版本將自動推導）。

---

## 5. 中間產物檢查

### 5.1 WorkloadIR（`workload_ir.json`）

代表 Frontend parse 的結果，可用來確認 YAML 是否被正確解讀：

```json
{
  "hardware": {
    "num_clusters": 4,
    "pes_per_cluster": 64,
    "spm_size_kb": 32,
    "isram_size_kb": 16,
    "dsram_size_kb": 64
  },
  "ops": [
    {
      "name": "conv3x3_layer",
      "type": "conv2d_3x3",
      "input": {"shape": [1, 14, 14, 16], "dtype": "fp16"},
      "weight": {"shape": [16, 3, 3, 16], "dtype": "fp16"},
      "output": {"shape": [1, 12, 12, 16], "dtype": "fp16"},
      "attrs": {"stride": 1, "padding": "valid"}
    }
  ]
}
```

### 5.2 HardwareIR（`hardware_ir.json`）

代表 Lowering 的結果，包含所有硬體參數：

```json
{
  "layers": [
    {
      "name": "conv3x3_layer",
      "op_type": "conv2d_3x3",
      "template": "conv1d_k3c4s1",
      "target_cluster_mask": 15,
      "spm_config_map": 228,
      "agu_ps": {
        "base_addr": 0,
        "iter01": 196612,
        "iter23": 1048579,
        "stride0": 8,
        "stride1": 32,
        "stride2": 96,
        "stride3": 288,
        "lane_cfg": 0,
        "tag_base": 0,
        "tag_stride0": 1,
        "tag_stride1": 0,
        "tag_ctrl": 1,
        "mask_cfg": 15,
        "ctrl": 0
      },
      "agu_pd": { "..." },
      "agu_pli": { "..." },
      "agu_plo": { "..." },
      "hddu_plane_en": 15,
      "hddu_plane_mode": 1,
      "scan_chain": [
        {"pe_id": 0, "ps_id": 0, "pd_id": 0, "pli_id": 0, "plo_id": 0, "route_mode": 1, "enable": true},
        {"pe_id": 1, "ps_id": 1, "pd_id": 1, "pli_id": 1, "plo_id": 1, "route_mode": 0, "enable": true}
      ],
      "pe_program": {
        "template": "conv1d_k3c4s1",
        "params": {"KERNEL_DMA_LEN": 192, "OUTPUT_WINDOW_CNT_MINUS_ONE": 195},
        "patched_instructions": [76, 4, 32, 12264, 48, "..."]
      },
      "num_clusters": 4
    }
  ]
}
```

### 5.3 Compile Report（`compile_report.json`）

```json
{
  "version": "0.1.0",
  "input_yaml": "mixed_network.yaml",
  "timestamp": "2024-01-15T10:30:00Z",
  "stages": {
    "frontend": {"status": "ok", "ops_count": 3},
    "lowering": {"status": "ok", "layers_count": 3},
    "codegen": {"status": "ok", "files": ["firmware_main.c", "firmware_layers.c", "firmware_hw.h", "firmware_payload.h"]},
    "pe_prep": {"status": "ok", "payloads": 3},
    "elf_link": {"status": "ok", "gcc_return_code": 0}
  },
  "elf_size": {
    "text": 5248,
    "data": 232,
    "bss": 0,
    "isram_usage_pct": 32.0,
    "dsram_usage_pct": 0.4
  },
  "layers": [
    {"name": "conv3x3_layer", "type": "conv2d_3x3", "pe_instructions": 36, "scan_chain_entries": 64},
    {"name": "conv1x1_layer", "type": "conv2d_1x1", "pe_instructions": 36, "scan_chain_entries": 64},
    {"name": "gemm_layer", "type": "gemm", "pe_instructions": 27, "scan_chain_entries": 64}
  ]
}
```

---

## 6. 除錯指南

### 6.1 常見錯誤與診斷

#### E001：YAML Parse Error

```
[ERROR] E001: Failed to parse workload YAML: expected a mapping, line 5, column 3
```

**原因**：YAML 語法錯誤。
**修正**：使用 YAML lint 工具（如 `yamllint`）檢查語法。

#### E002：Unknown Op Type

```
[ERROR] E002: Unknown operator type 'depthwise_conv' in op 'dw1'
```

**原因**：`type` 欄位不在支援列表 `{conv2d_3x3, conv2d_1x1, gemm}` 中。
**修正**：確認算子類型名稱。

#### E003：Shape Mismatch

```
[ERROR] E003: conv1: output shape [1, 12, 12, 16] does not match computed [1, 12, 12, 32]
```

**原因**：YAML 中的 `output.shape` 與由 `input.shape` + `weight.shape` 推導出的理論值不吻合。
**修正**：依據公式重新計算 output shape。

#### E004：Channel Alignment

```
[ERROR] E004: conv1(conv2d_3x3): C_in=15 is not divisible by 4
```

**原因**：Conv2D 3×3 要求 `C_in % 4 == 0`（packing constraint）。
**修正**：調整 C_in 為 4 的倍數（可考慮 zero-padding channels）。

#### E005：SPM Overflow

```
[ERROR] E005: conv1: SPM required 34816 B exceeds cluster SPM 32768 B
```

**原因**：此 layer 的 weight + activation + partial sum + output 總量超出 SPM 容量。
**修正**：
- 減小 tensor 維度
- 使用 tiling（未來版本）
- 增加 SPM 容量

#### E006：PE Template Not Found

```
[ERROR] E006: No PE template found for operator type 'conv2d_5x5'
```

**原因**：要求的算子類型沒有對應的 PE kernel template。
**修正**：使用支援的算子類型，或撰寫新的 PE kernel template。

#### E007：Patch Value Overflow

```
[ERROR] E007: conv1: KERNEL_DMA_LEN=1025 exceeds 10-bit payload max (1023)
```

**原因**：PE instruction payload 為 10-bit，value - 1 必須 ≤ 1023。
**修正**：調整 tensor 維度以使參數在範圍內。

#### E010：I-SRAM Overflow

```
[ERROR] E010: .text (17920 B) exceeds I-SRAM (16384 B) by 1536 bytes
```

**原因**：韌體 code 太大。
**修正**：
- 減少 layer 數量
- 啟用 data-driven AGU 模式（減少 code size，見 [03_CodeGeneration.md §9.1](03_CodeGeneration.md)）
- 增加 I-SRAM 容量

#### E011：Data-SRAM Overflow

```
[ERROR] E011: .data+.bss (62000 B) exceeds Data-SRAM available (61440 B)
```

**原因**：PE payload 和資料區域太大。
**修正**：減少 layer 數量或增加 Data-SRAM 容量。

### 6.2 使用 `--dump-ir` 檢查中間結果

```bash
# 生成 IR dump 但不編譯
hacc-compile workload.yaml -o debug/ --dump-ir --no-compile

# 查看 lowering 結果
python -m json.tool debug/hardware_ir.json

# 確認特定 layer 的 AGU 配置是否正確
python -c "
import json
with open('debug/hardware_ir.json') as f:
    ir = json.load(f)
layer = ir['layers'][0]
print(f'PS base_addr: 0x{layer[\"agu_ps\"][\"base_addr\"]:08X}')
print(f'PD base_addr: 0x{layer[\"agu_pd\"][\"base_addr\"]:08X}')
print(f'PLI base_addr: 0x{layer[\"agu_pli\"][\"base_addr\"]:08X}')
print(f'PLO base_addr: 0x{layer[\"agu_plo\"][\"base_addr\"]:08X}')
"
```

### 6.3 使用 objdump 檢查 ELF

```bash
# 反組譯
riscv32-unknown-elf-objdump -d build/firmware.elf

# 查看 section headers
riscv32-unknown-elf-objdump -h build/firmware.elf

# 查看 symbol table
riscv32-unknown-elf-nm build/firmware.elf | sort
```

### 6.4 在 Simulator 中驗證

```bash
# 使用 SystemC ESL simulator 載入並執行
cd /path/to/HybridAcc/output/cluster-sim/
./run_sim --elf build/firmware.elf --max-cycles 100000

# 檢查 CORE_STATUS register 最終值
# 預期值: 0x00000002 (all layers done)
```

---

## 7. 進階用法

### 7.1 自訂 Stack Size

若韌體需要更大的 call stack（例如使用了較深的 helper function 嵌套）：

```bash
hacc-compile workload.yaml -o build/ --stack-size 8192
```

### 7.2 指定 GCC 路徑

若系統安裝了多個版本的 RISC-V GCC：

```bash
hacc-compile workload.yaml -o build/ \
  --gcc /opt/riscv/bin/riscv32-unknown-elf-gcc
```

### 7.3 僅生成 C 原始碼

若需要手動修改生成的 C code 再編譯：

```bash
# 只生成 .c/.h，不呼叫 GCC
hacc-compile workload.yaml -o build/ --no-compile

# 手動修改 firmware_layers.c（若需要）
vim build/firmware_layers.c

# 手動編譯
cd build/
riscv32-unknown-elf-gcc \
    -march=rv32i_zicsr -mabi=ilp32 \
    -nostdlib -ffreestanding -O2 \
    -Wl,--gc-sections \
    -ffunction-sections -fdata-sections \
    -T linker.ld -I . \
    -o firmware.elf firmware_main.c firmware_layers.c
```

### 7.4 Debug Build（無最佳化）

```bash
hacc-compile workload.yaml -o build_debug/ --opt-level 0 -v
```

`-O0` 產生的 code 較大但保留所有 debug info，方便使用 GDB 或 simulator trace 對照。

---

## 8. 常見問答

### Q1：支援的最大 layer 數量是多少？

取決於 I-SRAM 容量。以預設 16 KB 估算，每個 layer 函數約 2~3 KB（含 scan-chain 展開），因此大約 5~7 layers。若使用 data-driven mode 可進一步壓縮。

### Q2：PE kernel template 來自哪裡？

來自 `design/hybridacc-cc/kernel/json/` 目錄中的 JSON 檔案。這些 JSON 由 `ha-asm` + `ha-package` 從手動撰寫的 `.asm` template 生成。

### Q3：可以混合不同 cluster 數量的 layer 嗎？

可以。每個 layer 的 `target_cluster_mask` 獨立設定。例如 conv layer 使用 4 個 cluster，gemm layer 只使用 1 個 cluster。Lowering 階段會根據 op 特性和硬體配置自動決定 cluster 分配策略。

### Q4：ELF 可以直接在 FPGA 上燒錄嗎？

v0.1 產生的 ELF 設計為 simulator 驗證用途。FPGA / ASIC 的 boot loader 需要另外實作 ELF → binary 轉換，或直接使用 `riscv32-unknown-elf-objcopy -O binary` 產生 raw binary。

### Q5：如何新增支援新的算子類型？

1. 在 `kernel/template/` 撰寫新的 `.asm` PE kernel
2. 使用 `ha-asm` + `ha-package` 產生對應的 `.json` template
3. 在 `hybridacc-cc` 的 Lowering 模組中新增算子對應的 `lower_<op_type>()` 函數
4. 在 Frontend 加入算子類型識別與 validation 規則
5. 更新本文件

---

## 9. 完整工作流程總結

```
┌─────────────────────────────────────────────────────────────┐
│                    使用者操作流程                              │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. 撰寫 workload.yaml                                      │
│     ↓                                                       │
│  2. hacc-compile workload.yaml -o build/ --dump-ir -v       │
│     ↓                                                       │
│  3. 檢查 compile_report.json 確認無錯誤                       │
│     ↓                                                       │
│  4. (Optional) 檢查 hardware_ir.json 確認 lowering 正確       │
│     ↓                                                       │
│  5. (Optional) 檢查 firmware_layers.c 確認 MMIO 序列          │
│     ↓                                                       │
│  6. 載入 firmware.elf 到 simulator 驗證                       │
│     ↓                                                       │
│  7. 確認 CORE_STATUS == 0x00000002                           │
│     ↓                                                       │
│  ✅ 完成                                                     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```
