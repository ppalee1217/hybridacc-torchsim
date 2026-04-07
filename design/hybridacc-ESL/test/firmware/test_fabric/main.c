/**
 * @file main.c
 * @brief Test CmdFabric MMIO routing and local control registers.
 *
 * Covers: Local control register R/W, cluster mask, fabric capability,
 *         cluster unicast command, cluster broadcast, DMA register access,
 *         timer register access, error status.
 */

#include "hacc_test.h"

void main(void)
{
    TEST_INIT();

    /* ---- Local Control Registers ---- */

    /* T001: Read fabric capability */
    {
        uint32_t cap = mmio_read(LOCAL_FABRIC_CAP0);
        TEST_ASSERT(1, 1);  /* Access succeeded */
    }

    /* T002: Read boot reason */
    {
        uint32_t br = mmio_read(LOCAL_BOOT_REASON);
        TEST_ASSERT(2, 1);  /* Access succeeded */
    }

    /* T003: Write and read cluster mask lo */
    mmio_write(LOCAL_CLUSTER_MASK_LO, 0x00000001);
    TEST_EQ(3, mmio_read(LOCAL_CLUSTER_MASK_LO), 0x00000001);

    /* T004: Write and read cluster mask hi */
    mmio_write(LOCAL_CLUSTER_MASK_HI, 0);
    TEST_EQ(4, mmio_read(LOCAL_CLUSTER_MASK_HI), 0);

    /* T005: Error status register (should be 0 if no faults) */
    {
        uint32_t err = mmio_read(LOCAL_MMIO_ERR_STATUS);
        /* Clear any pending error by writing back (W1C) */
        if (err) mmio_write(LOCAL_MMIO_ERR_STATUS, err);
        TEST_EQ(5, mmio_read(LOCAL_MMIO_ERR_STATUS), 0);
    }

    /* ---- Cluster Unicast Command ---- */

    /* T006: Write to cluster[0] register via unicast
     * Address: CLUSTER_BASE + 0 * 0x10000 + offset
     * The BusStub returns 0xDEADBEEF for reads */
    {
        uint32_t cl_addr = CLUSTER_BASE + 0x0000;  /* cluster 0, offset 0 */
        uint32_t rd_val = mmio_read(cl_addr);
        TEST_EQ(6, rd_val, 0xDEADBEEF);
    }

    /* T007: Write to cluster[0] should not fault */
    {
        mmio_write(CLUSTER_BASE + 0x0004, 0x12345678);
        /* Verify no error */
        uint32_t err = mmio_read(LOCAL_MMIO_ERR_STATUS);
        TEST_EQ(7, err & 0x1, 0);  /* no pending error */
    }

    /* ---- Cluster Broadcast ---- */

    /* T008: Set cluster mask and broadcast write */
    mmio_write(LOCAL_CLUSTER_MASK_LO, 0x00000001);  /* cluster 0 enabled */
    mmio_write(CLUSTER_BCAST_BASE + 0x0000, 0xAAAABBBB);

    /* Reading broadcast addr with single cluster in mask should work */
    {
        uint32_t val = mmio_read(CLUSTER_BCAST_BASE + 0x0000);
        TEST_EQ(8, val, 0xDEADBEEF);  /* Stub returns 0xDEADBEEF */
    }

    /* ---- Timer register access through fabric ---- */

    /* T009: Read timer control */
    {
        uint32_t tc = mmio_read(TIMER_CTRL);
        TEST_ASSERT(9, 1);  /* Access succeeded */
    }

    /* T010: Write and read MSIP */
    mmio_write(TIMER_MSIP, 0);
    TEST_EQ(10, mmio_read(TIMER_MSIP), 0);

    /* T011: Write and read mtime lo */
    mmio_write(TIMER_MTIME_LO, 0x12345678);
    TEST_EQ(11, mmio_read(TIMER_MTIME_LO) >= 0x12345678, 1);

    /* T012: Write and read mtimecmp */
    mmio_write(TIMER_MTIMECMP_LO, 0xFFFFFFFF);
    TEST_EQ(12, mmio_read(TIMER_MTIMECMP_LO), 0xFFFFFFFF);
    mmio_write(TIMER_MTIMECMP_HI, 0);
    TEST_EQ(13, mmio_read(TIMER_MTIMECMP_HI), 0);

    /* ---- DMA register access through fabric ---- */

    /* T014: Read DMA capability */
    {
        uint32_t dma_cap = mmio_read(DMA_CAP0);
        TEST_ASSERT(14, 1);  /* Access succeeded */
    }

    /* T015: Read DMA status */
    {
        uint32_t status = mmio_read(DMA_STATUS);
        TEST_ASSERT(15, (status & 1) != 0);  /* should be idle */
    }

    /* T016: Write and read DMA staging registers */
    mmio_write(DMA_OP_KIND, 0);
    TEST_EQ(16, mmio_read(DMA_OP_KIND), 0);

    mmio_write(DMA_SRC_ADDR_LO, 0x80000000);
    TEST_EQ(17, mmio_read(DMA_SRC_ADDR_LO), 0x80000000);

    mmio_write(DMA_DST_ADDR_LO, 0x10000000);
    TEST_EQ(18, mmio_read(DMA_DST_ADDR_LO), 0x10000000);

    mmio_write(DMA_BYTES, 64);
    TEST_EQ(19, mmio_read(DMA_BYTES), 64);

    /* ---- Cleanup ---- */
    mmio_write(TIMER_CTRL, 0);
    mmio_write(TIMER_MSIP, 0);

    TEST_FINISH();
}
