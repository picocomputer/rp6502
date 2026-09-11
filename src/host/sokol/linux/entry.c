/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* The few Xlib declarations this file needs are written out here instead of
 * including <X11/Xlib.h>, so XSizeHints below has to match Xlib's own layout.
 * X11 is already linked for the GL backend, and a Window is an XID, which is
 * an unsigned long. */
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
#define X_PASPECT (1L << 7) /* PAspect, from <X11/Xutil.h> */
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

void host_window_open_url(const char *url)
{
    /* The child forks again and exits at once, so the grandchild that runs
     * xdg-open is reparented to init and leaves no zombie to reap. */
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

int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt)
{
    int win_w, win_h;
    app_prepare(fb, scale, have_scale, exit_on_halt, &win_w, &win_h);
    sapp_run(&(sapp_desc){
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = win_w,
        .height = win_h,
        .swap_interval = 1,
        .window_title = "Picocomputer 6502",
        .enable_dragndrop = true,
        .enable_clipboard = true,
        .clipboard_size = 65536,
        .logger.func = app_log,
    });
    return app_exit_code();
}
