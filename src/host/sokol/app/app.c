/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/dir.h"
#include "osal/fs.h"
#include "osal/os.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/run.h"
#include "host/sokol/app/pace.h"
#include "host/host.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/util/sokol_framebuffer.h"
#include "sokol/util/sokol_letterbox.h"
#include "sokol/util/sokol_debugtext.h"
#include "sokol/util/sokol_gl.h"
#ifdef EMU_WITH_DEBUGGER
#include "host/sokol/dbg/dbgui.h"
#include "core/dap/dap.h"
#endif
#include "host/sokol/app/entry.h"
#include "host/sokol/app/prompt.h"
#include "host/sokol/app/icon.h"
#include "host/sokol/app/input.h"
#ifdef RP6502_PAD_HOST
#include "host/sokol/app/gamepad.h"
#endif
#include "core/sys/version.h"
#include "core/sys/debug_log.h"
#include "core/aud/mix.h"
#include "core/dap/dbg.h"
#include "core/sys/proc.h"
#include "core/api/proc.h"
#include "core/com/com.h"
#include "core/str/oem.h"
#include "core/hid/mouse.h"
#include "core/rom/rom.h"
#include "core/sys/sys.h"
#include "core/term/vterm_mouse.h"
#include "core/vga/vga_emu.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct
{
    bool exit_on_halt;
    int title_variant;
} app;

/* Read in app_cleanup, after the machine's thread is gone. */
static bool app_quit_asked;
static bool app_broke;

static bool (*app_break_asked)(void);
static void (*app_break_leave)(void);

void app_set_break(bool (*asked)(void), void (*leave)(void))
{
    app_break_asked = asked;
    app_break_leave = leave;
}

void app_set_unpaced(bool on)
{
    run_set_unpaced(on);
}

/* The window closes at the end of this callback. */
static void app_quit(void)
{
    if (!app_quit_asked)
    {
        app_quit_asked = true;
        sapp_request_quit();
    }
}

#ifndef __EMSCRIPTEN__
/* Whether the window is up, which makes the clipboard the desktop's. */
static bool app_has_window;
static void app_clipboard_serve(void);
#endif

/* Whether the last present waited for the display, and when the callbacks came
 * and went, for the display ticks the machine is paced by (run.h). */
static uint64_t entered_ns, returned_ns;
static bool paced, shown_new;
/* How long the window's own work after its wait for the machine took last
 * time, which the wait leaves room for before the display wants the next
 * present. */
static uint64_t work_ns;
#define WORK_MARGIN_NS 1000000
/* Callbacks whose present does not wait, and every how many of them one goes
 * at once: a half second's. */
static unsigned unpaced_presents;
#define UNPACED_PROBE 30
/* A present that waits for the display holds the thread for milliseconds and
 * one that waits for nothing returns in microseconds, so two milliseconds
 * between the stamp taken before the upload and the next callback tells the
 * two apart. */
#define PRESENT_WAIT_NS 2000000

/* The title says what the window is doing with the pointer, and whether the
 * program has stopped. Which one is read from the machine, under its lock,
 * and set once the lock is let go. */
static int title_variant(void)
{
    if (proc_exited())
        return 1;
    if (mouse_is_mapped() && input_mouse_locked())
        return 3;
    if (mouse_is_mapped() && !input_pointer_to_tablet())
        return 2;
    return 0;
}

static void update_title(int v)
{
    static const char *const titles[] = {
        "Picocomputer 6502",
        "Picocomputer 6502 (stopped)",
        "Picocomputer 6502  -  click to capture mouse",
        "Picocomputer 6502  -  Esc releases mouse",
    };
    if (v != app.title_variant)
    {
        app.title_variant = v;
        sapp_set_window_title(titles[v]);
    }
}

#ifndef __EMSCRIPTEN__
/* Called on the sound device's own thread (entry.h). It makes what it plays
 * on the spot, from what the frames have stamped, at the device's rate. The
 * web player has its own sound (host/web/audio.c). */
static void app_sound(float *dst, int frames, uint32_t rate)
{
    aud_set_sink_rate(rate);
    aud_render(dst, frames);
}
#endif

