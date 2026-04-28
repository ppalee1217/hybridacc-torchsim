/*
 * Linker script for HybridAcc CoreMcu firmware tests.
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
    .text : {
        *(.text.start)
        *(.text.trap_entry)
        *(.text .text.*)
    } > ISRAM

    .rodata : {
        *(.rodata .rodata.*)
    } > ISRAM

    /* Reserve 0x200 bytes (512 B) at DSRAM base for test results + work area,
     * then place BSS after that to avoid overlap.
     */
    .result_reserved (NOLOAD) : {
        . = . + 0x200;
    } > DSRAM

    .data : {
        __data_start = .;
        *(.sdata .sdata.*)
        *(.data .data.*)
        __data_end = .;
    } > DSRAM AT> DSRAM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.* COMMON)
        __bss_end = .;
    } > DSRAM

    __stack_top = ORIGIN(DSRAM) + LENGTH(DSRAM);
}
