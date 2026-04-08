/**
 * @file main.c
 * @brief DMA engine tests: DRAM ↔ Cluster SPM with interrupt-driven completion.
 *
 * Test 1: DRAM → Cluster SPM (64 bytes = 8 beats, 1D contiguous)
 *         TB pre-loads pattern word[i] = i+1 at DRAM 0x80020000.
 *         Firmware verifies cluster SPM via MMIO reads.
 *
 * Test 2: Cluster SPM → DRAM (64 bytes = 8 beats, 1D contiguous)
 *         Firmware writes pattern word[i] = 0xA0+i to cluster SPM offset 0x100.
 *         TB verifies DRAM at 0x80030000 post-sim.
 *
 * Both tests use PLIC external interrupt (source 2 = DMA) with trap handler.
 */

#include "hacc_test.h"

/* ---- DMA constants ---- */
#define DMA_ENDPOINT_DRAM        0
#define DMA_ENDPOINT_CLUSTER_SPM 1
#define DMA_CTRL_SUBMIT          (1u << 0)
#define DMA_CTRL_IRQ_EN          (1u << 3)
#define DMA_STATUS_IDLE          (1u << 0)
#define DMA_STATUS_BUSY          (1u << 1)

/* One beat = 8 bytes (cluster bus is 64-bit) */
#define BEAT_BYTES               8u

/* PLIC source IDs (NUM_CLUSTERS=1, NUM_NLU=0) */
#define PLIC_SRC_DMA             2

/* Test parameters — must match testbench C++ code */
#define TEST_DRAM_SRC            0x80020000u
#define TEST_DRAM_DST            0x80030000u
#define TEST_BYTES               64u
#define TEST_BEATS               (TEST_BYTES / BEAT_BYTES)  /* 8 */
#define TEST_WORDS               (TEST_BYTES / 4)

/* Cluster SPM offsets for test 2 write area */
#define CL_SPM_TEST2_OFFSET      0x100u

/* ---- Global flag set by ISR ---- */
volatile uint32_t g_dma_done;

/* ========================================================================
 * Trap handler — called by trap_entry.S
 *
 * trap_entry.S unconditionally adds 4 to mepc after this returns.
 * For interrupts we compensate by writing (mepc − 4) back to the CSR
 * so the net effect is returning to the interrupted PC.
 * ======================================================================== */

void trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    (void)mtval;

    if (mcause & MCAUSE_INT) {
        uint32_t cause_code = mcause & 0x7FFFFFFFu;
        if (cause_code == 11) {              /* Machine External Interrupt */
            uint32_t src = mmio_read(PLIC_CLAIM_COMPLETE);
            if (src == PLIC_SRC_DMA) {
                g_dma_done = 1;
            }
            if (src != 0) {
                mmio_write(PLIC_CLAIM_COMPLETE, src);  /* complete */
            }
        }
        /* Compensate for trap_entry.S mepc += 4 */
        CSR_WRITE(0x341, mepc - 4);
    }
    /* Synchronous exceptions: let trap_entry advance mepc by 4 */
}

/* ========================================================================
 * Helpers
 * ======================================================================== */

static void setup_interrupts(void)
{
    /* Set mtvec to trap_entry (linked by linker script) */
    extern void trap_entry(void);
    CSR_WRITE(0x305, (uint32_t)&trap_entry);

    /* Enable DMA IRQ in PLIC (source 2) */
    mmio_write(PLIC_ENABLE_LO, 1u << PLIC_SRC_DMA);
    mmio_write(PLIC_PRIORITY_BASE + 4 * PLIC_SRC_DMA, 1);
    mmio_write(PLIC_THRESHOLD, 0);

    /* Enable external interrupts in core */
    CSR_SET(0x304, MIE_MEIE);       /* mie.MEIE = 1 */
    CSR_SET(0x300, MSTATUS_MIE);    /* mstatus.MIE = 1 */
}

