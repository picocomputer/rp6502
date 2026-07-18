/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host-neutral window core: the render/frame/present pipeline shared by
 * every windowed host. It carries no platform #ifdefs (only EMU_WITH_DEBUGGER /
 * EMU_WITH_AUDIO feature gates). Anything that differs per host — the WM seam,
 * the entry point, the Android overlay — is a host_window_* hook (window_core.h)
 * implemented in host/<os>/window.c.
 */

#include "emu/app/window.h"
#include "emu/app/window_core.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "util/sokol_framebuffer.h"
#include "util/sokol_letterbox.h"
#ifdef EMU_WITH_AUDIO
#include "sokol_audio.h"
#include "emu/app/audio_out.h"
#endif
#ifdef EMU_WITH_DEBUGGER
#include "emu/dbg/dbgui.h"
#include "emu/dbg/dap.h"
#endif
#include "emu/app/icon.h"
#include "emu/app/input.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/api/pro.h"
#include "api/oem.h"
#include "emu/hid/mou.h"
#include "emu/hid/tab.h"
#include "emu/host/rom.h"
#include "emu/sys/cpu.h"
#include "emu/main.h"
#include "emu/plat.h"
#include "emu/sys/vga.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct
{
    double scale;     /* requested window scale (may be fractional) */
    int tex_w, tex_h; /* last seen canvas native size (sfb tracks it lazily) */
    bool exit_on_halt; /* close the window when the program stops */
    bool vsync;        /* false: pace the loop to 60 Hz in software (sleep_until_ns) */
    sfb_framebuffer sfb;          /* presents fb: upload, prescale, letterboxed blit */
    window_scale_filter_t filter; /* 0 == NEAREST default */
    float bg_r, bg_g, bg_b; /* letterbox/pillarbox fill (default black) */
    int title_variant;     /* last window-title state (running/stopped/mouse) */
    uint32_t *fb;          /* caller's framebuffer: vga renders in, frame_cb uploads */
} app;

/* Max emulated frames the pacer will run in one callback before dropping the
 * backlog (no fast-forward after a stall). Also the deepest frame-skip on a
 * sub-60 display: 6 supports presents down to ~10 Hz, caps catch-up to ~100 ms. */
#define WINDOW_MAX_SKIP 6

void window_set_bgcolor(uint8_t r, uint8_t g, uint8_t b)
{
    app.bg_r = r / 255.0f;
    app.bg_g = g / 255.0f;
    app.bg_b = b / 255.0f;
}

void window_set_scale_filter(window_scale_filter_t filter) { app.filter = filter; }

/* Sharp-bilinear prescale factor: the largest integer by which the canvas
 * still fits the window (floor per axis, take the smaller for square pixels),
 * clamped to [1, WINDOW_PRESCALE_MAX]. sfb's final LINEAR pass absorbs the
 * leftover fractional scale. The cap bounds VRAM (sfb has no cap of its own;
 * a maximized window on a tiny canvas can't allocate an enormous target); 1
 * keeps a window-smaller-than-canvas from hitting a zero-size target. */
#define WINDOW_PRESCALE_MAX 6 /* 640*6 x 480*6 RGBA8 ~= 42 MB ceiling */

static int sharp_prescale(int cw, int ch, int aw, int ah)
{
    int fx = aw / cw, fy = ah / ch;
    int f = fx < fy ? fx : fy;
    if (f < 1)
        f = 1;
    if (f > WINDOW_PRESCALE_MAX)
        f = WINDOW_PRESCALE_MAX;
    return f;
}

/* Is the debugger overlay (menu bar + dockspace) on this window? The overlay
 * owns the layout then: panels dock beside the canvas and the central node
 * letterboxes it, so the window resizes freely — no WM aspect hint, no width
 * re-fit — and its size persists per debug session instead of tracking --scale. */
static bool overlay_active(void)
{
#ifdef EMU_WITH_DEBUGGER
    return dbg_is_active();
#else
    return false;
#endif
}

/* Framebuffer pixels reserved at the top of the window for the debugger's menu
 * bar, so the canvas is laid out BELOW the menu instead of under it (0 when the
 * overlay is inactive). The overlay renders 1:1 (dbgui gets dpi_scale 1.0), so the
 * reported bar height is already framebuffer pixels; never reserve the whole window. */
