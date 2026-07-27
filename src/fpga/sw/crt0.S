/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Execution starts here, at the reset vector. The TCM arrives preloaded, so
 * there is no data copy — just a stack, a cleared BSS, and main.
 */

    .section .text.init
    .global _start
_start:
    la sp, __stack_top

    la a0, __bss_start
    la a1, __bss_end
1:  bgeu a0, a1, 2f
    sw zero, 0(a0)
    addi a0, a0, 4
    j 1b
2:
    call main

    /* main returned: report its result and stop. */
    li t0, 0xF0000004
    sw a0, 0(t0)
3:  wfi
    j 3b
