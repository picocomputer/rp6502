/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> /* ShellExecuteA, CommandLineToArgvW (WIN32_LEAN_AND_MEAN omits them) */

#include "host/sokol/app/entry.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

void host_window_resize(int w, int h)
{
    HWND hwnd = (HWND)sapp_win32_get_hwnd();
    if (!hwnd)
        return;
    /* w and h are client pixels, which are framebuffer pixels here, so the
     * frame for this window's DPI is added to them. SWP_NOMOVE keeps the
     * top-left corner where it is. */
    RECT r = {0, 0, w, h};
    AdjustWindowRectExForDpi(&r,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
                             GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }

static bool waiting_for_rom;

bool entry_wait_for_rom(void)
{
    waiting_for_rom = true;
    return true;
}

void host_window_init(void)
{
    if (waiting_for_rom)
        prompt_setup();
}

bool host_window_menu_active(void) { return waiting_for_rom; }

void host_window_menu_draw(void)
{
    if (waiting_for_rom)
        prompt_draw("Drop a .rp6502", "ROM file here");
}

void host_window_files_dropped(void)
{
    if (app_boot_rom(sapp_get_dropped_file_path(0)))
        waiting_for_rom = false;
}

char **entry_argv_utf8(int *argc)
{
    int n;
    WCHAR **wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wide)
        return NULL;
    /* One block: the pointer table, then the strings it points at. */
    size_t size = ((size_t)n + 1) * sizeof(char *);
    for (int i = 0; i < n; i++)
        size += (size_t)WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
    char **argv = malloc(size);
    if (argv)
    {
        char *at = (char *)(argv + n + 1);
        char *end = (char *)argv + size;
        for (int i = 0; i < n; i++)
        {
            argv[i] = at;
            at += WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, at, (int)(end - at), NULL, NULL);
        }
        argv[n] = NULL;
        *argc = n;
    }
    LocalFree(wide);
    return argv;
}

void host_window_open_url(const char *url)
{
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt)
{
    int win_w, win_h;
    app_prepare(fb, scale, have_scale, exit_on_halt, &win_w, &win_h);
    /* Without high_dpi the backbuffer stays at the logical size, and on a
     * DPI-scaled display the desktop compositor stretches it, which smears the
     * canvas. high_dpi asks for a backbuffer at the native resolution. */
    sapp_run(&(sapp_desc){
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = win_w,
        .height = win_h,
        .high_dpi = true,
        .swap_interval = 1,
        .window_title = "Picocomputer 6502",
        .enable_dragndrop = true,
        .enable_clipboard = true,
        .clipboard_size = 65536,
        .logger.func = app_log,
    });
    return app_exit_code();
}
