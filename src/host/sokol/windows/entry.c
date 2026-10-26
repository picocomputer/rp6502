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

/* The window rectangle around a w by h client area: the client area with the
 * frame for this window's DPI added. */
static RECT window_rect(HWND hwnd, int w, int h)
{
    RECT r = {0, 0, w, h};
    AdjustWindowRectExForDpi(&r,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
                             GetDpiForWindow(hwnd));
    return r;
}

void host_window_resize(int w, int h)
{
    HWND hwnd = (HWND)sapp_win32_get_hwnd();
    /* A maximized window keeps the size Windows gave it, and a minimized one
     * has no client area to size. */
    if (!hwnd || IsZoomed(hwnd) || IsIconic(hwnd))
        return;
    /* w and h are client pixels, which are framebuffer pixels here, so the
     * frame for this window's DPI is added to them. SWP_NOMOVE keeps the
     * top-left corner where it is. */
    RECT r = window_rect(hwnd, w, h);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The frame aspect that a drag holds the client area at. It stays 0 by 0, which
 * holds nothing, while the debugger's overlay is up, because gfx never gives
 * one then. */
static int aspect_w, aspect_h;

void host_window_set_aspect(int w, int h, bool refit)
{
    aspect_w = w;
    aspect_h = h;
    RECT r;
    if (refit && GetClientRect((HWND)sapp_win32_get_hwnd(), &r))
        host_window_resize(r.right, (int)(((long)r.right * h + w / 2) / w));
}

/* sokol's window procedure, which gets every message aspect_wndproc does not
 * answer. */
static WNDPROC sokol_wndproc;

/* Win32 has no aspect hint, so WM_SIZING, which carries the window rectangle a
 * drag proposes, is answered with that rectangle reshaped to the aspect. The
 * edge being dragged sets one side of the client area and the other side
 * follows, and a corner is led by its width. The edges across from the one
 * being dragged stay where they are. */
static LRESULT CALLBACK aspect_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg != WM_SIZING || !aspect_w)
        return CallWindowProcW(sokol_wndproc, hwnd, msg, wp, lp);
    RECT *r = (RECT *)lp;
    /* What the window adds around its client area, measured on an empty one. */
    RECT nc = window_rect(hwnd, 0, 0);
    int nw = nc.right - nc.left, nh = nc.bottom - nc.top;
    int cw = r->right - r->left - nw, ch = r->bottom - r->top - nh;
    if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM)
        cw = (int)(((long)ch * aspect_w + aspect_h / 2) / aspect_h);
    else
        ch = (int)(((long)cw * aspect_h + aspect_w / 2) / aspect_w);
    if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT)
        r->left = r->right - (cw + nw);
    else
        r->right = r->left + (cw + nw);
    if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)
        r->top = r->bottom - (ch + nh);
    else
        r->bottom = r->top + (ch + nh);
    return TRUE;
}

static bool waiting_for_rom;

bool entry_wait_for_rom(void)
{
    waiting_for_rom = true;
    return true;
}

/* sokol shows the window but never asks for the foreground. Windows grants that
 * only to a process the foreground process started, or one that had the last
 * input, so a window started from a background process, as an editor starts
 * its debug adapter, opens behind the editor, and a plain SetForegroundWindow
 * is refused for the same reason. Joining the foreground window's input queue
 * for the call makes the request come from the thread that holds the
 * foreground, which Windows grants. */
static void take_foreground(HWND hwnd)
{
    HWND fg = GetForegroundWindow();
    if (fg == hwnd)
        return;
    DWORD self = GetCurrentThreadId();
    DWORD other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    bool attached = other && other != self && AttachThreadInput(self, other, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (attached)
        AttachThreadInput(self, other, FALSE);
}

void host_window_init(void)
{
    HWND hwnd = (HWND)sapp_win32_get_hwnd();
    /* sokol offers no hook into its window procedure, so the window is
     * subclassed, and aspect_wndproc sees each message before sokol does. */
    sokol_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)aspect_wndproc);
    take_foreground(hwnd);
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
