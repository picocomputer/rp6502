/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * C-callable wrappers around the C++ ImGui debugger overlay (dbgui.cc), so the
 * C window layer (app_sokol.c) can drive it without pulling in ImGui or the
 * chips UI headers. Only compiled/called when EMU_WITH_DEBUGGER and the emulator
 * is in debug mode (dbg_is_active()).
 */

#ifndef _EMU_DBG_DBGUI_H_
#define _EMU_DBG_DBGUI_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Set the config FILE the debugger UI persists its layout into. Pass the --ini
 * value here BEFORE dbgui_init; with no override the UI uses <os-config-dir>/dbgui.ini. */
void dbgui_set_config_file(const char *path);

/* The last debug session's window size from the config file, readable before
 * dbgui_init (and the window) exist. False if absent or implausible. */
bool dbgui_window_size(int *w, int *h);

void dbgui_init(void);    /* create ImGui + the debugger windows (after sg_setup) */
void dbgui_discard(void); /* tear them down (before sg_shutdown) */
void dbgui_new_frame(int width, int height, double delta_time, float dpi_scale);
void dbgui_draw(void);   /* build the windows (between new_frame and render) */
void dbgui_render(void); /* draw ImGui into the current sokol-gfx pass */
/* Per-CPU-cycle view update: feed the chips ui_dbg its tick so the disassembly
 * heatmap/history/current-PC stay current. Display-only — never gates the CPU
 * (dbg.c is the authoritative engine). */
void dbgui_tick(uint64_t pins);
/* Height (ImGui points) of the overlay's top menu bar, so the window layer can
 * reserve room for it and lay the emulated canvas out below the menu rather than
 * under it. Valid after the first dbgui_draw; 0 before. */
float dbgui_menu_height(void);
/* Estimated menu-bar height (ImGui points) for sizing the window BEFORE the
 * first frame measures the real bar (dbgui_menu_height is 0 until then). */
float dbgui_menu_bar_estimate(void);
/* Feed a host input event (const sapp_event *). Returns true if ImGui consumed
 * it (the window layer should then not forward it to the emulated machine). */
bool dbgui_handle_event(const void *sapp_event_ptr);
/* The framebuffer-pixel rect (x, y from top-left, w, h) the emulated canvas should
 * fill: the dockspace central node, which shrinks as panels dock beside it. False
 * before the first dbgui_draw; the caller then falls back to the whole window. */
bool dbgui_canvas_rect(int *x, int *y, int *w, int *h);
/* True when ImGui wants the mouse (it is over a debugger panel/widget), so the
 * caller should leave ImGui's cursor alone. Only valid while the debugger is active. */
bool dbgui_wants_mouse(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_DBG_DBGUI_H_ */
