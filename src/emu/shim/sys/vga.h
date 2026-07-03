/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for vga/sys/vga.h. The emulator replaces the dual-core PIO scanout
 * with a single in-process renderer (term.c registers its per-scanline callback
 * through vga_prog_exclusive). That firmware VGA ABI lives in the emulator's own
 * VGA module header; this shim just forwards the firmware path "sys/vga.h" to it
 * so the vendored term.c / mode renderers compile unchanged.
 */

#ifndef _EMU_SHIM_SYS_VGA_H_
#define _EMU_SHIM_SYS_VGA_H_

#include "emu/sys/vga.h"

#endif /* _EMU_SHIM_SYS_VGA_H_ */
