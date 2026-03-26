# HybridAcc-CC：AI 模型編譯器概覽

> 版本：v0.1-draft
> 狀態：設計文件初稿（Implementation-Ready Spec）
> 適用位置：`design/hybridacc-cc/`

---

## 1. 編譯器定位

`hybridacc-cc` 是 HybridAcc 加速器平台的 **AI 模型編譯器**。它接收使用者定義的高層運算描述（Workload YAML），經過多階段編譯流程，最終輸出一個可直接由 `cc_section_loader` 載入到 Core Controller 並執行的 **RV32I_zicsr ELF** 韌體映像。

本編譯器實作為 Python 套件 `hybridacc-cc`，整合進現有的 `hybridacc_tools` Python monorepo（`pyproject.toml` 中的 `[project.scripts]`），以 CLI 命令 `hacc-compile` 對外提供服務。

### 1.1 為何需要此編譯器

在沒有 `hybridacc-cc` 之前，開發者需要：

1. 手動計算 AGU 暫存器值（base addr、iter、stride、tag）。
2. 手動選取正確的 PE kernel template 並計算其 patch 參數。
3. 手動撰寫 C/Assembly firmware，包含完整的 MMIO 指令序列。
4. 手動呼叫 RISC-V GCC cross-compiler 編譯 ELF。
5. 手動在 linker script 中安排各 section 位址。

`hybridacc-cc` 將上述所有手動步驟自動化，讓使用者只需提供「模型運算描述」即可得到可執行的 ELF。

### 1.2 設計目標

1. **正確性第一**：編譯產出的韌體必須在 SystemC ESL simulator 上產生 bit-exact 結果。
2. **可審查性**：每個編譯階段都必須可獨立 dump 中間產物，便於人工審查與除錯。
3. **增量擴充**：初版支援 conv2d 1×1、conv2d 3×3、gemm 三種算子；架構須允許未來新增算子而不需重構核心管線。
4. **相容既有工具鏈**：PE program 透過 `ha-asm` / `ha-package` 工具鏈處理；RV32I firmware 透過標準 `riscv32-unknown-elf-gcc` 編譯。
5. **確定性輸出**：相同輸入永遠產生 byte-level 相同的 ELF。

### 1.3 設計非目標

1. 不做通用 DNN 框架整合（不接 ONNX/TFLite）。
2. 不做自動 tiling search / auto-tuner。
3. 不做多 Core Controller 的 multi-chip 排程。
4. 不做 PE program 的自動生成（PE kernel 來自人工撰寫的 template）。
5. 不做 NLU 排程（NLU 配置在本版中由韌體 MMIO 直接控制，但 lowering 不在本版 scope）。

---

## 2. 系統上下文

```
                        Workload YAML
                             │
                             v
   ┌─────────────────────────────────────────────┐
   │              hybridacc-cc                   │
   │                                             │
   │  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
   │  │ Frontend │→ │ Lowering │→ │ CodeGen  │   │
   │  │ (YAML    │  │ (OpIR→   │  │ (IR→C    │   │
   │  │  parse)  │  │  HwIR)   │  │  source) │   │
   │  └──────────┘  └──────────┘  └────┬─────┘   │
   │                                   │         │
   │                    ┌──────────────┤         │
   │                    v              v         │
   │           ┌──────────────┐  ┌──────────┐    │
   │           │ PE Payload   │  │ ELF Link │    │
   │           │ (ha-package) │  │ (GCC +   │    │
   │           └──────┬───────┘  │  ld.ld)  │    │
   │                  │          └────┬─────┘    │
   │                  └───────┬───────┘          │
   │                          v                  │
   │                   ┌─────────────┐           │
   │                   │ Final ELF   │           │
   │                   │ (.elf)      │           │
   │                   └─────────────┘           │
   └─────────────────────────────────────────────┘

   外部依賴：
   - riscv32-unknown-elf-gcc (交叉編譯)
   - ha-asm / ha-package (PE ISA 工具鏈)
```

### 2.1 輸入

| 輸入 | 格式 | 來源 | 說明 |
|------|------|------|------|
| Workload YAML | `.yaml` | 使用者 | 描述模型運算圖、張量形狀、硬體配置 |
| PE kernel templates | `.asm` / `.json` | `design/hybridacc-cc/kernel/` | 預先撰寫的 PE 程式模板 |
| Hardware config | YAML 內嵌或獨立檔 | 使用者/預設 | 硬體參數（cluster 數、PE 數、SPM 大小等） |

