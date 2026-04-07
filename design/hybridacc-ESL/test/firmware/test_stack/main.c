/**
 * @file main.c
 * @brief Test stack operations and calling convention.
 *
 * Covers: stack push/pop, deep recursion, stack frame layout,
 *         callee-saved register preservation, large stack frames.
 */

#include "hacc_test.h"

/* Recursive fibonacci to stress stack */
static int __attribute__((noinline)) fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

/* Deep recursive function */
static int __attribute__((noinline)) recursive_sum(int n) {
    if (n <= 0) return 0;
    return n + recursive_sum(n - 1);
}

/* Function with many local variables (large stack frame) */
static int __attribute__((noinline)) large_frame(int seed) {
    volatile int a = seed + 1;
    volatile int b = seed + 2;
    volatile int c = seed + 3;
    volatile int d = seed + 4;
    volatile int e = seed + 5;
    volatile int f = seed + 6;
    volatile int g = seed + 7;
    volatile int h = seed + 8;
    return a + b + c + d + e + f + g + h;
}

/* Function that uses many callee-saved registers */
static int __attribute__((noinline)) callee_saved_test(int x) {
    register int s0 __asm__("s0") = x;
    register int s1 __asm__("s1") = x + 1;
    register int s2 __asm__("s2") = x + 2;
    register int s3 __asm__("s3") = x + 3;
    register int s4 __asm__("s4") = x + 4;
    register int s5 __asm__("s5") = x + 5;
    __asm__ volatile ("" : "+r"(s0), "+r"(s1), "+r"(s2),
                           "+r"(s3), "+r"(s4), "+r"(s5));
    return s0 + s1 + s2 + s3 + s4 + s5;
}

/* Nested calls that verify callee-saved registers survive */
static int __attribute__((noinline)) outer(int x) {
    volatile int saved = x * 10;
    int inner_result = callee_saved_test(x);
    return saved + inner_result;
}

void main(void)
{
    TEST_INIT();

    /* ---- Basic stack operations ---- */

    /* T001: Stack pointer is in DSRAM range */
    {
        uint32_t sp_val;
        __asm__ volatile ("mv %0, sp" : "=r"(sp_val));
        TEST_ASSERT(1, sp_val >= DSRAM_BASE && sp_val <= DSRAM_BASE + DSRAM_SIZE);
    }

    /* T002: Push and pop via inline asm */
    {
        uint32_t val = 0xCAFEBABE;
        uint32_t restored;
        __asm__ volatile (
            "addi sp, sp, -4\n"
            "sw %1, 0(sp)\n"
            "lw %0, 0(sp)\n"
            "addi sp, sp, 4\n"
            : "=r"(restored)
            : "r"(val)
            : "memory"
        );
        TEST_EQ(2, restored, 0xCAFEBABE);
    }

    /* ---- Recursion tests ---- */

    /* T003: Fibonacci (moderate depth) */
    TEST_EQ(3, fib(0), 0);
    TEST_EQ(4, fib(1), 1);
    TEST_EQ(5, fib(5), 5);
    TEST_EQ(6, fib(10), 55);

    /* T007: Recursive sum */
    TEST_EQ(7, recursive_sum(0), 0);
    TEST_EQ(8, recursive_sum(10), 55);
    TEST_EQ(9, recursive_sum(100), 5050);

    /* ---- Large stack frame ---- */

    /* T010: Function with many locals */
    /* large_frame(0) = 1+2+3+4+5+6+7+8 = 36 */
    TEST_EQ(10, large_frame(0), 36);
    /* large_frame(10) = 11+12+13+14+15+16+17+18 = 116 */
    TEST_EQ(11, large_frame(10), 116);

    /* ---- Callee-saved register preservation ---- */

    /* T012: Callee-saved regs after call */
    /* callee_saved_test(1) = 1+2+3+4+5+6 = 21 */
    TEST_EQ(12, callee_saved_test(1), 21);

    /* T013: Nested call preserving callee-saved regs */
    /* outer(2) = (2*10) + (2+3+4+5+6+7) = 20 + 27 = 47 */
    TEST_EQ(13, outer(2), 47);

    /* ---- Stack alignment ---- */

    /* T014: SP remains 16-byte aligned after function calls */
    {
        volatile int dummy = fib(3);
        (void)dummy;
        uint32_t sp_val;
        __asm__ volatile ("mv %0, sp" : "=r"(sp_val));
        TEST_EQ(14, sp_val & 0xF, 0);
    }

    /* ---- Multiple frames ---- */

    /* T015: Deeply nested calls */
    {
        int result = 0;
        for (int i = 0; i < 5; i++) {
            result += recursive_sum(i);
        }
        /* 0 + 1 + 3 + 6 + 10 = 20 */
        TEST_EQ(15, result, 20);
    }

    TEST_FINISH();
}
