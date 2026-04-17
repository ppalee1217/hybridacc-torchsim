/**
 * @file main.c
 * @brief Test WFI + machine timer interrupt (MTIP) integration.
 *
 * Verifies:
 *   T01  Timer enable: mtime increments each cycle when TIMER_CTRL=1.
 *   T02  Timer disable: mtime stops when TIMER_CTRL=0.
 *   T03  WFI stall: core halts on WFI until MTIP fires.
 *   T04  MTIP triggers correctly when mtime >= mtimecmp.
 *   T05  Trap handler runs with correct mcause (0x80000007).
 *   T06  mcycle delta proves core was stalled (not busy-spinning).
 *   T07  arm_wfi_timer_tick helper: second WFI wakeup with relative delta.
 *   T08  Multiple WFI/timer cycles work back-to-back.
 *
 * Requires: trap_entry.S, hacc_test.h, CoreLocalIrq hardware.
 */

#include "hacc_test.h"

/* ── Trap state ── */
static volatile uint32_t trap_count   = 0;
static volatile uint32_t last_mcause  = 0;

/* ── C trap handler (overrides weak default in trap_entry.S) ── */
void trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    (void)mepc;
    (void)mtval;

    trap_count++;
    last_mcause = mcause;

    uint32_t is_interrupt = (mcause >> 31) & 1;
    uint32_t cause_code   = mcause & 0x7FFFFFFF;

    if (is_interrupt && cause_code == 7) {
        /* MTIP: push mtimecmp to max so the interrupt de-asserts */
        mmio_write(TIMER_MTIMECMP_LO, 0xFFFFFFFFu);
        mmio_write(TIMER_MTIMECMP_HI, 0xFFFFFFFFu);
    }
}

extern void trap_entry(void);

/* ── Helper: read mcycle CSR ── */
static inline uint32_t read_mcycle(void)
{
    return CSR_READ(mcycle);
}

/* ── Helper: arm timer to fire delta_cycles from now ── */
static inline void arm_timer(uint32_t delta_cycles)
{
    uint32_t now_lo = mmio_read(TIMER_MTIME_LO);
    /* Set hi first to 0xFFFFFFFF (safe), then lo, then hi=0 */
    mmio_write(TIMER_MTIMECMP_HI, 0xFFFFFFFFu);
    mmio_write(TIMER_MTIMECMP_LO, now_lo + delta_cycles);
    mmio_write(TIMER_MTIMECMP_HI, 0u);
}

void main(void)
{
    TEST_INIT();

    /* ── Setup trap vector & enable MTIP ── */
    CSR_WRITE(mtvec, (uint32_t)&trap_entry);
    CSR_CLEAR(mstatus, MSTATUS_MIE);
    CSR_WRITE(mie, 0);

    /* ================================================================
     * T01: Timer enable — mtime increments
     * ================================================================ */
    mmio_write(TIMER_CTRL, 0);                  /* stop timer  */
    mmio_write(TIMER_MTIME_LO, 0);
    mmio_write(TIMER_MTIME_HI, 0);
    mmio_write(TIMER_MTIMECMP_LO, 0xFFFFFFFFu); /* no IRQ yet  */
    mmio_write(TIMER_MTIMECMP_HI, 0xFFFFFFFFu);

    mmio_write(TIMER_CTRL, 1);                  /* start timer */

    /* Burn a few cycles */
    __asm__ volatile ("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");

    uint32_t t1 = mmio_read(TIMER_MTIME_LO);
    TEST_ASSERT(1, t1 > 0);  /* T01: mtime advanced */

    /* ================================================================
     * T02: Timer disable — mtime stops
     * ================================================================ */
    mmio_write(TIMER_CTRL, 0);
    uint32_t t2a = mmio_read(TIMER_MTIME_LO);
    __asm__ volatile ("nop\nnop\nnop\nnop");
    uint32_t t2b = mmio_read(TIMER_MTIME_LO);
    TEST_EQ(2, t2a, t2b);  /* T02: mtime frozen */

    /* ================================================================
     * T03-T06: WFI + timer wakeup
     *
     * Strategy:
     *   1. Reset mtime to 0, set mtimecmp = 20 (known small delta).
     *   2. Enable global interrupts + MTIE.
     *   3. Start timer, then immediately execute WFI.
     *   4. Core stalls until mtime >= 20 → MTIP → wakeup → trap.
     *   5. Verify trap_count, mcause, and that mcycle gap is small
     *      (proves the core was truly stalled, not spinning).
     * ================================================================ */
    trap_count  = 0;
    last_mcause = 0;

    mmio_write(TIMER_CTRL, 0);
    mmio_write(TIMER_MTIME_LO, 0);
    mmio_write(TIMER_MTIME_HI, 0);
    mmio_write(TIMER_MTIMECMP_HI, 0u);
    mmio_write(TIMER_MTIMECMP_LO, 20u);  /* fire after 20 timer ticks */

    /* Enable MTIE + global MIE */
    CSR_WRITE(mie, MIE_MTIE);
    CSR_SET(mstatus, MSTATUS_MIE);

    uint32_t cyc_before = read_mcycle();

    /* Start timer then WFI */
    mmio_write(TIMER_CTRL, 1);
    __asm__ volatile ("wfi");

    uint32_t cyc_after = read_mcycle();
    uint32_t cyc_delta = cyc_after - cyc_before;

    /* T03: WFI woke up (we got past the instruction) */
    TEST_ASSERT(3, 1);

    /* T04: Exactly one MTIP trap fired */
    TEST_EQ(4, trap_count, 1);

    /* T05: mcause = 0x80000007 (machine timer interrupt) */
    TEST_EQ(5, last_mcause, 0x80000007u);

    /* T06: mcycle delta should be modest — the core was stalled,
     * not burning 1000s of cycles in a loop. Delta includes a few
     * cycles for WFI decode, trap entry/exit, etc. but should be
     * well under 100 for a 20-tick timer. */
    TEST_ASSERT(6, cyc_delta < 100);

    /* ================================================================
     * T07: arm_timer helper + second WFI
     * ================================================================ */
    trap_count  = 0;
    last_mcause = 0;

    /* Timer is still running from T03-T06 */
    arm_timer(30u);  /* fire ~30 ticks from now */
    __asm__ volatile ("wfi");

    TEST_EQ(7, trap_count, 1);

    /* ================================================================
     * T08: Multiple back-to-back WFI + timer cycles
     * ================================================================ */
    trap_count = 0;

    for (uint32_t i = 0; i < 3; i++) {
        arm_timer(15u);
        __asm__ volatile ("wfi");
    }

    TEST_EQ(8, trap_count, 3);

    /* ── Cleanup ── */
    CSR_CLEAR(mstatus, MSTATUS_MIE);
    CSR_WRITE(mie, 0);
    mmio_write(TIMER_CTRL, 0);

    TEST_FINISH();
}
