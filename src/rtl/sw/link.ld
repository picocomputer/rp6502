/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

    __heap_start = __bss_end;
    __heap_end = __stack_top - 4K;

    PROVIDE(regs = 0x20000000);
    PROVIDE(xstack = 0x20000100);
    PROVIDE(xstack_ptr = 0x20000320);
}
