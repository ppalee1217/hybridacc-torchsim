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

#define TEST_PAD_DRAM_SRC        0x80040000u
#define TEST_PAD_DRAM_DST        0x80050000u
#define TEST_PAD_SRC_H           2u
#define TEST_PAD_SRC_W           2u
#define TEST_PAD_TILE_H          4u
#define TEST_PAD_TILE_W          4u
#define TEST_PAD_BEATS           (TEST_PAD_TILE_H * TEST_PAD_TILE_W)

#define TEST_RELU_DRAM_SRC       0x80060000u
#define TEST_RELU_DRAM_DST       0x80070000u
#define TEST_RELU_BEATS          4u

/* SPM byte offset used by the loopback */
#define SPM_LOOPBACK_OFFSET      0u
#define SPM_PAD_OFFSET           0x100u
#define SPM_RELU_OFFSET          0x200u

/* ---- Global flag set by ISR ---- */
volatile uint32_t g_dma_done;

/* ========================================================================
 * Trap handler — called by trap_entry.S
 *
 * trap_entry.S returns with MRET, so interrupts resume from mepc directly.
 * ======================================================================== */

void trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    (void)mepc;
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
    }
    /* Synchronous exceptions are not expected in this test. */
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

static void dma_clear_transform(void)
{
    mmio_write(DMA_XFORM_CTRL, 0);
    mmio_write(DMA_PAD_WINDOW_H0, 0);
    mmio_write(DMA_PAD_WINDOW_W0, 0);
    mmio_write(DMA_PAD_SRC_H, 0);
    mmio_write(DMA_PAD_SRC_W, 0);
    mmio_write(DMA_BEATS_PER_PIXEL, 0);
    mmio_write(DMA_FILL_VALUE_LO, 0);
    mmio_write(DMA_FILL_VALUE_HI, 0);
    mmio_write(DMA_EPILOGUE_CTRL, DMA_EPILOGUE_NONE);
    mmio_write(DMA_EPILOGUE_PARAM0, 0);
}

static void dma_config_load_pad(int32_t window_h0, int32_t window_w0,
                                uint32_t src_h, uint32_t src_w,
                                uint32_t beats_per_pixel,
                                uint64_t fill_value,
                                uint32_t fill_mode)
{
    mmio_write(DMA_XFORM_CTRL, DMA_XFORM_LOAD_PAD_EN | DMA_XFORM_FILL_MODE(fill_mode));
    mmio_write(DMA_PAD_WINDOW_H0, (uint32_t)window_h0);
    mmio_write(DMA_PAD_WINDOW_W0, (uint32_t)window_w0);
    mmio_write(DMA_PAD_SRC_H, src_h);
    mmio_write(DMA_PAD_SRC_W, src_w);
    mmio_write(DMA_BEATS_PER_PIXEL, beats_per_pixel);
    mmio_write(DMA_FILL_VALUE_LO, (uint32_t)(fill_value & 0xFFFFFFFFu));
    mmio_write(DMA_FILL_VALUE_HI, (uint32_t)(fill_value >> 32));
}

