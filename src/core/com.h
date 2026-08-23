/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What the console answers for the machine. Every machine has one and each
 * writes its own driver -- a UART and CDC on the Pico, a ring the emulator
 * fills, the APF bridge on a Pocket -- so this is the part of com the machine
 * itself reaches for, not the whole driver.
 *
 * One function so far. It was declared by hand in three com.h at once
 * (vga/sys, emu/sys, rtl/sw), each with a comment saying term.c needed it,
 * which is what a missing contract looks like. */

#ifndef _CORE_COM_H_
#define _CORE_COM_H_

#include <stddef.h>

/* A terminal query's answer (DSR/CPR/DA), entering the console's input as
 * though it had been typed -- as the UART source, ahead of typed input, since
 * the program asked for it and is waiting. Dropped rather than truncated if it
 * does not fit, and dropped entirely where a real terminal is attached and
 * will answer the host's query itself. */
void com_in_write_reply(const char *s, size_t n);

#endif /* _CORE_COM_H_ */
