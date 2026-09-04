/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The application: the four sokol callbacks, and the frame between them. It
 * paces the machine against the wall clock, runs whole frames off that debt,
 * and hands the picture to gfx.c and the events to input.c.
 *
 * It carries no platform #ifdefs (only the EMU_WITH_DEBUGGER feature gate).
 * Anything that differs per platform — the WM seam, the entry, the Android
 * overlay — is a hook in entry.h, implemented in each <os>/entry.c.
 */

#include "osal/fs.h"
#include "osal/os.h" /* os_mono_ns */
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
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
#include "core/aud/mix.h"
#include "core/dap/dbg.h"
#include "core/sys/proc.h"
#include "core/sys/com.h"
#include "core/str/oem.h"
#include "core/hid/vtkeys.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/rom/rom.h"
#include "core/wdc/resb.h"
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
    bool exit_on_halt; /* close the window when the program stops */
    int title_variant; /* last window-title state (running/stopped/mouse) */
} app;

/* Wall time owed to the machine, repaid in whole frames at the top of each
 * callback. When the present holds the thread the display paces us; when it
 * returns at once nothing does, and the callback sleeps to the next frame. */
static uint64_t entered_ns, returned_ns, debt_ns, machine_ns;
static bool paced, ran_frame, held;
#define FRAME_NS (1000000000ull / VGA_HZ)
#define PRESENT_WAIT_NS 2000000 /* longer than a present that waits for nothing */

uint64_t app_machine_ns(void) { return machine_ns; }
/* Reflect run + mouse-capture state in the window title, only when it changes.
 * When a program has mapped the mouse, the title carries the capture hint. */
static void update_title(void)
{
    int v;
    const char *t;
    if (!resb_running())
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

/* The buffer the device is about to play. On WASAPI, ALSA and AAudio this
 * is the device's own thread; on WebAudio it is the browser's main thread,
 * called from onaudioprocess between frames. */
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
        .logger.func = slog_func,
    });
    if (aud_enabled()) /* --mute opens no OS audio device */
    {
        /* Ask for 48000, which most devices give and which keeps the
         * resampler's ratio near one. Sokol writes back what it actually
         * got, and the machine resamples from its own rate to that,
         * whatever it is. */
        saudio_setup(&(saudio_desc){
            .sample_rate = 48000,
            .num_channels = 2,
            .buffer_frames = 512, /* the device's own latency: 10.7 ms, not sokol's 43 */
            .stream_cb = stream_cb,
            .logger.func = slog_func,
        });
        aud_set_sink_rate((uint32_t)saudio_sample_rate());
    }
    gfx_setup();
    host_window_init(); /* Android stands up its text overlay; no-op elsewhere */
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init();
#endif
}

void app_frame(void)
{
    const uint64_t now = os_mono_ns();
    /* The display paces us when the present held the thread; when a frame
     * ate the whole period (past 240 Hz it does), the callback before knew. */
    paced = !returned_ns || now - returned_ns >= PRESENT_WAIT_NS || (paced && ran_frame);
    uint64_t dt = entered_ns ? now - entered_ns : FRAME_NS;
    entered_ns = now;
    /* A 59.94 or 60 Hz display is one frame per callback, not a beat. */
    if (paced && dt > FRAME_NS - FRAME_NS / 10 && dt < FRAME_NS + FRAME_NS / 10)
        dt = FRAME_NS;
    if (dt > 3 * FRAME_NS) /* a stall is forgiven, not replayed */
        dt = 3 * FRAME_NS;
    debt_ns += dt;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        dap_pump(); /* apply queued DAP requests before running this frame */
        if (dap_quit_requested()) /* the DAP client disconnected */
            sapp_request_quit();
    }
#endif
#ifdef RP6502_PAD_HOST
    /* Once per presented frame: sokol has delivered this frame's key and pointer
     * events before frame_cb, so reading the gamepads now gives them the same age as
     * every other input. */
    gamepad_input_task();
#endif

    /* A held machine is paced by nothing, and the present's rate is more
     * than the workbench needs. */
    if ((!paced || held) && debt_ns < FRAME_NS)
    {
        os_sleep_ns(FRAME_NS - debt_ns);
        const uint64_t woke = os_mono_ns();
        debt_ns += woke - entered_ns;
        entered_ns = woke;
    }
    ran_frame = held = false;
    const uint64_t t0 = os_mono_ns();
    while (debt_ns >= FRAME_NS)
    {
        if (!vga_run_frame()) /* held by a debugger: owed nothing on resume */
        {
            debt_ns = 0;
            held = true;
            break;
        }
        debt_ns -= FRAME_NS;
        ran_frame = true;
    }
    machine_ns += os_mono_ns() - t0;

    /* Reflect the run state in the title so the user knows the run is done (exec
     * un-halts within a frame, so this only trips on a real exit), and close the
     * window if asked, so a launcher can run a ROM and return. */
    /* Release a captured mouse if the program gave up the device (exec'd away,
     * unmapped) or mapped the absolute tablet (which never captures); then refresh
     * the title (run state + capture hint). */
    if (sapp_mouse_locked() && (!mouse_is_mapped() || tablet_is_mapped()))
        sapp_lock_mouse(false);
    update_title();
    /* A host overlay (the Android ROM menu, the desktop no-ROM prompt) holds the
     * CPU with no program yet, so a halt there isn't a program exit — don't quit. */
    if (!resb_running() && app.exit_on_halt && !host_window_menu_active())
        sapp_request_quit();

    /* EMU_BENCH_MS=N: run N ms then report the achieved VGA-frame rate (should
     * be ~60 Hz regardless of the host display) and quit. */
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
        sapp_request_quit();
    }

    gfx_canvas_changed();

