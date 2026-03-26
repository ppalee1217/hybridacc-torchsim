# HybridAcc-CC：Code Generation 詳細規格

> 前置閱讀：[00_Overview.md](00_Overview.md)、[02_OperatorLowering.md](02_OperatorLowering.md)

---

## 1. 總覽

Code Generation（Stage 2）負責將 `HardwareIR` 轉換為可在 `cc_core_mcu`（RV32I_zicsr 5-stage pipeline）上執行的 C 語言韌體原始碼。

生成的 C 程式碼有以下特徵：

1. **純 C**（C11 標準），不使用 C++
2. **`-nostdlib -ffreestanding`**：不連結標準庫，不依賴 OS
3. **所有硬體互動都透過 MMIO volatile pointer dereference**
4. **使用 Jinja2 模板引擎**從 HardwareIR 產生最終 C 原始碼

---

## 2. 生成的檔案結構

| 檔案 | 角色 | 內容 |
|------|------|------|
| `firmware_hw.h` | 硬體抽象 | MMIO 位址常數、helper inline function |
| `firmware_payload.h` | PE 程式資料 | 各 layer 的 patched PE program uint16_t array |
| `firmware_layers.c` | Layer 配置函數 | 每個 layer 一個 `void layer_XXX(void)` 函數 |
| `firmware_main.c` | 主程式入口 | `_start`、trap handler、layer 呼叫序列 |
| `linker.ld` | 連結腳本 | Memory region 定義與 section 配置 |

---

## 3. `firmware_hw.h` — 硬體抽象層

### 3.1 MMIO 位址常數

```c
#ifndef FIRMWARE_HW_H
#define FIRMWARE_HW_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════
 * Core Address Map
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
#define DMA_STREAM_BASE     0x20001800u

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

static inline uint32_t pack_scan_chain(
    uint8_t ps_id, uint8_t pd_id,
    uint8_t pli_id, uint8_t plo_id,
    uint8_t route_mode, uint8_t enable)
{
    uint32_t v = 0;
    v |= ((uint32_t)(ps_id   & 0x3Fu)) << 4;
    v |= ((uint32_t)(pd_id   & 0x3Fu)) << 10;
    v |= ((uint32_t)(pli_id  & 0x3Fu)) << 16;
    v |= ((uint32_t)(plo_id  & 0x3Fu)) << 22;
    v |= ((uint32_t)(route_mode & 0x03u)) << 28;
    v |= ((uint32_t)(enable  & 0x01u)) << 30;
    return pack_noc_cmd(NOC_CMD_SCAN_CHAIN, v);
}

static inline uint32_t pack_load_program(uint16_t im_addr_bytes,
                                         uint16_t inst16) {
    uint32_t p = 0;
    p |= ((uint32_t)(im_addr_bytes & 0xFFFu)) << 4;
    p |= ((uint32_t)(inst16 & 0xFFFFu)) << 16;
    return pack_noc_cmd(NOC_CMD_LOAD_PROGRAM, p);
}

/* ── HDDU Status polling ── */

static inline void wait_hddu_done(void) {
    /* Poll broadcast HDDU_STATUS until not busy */
    /* NOTE: broadcast read is only legal when mask popcount == 1.
     * For multi-cluster, poll each cluster individually or use IRQ. */
    while (1) {
        uint32_t st = mmio_read32(CLUSTER_BCAST_BASE + HDDU_BASE + HDDU_STATUS);
        if (st & (1u << 4)) break;  /* error */
        if (!(st & (1u << 1))) break; /* not busy → done */
    }
}

/* For multi-cluster: iterate and poll each */
static inline void wait_all_clusters_done(uint32_t num_clusters) {
    for (uint32_t c = 0; c < num_clusters; c++) {
        uint32_t base = CLUSTER_UNICAST_BASE + c * CLUSTER_STRIDE;
        while (1) {
            uint32_t st = mmio_read32(base + HDDU_BASE + HDDU_STATUS);
            if (st & (1u << 4)) return;  /* error */
            if (!(st & (1u << 1))) break; /* done */
        }
    }
}

#endif /* FIRMWARE_HW_H */
```

