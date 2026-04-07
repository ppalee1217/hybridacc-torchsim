/* Minimal diagnostic: store a value, load it back, write the loaded value to DSRAM */
#include "hacc_test.h"

#define DIAG_ADDR ((volatile uint32_t *)(DSRAM_BASE + 0x1000))
/* Diagnostic dump area starts at DSRAM + 0x40 (after result header) */
#define DIAG_DUMP ((volatile uint32_t *)(DSRAM_BASE + 0x40))

void main(void)
{
    TEST_INIT();

    /* Store known value */
    DIAG_ADDR[0] = 0xCAFEBABE;

    /* NOP padding */
    __asm__ volatile ("nop; nop; nop; nop; nop; nop; nop; nop");

    /* Load it back */
    uint32_t val = DIAG_ADDR[0];

    /* Write loaded value to dump area so TB can read it */
    DIAG_DUMP[0] = val;
    DIAG_DUMP[1] = 0xCAFEBABE; /* expected */
    DIAG_DUMP[2] = (val == 0xCAFEBABE) ? 1 : 0;

    /* Second test: immediate store-load */
    DIAG_ADDR[1] = 0xDEAD;
    uint32_t val2 = DIAG_ADDR[1];
    DIAG_DUMP[3] = val2;
    DIAG_DUMP[4] = 0xDEAD;
    DIAG_DUMP[5] = (val2 == 0xDEAD) ? 1 : 0;

    TEST_EQ(1, val, 0xCAFEBABE);
    TEST_EQ(2, val2, 0xDEAD);

    TEST_FINISH();
}
