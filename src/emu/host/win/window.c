/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows window host: the Win32 WM resize seam and the sokol entry (window_run
 * -> sapp_run with high_dpi for a native-resolution D3D11 backbuffer). The
 * render/frame/present pipeline is in app/window_core.c.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h> /* ShellExecuteA (WIN32_LEAN_AND_MEAN omits it) */

#include "ria/api/oem.h"
#include "emu/app/window.h"
#include "emu/app/window_core.h"
#include "sokol_app.h"
#include "sokol_log.h"
#include <stdint.h>
#include <stdio.h>

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

bool window_wait_for_rom(void)
{
    waiting_for_rom = true;
    return true;
}

void host_window_init(void)
{
    if (waiting_for_rom)
        window_core_prompt_setup();
}

bool host_window_menu_active(void) { return waiting_for_rom; }

void host_window_menu_draw(void)
{
    if (waiting_for_rom)
        window_core_draw_prompt("Drop a .rp6502", "ROM file here");
}

/* True when the wide path survives UTF-16 -> OEM -> UTF-16 unchanged, i.e.
 * window_core_boot_rom's OEM conversion of its UTF-8 spelling is lossless. */
static bool wide_is_oem_lossless(const WCHAR *w)
{
    char oem[MAX_PATH];
    uint16_t back[MAX_PATH];
    oem_from_wide((const uint16_t *)w, oem, sizeof oem);
    oem_to_wide(oem, back, MAX_PATH);
    return wcscmp(w, (const WCHAR *)back) == 0;
}

void host_window_files_dropped(void)
{
    /* sokol delivers the path as UTF-8 and window_core_boot_rom converts it to
     * the guest's OEM code page; fall back to the 8.3 short name when the path
     * has characters the active OEM code page can't hold. */
    const char *utf8 = sapp_get_dropped_file_path(0);
    WCHAR wide[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_PATH))
    {
        fprintf(stderr, "rp6502-emu: dropped path too long\n");
        return;
    }
    if (wide_is_oem_lossless(wide))
    {
        if (window_core_boot_rom(utf8))
            waiting_for_rom = false;
        return;
    }
    /* In place is allowed; a short name can be LONGER than the long name,
     * and a too-small buffer returns the needed size, not 0. */
    DWORD n = GetShortPathNameW(wide, wide, MAX_PATH);
    char shortu8[MAX_PATH * 3];
    if (!n || n >= MAX_PATH || !wide_is_oem_lossless(wide) ||
        !WideCharToMultiByte(CP_UTF8, 0, wide, -1, shortu8, sizeof shortu8, NULL, NULL))
    {
        fprintf(stderr, "rp6502-emu: dropped path not representable in the OEM code page\n");
        return;
    }
    if (window_core_boot_rom(shortu8))
        waiting_for_rom = false;
}

void host_window_open_url(const char *url)
{
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

int window_run(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
{
    int win_w, win_h;
    window_core_prepare(fb, scale, have_scale, vsync, exit_on_halt, &win_w, &win_h);
    /* D3D11 leaves the backbuffer at LOGICAL size unless high_dpi is requested,
     * so a DPI-scaled display DWM-stretches (smears) the menu/canvas; ask for a
     * native-resolution backbuffer. */
    sapp_run(&(sapp_desc){
        .init_cb = window_core_init,
        .frame_cb = window_core_frame,
        .event_cb = window_core_event,
        .cleanup_cb = window_core_cleanup,
        .width = win_w,
        .height = win_h,
        .high_dpi = true,
        .swap_interval = vsync ? 1 : 0, /* off: present uncapped (driver may ignore) */
        .window_title = "Picocomputer 6502",
        .enable_dragndrop = true, /* drop a .rp6502 to boot it */
        .enable_clipboard = true, /* Ctrl+V types into the emulated keyboard */
        .clipboard_size = 65536,
        .logger.func = slog_func,
    });
    return 0;
}
