# HybridAcc-CC：Code Generation 詳細規格

> 前置閱讀：[00_Overview.md](00_Overview.md)、[02_OperatorLowering.md](02_OperatorLowering.md)

---

## 1. 總覽

Code Generation（Stage 2）負責將 `HardwareIR` 轉換為可在 `cc_core_mcu`（RV32I_zicsr 5-stage pipeline）上執行的 C 語言韌體原始碼。

### 1.1 設計目標

1. **極致 code size**：I-SRAM 僅 16 KB，韌體必須以 data-driven 架構取代 per-layer code 展開
2. **Data Movement Hiding**：DMA 搬運時間必須隱藏在 cluster 計算期間，透過 ping-pong 雙緩衝實現 DMA/compute overlap
3. **Runtime PE Patching**：PE template 保留**一份未修改的原始 binary**，patch 在 firmware runtime 執行
4. **Compile-time + Runtime 協同計算**：per-wave 的 DMA 地址、AGU base、SPM mapping 不再以 O(waves) 的靜態陣列儲存，而是由 compiler 萃取 O(1) 的 tiling 常數（base/stride），firmware 在 runtime 以乘加動態推導

### 1.2 生成的 C 程式碼特徵

1. **純 C**（C11 標準），不使用 C++
2. **`-nostdlib -ffreestanding`**：不連結標準庫，不依賴 OS
3. **所有硬體互動都透過 MMIO volatile pointer dereference**
4. **使用 Jinja2 模板引擎**從 HardwareIR 產生最終 C 原始碼

---

## 2. 生成的檔案結構

| 檔案 | 角色 | 內容 |
|------|------|------|
| `firmware_hw.h` | 硬體抽象 | MMIO 位址常數、helper inline function（含 DMA） |
| `firmware_payload.h` | 靜態資料 | PE template（未 patch 原始 binary）、pre-encoded scan chain、patch 描述表 |
| `firmware_data.c` | Layer 參數 | 每個 layer 的配置 struct（AGU, HDDU, TilingParams compile-time 常數）放在 `.rodata`，O(1) per layer |
| `firmware_ops.c` | 共用函式 | Runtime patch, AGU 配置, scan chain 發送, PE 載入, tiling loop, DMA 控制 |
| `firmware_main.c` | 主程式入口 | `_start`、trap handler、layer 迴圈呼叫 |
| `linker.ld` | 連結腳本 | Memory region 定義與 section 配置 |

### 2.1 記憶體配置策略

```
I-SRAM (16 KB)                     Data-SRAM (64 KB)
┌─────────────────────┐            ┌─────────────────────┐
│ .text.start (entry) │            │ .rodata             │ ← layer config tables
│ .text (firmware_ops)│            │   pe_template[]     │
│   run_layer()       │            │   scan_chain[]      │
│   run_loop_tiling() │            │   patch_desc[]      │
│   cfg_agu_bank()    │            │   layer_cfgs[]      │ ← 含 TilingParams
│   load_pe_program() │            │     (O(1) per layer │
│   send_scan_chain() │            │      compile-time   │
│   dma_xfer_sync()   │            │      常數)          │
│   prefetch helpers   │            ├─────────────────────┤
│   pe_patch_runtime()│            │ .data / .bss        │ ← runtime state
│ .text (main)        │            │   patched_prog[]    │ ← runtime patch buf
│   main()            │            │   prefetch_queue[]  │ ← DMA prefetch state
│   trap_handler()    │            └─────────────────────┘
└─────────────────────┘
```

> **關鍵洞察**：所有 per-layer 差異表達為 Data-SRAM 的 `.rodata` table（含 `TilingParams` compile-time 常數），I-SRAM 僅保留一套通用 runtime 函式。.rodata 的大小為 **O(layers)**，不再與 wave 數量相關 — per-wave 配置由 `run_loop_tiling()` 在 runtime 以 `base + idx × stride` 動態計算。

---

## 3. `firmware_hw.h` — 硬體抽象層

### 3.1 MMIO 位址常數

```c
#ifndef FIRMWARE_HW_H
#define FIRMWARE_HW_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════
 * Core Address Map（見 Core.md §6.4）
 * ═══════════════════════════════════════════════════════ */

/* I-SRAM */
#define ISRAM_BASE          0x00000000u
#define ISRAM_SIZE          (16u * 1024u)

/* Data-SRAM */
#define DSRAM_BASE          0x10000000u
#define DSRAM_SIZE          (64u * 1024u)

/* Local Control MMIO */
#define LOCAL_MMIO_BASE     0x20000000u
#define REG_CORE_STATUS     (*(volatile uint32_t*)(LOCAL_MMIO_BASE + 0x0000u))
#define REG_CLUSTER_MASK_LO (*(volatile uint32_t*)(LOCAL_MMIO_BASE + 0x0004u))
#define REG_CLUSTER_MASK_HI (*(volatile uint32_t*)(LOCAL_MMIO_BASE + 0x0008u))

/* DMA MMIO */
#define DMA_MMIO_BASE       0x20001000u
#define DMA_REG_CTRL        (*(volatile uint32_t*)(DMA_MMIO_BASE + 0x000u))
#define DMA_REG_CLUSTER_MASK (*(volatile uint32_t*)(DMA_MMIO_BASE + 0x008u))
#define DMA_REG_WORD_COUNT  (*(volatile uint32_t*)(DMA_MMIO_BASE + 0x00Cu))
#define DMA_REG_ADDR        (*(volatile uint32_t*)(DMA_MMIO_BASE + 0x010u))
#define DMA_REG_ERROR       (*(volatile uint32_t*)(DMA_MMIO_BASE + 0x014u))
#define DMA_STREAM_BASE     0x20001800u

/* DMA Control/Status bits */
#define DMA_CTRL_START      (1u << 0)
#define DMA_STATUS_BUSY     (1u << 0)
#define DMA_STATUS_DONE     (1u << 1)
#define DMA_STATUS_ERROR    (1u << 2)

/* PLIC */
#define PLIC_BASE           0x0C000000u

/* Cluster Unicast MMIO (per-cluster stride = 0x10000) */
#define CLUSTER_UNICAST_BASE  0x40000000u
#define CLUSTER_STRIDE        0x00010000u

/* Cluster Broadcast MMIO */
#define CLUSTER_BCAST_BASE  0x50000000u

/* NLU MMIO (per-NLU stride = 0x1000) */
#define NLU_BASE            0x60000000u
#define NLU_STRIDE          0x00001000u

/* ═══════════════════════════════════════════════════════
 * Cluster Internal AHB Offsets
 * ═══════════════════════════════════════════════════════ */

/* SPM Config Window */
#define SPM_CONFIG_MAP      0x0000u
#define SPM_CONFIG_UPDATE   0x0004u

/* HDDU Window */
#define HDDU_BASE           0x1000u
#define AGU_BANK_STRIDE     0x0100u

/* AGU Bank Index */
#define AGU_PS              0u
#define AGU_PD              1u
#define AGU_PLI             2u
#define AGU_PLO             3u

/* AGU Per-Bank Register Offsets */
#define AGU_REG_BASE_ADDR   0x00u
#define AGU_REG_BASE_ADDR_H 0x04u
#define AGU_REG_ITER01      0x08u
#define AGU_REG_ITER23      0x0Cu
#define AGU_REG_STRIDE0     0x10u
#define AGU_REG_STRIDE1     0x14u
#define AGU_REG_STRIDE2     0x18u
#define AGU_REG_STRIDE3     0x1Cu
#define AGU_REG_CTRL        0x20u
#define AGU_REG_STATUS      0x24u
#define AGU_REG_LANE_CFG    0x28u
#define AGU_REG_TAG_BASE    0x40u
#define AGU_REG_TAG_STRIDE0 0x44u
#define AGU_REG_TAG_STRIDE1 0x48u
#define AGU_REG_TAG_CTRL    0x4Cu
#define AGU_REG_MASK_CFG    0x54u

/* AGU register count per bank（data-driven 迴圈用） */
#define AGU_NUM_REGS        15u

/* HDDU Global Register (offset from HDDU window) */
#define HDDU_CTRL           0x800u
#define HDDU_STATUS         0x804u
#define HDDU_PLANE_EN       0x808u
#define HDDU_PLANE_MODE     0x80Cu

/* NoC Command Window */
#define NOC_CMD             0x2000u

/* ═══════════════════════════════════════════════════════
 * NoC Command Encoding
 * ═══════════════════════════════════════════════════════ */

#define NOC_CMD_RESET           0u
#define NOC_CMD_INIT            1u
#define NOC_CMD_LOAD_PROGRAM    2u
#define NOC_CMD_STOP_PE         3u
#define NOC_CMD_START_PE        4u
#define NOC_CMD_SCAN_CHAIN      8u

/* ═══════════════════════════════════════════════════════
 * Helper Inline Functions
 * ═══════════════════════════════════════════════════════ */

static inline void mmio_write32(uint32_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}

static inline uint32_t mmio_read32(uint32_t addr) {
    return *(volatile uint32_t*)addr;
}

/* Broadcast window write: writes to all clusters selected by CLUSTER_MASK */
static inline void bcast_write32(uint32_t cluster_offset, uint32_t val) {
    mmio_write32(CLUSTER_BCAST_BASE + cluster_offset, val);
}

/* Unicast window write: writes to specific cluster N */
static inline void unicast_write32(uint32_t cluster_id,
                                   uint32_t cluster_offset,
                                   uint32_t val) {
    mmio_write32(CLUSTER_UNICAST_BASE
                 + cluster_id * CLUSTER_STRIDE
                 + cluster_offset, val);
}

/* Set cluster mask for broadcast operations */
static inline void set_cluster_mask(uint32_t mask_lo, uint32_t mask_hi) {
    REG_CLUSTER_MASK_LO = mask_lo;
    REG_CLUSTER_MASK_HI = mask_hi;
}

/* ── AGU helpers ── */

static inline uint32_t agu_addr(uint32_t bank, uint32_t reg) {
    return HDDU_BASE + bank * AGU_BANK_STRIDE + reg;
}

static inline void bcast_agu_write(uint32_t bank,
                                   uint32_t reg,
                                   uint32_t val) {
    bcast_write32(agu_addr(bank, reg), val);
}

/* ── NoC command packing ── */

static inline uint32_t pack_noc_cmd(uint32_t cmd, uint32_t param) {
    return (param & 0xFFFFFFF0u) | (cmd & 0x0Fu);
}

/* pack_load_program: 將 PE IM 位址 + instruction word 打包為 NOC 指令
 * 複雜度低，保留為 inline helper（scan chain 已移至 compile-time pre-encode） */
static inline uint32_t pack_load_program(uint16_t im_addr_bytes,
                                         uint16_t inst16) {
    uint32_t p = 0;
    p |= ((uint32_t)(im_addr_bytes & 0xFFFu)) << 4;
    p |= ((uint32_t)(inst16 & 0xFFFFu)) << 16;
    return pack_noc_cmd(NOC_CMD_LOAD_PROGRAM, p);
}

/* ── DMA helpers ── */

static inline void dma_start(uint32_t dram_addr,
                             uint32_t spm_addr,
                             uint32_t word_count) {
    DMA_REG_ADDR = dram_addr;
    DMA_REG_WORD_COUNT = word_count;
    /* SPM 目標由 DMA stream window 指定 */
    mmio_write32(DMA_STREAM_BASE + 0x00u, spm_addr);
    DMA_REG_CTRL = DMA_CTRL_START;
}

static inline void dma_wait_done(void) {
    while (!(DMA_REG_CTRL & DMA_STATUS_DONE)) {
        /* busy wait */
    }
}

static inline int dma_is_done(void) {
    return (DMA_REG_CTRL & DMA_STATUS_DONE) != 0;
}

/* ── HDDU / NoC Status polling ── */

static inline void wait_all_clusters_hddu_done(uint32_t num_clusters) {
    for (uint32_t c = 0; c < num_clusters; c++) {
        uint32_t base = CLUSTER_UNICAST_BASE + c * CLUSTER_STRIDE;
        while (1) {
            uint32_t st = mmio_read32(base + HDDU_BASE + HDDU_STATUS);
            if (st & HDDU_STATUS_ERROR) return;
            if (st & HDDU_STATUS_DONE) break;
        }
    }
}

static inline void wait_all_clusters_noc_quiesced(uint32_t num_clusters) {
    for (uint32_t c = 0; c < num_clusters; c++) {
        uint32_t base = CLUSTER_UNICAST_BASE + c * CLUSTER_STRIDE;
        while (1) {
            uint32_t st = mmio_read32(base + NOC_STATUS);
            uint32_t required = NOC_STATUS_ALL_ACTIVE_PES_HALTED;
            uint32_t blocked = NOC_STATUS_ANY_ROUTER_PENDING_RESP
                             | NOC_STATUS_ANY_ROUTER_FIFO_NONEMPTY;
            if ((st & required) == required && (st & blocked) == 0u) break;
        }
    }
}

#endif /* FIRMWARE_HW_H */
```

