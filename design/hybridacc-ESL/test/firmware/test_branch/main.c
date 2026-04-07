/**
 * @file main.c
 * @brief Test RV32I branch instructions.
 *
 * Covers: BEQ, BNE, BLT, BGE, BLTU, BGEU
 * Tests: taken / not-taken for each, forward and backward branches.
 */

#include "hacc_test.h"

/* Use noinline to prevent compiler from optimizing away branch logic */
static int __attribute__((noinline)) branch_beq(int a, int b) {
    int r = 0;
    __asm__ volatile (
        "beq %1, %2, 1f\n"
        "li %0, 0\n"
        "j 2f\n"
        "1: li %0, 1\n"
        "2:\n"
        : "=r"(r) : "r"(a), "r"(b)
    );
    return r;
}

static int __attribute__((noinline)) branch_bne(int a, int b) {
    int r = 0;
    __asm__ volatile (
        "bne %1, %2, 1f\n"
        "li %0, 0\n"
        "j 2f\n"
        "1: li %0, 1\n"
        "2:\n"
        : "=r"(r) : "r"(a), "r"(b)
    );
    return r;
}

static int __attribute__((noinline)) branch_blt(int a, int b) {
    int r = 0;
    __asm__ volatile (
        "blt %1, %2, 1f\n"
        "li %0, 0\n"
        "j 2f\n"
        "1: li %0, 1\n"
        "2:\n"
        : "=r"(r) : "r"(a), "r"(b)
    );
    return r;
}

static int __attribute__((noinline)) branch_bge(int a, int b) {
    int r = 0;
    __asm__ volatile (
        "bge %1, %2, 1f\n"
        "li %0, 0\n"
        "j 2f\n"
        "1: li %0, 1\n"
        "2:\n"
        : "=r"(r) : "r"(a), "r"(b)
    );
    return r;
}

static int __attribute__((noinline)) branch_bltu(unsigned a, unsigned b) {
    int r = 0;
    __asm__ volatile (
        "bltu %1, %2, 1f\n"
        "li %0, 0\n"
        "j 2f\n"
        "1: li %0, 1\n"
        "2:\n"
        : "=r"(r) : "r"(a), "r"(b)
    );
    return r;
}

static int __attribute__((noinline)) branch_bgeu(unsigned a, unsigned b) {
    int r = 0;
    __asm__ volatile (
        "bgeu %1, %2, 1f\n"
        "li %0, 0\n"
        "j 2f\n"
        "1: li %0, 1\n"
        "2:\n"
        : "=r"(r) : "r"(a), "r"(b)
    );
    return r;
}

void main(void)
{
    TEST_INIT();

    /* BEQ */
    TEST_EQ(1, branch_beq(5, 5), 1);     /* equal → taken */
    TEST_EQ(2, branch_beq(5, 6), 0);     /* not equal → not taken */
    TEST_EQ(3, branch_beq(0, 0), 1);     /* zero == zero */
    TEST_EQ(4, branch_beq(-1, -1), 1);   /* -1 == -1 */

    /* BNE */
    TEST_EQ(5, branch_bne(5, 6), 1);     /* not equal → taken */
    TEST_EQ(6, branch_bne(5, 5), 0);     /* equal → not taken */

    /* BLT (signed) */
    TEST_EQ(7, branch_blt(-1, 0), 1);    /* -1 < 0 → taken */
    TEST_EQ(8, branch_blt(0, -1), 0);    /* 0 < -1 → not taken */
    TEST_EQ(9, branch_blt(5, 5), 0);     /* equal → not taken */
    TEST_EQ(10, branch_blt(-100, 100), 1);

    /* BGE (signed) */
    TEST_EQ(11, branch_bge(0, -1), 1);   /* 0 >= -1 → taken */
    TEST_EQ(12, branch_bge(5, 5), 1);    /* equal → taken */
    TEST_EQ(13, branch_bge(-1, 0), 0);   /* -1 >= 0 → not taken */

    /* BLTU (unsigned) */
    TEST_EQ(14, branch_bltu(1, 2), 1);
    TEST_EQ(15, branch_bltu(2, 1), 0);
    TEST_EQ(16, branch_bltu(0, 0xFFFFFFFF), 1);  /* 0 < MAX → taken */
    TEST_EQ(17, branch_bltu(0xFFFFFFFF, 0), 0);

    /* BGEU (unsigned) */
    TEST_EQ(18, branch_bgeu(2, 1), 1);
    TEST_EQ(19, branch_bgeu(5, 5), 1);
    TEST_EQ(20, branch_bgeu(0, 0xFFFFFFFF), 0);

    /* Backward branch (loop) */
    {
        int count = 0;
        for (int i = 0; i < 10; i++) count++;
        TEST_EQ(21, count, 10);
    }

    /* Deeply nested branches */
    {
        int val = 0;
        if (1 == 1) {
            if (2 > 1) {
                if (-1 < 0) {
                    val = 42;
                }
            }
        }
        TEST_EQ(22, val, 42);
    }

    TEST_FINISH();
}