### 2.2 輸出

| 輸出 | 格式 | 消費者 | 說明 |
|------|------|--------|------|
| RV32I ELF | `.elf` | `cc_section_loader` / simulator | 包含 firmware + PE payload 的完整映像 |
| 編譯報告 | `.json` | 開發者 | 每 layer 的 AGU 配置、tiling 參數、cycle 預估 |
| 中間 C 原始碼 | `.c` / `.h` | 開發者（debug 用） | 生成的韌體原始碼 |

### 2.3 與硬體的對應關係

`hybridacc-cc` 產出的 ELF 將被載入到 HACC Core Controller，其中：

- `.hacc.core` section → `cc_isram`（MCU instruction SRAM，base `0x0000_0000`，16 KB）
- `.hacc.pe` section → `cc_data_sram` descriptor/payload ABI region（base `0x1000_0000`，64 KB 內）
- `.hacc.scan` section → `cc_data_sram` 同上
- `.hacc.dma` section → `cc_data_sram` 同上

MCU firmware 於 runtime 透過 MMIO load/store 完成：

1. 設定 `CLUSTER_MASK_LO/HI` 選定目標 cluster
2. 透過 broadcast/unicast window 配置每個 cluster 的 SPM、HDDU AGU、NoC scan-chain
3. 透過 NoC command 載入 PE program
4. 啟動 HDDU 並等待完成中斷
5. 視需要透過 DMA 搬移資料

---

## 3. 硬體基礎（快速參考）

以下為編譯器需要理解的核心硬體抽象。完整規格請參考各 `design/hybridacc-ESL/doc/*.md`。

### 3.1 Core Address Map

| Base | End | Size | Region |
|------|-----|------|--------|
| `0x0000_0000` | `0x0000_3FFF` | 16 KB | I-SRAM（`.hacc.core`） |
| `0x1000_0000` | `0x1000_FFFF` | 64 KB | Data-SRAM（descriptor/payload/event/debug ABI） |
| `0x2000_0000` | `0x2000_0FFF` | 4 KB | Local control MMIO（cluster mask, status） |
| `0x2000_1000` | `0x2000_1FFF` | 4 KB | DMA MMIO |
| `0x2000_1800` | `0x2000_18FF` | 256 B | DMA stream window（write-only push FIFO） |
| `0x0C00_0000` | `0x0C00_FFFF` | 64 KB | PLIC MMIO |
| `0x4000_0000` | `0x400F_FFFF` | 1 MB | Cluster unicast MMIO（stride 0x10000/cluster） |
| `0x5000_0000` | `0x5000_FFFF` | 64 KB | Cluster masked-broadcast MMIO |
| `0x6000_0000` | `0x6000_FFFF` | 64 KB | NLU MMIO（stride 0x1000/NLU） |

### 3.2 Cluster 內部 AHB Window

每個 cluster 內部的 AHB command 位址空間：

| Offset 區間 | 功能 |
|-------------|------|
| `0x0000 ~ 0x00FF` | SPM config / PMU |
| `0x1000 ~ 0x1FFF` | HDDU MMIO passthrough |
| `0x2000 ~ 0x20FF` | NoC command sideband |

Cluster unicast 公式：`cluster_n_base = 0x4000_0000 + n * 0x0001_0000`

Cluster broadcast 公式：`cluster_bcast_addr = 0x5000_0000 + cluster_local_offset`

### 3.3 HDDU AGU 暫存器（per-bank offset）

HDDU 有 4 個 AGU bank：PS(0)、PD(1)、PLI(2)、PLO(3)，bank stride = `0x100`。

