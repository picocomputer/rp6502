/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Debugger overlay layout persistence (dbgui_layout.cc): read/write the [EMU]
 * section of the config file (each window's open flag + geometry), preserving
 * every other section (e.g. the .rp6502's [RP6502] block). Pure file/INI +
 * core-ImGui geometry plumbing; it never touches the chips UI types — dbgui.cc
 * hands it a flat table of {key, title, open-flag pointer}.
 */

#ifndef _EMU_DBGUI_LAYOUT_H_
#define _EMU_DBGUI_LAYOUT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* One persistable window: its [EMU] key, the ImGui window title (must match the
 * title the window draws under), and a pointer to its open flag (which may live
 * inside a chips UI struct, so the caller owns it). */
typedef struct
{
    const char *key;
    const char *title;
    bool *open;
} dbgui_win_t;

/* Restore open flags + geometry from the [EMU] section. Positions are fed to
 * ImGui as an in-memory ini so they override the windows' compile-time defaults.
 * On the first run there is no file, so the windows keep their defaults. */
void dbgui_layout_load(const dbgui_win_t *wins, int n);

/* Write the [EMU] section (open flags + live geometry), preserving every other
 * section in the file. */
void dbgui_layout_save(const dbgui_win_t *wins, int n);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_DBGUI_LAYOUT_H_ */