void app_init(void)
{
#ifndef __EMSCRIPTEN__
    /* A web page sets its own tab icon. */
    sapp_set_icon(icon_desc());
#endif
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = app_log,
    });
#ifndef __EMSCRIPTEN__
    if (aud_enabled())
    {
        aud_set_streaming(true);
        host_audio_open(app_sound);
    }
#endif
    gfx_setup();
    host_window_init();
    input_read_locks();
    input_locks();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init(sapp_dpi_scale());
#endif
#ifndef __EMSCRIPTEN__
    app_has_window = true;
#endif
    /* Last, once all the machine reaches is up. */
    run_start();
}

void app_frame(void)
{
    const uint64_t now = os_mono_ns();
    /* Once the display has been seen to pace the callbacks it keeps that
     * standing while frames are still coming, because a callback whose own work
     * fills the display period leaves the present nothing left to wait for;
     * and while the machine follows the display, whose callbacks at 240 Hz are
     * so close that one late leaves the next present little to wait for. A
     * display that stops pacing is seen by the pace first. */
    paced = !returned_ns || now - returned_ns >= PRESENT_WAIT_NS || (paced && (shown_new || run_follows()));
    const uint64_t dt = entered_ns ? now - entered_ns : VGA_FRAME_NS;
    entered_ns = now;

    /* What the frames due at this callback read is handed over first, so they
     * see it at the age the events before the callback have: the lock keys
     * and the gamepads. The OS is asked without the machine's lock, which
     * only the handing over takes. */
#ifndef __EMSCRIPTEN__
    app_clipboard_serve();
#endif
#ifdef RP6502_PAD_HOST
    gamepad_input_poll();
#endif
    input_read_locks();
    run_lock();
    input_locks();
#ifdef RP6502_PAD_HOST
    gamepad_input_report();
#endif
    run_unlock();

    /* The machine runs the frames due, and the window waits for them as long
     * as it can and still make the display's next present. A present that
     * does not wait keeps no time, so the window keeps a frame's. */
    uint64_t room;
    if (paced)
    {
        const int64_t period = run_period();
        const int64_t left = period - (int64_t)work_ns - WORK_MARGIN_NS;
        room = (uint64_t)(left > period / 4 ? left : period / 4);
    }
    else if (++unpaced_presents % UNPACED_PROBE == 0)
        /* A present made at the machine's rate never waits on a display
         * faster than it, so a display that does pace would never be seen
         * to: now and then one goes straight after the last, to find out. */
        room = 0;
    else
    {
        /* The window shows each new frame as the machine finishes it, but
         * no sooner than half a frame after the last callback, which bounds
         * it under --phi2 0. It does not wait on its own clock, which would
         * line its present up with the display's and leave the present
         * nothing to wait for. */
#ifndef __EMSCRIPTEN__
        const uint64_t soonest = now - dt + VGA_FRAME_NS / 2;
        if (soonest > now)
            os_sleep_ns(soonest - now);
#endif
        room = VGA_FRAME_NS;
    }
    run_display(now, dt, paced, room);
    const uint64_t waited = os_mono_ns();

    run_lock();
    gfx_set_frame(run_frame(&shown_new));
#ifdef RP6502_PAD_HOST
    gamepad_input_watch();
#endif
    input_pointer_task();
    const int title = title_variant();
    /* The machine's last picture under exit_on_halt is the frame just taken,
     * so it is presented before the window closes. */
    const bool quit = run_quit_asked();
#ifdef EMU_WITH_DEBUGGER
    /* The debugger windows are built before the upload below, because
     * dbgui_canvas_rect reports the dockspace central node only after a
     * dbgui_draw and the canvas viewport is fitted into that rect.
     *
     * A dpi_scale of 1.0 makes ImGui units framebuffer pixels, so the UI scale,
     * rather than a fractional display scale, sizes the bitmap font, and at a
     * whole-multiple UI scale each texel lands on one pixel. */
    if (dbg_is_active())
    {
        dbgui_new_frame(sapp_width(), sapp_height(), (double)dt / 1e9, 1.0f);
        dbgui_draw();
    }
#endif
    /* After dbgui_draw, because ImGui knows which item the pointer is over only
     * once the windows for this frame are built. */
    input_cursor_task();
    run_unlock();

    input_apply();
    update_title(title);
    if (quit)
        app_quit();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dap_quit_requested())
        app_quit();
