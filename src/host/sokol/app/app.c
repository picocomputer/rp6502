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
#include "host/sokol/app/pace.h"
#include "host/host.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/util/sokol_framebuffer.h"
#include "sokol/util/sokol_letterbox.h"
#include "sokol/util/sokol_debugtext.h"
#include "sokol/util/sokol_gl.h"
#include "sokol/sokol_audio.h"
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
#include "core/vga/vga_emu.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct
{
    bool exit_on_halt;
    bool unpaced; /* --phi2 0 */
    int title_variant;
} app;

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
    app.unpaced = on;
}

/* The pace's debt is the wall time owed to the machine, repaid in whole
 * frames (pace.h), and cleared outright under app.unpaced and on a debugger
 * hold. */
static uint64_t entered_ns, returned_ns, machine_ns;
static pace_t pace;
static bool paced, ran_frame, held;
/* A present that waits for the display holds the thread for milliseconds and
 * one that waits for nothing returns in microseconds, so two milliseconds
 * between the stamp taken before the upload and the next callback tells the
 * two apart. */
#define PRESENT_WAIT_NS 2000000

uint64_t app_machine_ns(void) { return machine_ns; }

static void update_title(void)
{
    int v;
    const char *t;
    if (proc_exited())
    {
        v = 1;
        t = "Picocomputer 6502 (stopped)";
    }
    else if (mouse_is_mapped() && input_mouse_locked())
    {
        v = 3;
        t = "Picocomputer 6502  -  Esc releases mouse";
    }
    else if (mouse_is_mapped() && !input_pointer_to_tablet())
    {
        v = 2;
        t = "Picocomputer 6502  -  click to capture mouse";
    }
    else
    {
        v = 0;
        t = "Picocomputer 6502";
    }
    if (v != app.title_variant)
    {
        app.title_variant = v;
        sapp_set_window_title(t);
    }
}

/* Called on the audio device's own thread on WASAPI, PulseAudio, CoreAudio and
 * AAudio, and on the browser's main thread on WebAudio. It makes what it
 * plays on the spot, from what the frames have stamped. */
static void stream_cb(float *buffer, int num_frames, int num_channels)
{
    (void)num_channels;
    aud_render(buffer, num_frames);
}

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
    if (aud_enabled())
    {
        /* Most devices run at 48000 Hz, so asking for it keeps the resampler's
         * ratio near one. WebAudio writes back the rate it actually got and
         * the machine resamples from its own rate to that; WASAPI, PulseAudio
         * and CoreAudio keep the 48000 asked for and the OS resamples.
         *
         * buffer_frames is what the callback is asked for at a time, and
         * the sound plays that far behind the machine. What the device
         * holds beyond that is each backend's own. The sizes are the least
         * that measured clean:
         *
         * WASAPI in shared mode takes 480 frames every 10 ms from a buffer of
         * its own choosing, about 22 ms for any size asked for up to that. So
         * 480 is one callback for each period it takes; 512 would skip every
         * sixteenth and leave the mix a beat to cover.
         *
         * PulseAudio is asked to hold buffer_frames in all, and holds three
         * quarters of it in the stream, 8 ms at 512. That ran minutes
         * without running dry on a host with little else to do, where 256
         * did not, and ran dry a few times a minute beside a compiler.
         *
         * CoreAudio's queue is two buffers of buffer_frames, a callback for
         * each. 512 is 94 callbacks a second with 10.7 ms in hand; fewer
         * frames would be more calls into the OS for nothing.
         *
         * On the web, ScriptProcessorNode runs on the main thread with the
         * machine and the drawing, and its size must be a power of two. In
         * Chrome 1024 frames, 21.3 ms, still went silent for a 128th of a
         * second once in a while, and 512 twice as often.
         *
         * Sokol wants buffer_frames a multiple of its packets, which only
         * its push model uses, so they are a frame each. */
#if defined(__EMSCRIPTEN__)
        const int buffer_frames = 1024;
#elif defined(_WIN32)
        const int buffer_frames = 480;
#else
        const int buffer_frames = 512;
#endif
        aud_set_streaming(true);
        saudio_setup(&(saudio_desc){
            .sample_rate = 48000,
            .num_channels = 2,
            .buffer_frames = buffer_frames,
            .packet_frames = 1,
            .stream_cb = stream_cb,
            .logger.func = app_log,
        });
        aud_set_sink_rate((uint32_t)saudio_sample_rate());
    }
    gfx_setup();
    host_window_init();
    input_locks();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init(sapp_dpi_scale());
