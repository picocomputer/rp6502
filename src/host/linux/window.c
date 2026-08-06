/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Linux window host: the X11 WM seam (resize + aspect hint) and the sokol entry
 * (window_run -> sapp_run). The render/frame/present pipeline is in
 * app/window_core.c.
 */

#include "emu/app/window.h"
#include "emu/app/window_core.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_log.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* The window tracks the canvas aspect two ways via the X11 handle sokol exposes:
 * a one-shot resize when the canvas changes, and a PAspect size hint that asks
 * the window manager to keep that aspect during interactive resizes (we don't
 * fight the WM by snapping the size ourselves; the quad letterboxes whenever the
 * window ends up off-aspect anyway). Forward-declare the few Xlib bits we need
 * rather than pulling in <X11/Xlib.h> and its macro soup (Window is an XID =
 * unsigned long; X11 is already linked for the GL backend). */
typedef struct _XDisplay Display;
typedef struct
{
    long flags;
    int x, y, width, height;
    int min_width, min_height, max_width, max_height;
    int width_inc, height_inc;
    struct { int x, y; } min_aspect, max_aspect;
    int base_width, base_height, win_gravity;
} XSizeHints;
#define X_PASPECT (1L << 7) /* XSizeHints PAspect flag */
extern int XResizeWindow(Display *, unsigned long, unsigned, unsigned);
extern void XSetWMNormalHints(Display *, unsigned long, XSizeHints *);
extern int XFlush(Display *);

void host_window_resize(int w, int h)
{
    Display *dpy = (Display *)sapp_x11_get_display();
    unsigned long win = (unsigned long)(uintptr_t)sapp_x11_get_window();
    if (dpy && win)
    {
        XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
        XFlush(dpy);
    }
}

/* Ask the WM to constrain interactive resizes to the canvas aspect (cw:ch). */
void host_window_set_aspect_hint(int cw, int ch)
{
    Display *dpy = (Display *)sapp_x11_get_display();
    unsigned long win = (unsigned long)(uintptr_t)sapp_x11_get_window();
    if (!dpy || !win)
        return;
    XSizeHints h;
    memset(&h, 0, sizeof h);
    h.flags = X_PASPECT;
    h.min_aspect.x = h.max_aspect.x = cw;
    h.min_aspect.y = h.max_aspect.y = ch;
    XSetWMNormalHints(dpy, win, &h);
    XFlush(dpy);
}

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

void host_window_files_dropped(void)
{
    if (window_core_boot_rom(sapp_get_dropped_file_path(0)))
        waiting_for_rom = false;
}

void host_window_open_url(const char *url)
{
    /* Fire-and-forget xdg-open; double-fork so the grandchild reparents to init
     * and leaves no zombie for us to reap. */
    pid_t pid = fork();
    if (pid == 0)
    {
        if (fork() == 0)
        {
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

int window_run(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
{
    int win_w, win_h;
    window_core_prepare(fb, scale, have_scale, vsync, exit_on_halt, &win_w, &win_h);
    sapp_run(&(sapp_desc){
        .init_cb = window_core_init,
        .frame_cb = window_core_frame,
        .event_cb = window_core_event,
        .cleanup_cb = window_core_cleanup,
        .width = win_w,
        .height = win_h,
        .swap_interval = vsync ? 1 : 0, /* off: present uncapped (driver may ignore) */
        .window_title = "Picocomputer 6502",
        .enable_dragndrop = true, /* drop a .rp6502 to boot it */
        .enable_clipboard = true, /* Ctrl+V types into the emulated keyboard */
        .clipboard_size = 65536,
        .logger.func = slog_func,
    });
    return window_core_exit_code();
}
