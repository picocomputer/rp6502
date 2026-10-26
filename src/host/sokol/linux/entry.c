/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/entry.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* The few Xlib declarations this file needs are written out here instead of
 * including <X11/Xlib.h>, so XSizeHints below has to match Xlib's own layout.
 * X11 is already linked for the GL backend, and a Window and an Atom are both
 * XIDs, which are unsigned longs. */
typedef struct _XDisplay Display;
typedef unsigned long Atom;
typedef struct
{
    long flags;
    int x, y, width, height;
    int min_width, min_height, max_width, max_height;
    int width_inc, height_inc;
    struct { int x, y; } min_aspect, max_aspect;
    int base_width, base_height, win_gravity;
} XSizeHints;
#define X_PASPECT (1L << 7)       /* PAspect, from <X11/Xutil.h> */
#define X_PWINGRAVITY (1L << 9)   /* PWinGravity, from <X11/Xutil.h> */
#define X_NORTHWESTGRAVITY 1      /* NorthWestGravity, from <X11/X.h> */
#define X_XA_ATOM ((Atom)4)       /* XA_ATOM, from <X11/Xatom.h> */
extern int XResizeWindow(Display *, unsigned long, unsigned, unsigned);
extern void XSetWMNormalHints(Display *, unsigned long, XSizeHints *);
extern int XInternAtoms(Display *, char **, int, int, Atom *);
extern int XGetWindowProperty(Display *, unsigned long, Atom, long, long, int, Atom,
                              Atom *, int *, unsigned long *, unsigned long *,
                              unsigned char **);
extern int XFree(void *);
extern int XFlush(Display *);

/* _NET_WM_STATE lists what the window manager has done to the window. One
 * maximized either way, which is also how a window tiled to half the screen
 * shows, or made fullscreen has the size the window manager gave it, and a
 * resize would fight that. */
static bool wm_sized(Display *dpy, unsigned long win)
{
    static char *names[] = {"_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT",
                            "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_FULLSCREEN"};
    Atom atoms[4];
    XInternAtoms(dpy, names, 4, 0, atoms);
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    bool sized = false;
    /* A property of format 32 comes back as an array of longs. */
    if (XGetWindowProperty(dpy, win, atoms[0], 0, 64, 0, X_XA_ATOM, &type, &format,
                           &n, &after, &data) == 0 &&
        format == 32)
        for (unsigned long i = 0; i < n; i++)
            for (int k = 1; k < 4; k++)
                if (((Atom *)data)[i] == atoms[k])
                    sized = true;
    if (data)
        XFree(data);
    return sized;
}

void host_window_resize(int w, int h)
{
    Display *dpy = (Display *)sapp_x11_get_display();
    unsigned long win = (unsigned long)(uintptr_t)sapp_x11_get_window();
    if (dpy && win && !wm_sized(dpy, win))
    {
        XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
        XFlush(dpy);
    }
}

/* X11 window pixels are framebuffer pixels, so the width sokol reports is the
 * one the re-fit keeps. XSetWMNormalHints replaces every hint, which drops the
 * CenterGravity sokol opened the window with. NorthWest, which the ICCCM would
 * also assume, is named here because a window manager that honors gravity
 * keeps that corner still through the resize, so the window grows and shrinks
 * from its top-left corner rather than moving. */
void host_window_set_aspect(int w, int h, bool refit)
{
    Display *dpy = (Display *)sapp_x11_get_display();
    unsigned long win = (unsigned long)(uintptr_t)sapp_x11_get_window();
    if (!dpy || !win)
        return;
    XSizeHints hints;
    memset(&hints, 0, sizeof hints);
    hints.flags = X_PASPECT | X_PWINGRAVITY;
    hints.min_aspect.x = hints.max_aspect.x = w;
    hints.min_aspect.y = hints.max_aspect.y = h;
    hints.win_gravity = X_NORTHWESTGRAVITY;
    XSetWMNormalHints(dpy, win, &hints);
    XFlush(dpy);
    if (refit)
        host_window_resize(sapp_width(), (int)(((long)sapp_width() * h + w / 2) / w));
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