### 3.2 設計說明

- 所有 MMIO 存取都透過 `volatile` 指標，確保 GCC 不會優化掉 side effect
- `bcast_write32` / `unicast_write32` 封裝了地址計算
- `pack_scan_chain` / `pack_load_program` 與 ComputeCluster.md §9.1 的 C++ 範例完全對應
- 位址常數完全對應 Core.md §6.4 的 address map

---

## 4. `firmware_payload.h` — PE 程式 Payload

### 4.1 格式

每個 layer 的 patched PE program 以 `static const uint16_t` array 嵌入：

```c
#ifndef FIRMWARE_PAYLOAD_H
#define FIRMWARE_PAYLOAD_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════
 * Layer: conv1  (conv2d_3x3)
 * Template: conv1d_k3c4s1_template
 * Params: KERNEL_DMA_LEN=192, OUTPUT_WINDOW_CNT_MINUS_ONE=195,
 *         KERNEL_COUNT=16, KERNEL_LOOP_INNER=1, KERNEL_LOOP_OUTER=1
 * ═══════════════════════════════════════════════════════ */
static const uint16_t pe_prog_conv1[] = {
    0x004C, /* 0: SYSCTRL (CLEAR.P) */
    0x0004, /* 1: LOOPIN 1                (patched: KERNEL_LOOP_OUTER=1) */
    0x0020, /* 2: SDMA.ADDR 0 */
    0x2FE8, /* 3: SDMA.LEN 192            (patched: KERNEL_DMA_LEN=192) */
    0x0030, /* 4: SDMA.LOOP 1             (patched: KERNEL_LOOP_INNER=1) */
    /* ... 略 ... */
    0x001C, /* 35: HALT */
};
#define PE_PROG_CONV1_LEN  36

/* ═══════════════════════════════════════════════════════
 * Layer: fc1  (gemm)
 * Template: gemm_template
 * ... (類似)
 * ═══════════════════════════════════════════════════════ */
static const uint16_t pe_prog_fc1[] = {
    /* ... */
};
#define PE_PROG_FC1_LEN  27

#endif /* FIRMWARE_PAYLOAD_H */
```

### 4.2 Patch 過程

對 JSON template 中每個 `patches[]` entry，依照 `param_index` 找到對應的 runtime parameter value，替換 instruction word 的 payload 欄位（bits[15:6]）：

```python
def apply_patches(json_data: dict, params: Dict[str, int]) -> List[int]:
    """
    Apply parameter patches to PE template instructions.

    Args:
        json_data: Parsed JSON from kernel/json/*.json
        params: {param_name: value} overrides

    Returns:
        List of patched instruction words (uint16)
    """
    # Build param value array (indexed by param_index)
    param_defs = json_data["parameters"]
    param_values = []
    for p in param_defs:
        name = p["name"]
        if name in params:
            param_values.append(params[name])
        else:
            param_values.append(p["default"])

    # Copy base instructions
    instructions = [entry["dec"] for entry in json_data["instructions"]]

    # Apply patches
    for patch in json_data["patches"]:
        offset = patch["offset"]
        param_idx = patch["param_index"]
        value = param_values[param_idx]

        # Determine encoding: some instructions use N-1 encoding
        # The template was assembled with default values already N-1 encoded.
        # We need to re-encode: replace payload with (value - 1) for
        # LOOPIN/LDMA.LEN/SDMA.LEN/LDMA.LOOP/SDMA.LOOP,
        # or just value for others.
        #
        # Detection: check opcode/func2 of the instruction to determine encoding
        word = instructions[offset]
        opcode = (word >> 1) & 0x3
        func2 = (word >> 3) & 0x3

        if opcode == 0b10 and func2 == 0b00:
            # LOOPIN: N-1 encoding
            encoded = value - 1
        elif opcode == 0b00 and func2 in (0b01, 0b10):
            # LDMA.LEN / SDMA.LEN / LDMA.LOOP / SDMA.LOOP: N-1 encoding
            encoded = value - 1
        elif opcode == 0b00 and func2 == 0b11:
            # LDMA.LD/SDMA.SD/etc: payload includes stride+func3, complex
            # For SDMA.LEN (func2=01): already handled above
            # For SDMA.SD (func2=11): stride in payload, typically not patched
            encoded = value
        else:
            encoded = value

        # Replace payload (bits [15:6])
        instructions[offset] = ((encoded & 0x3FF) << 6) | (word & 0x3F)

    return instructions
```