#ifdef EMU_WITH_DEBUGGER
    /* Build the debugger windows first (between ImGui new-frame and render) so the
     * dockspace central-node rect is known before the canvas viewport below is
     * computed from it. */
    if (dbg_is_active())
    {
        /* dpi_scale 1.0: render the overlay at native resolution so the 13px
         * bitmap font lands 1:1 (crisp) instead of being magnified/blurred. */
        dbgui_new_frame(sapp_width(), sapp_height(), (double)dt / 1e9, 1.0f);
        dbgui_draw();
    }
#endif

    /* After dbgui_draw so ImGui's hovered-item cursor for this frame is known;
     * input_update_cursor is the sole cursor writer (simgui's is disabled). */
    input_update_cursor();

    /* Stamped before the upload: on Mesa DRI3 the wait for the display is the
     * next GL write, not the swap. */
    returned_ns = os_mono_ns();
    gfx_upload(ran_frame);
    gfx_begin_pass();
    gfx_blit();
    host_window_menu_draw(); /* Android ROM menu overlay; no-op elsewhere */
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_render(); /* ImGui draws on top of the canvas, same swapchain pass */
#endif
    gfx_end_pass();
}

bool app_boot_rom(const char *path)
{
#ifdef EMU_WITH_DEBUGGER
    if (dap_is_active())
        return false; /* a DAP client owns the run state; a plain --debug session
                       * lets you drop a ROM to boot it */
#endif
    /* The host hands a UTF-8 path; everything below the entry is guest OEM.
     * A lossy conversion can never open (0x7F substitutions name no file), so
     * refuse it here, before the machine is touched — otherwise the rom_load
     * failure below would halt the running program over a bad filename. */
    size_t osz = strlen(path) + 1; /* oem_from_utf8 contracts */
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
    /* Screen the file before proc_boot stops the machine, so an accidental
     * drop leaves the running program alone -- the loader would refuse it
     * too, but only after the program was already gone. Through an ordinary
     * descriptor, not fs_rom_open: that one is the outgoing program's, held
     * for its assets, and taking it would leave a refused drop with a program
     * still running and nothing to read its assets out of. rom_load claims it
     * a moment later anyway. */
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
    vtkeys_paste_cancel(); /* the new program must not receive an old paste */
    /* A dropped ROM is a program change (stop + load + run), not a machine reboot:
     * the code page / PHI2 ride through from the previous program, like an exec. */
    /* Committed here rather than left for the next frame: the load below
     * writes the RAM the outgoing program was running out of. */
    bool ok = proc_boot(oem, 0, NULL, PROC_UNCHAIN);
    free(oem);
    if (!ok)
        return false; /* RAM may be part-written; stays stopped */
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
    /* Docs link on the drop-a-ROM prompt: pointer cursor over it, open on click.
     * prompt_url is only a real box while that prompt is the active overlay. */
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
        return; /* the debug UI consumed this event */
#endif
    input_event(e);
}

/* The process exit code after entry_run returns. A ROM that halted the app
 * outside debug mode (exit_on_halt, i.e. !--debug/--dap) owns the code; a manual
 * window close, or debug mode where the DAP client carries the code instead,
 * stays 0. */
int app_exit_code(void)
{
    return (app.exit_on_halt && !resb_running()) ? proc_get_exit_code() : 0;
}

void app_cleanup(void)
{
#ifdef RP6502_PAD_HOST
    gamepad_input_stop(); /* the window is going; let go of the host's controllers */
#endif
    if (aud_enabled())
        saudio_shutdown();
#ifdef EMU_WITH_DEBUGGER
    dap_stop(); /* notify any attached DAP client before the window goes away */
    if (dbg_is_active())
        dbgui_discard();
#endif
    gfx_shutdown();
    sg_shutdown();
}

/* A window presents whole frames, so the beam keeps frame time with the
 * wall. The batch and script paths never call this and so never wait. */
void app_prepare(uint32_t *fb, double scale, bool have_scale,
                         bool exit_on_halt, int *out_w, int *out_h)
{
    /* Clamp to a sane range; the !(>=) form also maps NaN (atof of garbage) to
     * the floor, and the upper bound keeps win_h*scale in int range. */
    if (!(scale >= 0.1))
        scale = 0.1;
    if (scale > 64.0)
        scale = 64.0;
    app.exit_on_halt = exit_on_halt;
    vga_set_framebuffer(fb); /* what the window presents is what vga renders into */
    gfx_prepare(fb, scale, have_scale, out_w, out_h);
}