### 3.2 設計說明

- 所有 MMIO 存取都透過 `volatile` 指標，確保 GCC 不會優化掉 side effect
- `bcast_write32` / `unicast_write32` 封裝了地址計算
- **`pack_scan_chain` 已移除**：scan chain 在 compile 階段就 pre-encode 成 `uint32_t[]` hex array，runtime 只需逐 word 寫入 NOC_CMD（見 §4.2）
- `pack_load_program` 保留：encoding 簡單（僅兩個 field），且每個 word 的 `im_addr_bytes` 不同，無法完全 pre-encode
- **DMA helpers 新增**：`dma_start()` / `dma_wait_done()` / `dma_is_done()` 封裝 DMA 控制暫存器操作
- **完成條件已拆開**：`wait_all_clusters_hddu_done()` 專看 HDDU DONE；`wait_all_clusters_noc_quiesced()` 專看 layer-tail 的 STOP_PE + quiesce barrier
- 位址常數完全對應 Core.md §6.4 的 address map

---

## 4. `firmware_payload.h` — 靜態資料（PE Template + Scan Chain + Patch 描述）

### 4.1 設計原則

| 原則 | 做法 |
|------|------|
| **PE template 只保留一份** | 同一 template（如 `conv1d_k3c4s1`）不論被多少 layer 使用，binary 只嵌入一次 |
| **Patch 在 runtime 執行** | Compile 只生成 patch descriptor（offset + value），firmware 在 runtime 動態套用 |
| **Scan chain compile-time encode** | `pack_scan_chain()` 的 6 個參數在 compile 階段 pre-encode 成 `uint32_t[]` hex array |
| **PE program 也可共用** | 若多個 layer 使用同一 template 且 patch 值不同，可共用 template binary |

### 4.2 Pre-encoded Scan Chain

Compile 階段使用 Python 的 `encode_scan_chain()` 將每個 PE 的 scan chain entry 預先編碼為 32-bit NOC command word。Runtime 只需呼叫 `send_noc_scan_chain()` 逐 word 寫入。

```c
#ifndef FIRMWARE_PAYLOAD_H
#define FIRMWARE_PAYLOAD_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════
 * Pre-encoded Scan Chain（compile-time generated）
 *
 * 每個 entry 已經是 pack_noc_cmd(NOC_CMD_SCAN_CHAIN, payload)
 * 的結果，反序排列（從最後一個 PE 到第一個 PE）。
 * 同一 topology 的 layer 可共用同一份 scan chain。
 * ═══════════════════════════════════════════════════════ */

/* topology: 48 PEs, 3 ports (conv/gemm standard chain) */
static const uint32_t noc_scan_chain_48pe[] = {
    /* PE 63 (bus 3, last): PLI_FROM_LN_PLO_TO_BUS, route_mode=2 */
    0x480FFFF8u,
    /* PE 62: PLI_FROM_LN_PLO_TO_LN, route_mode=0 */
    0x40F8F9F8u,
    /* ... (中間 PE 由 codegen 逐一 pre-encode) ... */
    /* PE 1: PLI_FROM_LN_PLO_TO_LN, route_mode=0 */
    0x40044058u,
    /* PE 0 (bus 0, first): PLI_FROM_BUS_PLO_TO_LN, route_mode=1 */
    0x50000018u,
};
#define NOC_SCAN_CHAIN_64PE_LEN  64
```

> **Compile-time Pre-encode 過程**（Python codegen）：
> ```python
> def encode_scan_chain(scan_chain: List[ScanChainEntry]) -> List[int]:
>     """Pre-encode scan chain entries to uint32 NOC command words."""
>     words = []
>     for entry in reversed(scan_chain):  # 反序
>         v = 0
>         v |= (entry.ps_id  & 0x3F) << 4
>         v |= (entry.pd_id  & 0x3F) << 10
>         v |= (entry.pli_id & 0x3F) << 16
>         v |= (entry.plo_id & 0x3F) << 22
>         v |= (entry.route_mode & 0x03) << 28
>         v |= (1 if entry.enable else 0) << 30
>         words.append((v & 0xFFFFFFF0) | (NOC_CMD_SCAN_CHAIN & 0x0F))
>     return words
> ```

### 4.3 PE Template Binary（未 Patch）

每個 PE template 只嵌入**一份原始的 instruction binary**（未經 patch），所有使用該 template 的 layer 共用此 array。

```c
/* ═══════════════════════════════════════════════════════
 * PE Template: conv1d_k3c4s1
 * Source: kernel/json/conv1d_k3c4s1.json
 * 此為 "未 patch" 原始碼，runtime 由 pe_patch_runtime() 套用 per-layer 參數
 * ═══════════════════════════════════════════════════════ */
static const uint16_t pe_tmpl_conv1d_k3c4s1[] = {
    0x004C, /* 0: SYSCTRL (CLEAR.P) */
    0x0004, /* 1: LOOPIN 1            ← patch offset 1 */
    0x0020, /* 2: SDMA.ADDR 0 */
    0x0048, /* 3: SDMA.LEN 1          ← patch offset 3 */
    0x0030, /* 4: SDMA.LOOP 1         ← patch offset 4 */
    /* ... 略 ... */
    0x001C, /* 35: HALT */
};
#define PE_TMPL_CONV1D_K3C4S1_LEN  36

/* PE Template: conv1d_k1c12s1 */
static const uint16_t pe_tmpl_conv1d_k1c12s1[] = { /* ... */ };
#define PE_TMPL_CONV1D_K1C12S1_LEN  32

/* PE Template: gemm */
static const uint16_t pe_tmpl_gemm[] = { /* ... */ };
#define PE_TMPL_GEMM_LEN  27
```

### 4.4 Patch 描述表

每個 layer 的 patch 需求以 compact descriptor 表示。Compile 階段已完成所有 N-1 encoding。

```c
/* ═══════════════════════════════════════════════════════
 * Patch Descriptor
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  offset;       /* instruction index within template */
    uint16_t encoded_val;  /* 10-bit payload (已含 N-1 encoding) */
} PePatchEntry;

/* Per-Layer Patch Data */
static const PePatchEntry patch_conv1[] = {
    { 1,  0},   /* LOOPIN:  KERNEL_LOOP_OUTER=1  → 1-1 = 0 */
    { 3, 191},  /* SDMA.LEN: KERNEL_DMA_LEN=192 → 192-1 = 191 */
    { 4,  0},   /* SDMA.LOOP: KERNEL_LOOP_INNER=1 → 1-1 = 0 */
    {12, 195},  /* LOOPIN:  OUTPUT_WINDOW_CNT_MINUS_ONE=195 */
    {15, 15},   /* SDMA.LEN: KERNEL_COUNT=16 → 16-1 = 15 */
};
#define PATCH_CONV1_LEN  5

static const PePatchEntry patch_fc1[] = { /* ... */ };
#define PATCH_FC1_LEN  4

#endif /* FIRMWARE_PAYLOAD_H */
```

### 4.5 Compile-time Patch Encoding（Python 端）

Codegen 負責將 `PeProgramRef.params` 轉換為 `PePatchEntry[]`，在 compile 階段完成所有 N-1 encoding：

```python
def generate_patch_entries(json_data: dict, params: Dict[str, int]) -> List[dict]:
    """Generate PePatchEntry descriptors for runtime patching."""
    param_defs = json_data["parameters"]
    param_values = [params.get(p["name"], p["default"]) for p in param_defs]

    entries = []
    for patch in json_data["patches"]:
        offset = patch["offset"]
        value = param_values[patch["param_index"]]

        # N-1 encoding detection (compile-time only)
        word = json_data["instructions"][offset]["dec"]
        opcode = (word >> 1) & 0x3
        func2 = (word >> 3) & 0x3

        if opcode == 0b10 and func2 == 0b00:           # LOOPIN
            encoded = value - 1
        elif opcode == 0b00 and func2 in (0b01, 0b10):  # xDMA.LEN / xDMA.LOOP
            encoded = value - 1
        else:
            encoded = value

        if encoded > 1023:
            raise ValueError(f"E_PATCH_OVERFLOW: encoded={encoded} at offset {offset}")
        entries.append({"offset": offset, "encoded_val": encoded & 0x3FF})
    return entries
```

### 4.6 注意事項

