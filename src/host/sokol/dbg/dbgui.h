/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * C-callable wrappers around the C++ ImGui debugger overlay in dbgui.cc, so
 * the window layer can drive it without including ImGui or the chips UI
 * headers. None of it is compiled without EMU_WITH_DEBUGGER, and everything
 * but dbgui_set_config_file runs only while dbg_is_active().
 */

#ifndef _HOST_SOKOL_DBG_DBGUI_H_
#define _HOST_SOKOL_DBG_DBGUI_H_

#include <stdbool.h>
#include <stdint.h>

/* Where the debugger UI keeps its layout. The --ini value goes here before
 * dbgui_init; with no override the UI uses <os-config-dir>/dbgui.ini. */
void dbgui_set_config_file(const char *path);

/* The window size of the last debug session, read from the config file before
 * dbgui_init and the window exist. False when the file has no size or an
 * implausible one. */
bool dbgui_window_size(int *w, int *h);

void dbgui_init(void);    /* after sg_setup */
void dbgui_discard(void); /* before sg_shutdown */
void dbgui_new_frame(int width, int height, double delta_time, float dpi_scale);
void dbgui_draw(void);   /* between new_frame and render */
void dbgui_render(void); /* into the current sokol-gfx pass */

/* One CPU cycle of the chips ui_dbg view, so its disassembly, heatmap and
 * history stay current. This is the only entry on the CPU's per-cycle path,
 * and a ui_dbg breakpoint that traps here asks core/dap/dbg.c for a break, so
 * dbg.c stays the one authority on stopping. */
void dbgui_tick(uint64_t pins);

/* The height of the overlay's top menu bar in ImGui points, so the window
 * layer can lay the emulated canvas out below the bar rather than under it.
 * Valid after the first dbgui_draw, and 0 before it. */
float dbgui_menu_height(void);

/* An estimate of that height, for sizing the window before the first frame has
 * measured the real bar. */
float dbgui_menu_bar_estimate(void);

/* Offers a host input event (a const sapp_event *). True when ImGui consumed
 * it, and the window layer then keeps it from the emulated machine. */
bool dbgui_handle_event(const void *sapp_event_ptr);

/* The rect the emulated canvas should fill, in framebuffer pixels with x and y
 * from the top left: the dockspace central node, which shrinks as panels dock
 * beside it. False before the first dbgui_draw, and the caller then falls back
 * to the whole window. */
bool dbgui_canvas_rect(int *x, int *y, int *w, int *h);

/* True when the pointer is over a debugger panel or widget, so the caller
 * applies dbgui_mouse_cursor there. Valid only while the debugger is active. */
bool dbgui_wants_mouse(void);

/* The cursor ImGui wants this frame, as a sapp_mouse_cursor carried in an int
 * so this header need not include sokol. Valid only while the debugger is
 * active. */
int dbgui_mouse_cursor(void);

#endif /* _HOST_SOKOL_DBG_DBGUI_H_ */