#endif
    /* A console break is latched by the host's break handler and answered here,
     * because the machine has to be taken down by the loop that shows it, and
     * because this runs even while a debugger is holding the machine. */
    if (app_break_asked && app_break_asked() && !app_broke)
    {
        app_broke = true;
        app_quit();
    }
    gfx_canvas_changed();

    /* The stamp is taken before the upload because on Mesa DRI3 the wait for
     * the display happens on the next GL write rather than in the swap. Nothing
     * from here on reaches the machine, so a present the compositor holds up
     * holds up only the window. */
    returned_ns = os_mono_ns();
    work_ns = returned_ns - waited;
    gfx_upload(shown_new);
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_flush();
#endif
    gfx_begin_pass();
    gfx_blit();
    host_window_menu_draw();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_render();
#endif
    gfx_end_pass();
}

char *app_rom_path(const char *host)
{
    if (oem_maps_utf8(host))
    {
        /* oem_from_utf8 writes one byte per UTF-8 sequence, so the UTF-8
         * length holds the result. A name FAT refuses has no drive path
         * either, and os_dir_realpath returns NULL for it. So does a path
         * whose absolute form is longer than API_PATH_MAX. */
        size_t sz = strlen(host) + 1;
        char *oem = malloc(sz);
        if (oem)
            oem_from_utf8(host, oem, sz);
        char *abs = oem ? os_dir_realpath(oem) : NULL;
        bool named = abs && strlen(abs) <= API_PATH_MAX;
        free(abs);
        if (named)
            return oem;
        free(oem);
    }
    const char *name = rom_alias_insert(host);
    char *rom = name ? malloc(strlen(name) + 2) : NULL;
    if (rom)
        sprintf(rom, ":%s", name);
    return rom;
}

/* The ":name" installed on the null drive for the last dropped file that
 * booted, or NULL. */
static char *dropped_rom;

bool app_boot_rom(const char *path)
{
#ifdef EMU_WITH_DEBUGGER
    /* A DAP client controls the run state. A plain --debug session has no DAP
     * client, so a ROM dropped on it boots. */
    if (dap_is_active())
        return false;
#endif
    char *rom = app_rom_path(path);
    if (!rom)
    {
        com_printf("cannot read dropped file\n");
        return false;
    }
    /* The file is checked before proc_boot stops the machine, so an accidental
     * drop leaves the running program running; the loader would refuse the file
     * too, but only after stopping that program. The check uses an ordinary
     * descriptor, because there is one ROM descriptor and the running program
     * has it open for its assets. A file on the null drive opens only as a ROM
     * image, on that one descriptor, so it goes to the loader unchecked. */
    if (rom[0] != ':')
    {
        rom_pump_t pump;
        api_errno err;
        int fd = fs_std_open(rom, FS_RD, &err);
        if (fd < 0 || !rom_pump_open_fd(&pump, fd, &err))
        {
            com_printf(err == API_ENOEXEC ? "not a .rp6502 file (bad magic)\n"
                                          : "cannot read dropped file\n");
            free(rom);
            return false;
        }
        rom_pump_close(&pump);
    }
    bool ok = proc_boot(rom, 0, NULL, 0);
    /* The install from the previous drop is removed once proc_boot has
     * stopped the machine, whether or not this boot succeeded, unless this
     * drop installed a ROM of the same name, which replaced it. */
    if (dropped_rom && strcasecmp(dropped_rom, rom) != 0)
        rom_alias_remove(dropped_rom);
    if (!ok && rom[0] == ':')
        rom_alias_remove(rom);
    free(dropped_rom);
    dropped_rom = ok && rom[0] == ':' ? rom : NULL;
    if (rom != dropped_rom)
        free(rom);
    if (!ok)
        return false;
    /* proc_boot only requests the machine, and a caller outside a driver pass
     * carries the request out itself. */
    sys_commit();
    return true;
}

/* Sokol says when the window is minimized and when it is back. The web
 * player's page has neither, and its machine stops while the page is
 * hidden, so there it is never minimized. The terminal asks from the
 * machine's thread. */