1. **N-1 編碼由 compile 處理**：Runtime patch 不需判斷指令類型，直接使用 `encoded_val`。
2. **LE bit 保留**：Runtime patch 取原始 template instruction 的低 6 bits（含 LE bit），只替換 bits[15:6]。
3. **Payload 上限**：10-bit payload 最大值為 1023。Compile 時驗證，報 `E_PATCH_OVERFLOW`。
4. **Template 唯一性**：同名 template 只嵌入一次。若 `conv1` 和 `conv2` 都用 `conv1d_k3c4s1`，共享同一 `pe_tmpl_conv1d_k3c4s1[]`。
5. **Scan chain 唯一性**：若多個 layer 使用相同 PE topology，共享同一 `noc_scan_chain_*[]`。

---

## 5. `firmware_data.c` — Layer 參數配置表

### 5.1 設計原則

所有 per-layer 差異以 **C struct + const table** 表達，放在 Data-SRAM 的 `.rodata` section。I-SRAM 中的通用 runtime function 透過 struct pointer 存取參數，而非 per-layer 展開。

Per-wave 的動態配置（AGU base address, SPM map, DMA DRAM/SPM address）不再以 O(waves) 的靜態陣列儲存，而是由 **compiler 萃取 O(1) 的 tiling 常數**（base/stride/loop bounds），firmware 在 runtime 以 multi-level nested for loop + 簡單乘加推導。此 **Compile-time + Runtime 協同計算**設計確保 .rodata 大小為 O(layers)，不受 wave 數量影響。

### 5.2 AGU Configuration Struct

```c
/* ═══════════════════════════════════════════════════════
 * AGU 暫存器值 — 15 個 register 的 compact struct
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    uint32_t regs[AGU_NUM_REGS];
    /* regs[0]  = BASE_ADDR
     * regs[1]  = BASE_ADDR_H
     * regs[2]  = ITER01
     * regs[3]  = ITER23
     * regs[4]  = STRIDE0
     * regs[5]  = STRIDE1
     * regs[6]  = STRIDE2
     * regs[7]  = STRIDE3
     * regs[8]  = CTRL
     * regs[9]  = LANE_CFG
     * regs[10] = TAG_BASE
     * regs[11] = TAG_STRIDE0
     * regs[12] = TAG_STRIDE1
     * regs[13] = TAG_CTRL
     * regs[14] = MASK_CFG
     */
} AguRegs;

/* AGU register offset LUT（cfg_agu_bank 迴圈使用） */
static const uint32_t agu_reg_offsets[AGU_NUM_REGS] = {
    AGU_REG_BASE_ADDR, AGU_REG_BASE_ADDR_H,
    AGU_REG_ITER01, AGU_REG_ITER23,
    AGU_REG_STRIDE0, AGU_REG_STRIDE1, AGU_REG_STRIDE2, AGU_REG_STRIDE3,
    AGU_REG_CTRL, AGU_REG_LANE_CFG,
    AGU_REG_TAG_BASE, AGU_REG_TAG_STRIDE0, AGU_REG_TAG_STRIDE1, AGU_REG_TAG_CTRL,
    AGU_REG_MASK_CFG,
};
```

### 5.3 Tiling Parameters（Compile-time 常數）

舊版設計中，每個 wave 的 config（AGU base, SPM map, DMA descriptors）以 `WaveConfig[]` 陣列完整展開儲存在 `.rodata`。當 tile 數量達到上千（大型 feature map 的 Conv2D 或 GEMM），資料佔用為 O(waves)，嚴重消耗 D-SRAM。

新版設計利用 **Compile-time + Runtime 協同計算**：Compiler 分析每個 wave 的參數與 loop index 的數學關係，萃取出 O(1) 的常數係數，firmware 在 runtime 透過簡單的乘加運算動態產生 per-wave 配置。

```c
/* ═══════════════════════════════════════════════════════
 * Tiling Loop Parameters — Compile-time 萃取的常數
 *
 * 所有 per-wave 的動態值（AGU base, DMA addr, SPM map）
 * 都能由 loop index + 以下常數透過簡單乘加推導。
 * Compiler 負責分析 tiling / SPM layout 後產出此 struct。
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    /* ── Loop 迭代上界 ── */
    uint16_t num_oc_tiles;
    uint16_t num_h_tiles;
    uint16_t num_w_tiles;
    uint16_t num_ic_tiles;  /* Conv2D: IC, GEMM: K, 若只有 1 表示不需 reduction */

    /* ── SPM Ping/Pong base address（per-group, 2 組）──
     * 已由 compiler 從 SPM layout 預算：
     *   Linear group:  ping=0, pong=half_group_capacity
     *   Parallel group: ping=parallel_ping_base, pong=parallel_pong_base  */
    uint32_t spm_ping[4];  /* [PS, PD, PLI, PLO] — DMA 使用的 SPM 地址 */
    uint32_t spm_pong[4];

    /* ── AGU Ping/Pong base address（per-bank）──
     * Linear group:  = spm_ping/pong（與 DMA 相同）
     * Parallel group: = parallel_ping_base / parallel_pong_base（AGU 存 parallel region 地址）*/
    uint32_t agu_ping[4];  /* [PS, PD, PLI, PLO] — AGU BASE_ADDR */
    uint32_t agu_pong[4];

    /* ── SPM Group Mapping（PLI/PLO 交換用）──
     * ic_tile_idx 為偶數時用 spm_map_even，奇數用 spm_map_odd
     * Conv2D/GEMM: spm_map_even=0xE4 (port2→g2, port3→g3)
     *              spm_map_odd =0xD8 (port2→g3, port3→g2)
     * 若 num_ic_tiles == 1，只使用 spm_map_even */
    uint8_t spm_map_even;
    uint8_t spm_map_odd;

    /* ── DRAM 基底地址 ── */
    uint32_t dram_weight_base;  /* Conv2D: weight tensor base / GEMM: B matrix base */
    uint32_t dram_input_base;   /* Conv2D: input activation base / GEMM: A matrix base */
    uint32_t dram_output_base;  /* Conv2D: output activation base / GEMM: C matrix base */

    /* ── DRAM Tile Stride（per tile dimension）──
     * 每個 tile index 增加 1 時，DRAM 地址的增量（bytes）
     * Compiler 從 tensor layout + tile 維度計算：
     *   stride = tile_size × element_size × packing_factor
     * Runtime 公式：dram_addr = base + Σ(tile_idx_i × stride_i)  */

    /*  PS (Weight / B matrix) */
    uint32_t dram_ps_oc_stride;  /* Conv2D: oc_tile_idx 增加 1 的 DRAM offset */
    uint32_t dram_ps_ic_stride;  /* Conv2D: ic_tile_idx 增加 1 的 DRAM offset */
                                 /* GEMM: m_tile_idx / k_tile_idx strides 分別對應 */

    /*  PD (Activation / A matrix) */
    uint32_t dram_pd_h_stride;   /* h_tile_idx (Conv2D) 或 n_tile_idx (GEMM) 的 DRAM offset */
    uint32_t dram_pd_w_stride;   /* w_tile_idx (Conv2D) 的 DRAM offset; GEMM 為 k_tile stride */
    uint32_t dram_pd_ic_stride;  /* ic_tile_idx (Conv2D) 的 DRAM offset; GEMM 不使用（設 0） */

    /*  PLO (Output / C matrix) */
    uint32_t dram_out_oc_stride;  /* oc_tile_idx (Conv2D) 或 m_tile_idx (GEMM) */
    uint32_t dram_out_h_stride;   /* h_tile_idx (Conv2D) 或 n_tile_idx (GEMM) */
    uint32_t dram_out_w_stride;   /* w_tile_idx (Conv2D); GEMM 不使用（設 0） */

    /* ── DMA 搬運量（每 group 的 word count，wave 間不變）── */
    uint32_t dma_ps_words;   /* PS group word count per wave */
    uint32_t dma_pd_words;   /* PD group word count per wave */
    uint32_t dma_plo_words;  /* PLO group word count per wave (= PLI) */

    /* ── Weight Reuse 旗標 ──
     * 若 weight 在連續 spatial tile 之間不變（同 oc_tile + ic_tile），
     * 可跳過 PS DMA — runtime 只需判斷「是否為新的 oc/ic tile」 */
    uint8_t ps_reuse_across_spatial;  /* 1 = 同 oc/ic 的 h/w tile 間 weight 不變 */

    /* ── Parallel-mode DMA 相關 ──
     * Parallel group 的 DMA 需分別寫入 3 Banks（各 bank_depth_bytes 間距）
     * 以下常數供 runtime DMA 函式判斷是否需拆分搬運 */
    uint32_t bank_depth_bytes;          /* 單一 Bank 的 byte 容量 */
    uint8_t  parallel_groups;           /* bitmask: bit0=PS, bit1=PD, bit2=PLI, bit3=PLO */
    uint32_t dma_ps_words_per_bank;     /* parallel group 時每 bank 的 word count */
    uint32_t dma_plo_words_per_bank;    /* parallel group 時每 bank 的 word count */
} TilingParams;
```

> **設計理念：Compile-time 分析 → Runtime 乘加**
>
> Compiler 在 Stage 1（Lowering）完成 tiling + SPM layout 後，已知所有 tensor 的 layout 和 tile 維度。`TilingParams` 將這些資訊壓縮為 O(1) 常數。Runtime 的 per-wave 計算僅需：
> - **乘加**：`addr = base + idx * stride`（1 multiply + 1 add）
> - **位元操作**：`buf = wave_count & 1`（ping/pong 選擇）
> - **比較**：`ic_idx == 0`（first/last tile 判斷）
>
> 這些運算在 RV32I 上只需數條指令，遠小於存放 O(waves) 的 WaveConfig[] 的 D-SRAM 開銷。

### 5.5 Layer Configuration

```c
/* ═══════════════════════════════════════════════════════
 * Per-Layer Complete Configuration
 * ═══════════════════════════════════════════════════════ */
typedef struct {
    /* Cluster selection */
    uint32_t cluster_mask_lo;
    uint32_t cluster_mask_hi;
    uint32_t num_clusters;

    /* HDDU config */
    uint32_t hddu_plane_en;
    uint32_t hddu_plane_mode;

    /* AGU template（不含 base_addr，base_addr 由 run_loop_tiling runtime 計算） */
    AguRegs agu_ps;
    AguRegs agu_pd;
    AguRegs agu_pli;
    AguRegs agu_plo;

    /* NoC: pre-encoded scan chain (shared across layers with same topology) */
    const uint32_t* scan_chain;
    uint16_t scan_chain_len;

    /* PE program: template + patches */
    const uint16_t* pe_template;
    uint16_t pe_template_len;
    const PePatchEntry* patches;
    uint8_t patch_count;

    /* Tiling loop parameters — compile-time 常數，runtime 乘加計算 per-wave 配置 */
    TilingParams tiling;
} LayerConfig;
```

