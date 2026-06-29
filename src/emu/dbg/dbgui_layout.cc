/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Layout persistence for the debugger overlay. Everything lives in ONE config
 * file: the --ini override (the launcher passes the workstation's choice, e.g.
 * the workspace .rp6502) if given, else <os-config-dir>/dbgui.ini. We own only an
 * [EMU] section there (per-window open flag + geometry as plain
 * `key = open,x,y,w,h`); ALL other sections are preserved untouched -- most
 * importantly the [RP6502] device/key block that the .rp6502 file carries for the
 * hardware tools. ImGui's own ini writer is NOT used (it would clobber foreign
 * sections), so the file stays a clean, parseable INI.
 *
 * This module is pure file/INI handling plus the core-ImGui geometry calls
 * (LoadIniSettingsFromMemory / FindWindowByName); it has no chips-UI dependency.
 * dbgui.cc passes a flat {key, title, open*} table, so the overlay's window
 * objects stay on its side.
 */

#include "imgui.h"
#include "imgui_internal.h" /* FindWindowByName / ImGuiWindow */

#include "emu/dbg/dbgui.h"        /* dbgui_set_config_file (the public C entry point) */
#include "emu/dbg/dbgui_layout.h" /* dbgui_win_t + load/save */

#include <cstdio>
#include <cstdlib> /* getenv */
#include <cstring> /* strchr, strcmp, memcpy */
#if defined(_WIN32)
#include <direct.h> /* _mkdir */
#define DBGUI_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h> /* mkdir */
#define DBGUI_MKDIR(p) mkdir(p, 0755)
#endif

#define DBGUI_LAYOUT_MAX 16 /* a handful of overlay windows; clamp defensively */

static char g_cfg_override[1024]; /* --ini <file>, or "" for the OS default file */
static struct { int x, y, w, h; bool valid; } g_geo[DBGUI_LAYOUT_MAX]; /* last-known geometry */

void dbgui_set_config_file(const char *path)
{
    std::snprintf(g_cfg_override, sizeof g_cfg_override, "%s", path ? path : "");
}

/* Create `path` and any missing parent directories. */
static void dbgui_mkdir_p(const char *path)
{
    char tmp[1024];
    std::snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/'
#if defined(_WIN32)
            || *p == '\\'
#endif
        )
        {
            char c = *p;
            *p = 0;
            DBGUI_MKDIR(tmp);
            *p = c;
        }
    }
    DBGUI_MKDIR(tmp);
}

/* The config file path (override, else <os-config-dir>/dbgui.ini); its parent
 * directory is created. Returns false if there is nowhere to write it. */
static bool dbgui_config_path(char *out, size_t cap)
{
    if (g_cfg_override[0])
        std::snprintf(out, cap, "%s", g_cfg_override);
    else
    {
        char dir[896];
#if defined(_WIN32)
        const char *base = getenv("APPDATA");
        if (!base || !base[0])
            return false;
        std::snprintf(dir, sizeof dir, "%s\\rp6502-emu", base);
#else
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0])
            std::snprintf(dir, sizeof dir, "%s/rp6502-emu", xdg);
        else
        {
            const char *home = getenv("HOME");
            if (!home || !home[0])
                return false;
            std::snprintf(dir, sizeof dir, "%s/.config/rp6502-emu", home);
        }
#endif
        std::snprintf(out, cap, "%s/dbgui.ini", dir);
    }
    /* make the parent dir */
    char dir[1024];
    std::snprintf(dir, sizeof dir, "%s", out);
    char *slash = std::strrchr(dir, '/');
#if defined(_WIN32)
    char *bslash = std::strrchr(dir, '\\');
    if (bslash > slash)
        slash = bslash;
#endif
    if (slash && slash != dir)
    {
        *slash = 0;
        dbgui_mkdir_p(dir);
    }
    return true;
}

/* Read a whole file into buf (NUL-terminated). Returns length, or -1. */
static long dbgui_read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = std::fread(buf, 1, cap - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return (long)n;
}

