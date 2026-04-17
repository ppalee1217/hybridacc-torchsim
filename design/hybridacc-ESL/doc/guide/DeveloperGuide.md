# HybridAcc ESL Simulator — Developer Guide

## 目錄

1. [系統架構](#1-系統架構)
2. [編譯器 (hybridacc-cc) 內部結構](#2-編譯器-hybridacc-cc-內部結構)
3. [ESL 模擬器內部結構](#3-esl-模擬器內部結構)
4. [DRAM 映像格式](#4-dram-映像格式)
5. [驗證工具鏈](#5-驗證工具鏈)
6. [端到端資料流](#6-端到端資料流)
7. [新增運算類型](#7-新增運算類型)
8. [除錯指南](#8-除錯指南)

---

## 1. 系統架構

```
┌─────────────────────────────────────────────────────────┐
│                                                         │
│  ┌──────────┐    ┌──────────┐    ┌──────────────────┐   │
│  │ Workload │───▶│  hacc-   │───▶│  firmware.elf    │   │
│  │  YAML    │    │ compile  │    │  hardware_ir.json│   │
│  └──────────┘    └──────────┘    └────────┬─────────┘   │
│                                           │             │
│  ┌──────────────┐                         │             │
│  │ gen_test_dram│◀────────────────────────┘             │
│  │              │                                       │
│  │  → dram_init.bin                                     │
│  │  → golden_output.bin                                 │
│  │  → golden_meta.txt                                   │
│  └──────┬───────┘                                       │
│         │                                               │
│  ┌──────▼──────────────────────────────┐                │
│  │        hybridacc-sim (SystemC)      │                │
│  │  ┌──────────────────────────────┐   │                │
│  │  │ CoreController (RV32I+Zmmul) │   │                │
│  │  │  ├─ CmdToAhbBridge          │   │                │
│  │  │  ├─ I-SRAM (16KB)           │   │                │
│  │  │  └─ Data-SRAM (64KB)        │   │                │
│  │  └────────────┬─────────────────┘   │                │
│  │               │ AHB                 │                │
│  │  ┌────────────▼─────────────────┐   │                │
│  │  │ NetworkOnChip                │   │                │
│  │  │  └─ ComputeCluster × N      │   │                │
│  │  │       └─ ProcessElement × M  │   │                │
│  │  └──────────────────────────────┘   │                │
│  │               │                     │                │
│  │  ┌────────────▼─────────────────┐   │                │
│  │  │ FakeDram (DRAM mirror)       │   │                │
│  │  │  → dram_init.bin.out         │   │                │
│  │  └──────────────────────────────┘   │                │
│  └─────────────────────────────────────┘                │
│                                                         │
│  ┌──────────────┐                                       │
│  │compare_golden│ ← dram_init.bin.out + golden_output   │
│  │  → PASS/FAIL │                                       │
│  └──────────────┘                                       │
└─────────────────────────────────────────────────────────┘
```

---

## 2. 編譯器 (hybridacc-cc) 內部結構

### 2.1 五階段管線

```
Stage 0: Frontend    — YAML → WorkloadIR (parse_workload)
Stage 1: Lowering    — WorkloadIR → HardwareIR (lower_workload)
Stage 2: CodeGen     — HardwareIR → C 原始碼 (generate_firmware) [Jinja2]
Stage 3: PE Prep     — HardwareIR → kernel NLU payload + scan-chain (並行)
Stage 4: ELF Build   — GCC 交叉編譯 → firmware.elf
```

### 2.2 核心資料結構

**WorkloadIR** (Stage 0 output):
```python
@dataclass
class WorkloadIR:
    name: str
    hardware: HardwareConfig    # num_clusters, num_pes, num_bus, spm...
    tensors: dict[str, TensorDesc]  # shape, dtype, layout
    ops: list[OpDesc]           # type, inputs, outputs, attrs
```

**HardwareIR** (Stage 1 output):
```python
@dataclass
class HardwareIR:
    layers: list[LayerHwConfig]

@dataclass
class LayerHwConfig:
    pe_template: str            # "k3c4", "k1c12", "gemm_ultra"
    agu_configs: list[AguBankConfig]  # 15 registers each
    tiling_params: TilingParams      # DRAM 佈局 + tiling 維度
    scan_chain: list[ScanChainEntry] # PE 路由拓撲
```

**TilingParams** — 驅動韌體 runtime 計算:
```python
@dataclass
class TilingParams:
    dram_weight_base: int    # Weight 在 DRAM 中的起始位址
    dram_input_base: int     # Input 在 DRAM 中的起始位址
    dram_output_base: int    # Output 在 DRAM 中的起始位址
    # tiling dimensions...
    tile_oh: int             # Output height tile size
    tile_ow: int             # Output width tile size
    tile_oc: int             # Output channel tile size
    # ...
```

### 2.3 PE Template 對應

| 運算類型 | PE Template | 說明 |
|---------|-------------|------|
| `conv2d_3x3` | `k3c4` | 3×3 kernel, 4 channels/PE |
| `conv2d_1x1` | `k1c12` | 1×1 kernel, 12 channels/PE |
| `gemm` | `gemm_ultra` | 通用矩陣乘法 |

### 2.4 Jinja2 模板系統

| 模板 | 生成檔案 | 用途 |
|------|---------|------|
| `firmware_hw.h.j2` | `firmware_hw.h` | MMIO 常數、DMA helper |
| `firmware_main.c.j2` | `firmware_main.c` | 進入點、trap handler |
| `firmware_ops.c.j2` | `firmware_ops.c` | Layer 執行、DMA、runtime patch |
| `firmware_data.c.j2` | `firmware_data.c` | Layer config 結構 (.rodata) |
| `firmware_payload.h.j2` | `firmware_payload.h` | PE binary 與 scan-chain |
| `linker.ld.j2` | `linker.ld` | 連結腳本 |

---

## 3. ESL 模擬器內部結構

### 3.1 模組層級

```
HybridAcc (sc_module)
├── CoreController          — RV32I+Zmmul CPU core
│   ├── I-SRAM (16KB)       — 指令記憶體
│   └── Data-SRAM (64KB)    — 資料記憶體 (DMA register-mapped)
├── CmdToAhbBridge          — Core ↔ AHB bus 轉換
├── NetworkOnChip           — 全域路由
│   └── ComputeCluster[N]   — 計算叢集
│       ├── SPM[banks]      — Scratchpad Memory
│       ├── AGU[groups]     — Address Generation Unit
│       ├── DMA Engine      — DRAM ↔ SPM 搬移
│       └── ProcessElement[M] — 運算元件
└── FakeDram                — DRAM 模型 (mirror file backed)
```

### 3.2 DMA 協議

韌體透過 MMIO 寫入 Data-SRAM 中的 DMA 暫存器觸發 DMA 傳輸：

```
DMA Load:  DRAM → SPM   (activation/weight tile → scratchpad)
DMA Store: SPM → DRAM   (output tile → DRAM)
```

每個 DMA 操作包含 28 個 AGU 暫存器（4D address generation），支援：
- Linear mode: 連續位址
- Bank mode: SPM bank 交錯定址

### 3.3 執行流程

1. **Loader** 將 `firmware.elf` 載入 I-SRAM
2. **FakeDram** 載入 `dram_init.bin` 作為 DRAM 初始內容
3. **CoreController** 從 reset vector 開始執行 RV32I 指令
4. 韌體初始化 → 逐層執行：
   - DMA Load weight tile → SPM
   - DMA Load input tile → SPM
   - 配置 scan-chain → PE 路由
   - 觸發 PE 計算 (broadcast activation, accumulate partial sums)
   - DMA Store output tile → DRAM
5. 韌體執行 EBREAK → 模擬結束
6. FakeDram 將內容寫回 `dram_init.bin.out`

---

## 4. DRAM 映像格式

### 4.1 Flat format (`dram_init.bin`)

輸入給模擬器的 DRAM 映像是一個 flat binary，從 `dram_base` 開始的連續位元組。佈局由 `hardware_ir.json` 中的 `tiling_params` 決定：

```
Offset 0x0000: [weight tensor bytes]
        ...
Offset weight_size: [input tensor bytes]
        ...
Offset input_end: [zeroed output region]
        ...
```

### 4.2 Sparse format (`dram_init.bin.out`)

模擬器寫出的 DRAM dump 使用 sparse format：

```
Byte 0-3:   Magic 0x53505253 ('SPRS' little-endian)
Byte 4-7:   Version (uint32)
Byte 8+:    Records:
              Byte 0-3: Address (uint32)
              Byte 4-7: Length (uint32)
              Byte 8+:  Data[Length]
            Sentinel: Address=0, Length=0
```

### 4.3 Golden 元資料 (`golden_meta.txt`)

```
dram_base=0x80000000
dram_weight_base=0x80000000
dram_input_base=0x80001200
dram_output_base=0x80002A80
input_shape=[1, 16, 16, 4]
weight_shape=[16, 3, 3, 4]
output_shape=[1, 14, 14, 16]
dram_image_bytes=16384
golden_output_bytes=6272
output_dram_offset=10880
seed=42
```

---

## 5. 驗證工具鏈

### 5.1 Python 套件結構

```
python/hybridacc_verify/
├── __init__.py
├── gen/                        # 測試資料生成
│   ├── __init__.py
│   ├── gen_test_dram.py        # DRAM 映像 + golden (conv1x1/conv3x3/gemm)
│   ├── pe_gen.py               # PE level 測試資料
│   ├── noc_gen.py              # NoC level 測試資料
│   └── cluster_gen.py          # Cluster level 測試資料
├── check/                      # 驗證比較
│   ├── __init__.py
│   ├── comparator.py           # fp16 逐元素比對 (rtol/atol/CSV)
│   └── compare_golden.py       # DRAM output golden 驗證 (cosine sim)
└── model/                      # Golden reference 模型
    ├── __init__.py
    ├── conv.py                 # golden_conv2d, golden_conv1d
    └── gemm.py                 # golden_gemm
```

### 5.2 gen_test_dram 生成流程

```python
# 自動偵測運算類型
for each op in workload.ops:
    if op.type == "conv2d_3x3":
        weight, input → fp16_conv2d_golden() → output
    elif op.type == "conv2d_1x1":
        weight, input → fp16_conv2d_golden() → output  (KH=KW=1)
    elif op.type == "gemm":
        A, B → fp16_gemm_golden() → C

# Multi-layer: 逐層串接
layer[0].output → layer[1].input → ... → final_output
```

### 5.3 compare_golden 驗證邏輯

1. 解析 `golden_meta.txt` 取得 `dram_output_base` 與 `golden_output_bytes`
2. 從 sparse DRAM dump (`dram_init.bin.out`) 擷取對應區段
3. 與 `golden_output.bin` 比對：
   - Cosine similarity (主要指標)
   - MSE、Max absolute diff (輔助指標)
   - Exact match count (bit-exact)
4. 判定：cosine_sim ≥ threshold → PASS

### 5.4 comparator 逐元素比對

更細緻的比對，支援：
- **rtol/atol** 容忍度：`|sim - exp| > atol + rtol * |exp|` → mismatch
- **NaN 處理**：雙方皆 NaN → 忽略；單方 NaN → fail
- **CSV 輸出**：包含 hex, sign, exponent, mantissa, float, diff

---

## 6. 端到端資料流

### 6.1 Conv2D 3×3 範例

```
                    hacc-compile
                         │
workload.yaml ──────────────────▶ firmware.elf
     │                                  │         hardware_ir.json
     │                                  │              │
     ▼                                  │              ▼
gen_test_dram ◀────────────────────────────────────────┘
     │
     ├── dram_init.bin   (weight @ 0x80000000, input @ 0x80001200)
     ├── golden_output.bin
     └── golden_meta.txt
              │
              ▼
        hybridacc-sim ◀── firmware.elf + dram_init.bin
              │
              └── dram_init.bin.out  (output @ 0x80002A80)
                       │
                       ▼
              compare_golden
                       │
                  PASS / FAIL
```

### 6.2 Multi-layer 範例 (conv3x3 → conv1x1)

```
gen_test_dram:
  Layer 0 (conv3x3): random input + weight → golden_mid
  Layer 1 (conv1x1): golden_mid + weight2 → golden_output
  Write: input + weight0 + weight1 → dram_init.bin
  Write: golden_output → golden_output.bin

hybridacc-sim:
  firmware executes Layer 0 → DMA store mid → Layer 1 → DMA store output

compare_golden:
  dram_init.bin.out[output_region] vs golden_output.bin → cosine sim
```

---

## 7. 新增運算類型

### 7.1 新增 Workload 定義

在 `design/hybridacc-cc/example/` 中建立 YAML，定義新的 `ops[].type`。

### 7.2 編譯器支援

1. `frontend.py` — 新增 type 驗證
2. `lowering.py` — 實作 tiling 與 AGU mapping
3. `codegen.py` / templates — 產生對應的 DMA loop

### 7.3 測試資料生成

在 `hybridacc_verify/gen/gen_test_dram.py` 中的 `_compute_layer_golden()` 加入新運算的 golden 計算邏輯。

### 7.4 驗證

compare_golden 不需修改 — 它只比較 DRAM binary, 與運算類型無關。

---

## 8. 除錯指南

### 8.1 韌體 crash / 不結束

```bash
# Core pipeline trace
scripts/fast_entry/hybridacc_sim.sh run-task <dir> --core-debug --max-cycles 100000
```

觀察 PC 是否在同一位址迴圈，或跳入 trap handler。

### 8.2 DMA 資料錯誤

```bash
# 開啟 DMA loopback 檢查
scripts/fast_entry/hybridacc_sim.sh run-task <dir> --dma-check
```

### 8.3 Output 全零

可能原因：
- DMA store 未正確觸發 → 檢查 firmware_ops.c 中的 DMA store 呼叫
- DRAM mirror 未開啟 → 確認 `--mirror` 參數
- Output 位址計算錯誤 → 比對 `golden_meta.txt` 與 firmware 中的 DRAM offset

### 8.4 Cosine sim 低但非零

- fp16 精度累積誤差 → 正常範圍 0.95–1.0 (大型 tensor)
- Weight/Input layout 不匹配 → 檢查 NHWC vs OIHW 排列

### 8.5 使用 Debug build

```bash
scripts/fast_entry/hybridacc_sim.sh build_debug
scripts/fast_entry/hybridacc_sim.sh run-task <dir> --max-cycles 50000
# 觀察 sim.log 中的 [DBG] 訊息
```

### 8.6 逐元素差異分析

```bash
uv run python -m hybridacc_verify.check.comparator \
    --sim output/actual.bin \
    --expected output/golden.bin \
    --dump-csv diff_report.csv
```

檢查 CSV 中 `mismatch=1` 的行，觀察 exponent/mantissa 差異模式。