| Offset | Name | 說明 |
|--------|------|------|
| `0x00` | `REG_BASE_ADDR` | Base address low 32-bit |
| `0x04` | `REG_BASE_ADDR_H` | Base address high（保留，寫 0） |
| `0x08` | `REG_ITER01` | `iter0[15:0]`, `iter1[31:16]` |
| `0x0C` | `REG_ITER23` | `iter2[15:0]`, `iter3[31:16]` |
| `0x10` | `REG_STRIDE0` | stride0 |
| `0x14` | `REG_STRIDE1` | stride1 |
| `0x18` | `REG_STRIDE2` | stride2 |
| `0x1C` | `REG_STRIDE3` | stride3 |
| `0x20` | `REG_CTRL` | bit0=start, bit3=ultra |
| `0x28` | `REG_LANE_CFG` | Lane config |
| `0x40` | `REG_TAG_BASE` | Tag base |
| `0x44` | `REG_TAG_STRIDE0` | Tag stride inner |
| `0x48` | `REG_TAG_STRIDE1` | Tag stride outer |
| `0x4C` | `REG_TAG_CTRL` | Tag level/source control |
| `0x54` | `REG_MASK_CFG` | Descriptor mask |

HDDU Global 暫存器（offset 相對於 HDDU window base `0x1000`）：

| Offset | Name | 說明 |
|--------|------|------|
| `0x800` | `HDDU_CTRL` | bit0: soft-reset, bit1: start_all, bit2: stop_all |
| `0x804` | `HDDU_STATUS` | bit1: any_busy, bit4: err |
| `0x808` | `PLANE_EN` | bit0~3: PS/PD/PLI/PLO enable |
| `0x80C` | `PLANE_MODE` | 軟體定義模式旗標 |

### 3.4 NoC Command

NoC command 透過 cluster AHB `0x2000` 寫入，格式為 32-bit packed word：

```
[31:4] = param
[3:0]  = command
```

| Command | Value | 說明 |
|---------|-------|------|
| `CMD_RESET` | 0 | Reset PE array |
| `CMD_INIT` | 1 | Initialize with config |
| `CMD_LOAD_PROGRAM` | 2 | Load one 16-bit instruction |
| `CMD_STOP_PE` | 3 | Stop PE execution |
| `CMD_START_PE` | 4 | Start PE execution |
| `CMD_NOC_SCAN_CHAIN` | 8 | Configure PE router scan chain |

#### Scan-chain word 格式

```
bit[30]    = enable
bit[29:28] = route_mode (PERouterMode)
bit[27:22] = plo_id[5:0]
bit[21:16] = pli_id[5:0]
bit[15:10] = pd_id[5:0]
bit[9:4]   = ps_id[5:0]
bit[3:0]   = CMD_NOC_SCAN_CHAIN (8)
```

`PERouterMode` 定義：

| Value | Name | 意義 |
|-------|------|------|
| 0 | `PLI_FROM_LN_PLO_TO_LN` | PLI 從 local, PLO 到 local |
| 1 | `PLI_FROM_BUS_PLO_TO_LN` | PLI 從 bus, PLO 到 local |
| 2 | `PLI_FROM_LN_PLO_TO_BUS` | PLI 從 local, PLO 到 bus |
| 3 | `PLI_FROM_BUS_PLO_TO_BUS` | PLI 從 bus, PLO 到 bus |

#### Load-program word 格式

```
bit[31:16] = instruction[15:0] (16-bit PE instruction)
bit[15:4]  = im_addr[11:0] (byte address in PE instruction memory)
bit[3:0]   = CMD_LOAD_PROGRAM (2)
```

### 3.5 PE ISA 概要

PE 指令為 16-bit 固定長度：

```
15        6 5 4  3 2  1 0
[ payload ][f1][f2][opcode][LE]
```

PE program 以 JSON template 格式管理，包含：

- `template_name`：模板名稱
- `parameters[]`：可 patch 的參數清單（含 name、default、index）
- `patches[]`：指令 patch 點（offset → param_index 對映）
- `instructions[]`：完整指令序列（含 word、disasm）

現有 kernel templates：

| Template | 適用算子 | 參數 |
|----------|----------|------|
| `conv1d_k3c4s1` | conv2d 3×3（kernel_size=3, channels_per_cycle=4） | KERNEL_DMA_LEN, OUTPUT_WINDOW_CNT_MINUS_ONE, KERNEL_COUNT, KERNEL_LOOP_INNER, KERNEL_LOOP_OUTER |
| `conv1d_k1c12s1` | conv2d 1×1（kernel_size=1, channels_per_cycle=12） | 同上 |
| `gemm` | GEMM | KERNEL_DMA_STORE_LEN, KERNEL_DMA_LOAD_LEN, INPUT_DIM, OUTPUT_DIM, PSUM_COUNT, NUM_OF_KERNEL_SETS, NUM_OF_N_TILES, NUM_OF_M_TILES, K_TILE_DIM |

