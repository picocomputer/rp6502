/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_VGA_H_
#define _EMU_SYS_VGA_H_

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

/* Select a canvas geometry (vga_canvas_t code from vga/sys/vga.h). Returns
 * false for an out-of-range code, leaving the canvas state unchanged. */
bool vga_set_canvas(uint16_t canvas);

/* The scanline at which vsync fires for the current frame — the highest
 * scanline any installed program renders (firmware fires ria_vsync there). */
int vga_vsync_scanline(void);

/* Render one scanline y of the current frame into the present buffer (RGBA8
 * 0xAABBGGRR, canvas-native stride). Interleaved with the CPU between scanlines
 * so mid-frame state changes land on later lines (raster effects), matching
 * real per-scanline scanout. */
void vga_render_scanline(int y);
void vga_canvas_size(int *w, int *h);

/* The largest canvas (the 640x480 boot console); framebuffer owners size
 * their storage with these. */
#define VGA_MAX_WIDTH 640
#define VGA_MAX_HEIGHT 480

#define VGA_HZ 60         /* the RP6502 VGA is always 60 Hz */
#define VGA_SCANLINES 525 /* 640x480@60 total scanlines (480 visible + blanking) */

/* Register the app-owned framebuffer the scanlines render into (RGBA8, canvas
 * stride; must hold the largest canvas). NULL skips pixel work. */
void vga_set_framebuffer(uint32_t *fb);

/* ------------------------------------------------------------------ */
/* Firmware VGA ABI reached by the vendored term.c / rln.c / the mode  */
/* renderers through the firmware path "sys/vga.h", which the emu       */
/* include path resolves here directly. vga.c implements them; the      */
/* emulator collapses the PIO scanout into a single in-process render.  */
/* ------------------------------------------------------------------ */

int16_t vga_canvas_height(void);

/* Canvas selector (mirrors ria/sys/vga.h vga_canvas_t). */
typedef enum
{
    vga_canvas_console = 0,
    vga_canvas_320_240,
    vga_canvas_320_180,
    vga_canvas_640_480,
    vga_canvas_640_360,
} vga_canvas_t;

bool vga_connected(void);          /* the emulator always has a display */
vga_canvas_t vga_get_canvas(void);
uint8_t vga_get_display_type(void);
void vga_set_code_page(uint16_t cp); /* no PIX bus; loads the font directly */

#define VGA_PROG_MAX 512

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr,
                   bool (*fill_fn)(int16_t plane_id, int16_t scanline,
                                   int16_t width, uint16_t *rgb,
                                   uint16_t config_ptr));

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr,
                        bool (*fill_fn)(int16_t plane_id, int16_t scanline,
                                        int16_t width, uint16_t *rgb,
                                        uint16_t config_ptr));

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length,
                     void (*sprite_fn)(int16_t scanline, int16_t width,
                                       uint16_t *rgb, uint16_t config_ptr,
                                       uint16_t length));

#endif /* _EMU_SYS_VGA_H_ */