> **注意**：舊版使用 `const WaveConfig* waves; uint16_t num_waves;` 以 O(waves) 陣列預存所有 wave 的配置。新版改為 `TilingParams tiling`，僅儲存 O(1) 的 compile-time 常數，由 `run_loop_tiling()` 在 runtime 透過 loop index 推導 per-wave 參數。

### 5.6 完整範例（Compile-time 常數 + Runtime 乘加計算）

以下範例展示 2-layer workload：Layer 0 為 Conv2D 3×3（IC=12, OC=16, H=14, W=14），Layer 1 為 GEMM。

```c
#include "firmware_hw.h"
#include "firmware_payload.h"

/* ═══════════════════════════════════════════════════════
 * Layer 0: conv1 (conv2d_3×3)
 *   原始: IC=12, OC=16, H=14, W=14
 *   Tile:  tile_ic=4, tile_oc=16, tile_h=7, tile_w=14
 *   Loop bounds: num_oc_tiles=1, num_h_tiles=2, num_w_tiles=1, num_ic_tiles=3
 *   Total waves = 1 × 2 × 1 × 3 = 6
 *
 *   SPM layout (Linear mode, half_group_capacity = 98304 = 0x18000):
 *     g0 (PS/weight):  PING=0x0000_0000, PONG=0x0001_8000
 *     g1 (PD/act):     PING=0x0000_1200, PONG=0x0001_9200 (offset within group)
 *     g2 (PLI):        PING=0x0000_2A80, PONG=0x0001_AA80
 *     g3 (PLO):        PING=0x0000_4300, PONG=0x0001_C300
 *
 *   AGU base = SPM base（Linear mode，AGU 直接讀 SPM 地址）
 *
 *   以下 stride 由 compiler 從 tensor layout 計算：
 *     dram_ps_oc_stride = tile_oc × IC × K × K × elem_size = 16 × 12 × 9 × 1 = 1728
 *     dram_ps_ic_stride = tile_ic × K × K × elem_size      = 4 × 9 × 1 = 36
 *     dram_pd_h_stride  = tile_h × W × IC × elem_size      = 7 × 14 × 12 × 1 = 1176
 *     dram_pd_w_stride  = tile_w × IC × elem_size           = 14 × 12 × 1 = 168
 *     dram_pd_ic_stride = tile_ic × elem_size               = 4 × 1 = 4
 *     dram_out_oc_stride = tile_oc × H_out × W_out × elem_size = 16 × 14 × 14 × 1 = 3136
 *     dram_out_h_stride  = tile_h × W_out × elem_size           = 7 × 14 × 1 = 98
 *     dram_out_w_stride  = tile_w × elem_size                   = 14 × 1 = 14
 * ═══════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════
 * Layer Table — 每個 layer 僅包含 compile-time 常數
 * ═══════════════════════════════════════════════════════ */
static const LayerConfig layer_configs[] = {
    {   /* Layer 0: conv1 (conv2d_3x3) */
        .cluster_mask_lo = 0x0000000Fu, .cluster_mask_hi = 0,
        .num_clusters = 4,
        .hddu_plane_en = 0x0Fu, .hddu_plane_mode = 0x01u,
        .agu_ps  = { .regs = {0, 0, 0x00030004u, 0x00100003u, 8, 32, 96, 288, 0, 0, 0, 1, 0, 1, 0x0F} },
        .agu_pd  = { .regs = {0, 0, 0x000E0004u, 0x0001000Eu, 8, 448, 32, 0, 0, 0, 0, 1, 0, 1, 0x0F} },
        .agu_pli = { .regs = {0, 0, 0x00050010u, 0x00010005u, 8, 40, 200, 0, 0, 0, 0, 1, 0, 1, 0x0F} },
        .agu_plo = { .regs = {0, 0, 0x00050010u, 0x00010005u, 8, 40, 200, 0, 0, 0, 0, 1, 0, 1, 0x0F} },
        .scan_chain = noc_scan_chain_64pe,
        .scan_chain_len = NOC_SCAN_CHAIN_64PE_LEN,
        .pe_template = pe_tmpl_conv1d_k3c4s1,
        .pe_template_len = PE_TMPL_CONV1D_K3C4S1_LEN,
        .patches = patch_conv1, .patch_count = PATCH_CONV1_LEN,

        /* ─── TilingParams: compile-time 常數 ─── */
        .tiling = {
            /* Loop bounds: for(oc) for(h) for(w) for(ic) */
            .num_oc_tiles = 1,   /* OC=16, tile_oc=16 → 16/16 = 1 */
            .num_h_tiles  = 2,   /* H=14,  tile_h=7   → 14/7  = 2 */
            .num_w_tiles  = 1,   /* W=14,  tile_w=14  → 14/14 = 1 */
            .num_ic_tiles = 3,   /* IC=12, tile_ic=4  → 12/4  = 3 */

            /* SPM ping/pong base per group (Linear mode) */
            .spm_ping = {0x00000000u, 0x00001200u, 0x00002A80u, 0x00004300u},
            .spm_pong = {0x0000C000u, 0x0000D200u, 0x0000DA80u, 0x0000F300u},

            /* AGU ping/pong base per bank (Linear mode: same as SPM) */
            .agu_ping = {0x00000000u, 0x00001200u, 0x00002A80u, 0x00004300u},
            .agu_pong = {0x0000C000u, 0x0000D200u, 0x0000DA80u, 0x0000F300u},

            /* SPM group mapping (PLI/PLO 交換) */
            .spm_map_even = 0xE4u,  /* port2→g2(PLI), port3→g3(PLO) */
            .spm_map_odd  = 0xD8u,  /* port2→g3(PLI), port3→g2(PLO) — 交換 */

            /* DRAM base addresses */
            .dram_weight_base = 0x80000000u,  /* weight tensor base */
            .dram_input_base  = 0x80010000u,  /* input activation base */
            .dram_output_base = 0x80020000u,  /* output activation base */

            /* DRAM strides: addr = base + Σ(tile_idx_i × stride_i)
             *   PS (weight): depends on (oc, ic) only */
            .dram_ps_oc_stride = 1728u,  /* tile_oc=16 × IC=12 × K²=9 × 1B */
            .dram_ps_ic_stride = 36u,    /* tile_ic=4  × K²=9 × 1B */

            /*   PD (activation): depends on (h, w, ic) */
            .dram_pd_h_stride  = 1176u,  /* tile_h=7 × W=14 × IC=12 × 1B */
            .dram_pd_w_stride  = 168u,   /* tile_w=14 × IC=12 × 1B */
            .dram_pd_ic_stride = 4u,     /* tile_ic=4 × 1B */

            /*   PLO (output): depends on (oc, h, w) */
            .dram_out_oc_stride = 3136u, /* tile_oc=16 × H_out=14 × W_out=14 × 1B */
            .dram_out_h_stride  = 98u,   /* tile_h=7 × W_out=14 × 1B */
            .dram_out_w_stride  = 14u,   /* tile_w=14 × 1B */

            /* DMA word counts (每 wave 恆定, 64-bit words) */
            .dma_ps_words  = 576u,   /* tile_oc × tile_ic × K² / 8 = 16×4×9/8 */
            .dma_pd_words  = 392u,   /* tile_h × tile_w × tile_ic / 8 (含 halo) */
            .dma_plo_words = 560u,   /* tile_oc × tile_h × tile_w / 8 */

            /* Weight reuse: weight 僅依賴 (oc, ic)，h/w 變化時不需重載 */
            .ps_reuse_across_spatial = 1u,

            /* Parallel mode: 此 layer 為 linear mode, 不使用 parallel DMA */
            .bank_depth_bytes = 0u,
            .parallel_groups  = 0x00u,
            .dma_ps_words_per_bank  = 0u,
            .dma_plo_words_per_bank = 0u,
        },
    },
    /* Layer 1: fc1 (GEMM) — TilingParams 使用 num_ic_tiles 對應 K-dim,
     *   num_oc_tiles → M-dim, num_h_tiles → N-dim, num_w_tiles = 1 */
};
#define NUM_LAYERS  (sizeof(layer_configs) / sizeof(layer_configs[0]))
```

> **對比舊版**：舊版需要為每個 wave 展開 `DmaTransferDesc[]` 和 `WaveConfig[]` 陣列，此 layer 的 6 個 wave 需 6 × 28 + 6 × 2 × 16 = 360 bytes。新版 `TilingParams` 固定 ~128 bytes（不論 wave 數量），節省 **64%** per-layer .rodata。若 wave 數量為 1000，舊版需 28 KB + 32 KB = 60 KB，**超出 D-SRAM 容量**；新版仍為 128 bytes。

### 5.7 PLI/PLO Group Mapping 交換

當 IC（或 GEMM 的 K）維度存在多個 tile 時，前後 tile 的 partial sum 需累加。為避免 DMA 搬運 partial sum（效率極低），firmware 在 wave 之間**交換 PLI/PLO 的 SPM group mapping**：

```
Wave N (ic_tile_idx=0, even):
  PLI → Group 2 (讀初始值 0)
  PLO → Group 3 (寫 partial sum)
  spm_map = spm_map_even = 0xE4

Wave N+1 (ic_tile_idx=1, odd):
  PLI → Group 3 (讀上一輪 partial sum)   ← 交換
  PLO → Group 2 (寫累加後 partial sum)   ← 交換
  spm_map = spm_map_odd = 0xD8 (port2→g3, port3→g2)
```

Runtime 計算方式極為簡潔（在 `run_loop_tiling()` 的最內層 loop）：
```c
uint8_t spm_map = (ic_idx & 1) ? t->spm_map_odd : t->spm_map_even;
```

不需額外的 firmware 狀態或查表。Compiler 只需在 `TilingParams` 中提供兩個 8-bit 常數即可。

---

## 6. `firmware_ops.c` — 共用 Runtime 函式

### 6.1 概述

`firmware_ops.c` 放在 I-SRAM 的 `.text` section，包含所有共用的 runtime 函式。以 data-driven 方式操作，接收 struct pointer 而非 per-layer hardcode。

### 6.2 `cfg_agu_bank()` — AGU 配置（data-driven loop）

```c
#include "firmware_hw.h"
#include "firmware_payload.h"

void cfg_agu_bank(uint32_t bank, const AguRegs* regs) {
    for (uint32_t i = 0; i < AGU_NUM_REGS; i++) {
        bcast_agu_write(bank, agu_reg_offsets[i], regs->regs[i]);
    }
}
```

> 單個 AGU bank 配置：15 次 MMIO write。與展開 15 行 `bcast_agu_write` 相比，使用迴圈的 code size = **~40 bytes**（vs 展開 ~180 bytes），節省 78%。

### 6.3 `send_noc_scan_chain()` — Scan chain 發送

