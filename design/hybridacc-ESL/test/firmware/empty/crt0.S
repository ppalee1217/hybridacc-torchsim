/* Minimal startup for HybridAcc CoreMcu */

    .section .text.start, "ax", @progbits
    .global _start
    .type   _start, @function

_start:
    /* Set stack pointer to end of DSRAM (0x10000000 + 64KB) */
    lui     sp, %hi(0x10010000)
    addi    sp, sp, %lo(0x10010000)

    /* Zero the .bss segment */
    la      a0, __bss_start
    la      a1, __bss_end
1:  bge     a0, a1, 2f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       1b
2:
    /* Call main */
    call    main

    /* Halt: EBREAK */
    nop
    nop
    nop
    nop
    nop
    ebreak

    /* Safety loop (should never reach here) */
3:  j       3b
