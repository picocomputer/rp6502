/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One RAM: the 48 KB tightly-coupled memory at zero, code first because the
 * reset vector is address zero. The stack grows down from the top.
 *
 * The size is a block-memory decision as much as a software one. Each byte
 * lane costs one block per 1024 words, so 48 KB is forty-eight of them and
 * 64 KB would be sixty-four. The blocks for 64 KB now exist — a harvest
 * found nine and eight were already free — but the sizes between are not
 * free to pick: Quartus decomposes a depth into at most two power-of-two
 * chunks and rounds up, so 56 KB costs the same sixty-four as 64 KB unless
 * the lanes are banked by hand, and banking means per-bank init files
 * through rv_tcm_gen.py and every tb_load_tcm call site. Left at 48 KB
 * until the final block count is known and the size can be chosen once.
 */

MEMORY
{
    TCM (rwx) : ORIGIN = 0x00000000, LENGTH = 48K
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
