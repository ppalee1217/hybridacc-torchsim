/*
 * Linker script for HybridAcc CoreMcu firmware.
 *
 * Memory map:
 *   ISRAM:  0x00000000 .. 0x00003FFF  (16 KB) — code + rodata
 *   DSRAM:  0x10000000 .. 0x1000FFFF  (64 KB) — data + bss + stack
 */

MEMORY
{
    ISRAM (rx)  : ORIGIN = 0x00000000, LENGTH = 16K
    DSRAM (rwx) : ORIGIN = 0x10000000, LENGTH = 64K
}

ENTRY(_start)

SECTIONS
{
    /* Code and read-only data in ISRAM */
    .text : {
        *(.text.start)
        *(.text .text.*)
    } > ISRAM

    .rodata : {
        *(.rodata .rodata.*)
    } > ISRAM

    /* Initialized data — stored in ISRAM (LMA), copied to DSRAM at runtime.
       For this minimal test we avoid the copy by placing directly in DSRAM.
       The loader will load a DSRAM section for us. */
    .data : {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } > DSRAM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    } > DSRAM

    /* Stack grows downward from top of DSRAM (set in crt0) */
    __stack_top = ORIGIN(DSRAM) + LENGTH(DSRAM);
}