static int top_reserved_px(void)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        int px = (int)(dbgui_menu_height() + 0.5f);
        if (px < 0)
            px = 0;
        if (px > sapp_height() - 1)
            px = sapp_height() - 1;
        return px;
    }
#endif
    return 0;
}

/* Window width that gives the canvas its square-pixel aspect (cw:ch) at height
 * h; long math avoids overflow on tall canvases. */
static int aspect_width(int h, int cw, int ch)
{
    return (int)((long)h * cw / ch);
}

/* Canvas height in framebuffer pixels for a requested --scale (VGA_MAX_HEIGHT
 * rows at scale, rounded); inverse of window_get_scale. */
static int scaled_canvas_height(double scale)
{
    return (int)(VGA_MAX_HEIGHT * scale + 0.5);
}

void window_set_scale(double scale)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    /* scaled_canvas_height is logical canvas px; host_window_resize wants
     * framebuffer (== physical) px, so scale by the DPI factor (1.0 unless
     * high_dpi is on). top_reserved_px() is already framebuffer px. */
    int h = (int)(scaled_canvas_height(scale) * sapp_dpi_scale() + 0.5f);
    int w = aspect_width(h, cw, ch);
    host_window_resize(w, h + top_reserved_px());
}

double window_get_scale(void)
{
    if (!sapp_isvalid())
        return 0.0;
    /* sapp_height() and top_reserved_px() are framebuffer (physical) px; divide
     * out the DPI factor so the reported scale stays in logical --scale units. */
    return (double)(sapp_height() - top_reserved_px()) / (VGA_MAX_HEIGHT * sapp_dpi_scale());
}

/* The framebuffer-pixel rect (x,y from top-left, w,h) the emulated canvas draws
 * into: the dockspace central node when the debugger overlay is up (so docked
 * panels take space beside the screen, not over it), else the whole window below
 * the menu-bar strip. */
static void canvas_region(int *x, int *y, int *w, int *h)
{
    int top = top_reserved_px();
    int rx = 0, ry = top, rw = sapp_width(), rh = sapp_height() - top;
#ifdef EMU_WITH_DEBUGGER
    int cx, cy, cw, ch;
    if (dbg_is_active() && dbgui_canvas_rect(&cx, &cy, &cw, &ch))
    {
        rx = cx;
        ry = cy;
        rw = cw;
        rh = ch;
    }
#endif
    if (rw < 1)
        rw = 1;
    if (rh < 1)
        rh = 1;
    *x = rx;
    *y = ry;
    *w = rw;
    *h = rh;
}

/* The aspect-fit viewport the canvas draws into, centered within canvas_region
 * (expressed to slbx as borders against the full framebuffer). Computed live on
 * every call — events arrive before the first frame and across dock/menu
 * transitions, so a cached copy would be stale exactly when it matters; this
 * function is the single source of truth for the render pass and the input
 * mappers below. */
static slbx_viewport canvas_viewport(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    int rx, ry, rw, rh;
    canvas_region(&rx, &ry, &rw, &rh);
    return slbx_letterbox(sapp_width(), sapp_height(),
                          &(slbx_letterbox_desc){
                              .content_aspect_ratio = (float)cw / (float)ch,
                              .border = {
                                  .left = rx,
                                  .right = sapp_width() - (rx + rw),
                                  .top = ry,
                                  .bottom = sapp_height() - (ry + rh),
                              },
                          });
}

/* On-screen pixels per canvas pixel (the aspect-fit scale). Host mouse motion is
 * divided by this to get canvas-space motion, so pointer speed doesn't change
 * with the window size. */
float window_canvas_scale(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    slbx_viewport vp = canvas_viewport();
    return (float)vp.width / cw;
}

bool window_canvas_from_fb(float px, float py, int *cx, int *cy)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    slbx_viewport vp = canvas_viewport();
    if (vp.width < 1 || vp.height < 1)
    {
        *cx = *cy = 0;
        return false;
    }
    float fx = (px - vp.x) * cw / vp.width;
    float fy = (py - vp.y) * ch / vp.height;
    bool inside = fx >= 0.0f && fx < cw && fy >= 0.0f && fy < ch;
    int ix = (int)fx, iy = (int)fy;
    if (ix < 0)
        ix = 0;
    else if (ix > cw - 1)
        ix = cw - 1;
    if (iy < 0)
        iy = 0;
    else if (iy > ch - 1)
        iy = ch - 1;
    *cx = ix;
    *cy = iy;
    return inside;
}