```c
void send_noc_scan_chain(const uint32_t* chain, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        bcast_write32(NOC_CMD, chain[i]);
    }
}
```

> Scan chain 已在 compile-time pre-encode，runtime 只是逐 word 寫入 `NOC_CMD` window。相比原本 per-PE 呼叫 `pack_scan_chain()` 的 inline 展開（每個 PE ~10 條指令），此函式僅 **~24 bytes**。

### 6.4 `load_pe_program()` — PE 程式載入

```c
void load_pe_program(const uint16_t* prog, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        bcast_write32(NOC_CMD, pack_load_program(
            (uint16_t)(i * 2u), prog[i]));
    }
}
```

> `pack_load_program` 保留為 inline：每個 word 的 `im_addr_bytes` 隨 index 變化，無法 pre-encode。但 loop 本身只佔 ~32 bytes text。

### 6.5 `pe_patch_runtime()` — Runtime PE Template Patching

```c
/* Runtime buffer：patch 後的 PE program 存放於 Data-SRAM */
static uint16_t patched_prog[64];  /* 最大 template 長度 */

void pe_patch_runtime(const uint16_t* tmpl, uint32_t tmpl_len,
                      const PePatchEntry* patches, uint32_t patch_count) {
    /* 1. 複製原始 template 到 runtime buffer */
    for (uint32_t i = 0; i < tmpl_len; i++) {
        patched_prog[i] = tmpl[i];
    }
    /* 2. 套用 patches：替換 bits[15:6]，保留 bits[5:0] (含 LE bit) */
    for (uint32_t i = 0; i < patch_count; i++) {
        uint32_t off = patches[i].offset;
        uint16_t val = patches[i].encoded_val;
        patched_prog[off] = (uint16_t)(((val & 0x3FFu) << 6)
                                       | (patched_prog[off] & 0x3Fu));
    }
}
```

> **流程**：copy template → apply patches → `patched_prog[]` 即可用於 `load_pe_program()`。本函式 ~48 bytes text。

### 6.6 DMA Runtime Helper（地址由 caller 計算）

舊版的 `dma_load_wave()` / `dma_store_wave()` 接收 `DmaTransferDesc[]` 陣列。新版不再需要 descriptor 陣列 — 所有地址由 `run_loop_tiling()` 在 innermost loop 中以乘加計算，直接呼叫底層 DMA primitives。

```c
/* ─── 同步 DMA：等待完成後返回 ─── */
static inline void dma_xfer_sync(uint32_t dram_addr, uint32_t spm_addr,
                                 uint32_t word_count) {
    dma_start(dram_addr, spm_addr, word_count);
    dma_wait_done();
}

/* ─── Parallel-mode DMA Helper ──
 * Parallel group 的 SPM 佈局跨 3 Banks（interleaved），
 * DMA 需分 3 次搬運，每次 words_per_bank，SPM 地址間隔 bank_depth_bytes */
static void dma_xfer_parallel(uint32_t dram_addr, uint32_t spm_base,
                               uint32_t words_per_bank,
                               uint32_t bank_depth_bytes) {
    for (uint32_t b = 0; b < 3; b++) {
        dma_start(dram_addr + b * words_per_bank * 8,
                  spm_base + b * bank_depth_bytes,
                  words_per_bank);
        dma_wait_done();
    }
}
```

### 6.7 DMA Prefetch 狀態機（非同步 + 完成等待）

為實現 **data movement hiding**，DMA 預載下一個 wave 的資料與當前 wave 的 compute 重疊執行。新版 prefetch 狀態機不依賴 `DmaTransferDesc[]` 陣列，而是**快取計算好的地址列表**：

```c
/* ─── Prefetch 狀態（最多 3 筆 DMA：PS + PD + PLI，各有 dram/spm/words）─── */
#define MAX_PREFETCH_OPS 4
typedef struct {
    uint32_t dram_addr;
    uint32_t spm_addr;
    uint32_t word_count;
    uint8_t  is_parallel;    /* 1 = parallel-mode, 需拆分 3 Banks */
} PrefetchOp;

static PrefetchOp prefetch_queue[MAX_PREFETCH_OPS];
static uint32_t   prefetch_count;
static uint32_t   prefetch_idx;

/* 註冊 prefetch 操作（由 run_loop_tiling 在 DMA overlap 階段呼叫） */
static void prefetch_enqueue(uint32_t dram, uint32_t spm,
                             uint32_t words, uint8_t is_parallel) {
    if (prefetch_count < MAX_PREFETCH_OPS) {
        prefetch_queue[prefetch_count].dram_addr  = dram;
        prefetch_queue[prefetch_count].spm_addr   = spm;
        prefetch_queue[prefetch_count].word_count = words;
        prefetch_queue[prefetch_count].is_parallel = is_parallel;
        prefetch_count++;
    }
}

/* 啟動 prefetch queue（非阻塞：啟動第一筆後立即返回） */
static void prefetch_start(void) {
    prefetch_idx = 0;
    if (prefetch_count > 0) {
        PrefetchOp* op = &prefetch_queue[0];
        dma_start(op->dram_addr, op->spm_addr, op->word_count);
    }
}

/* 等待全部 prefetch 完成 */
static void prefetch_wait_all(void) {
    while (prefetch_idx < prefetch_count) {
        dma_wait_done();
        prefetch_idx++;
        if (prefetch_idx < prefetch_count) {
            PrefetchOp* op = &prefetch_queue[prefetch_idx];
            if (op->is_parallel) {
                /* parallel group 需 per-bank 搬運 — 此處簡化為同步模式 */
                dma_wait_done();
                /* 已在 prefetch_start 啟動第一筆，後續 bank 在此補完 */
            }
            dma_start(op->dram_addr, op->spm_addr, op->word_count);
        }
    }
}
```

### 6.8 `run_loop_tiling()` — Tiling Loop 排程器（核心）

此函式取代舊版的 `run_wave_loop()`。不再以 flat array 遍歷 `WaveConfig[]`，而是展開 **multi-level nested for loop**，在 innermost loop 中從 `TilingParams` 的 compile-time 常數 + loop index 動態計算 per-wave 參數。

#### 6.8.1 Conv2D Loop 結構

```c
/* ═══════════════════════════════════════════════════════
 * run_loop_tiling() — Conv2D tiling loop
 *
 * 迭代順序：for(oc) for(h) for(w) for(ic)
 *   oc: output channel tile
 *   h:  spatial height tile
 *   w:  spatial width tile
 *   ic: input channel tile (reduction 維度, 最內層)
 *
 * Runtime 計算項目（每個 wave）：
 *   1. buf (ping/pong):    wave_count & 1
 *   2. AGU base addr:      buf ? agu_pong[bank] : agu_ping[bank]
 *   3. SPM map:            (ic & 1) ? spm_map_odd : spm_map_even
 *   4. DMA DRAM addr (PS): dram_weight_base + oc * dram_ps_oc_stride + ic * dram_ps_ic_stride
 *   5. DMA DRAM addr (PD): dram_input_base  + h * dram_pd_h_stride  + w * dram_pd_w_stride + ic * dram_pd_ic_stride
 *   6. DMA DRAM addr (out):dram_output_base + oc * dram_out_oc_stride + h * dram_out_h_stride + w * dram_out_w_stride
 *   7. DMA SPM addr:       buf ? spm_pong[group] : spm_ping[group]
 *   8. is_first/last_ic:   ic == 0 / ic == num_ic_tiles - 1
 *   9. need_ps_dma:        !ps_reuse || (h == 0 && w == 0)
 * ═══════════════════════════════════════════════════════ */
void run_loop_tiling(const LayerConfig* cfg) {
    const TilingParams* t = &cfg->tiling;
    uint32_t wave_count = 0;  /* 全域 wave 計數器（用於 ping/pong） */

    for (uint16_t oc = 0; oc < t->num_oc_tiles; oc++) {
      for (uint16_t h = 0; h < t->num_h_tiles; h++) {
        for (uint16_t w = 0; w < t->num_w_tiles; w++) {
          for (uint16_t ic = 0; ic < t->num_ic_tiles; ic++) {

            /* ═══ Runtime 計算：所有值均為 base + idx × stride ═══ */

            uint32_t buf = wave_count & 1;  /* 0=PING, 1=PONG */

            /* ── AGU base address（ping/pong 切換）── */
            uint32_t agu_base_ps  = buf ? t->agu_pong[0] : t->agu_ping[0];
            uint32_t agu_base_pd  = buf ? t->agu_pong[1] : t->agu_ping[1];
            uint32_t agu_base_pli = buf ? t->agu_pong[2] : t->agu_ping[2];
            uint32_t agu_base_plo = buf ? t->agu_pong[3] : t->agu_ping[3];

            /* ── SPM group mapping（PLI/PLO 交換）── */
            uint8_t spm_map = (ic & 1) ? t->spm_map_odd : t->spm_map_even;

            /* ── DMA DRAM 地址（tile index × stride）── */
            uint32_t dram_ps = t->dram_weight_base
                             + oc * t->dram_ps_oc_stride
                             + ic * t->dram_ps_ic_stride;

            uint32_t dram_pd = t->dram_input_base
                             + h  * t->dram_pd_h_stride
                             + w  * t->dram_pd_w_stride
                             + ic * t->dram_pd_ic_stride;

            uint32_t dram_out = t->dram_output_base
                              + oc * t->dram_out_oc_stride
                              + h  * t->dram_out_h_stride
                              + w  * t->dram_out_w_stride;

            /* ── DMA SPM 地址（ping/pong 切換）── */
            uint32_t spm_ps  = buf ? t->spm_pong[0] : t->spm_ping[0];
            uint32_t spm_pd  = buf ? t->spm_pong[1] : t->spm_ping[1];
            uint32_t spm_plo = buf ? t->spm_pong[3] : t->spm_ping[3];

            /* ── 旗標 ── */
            uint8_t is_first_ic = (ic == 0);
            uint8_t is_last_ic  = (ic == t->num_ic_tiles - 1);

            /* Weight reuse: PS 僅依賴 (oc, ic)，同 oc/ic 的 h/w tile 不需重載
             * need_ps_dma=1 when: 不啟用 reuse, 或 spatial tile 起始（h==0 && w==0） */
            uint8_t need_ps_dma = !t->ps_reuse_across_spatial
                                || (h == 0 && w == 0);

            /* ══════════════════════════════════════════
             * Phase 1: 確保當前 wave 的 DMA prefetch/load 完成
             * ══════════════════════════════════════════ */
            if (wave_count == 0) {
                /* 第一個 wave: 同步載入所有 group */
                if (need_ps_dma)
                    dma_xfer_sync(dram_ps, spm_ps, t->dma_ps_words);
                dma_xfer_sync(dram_pd, spm_pd, t->dma_pd_words);
            } else {
                /* 後續 wave: 等待前一輪 async prefetch 完成 */
                prefetch_wait_all();
            }

            /* ══════════════════════════════════════════
             * Phase 2: 配置 SPM group mapping
             * ══════════════════════════════════════════ */
            bcast_write32(SPM_CONFIG_MAP,    spm_map);
            bcast_write32(SPM_CONFIG_UPDATE, 0x01u);

            /* ══════════════════════════════════════════
             * Phase 3: 更新 AGU base address（ping/pong）
             * ══════════════════════════════════════════ */
            bcast_agu_write(AGU_PS,  AGU_REG_BASE_ADDR, agu_base_ps);
            bcast_agu_write(AGU_PD,  AGU_REG_BASE_ADDR, agu_base_pd);
            bcast_agu_write(AGU_PLI, AGU_REG_BASE_ADDR, agu_base_pli);
            bcast_agu_write(AGU_PLO, AGU_REG_BASE_ADDR, agu_base_plo);

            /* ══════════════════════════════════════════
             * Phase 4: 啟動 PE + HDDU
             * ══════════════════════════════════════════ */
            bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_START_PE, 0));
            bcast_write32(HDDU_BASE + HDDU_CTRL, (1u << 1));  /* start_all */

            /* ══════════════════════════════════════════
             * Phase 5: DMA prefetch 下一個 wave（overlap with compute）
             * ══════════════════════════════════════════ */
            {
                /* 計算 next wave 的 tile indices */
                uint16_t n_ic = ic + 1, n_w = w, n_h = h, n_oc = oc;
                if (n_ic >= t->num_ic_tiles) {
                    n_ic = 0; n_w = w + 1;
                    if (n_w >= t->num_w_tiles) {
                        n_w = 0; n_h = h + 1;
                        if (n_h >= t->num_h_tiles) {
                            n_h = 0; n_oc = oc + 1;
                        }
                    }
                }

                uint8_t has_next = (n_oc < t->num_oc_tiles);
                if (has_next) {
                    uint32_t n_buf = (wave_count + 1) & 1;
                    prefetch_count = 0;  /* reset queue */

                    /* PS DMA for next wave */
                    uint8_t next_need_ps = !t->ps_reuse_across_spatial
                                         || (n_h == 0 && n_w == 0);
                    if (next_need_ps) {
                        uint32_t n_dram_ps = t->dram_weight_base
                                           + n_oc * t->dram_ps_oc_stride
                                           + n_ic * t->dram_ps_ic_stride;
                        uint32_t n_spm_ps = n_buf ? t->spm_pong[0] : t->spm_ping[0];
                        prefetch_enqueue(n_dram_ps, n_spm_ps, t->dma_ps_words, 0);
                    }

                    /* PD DMA for next wave */
                    uint32_t n_dram_pd = t->dram_input_base
                                       + n_h  * t->dram_pd_h_stride
                                       + n_w  * t->dram_pd_w_stride
                                       + n_ic * t->dram_pd_ic_stride;
                    uint32_t n_spm_pd = n_buf ? t->spm_pong[1] : t->spm_ping[1];
                    prefetch_enqueue(n_dram_pd, n_spm_pd, t->dma_pd_words, 0);

                    prefetch_start();
                }
            }

            /* ══════════════════════════════════════════
             * Phase 6: 等待 HDDU issue done
             * ══════════════════════════════════════════ */
            wait_all_clusters_hddu_done(cfg->num_clusters);

            /* ══════════════════════════════════════════
             * Phase 7: Stop HDDU，等待 AGU idle
             * ══════════════════════════════════════════ */
            bcast_write32(HDDU_BASE + HDDU_CTRL, (1u << 2));
            wait_all_clusters_agu_idle(cfg->num_clusters);

            /* ══════════════════════════════════════════
             * Phase 8: DMA writeback（僅 IC 累加的最後一個 tile）
             * ══════════════════════════════════════════ */
            if (is_last_ic) {
                dma_xfer_sync(dram_out, spm_plo, t->dma_plo_words);
            }

            wave_count++;
          } /* ic */
        } /* w */
      } /* h */
    } /* oc */

    /* ══════════════════════════════════════════════════
     * Layer tail: STOP_PE graceful quiesce
     * ══════════════════════════════════════════════════ */
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_STOP_PE, 0));
    wait_all_clusters_noc_quiesced(cfg->num_clusters);
}
```

