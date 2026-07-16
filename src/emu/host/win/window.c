/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows window host: the Win32 WM resize seam and the sokol entry (window_run
 * -> sapp_run with high_dpi for a native-resolution D3D11 backbuffer). The
 * render/frame/present pipeline is in host/window_core.c.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "emu/host/window.h"
#include "emu/host/window_core.h"
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
void host_window_init(void) {}
bool host_window_menu_active(void) { return false; }
void host_window_menu_draw(void) {}

void host_window_files_dropped(void)
{
    /* sokol delivers the path as UTF-8 but rom_load opens with the ANSI CRT
     * fopen; convert, falling back to the 8.3 short name when the path has
     * characters the ANSI code page can't hold. When the process code page IS
     * UTF-8 (app manifest or the Windows setting), fopen takes it as-is — and
     * WideCharToMultiByte rejects WC_NO_BEST_FIT_CHARS/lpUsedDefaultChar there. */
    const char *utf8 = sapp_get_dropped_file_path(0);
    if (GetACP() == CP_UTF8)
    {
        window_core_boot_rom(utf8);
        return;
    }
    WCHAR wide[MAX_PATH];
    char ansi[MAX_PATH];
    BOOL lossy = FALSE;
    bool ok = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_PATH) != 0;
    if (ok &&
        (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1,
                              ansi, sizeof ansi, NULL, &lossy) ||
         lossy))
    {
        /* In place is allowed; a short name can be LONGER than the long name,
         * and a too-small buffer returns the needed size, not 0. */
        DWORD n = GetShortPathNameW(wide, wide, MAX_PATH);
        lossy = FALSE;
        ok = n > 0 && n < MAX_PATH &&
             WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1,
                                 ansi, sizeof ansi, NULL, &lossy) &&
             !lossy;
    }
    if (!ok)
    {
        fprintf(stderr, "rp6502-emu: dropped path not representable in the ANSI code page\n");
        return;
    }
    window_core_boot_rom(ansi);
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
