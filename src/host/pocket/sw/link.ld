/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Code first because the reset vector is address zero.
 *
 * VIDCELLS length must match mode0.sv, and covers both screens, so
 * it moves with TERM_ALT_SCREEN in vga/term/term.h.
 */

MEMORY
{
    TCM (rwx) : ORIGIN = 0x00000000, LENGTH = 96K
    VIDCELLS (rw) : ORIGIN = 0x50000000, LENGTH = 61440
}

ENTRY(_start)

SECTIONS
{
    .text : {
        KEEP(*(.text.init))
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    } > TCM

    .data : {
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
    } > TCM

    /* Claimed before the .bss glob below can take them. NOLOAD: BRAM
     * powers up zero, nothing to copy or clear. */
    .vidcells (NOLOAD) : {
        *(.bss.term40_pri_mem*)
        *(.bss.term40_alt_mem*)
        *(.bss.term80_pri_mem*)
        *(.bss.term80_alt_mem*)
    } > VIDCELLS

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > TCM

    __stack_top = ORIGIN(TCM) + LENGTH(TCM);

    /* The heap exists for setenv: the time zone rides the environment so
     * localtime, mktime and strftime all read one offset. */
    __heap_start = __bss_end;
    __heap_end = __stack_top - 4K;

    /* Hardware register cells, not memory. */
    PROVIDE(regs = 0x20000000);
    PROVIDE(xstack = 0x20000100);
    PROVIDE(xstack_ptr = 0x20000320);
}