static void dma_config_epilogue(uint32_t mode)
{
    mmio_write(DMA_EPILOGUE_CTRL, mode);
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
    dma_clear_transform();

    /* T001: DMA status should be idle before any transfer */
    TEST_ASSERT(1, mmio_read(DMA_STATUS) & DMA_STATUS_IDLE);

    /* ================================================================
     * Step 1: DRAM → Cluster SPM  (loopback — first half)
     *
     * TB pre-loaded word[i] = i+1 at DRAM 0x80020000.
     * DMA writes into SPM at SPM_LOOPBACK_OFFSET via AXI data path.
     * ================================================================ */
    g_dma_done = 0;
    dma_clear_transform();

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
    dma_clear_transform();

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

    /* ================================================================
     * Step 3: DRAM -> SPM with load-side zero halo synthesis.
     * Raw source is 2x2 beats, destination tile is 4x4 beats.
     * ================================================================ */
    g_dma_done = 0;
    dma_clear_transform();
    dma_config_load_pad(-1, -1,
                        TEST_PAD_SRC_H, TEST_PAD_SRC_W,
                        1, 0, DMA_XFORM_FILL_ZERO);

    dma_submit_4d(
        DMA_ENDPOINT_DRAM,
        DMA_ENDPOINT_CLUSTER_SPM,
        TEST_PAD_DRAM_SRC,
        SPM_PAD_OFFSET,
        0, 0,
        TEST_PAD_TILE_W,
        TEST_PAD_TILE_H,
        1, 1,
        BEAT_BYTES, TEST_PAD_SRC_W * BEAT_BYTES, 0, 0,
        BEAT_BYTES, TEST_PAD_TILE_W * BEAT_BYTES, 0, 0,
        3
    );

    wait_dma_done();

    /* T007: load-pad completed with correct tag */
    TEST_EQ(7, mmio_read(DMA_DONE_TAG), 3);
    /* T008: load-pad should not report transform error */
    TEST_EQ(8, mmio_read(DMA_ERR_CODE), 0);

    /* Step 4: write back padded tile for C++ post-check */
    g_dma_done = 0;
    dma_clear_transform();

    dma_submit_4d(
        DMA_ENDPOINT_CLUSTER_SPM,
        DMA_ENDPOINT_DRAM,
        SPM_PAD_OFFSET,
        TEST_PAD_DRAM_DST,
        0, 0,
        TEST_PAD_TILE_W,
        TEST_PAD_TILE_H,
        1, 1,
        BEAT_BYTES, TEST_PAD_TILE_W * BEAT_BYTES, 0, 0,
        BEAT_BYTES, TEST_PAD_TILE_W * BEAT_BYTES, 0, 0,
        4
    );

    wait_dma_done();

    /* T009: padded tile writeback completed */
    TEST_EQ(9, mmio_read(DMA_DONE_TAG), 4);
    /* T010: padded tile writeback no error */
    TEST_EQ(10, mmio_read(DMA_ERR_CODE), 0);

    /* ================================================================
     * Step 5: DRAM -> SPM copy of fp16 lanes for store-side ReLU.
     * Step 6: SPM -> DRAM with ReLU epilogue enabled.
     * ================================================================ */
    g_dma_done = 0;
    dma_clear_transform();

    dma_submit_4d(
        DMA_ENDPOINT_DRAM,
        DMA_ENDPOINT_CLUSTER_SPM,
        TEST_RELU_DRAM_SRC,
        SPM_RELU_OFFSET,
        0, 0,
        TEST_RELU_BEATS,
        1, 1, 1,
        BEAT_BYTES, 0, 0, 0,
        BEAT_BYTES, 0, 0, 0,
        5
    );

    wait_dma_done();

    /* T011: ReLU source preload completed */
    TEST_EQ(11, mmio_read(DMA_DONE_TAG), 5);
    /* T012: ReLU source preload no error */
    TEST_EQ(12, mmio_read(DMA_ERR_CODE), 0);

    g_dma_done = 0;
    dma_clear_transform();
    dma_config_epilogue(DMA_EPILOGUE_RELU);

    dma_submit_4d(
        DMA_ENDPOINT_CLUSTER_SPM,
        DMA_ENDPOINT_DRAM,
        SPM_RELU_OFFSET,
        TEST_RELU_DRAM_DST,
        0, 0,
        TEST_RELU_BEATS,
        1, 1, 1,
        BEAT_BYTES, 0, 0, 0,
        BEAT_BYTES, 0, 0, 0,
        6
    );

    wait_dma_done();

    /* T013: ReLU writeback completed */
    TEST_EQ(13, mmio_read(DMA_DONE_TAG), 6);
    /* T014: ReLU writeback no error */
    TEST_EQ(14, mmio_read(DMA_ERR_CODE), 0);

    /* Note: DRAM dst contents verified by C++ testbench post-sim
     *       (expects loopback: word[i] = i+1, matching DRAM src) */

    TEST_FINISH();
}
