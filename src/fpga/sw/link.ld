/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One RAM: the 128 KB tightly-coupled memory at zero, code first because the
 * reset vector is address zero. The stack grows down from the top. Sizing it
 * down buys no block memory on the Pocket — the fabric allocates a
 * power-of-two depth, so anything above 64 KB costs the same 128 M10Ks.
 */

MEMORY
{
    TCM (rwx) : ORIGIN = 0x00000000, LENGTH = 128K
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

    /* term.c's four screen buffers land behind the vid window, where the
     * hardware is the memory; claimed here so the .bss glob below cannot.
     * NOLOAD: BRAM powers up zero, nothing to copy or clear. */
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

    /* The RIA register cells are hardware; REGS() indexes them here, and
     * the xstack and its pointer live behind the same window. */
    PROVIDE(regs = 0x20000000);
    PROVIDE(xstack = 0x20000100);
    PROVIDE(xstack_ptr = 0x20000320);
}
