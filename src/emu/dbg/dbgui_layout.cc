/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Debugger overlay layout persistence. The config file is owned by ImGui's
 * settings system: this module is only the file-path resolution (--ini override,
 * else <os-config-dir>/dbgui.ini) plus read-file -> LoadIniSettingsFromMemory and
 * SaveIniSettingsToMemory -> write-file. Window geometry rides in ImGui's built-in
 * [Window] handler; the window open-flags ([Chips]) and the [RP6502][Launch]
 * device block ride in custom ImGuiSettingsHandlers registered in dbgui.cc. No
 * chips-UI dependency here.
 */

#include "imgui.h"

#include "emu/dbg/dbgui.h"        /* dbgui_set_config_file (the public C entry point) */
#include "emu/dbg/dbgui_layout.h" /* load/save */

#include <cstdio>
#include <cstdlib> /* getenv */
#include <cstring> /* strrchr */
#if defined(_WIN32)
#include <direct.h> /* _mkdir */
#define DBGUI_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h> /* mkdir */
#define DBGUI_MKDIR(p) mkdir(p, 0755)
#endif

static char g_cfg_override[1024]; /* --ini <file>, or "" for the OS default file */

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

/* Read a file into buf (NUL-terminated, up to cap-1 bytes), or -1 if absent. */
static long dbgui_read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return -1;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    size_t want = (sz < 0) ? cap - 1 : (size_t)sz;
    size_t lim = want < cap - 1 ? want : cap - 1;
    size_t n = std::fread(buf, 1, lim, f);
    std::fclose(f);
    buf[n] = 0;
    return (long)n;
}

void dbgui_layout_load(void)
{
    char path[1024];
    if (!dbgui_config_path(path, sizeof path))
        return;
    char buf[16384];
    long n = dbgui_read_file(path, buf, sizeof buf);
    if (n < 0)
        return; /* no file yet: windows keep their compile-time defaults */
    ImGui::LoadIniSettingsFromMemory(buf, (size_t)n);
}

void dbgui_layout_save(void)
{
    char path[1024];
    if (!dbgui_config_path(path, sizeof path))
        return;
    size_t n = 0;
    const char *ini = ImGui::SaveIniSettingsToMemory(&n);
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fwrite(ini, 1, n, f);
    std::fclose(f);
}
