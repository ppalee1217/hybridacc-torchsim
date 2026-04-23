/**
 * @file hacc_test.h
 * @brief Common test infrastructure for HybridAcc CoreMcu firmware tests.
 *
 * Provides MMIO register addresses, test assertion macros, and result
 * reporting through Data SRAM.
 *
 * Convention:
 *   - Test results are written to DSRAM starting at TEST_RESULT_BASE (0x10000000).
 *   - Word[0] = total test count
 *   - Word[1] = pass count
 *   - Word[2] = fail count
 *   - Word[3] = first failing test ID (0 if all pass)
 *   - Word[4..] = per-test results: 0=PASS, non-zero=fail-code
 */

#ifndef HACC_TEST_H
#define HACC_TEST_H

#include <stdint.h>

/* ========================================================================
 * Memory Map
 * ======================================================================== */

#define DSRAM_BASE          0x10000000u
#define DSRAM_SIZE          0x00010000u  /* 64 KB */

#define LOCAL_CTRL_BASE     0x20000000u
#define DMA_MMIO_BASE       0x20001000u
#define LOCAL_TIMER_BASE    0x20002000u
#define PLIC_BASE           0x0C000000u
#define CLUSTER_BASE        0x40000000u
#define CLUSTER_STRIDE      0x00010000u
#define CLUSTER_BCAST_BASE  0x50000000u
#define NLU_BASE            0x60000000u

/* ---- Cluster control offsets ---- */
#define CLUSTER_MODE_OFF        0x2100u
#define CLUSTER_CTRL_OFF        0x2104u
#define CLUSTER_STATUS_OFF      0x2108u
#define CLUSTER_ERROR_CODE_OFF  0x210Cu
#define CLUSTER_SUBSTATE_OFF    0x2110u

#define CLUSTER_MODE_DIRECT_DEBUG   0u
#define CLUSTER_MODE_LAYER_MANAGED  1u

#define CLUSTER_CTRL_START       (1u << 0)
#define CLUSTER_CTRL_STOP        (1u << 1)
#define CLUSTER_CTRL_SOFT_RESET  (1u << 2)

#define CLUSTER_STATUS_IDLE      (1u << 0)
#define CLUSTER_STATUS_BUSY      (1u << 1)
#define CLUSTER_STATUS_DONE      (1u << 2)
#define CLUSTER_STATUS_QUIESCED  (1u << 3)
#define CLUSTER_STATUS_ERROR     (1u << 4)

/* ---- Local Control registers (base 0x20000000) ---- */
#define LOCAL_CLUSTER_MASK_LO  (LOCAL_CTRL_BASE + 0x000)
#define LOCAL_CLUSTER_MASK_HI  (LOCAL_CTRL_BASE + 0x004)
#define LOCAL_MMIO_ERR_STATUS  (LOCAL_CTRL_BASE + 0x008)
#define LOCAL_LAST_TARGET_ID   (LOCAL_CTRL_BASE + 0x00C)
#define LOCAL_LAST_FAULT_ADDR  (LOCAL_CTRL_BASE + 0x010)
#define LOCAL_LAST_FAULT_INFO  (LOCAL_CTRL_BASE + 0x014)
#define LOCAL_BOOT_REASON      (LOCAL_CTRL_BASE + 0x018)
#define LOCAL_FABRIC_CAP0      (LOCAL_CTRL_BASE + 0x01C)