### 3.6 Local Control MMIO

| Offset（from `0x2000_0000`） | Name | 說明 |
|------|------|------|
| `0x0000` | `CORE_STATUS` | Core 狀態 |
| `0x0004` | `CLUSTER_MASK_LO` | Cluster mask bits [31:0] |
| `0x0008` | `CLUSTER_MASK_HI` | Cluster mask bits [63:32]（保留） |

---

## 4. 支援的算子

### 4.1 Conv2D 3×3

- **語義**：標準 2D 卷積，kernel size = 3×3
- **PE kernel**：`conv1d_k3c4s1`（channels_per_cycle = 4）
- **輸入張量**：`[N, H, W, C_in]`（channels_last layout）
- **權重張量**：`[OC, KH=3, KW=3, C_in]`
- **輸出張量**：`[N, H_out, W_out, OC]`
- **限制**：`C_in` 必須為 4 的倍數；tile_ic=4, tile_oc=min(OC,16) 為固定 tile 維度

### 4.2 Conv2D 1×1

- **語義**：Pointwise convolution，kernel size = 1×1
- **PE kernel**：`conv1d_k1c12s1`（channels_per_cycle = 12）
- **輸入張量**：`[N, H, W, C_in]`
- **權重張量**：`[OC, KH=1, KW=1, C_in]`
- **輸出張量**：`[N, H_out, W_out, OC]`
- **限制**：`C_in` 必須為 12 的倍數；tile_ic=12, tile_oc=min(OC,16) 為固定 tile 維度

### 4.3 GEMM

- **語義**：General Matrix Multiplication，`C = A × B`
- **PE kernel**：`gemm`
- **輸入矩陣 A**：`[M, K]`
- **輸入矩陣 B**：`[K, N]`
- **輸出矩陣 C**：`[M, N]`
- **限制**：tiling 細節由 lowering 階段決定

---

## 5. 編譯流程概覽

完整的編譯流程分為 5 個主要階段，每個階段都有獨立的中間表示（IR）：

```
Stage 0: Frontend (YAML Parse)
   輸入: workload.yaml
   輸出: WorkloadIR (Python dataclass)
         ↓
Stage 1: Operator Lowering
   輸入: WorkloadIR
   輸出: HardwareIR (per-layer config)
         ↓
Stage 2: Code Generation
   輸入: HardwareIR
   輸出: firmware.c + firmware.h (C source)
         ↓
Stage 3: PE Payload Preparation
   輸入: HardwareIR (PE template refs + params)
   輸出: pe_payload.pkg + pe_payload.h (via ha-package)
         ↓
Stage 4: ELF Compilation & Linking
   輸入: firmware.c + pe_payload.h + linker.ld
   輸出: output.elf
```

每個 Stage 的詳細規格請分別參閱：

- [01_CompilationPipeline.md](01_CompilationPipeline.md)
- [02_OperatorLowering.md](02_OperatorLowering.md)
- [03_CodeGeneration.md](03_CodeGeneration.md)
- [04_ELF_Layout.md](04_ELF_Layout.md)
- [05_UserGuide.md](05_UserGuide.md)

---

## 6. 核心資料結構

### 6.1 WorkloadIR

```python
@dataclass
class TensorDesc:
    name: str            # e.g., "input0"
    shape: List[int]     # e.g., [1, 14, 14, 16]
    dtype: str           # "fp16" (唯一支援)
    layout: str          # "NHWC" or "MK" / "KN" / "MN"

@dataclass
class OpDesc:
    op_type: str         # "conv2d_3x3" | "conv2d_1x1" | "gemm"
    name: str            # 使用者自定義名稱
    inputs: List[TensorDesc]
    outputs: List[TensorDesc]
    attrs: Dict[str, Any]  # stride, padding 等

@dataclass
class HardwareDesc:
    num_clusters: int    # 1..16
    num_pes: int         # per cluster
    num_bus: int         # per cluster
    spm_banks_per_group: int  # SPM banks per group（參照 SPM.md）
    spm_bank_depth: int       # words per bank（每 word = 8 bytes）
    dram_base: int       # external DRAM base

    @property
    def group_capacity(self) -> int:
        """Per-group linear capacity (bytes) = banks_per_group × bank_depth × 8"""
        return self.spm_banks_per_group * self.spm_bank_depth * 8

@dataclass
class WorkloadIR:
    name: str
    hardware: HardwareDesc
    ops: List[OpDesc]    # 按依賴順序排列
```

