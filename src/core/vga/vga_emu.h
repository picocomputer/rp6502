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
void vga_run(void);

/* The console canvas again, at a size a host gives, with the frame count
 * the beam's. The canvas and the table are put back by the console reset
 * at the start of the next vga_task, before any row is drawn. */
void vga_reset(void);

void vga_task(void);

/* Scanlines the beam has passed since boot, blanking included; only a
 * savestate load ever sets it to anything else. bus_task turns the ones the
 * 6502 has not answered for into a cycle budget, and host_clock_us turns the
 * whole count into microseconds. */
uint64_t vga_beam_lines(void);

/* Runs the machine to the next frame, or until a debugger holds it, then
 * steps the mix up to where it got. False when it was held. */
bool vga_run_frame(void);

/* No canvas is larger than 640x480, and a framebuffer must hold that. */
#define VGA_MAX_WIDTH 640
#define VGA_MAX_HEIGHT 480

#define VGA_HZ 60         /* the RP6502 VGA is always 60 Hz */
#define VGA_FRAME_NS (1000000000ull / VGA_HZ)
#define VGA_SCANLINES 525 /* 640x480@60 total scanlines (480 visible + blanking) */

/* The framebuffer the scanlines render into, which the caller owns: RGBA8 at
 * the canvas width, large enough for the largest canvas. NULL skips pixel
 * work. */
void vga_set_framebuffer(uint32_t *fb);

/* Whether to paint at all, on by default. Off skips the per-scanline render
 * and nothing else, so the machine runs identically either way. Its one caller
 * flips it immediately before vga_run_frame, which returns on a frame boundary
 * unless a debugger stopped it, so the first frame painted after it comes back
 * on is whole. */
void vga_set_scanout(bool on);

uint32_t *vga_get_framebuffer(void);

bool vga_frame_crc(uint32_t *crc);

/* The savestate carries the beam and the frame's vsync. The frame count is
 * the beam divided by the scanlines in a frame, and the canvas and the
 * scanline program are their own files' rows. */
extern const sst_table_t vga_sst;

#define VGA_DRIVER DRIVER(                                  \
    INIT(vga_init)                                          \
    TASK_PUMPED(vga_task)                                   \
    RUN(vga_run)                                            \
    STOP(vga_stop)                                          \
    RESET(vga_reset)                                        \
    SST("VGA_", vga))

#endif /* _CORE_VGA_VGA_EMU_H_ */