/* ---- DMA registers (base 0x20001000) ---- */
#define DMA_CAP0            (DMA_MMIO_BASE + 0x000)
#define DMA_STATUS          (DMA_MMIO_BASE + 0x004)
#define DMA_CTRL            (DMA_MMIO_BASE + 0x008)
#define DMA_SRC_KIND        (DMA_MMIO_BASE + 0x00C)
#define DMA_DST_KIND        (DMA_MMIO_BASE + 0x010)
#define DMA_SRC_ADDR_LO     (DMA_MMIO_BASE + 0x014)
#define DMA_SRC_ADDR_HI     (DMA_MMIO_BASE + 0x018)
#define DMA_DST_ADDR_LO     (DMA_MMIO_BASE + 0x01C)
#define DMA_DST_ADDR_HI     (DMA_MMIO_BASE + 0x020)
#define DMA_SRC_CLUSTER_ID  (DMA_MMIO_BASE + 0x024)
#define DMA_DST_CLUSTER_ID  (DMA_MMIO_BASE + 0x028)
#define DMA_COUNT_D0        (DMA_MMIO_BASE + 0x02C)
#define DMA_COUNT_D1        (DMA_MMIO_BASE + 0x030)
#define DMA_COUNT_D2        (DMA_MMIO_BASE + 0x034)
#define DMA_COUNT_D3        (DMA_MMIO_BASE + 0x038)
#define DMA_SRC_STRIDE_D0   (DMA_MMIO_BASE + 0x03C)
#define DMA_SRC_STRIDE_D1   (DMA_MMIO_BASE + 0x040)
#define DMA_SRC_STRIDE_D2   (DMA_MMIO_BASE + 0x044)
#define DMA_SRC_STRIDE_D3   (DMA_MMIO_BASE + 0x048)
#define DMA_DST_STRIDE_D0   (DMA_MMIO_BASE + 0x04C)
#define DMA_DST_STRIDE_D1   (DMA_MMIO_BASE + 0x050)
#define DMA_DST_STRIDE_D2   (DMA_MMIO_BASE + 0x054)
#define DMA_DST_STRIDE_D3   (DMA_MMIO_BASE + 0x058)
#define DMA_CMD_TAG         (DMA_MMIO_BASE + 0x05C)
#define DMA_DONE_TAG        (DMA_MMIO_BASE + 0x060)
#define DMA_ERR_CODE        (DMA_MMIO_BASE + 0x064)
#define DMA_ERR_INFO        (DMA_MMIO_BASE + 0x068)
#define DMA_DEBUG_STATE     (DMA_MMIO_BASE + 0x06C)
#define DMA_XFORM_CTRL      (DMA_MMIO_BASE + 0x070)
#define DMA_PAD_WINDOW_H0   (DMA_MMIO_BASE + 0x074)
#define DMA_PAD_WINDOW_W0   (DMA_MMIO_BASE + 0x078)
#define DMA_PAD_SRC_H       (DMA_MMIO_BASE + 0x07C)
#define DMA_PAD_SRC_W       (DMA_MMIO_BASE + 0x080)
#define DMA_BEATS_PER_PIXEL (DMA_MMIO_BASE + 0x084)
#define DMA_FILL_VALUE_LO   (DMA_MMIO_BASE + 0x088)
#define DMA_FILL_VALUE_HI   (DMA_MMIO_BASE + 0x08C)
#define DMA_EPILOGUE_CTRL   (DMA_MMIO_BASE + 0x090)
#define DMA_EPILOGUE_PARAM0 (DMA_MMIO_BASE + 0x094)

#define DMA_XFORM_LOAD_PAD_EN   (1u << 0)
#define DMA_XFORM_FILL_MODE_SHIFT 4u
#define DMA_XFORM_FILL_MODE(mode) (((uint32_t)(mode) & 0x3u) << DMA_XFORM_FILL_MODE_SHIFT)

#define DMA_XFORM_FILL_ZERO     0u
#define DMA_XFORM_FILL_EPSILON  1u
#define DMA_XFORM_FILL_CONST    2u

#define DMA_EPILOGUE_NONE       0u
#define DMA_EPILOGUE_RELU       1u

#define DMA_ERR_BAD_XFORM       8u

/* ---- Timer / MSIP registers (base 0x20002000) ---- */
#define TIMER_MSIP          (LOCAL_TIMER_BASE + 0x000)
#define TIMER_MTIMECMP_LO   (LOCAL_TIMER_BASE + 0x004)
#define TIMER_MTIMECMP_HI   (LOCAL_TIMER_BASE + 0x008)
#define TIMER_MTIME_LO      (LOCAL_TIMER_BASE + 0x00C)
#define TIMER_MTIME_HI      (LOCAL_TIMER_BASE + 0x010)
#define TIMER_CTRL          (LOCAL_TIMER_BASE + 0x014)

/* ---- PLIC registers (base 0x0C000000) ---- */
#define PLIC_PRIORITY_BASE  (PLIC_BASE + 0x0000)
#define PLIC_PENDING_LO     (PLIC_BASE + 0x0800)
#define PLIC_PENDING_HI     (PLIC_BASE + 0x0804)
#define PLIC_ENABLE_LO      (PLIC_BASE + 0x1000)
#define PLIC_ENABLE_HI      (PLIC_BASE + 0x1004)
#define PLIC_THRESHOLD       (PLIC_BASE + 0x1800)
#define PLIC_CLAIM_COMPLETE  (PLIC_BASE + 0x1804)
#define PLIC_MAX_SOURCE_ID   (PLIC_BASE + 0x1808)

