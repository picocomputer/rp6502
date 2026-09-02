/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows window host: the Win32 WM resize seam and the sokol entry (entry_run
 * -> sapp_run with high_dpi for a native-resolution D3D11 backbuffer). The
 * render/frame/present pipeline is in host/sokol/app/app.c.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> /* ShellExecuteA (WIN32_LEAN_AND_MEAN omits it) */

#include "core/str/oem.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

void host_window_resize(int w, int h)
{
    HWND hwnd = (HWND)sapp_win32_get_hwnd();
    if (!hwnd)
        return;
    /* w,h are client (== framebuffer/physical) px; grow by this window's DPI
     * frame and keep the top-left corner. */
    RECT r = {0, 0, w, h};
    AdjustWindowRectExForDpi(&r,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
                             GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }

/* Held with no program until a .rp6502 is dropped: the core freezes the machine
 * and draws the "drop a ROM" prompt instead of the canvas while this is set. */
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

/* True when the wide path survives UTF-16 -> OEM -> UTF-16 unchanged, i.e.
 * app_boot_rom's OEM conversion of its UTF-8 spelling is lossless. */
static bool wide_is_oem_lossless(const WCHAR *w)
{
    size_t n = wcslen(w) + 1; /* one OEM byte per unit, and one unit back */
    char *oem = malloc(n);
    uint16_t *back = malloc(n * sizeof *back);
    bool same = false;
    if (oem && back)
    {
        oem_from_wide((const uint16_t *)w, oem, n);
        oem_to_wide(oem, back, (int)n);
        same = wcscmp(w, (const WCHAR *)back) == 0;
    }
    free(oem), free(back);
    return same;
}

void host_window_files_dropped(void)
{
    /* sokol delivers the path as UTF-8 and app_boot_rom converts it to
     * the guest's OEM code page; fall back to the 8.3 short name when the path
     * has characters the active OEM code page can't hold. */
    const char *utf8 = sapp_get_dropped_file_path(0);
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0); /* asks its own size */
    WCHAR *wide = wn > 0 ? malloc((size_t)wn * sizeof *wide) : NULL;
    if (!wide || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wn))
    {
        free(wide);
        fprintf(stderr, "rp6502-emu: cannot take the dropped path\n");
        return;
    }
    if (wide_is_oem_lossless(wide))
    {
        free(wide);
        if (app_boot_rom(utf8))
            waiting_for_rom = false;
        return;
    }
    /* A short name can be LONGER than the long name, so it is sized on its
     * own; a too-small buffer returns the needed size, not 0. */
    DWORD sn = GetShortPathNameW(wide, NULL, 0);
    WCHAR *shortw = sn ? malloc((size_t)sn * sizeof *shortw) : NULL;
    DWORD got = shortw ? GetShortPathNameW(wide, shortw, sn) : 0;
    free(wide);
    char *shortu8 = NULL;
    if (got && got < sn && wide_is_oem_lossless(shortw))
    {
        int un = WideCharToMultiByte(CP_UTF8, 0, shortw, -1, NULL, 0, NULL, NULL);
        shortu8 = un > 0 ? malloc((size_t)un) : NULL;
        if (shortu8 && !WideCharToMultiByte(CP_UTF8, 0, shortw, -1, shortu8, un, NULL, NULL))
        {
            free(shortu8);
            shortu8 = NULL;
        }
    }
    free(shortw);
    if (!shortu8)
    {
        fprintf(stderr, "rp6502-emu: dropped path not representable in the OEM code page\n");
        return;
    }
    bool booted = app_boot_rom(shortu8);
    free(shortu8);
    if (booted)
        waiting_for_rom = false;
}

void host_window_open_url(const char *url)
{
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt)
{
    int win_w, win_h;
    app_prepare(fb, scale, have_scale, exit_on_halt, &win_w, &win_h);
    /* D3D11 leaves the backbuffer at LOGICAL size unless high_dpi is requested,
     * so a DPI-scaled display DWM-stretches (smears) the menu/canvas; ask for a
     * native-resolution backbuffer. */
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
        .enable_dragndrop = true, /* drop a .rp6502 to boot it */
        .enable_clipboard = true, /* Ctrl+V types into the emulated keyboard */
        .clipboard_size = 65536,
        .logger.func = slog_func,
    });
    return app_exit_code();
}