> **Pipeline 時序圖**（4 waves, ic_tiles=2, h_tiles=2）：
> ```
> Loop: oc=0, h=0, w=0, ic=0 → wave 0 (PING)
>       oc=0, h=0, w=0, ic=1 → wave 1 (PONG)  ← is_last_ic → writeback
>       oc=0, h=1, w=0, ic=0 → wave 2 (PING)  ← ps_reuse: skip PS DMA
>       oc=0, h=1, w=0, ic=1 → wave 3 (PONG)  ← is_last_ic → writeback
>
> Time ────────────────────────────────────────────────→
> Wave 0: [DMA load PS+PD] [compute 0]          [skip writeback]
> Wave 1:                   [prefetch PD] [compute 1] [writeback 1]
> Wave 2:                                 [prefetch PD only(reuse PS)] [compute 2]
> Wave 3:                                              [prefetch...] [compute 3] [writeback 3]
>                                          ^^^^^^^^
>                                          weight reuse: 只搬 PD
> ```

#### 6.8.2 GEMM Loop 結構

GEMM 使用同一個 `run_loop_tiling()` 函式。Compiler 將 GEMM 的 3 層 loop 對應到 `TilingParams` 的 4 層迴圈：

| TilingParams field | GEMM 語意 | Conv2D 語意 |
|---|---|---|
| `num_oc_tiles` | M-dim tiles | OC tiles |
| `num_h_tiles` | N-dim tiles | H tiles |
| `num_w_tiles` | **1**（不使用） | W tiles |
| `num_ic_tiles` | K-dim tiles（reduction） | IC tiles |

此映射確保 **GEMM 和 Conv2D 共用同一份 firmware code**，不需額外的 loop variant。GEMM 的 `num_w_tiles = 1` 使 W loop 自動退化。

#### 6.8.3 Runtime 計算的指令成本分析

每個 wave 的 innermost loop body 的關鍵 runtime 計算：

| 計算項 | RV32I 指令數 | 說明 |
|--------|-------------|------|
| `buf = wave_count & 1` | 1 | `andi` |
| `agu_base = buf ? pong : ping` | 3 × 4 banks | `beq` + `lw` × 2 paths |
| `spm_map = (ic & 1) ? odd : even` | 3 | `andi` + `beq` + `lbu` |
| `dram_ps = base + oc*s + ic*s` | 4 | 2 × `mul` + 2 × `add` |
| `dram_pd = base + h*s + w*s + ic*s` | 6 | 3 × `mul` + 3 × `add` |
| `dram_out = base + oc*s + h*s + w*s` | 6 | 3 × `mul` + 3 × `add` |
| `spm_X = buf ? pong : ping` | 3 × 3 groups | conditional load |
| `is_first/last_ic` | 2 | `seqz` / `seq` |
| `need_ps_dma` check | 3 | `or` + `seqz` + `and` |
| **Total per-wave overhead** | **~40** | **< 50 cycles on single-issue RV32I** |

> Compute phase 通常 > 1000 cycles（HDDU × 48 PEs），runtime overhead 佔比 < 5%。

### 6.9 `run_layer()` — 完整 Layer 執行

```c
void run_layer(const LayerConfig* cfg) {
    /* Step 1: Cluster mask */
    set_cluster_mask(cfg->cluster_mask_lo, cfg->cluster_mask_hi);

    /* Step 2: HDDU soft-reset */
    bcast_write32(HDDU_BASE + HDDU_CTRL, (1u << 0));

    /* Step 3: Configure AGU iter/stride（只做一次，base_addr 由 tiling loop 更新） */
    cfg_agu_bank(AGU_PS,  &cfg->agu_ps);
    cfg_agu_bank(AGU_PD,  &cfg->agu_pd);
    cfg_agu_bank(AGU_PLI, &cfg->agu_pli);
    cfg_agu_bank(AGU_PLO, &cfg->agu_plo);

    /* Step 4: HDDU global */
    bcast_write32(HDDU_BASE + HDDU_PLANE_EN,   cfg->hddu_plane_en);
    bcast_write32(HDDU_BASE + HDDU_PLANE_MODE, cfg->hddu_plane_mode);

    /* Step 5: NoC Reset & Init */
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_RESET, 0));
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_INIT,  0));

    /* Step 6: Scan chain (pre-encoded, 只做一次) */
    send_noc_scan_chain(cfg->scan_chain, cfg->scan_chain_len);

    /* Step 7: Runtime patch + Load PE program (只做一次) */
    pe_patch_runtime(cfg->pe_template, cfg->pe_template_len,
                     cfg->patches, cfg->patch_count);
    load_pe_program(patched_prog, cfg->pe_template_len);

    /* Step 8: Tiling loop（含 DMA prefetch / compute / writeback） */
    run_loop_tiling(cfg);
}
```

### 6.10 MMIO Sequence 正式順序

Layer 級別（只做一次，在 tiling loop 之前）：

| 序號 | 目標 | 操作 | 說明 |
|------|------|------|------|
| 1 | Local | Write `CLUSTER_MASK` | 選定 broadcast 目標 |
| 2 | Bcast | Write `HDDU_CTRL` (bit0) | Soft-reset HDDU |
| 3 | Bcast | Write AGU regs × 4 banks | Iter/stride 設定（base_addr 由 tiling loop 更新） |
| 4 | Bcast | Write `HDDU_PLANE_EN/MODE` | HDDU 全域設定 |
| 5 | Bcast | Write `NOC_CMD_RESET` + `NOC_CMD_INIT` | Reset PE array |
| 6 | Bcast | Write scan chain × num_pes | Pre-encoded, 逐 word 寫入 |
| 7 | Bcast | Write PE program × prog_len | Runtime patched program |

Wave 級別（由 `run_loop_tiling()` 的 innermost loop 每次迭代執行）：