/* ---- CSR addresses ---- */
#define CSR_MSTATUS     0x300
#define CSR_MISA        0x301
#define CSR_MIE         0x304
#define CSR_MTVEC       0x305
#define CSR_MSCRATCH    0x340
#define CSR_MEPC        0x341
#define CSR_MCAUSE      0x342
#define CSR_MTVAL       0x343
#define CSR_MIP         0x344
#define CSR_MCYCLE      0xB00
#define CSR_MINSTRET    0xB02

/* mstatus bits */
#define MSTATUS_MIE     (1u << 3)
#define MSTATUS_MPIE    (1u << 7)

/* mie / mip bits */
#define MIE_MSIE        (1u << 3)
#define MIE_MTIE        (1u << 7)
#define MIE_MEIE        (1u << 11)

/* mcause interrupt bit */
#define MCAUSE_INT      (1u << 31)

/* ========================================================================
 * MMIO helpers
 * ======================================================================== */

#define REG32(addr)  (*(volatile uint32_t *)(addr))

static inline uint32_t mmio_read(uint32_t addr)  { return REG32(addr); }
static inline void mmio_write(uint32_t addr, uint32_t val) { REG32(addr) = val; }

static inline uint32_t cluster_addr(uint32_t cluster_id, uint32_t offset) {
    return CLUSTER_BASE + cluster_id * CLUSTER_STRIDE + offset;
}

/* ========================================================================
 * Test result area in DSRAM
 * ======================================================================== */

/* Test results go at the very start of DSRAM */
#define TEST_RESULT_BASE    ((volatile uint32_t *)DSRAM_BASE)
#define TEST_TOTAL_IDX      0
#define TEST_PASS_IDX       1
#define TEST_FAIL_IDX       2
#define TEST_FIRST_FAIL_IDX 3
#define TEST_DATA_START     4

/* Working area starts after result area (256 bytes reserved) */
#define TEST_WORK_BASE      (DSRAM_BASE + 0x100)

/*
 * All test counters live in local variables (registers) to avoid
 * the ESL SRAM store→load timing hazard.  Macros expand to operate
 * on variables _t_total, _t_pass, _t_fail, _t_ff that must be
 * declared by TEST_INIT().  TEST_FINISH() writes them to DSRAM once.
 */

#define TEST_INIT()             \
    uint32_t _t_total = 0;     \
    uint32_t _t_pass  = 0;     \
    uint32_t _t_fail  = 0;     \
    uint32_t _t_ff    = 0

#define TEST_CHECK(test_id, actual, expected) do {  \
    _t_total++;                                     \
    if ((uint32_t)(actual) == (uint32_t)(expected)) \
        _t_pass++;                                  \
    else {                                          \
        _t_fail++;                                  \
        if (_t_ff == 0) _t_ff = (test_id);          \
    }                                               \
} while (0)

#define TEST_EQ(test_id, actual, expected) \
    TEST_CHECK((test_id), (actual), (expected))

#define TEST_ASSERT(test_id, cond) \
    TEST_CHECK((test_id), (cond) ? 1u : 0u, 1u)

#define TEST_FINISH() do {                                          \
    volatile uint32_t *_r = TEST_RESULT_BASE;                       \
    _r[TEST_TOTAL_IDX]      = _t_total;                             \
    _r[TEST_PASS_IDX]       = _t_pass;                              \
    _r[TEST_FAIL_IDX]       = _t_fail;                              \
    _r[TEST_FIRST_FAIL_IDX] = _t_ff;                                \
} while (0)

/* CSR access helpers (inline assembly) */
#define CSR_READ(csr) ({                    \
    uint32_t __val;                         \
    __asm__ volatile ("csrr %0, " #csr      \
                      : "=r"(__val));        \
    __val;                                  \
})

#define CSR_WRITE(csr, val)                 \
    __asm__ volatile ("csrw " #csr ", %0"   \
                      : : "r"((uint32_t)(val)))

#define CSR_SET(csr, val)                   \
    __asm__ volatile ("csrs " #csr ", %0"   \
                      : : "r"((uint32_t)(val)))

#define CSR_CLEAR(csr, val)                 \
    __asm__ volatile ("csrc " #csr ", %0"   \
                      : : "r"((uint32_t)(val)))

#define CSR_SWAP(csr, val) ({               \
    uint32_t __old;                         \
    __asm__ volatile ("csrrw %0, " #csr ", %1" \
                      : "=r"(__old)         \
                      : "r"((uint32_t)(val))); \
    __old;                                  \
})

#endif /* HACC_TEST_H */