/* Map the ROM's tablet control byte to a sokol system cursor. */
static sapp_mouse_cursor tab_cursor_to_sokol(uint8_t shape)
{
    switch (shape)
    {
    case TAB_CURSOR_ARROW: return SAPP_MOUSECURSOR_ARROW;
    case TAB_CURSOR_CROSSHAIR: return SAPP_MOUSECURSOR_CROSSHAIR;
    case TAB_CURSOR_IBEAM: return SAPP_MOUSECURSOR_IBEAM;
    case TAB_CURSOR_HAND: return SAPP_MOUSECURSOR_POINTING_HAND;
    case TAB_CURSOR_RESIZE_EW: return SAPP_MOUSECURSOR_RESIZE_EW;
    case TAB_CURSOR_RESIZE_NS: return SAPP_MOUSECURSOR_RESIZE_NS;
    default: return SAPP_MOUSECURSOR_DEFAULT;
    }
}

/* Whether the host pointer is over the drawn canvas, set by the input layer. The
 * tablet only owns the host cursor while true; in the letterbox (or a debugger
 * panel, handled below) the system cursor shows. Defaults true so a freshly
 * mapped tablet shows its cursor before the first motion. */
static bool pointer_on_canvas = true;

void window_set_pointer_on_canvas(bool on)
{
    pointer_on_canvas = on;
}

/* Apply the tablet ROM's requested host cursor (control byte): TAB_CURSOR_OFF
 * hides it (the ROM draws its own), otherwise show that shape. Applied every
 * frame — the debugger's simgui also sets the cursor every frame, so a one-shot
 * would be overwritten. Over a debugger panel ImGui owns the shape, so we only
 * keep the pointer visible there. */
static void update_cursor(void)
{
    static bool had_tablet;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_wants_mouse())
    {
        /* Over a debugger panel ImGui owns the cursor shape; undo any
         * TAB_CURSOR_OFF hide so the panel is usable with a visible pointer. */
        sapp_show_mouse(true);
        return;
    }
#endif
    if (tab_is_mapped() && pointer_on_canvas)
    {
        had_tablet = true;
        int shape = tab_control();
        if (shape >= TAB_CURSOR_COUNT)
            shape = TAB_CURSOR_OFF;
        if (shape == TAB_CURSOR_OFF)
            sapp_show_mouse(false);
        else
        {
            sapp_set_mouse_cursor(tab_cursor_to_sokol(shape));
            sapp_show_mouse(true);
        }
    }
    else if (had_tablet)
    {
        had_tablet = false;
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_DEFAULT);
        sapp_show_mouse(true);
    }
}

/* Reflect run + mouse-capture state in the window title, only when it changes.
 * When a program has mapped the mouse, the title carries the capture hint. */
static void update_title(void)
{
    int v;
    const char *t;
    if (cpu_halted())
    {
        v = 1;
        t = "Picocomputer 6502 (stopped)";
    }
    else if (mou_is_mapped() && sapp_mouse_locked())
    {
        v = 3;
        t = "Picocomputer 6502  -  Esc releases mouse";
    }
    else if (mou_is_mapped() && !tab_is_mapped())
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

void window_core_init(void)
{
    sapp_set_icon(icon_desc());
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
#ifdef EMU_WITH_AUDIO
    if (aud_enabled()) /* --mute opens no OS audio device */
        saudio_setup(&(saudio_desc){
            .num_channels = 2,
            .logger.func = slog_func,
        });
#endif
    sfb_setup(&(sfb_desc){
        .logger.func = slog_func,
    });
    host_window_init(); /* Android stands up its text overlay; no-op elsewhere */
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    app.sfb = sfb_make_framebuffer(&(sfb_framebuffer_desc){
        .width = cw,
        .height = ch,
    });
    app.tex_w = cw;
    app.tex_h = ch;
    if (!overlay_active())
        host_window_set_aspect_hint(cw, ch);
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init();
#endif
}

void window_core_frame(void)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        dap_pump(); /* apply queued DAP requests before running this frame */
        if (dap_quit_requested()) /* the DAP client disconnected */
            sapp_request_quit();
    }
