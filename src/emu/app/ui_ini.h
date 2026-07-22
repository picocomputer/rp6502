/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Dummy chips-ui elements (nothing drawn) behind the emulator's own entries in
 * the debugger's ini file:
 *   [RP6502][Launch]  key=value — rp6502.py's device block, round-tripped
 *                     verbatim by an ImGuiSettingsHandler (the emulator never
 *                     interprets it), placed so the block leads the file
 *   [Window][Manager] — the HOST window, riding ImGui's built-in geometry
 *                     handler as a window named "Manager" that is never drawn:
 *                     each save refreshes its Size from the live sokol window,
 *                     and the next session reads it back to reopen at that size
 *
 * Header-only with the implementation under CHIPS_UI_IMPL, emitted by the single
 * TU that defines it (dbgui.cc), matching the chips-ui convention. ImGui
 * (including imgui_internal.h, for the window-settings storage) is assumed
 * already included by that TU.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    int launch_len;
    char launch[2048]; /* [Launch] raw lines; a line that would not fit is dropped whole */
} ui_ini_t;

/* Add the settings handler at the FRONT of the current ImGui context's handler
 * list: [RP6502][Launch] then leads the written file, and the [Window][Manager]
 * Size refresh lands before ImGui's built-in [Window] handler writes it. */
void ui_ini_register(ui_ini_t *ini);
/* The [Window][Manager] size in the current ImGui context's settings (load the
 * ini first); false if absent or implausible. */
bool ui_ini_window_size(int *w, int *h);

#ifdef __cplusplus
}
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_UI_IMPL
#include <string.h>
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif
#include "sokol/sokol_app.h" /* the [Window][Manager] refresh reads the live window size */

#define _UI_INI_MANAGER "Manager" /* the dummy [Window] entry's name */

static ui_ini_t *_ui_ini_self(ImGuiSettingsHandler *handler)
{
    return (ui_ini_t *)handler->UserData;
}

static void _ui_ini_launch_clear(ui_ini_t *ini)
{
    ini->launch_len = 0;
    ini->launch[0] = 0;
}

static void _ui_ini_launch_line(ui_ini_t *ini, const char *line)
{
    size_t n = strlen(line);
    if (ini->launch_len + n + 2 > sizeof ini->launch) /* +'\n' +NUL */
        return;
    memcpy(ini->launch + ini->launch_len, line, n);
    ini->launch_len += (int)n;
    ini->launch[ini->launch_len++] = '\n';
    ini->launch[ini->launch_len] = 0;
}

static void _ui_ini_clear(ImGuiContext *, ImGuiSettingsHandler *handler)
{
    _ui_ini_launch_clear(_ui_ini_self(handler));
}

static void *_ui_ini_readopen(ImGuiContext *, ImGuiSettingsHandler *handler, const char *name)
{
    ui_ini_t *ini = _ui_ini_self(handler);
    if (strcmp(name, "Launch") != 0)
        return nullptr;
    _ui_ini_launch_clear(ini);
    return (void *)ini->launch;
}

static void _ui_ini_readline(ImGuiContext *, ImGuiSettingsHandler *handler, void *, const char *line)
{
    _ui_ini_launch_line(_ui_ini_self(handler), line);
}

static void _ui_ini_writeall(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf)
{
    ui_ini_t *ini = _ui_ini_self(handler);
    if (ini->launch_len) /* rp6502.py's block leads the file */
    {
        buf->appendf("[%s][Launch]\n", handler->TypeName);
        buf->append(ini->launch, ini->launch + ini->launch_len);
        buf->append("\n");
    }
    /* Refresh the dummy [Window][Manager] entry from the live window; ImGui's
     * built-in [Window] handler, which runs after this one, writes it out. */
    if (sapp_isvalid())
    {
        ImGuiWindowSettings *s = ImGui::FindWindowSettingsByID(ImHashStr(_UI_INI_MANAGER));
        if (!s)
            s = ImGui::CreateNewWindowSettings(_UI_INI_MANAGER);
        /* sapp_width/height are framebuffer (physical) px under high_dpi, but this
         * is restored into sapp_desc, which is logical — store logical so the
         * window doesn't grow by the DPI factor each session (dpi_scale is 1.0
         * where high_dpi is off). */
        float d = sapp_dpi_scale();
        s->Size = ImVec2ih((short)(sapp_width() / d + 0.5f), (short)(sapp_height() / d + 0.5f));
    }
}

void ui_ini_register(ui_ini_t *ini)
{
    CHIPS_ASSERT(ini);
    ImGuiSettingsHandler h;
    h.TypeName = "RP6502";
    h.TypeHash = ImHashStr("RP6502");
    h.ClearAllFn = _ui_ini_clear;
    h.ReadOpenFn = _ui_ini_readopen;
    h.ReadLineFn = _ui_ini_readline;
    h.WriteAllFn = _ui_ini_writeall;
    h.UserData = ini;
    ImGui::AddSettingsHandler(&h);
    /* AddSettingsHandler appends and the handler list is the write order; move
     * this one (just appended) to the front, ahead of the built-ins. */
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiSettingsHandler moved = g.SettingsHandlers.back();
    g.SettingsHandlers.pop_back();
    g.SettingsHandlers.insert(g.SettingsHandlers.begin(), moved);
}

bool ui_ini_window_size(int *w, int *h)
{
    CHIPS_ASSERT(w && h);
    const ImGuiWindowSettings *s = ImGui::FindWindowSettingsByID(ImHashStr(_UI_INI_MANAGER));
    if (!s)
        return false;
    int ww = s->Size.x, hh = s->Size.y;
    if (ww < 160 || hh < 120 || ww > 16384 || hh > 16384)
        return false;
    *w = ww;
    *h = hh;
    return true;
}
#endif /* CHIPS_UI_IMPL */
