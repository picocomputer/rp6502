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
 * catching up to vga_beam_clk() -- on hardware the two run at once, here
 * they zip. */
void vga_task(void);

/* The machine clock the beam has reached: what the CPU is owed. */
uint64_t vga_beam_clk(void);

/* Frames completed. Pumping until this moves is how every host asks for a
 * frame -- a window, a frontend, a script, a screenshot, a test. */
unsigned long vga_frame_count(void);

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
 * This file is only what the emulator additionally has: a framebuffer. */

/* What the beam waits for. Both modes release one scanline at a time, so the
 * CPU zips in between either way; they differ only in what real time is held
 * against. A host presenting a framebuffer keeps whole frames on time; a host
 * consuming scanlines keeps each line on time. Unpaced -- the default -- never
 * waits, which is what a script, a screenshot batch, a test and a
 * frontend-paced core all want. */
typedef enum
{
    VGA_PACE_NONE,
    VGA_PACE_FRAME,
    VGA_PACE_SCANLINE,
} vga_pace_t;

void vga_set_pace(vga_pace_t pace);

/* This driver's machine-lifecycle row; see core/lifecycle.h. Video leads: its
 * task runs before the CPU's, which follows the beam. */
#define VGA_MACH_LIFECYCLE LIFECYCLE(vga_init, vga_task, nul_task, nul_run, vga_stop, nul_break)

#endif /* _CORE_VGA_VGA_EMU_H_ */
