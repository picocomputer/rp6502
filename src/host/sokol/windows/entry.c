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
#include <shellapi.h> /* ShellExecuteA (WIN32_LEAN_AND_MEAN omits it) */

#include "core/str/oem.h"
#include "core/sys/debug_log.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

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

/* True when the path survives UTF-16 to OEM and back unchanged, which is what
 * app_boot_rom's conversion of its UTF-8 spelling has to do to it. */
static bool wide_is_oem_lossless(const WCHAR *w)
{
    /* oem_from_wide writes at most one byte per UTF-16 unit and oem_to_wide
     * one unit per byte, so one length serves both buffers. */
    size_t n = wcslen(w) + 1;
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
    /* sokol delivers the path as UTF-8 and app_boot_rom converts it to the
     * guest's OEM code page, so a path with characters that code page cannot
     * hold falls back to its 8.3 short name. */
    const char *utf8 = sapp_get_dropped_file_path(0);
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    WCHAR *wide = wn > 0 ? malloc((size_t)wn * sizeof *wide) : NULL;
    if (!wide || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wn))
    {
        free(wide);
        RP6502_LOG(emu, ERROR, "cannot take the dropped path");
        return;
    }
    if (wide_is_oem_lossless(wide))
    {
        free(wide);
        if (app_boot_rom(utf8))
            waiting_for_rom = false;
        return;
    }
    /* A short name can be longer than the long name it came from, so it is
     * measured on its own. GetShortPathNameW returns the size it needs when
     * the buffer is too small, rather than failing. */
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
        RP6502_LOG(emu, ERROR, "dropped path not representable in the OEM code page");
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