### 4.3 注意事項

1. **N-1 編碼**：`LOOPIN`、`LDMA.LEN`、`SDMA.LEN`、`LDMA.LOOP`、`SDMA.LOOP` 的 payload 使用 `value - 1` 編碼。Patch 時必須判斷指令類型並相應處理。
2. **LE bit 保留**：Patch 不得改變 bit[0]（LOOPEND marker）。上述 mask `0x3F` 已包含 LE bit。
3. **Payload 上限**：10-bit payload 最大值為 1023。若 `value - 1 > 1023`，則超出範圍，需報錯。

---

## 5. `firmware_layers.c` — Layer 配置函數

### 5.1 函數簽名

每個 layer 生成一個配置函數：

```c
void layer_conv1(void);
void layer_conv2(void);
void layer_fc1(void);
```

### 5.2 函數內部結構（以 Conv2D 3×3 為例）

每個 layer 函數遵循嚴格的 MMIO sequence：

```c
#include "firmware_hw.h"
#include "firmware_payload.h"

void layer_conv1(void) {
    /* ────────────────────────────────────────────
     * Step 1: Set cluster mask
     * ──────────────────────────────────────────── */
    set_cluster_mask(0x0000000Fu, 0x00000000u);  /* clusters 0-3 */

    /* ────────────────────────────────────────────
     * Step 2: SPM config (broadcast to all selected clusters)
     * ──────────────────────────────────────────── */
    bcast_write32(SPM_CONFIG_MAP,    0xE4u);  /* port0→g0, port1→g1, port2→g2, port3→g3 */
    bcast_write32(SPM_CONFIG_UPDATE, 0x01u);  /* trigger config_update pulse */

    /* ────────────────────────────────────────────
     * Step 3: HDDU soft-reset (clear FIFO and errors)
     * ──────────────────────────────────────────── */
    bcast_write32(HDDU_BASE + HDDU_CTRL, (1u << 0));

    /* ────────────────────────────────────────────
     * Step 4: Configure AGU Bank 0 (PS - Weight)
     * ──────────────────────────────────────────── */
    bcast_agu_write(AGU_PS, AGU_REG_BASE_ADDR,   0x00000000u);
    bcast_agu_write(AGU_PS, AGU_REG_BASE_ADDR_H, 0x00000000u);
    bcast_agu_write(AGU_PS, AGU_REG_ITER01,      0x00030004u);  /* iter1=3(KW), iter0=4(IC_pack) */
    bcast_agu_write(AGU_PS, AGU_REG_ITER23,      0x00100003u);  /* iter3=16(OC), iter2=3(KH) */
    bcast_agu_write(AGU_PS, AGU_REG_STRIDE0,     0x00000008u);  /* 8 bytes */
    bcast_agu_write(AGU_PS, AGU_REG_STRIDE1,     0x00000020u);  /* 4*8=32 */
    bcast_agu_write(AGU_PS, AGU_REG_STRIDE2,     0x00000060u);  /* 3*4*8=96 */
    bcast_agu_write(AGU_PS, AGU_REG_STRIDE3,     0x00000120u);  /* 3*3*4*8=288 */
    bcast_agu_write(AGU_PS, AGU_REG_LANE_CFG,    0x00000000u);
    bcast_agu_write(AGU_PS, AGU_REG_TAG_BASE,    0x00000000u);
    bcast_agu_write(AGU_PS, AGU_REG_TAG_STRIDE0, 0x00000001u);
    bcast_agu_write(AGU_PS, AGU_REG_TAG_STRIDE1, 0x00000000u);
    bcast_agu_write(AGU_PS, AGU_REG_TAG_CTRL,    0x00000001u);
    bcast_agu_write(AGU_PS, AGU_REG_MASK_CFG,    0x0000000Fu);
    bcast_agu_write(AGU_PS, AGU_REG_CTRL,        0x00000000u);

    /* ────────────────────────────────────────────
     * Step 5: Configure AGU Bank 1 (PD - Activation)
     * ──────────────────────────────────────────── */
    bcast_agu_write(AGU_PD, AGU_REG_BASE_ADDR,   0x00001200u);  /* pd_base */
    bcast_agu_write(AGU_PD, AGU_REG_BASE_ADDR_H, 0x00000000u);
    bcast_agu_write(AGU_PD, AGU_REG_ITER01,      0x000E0004u);  /* iter1=14(H), iter0=4(IC_pack) */
    bcast_agu_write(AGU_PD, AGU_REG_ITER23,      0x0001000Eu);  /* iter3=1, iter2=14(W) */
    bcast_agu_write(AGU_PD, AGU_REG_STRIDE0,     0x00000008u);
    bcast_agu_write(AGU_PD, AGU_REG_STRIDE1,     0x00000380u);  /* W*IC*8 = 14*4*8 = 448 */
    bcast_agu_write(AGU_PD, AGU_REG_STRIDE2,     0x00000020u);  /* IC*8 = 32 */
    bcast_agu_write(AGU_PD, AGU_REG_STRIDE3,     0x00000000u);
    bcast_agu_write(AGU_PD, AGU_REG_LANE_CFG,    0x00000000u);
    bcast_agu_write(AGU_PD, AGU_REG_TAG_BASE,    0x00000000u);
    bcast_agu_write(AGU_PD, AGU_REG_TAG_STRIDE0, 0x00000001u);
    bcast_agu_write(AGU_PD, AGU_REG_TAG_STRIDE1, 0x00000000u);
    bcast_agu_write(AGU_PD, AGU_REG_TAG_CTRL,    0x00000001u);
    bcast_agu_write(AGU_PD, AGU_REG_MASK_CFG,    0x0000000Fu);
    bcast_agu_write(AGU_PD, AGU_REG_CTRL,        0x00000000u);

    /* ────────────────────────────────────────────
     * Step 6: Configure AGU Bank 2 (PLI - Partial Sum In)
     * ──────────────────────────────────────────── */
    bcast_agu_write(AGU_PLI, AGU_REG_BASE_ADDR,   0x00002A80u);  /* pli_base */
    /* ... (iter/stride 計算如同 §5.2 但使用 PLI 對應的值) ... */
    bcast_agu_write(AGU_PLI, AGU_REG_CTRL,        0x00000000u);

    /* ────────────────────────────────────────────
     * Step 7: Configure AGU Bank 3 (PLO - Output)
     * ──────────────────────────────────────────── */
    bcast_agu_write(AGU_PLO, AGU_REG_BASE_ADDR,   0x00004300u);  /* plo_base */
    /* ... (與 PLI 相同 stride pattern，不同 base) ... */
    bcast_agu_write(AGU_PLO, AGU_REG_CTRL,        0x00000000u);

    /* ────────────────────────────────────────────
     * Step 8: HDDU global config
     * ──────────────────────────────────────────── */
    bcast_write32(HDDU_BASE + HDDU_PLANE_EN,   0x0000000Fu);
    bcast_write32(HDDU_BASE + HDDU_PLANE_MODE, 0x00000001u);

    /* ────────────────────────────────────────────
     * Step 9: NoC - Reset & Init
     * ──────────────────────────────────────────── */
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_RESET, 0));
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_INIT, 0x00000000u));

    /* ────────────────────────────────────────────
     * Step 10: NoC - Scan chain (反序 shift)
     * 從最後一個 PE 開始，到第一個 PE 結束
     * ──────────────────────────────────────────── */
    /* PE 63 (bus 3, last PE): PLI_FROM_LN_PLO_TO_BUS */
    bcast_write32(NOC_CMD, pack_scan_chain(63, 63, 63, 3, 2, 1));
    /* PE 62: PLI_FROM_LN_PLO_TO_LN */
    bcast_write32(NOC_CMD, pack_scan_chain(62, 62, 62, 62, 0, 1));
    /* ... (中間 PE 省略，實際由 codegen 逐一展開) ... */
    /* PE 1: PLI_FROM_LN_PLO_TO_LN */
    bcast_write32(NOC_CMD, pack_scan_chain(1, 1, 1, 1, 0, 1));
    /* PE 0 (bus 0, first PE): PLI_FROM_BUS_PLO_TO_LN */
    bcast_write32(NOC_CMD, pack_scan_chain(0, 0, 0, 0, 1, 1));

    /* ────────────────────────────────────────────
     * Step 11: NoC - Load PE program
     * ──────────────────────────────────────────── */
    for (uint32_t i = 0; i < PE_PROG_CONV1_LEN; i++) {
        bcast_write32(NOC_CMD, pack_load_program(
            (uint16_t)(i * 2),    /* byte address = word_index * 2 */
            pe_prog_conv1[i]
        ));
    }

    /* ────────────────────────────────────────────
     * Step 12: NoC - Start PE
     * ──────────────────────────────────────────── */
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_START_PE, 0));

    /* ────────────────────────────────────────────
     * Step 13: HDDU - Start all AGUs
     * ──────────────────────────────────────────── */
    bcast_write32(HDDU_BASE + HDDU_CTRL, (1u << 1));  /* start_all */

    /* ────────────────────────────────────────────
     * Step 14: Wait for completion
     * ──────────────────────────────────────────── */
    wait_all_clusters_done(4);
}
```

