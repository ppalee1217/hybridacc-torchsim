/* Minimal trap/interrupt handler for HybridAcc CoreMcu tests.
 *
 * Saves caller-saved registers, calls C handler trap_handler(),
 * restores context and returns via MRET.
 *
 * The C handler signature:
 *   void trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval);
 *
 * The default (weak) implementation is a no-op so tests that don't
 * need traps can simply not define it.
 */

    .section .text.trap_entry, "ax", @progbits
    .global trap_entry
    .type   trap_entry, @function
    .align 4
trap_entry:
    /* Save caller-saved regs on stack */
    addi    sp, sp, -64
    sw      ra,  0(sp)
    sw      t0,  4(sp)
    sw      t1,  8(sp)
    sw      t2, 12(sp)
    sw      a0, 16(sp)
    sw      a1, 20(sp)
    sw      a2, 24(sp)
    sw      a3, 28(sp)
    sw      a4, 32(sp)
    sw      a5, 36(sp)
    sw      a6, 40(sp)
    sw      a7, 44(sp)
    sw      t3, 48(sp)
    sw      t4, 52(sp)
    sw      t5, 56(sp)
    sw      t6, 60(sp)

    /* Load trap info as arguments */
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval

    /* Call C handler */
    call    trap_handler

    /* Restore context */
    lw      ra,  0(sp)
    lw      t0,  4(sp)
    lw      t1,  8(sp)
    lw      t2, 12(sp)
    lw      a0, 16(sp)
    lw      a1, 20(sp)
    lw      a2, 24(sp)
    lw      a3, 28(sp)
    lw      a4, 32(sp)
    lw      a5, 36(sp)
    lw      a6, 40(sp)
    lw      a7, 44(sp)
    lw      t3, 48(sp)
    lw      t4, 52(sp)
    lw      t5, 56(sp)
    lw      t6, 60(sp)
    addi    sp, sp, 64

    mret

    /* Default weak trap_handler — NOP */
    .weak   trap_handler
    .type   trap_handler, @function
trap_handler:
    ret
