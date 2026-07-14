/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Win32 window glue for the sokol app layer (window.c's resize_window on
 * Windows). Kept in its own translation unit so <windows.h> and its macros
 * (near/far/min/max) stay out of window.c.
 */

#include <windows.h>

/* From sokol_app (SOKOL_D3D11 build): the native window handle. Forward-declared
 * like window.c does for the X11 handles, so this file needs no sokol header. */
extern const void *sapp_win32_get_hwnd(void);

/* Resize the window so its CLIENT area is w x h framebuffer(== physical) pixels,
 * keeping the top-left corner. sokol owns the HWND; we only nudge its size. */
void window_win32_resize(int w, int h)
{
    HWND hwnd = (HWND)sapp_win32_get_hwnd();
    if (!hwnd)
        return;
    RECT r = {0, 0, w, h};
    AdjustWindowRectExForDpi(&r,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
                             GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
