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

extern "C"
{
#include "host/sokol/dbg/dbgui.h"        /* dbgui_set_config_file (the public C entry point) */
#include "host/sokol/dbg/dbgui_layout.h" /* load/save */
#include "osal/os.h"                /* os_config_dir, os_ensure_parent_dir */
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

static char *g_cfg_override; /* --ini <file>, or null for the OS default file */

void dbgui_set_config_file(const char *path)
{
    std::free(g_cfg_override);
    g_cfg_override = (path && *path) ? strdup(path) : nullptr;
}

/* The config file path (override, else <os-config-dir>/dbgui.ini); its parent
 * directory is created. Null if there is nowhere to write it. Caller frees. */
static char *dbgui_config_path(void)
{
    char *path;
    if (g_cfg_override)
        path = strdup(g_cfg_override);
    else
    {
        char *dir = os_config_dir();
        if (!dir)
            return nullptr;
        static const char tail[] = "/dbgui.ini";
        path = (char *)std::malloc(std::strlen(dir) + sizeof tail);
        if (path)
            std::sprintf(path, "%s%s", dir, tail);
        std::free(dir);
    }
    if (path)
        os_ensure_parent_dir(path);
    return path;
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
    char *path = dbgui_config_path();
    if (!path)
        return;
    static char buf[16384]; /* the ini's contents, not a path */
    long n = dbgui_read_file(path, buf, sizeof buf);
    std::free(path);
    if (n < 0)
        return; /* no file yet: windows keep their compile-time defaults */
    ImGui::LoadIniSettingsFromMemory(buf, (size_t)n);
}

void dbgui_layout_save(void)
{
    char *path = dbgui_config_path();
    if (!path)
        return;
    size_t n = 0;
    const char *ini = ImGui::SaveIniSettingsToMemory(&n);
    FILE *f = std::fopen(path, "wb");
    std::free(path);
    if (!f)
        return;
    std::fwrite(ini, 1, n, f);
    std::fclose(f);
}
