/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Debugger overlay layout persistence (dbgui_layout.cc): the config file is owned
 * by ImGui's settings system. This module is just the file path resolution plus
 * the read-file -> LoadIniSettingsFromMemory / SaveIniSettingsToMemory -> write-file
 * plumbing. The window open-flags + the [RP6502][Launch] block ride inside that
 * ini via custom ImGuiSettingsHandlers registered in dbgui.cc; geometry rides in
 * ImGui's built-in [Window] handler. No chips-UI dependency here.
 */

#ifndef _EMU_DBGUI_LAYOUT_H_
#define _EMU_DBGUI_LAYOUT_H_

#ifdef __cplusplus
extern "C"
{
#endif

/* Load the config file into ImGui's settings (geometry + the custom handlers).
 * No file on first run -> windows keep their compile-time defaults. */
void dbgui_layout_load(void);

/* Serialize ImGui's settings (geometry + custom handlers) to the config file. */
void dbgui_layout_save(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_DBGUI_LAYOUT_H_ */
