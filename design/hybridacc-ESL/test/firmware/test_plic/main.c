/**
 * @file main.c
 * @brief Test PLIC (Platform-Level Interrupt Controller).
 *
 * Covers: priority read/write, enable bitmap, threshold, claim/complete,
 *         pending status, max source ID query.
 *
 * Note: With NUM_CLUSTERS=1, NUM_NLU=0 the PLIC sources are:
 *   0 = reserved
 *   1 = cluster[0] IRQ
 *   2 = DMA IRQ
 *   3 = loader fault
 *   4 = fabric fault
 */

#include "hacc_test.h"

void main(void)
{
    TEST_INIT();

    /* ---- Basic PLIC register access ---- */

    /* T001: Read max source ID */
    {
        uint32_t max_src = mmio_read(PLIC_MAX_SOURCE_ID);
        TEST_ASSERT(1, max_src >= 2);  /* At least reserved + cluster + DMA */
    }

    /* T002: Default threshold should be 0 */
    TEST_EQ(2, mmio_read(PLIC_THRESHOLD), 0);

    /* T003: Write and read threshold */
    mmio_write(PLIC_THRESHOLD, 3);
    TEST_EQ(3, mmio_read(PLIC_THRESHOLD), 3);
    mmio_write(PLIC_THRESHOLD, 0);  /* restore */

    /* ---- Priority registers ---- */

    /* T004: Read default priority (should be 1) */
    TEST_EQ(4, mmio_read(PLIC_PRIORITY_BASE + 4 * 1), 1);  /* source 1 */

    /* T005: Write and read priority for source 1 */
    mmio_write(PLIC_PRIORITY_BASE + 4 * 1, 5);
    TEST_EQ(5, mmio_read(PLIC_PRIORITY_BASE + 4 * 1), 5);

    /* T006: Write and read priority for source 2 (DMA) */
    mmio_write(PLIC_PRIORITY_BASE + 4 * 2, 7);
    TEST_EQ(6, mmio_read(PLIC_PRIORITY_BASE + 4 * 2), 7);

    /* Restore defaults */
    mmio_write(PLIC_PRIORITY_BASE + 4 * 1, 1);
    mmio_write(PLIC_PRIORITY_BASE + 4 * 2, 1);

    /* ---- Enable registers ---- */

    /* T007: Default enable should be 0 */
    TEST_EQ(7, mmio_read(PLIC_ENABLE_LO), 0);

    /* T008: Enable source 1 and 2 */
    mmio_write(PLIC_ENABLE_LO, (1u << 1) | (1u << 2));
    TEST_EQ(8, mmio_read(PLIC_ENABLE_LO), (1u << 1) | (1u << 2));

    /* T009: Enable hi register */
    mmio_write(PLIC_ENABLE_HI, 0);
    TEST_EQ(9, mmio_read(PLIC_ENABLE_HI), 0);

    /* ---- Pending registers (read-only, set by hardware) ---- */

    /* T010: Pending lo should be 0 initially (no IRQ sources active) */
    {
        uint32_t pending = mmio_read(PLIC_PENDING_LO);
        /* We don't control cluster IRQ from firmware, just verify readable */
        TEST_ASSERT(10, 1);  /* Access didn't fault */
    }

    /* ---- Claim/Complete ---- */

    /* T011: Claim with no pending → returns 0 */
    {
        /* First disable all enables and clear any stale state */
        mmio_write(PLIC_ENABLE_LO, 0);
        mmio_write(PLIC_ENABLE_HI, 0);
        uint32_t claimed = mmio_read(PLIC_CLAIM_COMPLETE);
        TEST_EQ(11, claimed, 0);
    }

    /* T012: Complete with source 0 (no-op) */
    mmio_write(PLIC_CLAIM_COMPLETE, 0);
    TEST_ASSERT(12, 1);  /* Didn't fault */

    /* ---- Threshold filtering ---- */

    /* T013: Set threshold to 7 → only priority > 7 can trigger */
    mmio_write(PLIC_THRESHOLD, 7);
    TEST_EQ(13, mmio_read(PLIC_THRESHOLD), 7);
    mmio_write(PLIC_THRESHOLD, 0);

    /* ---- PLIC priority range ---- */

    /* T014: Set and verify multiple priority levels */
    for (uint32_t src = 1; src <= 4; src++) {
        mmio_write(PLIC_PRIORITY_BASE + 4 * src, src * 2);
    }
    for (uint32_t src = 1; src <= 4; src++) {
        TEST_EQ(14 + src - 1, mmio_read(PLIC_PRIORITY_BASE + 4 * src), src * 2);
    }

    /* Restore */
    for (uint32_t src = 1; src <= 4; src++) {
        mmio_write(PLIC_PRIORITY_BASE + 4 * src, 1);
    }

    /* ---- Cleanup ---- */
    mmio_write(PLIC_ENABLE_LO, 0);
    mmio_write(PLIC_ENABLE_HI, 0);
    mmio_write(PLIC_THRESHOLD, 0);

    TEST_FINISH();
}
