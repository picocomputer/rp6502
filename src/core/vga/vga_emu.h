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
#define VGA_FRAME_NS (1000000000ull / VGA_HZ)
#define VGA_SCANLINES 525 /* 640x480@60 total scanlines (480 visible + blanking) */

/* Register the app-owned framebuffer the scanlines render into (RGBA8, canvas
 * stride; must hold the largest canvas). NULL skips pixel work. */
void vga_set_framebuffer(uint32_t *fb);

/* Whether to paint at all. Off skips the per-scanline render and nothing
 * else; the machine runs identically either way. Defaults on. */
void vga_set_scanout(bool on);

/* What the last rendered frame went into, for a caller that wants the pixels
 * without owning them (a screenshot, a frame hash). NULL when none is set. */
uint32_t *vga_get_framebuffer(void);

/* The last rendered frame as a CRC-32, for a script or a batch run to
 * compare. False when no framebuffer is registered. */
bool vga_frame_crc(uint32_t *crc);

/* The rest of what a machine's video answers -- the canvas, the scanline
 * program, the code page -- is core/vga/vga.h, which every machine shares.
 * This file is only what the emulator additionally has: a framebuffer, and
 * the beam as the clock the whole machine follows. */

/* This driver's row in a machine's driver list; see core/sys/driver.h. Video leads: its
 * task runs before the CPU's, which follows the beam. */
/* The beam, which is this machine's clock as well as its picture, and the
 * canvas the beam is painting. frame_n is not carried: it is beam_n divided
 * by the scanlines in a frame, exactly, so carrying both would be carrying
 * one fact twice.
 *
 * vga_needs_reset is armed teardown, not a cosmetic flag. A machine saved
 * with it set owes itself a console reset on the next task pass, and it gets
 * one, which is what the machine that made the blob would have done.
 *
 * The scanline program goes with it: for every row a plane can draw, which
 * renderer draws it and what it reads. A renderer is named by mode and
 * attribute rather than by address, because an address is this build's own.
 * The booking that installed it cannot be replayed instead: the table is the
 * fold of an unbounded sequence of bookings that overwrite one another, and
 * the ranges they were made over are gone.
 *
 * Rows at or past 480 are never booked, because every booking is bounded
 * against the canvas and no canvas is taller than that.
 *
 * 8 beam, 1 vsynced, 1 needs_reset, 2 canvas, 2 watermark, 2 mode-0 begin,
 * then the rows: 3 planes of 12 each. */
#define VGA_SST_ROWS 480
#define VGA_SST_SIZE (16 + VGA_SST_ROWS * SCANVIDEO_PLANE_COUNT * 12)
/* A row of the scanline table names its renderer by mode and attribute
 * rather than by address. These are the two directions, over every mode this
 * machine has. A mode of 0xFF is an empty slot. */
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
