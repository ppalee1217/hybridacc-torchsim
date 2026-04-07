/**
 * @file main.c
 * @brief Test trap/exception handling: ECALL and software interrupt (MSIP).
 *
 * Covers: mtvec setup, ECALL exception, MSIP trap, mepc/mcause verification,
 *         MRET return, mstatus MIE/MPIE transitions.
 *
 * Note: This test requires the trap_entry.S handler and MRET support.
 */

#include "hacc_test.h"

/* Trap state — written by C trap handler */
static volatile uint32_t trap_count = 0;
static volatile uint32_t last_mcause = 0;
static volatile uint32_t last_mepc = 0;

/* Override the weak trap_handler from trap_entry.S */
void trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    (void)mtval;
    trap_count++;
    last_mcause = mcause;
    last_mepc   = mepc;

    uint32_t is_interrupt = (mcause >> 31) & 1;
    uint32_t cause_code   = mcause & 0x7FFFFFFF;

    if (is_interrupt) {
        /* Interrupt: handle based on cause */
        if (cause_code == 3) {
            /* MSIP — clear the software interrupt */
            mmio_write(TIMER_MSIP, 0);
        } else if (cause_code == 7) {
            /* MTIP — disable timer to stop re-triggering */
            mmio_write(TIMER_CTRL, 0);
        }
        /* For interrupts, mepc points to the instruction to resume.
         * MRET will return to mepc (no increment needed). */
    } else {
        /* Exception: advance mepc past the faulting instruction (4 bytes) */
        uint32_t new_mepc = mepc + 4;
        CSR_WRITE(mepc, new_mepc);
    }

    /* Re-enable MIE so future interrupts can fire */
    CSR_SET(mstatus, MSTATUS_MIE);
}

/* External declaration of trap_entry (in trap_entry.S) */
extern void trap_entry(void);

void main(void)
{
    TEST_INIT();

    /* ---- Setup trap vector ---- */
    CSR_WRITE(mtvec, (uint32_t)&trap_entry);
    TEST_EQ(1, CSR_READ(mtvec), (uint32_t)&trap_entry);

    /* Ensure interrupts are disabled initially */
    CSR_CLEAR(mstatus, MSTATUS_MIE);
    CSR_WRITE(mie, 0);

    /* ---- Test ECALL exception ---- */

    /* T002: ECALL triggers trap with mcause = 11 */
    trap_count = 0;
    last_mcause = 0;

    CSR_SET(mstatus, MSTATUS_MIE); /* enable global interrupts for trap to work */
    __asm__ volatile ("ecall");

    TEST_EQ(2, trap_count, 1);
    TEST_EQ(3, last_mcause, 11);  /* Environment call from M-mode */

    /* T004: mepc should have pointed to the ECALL instruction */
    TEST_ASSERT(4, last_mepc != 0);

    /* ---- Test MSIP (software interrupt) ---- */

    /* T005: Trigger software interrupt */
    trap_count = 0;
    last_mcause = 0;

    /* Enable MSIP in mie */
    CSR_WRITE(mie, MIE_MSIE);
    CSR_SET(mstatus, MSTATUS_MIE);

    /* Set MSIP pending via timer MMIO */
    mmio_write(TIMER_MSIP, 1);

    /* Interrupt should fire on the next instruction */
    __asm__ volatile ("nop\nnop\nnop\nnop");

    TEST_EQ(5, trap_count, 1);
    TEST_EQ(6, last_mcause, 0x80000003);  /* MSI interrupt */

    /* T007: MSIP should be cleared by handler */
    TEST_EQ(7, mmio_read(TIMER_MSIP), 0);

    /* ---- Test MTIP (timer interrupt) ---- */

    /* T008: Trigger timer interrupt */
    trap_count = 0;
    last_mcause = 0;

    /* Setup timer: set mtimecmp very close to current mtime */
    mmio_write(TIMER_CTRL, 0);  /* stop timer first */
    mmio_write(TIMER_MTIME_LO, 0);
    mmio_write(TIMER_MTIME_HI, 0);
    mmio_write(TIMER_MTIMECMP_LO, 5);  /* fire after 5 cycles */
    mmio_write(TIMER_MTIMECMP_HI, 0);

    /* Enable MTIP in mie */
    CSR_WRITE(mie, MIE_MTIE);
    CSR_SET(mstatus, MSTATUS_MIE);

    /* Start timer */
    mmio_write(TIMER_CTRL, 1);

    /* Wait for interrupt (spin) */
    for (volatile int i = 0; i < 100 && trap_count == 0; i++) {
        __asm__ volatile ("nop");
    }

    TEST_EQ(8, trap_count, 1);
    TEST_EQ(9, last_mcause, 0x80000007);  /* MTI interrupt */

    /* ---- Test multiple traps ---- */

    /* T010: Multiple ECALL traps */
    trap_count = 0;
    CSR_WRITE(mie, 0);  /* disable external interrupts */
    CSR_SET(mstatus, MSTATUS_MIE);

    __asm__ volatile ("ecall");
    __asm__ volatile ("ecall");
    __asm__ volatile ("ecall");

    TEST_EQ(10, trap_count, 3);

    /* ---- Cleanup ---- */
    CSR_CLEAR(mstatus, MSTATUS_MIE);
    CSR_WRITE(mie, 0);
    mmio_write(TIMER_CTRL, 0);
    mmio_write(TIMER_MSIP, 0);

    TEST_FINISH();
}