#endif
}

void app_frame(void)
{
    const uint64_t now = os_mono_ns();
    /* Once the display has been seen to pace the callbacks it keeps that
     * standing while frames are still coming, because a callback whose own work
     * fills the display period leaves the present nothing left to wait for;
     * and while the pace follows the display, whose callbacks at 240 Hz are
     * so close that one late leaves the next present little to wait for. A
     * display that stops pacing is seen by the pace first. */
    paced = !returned_ns || now - returned_ns >= PRESENT_WAIT_NS || (paced && (ran_frame || pace.follows));
    const uint64_t dt = entered_ns ? now - entered_ns : VGA_FRAME_NS;
    pace_add(&pace, dt, paced);
    aud_set_speed(pace_speed(&pace));
    entered_ns = now;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        dap_pump();
        if (dap_quit_requested())
        {
            app_quit_asked = true;
            sapp_request_quit();
        }
    }
#endif
    input_locks();
#ifdef RP6502_PAD_HOST
    /* Sokol has delivered this frame's key and pointer events before the frame
     * callback, so polling the gamepads here gives them the same age as every
     * other input. */
    gamepad_input_task();
#endif

    /* Nothing limits the callback rate when the present does not wait, and
     * nothing runs at all while a debugger holds the machine, so sleep out the
     * rest of the frame period rather than spinning. */
    if (!app.unpaced && (!paced || held) && pace.debt_ns < (int64_t)VGA_FRAME_NS)
    {
        os_sleep_ns((uint64_t)((int64_t)VGA_FRAME_NS - pace.debt_ns));
        const uint64_t woke = os_mono_ns();
        pace_slept(&pace, woke - entered_ns);
        entered_ns = woke;
    }
    ran_frame = held = false;
    const uint64_t t0 = os_mono_ns();
    if (app.unpaced)
    {
        do
        {
            if (!vga_run_frame())
            {
                held = true;
                break;
            }
            ran_frame = true;
        } while (os_mono_ns() - t0 < VGA_FRAME_NS);
        pace.debt_ns = 0;
    }
    else
        while (pace_due(&pace))
        {
            /* A debugger hold clears the debt, so the machine does not run a
             * burst of catch-up frames when it resumes. */
            if (!vga_run_frame())
            {
                pace.debt_ns = 0;
                held = true;
                break;
            }
            pace_ran(&pace);
            ran_frame = true;
        }
    machine_ns += os_mono_ns() - t0;

    input_pointer_task();
    update_title();
    /* The desktop drop-a-ROM prompt holds the CPU with no program loaded, so
     * the halt it shows is not a program exiting and must not close the
     * window. The frame that saw the exit drew the lines above the beam
     * before the program's last writes, so one more frame is drawn after it
     * to be the last picture presented. */
    if (proc_exited() && app.exit_on_halt && !host_window_menu_active())
    {
        vga_run_frame();
        ran_frame = true;
        app_quit_asked = true;
        sapp_request_quit();
    }
    /* A console break is latched by the host's break handler and answered here,
     * because the machine has to be taken down by the loop that runs it, and
     * because this runs even while a debugger is holding the machine. */
    if (app_break_asked && app_break_asked() && !app_broke)
    {
        app_quit_asked = app_broke = true;
        sapp_request_quit();
    }

    static uint64_t bench_limit_ns = UINT64_MAX, bench_start_ns;
    if (bench_limit_ns == UINT64_MAX)
    {
        const char *e = getenv("EMU_BENCH_MS");
        bench_limit_ns = e ? (uint64_t)(atof(e) * 1000000.0) : 0;
        bench_start_ns = now;
    }
    if (bench_limit_ns && now - bench_start_ns >= bench_limit_ns)
    {
        const double secs = (double)(now - bench_start_ns) / 1e9;
        fprintf(stderr, "EMU_BENCH: %lu VGA frames in %.3fs = %.1f Hz\n",
                vga_frame_count(), secs, (double)vga_frame_count() / secs);
        app_quit_asked = true;
        sapp_request_quit();
    }

    gfx_canvas_changed();

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
    input_update_cursor();

    /* The stamp is taken before the upload because on Mesa DRI3 the wait for
     * the display happens on the next GL write rather than in the swap. */
    returned_ns = os_mono_ns();
    gfx_upload(ran_frame);
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
 * hidden, so there it is never minimized. */