### 6.2 HardwareIR

```python
@dataclass
class AguBankConfig:
    base_addr: int
    iter0: int; iter1: int; iter2: int; iter3: int
    stride0: int; stride1: int; stride2: int; stride3: int
    lane_cfg: int
    tag_base: int; tag_stride0: int; tag_stride1: int; tag_ctrl: int
    mask_cfg: int
    ultra: bool

@dataclass
class ScanChainEntry:
    ps_id: int
    pd_id: int
    pli_id: int
    plo_id: int
    route_mode: int   # PERouterMode value
    enable: bool

@dataclass
class PeProgramRef:
    template_name: str         # e.g., "conv1d_k3c4s1_template"
    params: Dict[str, int]     # template parameter overrides

@dataclass
class HdduConfig:
    plane_en: int              # bitmask of enabled planes
    plane_mode: int            # software-defined mode flag

@dataclass
class SpmGroupLayout:
    """Per-group SPM address layout (see SPM.md)."""
    ping_base: int       # ping buffer 起始地址 (group-local byte offset)
    pong_base: int       # pong buffer 起始地址
    size: int            # 單個 buffer 的大小 (bytes)
    pingpong: bool = True    # 強制 ping-pong（always True）
    spm_mode: str = "linear" # "linear" or "parallel"（parallel 時 ping/pong 位於 parallel 地址範圍）

@dataclass
class SpmPerGroupLayout:
    """4 Groups 各自的地址配置"""
    ps: SpmGroupLayout   # Group 0 (weight / A)
    pd: SpmGroupLayout   # Group 1 (activation / B)
    pli: SpmGroupLayout  # Group 2 (partial-sum in)
    plo: SpmGroupLayout  # Group 3 (partial-sum out)

@dataclass
class TilingResult:
    """Tiling 搜尋結果"""
    loop_dims: List[str]          # e.g., ["oc_tile", "h_tile", "w_tile", "ic_tile"]
    loop_bounds: Dict[str, int]   # e.g., {"oc_tile": 1, "h_tile": 2, "w_tile": 1, "ic_tile": 1}
    total_waves: int              # product of all loop bounds
    reduction_dims: List[str]     # reduction 維度（Conv2D: ["ic_tile"], GEMM: ["k_tile"]）

@dataclass
class ClusterMapping:
    """Multi-cluster spatial tile 分配"""
    active_clusters: List[int]    # 參與計算的 cluster IDs
    split_dim: Optional[str]      # 切分維度（e.g. "h_tile", "oc_tile", "n_tile"）
    shared_tensor: Optional[str]  # 共用張量（e.g. "weight", "input", "A"）
    tile_ranges: Dict[int, Tuple[int, int]]  # cluster_id → (tile_start, tile_end)

@dataclass
class LayerHwConfig:
    name: str
    op_type: str

    # Cluster config
    target_cluster_mask: int         # broadcast mask
    spm_config_map: int              # SPM port → group mapping (default: 0xE4)

    # HDDU
    hddu: HdduConfig
    agu_ps: AguBankConfig            # AGU bank 0 (weight / A) → Group 0
    agu_pd: AguBankConfig            # AGU bank 1 (activation / B) → Group 1
    agu_pli: AguBankConfig           # AGU bank 2 (partial-sum in) → Group 2
    agu_plo: AguBankConfig           # AGU bank 3 (partial-sum out) → Group 3

    # NoC
    scan_chain: List[ScanChainEntry] # 反序 shift
    pe_program: PeProgramRef

    # SPM per-group layout（NEW: 取代 flat layout）
    spm_layout: SpmPerGroupLayout

    # Tiling（NEW）
    tiling: Optional[TilingResult]   # None 表示整個 layer fit 進單一 wave

    # Cluster mapping（NEW）
    cluster_mapping: Optional[ClusterMapping]  # None 表示單 cluster

    # DMA descriptors（含 ping-pong 目標地址）
    dma_transfers: List[Dict]

@dataclass
class HardwareIR:
    workload_name: str
    hardware: HardwareDesc
    layers: List[LayerHwConfig]
```

---

## 7. 外部依賴

