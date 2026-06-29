/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * VGA glue (vga.c): the boot console, canvas selection, the vsync scanline, the
 * per-frame terminal housekeeping, and rendering the current frame into an
 * RGBA8 framebuffer at the canvas's native size.
 */

#ifndef _EMU_VGA_H_
#define _EMU_VGA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Install the boot console canvas (640x480 term) so the terminal renders
 * at startup without any xreg, matching real hardware. */
void vga_boot_console(void);

/* Select a canvas geometry (vga_canvas_t code from vga/sys/vga.h). */
void vga_set_canvas(uint16_t canvas);

/* The scanline at which vsync fires for the current frame — the highest
 * scanline any installed program renders (firmware fires ria_vsync there). */
int vga_vsync_scanline(void);

/* Per-frame terminal housekeeping (cursor blink, cell blink, lazy clears),
 * driven off the virtual clock. */
void vga_task(void);

/* Render the current frame into an RGBA8 (0xAABBGGRR) framebuffer at the
 * canvas's NATIVE size, tightly packed. EMU_FB_WIDTH x EMU_FB_HEIGHT is the
 * maximum (allocate that); emu_canvas_size reports the live dimensions. */
void vga_render_frame(uint32_t *fb);
void emu_canvas_size(int *w, int *h);

/* ------------------------------------------------------------------ */
/* Firmware VGA ABI reached by the vendored term.c / rln.c / the mode  */
/* renderers through the firmware path "sys/vga.h" (the shim there      */
/* forwards to this header). vga.c implements them; the emulator        */
/* collapses the dual-core PIO scanout into a single in-process render. */
/* ------------------------------------------------------------------ */

int16_t vga_canvas_height(void);

/* Canvas selector (mirrors ria/sys/vga.h vga_canvas_t). rln.c uses it for a
 * geometry fallback when the CPR handshake hasn't resolved a width yet. */
typedef enum
{
    vga_canvas_console = 0,
    vga_canvas_320_240,
    vga_canvas_320_180,
    vga_canvas_640_480,
    vga_canvas_640_360,
} vga_canvas_t;

bool vga_connected(void);          /* the emulator always has a display */
vga_canvas_t vga_get_canvas(void); /* current canvas (rln geometry fallback) */
uint8_t vga_get_display_type(void);

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

#ifdef __cplusplus
}
#endif

#endif /* _EMU_VGA_H_ */
