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

#ifndef _EMU_DBGUI_H_
#define _EMU_DBGUI_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Set the config FILE the debugger UI persists its layout into (an [EMU] section;
 * other sections, e.g. a .rp6502's [RP6502], are preserved). Pass the --ini value
 * here BEFORE dbgui_init; with no override the UI uses <os-config-dir>/dbgui.ini. */
void dbgui_set_config_file(const char *path);

void dbgui_init(void);    /* create ImGui + the debugger windows (after sg_setup) */
void dbgui_discard(void); /* tear them down (before sg_shutdown) */
void dbgui_new_frame(int width, int height, double delta_time, float dpi_scale);
void dbgui_draw(void);   /* build the windows (between new_frame and render) */
void dbgui_render(void); /* draw ImGui into the current sokol-gfx pass */
/* Per-CPU-cycle view update: feed the chips ui_dbg its tick so the disassembly
 * heatmap/history/current-PC stay current. Registered as emu_dbg_cycle_cb by
 * dbgui_init and called from sys.c's tick loop; display-only (never gates the
 * CPU — dbg.c is the authoritative engine). */
void dbgui_tick(uint64_t pins);
/* Height (ImGui points) of the overlay's top menu bar, so the window layer can
 * reserve room for it and lay the emulated canvas out below the menu rather than
 * under it. Valid after the first dbgui_draw; 0 before. */
float dbgui_menu_height(void);
/* Feed a host input event (const sapp_event *). Returns true if ImGui consumed
 * it (the window layer should then not forward it to the emulated machine). */
bool dbgui_handle_event(const void *sapp_event_ptr);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_DBGUI_H_ */
