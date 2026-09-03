/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_VGA_VGA_EMU_H_
#define _CORE_VGA_VGA_EMU_H_

#include "core/vga/vga.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Install the boot console canvas (640x480 term) so the terminal renders
 * at startup without any xreg, matching real hardware. */
void vga_init(void);

/* Arm a console reset for the next vga_task() when a program stops (firmware vga_stop). */
void vga_stop(void);

/* Advance the beam at most one scanline: render it, fire vsync where the
 * program's last line falls, count the frame at the wrap. The 6502 follows,
 * catching up to vga_beam_lines() -- on hardware the two run at once, here
 * they zip. */
void vga_task(void);

/* Scanlines the beam has done, ever. The bus turns the ones it has not
 * answered for yet into a cycle budget; host_clock_us turns the whole count
 * into microseconds. */
uint64_t vga_beam_lines(void);

/* Run the machine until video says one frame went by. False when a
 * debugger holds it -- a held machine never will, and a caller must not
 * wait for it. */
bool vga_run_frame(void);

/* The largest canvas (the 640x480 boot console); framebuffer owners size
 * their storage with these. */
#define VGA_MAX_WIDTH 640
#define VGA_MAX_HEIGHT 480

#define VGA_HZ 60         /* the RP6502 VGA is always 60 Hz */
#define VGA_SCANLINES 525 /* 640x480@60 total scanlines (480 visible + blanking) */

/* Register the app-owned framebuffer the scanlines render into (RGBA8, canvas
 * stride; must hold the largest canvas). NULL skips pixel work. */
void vga_set_framebuffer(uint32_t *fb);

/* What the last rendered frame went into, for a caller that wants the pixels
 * without owning them (a screenshot, a frame hash). NULL when none is set. */
uint32_t *vga_get_framebuffer(void);

/* The rest of what a machine's video answers -- the canvas, the scanline
 * program, the code page -- is core/vga/vga.h, which every machine shares.
 * This file is only what the emulator additionally has: a framebuffer, and
 * the beam as the clock the whole machine follows. */

/* This driver's row in a machine's driver list; see core/sys/driver.h. Video leads: its
 * task runs before the CPU's, which follows the beam. */
#define VGA_DRIVER DRIVER(vga_init, vga_task, nul_task, nul_run, vga_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_VGA_VGA_EMU_H_ */
