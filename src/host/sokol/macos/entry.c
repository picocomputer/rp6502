/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/entry.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "core/hid/usage.h"
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

/* A Mac has no NumLock and its keypad always types digits, so NumLock reads
 * on. It has no Scroll Lock either. */
uint8_t host_locks(void)
{
    uint8_t locks = KEYBOARD_LED_NUMLOCK;
    if ([NSEvent modifierFlags] & NSEventModifierFlagCapsLock)
        locks |= KEYBOARD_LED_CAPSLOCK;
    return locks;
}

void host_window_init(void)
{
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