static _Atomic bool app_minimized;

bool host_minimized(void)
{
    return atomic_load(&app_minimized);
}

/* The desktop's window and the web player both gain and lose focus. */
bool host_focusable(void)
{
    return true;
}

#ifndef __EMSCRIPTEN__
bool host_mac_keys(void)
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

/* The clipboard is the desktop's while the window is up. Only the window's
 * thread may reach it, never under the machine's lock, because the OS may
 * keep a clipboard call waiting on another program; and the machine copies
 * to it and pastes from it on its own thread. So a copy waits in
 * app_clipboard_out, newest wins, for the window to hand it over. A paste
 * asks for the text as its turn comes: the machine raises
 * app_clipboard_asked and the paste waits (HOST_CLIPBOARD_LATER) until the
 * window, at its next event or frame, hands over the copies still waiting
 * and then the desktop's text in app_clipboard_in. A text not taken within a
 * moment is no longer the clipboard as it is, and is asked for again.
 *
 * With no window, a script or a headless run, the clipboard is one the
 * emulator keeps for itself, empty until something is put in it, which
 * asking for never writes over. The web player's are in its entry.c. */
#define APP_CLIPBOARD_FRESH_NS 100000000ull
static char *_Atomic app_clipboard_out;
static _Atomic bool app_clipboard_asked;
static char *_Atomic app_clipboard_in;
static _Atomic uint64_t app_clipboard_in_at;
static char *app_clipboard_given; /* the machine's, until it asks again */
static bool app_clipboard_too_big;
static char *app_clipboard;

/* A copy of utf8, or NULL when there is no memory for it. */
static char *app_clipboard_copy(const char *utf8)
{
    const size_t n = strlen(utf8) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, utf8, n);
    return copy;
}

const char *host_clipboard_get(void)
{
    if (!app_has_window)
        return app_clipboard ? app_clipboard : "";
    char *in = atomic_exchange(&app_clipboard_in, NULL);
    if (in && os_mono_ns() - atomic_load(&app_clipboard_in_at) < APP_CLIPBOARD_FRESH_NS)
    {
        free(app_clipboard_given);
        app_clipboard_given = in;
        return in;
    }
    free(in);
    atomic_store(&app_clipboard_asked, true);
    return HOST_CLIPBOARD_LATER;
}

bool host_clipboard_set(const char *utf8)
{
    char *copy = app_clipboard_copy(utf8);
    if (!copy)
        return false;
    if (app_has_window)
        free(atomic_exchange(&app_clipboard_out, copy));
    else
    {
        free(app_clipboard);
        app_clipboard = copy;
    }
    return true;
}

static void app_clipboard_flush(void)
{
    char *out = atomic_exchange(&app_clipboard_out, NULL);
    if (!out)
        return;
    sapp_set_clipboard_string(out);
    free(out);
}

/* What the machine asked of the clipboard, answered without the lock: the
 * copies first, so a cut and then a paste paste what was cut. sokol hands
 * back the text it last had when the desktop's is too big for its buffer,
 * so then there is none. */
static void app_clipboard_serve(void)
{
    app_clipboard_flush();
    if (!atomic_exchange(&app_clipboard_asked, false))
        return;
    app_clipboard_too_big = false;
    const char *text = sapp_get_clipboard_string();
    char *copy = app_clipboard_copy(app_clipboard_too_big ? "" : text);
    atomic_store(&app_clipboard_in_at, os_mono_ns());
    free(atomic_exchange(&app_clipboard_in, copy));
}
#endif

/* Every event can reach into the machine, so each is taken under its lock.
 * An event the OS delivers inside a call the window makes under the lock
 * takes it again (run.h). */
static void app_event(const struct sapp_event *e)
{
    if (e->type == SAPP_EVENTTYPE_ICONIFIED)
        atomic_store(&app_minimized, true);
    else if (e->type == SAPP_EVENTTYPE_RESTORED)
        atomic_store(&app_minimized, false);
#ifndef __EMSCRIPTEN__
    /* The desktop window's focus. In the browser sokol's focus is the
     * browser window's, which several players on a page share, so the web
     * player's comes from its page instead (entry.c). */
    else if (e->type == SAPP_EVENTTYPE_FOCUSED)
        vterm_focus(true);
    else if (e->type == SAPP_EVENTTYPE_UNFOCUSED)
        vterm_focus(false);
#endif
    if (e->type == SAPP_EVENTTYPE_FILES_DROPPED)
    {
        host_window_files_dropped();
        return;
    }
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_handle_event(e))
        return;