static bool app_minimized;

bool host_minimized(void)
{
    return app_minimized;
}

#ifndef __EMSCRIPTEN__
/* The clipboard is the desktop's while the window runs. sokol hands back the
 * text it last had when the desktop's is too big for its buffer, so then
 * there is none. With no window, a script or a headless run, the clipboard
 * is one the emulator keeps for itself, empty until something is put in it,
 * which asking for never writes over. The web player's are in its entry.c. */
static bool app_clipboard_too_big;
static char *app_clipboard;

const char *host_clipboard_get(void)
{
    if (!sapp_isvalid())
        return app_clipboard ? app_clipboard : "";
    app_clipboard_too_big = false;
    const char *text = sapp_get_clipboard_string();
    return app_clipboard_too_big ? "" : text;
}

bool host_mac_keys(void)
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool host_clipboard_set(const char *utf8)
{
    if (sapp_isvalid())
    {
        sapp_set_clipboard_string(utf8);
        return true;
    }
    const size_t n = strlen(utf8) + 1;
    free(app_clipboard);
    app_clipboard = malloc(n);
    if (!app_clipboard)
        return false;
    memcpy(app_clipboard, utf8, n);
    return true;
}
#endif

void app_input(const struct sapp_event *e)
{
    if (e->type == SAPP_EVENTTYPE_ICONIFIED)
        app_minimized = true;
    else if (e->type == SAPP_EVENTTYPE_RESTORED)
        app_minimized = false;
    if (e->type == SAPP_EVENTTYPE_FILES_DROPPED)
    {
        host_window_files_dropped();
        return;
    }
    /* The docs link's rectangle is set by prompt_draw, so it means something
     * only while that prompt is the overlay on screen. */
    if (host_window_menu_active())
    {
        if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE)
            sapp_set_mouse_cursor(prompt_url_hit(e->mouse_x, e->mouse_y)
                                      ? SAPP_MOUSECURSOR_POINTING_HAND
                                      : SAPP_MOUSECURSOR_DEFAULT);
        else if (e->type == SAPP_EVENTTYPE_MOUSE_UP &&
                 e->mouse_button == SAPP_MOUSEBUTTON_LEFT &&
                 prompt_url_hit(e->mouse_x, e->mouse_y))
        {
            prompt_url_open();
            return;
        }
    }
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_handle_event(e))
        return;
#endif
    input_event(e);
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
    /* proc_shutdown runs before saudio, gfx and sg shut down, so every
     * driver's stop hook still has the host under it. Reaching here without
     * app_quit_asked means the close button stopped a machine that was still
     * running, which is a break. */
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
    if (aud_enabled())
        saudio_shutdown();
#ifdef EMU_WITH_DEBUGGER
    dap_stop();
    if (dbg_is_active())
        dbgui_discard();
#endif
    gfx_shutdown();
    sg_shutdown();
}

void app_prepare(uint32_t *fb, double scale, bool have_scale,
                         bool exit_on_halt, int *out_w, int *out_h)
{
    /* Written as !(scale >= 0.1) so that a NaN also lands on the floor. */
    if (!(scale >= 0.1))
        scale = 0.1;
    if (scale > 64.0)
        scale = 64.0;
    app.exit_on_halt = exit_on_halt;
    vga_set_framebuffer(fb);
    gfx_prepare(fb, scale, have_scale, out_w, out_h);
}

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