### 5.3 MMIO Sequence 正式順序

所有 layer 函數必須嚴格遵守以下 MMIO 指令順序：

| 序號 | 目標 | MMIO Window | 操作 | 說明 |
|------|------|-------------|------|------|
| 1 | Local | `0x2000_0004` | Write `CLUSTER_MASK_LO` | 選定 broadcast 目標 |
| 2 | Local | `0x2000_0008` | Write `CLUSTER_MASK_HI` | （若 > 32 clusters） |
| 3 | Bcast | `0x5000_0000` | Write `SPM_CONFIG_MAP` | SPM port→group 映射 |
| 4 | Bcast | `0x5000_0004` | Write `SPM_CONFIG_UPDATE` | Trigger pulse |
| 5 | Bcast | `0x5000_1800` | Write `HDDU_CTRL` (bit0) | Soft-reset HDDU |
| 6 | Bcast | `0x5000_1000..` | Write AGU PS regs ×15 | 14 個暫存器 + CTRL |
| 7 | Bcast | `0x5000_1100..` | Write AGU PD regs ×15 | 同上 |
| 8 | Bcast | `0x5000_1200..` | Write AGU PLI regs ×15 | 同上 |
| 9 | Bcast | `0x5000_1300..` | Write AGU PLO regs ×15 | 同上 |
| 10 | Bcast | `0x5000_1808` | Write `HDDU_PLANE_EN` | 啟用平面 |
| 11 | Bcast | `0x5000_180C` | Write `HDDU_PLANE_MODE` | 模式旗標 |
| 12 | Bcast | `0x5000_2000` | Write `NOC_CMD_RESET` | Reset PE array |
| 13 | Bcast | `0x5000_2000` | Write `NOC_CMD_INIT` | Init PE |
| 14 | Bcast | `0x5000_2000` | Write `NOC_SCAN_CHAIN` ×num_pes | 反序 shift |
| 15 | Bcast | `0x5000_2000` | Write `NOC_LOAD_PROGRAM` ×prog_len | 逐 word 載入 |
| 16 | Bcast | `0x5000_2000` | Write `NOC_CMD_START_PE` | 啟動 PE 執行 |
| 17 | Bcast | `0x5000_1800` | Write `HDDU_CTRL` (bit1) | Start all AGU |
| 18 | Unicast | `0x4000_X804` | Poll `HDDU_STATUS` | 等待完成 |

