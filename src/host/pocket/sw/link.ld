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
 * VIDCELLS is the cell memory of mode0.sv and VIDREGS its registers.
 * core/vga/mode/mode0.c's cells and words go there, so cells.c writes
 * them where they are drawn from. The VIDCELLS length must match the
 * 15360-word cell memory in mode0.sv, which holds 8-byte cells for up to
 * 32 rows of the 40- and 80-column terminals, each with a primary and an
 * alternate screen. The registers take whole words only.
 *
 * VIDFONT is the font memories of font.sv, which the fabric draws from.
 * core/term/font.c's tables go there, one at each face's address, so
 * font.c builds them where they are drawn from. The faces take whole
 * words and cannot be read back, and font.c writes them that way.
 * -fdata-sections gives each table a section named after it.
 */

MEMORY
{
    ROM (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rw) : ORIGIN = 0x00040000, LENGTH = 64K
    VIDCELLS (rw) : ORIGIN = 0x50000000, LENGTH = 61440
    VIDREGS (rw) : ORIGIN = 0x50010000, LENGTH = 140
    VIDFONT (rw) : ORIGIN = 0x50040000, LENGTH = 16K
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
    .vidregs (NOLOAD) : {
        *(.bss.mode0_regs .bss.mode0_regs.*)
    } > VIDREGS
    ASSERT(SIZEOF(.vidregs) == 140, "mode0_regs is not on mode0.sv's registers")

    /* These come before .bss for the same reason. The sizes are the
     * faces' sizes in font.sv. */
    .font16 0x50040000 (NOLOAD) : {
        *(.bss.font16 .bss.font16.*)
    } > VIDFONT
    .font8 0x50041000 (NOLOAD) : {
        *(.bss.font8 .bss.font8.*)
    } > VIDFONT
    .italic16 0x50042000 (NOLOAD) : {
        *(.bss.italic16 .bss.italic16.*)
    } > VIDFONT
    .font_dec_16 0x50043000 (NOLOAD) : {
        *(.bss.font_dec_16 .bss.font_dec_16.*)
    } > VIDFONT
    .font_dec_8 0x50043200 (NOLOAD) : {
        *(.bss.font_dec_8 .bss.font_dec_8.*)
    } > VIDFONT
    ASSERT(SIZEOF(.font16) == 4096, "font16 is not on its face")
    ASSERT(SIZEOF(.font8) == 2048, "font8 is not on its face")
    ASSERT(SIZEOF(.italic16) == 2048, "italic16 is not on its face")
    ASSERT(SIZEOF(.font_dec_16) == 512, "font_dec_16 is not on its face")
    ASSERT(SIZEOF(.font_dec_8) == 256, "font_dec_8 is not on its face")

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