#endif
    /* Emulation is paced by an absolute monotonic clock: run exactly the number
     * of fixed-60 Hz frames real time owes us since start — independent of the
     * display refresh, so emulation speed is always correct. The deficit is
     * capped and dropped (no fast-forward after a stall). Presentation is
     * decoupled: only the LAST frame of a catch-up batch is rendered; the rest
     * skip per-scanline pixel work (most of the per-frame cost), so falling
     * behind stays cheap. The present clock is the vsync swap (vsync) or the
     * software sleep at the bottom (no-vsync). */
    double dt = sapp_frame_duration(); /* smoothed; only the EMU_BENCH_MS block uses it */
    static uint64_t start_ns, done;
    static bool started;
    if (!started)
    {
        started = true;
        start_ns = os_mono_ns();
    }
    uint64_t target = (os_mono_ns() - start_ns) * VGA_HZ / 1000000000ull;
    if (host_window_menu_active()) /* Android ROM menu: freeze emulation */
        done = target;
    uint64_t behind = target > done ? target - done : 0;
    if (behind > WINDOW_MAX_SKIP) /* hopelessly behind: drop the deficit, resync */
    {
        done += behind - WINDOW_MAX_SKIP;
        behind = WINDOW_MAX_SKIP;
    }
    for (uint64_t i = 0; i < behind; i++)
    {
        if (i + 1 < behind)
            main_run_frame_norender(); /* catch-up frame: CPU/timing only, no pixels */
        else
            main_run_frame(); /* the frame we'll present: render it */
        done++;
    }

#ifdef EMU_WITH_AUDIO
    audio_out_pump();
#endif
    input_paste_pump();

    /* Reflect the run state in the title so the user knows the run is done (exec
     * un-halts within a frame, so this only trips on a real exit), and close the
     * window if asked, so a launcher can run a ROM and return. */
    /* Release a captured mouse if the program gave up the device (exec'd away,
     * unmapped) or mapped the absolute tablet (which never captures); then refresh
     * the title (run state + capture hint). */
    if (sapp_mouse_locked() && (!mou_is_mapped() || tab_is_mapped()))
        sapp_lock_mouse(false);
    update_title();
    if (cpu_halted() && app.exit_on_halt)
        sapp_request_quit();

    /* EMU_BENCH_MS=N: run N ms then report the achieved VGA-frame rate (should
     * be ~60 Hz regardless of the host display) and quit. */
    static double bench_limit = -2.0, bench_total;
    if (bench_limit < -1.0)
    {
        const char *e = getenv("EMU_BENCH_MS");
        bench_limit = e ? atof(e) / 1000.0 : -1.0;
    }
    if (bench_limit > 0.0)
    {
        bench_total += dt;
        if (bench_total >= bench_limit)
        {
            fprintf(stderr, "EMU_BENCH: %lu VGA frames in %.3fs = %.1f Hz\n",
                    main_frame_count(), bench_total,
                    (double)main_frame_count() / bench_total);
            sapp_request_quit();
        }
    }

    int cw, ch;
    vga_canvas_size(&cw, &ch);
    if (cw != app.tex_w || ch != app.tex_h)
    {
        /* Before tex_w/tex_h update, note whether the window is still within <1px
         * of the OLD canvas aspect, i.e. the user hasn't resized it off-aspect. */
        int w = sapp_width(), h = sapp_height();
        double off = (double)w - (double)h * app.tex_w / app.tex_h;
        int at_aspect = off < 1.0 && off > -1.0;

        app.tex_w = cw;
        app.tex_h = ch;
        if (!overlay_active()) /* the debug workbench never tracks the canvas aspect */
        {
            /* Ask the WM to keep the new aspect on interactive resize. WSLg ignores
             * this (the quad below letterboxes instead); native X11/other WMs honor it. */
            host_window_set_aspect_hint(cw, ch);

            /* Re-fit the window width to the new aspect ONLY if it was still pristine;
             * a window the user has resized off-aspect is left alone (and letterboxed).
             * We don't poll-and-snap to enforce it: programmatic resizes are unreliable
             * under WSLg (it restores geometry and drops requests). Height is left
             * as-is; only the width tracks the aspect. */
            int new_w = aspect_width(h, cw, ch);
            if (at_aspect && new_w != w)
                host_window_resize(new_w, h);
        }
    }