**關鍵原則**：
- 所有 broadcast write 之間不需要等待（AHB pipeline 保證順序）
- AGU CTRL 的 start bit 不在個別 AGU 中設置，而是由 `HDDU_CTRL.start_all` 統一觸發
- Poll 必須使用 unicast（broadcast read 僅在 mask popcount=1 時合法）
- PE program load 必須在 scan-chain 之後、start_all 之前

### 5.4 Code Generation Template（Jinja2）

Codegen 使用 Jinja2 模板產生 C code。以下為 `firmware_layer.c.j2` 的結構：

```jinja2
/* Auto-generated by hybridacc-cc — DO NOT EDIT */
#include "firmware_hw.h"
#include "firmware_payload.h"

{% for layer in layers %}
void layer_{{ layer.name }}(void) {
    /* Step 1: Cluster mask */
    set_cluster_mask({{ "0x%08X" | format(layer.cluster_mask_lo) }}u,
                     {{ "0x%08X" | format(layer.cluster_mask_hi) }}u);

    /* Step 2: SPM config */
    bcast_write32(SPM_CONFIG_MAP,    {{ "0x%02X" | format(layer.spm_config_map) }}u);
    bcast_write32(SPM_CONFIG_UPDATE, 0x01u);

    /* Step 3: HDDU soft-reset */
    bcast_write32(HDDU_BASE + HDDU_CTRL, (1u << 0));

    /* Step 4-7: AGU Banks */
{% for bank_name, bank in [("PS", layer.agu_ps), ("PD", layer.agu_pd), ("PLI", layer.agu_pli), ("PLO", layer.agu_plo)] %}
{% set bank_idx = loop.index0 %}
    /* AGU {{ bank_name }} (Bank {{ bank_idx }}) */
    bcast_agu_write({{ bank_idx }}u, AGU_REG_BASE_ADDR,   {{ "0x%08X" | format(bank.base_addr) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_BASE_ADDR_H, 0x00000000u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_ITER01,      {{ "0x%08X" | format(bank.iter01) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_ITER23,      {{ "0x%08X" | format(bank.iter23) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_STRIDE0,     {{ "0x%08X" | format(bank.stride0) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_STRIDE1,     {{ "0x%08X" | format(bank.stride1) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_STRIDE2,     {{ "0x%08X" | format(bank.stride2) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_STRIDE3,     {{ "0x%08X" | format(bank.stride3) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_LANE_CFG,    {{ "0x%08X" | format(bank.lane_cfg) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_TAG_BASE,    {{ "0x%08X" | format(bank.tag_base) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_TAG_STRIDE0, {{ "0x%08X" | format(bank.tag_stride0) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_TAG_STRIDE1, {{ "0x%08X" | format(bank.tag_stride1) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_TAG_CTRL,    {{ "0x%08X" | format(bank.tag_ctrl) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_MASK_CFG,    {{ "0x%08X" | format(bank.mask_cfg) }}u);
    bcast_agu_write({{ bank_idx }}u, AGU_REG_CTRL,        {{ "0x%08X" | format(bank.ctrl) }}u);

{% endfor %}
    /* Step 8: HDDU global */
    bcast_write32(HDDU_BASE + HDDU_PLANE_EN,   {{ "0x%08X" | format(layer.hddu_plane_en) }}u);
    bcast_write32(HDDU_BASE + HDDU_PLANE_MODE, {{ "0x%08X" | format(layer.hddu_plane_mode) }}u);

    /* Step 9: NoC Reset & Init */
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_RESET, 0));
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_INIT,  0));

    /* Step 10: Scan chain (reverse order) */
{% for entry in layer.scan_chain_reversed %}
    bcast_write32(NOC_CMD, pack_scan_chain(
        {{ entry.ps_id }}u, {{ entry.pd_id }}u,
        {{ entry.pli_id }}u, {{ entry.plo_id }}u,
        {{ entry.route_mode }}u, {{ 1 if entry.enable else 0 }}u));
{% endfor %}

    /* Step 11: Load PE program */
    for (uint32_t i = 0; i < {{ layer.pe_prog_len }}u; i++) {
        bcast_write32(NOC_CMD, pack_load_program(
            (uint16_t)(i * 2u), {{ layer.pe_prog_symbol }}[i]));
    }

    /* Step 12: Start PE */
    bcast_write32(NOC_CMD, pack_noc_cmd(NOC_CMD_START_PE, 0));

    /* Step 13: Start HDDU */
    bcast_write32(HDDU_BASE + HDDU_CTRL, (1u << 1));

    /* Step 14: Wait completion */
    wait_all_clusters_done({{ layer.num_clusters }}u);
}

{% endfor %}
```

