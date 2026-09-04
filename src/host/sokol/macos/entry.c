/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * macOS window host: the sokol entry (entry_run -> sapp_run). Cocoa needs no
 * manual resize or aspect hint, so those hooks are no-ops; the render/frame/
 * present pipeline is in host/sokol/app/app.c.
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_log.h"
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
/* Held with no program until a .rp6502 is dropped: the core freezes the machine
 * and draws the "drop a ROM" prompt instead of the canvas while this is set. */
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
    /* Fire-and-forget /usr/bin/open; double-fork so the grandchild reparents to
     * launchd and leaves no zombie for us to reap. */
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
    sapp_run(&(sapp_desc){
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = win_w,
        .height = win_h,
        .swap_interval = 1,
        .window_title = "Picocomputer 6502",
        .enable_dragndrop = true, /* drop a .rp6502 to boot it */
        .enable_clipboard = true, /* Cmd+V types into the emulated keyboard */
        .clipboard_size = 65536,
        .logger.func = slog_func,
    });
    return app_exit_code();
}