| 依賴 | 版本要求 | 用途 |
|------|---------|------|
| Python | ≥ 3.10 | 編譯器主體 |
| PyYAML | ≥ 6.0 | YAML 前端解析 |
| pyelftools | ≥ 0.29 | ELF 檢查（debug 用） |
| riscv32-unknown-elf-gcc | ≥ 12.0 | RV32I 交叉編譯 |
| ha-asm | local build | PE 組譯器 |
| ha-package | local build | PE 打包工具 |
| Jinja2 | ≥ 3.0 | C source template rendering |

---

## 8. 目錄結構

```
design/hybridacc-cc/
├── doc/                              # ← 本文件所在處
│   ├── 00_Overview.md
│   ├── 01_CompilationPipeline.md
│   ├── 02_OperatorLowering.md
│   ├── 03_CodeGeneration.md
│   ├── 04_ELF_Layout.md
│   └── 05_UserGuide.md
├── example/
│   └── conv.yaml                     # 範例 workload
├── kernel/
│   ├── template/                     # PE assembly templates
│   │   ├── conv1d_k3c4s1.asm
│   │   ├── conv1d_k1c12s1.asm
│   │   └── gemm.asm
│   ├── json/                         # 已組譯的 JSON metadata
│   │   ├── conv1d_k3c4s1.json
│   │   ├── conv1d_k1c12s1.json
│   │   └── gemm.json
│   ├── bin/                          # 預編譯 binary
│   │   ├── conv1d_k3c4s1.bin
│   │   ├── conv1d_k1c12s1.bin
│   │   └── gemm.bin
│   └── build.mk                     # Kernel 建構 Makefile
├── scripts/
│   └── (build / test scripts)
├── src/                              # (未來) Python 實作
│   └── hybridacc_cc/
│       ├── __init__.py
│       ├── frontend.py               # Stage 0: YAML parser
│       ├── lowering.py               # Stage 1: Operator lowering
│       ├── codegen.py                # Stage 2: C code generation
│       ├── pe_payload.py             # Stage 3: PE payload prep
│       ├── elf_builder.py            # Stage 4: ELF link orchestration
│       ├── ir.py                     # IR dataclass definitions
│       └── templates/                # Jinja2 C templates
│           ├── firmware_main.c.j2
│           ├── firmware_layer.c.j2
│           └── linker.ld.j2
└── tests/
    └── (unit tests)
```

---

## 9. 文件導覽

| 文件 | 內容 | 適用讀者 |
|------|------|---------|
| **00_Overview.md**（本文件） | 整體架構、定位、資料結構 | 所有人 |
| **01_CompilationPipeline.md** | 五個編譯階段的流程、輸入輸出、錯誤處理 | compiler 開發者 |
| **02_OperatorLowering.md** | 每個算子如何 lowering 到 HardwareIR | 演算法開發者 |
| **03_CodeGeneration.md** | C 原始碼生成規則、MMIO 指令序列、template | firmware 開發者 |
| **04_ELF_Layout.md** | ELF section 設計、linker script、memory map | toolchain 維護者 |
| **05_UserGuide.md** | 端到端操作教學、CLI 用法、範例 | 所有使用者 |

---

## 10. 術語表

| 術語 | 定義 |
|------|------|
| AGU | Address Generate Unit，HDDU 內的地址產生器，共 4 bank（PS/PD/PLI/PLO） |
| HDDU | Hybrid Data Deliver Unit，cluster 內的資料搬運核心 |
| SPM | Scratchpad Memory，cluster 內的 4-port SRAM |
| NoC | Network on Chip，PE array 的互連網路 |
| PE | Processing Element，16-bit SIMD 運算單元 |
| Scan-chain | NoC 中每個 PE router 的配置暫存器鏈，以反序 shift 載入 |
| MCU | Micro Controller Unit，即 `cc_core_mcu`（RV32I_zicsr 5-stage pipeline） |
| Broadcast | 透過 `CLUSTER_MASK` 對多個 cluster 同時發送相同 MMIO transaction |
| Template patch | PE program JSON 中的 `patches[]`，描述哪些指令 word 的 payload 需在 runtime 依參數替換 |
| Workload YAML | 使用者提供的運算描述檔，定義 layer 序列與張量形狀 |
| HardwareIR | 編譯器內部的硬體配置中間表示，包含所有 AGU/scan-chain/PE 配置 |