---

## 6. `firmware_main.c` — 主程式

### 6.1 結構

```c
/* Auto-generated by hybridacc-cc — DO NOT EDIT */
#include "firmware_hw.h"

/* Forward declarations */
void layer_conv1(void);
void layer_conv2(void);
void layer_fc1(void);

/* ═══════════════════════════════════════════════════════
 * Trap Handler (minimal)
 * ═══════════════════════════════════════════════════════ */
void __attribute__((interrupt("machine"), aligned(4)))
trap_handler(void) {
    /* In this minimal firmware, traps indicate a fatal error.
     * Write error code to CORE_STATUS and halt. */
    REG_CORE_STATUS = 0xDEAD0001u;
    while (1) { __asm__ volatile("wfi"); }
}

/* ═══════════════════════════════════════════════════════
 * Entry Point
 * ═══════════════════════════════════════════════════════ */
void __attribute__((naked, section(".text.start")))
_start(void) {
    __asm__ volatile(
        /* Set up stack pointer */
        "la sp, _stack_top\n"
        /* Set mtvec to trap_handler (direct mode) */
        "la t0, trap_handler\n"
        "csrw mtvec, t0\n"
        /* Jump to main */
        "j main\n"
    );
}

void main(void) {
    /* Signal firmware start */
    REG_CORE_STATUS = 0x00000001u;

    /* Execute layers in order */
    layer_conv1();
    layer_conv2();
    layer_fc1();

    /* Signal completion */
    REG_CORE_STATUS = 0x00000002u;

    /* Halt */
    while (1) { __asm__ volatile("wfi"); }
}
```

