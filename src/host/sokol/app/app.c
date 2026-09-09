/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "osal/fs.h"
#include "osal/os.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/cli/state.h"
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
#include "core/sys/com.h"
#include "core/str/oem.h"
#include "core/hid/vtkeys.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
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

/* debt_ns is the wall time owed to the machine, repaid in whole frames, and
 * cleared outright under app.unpaced and on a debugger hold. */
static uint64_t entered_ns, returned_ns, debt_ns, machine_ns;
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
    else if (mouse_is_mapped() && sapp_mouse_locked())
    {
        v = 3;
        t = "Picocomputer 6502  -  Esc releases mouse";
    }
    else if (mouse_is_mapped() && !tablet_is_mapped())
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

/* Called on the audio device's own thread on WASAPI, ALSA, CoreAudio and
 * AAudio, and on the browser's main thread on WebAudio. */
static void stream_cb(float *buffer, int num_frames, int num_channels)
{
    (void)num_channels;
    aud_render(buffer, num_frames);
}

void app_init(void)
{
    sapp_set_icon(icon_desc());
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = app_log,
    });
    if (aud_enabled())
    {
        /* Most devices run at 48000 Hz, so asking for it keeps the resampler's
         * ratio near one. ALSA and WebAudio write back the rate they actually
         * got and the machine resamples from its own rate to that; WASAPI and
         * CoreAudio keep the 48000 asked for and the OS resamples. 512 frames
         * of that rate is 10.7 ms of device latency, where sokol's default of
         * 2048 is 43. */
        saudio_setup(&(saudio_desc){
            .sample_rate = 48000,
            .num_channels = 2,
            .buffer_frames = 512,
            .stream_cb = stream_cb,
            .logger.func = app_log,
        });
        aud_set_sink_rate((uint32_t)saudio_sample_rate());
        /* The callback runs on the device's own thread on every backend but
         * WebAudio, so saving or loading a state has to park it out of the
         * engines. The flag is set on WebAudio too, where the callback shares
         * the main thread and the park goes unanswered until state_park's spin
         * bound runs out. */
        state_audio_is_threaded(true);
    }
    gfx_setup();
    host_window_init();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init();
#endif
}

