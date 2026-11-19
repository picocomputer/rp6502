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
 * VIDCELLS, VIDCOLORS and VIDPALETTE are the memories of mode0.sv and
 * VIDREGS its registers. core/vga/mode/mode0.c's cells, colors, palette
 * and words go there, so cells.c writes them where they are drawn from.
 * The lengths are the memories' in mode0.sv, which hold 30 rows of the
 * 40- and 80-column terminals, each with a primary and an alternate
 * screen. The registers take whole words only.
 *
 * VIDFONT is the font memories of font.sv, which the fabric draws from.
 * core/term/font.c's tables go there, each font at its address, so
 * font.c builds them where they are drawn from. The fonts take whole
 * words and cannot be read back, and font.c writes them that way.
 * -fdata-sections gives each table a section named after it.
 */

MEMORY
{
    ROM (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rw) : ORIGIN = 0x00040000, LENGTH = 64K
    VIDCELLS (rw) : ORIGIN = 0x50000000, LENGTH = 29760
    VIDREGS (rw) : ORIGIN = 0x50010000, LENGTH = 156
    VIDFONT (rw) : ORIGIN = 0x50040000, LENGTH = 16K
    VIDCOLORS (rw) : ORIGIN = 0x50080000, LENGTH = 44640
    VIDPALETTE (rw) : ORIGIN = 0x50100000, LENGTH = 1032
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
        *(.bss.mode0_cells .bss.mode0_cells.*)
    } > VIDCELLS
    .vidcolors (NOLOAD) : {
        *(.bss.mode0_colors .bss.mode0_colors.*)
    } > VIDCOLORS
    .vidpalette (NOLOAD) : {
        *(.bss.mode0_palette .bss.mode0_palette.*)
    } > VIDPALETTE
    .vidregs (NOLOAD) : {
        *(.bss.mode0_regs .bss.mode0_regs.*)
    } > VIDREGS
    ASSERT(SIZEOF(.vidcells) == 29760, "mode0_cells is not mode0.sv's cells")
    ASSERT(SIZEOF(.vidcolors) == 44640, "mode0_colors is not mode0.sv's colors")
    ASSERT(SIZEOF(.vidpalette) == 1032, "mode0_palette is not mode0.sv's palette")
    ASSERT(SIZEOF(.vidregs) == 156, "mode0_regs is not on mode0.sv's registers")

    /* These come before .bss for the same reason. The sizes are the
     * fonts' sizes in font.sv. */
    .font16 0x50040000 (NOLOAD) : {
        *(.bss.font16 .bss.font16.*)
    } > VIDFONT
    .font8 0x50042000 (NOLOAD) : {
        *(.bss.font8 .bss.font8.*)
    } > VIDFONT
    ASSERT(SIZEOF(.font16) == 8192, "font16 is not on its memory")
    ASSERT(SIZEOF(.font8) == 4096, "font8 is not on its memory")

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
