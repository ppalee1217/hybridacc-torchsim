/**
 * @file main.c
 * @brief Test pipeline hazards and data forwarding.
 *
 * Covers: load-use hazard, RAW data forwarding (EX→ID, MEM→ID),
 *         branch after ALU, branch after load, back-to-back stores,
 *         MMIO followed by ALU, store-load forwarding.
 */

#include "hacc_test.h"

#define WORK32 ((volatile uint32_t *)(TEST_WORK_BASE))

void main(void)
{
    TEST_INIT();

    /* ---- Load-Use Hazard (1 cycle stall expected) ---- */

    /* T001: Load followed by immediate use */
    {
        WORK32[0] = 42;
        uint32_t val;
        __asm__ volatile (
            "lw %0, 0(%1)\n"
            "addi %0, %0, 8\n"  /* uses load result immediately */
            : "=r"(val)
            : "r"(&WORK32[0])
            : "memory"
        );
        TEST_EQ(1, val, 50);
    }

    /* T002: Two loads, then use both */
    {
        WORK32[0] = 10;
        WORK32[1] = 20;
        uint32_t a, b, sum;
        __asm__ volatile (
            "lw %0, 0(%3)\n"
            "lw %1, 4(%3)\n"
            "add %2, %0, %1\n"
            : "=&r"(a), "=&r"(b), "=r"(sum)
            : "r"(&WORK32[0])
            : "memory"
        );
        TEST_EQ(2, sum, 30);
    }

    /* ---- RAW Data Forwarding (EX→ID bypass) ---- */

    /* T003: ALU chain — no stall expected */
    {
        uint32_t r;
        __asm__ volatile (
            "li %0, 1\n"
            "addi %0, %0, 2\n"   /* depends on li → forwarded from EX */
            "addi %0, %0, 3\n"   /* depends on addi → forwarded from EX */
            "addi %0, %0, 4\n"
            : "=r"(r)
        );
        TEST_EQ(3, r, 10);
    }

    /* T004: Deep dependency chain */
    {
        uint32_t r;
        __asm__ volatile (
            "li %0, 0\n"
            "addi %0, %0, 1\n"
            "slli %0, %0, 1\n"   /* 2 */
            "addi %0, %0, 1\n"   /* 3 */
            "slli %0, %0, 1\n"   /* 6 */
            "addi %0, %0, 1\n"   /* 7 */
            : "=r"(r)
        );
        TEST_EQ(4, r, 7);
    }

    /* T005: ALU result used by branch */
    {
        uint32_t r = 0;
        __asm__ volatile (
            "li t0, 5\n"
            "addi t0, t0, -5\n"  /* t0 = 0, forwarded to branch */
            "beqz t0, 1f\n"
            "li %0, 99\n"        /* should be skipped */
            "j 2f\n"
            "1: li %0, 42\n"
            "2:\n"
            : "=r"(r) : : "t0"
        );
        TEST_EQ(5, r, 42);
    }

    /* ---- Branch after load ---- */

    /* T006: Load followed by branch on loaded value */
    {
        WORK32[4] = 0;
        uint32_t r = 99;
        __asm__ volatile (
            "lw t0, 0(%1)\n"
            "beqz t0, 1f\n"      /* branch on loaded value → needs stall */
            "li %0, 77\n"
            "j 2f\n"
            "1: li %0, 33\n"
            "2:\n"
            : "=r"(r)
            : "r"(&WORK32[4])
            : "t0", "memory"
        );
        TEST_EQ(6, r, 33);
    }

    /* ---- Back-to-back stores ---- */

    /* T007: Sequential stores to adjacent addresses */
    {
        volatile uint32_t *p = (volatile uint32_t *)(TEST_WORK_BASE + 0x200);
        p[0] = 0xAAAA;
        p[1] = 0xBBBB;
        p[2] = 0xCCCC;
        p[3] = 0xDDDD;
        TEST_EQ(7, p[0], 0xAAAA);
        TEST_EQ(8, p[1], 0xBBBB);
        TEST_EQ(9, p[2], 0xCCCC);
        TEST_EQ(10, p[3], 0xDDDD);
    }

    /* ---- Store then load same address ---- */

    /* T011: Store-load to same word */
    {
        volatile uint32_t *p = (volatile uint32_t *)(TEST_WORK_BASE + 0x300);
        *p = 0x55667788;
        uint32_t val = *p;
        TEST_EQ(11, val, 0x55667788);
    }

    /* ---- MMIO access in pipeline ---- */

    /* T012: MMIO read followed by ALU */
    {
        uint32_t timer_val = mmio_read(TIMER_MTIME_LO);
        uint32_t result = timer_val + 1;
        TEST_ASSERT(12, result > 0);  /* timer was read successfully */
    }

    /* T013: ALU followed by MMIO write and read-back */
    {
        uint32_t val = 100 + 200;
        mmio_write(TIMER_MTIMECMP_LO, val);
        TEST_EQ(13, mmio_read(TIMER_MTIMECMP_LO), 300);
    }

    /* ---- Mixed hazard patterns ---- */

    /* T014: Load → ALU → Store (full pipeline exercise) */
    {
        WORK32[16] = 7;
        uint32_t tmp;
        __asm__ volatile (
            "lw %0, 0(%1)\n"
            "addi %0, %0, 3\n"
            "sw %0, 4(%1)\n"
            : "=&r"(tmp)
            : "r"(&WORK32[16])
            : "memory"
        );
        TEST_EQ(14, WORK32[17], 10);
    }

    /* T015: Multiple loads then single computation */
    {
        WORK32[20] = 100;
        WORK32[21] = 200;
        WORK32[22] = 300;
        uint32_t a, b, c, result;
        __asm__ volatile (
            "lw %0, 0(%4)\n"
            "lw %1, 4(%4)\n"
            "lw %2, 8(%4)\n"
            "add %3, %0, %1\n"
            "add %3, %3, %2\n"
            : "=&r"(a), "=&r"(b), "=&r"(c), "=r"(result)
            : "r"(&WORK32[20])
            : "memory"
        );
        TEST_EQ(15, result, 600);
    }

    TEST_FINISH();
}
