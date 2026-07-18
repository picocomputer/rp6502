/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for scanvideo/scanvideo.h. The vendored term.c/color.c only use
 * the pixel-format macros (the shared scanvideo/pixel_format.h); the PIO/DMA
 * scanout engine is not modeled (the emulator runs the per-scanline render
 * callbacks itself).
 *
 * Pixel format is 16-bit RGB555 with alpha at bit 5:
 *   bits 0-4  = red,  bit 5 = alpha, bits 6-10 = green, bits 11-15 = blue.
 */

#ifndef _EMU_SHIM_SCANVIDEO_SCANVIDEO_H_
#define _EMU_SHIM_SCANVIDEO_SCANVIDEO_H_

#include <stdint.h>

#include "vga/scanvideo/pixel_format.h"

#endif /* _EMU_SHIM_SCANVIDEO_SCANVIDEO_H_ */
