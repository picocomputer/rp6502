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

/* Perform an armed console reset via the DISPLAY xreg; call once per frame. */
void vga_task(void);

/* The scanline at which vsync fires for the current frame — the highest
 * scanline any installed program renders (firmware fires ria_vsync there). */
int16_t vga_vsync_scanline(void);

/* Render one scanline y of the current frame into the present buffer (RGBA8
 * 0xAABBGGRR, canvas-native stride). Interleaved with the CPU between scanlines
 * so mid-frame state changes land on later lines (raster effects), matching
 * real per-scanline scanout. */
void vga_render_scanline(int y);

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

#endif /* _CORE_VGA_VGA_EMU_H_ */
