#include <stdint.h>

/* Data SRAM base = 0x10000000 */
#define DSRAM_BASE ((volatile uint32_t *)0x10000000)

void main(void)
{
    /* Write 0xcafecafe to the first word of data SRAM */
    for (volatile int i = 0; i < 4; ++i) {
        DSRAM_BASE[i] = 0xcafecafeU;
    }
    /* Return → crt0 will execute EBREAK */
    return;
}