### 6.2 啟動流程

```
_start (address 0x00000000):
  1. 設定 sp = _stack_top (linker 提供)
  2. 設定 mtvec = trap_handler (CSR write)
  3. 跳轉到 main()

main():
  4. 寫 CORE_STATUS = 1 (firmware running)
  5. 依序呼叫每個 layer 函數
  6. 寫 CORE_STATUS = 2 (all done)
  7. wfi loop
```

### 6.3 CSR 使用

本韌體僅使用以下 CSR：

| CSR | 用途 |
|-----|------|
| `mtvec` | 設定 trap handler 地址 |
| `mstatus` | 未主動操作（reset 後 MIE=0，中斷關閉） |

IRQ 處理在初版中不使用（改用 polling）。未來可加入 PLIC IRQ 驅動的 layer 完成通知。

---

## 7. Jinja2 Template 資料注入

Codegen 模組需要將 `HardwareIR` 轉換為 Jinja2 template context：

```python
def prepare_template_context(hw_ir: HardwareIR) -> Dict:
    layers = []
    for layer_cfg in hw_ir.layers:
        # Pack AGU iter/stride into 32-bit register values
        def pack_agu(agu: AguBankConfig) -> Dict:
            return {
                "base_addr": agu.base_addr,
                "iter01": ((agu.iter1 & 0xFFFF) << 16) | (agu.iter0 & 0xFFFF),
                "iter23": ((agu.iter3 & 0xFFFF) << 16) | (agu.iter2 & 0xFFFF),
                "stride0": agu.stride0 & 0xFFFFFFFF,
                "stride1": agu.stride1 & 0xFFFFFFFF,
                "stride2": agu.stride2 & 0xFFFFFFFF,
                "stride3": agu.stride3 & 0xFFFFFFFF,
                "lane_cfg": agu.lane_cfg,
                "tag_base": agu.tag_base,
                "tag_stride0": agu.tag_stride0,
                "tag_stride1": agu.tag_stride1,
                "tag_ctrl": agu.tag_ctrl,
                "mask_cfg": agu.mask_cfg,
                "ctrl": (1 << 3) if agu.ultra else 0,
            }

        layers.append({
            "name": layer_cfg.name,
            "cluster_mask_lo": layer_cfg.target_cluster_mask & 0xFFFFFFFF,
            "cluster_mask_hi": (layer_cfg.target_cluster_mask >> 32) & 0xFFFFFFFF,
            "spm_config_map": layer_cfg.spm_config_map,
            "agu_ps": pack_agu(layer_cfg.agu_ps),
            "agu_pd": pack_agu(layer_cfg.agu_pd),
            "agu_pli": pack_agu(layer_cfg.agu_pli),
            "agu_plo": pack_agu(layer_cfg.agu_plo),
            "hddu_plane_en": layer_cfg.hddu.plane_en,
            "hddu_plane_mode": layer_cfg.hddu.plane_mode,
            "scan_chain_reversed": list(reversed(layer_cfg.scan_chain)),
            "pe_prog_symbol": f"pe_prog_{layer_cfg.name}",
            "pe_prog_len": f"PE_PROG_{layer_cfg.name.upper()}_LEN",
            "num_clusters": hw_ir.hardware.num_clusters,
        })

    return {"layers": layers}
```