void app_frame(void)
{
    const uint64_t now = os_mono_ns();
    /* Once the display has been seen to pace the callbacks it keeps that
     * standing while frames are still coming, because a callback whose own work
     * fills the display period leaves the present nothing left to wait for. */
    paced = !returned_ns || now - returned_ns >= PRESENT_WAIT_NS || (paced && ran_frame);
    uint64_t dt = entered_ns ? now - entered_ns : VGA_FRAME_NS;
    entered_ns = now;
    /* A 59.94 Hz display delivers a callback every 16.68 ms against the
     * emulated 16.67, so an interval within a tenth of a frame counts as
     * exactly one frame and the two rates do not beat against each other. */
    if (paced && dt > VGA_FRAME_NS - VGA_FRAME_NS / 10 && dt < VGA_FRAME_NS + VGA_FRAME_NS / 10)
        dt = VGA_FRAME_NS;
    if (dt > 3 * VGA_FRAME_NS) /* a longer stall is dropped, not replayed as a burst */
        dt = 3 * VGA_FRAME_NS;
    debt_ns += dt;
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
#ifdef RP6502_PAD_HOST
    /* Sokol has delivered this frame's key and pointer events before the frame
     * callback, so polling the gamepads here gives them the same age as every
     * other input. */
    gamepad_input_task();
#endif

    /* Nothing limits the callback rate when the present does not wait, and
     * nothing runs at all while a debugger holds the machine, so sleep out the
     * rest of the frame period rather than spinning. */
    if (!app.unpaced && (!paced || held) && debt_ns < VGA_FRAME_NS)
    {
        os_sleep_ns(VGA_FRAME_NS - debt_ns);
        const uint64_t woke = os_mono_ns();
        debt_ns += woke - entered_ns;
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
        debt_ns = 0;
    }
    else
        while (debt_ns >= VGA_FRAME_NS)
        {
            /* A debugger hold clears the debt, so the machine does not run a
             * burst of catch-up frames when it resumes. */
            if (!vga_run_frame())
            {
                debt_ns = 0;
                held = true;
                break;
            }
            debt_ns -= VGA_FRAME_NS;
            ran_frame = true;
        }
    machine_ns += os_mono_ns() - t0;

    /* The absolute tablet never captures the pointer, and a program that has
     * unmapped the mouse no longer wants it. */
    if (sapp_mouse_locked() && (!mouse_is_mapped() || tablet_is_mapped()))
        sapp_lock_mouse(false);
    update_title();
    /* A host overlay, such as the Android ROM menu or the desktop drop-a-ROM
     * prompt, holds the CPU with no program loaded, so the halt it shows is not
     * a program exiting and must not close the window. */
    if (proc_exited() && app.exit_on_halt && !host_window_menu_active())
    {
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
     * A dpi_scale of 1.0 renders the overlay at native resolution, so its 13px
     * bitmap font lands one texel to a pixel instead of being magnified. */
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
    gfx_begin_pass();
    gfx_blit();
    host_window_menu_draw();
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_render();
#endif
    gfx_end_pass();
}

bool app_boot_rom(const char *path)
{
#ifdef EMU_WITH_DEBUGGER
    /* A DAP client owns the run state. A plain --debug session does not, so a
     * ROM dropped on it boots. */
    if (dap_is_active())
        return false;
#endif
    /* The host hands a UTF-8 path and the machine works in the guest's OEM code
     * page. A character the code page cannot spell becomes 0x7F, which names no
     * file, so an unrepresentable path is refused here before the machine is
     * touched rather than halting the running program on a failed load.
     * oem_from_utf8 writes one OEM byte per UTF-8 sequence, so the UTF-8 length
     * always holds the result. */
    size_t osz = strlen(path) + 1;
    char *oem = malloc(osz);
    if (!oem)
        return false;
    oem_from_utf8(path, oem, osz);
    size_t bsz = oem_to_utf8(oem, NULL, 0) + 1;
    char *back = malloc(bsz);
    if (back)
        oem_to_utf8(oem, back, bsz);
    bool same = back && strcmp(path, back) == 0;
    free(back);
    if (!same)
    {
        free(oem);
        com_printf("dropped path not representable in the OEM code page\n");
        return false;
    }
    /* The file is screened before proc_boot stops the machine, so an accidental
     * drop leaves the running program alone; the loader would refuse it too, but
     * only after that program was gone. The screen uses an ordinary descriptor
     * because there is one ROM descriptor and the running program is holding it
     * open for its assets. */
    uint8_t buf[ROM_RECORD_MAX];
    rom_pump_t pump;
    api_errno err;
    int fd = fs_std_open(oem, FS_RD, &err);
    if (fd < 0 || !rom_pump_open_fd(&pump, fd, buf, &err))
    {
        com_printf(err == API_ENOEXEC ? "not a .rp6502 file (bad magic)\n"
                                      : "cannot read dropped file\n");
        free(oem);
        return false;
    }
    rom_pump_close(&pump);
    vtkeys_paste_cancel(); /* the new program must not receive the old one's paste */
    bool ok = proc_boot(oem, 0, NULL, PROC_UNCHAIN);
    free(oem);
    if (!ok)
        return false;
    /* proc_boot only requests the machine, and a caller outside a driver pass
     * carries the request out itself. */
    sys_commit();
    return true;
}

void app_input(const struct sapp_event *e)
{
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
    /* sys_stop and sys_commit run before saudio, gfx and sg shut down, so every
     * driver's stop hook still has the host under it. Reaching here without
     * app_quit_asked means the close button stopped a machine that was still
     * running, which is a break. */
    if (!app_quit_asked)
        app_broke = true;
    if (app_broke)
        sys_break_request();
    sys_stop();
    sys_commit();
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