| 序號 | 目標 | 操作 | Runtime 計算來源 |
|------|------|------|------|
| W1 | DMA | Load / wait prefetch | `dram_addr = base + idx × stride`, `spm_addr = buf ? pong : ping` |
| W2 | Bcast | Write `SPM_CONFIG_MAP` + UPDATE | `spm_map = (ic & 1) ? odd : even` |
| W3 | Bcast | Write AGU `BASE_ADDR` × 4 banks | `agu_base = buf ? pong : ping` |
| W4 | Bcast | Write `NOC_CMD_START_PE` | — |
| W5 | Bcast | Write `HDDU_CTRL` (bit1) | start_all |
| W6 | DMA | Async prefetch next wave | 計算 next_tile_indices → `base + next_idx × stride` |
| W7 | Unicast | Poll `HDDU_STATUS` | 等待 HDDU DONE |
| W8 | Bcast + Unicast | Write `HDDU_CTRL[STOP]` + wait AGU idle | wave boundary clean-up |
| W9 | DMA | Writeback（if `is_last_ic`） | `dram_out = base + oc × s + h × s + w × s` |
| W10 | Bcast + Unicast | Write `NOC_CMD_STOP_PE` + wait NOC quiesced | layer-tail graceful quiesce |

**關鍵原則**：
- AGU iter/stride + scan chain + PE program 在 layer 內**只配置一次**，所有 wave 共用
- 每個 wave 只更新 `BASE_ADDR`（ping/pong）和 `SPM_CONFIG_MAP`（PLI/PLO 交換）— 值由 loop index 計算
- Compile-time 存放 O(1) 常數，Runtime 以 `base + idx × stride` 推導 O(waves) 的動態值
- Weight reuse: 同 `(oc, ic)` 的空間 tile 跳過 PS DMA（`need_ps_dma` 判斷）
- Broadcast write 之間不需要等待（AHB pipeline 保證順序）
- Poll 必須使用 unicast（broadcast read 僅在 mask popcount=1 時合法）

---

## 7. `firmware_main.c` — 主程式

### 7.1 結構

```c
/* Auto-generated by hybridacc-cc — DO NOT EDIT */
#include "firmware_hw.h"

/* Forward declarations */
void run_layer(const LayerConfig* cfg);

/* Layer config table（定義在 firmware_data.c） */
extern const LayerConfig layer_configs[];
extern const uint32_t num_layers;

/* ═══════════════════════════════════════════════════════
 * Trap Handler (minimal)
 * ═══════════════════════════════════════════════════════ */
void __attribute__((interrupt("machine"), aligned(4)))
trap_handler(void) {
    REG_CORE_STATUS = 0xDEAD0001u;
    while (1) { __asm__ volatile("wfi"); }
}

/* ═══════════════════════════════════════════════════════
 * Entry Point
 * ═══════════════════════════════════════════════════════ */
void __attribute__((naked, section(".text.start")))
_start(void) {
    __asm__ volatile(
        "la sp, _stack_top\n"
        "la t0, trap_handler\n"
        "csrw mtvec, t0\n"
        "j main\n"
    );
}

void main(void) {
    REG_CORE_STATUS = 0x00000001u;  /* firmware running */

    for (uint32_t i = 0; i < num_layers; i++) {
        run_layer(&layer_configs[i]);
    }

    REG_CORE_STATUS = 0x00000002u;  /* all done */
    while (1) { __asm__ volatile("wfi"); }
}
```

### 7.2 啟動流程

```
_start (address 0x00000000):
  1. 設定 sp = _stack_top (linker 提供)
  2. 設定 mtvec = trap_handler (CSR write)
  3. 跳轉到 main()

main():
  4. 寫 CORE_STATUS = 1 (firmware running)
  5. for 迴圈依序呼叫 run_layer() — 所有 layer 共用同一函式
  6. 寫 CORE_STATUS = 2 (all done)
  7. wfi loop
```

### 7.3 CSR 使用

| CSR | 用途 |
|-----|------|
| `mtvec` | 設定 trap handler 地址 |
| `mstatus` | 未主動操作（reset 後 MIE=0，中斷關閉） |

IRQ 處理在初版中不使用（改用 polling）。未來可加入 PLIC IRQ 驅動的 wave 完成通知。

---

## 8. Jinja2 Template 資料注入

### 8.1 Template Context 生成

Codegen 模組將 `HardwareIR` 轉換為 Jinja2 template context。不再以 `generate_wave_configs()` 展開 O(waves) 的 per-wave 資料，而是以 `generate_tiling_params()` 萃取 O(1) 的 compile-time 常數：

```python
def prepare_template_context(hw_ir: HardwareIR) -> Dict:
    # Collect unique templates
    templates = {}      # template_name → {"symbol": str, "instructions": List[int]}
    scan_chains = {}    # topology_key → {"symbol": str, "words": List[int]}

    for layer_cfg in hw_ir.layers:
        # Template dedup
        tmpl_name = layer_cfg.pe_program.template_name
        if tmpl_name not in templates:
            json_data = load_template_json(tmpl_name)
            templates[tmpl_name] = {
                "symbol": f"pe_tmpl_{tmpl_name}",
                "instructions": [e["dec"] for e in json_data["instructions"]],
            }

        # Scan chain dedup (by topology hash)
        topo_key = hash_scan_chain(layer_cfg.scan_chain)
        if topo_key not in scan_chains:
            encoded = encode_scan_chain(layer_cfg.scan_chain)
            scan_chains[topo_key] = {
                "symbol": f"noc_scan_chain_{len(layer_cfg.scan_chain)}pe",
                "words": encoded,
            }

    # Generate per-layer configs (with TilingParams instead of WaveConfig[])
    layers = []
    for layer_cfg in hw_ir.layers:
        tmpl_name = layer_cfg.pe_program.template_name
        json_data = load_template_json(tmpl_name)
        patch_entries = generate_patch_entries(json_data, layer_cfg.pe_program.params)
        topo_key = hash_scan_chain(layer_cfg.scan_chain)

        tiling = generate_tiling_params(layer_cfg)   # O(1) compile-time 常數

        layers.append({
            "name": layer_cfg.name,
            "cluster_mask_lo": layer_cfg.target_cluster_mask & 0xFFFFFFFF,
            "cluster_mask_hi": (layer_cfg.target_cluster_mask >> 32) & 0xFFFFFFFF,
            "num_clusters": len(layer_cfg.cluster_mapping.active_clusters)
                            if layer_cfg.cluster_mapping else 1,
            "hddu_plane_en": layer_cfg.hddu.plane_en,
            "hddu_plane_mode": layer_cfg.hddu.plane_mode,
            "agu_ps": pack_agu_regs(layer_cfg.agu_ps),
            "agu_pd": pack_agu_regs(layer_cfg.agu_pd),
            "agu_pli": pack_agu_regs(layer_cfg.agu_pli),
            "agu_plo": pack_agu_regs(layer_cfg.agu_plo),
            "scan_chain_symbol": scan_chains[topo_key]["symbol"],
            "scan_chain_len": len(scan_chains[topo_key]["words"]),
            "pe_template_symbol": templates[tmpl_name]["symbol"],
            "pe_template_len": len(templates[tmpl_name]["instructions"]),
            "patch_entries": patch_entries,
            "tiling": tiling,   # TilingParams dict
        })

    return {
        "templates": templates,
        "scan_chains": scan_chains,
        "layers": layers,
    }
```

### 8.2 Tiling Parameters 生成（Compile-time 常數萃取）

```python
def generate_tiling_params(layer_cfg: LayerHwConfig) -> Dict:
    """
    從 tiling result + SPM layout 中萃取 O(1) compile-time 常數。
    不再展開 per-wave config — 所有 per-wave 值由 firmware runtime 以
    base + idx × stride 計算。
    """
    tiling = layer_cfg.tiling
    spm = layer_cfg.spm_layout
    tensor_layout = layer_cfg.tensor_layout

    # ── Loop bounds ──
    loop_bounds = {
        "num_oc_tiles": tiling.loop_bounds.get("oc", tiling.loop_bounds.get("m", 1)),
        "num_h_tiles":  tiling.loop_bounds.get("h",  tiling.loop_bounds.get("n", 1)),
        "num_w_tiles":  tiling.loop_bounds.get("w",  1),
        "num_ic_tiles": tiling.loop_bounds.get("ic", tiling.loop_bounds.get("k", 1)),
    }

    # ── SPM ping/pong base per group ──
    spm_bases = compute_spm_ping_pong_bases(spm)
    # spm_bases = {"ping": [ps, pd, pli, plo], "pong": [ps, pd, pli, plo]}

    # ── AGU ping/pong base ──
    agu_bases = compute_agu_ping_pong_bases(spm)

    # ── SPM map (PLI/PLO swap) ──
    spm_map_even = 0xE4   # port2→g2(PLI), port3→g3(PLO)
    spm_map_odd  = 0xD8   # port2→g3(PLI), port3→g2(PLO)

    # ── DRAM base + stride 計算 ──
    # 每個 tile index 增加 1 時 DRAM 地址的 byte 增量
    dram = compute_dram_base_and_strides(tensor_layout, tiling)
    # dram = {
    #   "weight_base", "input_base", "output_base",
    #   "ps_oc_stride", "ps_ic_stride",
    #   "pd_h_stride", "pd_w_stride", "pd_ic_stride",
    #   "out_oc_stride", "out_h_stride", "out_w_stride",
    # }

    # ── DMA word counts (每 wave 恆定) ──
    dma_words = compute_dma_word_counts(tiling, spm)

    # ── Weight reuse ──
    # Conv2D: weight depends on (oc, ic) only → reuse across spatial tiles
    # GEMM: B depends on (m, k) only → reuse across n tiles
    ps_reuse = (layer_cfg.op_type in ["conv2d", "gemm"])

    # ── Parallel mode ──
    parallel = compute_parallel_mode_params(spm)

    return {
        **loop_bounds,
        "spm_ping": spm_bases["ping"],
        "spm_pong": spm_bases["pong"],
        "agu_ping": agu_bases["ping"],
        "agu_pong": agu_bases["pong"],
        "spm_map_even": spm_map_even,
        "spm_map_odd":  spm_map_odd,
        "dram_weight_base": dram["weight_base"],
        "dram_input_base":  dram["input_base"],
        "dram_output_base": dram["output_base"],
        "dram_ps_oc_stride": dram["ps_oc_stride"],
        "dram_ps_ic_stride": dram["ps_ic_stride"],
        "dram_pd_h_stride":  dram["pd_h_stride"],
        "dram_pd_w_stride":  dram["pd_w_stride"],
        "dram_pd_ic_stride": dram["pd_ic_stride"],
        "dram_out_oc_stride": dram["out_oc_stride"],
        "dram_out_h_stride":  dram["out_h_stride"],
        "dram_out_w_stride":  dram["out_w_stride"],
        "dma_ps_words":  dma_words["ps"],
        "dma_pd_words":  dma_words["pd"],
        "dma_plo_words": dma_words["plo"],
        "ps_reuse_across_spatial": 1 if ps_reuse else 0,
        **parallel,
    }
```

