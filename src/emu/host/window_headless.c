/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * No-window build: the emulator still runs ROMs and renders via --screenshot,
 * but there is no live window. Built (instead of window_core.c + a per-host
 * window.c) when EMU_HAS_WINDOW is OFF — i.e. on Linux without the GL/X11 dev
 * headers. Provides the window.h symbols the app references as no-ops.
 */

#include "emu/host/window.h"
#include <stdint.h>
#include <stdio.h>

int window_run(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
{
    (void)fb;
    (void)scale;
    (void)have_scale;
    (void)vsync;
    (void)exit_on_halt;
    fprintf(stderr, "rp6502-emu: built without window support; use --screenshot\n");
    return 1;
}

void window_set_bgcolor(uint8_t r, uint8_t g, uint8_t b) { (void)r, (void)g, (void)b; }

/* Headless renders at native resolution (no canvas->window scaling), so the
 * filter is genuinely a no-op here. */
void window_set_scale_filter(window_scale_filter_t filter) { (void)filter; }

void window_set_scale(double scale) { (void)scale; }

double window_get_scale(void) { return 0.0; }
