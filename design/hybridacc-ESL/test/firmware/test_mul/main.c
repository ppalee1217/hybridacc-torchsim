/**
 * @file main.c
 * @brief Test Zmmul extension (MUL, MULH, MULHSU, MULHU).
 *
 * Covers all multiply instructions with edge cases:
 *  - positive × positive, negative × negative, mixed signs
 *  - overflow, zero, one, min/max values
 */

#include "hacc_test.h"

void main(void)
{
    TEST_INIT();
    uint32_t r;

    /* ---- MUL (lower 32 bits of product) ---- */

    /* T001: simple multiply */
    {
        uint32_t a = 6, b = 7;
        __asm__ volatile ("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(1, r, 42);
    }

    /* T002: multiply by zero */
    {
        uint32_t a = 12345, b = 0;
        __asm__ volatile ("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(2, r, 0);
    }

    /* T003: multiply by one */
    {
        uint32_t a = 0xDEADBEEF, b = 1;
        __asm__ volatile ("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(3, r, 0xDEADBEEF);
    }

    /* T004: negative × positive (signed view: -2 × 3 = -6) */
    {
        uint32_t a = (uint32_t)(-2), b = 3;
        __asm__ volatile ("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(4, r, (uint32_t)(-6));
    }

    /* T005: negative × negative (signed view: -3 × -4 = 12) */
    {
        uint32_t a = (uint32_t)(-3), b = (uint32_t)(-4);
        __asm__ volatile ("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(5, r, 12);
    }

    /* T006: large multiply (overflow lower 32 bits) */
    {
        uint32_t a = 0x10000, b = 0x10000;  /* 65536 × 65536 = 2^32 → lower 32 = 0 */
        __asm__ volatile ("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(6, r, 0);
    }

    /* T007: 0xFFFF × 0xFFFF */
    {
        uint32_t a = 0xFFFF, b = 0xFFFF;
        __asm__ volatile ("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(7, r, 0xFFFE0001);
    }

    /* ---- MULH (upper 32 bits, signed × signed) ---- */

    /* T008: small numbers → high word = 0 */
    {
        uint32_t a = 100, b = 200;
        __asm__ volatile ("mulh %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(8, r, 0);
    }

    /* T009: -1 × -1 → product = 1 → high = 0 */
    {
        uint32_t a = (uint32_t)(-1), b = (uint32_t)(-1);
        __asm__ volatile ("mulh %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(9, r, 0);
    }

    /* T010: -1 × 1 → product = -1 → high = -1 = 0xFFFFFFFF */
    {
        uint32_t a = (uint32_t)(-1), b = 1;
        __asm__ volatile ("mulh %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(10, r, 0xFFFFFFFF);
    }

    /* T011: 0x10000 × 0x10000 → 2^32 → high = 1 */
    {
        uint32_t a = 0x10000, b = 0x10000;
        __asm__ volatile ("mulh %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(11, r, 1);
    }

    /* ---- MULHU (upper 32 bits, unsigned × unsigned) ---- */

    /* T012: 0xFFFFFFFF × 2 → upper = 1 */
    {
        uint32_t a = 0xFFFFFFFF, b = 2;
        __asm__ volatile ("mulhu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(12, r, 1);
    }

    /* T013: 0xFFFFFFFF × 0xFFFFFFFF → upper */
    {
        uint32_t a = 0xFFFFFFFF, b = 0xFFFFFFFF;
        __asm__ volatile ("mulhu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        /* 0xFFFFFFFF² = 0xFFFFFFFE00000001 → upper = 0xFFFFFFFE */
        TEST_EQ(13, r, 0xFFFFFFFE);
    }

    /* ---- MULHSU (upper 32 bits, signed × unsigned) ---- */

    /* T014: positive × unsigned → same as mulhu for small values */
    {
        uint32_t a = 0x10000, b = 0x10000;
        __asm__ volatile ("mulhsu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(14, r, 1);
    }

    /* T015: -1 (signed) × 1 (unsigned) → -1 → high = 0xFFFFFFFF */
    {
        uint32_t a = (uint32_t)(-1), b = 1;
        __asm__ volatile ("mulhsu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(15, r, 0xFFFFFFFF);
    }

    /* T016: -1 × 0 → 0 */
    {
        uint32_t a = (uint32_t)(-1), b = 0;
        __asm__ volatile ("mulhsu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
        TEST_EQ(16, r, 0);
    }

    /* ---- Compound multiply tests ---- */

    /* T017: multiply accumulate pattern */
    {
        int32_t acc = 0;
        for (int i = 1; i <= 10; i++) {
            acc += i * i;
        }
        /* 1+4+9+16+25+36+49+64+81+100 = 385 */
        TEST_EQ(17, (uint32_t)acc, 385);
    }

    /* T018: 64-bit multiply using MUL + MULHU */
    {
        uint32_t a = 0x12345678, b = 0x9ABCDEF0;
        uint32_t lo, hi;
        __asm__ volatile ("mul %0, %1, %2" : "=r"(lo) : "r"(a), "r"(b));
        __asm__ volatile ("mulhu %0, %1, %2" : "=r"(hi) : "r"(a), "r"(b));
        /* Verify: 0x12345678 × 0x9ABCDEF0 = 0x0B00EA4E_242D2080 */
        TEST_EQ(18, lo, 0x242D2080);
        TEST_EQ(19, hi, 0x0B00EA4E);
    }

    TEST_FINISH();
}