> **與舊版 `generate_wave_configs()` 的差異**：
>
> | 面向 | 舊版 `generate_wave_configs()` | 新版 `generate_tiling_params()` |
> |------|------|------|
> | 輸出資料量 | O(waves) — 每個 wave 一筆 WaveConfig + DMA descriptors | O(1) — 每個 layer 一組常數 |
> | Loop 展開 | Compiler 展開 `for wave_idx in range(total_waves)` | **不展開** — 只萃取 stride/base |
> | DRAM 地址 | Pre-compute: `dram_weight_base + oc_start * ... + ic_start * ...` per wave | 儲存 `base` + `stride`, runtime 計算 `base + idx * stride` |
> | SPM 地址 | Per-wave ping/pong base address | 只存 `ping[4]` + `pong[4]`, runtime 以 `buf & 1` 選取 |
> | 1000-wave workload | 需 ~60 KB .rodata（**超出 D-SRAM**） | 仍為 ~128 B .rodata |

### 8.3 Jinja2 Template 概覽

生成 4 個 C 源檔，主要差異在 `firmware_payload.h` 和 `firmware_data.c`（per-workload 變化），`firmware_ops.c` 和 `firmware_main.c` 幾乎不變：

| Template File | 變動性 | 由 Jinja2 產生的內容 |
|---------------|--------|---------------------|
| `firmware_payload.h.j2` | 中 | PE template arrays, scan chain arrays, patch entry arrays |
| `firmware_data.c.j2` | 高 | `LayerConfig` table（含 inline `TilingParams`，O(1) per layer） |
| `firmware_ops.c.j2` | 低 | 幾乎固定（`run_loop_tiling()`、DMA helpers） |
| `firmware_main.c.j2` | 低 | 幾乎固定 |

---

## 9. 程式碼大小估算

### 9.1 I-SRAM (.text) — 固定大小

Data-driven + Compile-time/Runtime 協同計算架構下，I-SRAM 的 code size 與 layer 數量和 wave 數量**均無關**：

| 函式 | 大約 bytes | 說明 |
|------|-----------|------|
| `_start` + `trap_handler` | 32 | Entry + CSR setup |
| `main` | 48 | Layer for-loop + status writes |
| `run_layer` | 160 | 9-step one-time setup |
| `run_loop_tiling` | 320 | **4-level nested for loop + runtime 乘加 + DMA/compute overlap** |
| `cfg_agu_bank` | 40 | 15-reg data-driven loop |
| `send_noc_scan_chain` | 24 | Simple word loop |
| `load_pe_program` | 32 | Pack + write loop |
| `pe_patch_runtime` | 48 | Copy + patch loop |
| `dma_xfer_sync` + `dma_xfer_parallel` | 48 | 底層 DMA helper（取代舊版 dma_load/store_wave） |
| `prefetch_enqueue` / `prefetch_start` / `prefetch_wait_all` | 80 | Async prefetch 狀態機 |
| inline helpers（in .text） | ~200 | mmio_write32, bcast_write32 etc. |
| **Total .text** | **~1032** | **≈ 1 KB** |

> `run_loop_tiling()` 因含有 runtime 乘加計算（每 wave ~40 RV32I 指令的 address computation）和 next-wave prefetch 邏輯，比舊版 `run_wave_loop()` 增加約 120 bytes。但相對於 I-SRAM 的 16 KB 容量，仍然綽綽有餘。

### 9.2 Data-SRAM (.rodata) — **O(layers)，不再與 wave 數量相關**

| 資料 | 每 layer (bytes) | 累計規模 | 說明 |
|------|-----------------|---------|------|
| `LayerConfig`（含 `TilingParams`） | ~280 | O(layers) | 4 × AguRegs(60B each) + pointers + TilingParams(~128B) |
| `PePatchEntry` | ~3 per entry | O(layers) | offset + encoded_val |
| PE template（shared） | ~72 per unique template | O(unique_templates) | 36 instructions × 2 bytes |
| Scan chain（shared） | ~192 per unique topology | O(unique_topologies) | 48 PEs × 4 bytes |

> **不再存在**：`WaveConfig[]`（28B×waves）、`DmaTransferDesc[]`（16B×descriptors）— 已由 runtime 計算取代。

**範例估算**：3 layers, 每 layer 6 waves (Conv2D), 1 layer 1000 waves (大型 Conv2D)
- LayerConfig + TilingParams: 4 × 280 = 1120 B
- Patches: ~50 B
- Templates: ~200 B (2 unique)
- Scan chain: ~256 B (1 unique)
- **Total .rodata ≈ 1.6 KB**

> 即使某 layer 有 **1000 個 wave**，.rodata 用量完全相同 — 因為 per-wave 資料已改為 runtime 計算。

### 9.3 與舊版對比

| 指標 | 最舊版（per-layer 展開） | 前版（data-driven WaveConfig）| **新版（compile+runtime 協同）** |
|------|----------------------|-----------------------------|------|
| I-SRAM per layer | ~2172 B | 0（共用函式） | 0（共用函式） |
| I-SRAM 總量 | ~2172 × N layers | < 1 KB 固定 | **≈ 1 KB 固定** |
| 最大 layer 數 | ~7 | 不受 I-SRAM 限制 | 不受 I-SRAM 限制 |
| D-SRAM .rodata | 0 | O(layers × waves)：~(160 + 28W + 32W) | **O(layers)：~280 per layer** |
| 1000-wave layer | — | 需 ~60 KB (**超出 D-SRAM**) | **仍為 280 B** |
| Scan chain 儲存 | per-layer inline 展開 | 共用 pre-encoded array | 共用 pre-encoded array |
| PE program 儲存 | per-layer patched copy | 共用 template + patch desc | 共用 template + patch desc |
| DMA overlap | 無 | 靜態 descriptor prefetch | **runtime 計算 + async prefetch** |
| Runtime overhead | 0 | 0（存取 array） | ~40 cycles/wave（< 5%） |

---

## 10. Data Movement Hiding 設計

### 10.1 設計目標

HybridAcc 為 **data-movement aware** 加速器，DMA 搬運延遲必須 overlap 在 cluster 計算期間。此設計對應 `test_cluster_sim_advanced.cpp` 中的 wave pipeline 建模。

在新版 compile+runtime 協同架構下，DMA 的 DRAM/SPM 地址不再從 `DmaTransferDesc[]` 查表，而是由 `run_loop_tiling()` 在每個 wave 的 innermost loop 中，以 `base + idx × stride` 動態計算後直接傳給 DMA engine。

### 10.2 Ping-Pong 雙緩衝生命週期（Loop-based）

```
              PING buffer (buf=0)                PONG buffer (buf=1)
Wave 0 (oc=0,h=0,w=0,ic=0):
              [compute ← AGU ping]               [DMA prefetch wave 1]
Wave 1 (oc=0,h=0,w=0,ic=1):
              [DMA prefetch wave 2]               [compute ← AGU pong]
Wave 2 (oc=0,h=1,w=0,ic=0):
              [compute ← AGU ping]               [DMA prefetch wave 3]
  ...
```

每個 wave 結束後，`wave_count++` 使 `buf = wave_count & 1` 自動切換 ping/pong。`run_loop_tiling()` 計算：
- **當前 wave**：`agu_base = buf ? pong : ping`，`spm_addr = buf ? spm_pong[group] : spm_ping[group]`
- **下一 wave**：`n_buf = (wave_count+1) & 1`，prefetch 到 `n_buf ? spm_pong : spm_ping`

### 10.3 SPM 安全性條件

DMA prefetch 下一 wave 時，必須確保**寫入的 SPM 地址不與當前 compute 讀取的地址衝突**：

1. **不同 Half-group**：DMA 寫入 PONG half，compute 讀取 PING half → 安全（地址空間完全分離）
2. **地址隔離保證**：由 `TilingParams.spm_ping[]` / `spm_pong[]` 的 compile-time 值保證，PING 和 PONG 的地址範圍不重疊
3. **Weight Reuse**：若 `ps_reuse_across_spatial=1` 且只有 h/w tile 變化，firmware 以 `need_ps_dma` 判斷跳過 PS DMA → prefetch queue 較短

### 10.4 IC/K 累加與 PLI/PLO 交換

當 reduction 維度（Conv2D 的 IC, GEMM 的 K）有多個 tile 時：

```
ic=0 (even):  PLI=Group2(讀初始0), PLO=Group3(寫partial)    spm_map=spm_map_even=0xE4
ic=1 (odd):   PLI=Group3(讀partial), PLO=Group2(寫累加)     spm_map=spm_map_odd=0xD8  ← 交換
ic=2 (even):  PLI=Group2(讀partial), PLO=Group3(寫累加)     spm_map=spm_map_even=0xE4  ← 交換回
```

Runtime 計算：`spm_map = (ic & 1) ? t->spm_map_odd : t->spm_map_even`。此交換避免了 DMA 搬運 partial sum（partial sum 直接在 SPM 中原地累加）。

### 10.5 DMA 地址計算原則（Runtime 版）

`run_loop_tiling()` 在每個 wave 中以 compile-time 常數 + loop index 計算 DMA 地址：

| 項目 | Runtime 計算公式 | Compile-time 常數 |
|------|------|------|
| **PS DRAM addr** | `dram_weight_base + oc * ps_oc_stride + ic * ps_ic_stride` | `dram_weight_base`, `dram_ps_oc_stride`, `dram_ps_ic_stride` |
| **PD DRAM addr** | `dram_input_base + h * pd_h_stride + w * pd_w_stride + ic * pd_ic_stride` | `dram_input_base`, `dram_pd_h_stride`, `dram_pd_w_stride`, `dram_pd_ic_stride` |
| **Output DRAM addr** | `dram_output_base + oc * out_oc_stride + h * out_h_stride + w * out_w_stride` | `dram_output_base`, `dram_out_oc_stride`, `dram_out_h_stride`, `dram_out_w_stride` |
| **SPM addr (ping/pong)** | `buf ? spm_pong[group] : spm_ping[group]` | `spm_ping[4]`, `spm_pong[4]` |
| **Weight reuse skip** | `!ps_reuse \|\| (h == 0 && w == 0)` | `ps_reuse_across_spatial` |
| **Writeback guard** | `ic == num_ic_tiles - 1` | `num_ic_tiles` |
| **Parallel-mode DMA** | `dma_xfer_parallel(dram, spm, words_per_bank, bank_depth)` | `parallel_groups`, `dma_ps_words_per_bank`, `bank_depth_bytes` |

> **核心設計**：所有 per-wave DMA 地址都是 tile index 的**仿射函數**（affine function）：`addr = base + Σ(idx_i × stride_i)`。Compiler 負責分析 tensor layout 和 tiling 結構來萃取 `base` 和 `stride`，firmware 僅做乘加。此設計將 O(waves) 的 .rodata 壓縮為 O(1) 的 compile-time 常數，從根本上解決大型 workload 的 D-SRAM 擴展性問題。
