/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for scanvideo/scanvideo.h. The vendored term.c/color.c only
 * use the pixel-format macros below; the PIO/DMA scanout engine is not
 * modeled (the emulator runs the per-scanline render callbacks itself).
 *
 * Pixel format is 16-bit RGB555 with alpha at bit 5:
 *   bits 0-4  = red,  bit 5 = alpha, bits 6-10 = green, bits 11-15 = blue.
 */

#ifndef _EMU_SHIM_SCANVIDEO_SCANVIDEO_H_
#define _EMU_SHIM_SCANVIDEO_SCANVIDEO_H_

#include <stdint.h>

/* Number of overlay planes the scanout composites (matches the firmware). The
 * graphics modes index their per-scanline state by plane. */
#define SCANVIDEO_PLANE_COUNT 3

#define SCANVIDEO_ALPHA_PIN 5u
#define SCANVIDEO_ALPHA_MASK (1u << SCANVIDEO_ALPHA_PIN)
#define SCANVIDEO_PIXEL_RSHIFT 0u
#define SCANVIDEO_PIXEL_GSHIFT 6u
#define SCANVIDEO_PIXEL_BSHIFT 11u
#define SCANVIDEO_PIXEL_RCOUNT 5u
#define SCANVIDEO_PIXEL_GCOUNT 5u
#define SCANVIDEO_PIXEL_BCOUNT 5u
#define SCANVIDEO_PIXEL_FROM_RGB8(r, g, b) ((((b) >> 3u) << SCANVIDEO_PIXEL_BSHIFT) | (((g) >> 3u) << SCANVIDEO_PIXEL_GSHIFT) | (((r) >> 3u) << SCANVIDEO_PIXEL_RSHIFT))
#define SCANVIDEO_PIXEL_FROM_RGB5(r, g, b) (((b) << SCANVIDEO_PIXEL_BSHIFT) | ((g) << SCANVIDEO_PIXEL_GSHIFT) | ((r) << SCANVIDEO_PIXEL_RSHIFT))
#define SCANVIDEO_R5_FROM_PIXEL(p) (((p) >> SCANVIDEO_PIXEL_RSHIFT) & 0x1f)
#define SCANVIDEO_G5_FROM_PIXEL(p) (((p) >> SCANVIDEO_PIXEL_GSHIFT) & 0x1f)
#define SCANVIDEO_B5_FROM_PIXEL(p) (((p) >> SCANVIDEO_PIXEL_BSHIFT) & 0x1f)

#endif /* _EMU_SHIM_SCANVIDEO_SCANVIDEO_H_ */
