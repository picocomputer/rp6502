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
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#import <Cocoa/Cocoa.h>

/* AppKit sizes a window in points and puts the origin of the screen at its
 * bottom-left corner, so keeping the top-left corner still means keeping the
 * top edge, the origin's y plus the height, where it is. A window zoomed to
 * fill the screen or in fullscreen keeps the size AppKit gave it. */
static void resize_content(NSSize size)
{
    NSWindow *win = (__bridge NSWindow *)sapp_macos_get_window();
    if ([win isZoomed] || ([win styleMask] & NSWindowStyleMaskFullScreen))
        return;
    NSRect frame = [win frame];
    NSRect next = [win frameRectForContentRect:NSMakeRect(0, 0, size.width, size.height)];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [win setFrame:next display:NO];
}

void host_window_resize(int w, int h)
{
    /* w and h are framebuffer pixels, and with high_dpi a point is dpi_scale
     * of them. */
    float d = sapp_dpi_scale();
    resize_content(NSMakeSize(w / d, h / d));
}

/* The re-fit works in points, the units AppKit keeps the window in, so the
 * width it keeps is exactly the window's. */
void host_window_set_aspect(int w, int h, bool refit)
{
    NSWindow *win = (__bridge NSWindow *)sapp_macos_get_window();
    [win setContentAspectRatio:NSMakeSize(w, h)];
    if (refit)
    {
        CGFloat width = [win contentRectForFrameRect:[win frame]].size.width;
        resize_content(NSMakeSize(width, round(width * h / w)));
    }
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
     * open is reparented to launchd and leaves no zombie to reap. */
    pid_t pid = fork();
    if (pid == 0)
    {
        if (fork() == 0)
        {
            execl("/usr/bin/open", "open", url, (char *)NULL);
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
    /* Without high_dpi the backbuffer stays at the window's size in points,
     * and on a Retina display the compositor stretches it, which smears the
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
