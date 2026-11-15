/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The .text.init section is placed first because the Hazard3 reset
 * vector is address zero.
 *
 * ROM is the ROM block, which the host loads from firmware.bin and
 * nothing writes. RAM is the RAM block. firmware.bin is everything
 * placed in ROM: the code, the constants and the first values of .data,
 * which crt0.S copies to RAM. stage_map_gate.py checks both regions
 * against mmio.h.
 *
 * The VIDCELLS length must match the 15360-word cell memory in mode0.sv,
 * which holds 8-byte cells for up to 32 rows of the 40- and 80-column
 * terminals, each with a primary and an alternate screen.
 */

MEMORY
{
    ROM (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rw) : ORIGIN = 0x00040000, LENGTH = 64K
    VIDCELLS (rw) : ORIGIN = 0x50000000, LENGTH = 61440
}

ENTRY(_start)

/* core/sys/xram.h's xram is the fabric's XRAM window. */
xram = 0x30000000;

SECTIONS
{
    .text : {
        KEEP(*(.text.init))
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    } > ROM

    .data : {
        __data_start = .;
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT> ROM
    __data_load = LOADADDR(.data);

    /* This section comes before .bss because ld assigns each input
     * section to the first pattern that matches it. */
    .vidcells (NOLOAD) : {
        *(.bss.term40_pri_mem*)
        *(.bss.term40_alt_mem*)
        *(.bss.term80_pri_mem*)
        *(.bss.term80_alt_mem*)
    } > VIDCELLS

    /* The thread-local variables, which are picolibc's errno. A
     * thread-local section takes no room of its own, so .bss leaves room
     * for it at __tls, where crt0.S points tp. */
    .tbss : {
        *(.tbss*)
    } > RAM

    .bss : {
        __bss_start = .;
        __tls = .;
        . += SIZEOF(.tbss);
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    __stack_top = ORIGIN(RAM) + LENGTH(RAM);

    __heap_start = __bss_end;
    __heap_end = __stack_top - 4K;

    PROVIDE(regs = 0x20000000);
    PROVIDE(xstack = 0x20000100);
    PROVIDE(xstack_ptr = 0x20000320);
}