---

## 8. 程式碼大小估算

每個 layer 函數的 MMIO store 數量：

| 類別 | 指令數 |
|------|--------|
| Cluster mask | 2 |
| SPM config | 2 |
| HDDU reset | 1 |
| AGU × 4 banks × 15 regs | 60 |
| HDDU global | 2 |
| NoC reset + init | 2 |
| Scan chain | `num_pes` |
| Load program | `prog_len` |
| Start PE + Start HDDU | 2 |
| Poll loop | ~10 instructions |

每個 MMIO store 編譯後約 3~4 RV32I instructions（`lui` + `addi` + `sw` 或 `lui` + `sw`），因此單一 layer 的大約 code size：

`(60 + num_pes + prog_len + 21) × 4 × 3 ≈ (num_pes + prog_len + 81) × 12 bytes`

例如 64 PEs + 36 instructions ≈ (64 + 36 + 81) × 12 = 2172 bytes per layer。

I-SRAM 為 16KB（16384 bytes），因此大約可容納 7 個 layer（含 main 和 trap handler）。超過此限制時需要：
1. 使用函數呼叫抽出共用程式碼
2. 使用 data-driven loop 取代展開的 MMIO sequence
3. 增加 I-SRAM 容量

---

## 9. 優化策略

### 9.1 Code Size 優化

若 layer 數量多導致 I-SRAM 溢出，codegen 可切換為 **data-driven 模式**：

```c
/* 不展開每個 AGU write，改用 data table + loop */
static const uint32_t agu_ps_regs[] = {
    /* REG_BASE_ADDR, val, REG_ITER01, val, ... */
    AGU_REG_BASE_ADDR,   0x00000000u,
    AGU_REG_BASE_ADDR_H, 0x00000000u,
    AGU_REG_ITER01,      0x00030004u,
    /* ... */
};

void write_agu(uint32_t bank, const uint32_t* table, uint32_t count) {
    for (uint32_t i = 0; i < count; i += 2) {
        bcast_agu_write(bank, table[i], table[i + 1]);
    }
}
```

此模式將 AGU 配置數據放到 Data-SRAM 的 .rodata section，code 只保留一個通用 loop。

### 9.2 Runtime Performance 優化

- 所有 broadcast write 都是 non-blocking（AHB pipelined）
- Scan-chain 和 PE program load 的 MMIO write 是主要耗時
- 未來可加入 DMA-assisted program load（由 DMA engine 批量寫入 NoC command window）