#ifdef EMU_WITH_DEBUGGER
    /* Build the debugger windows first (between ImGui new-frame and render) so the
     * dockspace central-node rect is known before the canvas viewport below is
     * computed from it. */
    if (dbg_is_active())
    {
        /* dpi_scale 1.0: render the overlay at native resolution so the 13px
         * bitmap font lands 1:1 (crisp) instead of being magnified/blurred. */
        dbgui_new_frame(sapp_width(), sapp_height(), sapp_frame_duration(), 1.0f);
        dbgui_draw();
    }
#endif

    /* After the debugger set its cursor (simgui, every frame) so the tablet's
     * cursor wins over the canvas. */
    update_cursor();

    /* Aspect-preserving viewport fitted into the canvas region (the dockspace
     * central node when the debugger is up, else the whole window). The window
     * tracks the canvas aspect (RP6502 square pixels -> canvas aspect = display
     * aspect), so the viewport normally fills the region; if it is off-aspect
     * (the WM ignored the aspect hint, or mid-resize) it letterboxes/pillarboxes
     * against the clear so content never stretches. */
    slbx_viewport vp = canvas_viewport();
    int f = app.filter == WINDOW_FILTER_SHARP
                ? sharp_prescale(cw, ch, vp.width, vp.height)
                : 1;
    /* Lazy: recreates sfb's images only when the canvas or the sharp prescale
     * factor changed. cliprect must be spelled out — sfb_resize stores the raw
     * desc value, and a zeroed rect on a recreating resize makes a 0x0 image. */
    bool recreated = sfb_resize(app.sfb, &(sfb_resize_desc){
        .width = cw,
        .height = ch,
        .prescale = f,
        .cliprect = {0, 0, cw, ch},
    });

    /* Upload the new frame from the window's framebuffer, but only when one was
     * produced this callback; a duplicate present (behind == 0, e.g. a display
     * faster than 60 Hz) re-blits sfb's existing texture below without
     * re-uploading. A recreating resize must repopulate regardless. */
    static bool ever_uploaded;
    if (behind > 0 || recreated)
    {
        sfb_update(app.sfb, &(sfb_update_desc){
            .pixels = {.ptr = app.fb, .size = (size_t)cw * ch * sizeof(uint32_t)},
        });
        ever_uploaded = true;
    }

    sg_begin_pass(&(sg_pass){
        .action = {.colors[0] = {.load_action = SG_LOADACTION_CLEAR,
                                 .clear_value = {app.bg_r, app.bg_g, app.bg_b, 1}}},
        .swapchain = sglue_swapchain(),
    });
    /* Until the first frame has been uploaded sfb's texture is undefined; skip
     * the blit so the pass shows only the clear color. host_window_menu_active()
     * also suppresses the canvas while the Android ROM menu is up — its sdtx
     * overlay then draws with the pass's full-window viewport still in effect. */
    if (ever_uploaded && vp.width > 0 && vp.height > 0 && !host_window_menu_active())
    {
        sg_apply_viewport(vp.x, vp.y, vp.width, vp.height, true);
        if (app.filter == WINDOW_FILTER_NEAREST)
            sfb_render_ex(app.sfb, &(sfb_render_desc){.use_nearest_filter = true});
        else
            sfb_render(app.sfb);
    }
    host_window_menu_draw(); /* Android ROM menu overlay; no-op elsewhere */
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_render(); /* ImGui draws on top of the canvas, same swapchain pass */
#endif
    sg_end_pass();
    sg_commit();

    /* No-vsync: sokol's loop is uncapped, so pace it in software to when the NEXT
     * frame is due — start + (done+1)·period (absolute → no drift). Sleeping to
     * done·period would target a deadline already past and busy-loop. With vsync
     * the swap-block above already paces the loop. */
    if (!app.vsync)
        os_sleep_until_ns(start_ns + (done + 1) * (1000000000ull / VGA_HZ));
}