#endif
    input_event(e);
}

void app_input(const struct sapp_event *e)
{
    /* What the event needs of the OS is asked for first, without the lock: the
     * clipboard the machine asked for, the drop-a-ROM card's link, a paste
     * into a debugger field, and the lock keys a key goes down with. What it
     * asks of the OS in return, the pointer's capture, is done after. */
#ifndef __EMSCRIPTEN__
    app_clipboard_serve();
    if (prompt_event(e))
        return;
#endif
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_clipboard_read(e);
#endif
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN)
        input_read_locks();
    run_lock();
    app_event(e);
    run_unlock();
    input_apply();
}

int app_exit_code(void)
{
    /* A console break leaves through app_break_leave, which does not return, so
     * a break recorded here is the window closed on a running machine. */
    if (app_broke)
        return 1;
    return (app.exit_on_halt && proc_exited()) ? proc_get_exit_code() : 0;
}

void app_cleanup(void)
{
    /* The machine stops first, so nothing below runs beside it. proc_shutdown
     * runs before the sound, gfx and sg shut down, so every driver's stop hook
     * still has the host under it. Reaching here without app_quit_asked means
     * the close button stopped a machine that was still running, which is a
     * break. */
    run_stop();
#ifndef __EMSCRIPTEN__
    app_clipboard_serve();
    app_has_window = false;
#endif
    if (!app_quit_asked)
        app_broke = true;
    proc_shutdown();
    /* app_break_leave does not return, so buffered output is flushed before
     * it. */
    if (app_broke && app_break_leave && app_break_asked && app_break_asked())
    {
        fflush(NULL);
        app_break_leave();
    }
#ifdef RP6502_PAD_HOST
    gamepad_input_stop();
#endif
#ifndef __EMSCRIPTEN__
    if (aud_enabled())
        host_audio_close();
#endif
#ifdef EMU_WITH_DEBUGGER
    dap_stop();
    if (dbg_is_active())
        dbgui_discard();
#endif
    gfx_shutdown();
    sg_shutdown();
}

void app_prepare(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt,
                 int *out_w, int *out_h)
{
    /* Written as !(scale >= 0.1) so that a NaN also lands on the floor. */
    if (!(scale >= 0.1))
        scale = 0.1;
    if (scale > 64.0)
        scale = 64.0;
    app.exit_on_halt = exit_on_halt;
    run_prepare(fb, exit_on_halt);
    bool fresh;
    gfx_set_frame(run_frame(&fresh));
    gfx_prepare(scale, have_scale, out_w, out_h);
}

#ifndef __EMSCRIPTEN__
/* Every desktop's window. high_dpi asks for a backbuffer at the display's own
 * resolution, where without it the compositor stretches one at the logical
 * size and smears the canvas; X11 has no such scaling, and sokol ignores it
 * there. The web player's is in its entry.c. */
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
#endif

void app_log(const char *tag, uint32_t log_level, uint32_t log_item_id,
             const char *message_or_null, uint32_t line_nr,
             const char *filename_or_null, void *user_data)
{
    (void)user_data;
#ifndef __EMSCRIPTEN__
    if (log_item_id == SAPP_LOGITEM_CLIPBOARD_STRING_TOO_BIG && !strcmp(tag, "sapp"))
        app_clipboard_too_big = true;
#endif
    const char *message = message_or_null ? message_or_null : "";
    const char *filename = filename_or_null ? filename_or_null : "";
    switch (log_level)
    {
    case 0:
        RP6502_LOG(sokol, ERROR, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        abort();
    case 1:
        RP6502_LOG(sokol, ERROR, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        break;
    case 2:
        RP6502_LOG(sokol, WARN, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        break;
    default:
        RP6502_LOG(sokol, INFO, "%s [%u] %s (%s:%u)", tag, log_item_id, message, filename, line_nr);
        break;
    }
}
