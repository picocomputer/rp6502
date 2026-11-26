/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/vterm_mouse.h"

/* The mouse is the RIA's: its terminal follows this one, and it answers
 * what a program asks of the mouse. Here the asking does nothing. */

void vterm_mouse_track(int func, int col, int row, int first, int last)
{
    (void)func, (void)col, (void)row, (void)first, (void)last;
}

void vterm_mouse_locate(void) {}

void vterm_mouse_filter(void) {}

/* The Pico has no window to lose the focus. */
bool vterm_focused(void)
{
    return true;
}
