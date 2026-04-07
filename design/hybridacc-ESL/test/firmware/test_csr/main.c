/**
 * @file main.c
 * @brief Test Zicsr CSR instructions.
 *
 * Covers: CSRRW, CSRRS, CSRRC, CSRRWI, CSRRSI, CSRRCI
 * Tests: mscratch R/W, mstatus bits, mcycle/minstret counters,
 *        misa read-only, mtvec configuration.
 */

#include "hacc_test.h"

void main(void)
{
    TEST_INIT();

    /* ---- CSRRW / CSRRS / CSRRC on mscratch ---- */

    /* T001: CSRW + CSRR on mscratch */
    CSR_WRITE(mscratch, 0xCAFEBABE);
    TEST_EQ(1, CSR_READ(mscratch), 0xCAFEBABE);

    /* T002: CSRRW returns old value */
    CSR_WRITE(mscratch, 0x11111111);
    {
        uint32_t old = CSR_SWAP(mscratch, 0x22222222);
        TEST_EQ(2, old, 0x11111111);
        TEST_EQ(3, CSR_READ(mscratch), 0x22222222);
    }

    /* T004: CSRRS sets bits */
    CSR_WRITE(mscratch, 0x00FF0000);
    CSR_SET(mscratch, 0x0000FF00);
    TEST_EQ(4, CSR_READ(mscratch), 0x00FFFF00);

    /* T005: CSRRC clears bits */
    CSR_WRITE(mscratch, 0xFFFFFFFF);
    CSR_CLEAR(mscratch, 0x0F0F0F0F);
    TEST_EQ(5, CSR_READ(mscratch), 0xF0F0F0F0);

    /* T006: CSRRWI with zero-extended immediate */
    {
        uint32_t old;
        /* csrrwi rd, mscratch, 0x1F (max 5-bit immediate) */
        __asm__ volatile ("csrrwi %0, mscratch, 0x1F" : "=r"(old));
        TEST_EQ(6, CSR_READ(mscratch), 0x1F);
    }

    /* T007: CSRRSI with immediate */
    CSR_WRITE(mscratch, 0);
    __asm__ volatile ("csrsi mscratch, 0x0A");  /* set bits 1,3 */
    TEST_EQ(7, CSR_READ(mscratch), 0x0A);

    /* T008: CSRRCI with immediate */
    CSR_WRITE(mscratch, 0x1F);
    __asm__ volatile ("csrci mscratch, 0x15");  /* clear bits 0,2,4 */
    TEST_EQ(8, CSR_READ(mscratch), 0x0A);

    /* ---- misa (read-only) ---- */

    /* T009: misa has I and M extension bits set */
    {
        uint32_t misa = CSR_READ(misa);
        /* Bit 8 = I extension, bit 12 = M extension */
        TEST_ASSERT(9, (misa & (1u << 8)) != 0);   /* I */
        TEST_ASSERT(10, (misa & (1u << 12)) != 0);  /* M */
    }

    /* ---- mtvec ---- */

    /* T011: Write and read mtvec */
    CSR_WRITE(mtvec, 0x00001000);
    TEST_EQ(11, CSR_READ(mtvec), 0x00001000);

    /* T012: mtvec alignment (lower 2 bits encode mode) */
    CSR_WRITE(mtvec, 0x00002004);  /* mode = vectored (bits[1:0] = 0b00 after mask) */
    {
        uint32_t mtvec = CSR_READ(mtvec);
        TEST_EQ(12, mtvec, 0x00002004);
    }

    /* ---- mcycle counter ---- */

    /* T013: mcycle advances */
    {
        uint32_t c1 = CSR_READ(mcycle);
        /* Do some work */
        volatile uint32_t dummy = 0;
        for (int i = 0; i < 10; i++) dummy += i;
        uint32_t c2 = CSR_READ(mcycle);
        TEST_ASSERT(13, c2 > c1);
    }

    /* T014: mcycle is writable */
    CSR_WRITE(mcycle, 0);
    {
        uint32_t c = CSR_READ(mcycle);
        /* Should be very small (a few cycles since write) */
        TEST_ASSERT(14, c < 100);
    }

    /* ---- minstret counter ---- */

    /* T015: minstret advances */
    {
        uint32_t i1 = CSR_READ(minstret);
        __asm__ volatile ("nop\nnop\nnop\nnop");
        uint32_t i2 = CSR_READ(minstret);
        TEST_ASSERT(15, i2 > i1);
    }

    /* ---- mstatus ---- */

    /* T016: MIE bit in mstatus */
    {
        uint32_t ms = CSR_READ(mstatus);
        /* After reset, MIE could be 0 or 1; just verify we can toggle */
        CSR_CLEAR(mstatus, MSTATUS_MIE);
        TEST_ASSERT(16, (CSR_READ(mstatus) & MSTATUS_MIE) == 0);
    }

    /* T017: Set MIE bit */
    CSR_SET(mstatus, MSTATUS_MIE);
    TEST_ASSERT(17, (CSR_READ(mstatus) & MSTATUS_MIE) != 0);

    /* Disable interrupts to prevent unexpected traps before EBREAK */
    CSR_CLEAR(mstatus, MSTATUS_MIE);

    /* ---- mie register ---- */

    /* T018: Write and read mie */
    CSR_WRITE(mie, MIE_MSIE | MIE_MTIE | MIE_MEIE);
    TEST_EQ(18, CSR_READ(mie), MIE_MSIE | MIE_MTIE | MIE_MEIE);

    /* T019: Clear individual mie bits */
    CSR_CLEAR(mie, MIE_MTIE);
    TEST_EQ(19, CSR_READ(mie), MIE_MSIE | MIE_MEIE);

    /* Clean up */
    CSR_WRITE(mie, 0);

    TEST_FINISH();
}
