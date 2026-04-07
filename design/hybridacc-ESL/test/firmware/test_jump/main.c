/**
 * @file main.c
 * @brief Test RV32I jump instructions (JAL / JALR).
 *
 * Covers: JAL, JALR, function call/return, indirect jump.
 */

#include "hacc_test.h"

static int __attribute__((noinline)) add_func(int a, int b) {
    return a + b;
}

static int __attribute__((noinline)) nested_call(int x) {
    return add_func(x, x);
}

/* Function pointer table for indirect calls */
typedef int (*func_ptr_t)(int, int);

static int __attribute__((noinline)) sub_func(int a, int b) {
    return a - b;
}

void main(void)
{
    TEST_INIT();

    /* T001: Simple function call (uses JAL) */
    TEST_EQ(1, add_func(3, 4), 7);

    /* T002: Nested function calls */
    TEST_EQ(2, nested_call(5), 10);

    /* T003: Function pointer (indirect call via JALR) */
    {
        volatile func_ptr_t fp = add_func;
        TEST_EQ(3, fp(10, 20), 30);
    }

    /* T004: Different function via pointer */
    {
        volatile func_ptr_t fp = sub_func;
        TEST_EQ(4, fp(30, 10), 20);
    }

    /* T005: JAL link register preservation */
    {
        uint32_t ra_val = 0;
        __asm__ volatile (
            "jal ra, 1f\n"
            "j 2f\n"
            "1: mv %0, ra\n"
            "2:\n"
            : "=r"(ra_val)
        );
        /* ra should point to "j 2f" instruction (PC + 4 of JAL) */
        TEST_ASSERT(5, ra_val != 0);
    }

    /* T006: JALR with offset */
    {
        uint32_t result = 0;
        __asm__ volatile (
            "la t0, 1f\n"
            "jalr x0, t0, 0\n"  /* jump to t0, discard link */
            "li %0, 99\n"       /* should be skipped */
            "1: li %0, 42\n"
            : "=r"(result) : : "t0"
        );
        TEST_EQ(6, result, 42);
    }

    /* T007: Multiple return levels */
    {
        int r = nested_call(7);
        TEST_EQ(7, r, 14);
        r = add_func(r, 1);
        TEST_EQ(8, r, 15);
    }

    /* T008: Tight loop with function calls */
    {
        int sum = 0;
        for (int i = 1; i <= 5; i++) {
            sum = add_func(sum, i);
        }
        TEST_EQ(9, sum, 15);  /* 1+2+3+4+5 */
    }

    TEST_FINISH();
}
