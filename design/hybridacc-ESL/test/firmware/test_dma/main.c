/**
 * @file main.c
 * @brief DMA engine tests: DRAM ↔ Cluster SPM loopback.
 *
 * Architecture note:
 *   ComputeCluster has two separate address spaces:
 *     - AXI data path (64-bit) → SPM SRAM banks (used by DMA)
 *     - AHB cmd  path (32-bit) → config regs / HDDU / NoC (used by MMIO)
 *   Firmware MMIO (AHB) **cannot** read/write SPM SRAM data directly.
 *
 * Strategy: loopback test
 *   Step 1: DMA DRAM→SPM   (TB pre-loads pattern word[i]=i+1 at DRAM src)
 *   Step 2: DMA SPM→DRAM   (same SPM offset → different DRAM dst)
 *   Step 3: C++ testbench verifies DRAM dst == DRAM src post-sim.
 *
 * Firmware checks: DMA status, done_tag, err_code, idle flag.
 * Both steps use PLIC external interrupt (source 2 = DMA).
 */

#include "hacc_test.h"

/* ---- DMA constants ---- */
#define DMA_ENDPOINT_DRAM        0
#define DMA_ENDPOINT_CLUSTER_SPM 1
#define DMA_CTRL_SUBMIT          (1u << 0)
#define DMA_CTRL_IRQ_EN          (1u << 3)
#define DMA_STATUS_IDLE          (1u << 0)

/* One beat = 8 bytes (cluster AXI data bus is 64-bit) */
#define BEAT_BYTES               8u

/* PLIC source IDs (NUM_CLUSTERS=1, NUM_NLU=0) */
#define PLIC_SRC_DMA             2

/* Test parameters — must match testbench C++ code */
#define TEST_DRAM_SRC            0x80020000u
#define TEST_DRAM_DST            0x80030000u
#define TEST_BYTES               64u
#define TEST_BEATS               (TEST_BYTES / BEAT_BYTES)  /* 8 */

/* SPM byte offset used by the loopback */
#define SPM_LOOPBACK_OFFSET      0u

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

    /* T001: DMA status should be idle before any transfer */
    TEST_ASSERT(1, mmio_read(DMA_STATUS) & DMA_STATUS_IDLE);

    /* ================================================================
     * Step 1: DRAM → Cluster SPM  (loopback — first half)
     *
     * TB pre-loaded word[i] = i+1 at DRAM 0x80020000.
     * DMA writes into SPM at SPM_LOOPBACK_OFFSET via AXI data path.
     * ================================================================ */
    g_dma_done = 0;

    dma_submit_4d(
        DMA_ENDPOINT_DRAM,          /* src = DRAM */
        DMA_ENDPOINT_CLUSTER_SPM,   /* dst = Cluster SPM */
        TEST_DRAM_SRC,              /* src_addr (DRAM physical) */
        SPM_LOOPBACK_OFFSET,        /* dst_addr (cluster-local byte offset) */
        0,                          /* src_cluster_id (unused for DRAM) */
        0,                          /* dst_cluster_id = 0 */
        TEST_BEATS,                 /* count_d0 = 8 beats */
        1, 1, 1,                    /* count_d1/d2/d3 = 1 (flat 1D) */
        BEAT_BYTES, 0, 0, 0,       /* src strides: contiguous */
        BEAT_BYTES, 0, 0, 0,       /* dst strides: contiguous */
        1                           /* tag = 1 */
    );

    wait_dma_done();

    /* T002: DRAM→SPM completed with correct tag */
    TEST_EQ(2, mmio_read(DMA_DONE_TAG), 1);
    /* T003: No DMA error */
    TEST_EQ(3, mmio_read(DMA_ERR_CODE), 0);
    /* T004: DMA should be idle after completion */
    TEST_ASSERT(4, mmio_read(DMA_STATUS) & DMA_STATUS_IDLE);

    /* ================================================================
     * Step 2: Cluster SPM → DRAM  (loopback — second half)
     *
     * Read from same SPM offset, write to DRAM dst.
     * C++ testbench will verify DRAM dst == original DRAM src post-sim.
     * ================================================================ */
    g_dma_done = 0;

    dma_submit_4d(
        DMA_ENDPOINT_CLUSTER_SPM,   /* src = Cluster SPM */
        DMA_ENDPOINT_DRAM,          /* dst = DRAM */
        SPM_LOOPBACK_OFFSET,        /* src_addr (cluster-local byte offset) */
        TEST_DRAM_DST,              /* dst_addr (DRAM physical) */
        0,                          /* src_cluster_id = 0 */
        0,                          /* dst_cluster_id (unused for DRAM) */
        TEST_BEATS,                 /* count_d0 = 8 beats */
        1, 1, 1,                    /* count_d1/d2/d3 = 1 (flat 1D) */
        BEAT_BYTES, 0, 0, 0,       /* src strides: contiguous */
        BEAT_BYTES, 0, 0, 0,       /* dst strides: contiguous */
        2                           /* tag = 2 */
    );

    wait_dma_done();

    /* T005: SPM→DRAM completed with correct tag */
    TEST_EQ(5, mmio_read(DMA_DONE_TAG), 2);
    /* T006: No DMA error */
    TEST_EQ(6, mmio_read(DMA_ERR_CODE), 0);

    /* Note: DRAM dst contents verified by C++ testbench post-sim
     *       (expects loopback: word[i] = i+1, matching DRAM src) */

    TEST_FINISH();
}
