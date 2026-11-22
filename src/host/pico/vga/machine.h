/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

#include <pico.h>

/* Code and data take separate sections, because GCC rejects both in one
 * named section of a file. A function the compiler inlines lands where its
 * caller is, so HOST_IN_FLASH_FUNC is never inlined. */
#define HOST_IN_FLASH __in_flash("data")
#define HOST_IN_FLASH_FUNC __noinline __in_flash("code")
#define HOST_IN_RAM __not_in_flash("data")
#define HOST_IN_RAM_FUNC __not_in_flash("code")
/* An interrupt handler runs from RAM with everything it calls inlined. */
#define HOST_ISR_FUNC __attribute__((flatten, optimize("O3"))) __not_in_flash("code")

/* The SXGA console has 512 scanlines and a 16-line font, which gives 32
 * rows. */
#define TERM_MAX_HEIGHT 32

/* Core 0 renders a scanline each pass of the main loop and may be away
 * about 200 us before a line is late. A row of a job is 5 to 12 us, so 11
 * a pass are at most about 130 us, and a whole screen of 32 or 33 rows takes
 * three passes. Measured with termload (tests/hil), a whole-screen job at
 * both sizes is done inside the RIA's hold (ria/sys/vga.c). */
#define TERM_JOB_ROWS 11

/* Mode 0 reads its words as it draws each scanline, with no time to spare
 * under two 8-bit bitmaps, so they change once a frame at most
 * (core/term/draw.h). */
#define DRAW_PUBLISH_PER_FRAME 1

#endif /* _HOST_MACHINE_H_ */
