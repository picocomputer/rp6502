/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_VGA_VGA_EMU_H_
#define _CORE_VGA_VGA_EMU_H_

#include "core/vga/vga.h"
#include "core/vga/prog.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Installs the console canvas, so the terminal draws at startup without a
 * program having written any xreg, as it does on hardware. */
void vga_init(void);

void vga_stop(void);

void vga_task(void);

/* Scanlines the beam has passed since boot, blanking included; only a
 * savestate load ever sets it to anything else. bus_task turns the ones the
 * 6502 has not answered for into a cycle budget, and host_clock_us turns the
 * whole count into microseconds. */
uint64_t vga_beam_lines(void);

bool vga_run_frame(void);

/* No canvas is larger than 640x480, and a framebuffer must hold that. */
#define VGA_MAX_WIDTH 640
#define VGA_MAX_HEIGHT 480

#define VGA_HZ 60         /* the RP6502 VGA is always 60 Hz */
#define VGA_FRAME_NS (1000000000ull / VGA_HZ)
#define VGA_SCANLINES 525 /* 640x480@60 total scanlines (480 visible + blanking) */

/* The framebuffer the scanlines render into, which the app owns: RGBA8 at the
 * canvas width, large enough for the largest canvas. NULL skips pixel work. */
void vga_set_framebuffer(uint32_t *fb);

/* Whether to paint at all, on by default. Off skips the per-scanline render
 * and nothing else, so the machine runs identically either way. Its one caller
 * flips it immediately before vga_run_frame, which returns on a frame boundary
 * unless a debugger stopped it, so the first frame painted after it comes back
 * on is whole. */
void vga_set_scanout(bool on);

uint32_t *vga_get_framebuffer(void);

bool vga_frame_crc(uint32_t *crc);

/* The savestate carries the beam, the canvas, and the scanline program.
 * frame_n is not in it, because it is beam_n divided by the scanlines in a
 * frame exactly.
 *
 * Rows at or past 480 are never booked, because every booking is bounded
 * against the canvas and no canvas the emulator has is taller than that.
 *
 * The 16 leading bytes are 8 beam, 1 vsynced, 1 needs_reset, 2 canvas,
 * 2 highest scanline and 2 mode-0 begin; each row is 3 planes of 12 bytes. */
#define VGA_SST_ROWS 480
#define VGA_SST_SIZE (16 + VGA_SST_ROWS * SCANVIDEO_PLANE_COUNT * 12)

#define VGA_MODE_NONE 0xFF
vga_fill_fn_t vga_mode_fill_fn(uint8_t mode, uint16_t attributes);
bool vga_mode_fill_id(vga_fill_fn_t fn, int16_t scanline, int16_t plane,
                      uint8_t *mode, uint16_t *attributes);
vga_sprite_fn_t vga_mode_sprite_fn(uint8_t mode, uint16_t attributes);
bool vga_mode_sprite_id(vga_sprite_fn_t fn, uint8_t *mode, uint16_t *attributes);

void vga_sst_save(sst_cursor_t *c, unsigned flags);
bool vga_sst_load(sst_cursor_t *c, unsigned flags);

#define VGA_DRIVER DRIVER(vga_init, vga_task, nul_task, nul_run, vga_stop, nul_break, \
    nul_config, nul_config, SST(VGA_, 1, VGA_SST_SIZE, vga_sst_save, vga_sst_load))

#endif /* _CORE_VGA_VGA_EMU_H_ */
