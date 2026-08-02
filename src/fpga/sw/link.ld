/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One RAM: the 64 KB tightly-coupled memory at zero, code first because the
 * reset vector is address zero. The stack grows down from the top.
 *
 * The TCM was 48 KB for a while, and the alternate screen buffer paid for
 * the growth to 64: turning it off halved the cell store while the blocks
 * were not there. The buffer came back for a day when the blocks did, and
 * went back out when the device ran short of LABs instead — packing the
 * die to its last rows made every fit a coin toss. See TERM_ALT_SCREEN in
 * vga/term/term.h.
 */

MEMORY
{
    TCM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
    VIDCELLS (rw) : ORIGIN = 0x50000000, LENGTH = 30720
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

    /* Not cleared by crt0 and not loaded from the bitstream, which is
     * the whole point: what is here after a boot came through that boot
     * alive. It is how the machine tells a reset from a reconfigure. */
    .noinit (NOLOAD) : {
        . = ALIGN(4);
        __noinit_start = .;
        *(.noinit*)
        . = ALIGN(4);
        __noinit_end = .;
    } > TCM

    __stack_top = ORIGIN(TCM) + LENGTH(TCM);

    /* The heap exists for setenv: the time zone rides the environment
     * so the C library's localtime, mktime and strftime all read one
     * offset. A few short strings, once — but sbrk needs its bounds,
     * and the top is held back so the stack keeps room of its own. */
    __heap_start = __noinit_end;
    __heap_end = __stack_top - 4K;

    /* The RIA register cells are hardware; REGS() indexes them here, and
     * the xstack and its pointer live behind the same window. */
    PROVIDE(regs = 0x20000000);
    PROVIDE(xstack = 0x20000100);
    PROVIDE(xstack_ptr = 0x20000320);
}