/* Tiny INI getter: value of `key` within section [sect]. Returns false if absent. */
static bool dbgui_ini_get(const char *text, const char *sect, const char *key,
                          char *out, size_t cap)
{
    char cur[64] = "";
    const char *p = text;
    while (*p)
    {
        const char *eol = std::strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : std::strlen(p);
        char line[1024];
        if (len >= sizeof line)
            len = sizeof line - 1;
        std::memcpy(line, p, len);
        line[len] = 0;
        char *e = line + std::strlen(line);
        while (e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '[')
        {
            char *rb = std::strchr(s, ']');
            if (rb)
            {
                *rb = 0;
                std::snprintf(cur, sizeof cur, "%s", s + 1);
            }
        }
        else if (*s && *s != ';' && *s != '#' && std::strcmp(cur, sect) == 0)
        {
            char *eq = std::strchr(s, '=');
            if (eq)
            {
                char *ke = eq;
                *eq = 0;
                while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t'))
                    ke--;
                *ke = 0;
                if (std::strcmp(s, key) == 0)
                {
                    char *v = eq + 1;
                    while (*v == ' ' || *v == '\t')
                        v++;
                    std::snprintf(out, cap, "%s", v);
                    return true;
                }
            }
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    return false;
}

void dbgui_layout_load(const dbgui_win_t *wins, int n)
{
    if (n > DBGUI_LAYOUT_MAX)
        n = DBGUI_LAYOUT_MAX;
    char path[1024];
    if (!dbgui_config_path(path, sizeof path))
        return;
    char buf[8192];
    if (dbgui_read_file(path, buf, sizeof buf) < 0)
        return;
    char imgui_ini[2048];
    int ii = 0;
    for (int i = 0; i < n; i++)
    {
        char val[64];
        if (!dbgui_ini_get(buf, "EMU", wins[i].key, val, sizeof val))
            continue;
        int open = 1, x = 0, y = 0, w = 0, h = 0;
        int got = std::sscanf(val, "%d,%d,%d,%d,%d", &open, &x, &y, &w, &h);
        if (got >= 1 && wins[i].open)
            *wins[i].open = (open != 0);
        if (got == 5 && w > 0 && h > 0)
        {
            g_geo[i] = {x, y, w, h, true};
            ii += std::snprintf(imgui_ini + ii, sizeof imgui_ini - (size_t)ii,
                                "[Window][%s]\nPos=%d,%d\nSize=%d,%d\n\n",
                                wins[i].title, x, y, w, h);
        }
    }
    if (ii > 0)
        ImGui::LoadIniSettingsFromMemory(imgui_ini, (size_t)ii);
}

void dbgui_layout_save(const dbgui_win_t *wins, int n)
{
    if (n > DBGUI_LAYOUT_MAX)
        n = DBGUI_LAYOUT_MAX;
    char path[1024];
    if (!dbgui_config_path(path, sizeof path))
        return;

    for (int i = 0; i < n; i++)
    {
        ImGuiWindow *w = ImGui::FindWindowByName(wins[i].title);
        if (w)
            g_geo[i] = {(int)w->Pos.x, (int)w->Pos.y, (int)w->Size.x, (int)w->Size.y, true};
    }

    char emu[1024];
    int en = std::snprintf(emu, sizeof emu, "[EMU]\n");
    for (int i = 0; i < n; i++)
    {
        int open = (wins[i].open && *wins[i].open) ? 1 : 0;
        if (g_geo[i].valid)
            en += std::snprintf(emu + en, sizeof emu - (size_t)en, "%s = %d,%d,%d,%d,%d\n",
                                wins[i].key, open, g_geo[i].x, g_geo[i].y, g_geo[i].w, g_geo[i].h);
        else
            en += std::snprintf(emu + en, sizeof emu - (size_t)en, "%s = %d\n",
                                wins[i].key, open);
    }

    char in[8192];
    if (dbgui_read_file(path, in, sizeof in) < 0)
        in[0] = 0;

    char out[16384];
    int on = 0;
    const char *p = in;
    bool in_emu = false;
    while (*p) /* copy through every section except the old [EMU] */
    {
        const char *eol = std::strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p + 1) : std::strlen(p);
        const char *t = p;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[')
            in_emu = (std::strncmp(t, "[EMU]", 5) == 0);
        if (!in_emu && on + (int)len < (int)sizeof out)
        {
            std::memcpy(out + on, p, len);
            on += (int)len;
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    while (on > 0 && (out[on - 1] == '\n' || out[on - 1] == '\r' ||
                      out[on - 1] == ' ' || out[on - 1] == '\t'))
        on--;
    if (on > 0)
    {
        out[on++] = '\n';
        out[on++] = '\n';
    }
    if (on + en < (int)sizeof out)
    {
        std::memcpy(out + on, emu, (size_t)en);
        on += en;
    }

    FILE *f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fwrite(out, 1, (size_t)on, f);
    std::fclose(f);
}
