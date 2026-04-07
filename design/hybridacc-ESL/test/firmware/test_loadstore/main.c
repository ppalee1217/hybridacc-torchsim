/**
 * @file main.c
 * @brief Test RV32I load/store instructions.
 *
 * Covers: LW, LH, LHU, LB, LBU, SW, SH, SB
 * Tests: alignment, sign extension, byte-enable strobes.
 */

#include "hacc_test.h"

/* Use a working area in DSRAM past the result area */
#define WORK ((volatile uint8_t *)(TEST_WORK_BASE))
#define WORK32 ((volatile uint32_t *)(TEST_WORK_BASE))
#define WORK16 ((volatile uint16_t *)(TEST_WORK_BASE))

void main(void)
{
    TEST_INIT();

    /* ---- SW / LW (32-bit) ---- */

    /* T001: Store and load word */
    WORK32[0] = 0xDEADBEEF;
    TEST_EQ(1, WORK32[0], 0xDEADBEEF);

    /* T002: Store zero */
    WORK32[1] = 0;
    TEST_EQ(2, WORK32[1], 0);

    /* T003: Store max uint32 */
    WORK32[2] = 0xFFFFFFFF;
    TEST_EQ(3, WORK32[2], 0xFFFFFFFF);

    /* T004: Sequential word writes and reads */
    for (int i = 0; i < 8; i++) {
        WORK32[4 + i] = (uint32_t)(i * 0x11111111);
    }
    for (int i = 0; i < 8; i++) {
        TEST_EQ(4 + i, WORK32[4 + i], (uint32_t)(i * 0x11111111));
    }

    /* ---- SH / LH / LHU (16-bit) ---- */

    /* T012: Store and load halfword */
    WORK32[16] = 0; /* clear */
    WORK16[32] = 0x1234;
    TEST_EQ(12, WORK16[32], 0x1234);

    /* T013: LH sign extension (negative halfword) */
    {
        volatile uint16_t *p = (volatile uint16_t *)(TEST_WORK_BASE + 0x80);
        *p = 0x8000;  /* -32768 as signed */
        int32_t val;
        __asm__ volatile (
            "lh %0, 0(%1)"
            : "=r"(val)
            : "r"(p)
        );
        TEST_EQ(13, (uint32_t)val, 0xFFFF8000);  /* sign extended */
    }

    /* T014: LHU zero extension */
    {
        volatile uint16_t *p = (volatile uint16_t *)(TEST_WORK_BASE + 0x84);
        *p = 0x8000;
        uint32_t val;
        __asm__ volatile (
            "lhu %0, 0(%1)"
            : "=r"(val)
            : "r"(p)
        );
        TEST_EQ(14, val, 0x00008000);  /* zero extended */
    }

    /* ---- SB / LB / LBU (8-bit) ---- */

    /* T015: Store and load individual bytes */
    WORK[0x100] = 0xAB;
    WORK[0x101] = 0xCD;
    WORK[0x102] = 0xEF;
    WORK[0x103] = 0x01;
    TEST_EQ(15, WORK[0x100], 0xAB);
    TEST_EQ(16, WORK[0x101], 0xCD);
    TEST_EQ(17, WORK[0x102], 0xEF);
    TEST_EQ(18, WORK[0x103], 0x01);

    /* T019: Verify byte writes compose into word */
    {
        uint32_t word = *(volatile uint32_t *)(TEST_WORK_BASE + 0x100);
        TEST_EQ(19, word, 0x01EFCDAB);  /* little-endian */
    }

    /* T020: LB sign extension */
    {
        volatile uint8_t *p = (volatile uint8_t *)(TEST_WORK_BASE + 0x110);
        *p = 0x80;  /* -128 as signed byte */
        int32_t val;
        __asm__ volatile (
            "lb %0, 0(%1)"
            : "=r"(val)
            : "r"(p)
        );
        TEST_EQ(20, (uint32_t)val, 0xFFFFFF80);
    }

    /* T021: LBU zero extension */
    {
        volatile uint8_t *p = (volatile uint8_t *)(TEST_WORK_BASE + 0x114);
        *p = 0x80;
        uint32_t val;
        __asm__ volatile (
            "lbu %0, 0(%1)"
            : "=r"(val)
            : "r"(p)
        );
        TEST_EQ(21, val, 0x00000080);
    }

    /* T022: LH positive halfword */
    {
        volatile uint16_t *p = (volatile uint16_t *)(TEST_WORK_BASE + 0x88);
        *p = 0x7FFF;
        int32_t val;
        __asm__ volatile (
            "lh %0, 0(%1)"
            : "=r"(val)
            : "r"(p)
        );
        TEST_EQ(22, (uint32_t)val, 0x00007FFF);
    }

    /* T023: Offset addressing */
    {
        volatile uint32_t *base = (volatile uint32_t *)(TEST_WORK_BASE + 0x200);
        base[0] = 0xAAAA;
        base[1] = 0xBBBB;
        base[2] = 0xCCCC;
        uint32_t val;
        __asm__ volatile (
            "lw %0, 8(%1)"
            : "=r"(val)
            : "r"(base)
        );
        TEST_EQ(23, val, 0xCCCC);
    }

    /* T024: Negative offset addressing */
    {
        volatile uint32_t *base = (volatile uint32_t *)(TEST_WORK_BASE + 0x220);
        base[-1] = 0x12345678;
        uint32_t val;
        __asm__ volatile (
            "lw %0, -4(%1)"
            : "=r"(val)
            : "r"(base)
        );
        TEST_EQ(24, val, 0x12345678);
    }

    /* T025: Store byte to each position in a word */
    {
        volatile uint32_t *wp = (volatile uint32_t *)(TEST_WORK_BASE + 0x240);
        *wp = 0;
        volatile uint8_t *bp = (volatile uint8_t *)wp;
        bp[0] = 0x11;
        bp[1] = 0x22;
        bp[2] = 0x33;
        bp[3] = 0x44;
        TEST_EQ(25, *wp, 0x44332211);
    }

    TEST_FINISH();
}