static void dma_submit_4d(uint32_t src_kind, uint32_t dst_kind,
                          uint32_t src_addr, uint32_t dst_addr,
                          uint32_t src_cid, uint32_t dst_cid,
                          uint32_t count_d0, uint32_t count_d1,
                          uint32_t count_d2, uint32_t count_d3,
                          uint32_t src_s0, uint32_t src_s1,
                          uint32_t src_s2, uint32_t src_s3,
                          uint32_t dst_s0, uint32_t dst_s1,
                          uint32_t dst_s2, uint32_t dst_s3,
                          uint32_t tag)
{
    mmio_write(DMA_SRC_KIND,       src_kind);
    mmio_write(DMA_DST_KIND,       dst_kind);
    mmio_write(DMA_SRC_ADDR_LO,    src_addr);
    mmio_write(DMA_SRC_ADDR_HI,    0);
    mmio_write(DMA_DST_ADDR_LO,    dst_addr);
    mmio_write(DMA_DST_ADDR_HI,    0);
    mmio_write(DMA_SRC_CLUSTER_ID, src_cid);
    mmio_write(DMA_DST_CLUSTER_ID, dst_cid);
    mmio_write(DMA_COUNT_D0,       count_d0);
    mmio_write(DMA_COUNT_D1,       count_d1);
    mmio_write(DMA_COUNT_D2,       count_d2);
    mmio_write(DMA_COUNT_D3,       count_d3);
    mmio_write(DMA_SRC_STRIDE_D0,  src_s0);
    mmio_write(DMA_SRC_STRIDE_D1,  src_s1);
    mmio_write(DMA_SRC_STRIDE_D2,  src_s2);
    mmio_write(DMA_SRC_STRIDE_D3,  src_s3);
    mmio_write(DMA_DST_STRIDE_D0,  dst_s0);
    mmio_write(DMA_DST_STRIDE_D1,  dst_s1);
    mmio_write(DMA_DST_STRIDE_D2,  dst_s2);
    mmio_write(DMA_DST_STRIDE_D3,  dst_s3);
    mmio_write(DMA_CMD_TAG,        tag);
    mmio_write(DMA_CTRL,           DMA_CTRL_SUBMIT | DMA_CTRL_IRQ_EN);
}

static void wait_dma_done(void)
{
    while (!g_dma_done) {
        /* spin — ISR sets g_dma_done on DMA completion interrupt */
    }
}

/* ========================================================================
 * Main
 * ======================================================================== */

void main(void)
{
    TEST_INIT();

    setup_interrupts();

    /* ================================================================
     * Test 1: DRAM → Cluster SPM
     * ================================================================ */
    g_dma_done = 0;

    dma_submit_4d(
        DMA_ENDPOINT_DRAM,          /* src = DRAM */
        DMA_ENDPOINT_CLUSTER_SPM,   /* dst = Cluster SPM */
        TEST_DRAM_SRC,              /* src_addr (DRAM physical) */
        0,                          /* dst_addr (cluster-local offset) */
        0,                          /* src_cluster_id (unused for DRAM) */
        0,                          /* dst_cluster_id = 0 */
        TEST_BEATS,                 /* count_d0 = 8 beats */
        1, 1, 1,                    /* count_d1/d2/d3 = 1 (flat 1D) */
        BEAT_BYTES, 0, 0, 0,       /* src strides: contiguous, rest unused */
        BEAT_BYTES, 0, 0, 0,       /* dst strides: contiguous, rest unused */
        1                           /* tag = 1 */
    );

    wait_dma_done();

    /* Verify: read cluster[0] SPM via MMIO and compare */
    {
        uint32_t ok = 1;
        for (uint32_t i = 0; i < TEST_WORDS; i++) {
            uint32_t expected = i + 1;
            uint32_t actual   = mmio_read(CLUSTER_BASE + i * 4);
            if (actual != expected) {
                ok = 0;
            }
        }
        TEST_ASSERT(1, ok);
    }

    /* Check DMA completion status */
    TEST_EQ(2, mmio_read(DMA_DONE_TAG), 1);
    TEST_EQ(3, mmio_read(DMA_ERR_CODE), 0);

    /* ================================================================
     * Test 2: Cluster SPM → DRAM
     * ================================================================ */

    /* Write known pattern to cluster[0] SPM at offset 0x100 */
    for (uint32_t i = 0; i < TEST_WORDS; i++) {
        mmio_write(CLUSTER_BASE + CL_SPM_TEST2_OFFSET + i * 4, 0xA0 + i);
    }

    g_dma_done = 0;

    dma_submit_4d(
        DMA_ENDPOINT_CLUSTER_SPM,   /* src = Cluster SPM */
        DMA_ENDPOINT_DRAM,          /* dst = DRAM */
        CL_SPM_TEST2_OFFSET,       /* src_addr (cluster-local offset) */
        TEST_DRAM_DST,              /* dst_addr (DRAM physical) */
        0,                          /* src_cluster_id = 0 */
        0,                          /* dst_cluster_id (unused for DRAM) */
        TEST_BEATS,                 /* count_d0 = 8 beats */
        1, 1, 1,                    /* count_d1/d2/d3 = 1 (flat 1D) */
        BEAT_BYTES, 0, 0, 0,       /* src strides: contiguous, rest unused */
        BEAT_BYTES, 0, 0, 0,       /* dst strides: contiguous, rest unused */
        2                           /* tag = 2 */
    );

    wait_dma_done();

    /* Check DMA completion status */
    TEST_EQ(4, mmio_read(DMA_DONE_TAG), 2);
    TEST_EQ(5, mmio_read(DMA_ERR_CODE), 0);

    /* Note: DRAM contents are verified by the C++ testbench post-sim */

    TEST_FINISH();
}
