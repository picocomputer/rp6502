/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Two entries in the debugger's ini file that draw nothing.
 *
 * [RP6502][PICO] is rp6502.py's settings for running on an RP6502-PICO, and
 * [RP6502][Launch] is its older name. An ImGuiSettingsHandler round-trips it
 * verbatim under the name it was read with, and the emulator never reads what
 * is in it.
 *
 * [Window][Manager] is the host window's own size, kept in ImGui's built-in
 * geometry handler as a window named "Manager" that is never drawn: each save
 * refreshes its Size from the live sokol window, and the next session reads it
 * back to reopen at that size.
 *
 * This header carries its implementation under CHIPS_UI_IMPL, which one
 * translation unit defines (dbgui.cc), following the chips-ui convention. That
 * unit must have included ImGui already, imgui_internal.h with it for the
 * window-settings storage, because this file includes neither.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    const char *name; /* "PICO" or "Launch", as read */
    int pico_len;
    char pico[2048]; /* raw [PICO] lines; a line that will not fit is dropped whole */
} ui_ini_t;

/* Adds the settings handler at the front of the current ImGui context's
 * handler list, because that list is the write order: [RP6502][PICO] then
 * leads the written file, and the [Window][Manager] size is refreshed before
 * ImGui's built-in [Window] handler writes it out. */
void ui_ini_register(ui_ini_t *ini);

/* The [Window][Manager] size in the current ImGui context's settings, so the
 * ini has to be loaded first. False when it is absent or implausible. */
bool ui_ini_window_size(int *w, int *h);

#ifdef __cplusplus
}
#endif

#ifdef CHIPS_UI_IMPL
#include <string.h>
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif
#include "sokol/sokol_app.h"

#define _UI_INI_MANAGER "Manager"

static ui_ini_t *_ui_ini_self(ImGuiSettingsHandler *handler)
{
    return (ui_ini_t *)handler->UserData;
}

static void _ui_ini_pico_clear(ui_ini_t *ini)
{
    ini->pico_len = 0;
    ini->pico[0] = 0;
}

static void _ui_ini_pico_line(ui_ini_t *ini, const char *line)
{
    size_t n = strlen(line);
    /* The 2 is the newline this appends and the terminator. */
    if (ini->pico_len + n + 2 > sizeof ini->pico)
        return;
    memcpy(ini->pico + ini->pico_len, line, n);
    ini->pico_len += (int)n;
    ini->pico[ini->pico_len++] = '\n';
    ini->pico[ini->pico_len] = 0;
}

static void _ui_ini_clear(ImGuiContext *, ImGuiSettingsHandler *handler)
{
    _ui_ini_pico_clear(_ui_ini_self(handler));
}

static void *_ui_ini_readopen(ImGuiContext *, ImGuiSettingsHandler *handler, const char *name)
{
    ui_ini_t *ini = _ui_ini_self(handler);
    if (strcmp(name, "PICO") == 0)
        ini->name = "PICO";
    else if (strcmp(name, "Launch") == 0)
        ini->name = "Launch";
    else
        return nullptr;
    _ui_ini_pico_clear(ini);
    return (void *)ini->pico;
}

static void _ui_ini_readline(ImGuiContext *, ImGuiSettingsHandler *handler, void *, const char *line)
{
    _ui_ini_pico_line(_ui_ini_self(handler), line);
}

static void _ui_ini_writeall(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf)
{
    ui_ini_t *ini = _ui_ini_self(handler);
    if (ini->pico_len)
    {
        buf->appendf("[%s][%s]\n", handler->TypeName, ini->name);
        buf->append(ini->pico, ini->pico + ini->pico_len);
        buf->append("\n");
    }
    if (sapp_isvalid())
    {
        ImGuiWindowSettings *s = ImGui::FindWindowSettingsByID(ImHashStr(_UI_INI_MANAGER));
        if (!s)
            s = ImGui::CreateNewWindowSettings(_UI_INI_MANAGER);
        /* sapp_width and sapp_height are framebuffer pixels under high_dpi,
         * while the size is restored into sapp_desc, which is logical, so the
         * logical size is what is stored. Storing the framebuffer size would
         * grow the window by the DPI factor every session. dpi_scale is 1.0
         * where high_dpi is off, except on X11, where sokol takes it from
         * Xft.dpi either way. */
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
    /* AddSettingsHandler appends, so the handler it just added is moved from
     * the back of the list to the front, ahead of the built-ins. */
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
