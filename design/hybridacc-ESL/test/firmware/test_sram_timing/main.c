#include "hacc_test.h"

/* Place test data far from test results to avoid any aliasing concerns */
#define TEST_ADDR ((volatile uint32_t *)(DSRAM_BASE + 0x1000))

void main(void)
{
    TEST_INIT();

    /* T1: SW/LW with 4 NOPs gap */
    {
        volatile uint32_t *p = TEST_ADDR;
        *p = 0xCAFEBABE;
        __asm__ volatile ("nop; nop; nop; nop");
        uint32_t val = *p;
        TEST_EQ(1, val, 0xCAFEBABE);
    }

    /* T2: SW/LW with 2 NOPs gap */
    {
        volatile uint32_t *p = TEST_ADDR + 1;
        *p = 0xDEADBEEF;
        __asm__ volatile ("nop; nop");
        uint32_t val = *p;
        TEST_EQ(2, val, 0xDEADBEEF);
    }

    /* T3: SW/LW immediate (no gap) */
    {
        volatile uint32_t *p = TEST_ADDR + 2;
        *p = 0x12345678;
        uint32_t val = *p;
        TEST_EQ(3, val, 0x12345678);
    }

    TEST_FINISH();
}