bool window_core_boot_rom(const char *path)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        return false; /* a DAP session owns the machine's run state */
#endif
    /* The host hands a UTF-8 path; everything below the entry is guest OEM.
     * A lossy conversion can never open (0x7F substitutions name no file), so
     * refuse it here, before the machine is touched — otherwise the rom_load
     * failure below would halt the running program over a bad filename. */
    char oem[4096], back[3 * 4096];
    if (oem_from_utf8(path, oem, sizeof oem) >= sizeof oem ||
        oem_to_utf8(oem, back, sizeof back) >= sizeof back)
    {
        fprintf(stderr, "rp6502-emu: dropped path too long\n");
        return false;
    }
    if (strcmp(path, back) != 0)
    {
        fprintf(stderr, "rp6502-emu: dropped path not representable in the OEM code page\n");
        return false;
    }
    /* Screen out not-a-ROM files before rom_load touches machine state, so an
     * accidental drop leaves the running program alone. rom_load repeats the
     * check after resolving the drive/:name spellings this open can't. */
    FILE *f = fs_fopen_rd(oem);
    if (f)
    {
        char magic[8];
        size_t got = fread(magic, 1, sizeof magic, f);
        fclose(f);
        if (got != sizeof magic || strncasecmp(magic, "#!RP6502", 8) != 0)
        {
            fprintf(stderr, "rp6502-emu: not a .rp6502 file (bad magic)\n");
            return false;
        }
    }
    input_paste_cancel(); /* the new program must not receive an old paste */
    /* A dropped ROM is a program change (stop + load + run), not a machine reboot:
     * the code page / PHI2 ride through from the previous program, like an exec. */
    main_stop(); /* tear down the outgoing program (cpu_stop halts it) */
    if (!rom_load(oem))
        return false; /* RAM may be part-written; stays halted from main_stop */
    pro_set_argv(oem, 0, NULL); /* like a CLI boot: the ROM's own path, no args */
    pro_set_launcher(false);    /* a drop breaks any launcher chain */
    main_run();
    return true;
}

void window_core_event(const sapp_event *e)
{
    if (e->type == SAPP_EVENTTYPE_FILES_DROPPED)
    {
        host_window_files_dropped();
        return;
    }
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_handle_event(e))
        return; /* the debug UI consumed this event */
#endif
    input_event(e);
}

void window_core_cleanup(void)
{
#ifdef EMU_WITH_AUDIO
    if (aud_enabled())
        saudio_shutdown();
#endif
#ifdef EMU_WITH_DEBUGGER
    dap_stop(); /* notify any attached DAP client before the window goes away */
    if (dbg_is_active())
        dbgui_discard();
#endif
    sfb_shutdown();
    sg_shutdown();
}

void window_core_prepare(uint32_t *fb, double scale, bool have_scale, bool vsync,
                         bool exit_on_halt, int *out_w, int *out_h)
{
    (void)have_scale;
    /* Clamp to a sane range; the !(>=) form also maps NaN (atof of garbage) to
     * the floor, and the upper bound keeps win_h*scale in int range. */
    if (!(scale >= 0.1))
        scale = 0.1;
    if (scale > 64.0)
        scale = 64.0;
    app.fb = fb;
    app.scale = scale;
    app.vsync = vsync;
    app.exit_on_halt = exit_on_halt;
    vga_set_framebuffer(fb); /* what the window presents is what vga renders into */
    /* Open at a fixed height with the width set to the canvas aspect (square
     * pixels: display aspect = cw/ch), so a 4:3 canvas opens 640x480 and a 16:9
     * canvas opens wider. The WM may restore a previous size instead; that's fine
     * — init_cb sets the aspect hint and the quad letterboxes either way. */
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    int canvas_h = scaled_canvas_height(app.scale);
    int win_w = aspect_width(canvas_h, cw, ch);
    int win_h = canvas_h;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        /* In debug mode the menu bar sits ABOVE the canvas, so open the window
         * taller by the bar's height; otherwise the canvas-aspect window squeezes
         * the VGA picture under the menu. Post-open resizes are unreliable (WSLg
         * drops them), so size it right up front with the pre-frame estimate. */
        win_h += (int)(dbgui_menu_bar_estimate() + 0.5f);
        /* Reopen at the last debug session's window size (persisted with the
         * layout); an explicit --scale asks for a specific size and wins. */
        int last_w, last_h;
        if (!have_scale && dbgui_window_size(&last_w, &last_h))
        {
            win_w = last_w;
            win_h = last_h;
        }
    }
#endif
    *out_w = win_w;
    *out_h = win_h;
}
