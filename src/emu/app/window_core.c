/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host-neutral window core: the render/frame/present pipeline shared by
 * every windowed host. It carries no platform #ifdefs (only the EMU_WITH_DEBUGGER
 * feature gate). Anything that differs per host — the WM seam,
 * the entry point, the Android overlay — is a host_window_* hook (window_core.h)
 * implemented in host/<os>/window.c.
 */

#include "emu/app/window.h"
#include "emu/app/window_core.h"
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
#include "emu/dbg/dbgui.h"
#include "emu/dbg/dap.h"
#endif
#include "emu/app/icon.h"
#include "emu/app/input.h"
#include "emu/emu/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/emu/pro.h"
#include "ria/api/oem.h"
#include "emu/hid/mou.h"
#include "emu/hid/tab.h"
#include "emu/emu/rom.h"
#include "emu/sys/cpu.h"
#include "emu/main.h"
#include "emu/sys/sys.h"
#include "emu/host/host.h"
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
 * hides it (the ROM draws its own), otherwise show that shape. This is the sole
 * cursor writer (simgui's own control is disabled), run every frame so a ROM
 * cursor change or a debugger panel-hover change is reflected promptly. Over a
 * debugger panel ImGui owns the shape, applied via dbgui_mouse_cursor. */
static void update_cursor(void)
{
    static bool had_tablet;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_wants_mouse())
    {
        /* Over a debugger panel ImGui owns the shape; apply it (simgui no longer
         * does) and keep the pointer visible over any TAB_CURSOR_OFF hide. */
        sapp_set_mouse_cursor((sapp_mouse_cursor)dbgui_mouse_cursor());
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
    if (aud_enabled()) /* --mute opens no OS audio device */
        saudio_setup(&(saudio_desc){
            .num_channels = 2,
            .logger.func = slog_func,
        });
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
            sys_run_frame_norender(); /* catch-up frame: CPU/timing only, no pixels */
        else
            sys_run_frame(); /* the frame we'll present: render it */
        done++;
    }

    if (saudio_isvalid()) /* --mute opens no device; skip the resample+push */
        aud_pump(saudio_sample_rate(), saudio_push);
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
    /* A host overlay (the Android ROM menu, the desktop no-ROM prompt) holds the
     * CPU with no program yet, so a halt there isn't a program exit — don't quit. */
    if (cpu_halted() && app.exit_on_halt && !host_window_menu_active())
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
                    sys_frame_count(), bench_total,
                    (double)sys_frame_count() / bench_total);
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

    /* After dbgui_draw so ImGui's hovered-item cursor for this frame is known;
     * update_cursor is the sole cursor writer (simgui's control is disabled). */
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
    if (dap_is_active())
        return false; /* a DAP client owns the run state; a plain --debug session
                       * lets you drop a ROM to boot it */
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

/* One convex filled rounded rect as a center-fan of triangles (sokol_gl, in the
 * current pass, window-pixel coords per the ortho set in window_core_draw_prompt). */
static void prompt_round_rect(float x, float y, float w, float h, float rad,
                              uint8_t r, uint8_t g, uint8_t b)
{
    if (rad < 0.0f)
        rad = 0.0f;
    if (rad > w * 0.5f)
        rad = w * 0.5f;
    if (rad > h * 0.5f)
        rad = h * 0.5f;
    const float pi = 3.14159265f;
    enum { seg = 6 }; /* points per corner arc */
    const float ccx[4] = {x + rad, x + w - rad, x + w - rad, x + rad};
    const float ccy[4] = {y + rad, y + rad, y + h - rad, y + h - rad};
    const float a0[4] = {pi, 1.5f * pi, 2.0f * pi, 2.5f * pi};
    float vx[4 * (seg + 1)], vy[4 * (seg + 1)];
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int s = 0; s <= seg; s++)
        {
            float a = a0[c] + 0.5f * pi * (float)s / (float)seg;
            vx[n] = ccx[c] + rad * cosf(a);
            vy[n] = ccy[c] + rad * sinf(a);
            n++;
        }
    float mx = x + w * 0.5f, my = y + h * 0.5f;
    sgl_begin_triangles();
    sgl_c3b(r, g, b);
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        sgl_v2f(mx, my);
        sgl_v2f(vx[i], vy[i]);
        sgl_v2f(vx[j], vy[j]);
    }
    sgl_end();
}

/* Stroke the rounded-rect outline as a heavy dashed line: walk the perimeter
 * (4 edges + 4 corner arcs) as a fine closed polyline and emit a thick quad for
 * each sample segment whose midpoint falls in a dash (not a gap) phase. */
static void prompt_dashed_border(float x, float y, float w, float h, float rad,
                                 float thick, float dash, float gap,
                                 uint8_t r, uint8_t g, uint8_t b)
{
    if (rad < 0.0f)
        rad = 0.0f;
    if (rad > w * 0.5f)
        rad = w * 0.5f;
    if (rad > h * 0.5f)
        rad = h * 0.5f;
    const float pi = 3.14159265f;
    float perim = 2.0f * ((w - 2.0f * rad) + (h - 2.0f * rad)) + 2.0f * pi * rad;
    float step = perim / 1400.0f;
    if (step < 2.5f)
        step = 2.5f;

    /* edges (from->to) and corner arcs (center, start angle), interleaved
     * clockwise from the top edge; y is down. */
    const float ex0[4] = {x + rad, x + w, x + w - rad, x};
    const float ey0[4] = {y, y + rad, y + h, y + h - rad};
    const float ex1[4] = {x + w - rad, x + w, x + rad, x};
    const float ey1[4] = {y, y + h - rad, y + h, y + rad};
    const float acx[4] = {x + w - rad, x + w - rad, x + rad, x + rad};
    const float acy[4] = {y + rad, y + h - rad, y + h - rad, y + rad};
    const float aa0[4] = {1.5f * pi, 0.0f, 0.5f * pi, pi};

    static float px[1800], py[1800];
    int n = 0;
    for (int p = 0; p < 4; p++)
    {
        float L = hypotf(ex1[p] - ex0[p], ey1[p] - ey0[p]);
        int ns = (int)(L / step);
        if (ns < 1)
            ns = 1;
        for (int k = 0; k < ns && n < 1800; k++)
        {
            float t = (float)k / (float)ns;
            px[n] = ex0[p] + (ex1[p] - ex0[p]) * t;
            py[n] = ey0[p] + (ey1[p] - ey0[p]) * t;
            n++;
        }
        int as = (int)(rad * 0.5f * pi / step);
        if (as < 1)
            as = 1;
        for (int k = 0; k < as && n < 1800; k++)
        {
            float a = aa0[p] + 0.5f * pi * (float)k / (float)as;
            px[n] = acx[p] + rad * cosf(a);
            py[n] = acy[p] + rad * sinf(a);
            n++;
        }
    }

    float thalf = thick * 0.5f, acc = 0.0f, period = dash + gap;
    sgl_begin_triangles();
    sgl_c3b(r, g, b);
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        float ax = px[i], ay = py[i], bx = px[j], by = py[j];
        float dx = bx - ax, dy = by - ay, len = hypotf(dx, dy);
        if (len < 1e-4f)
            continue;
        if (fmodf(acc + len * 0.5f, period) < dash)
        {
            float nx = -dy / len * thalf, ny = dx / len * thalf;
            sgl_v2f(ax + nx, ay + ny);
            sgl_v2f(bx + nx, by + ny);
            sgl_v2f(bx - nx, by - ny);
            sgl_v2f(ax + nx, ay + ny);
            sgl_v2f(bx - nx, by - ny);
            sgl_v2f(ax - nx, ay - ny);
        }
        acc += len;
    }
    sgl_end();
}

/* GPU resources for the prompt masthead icon, created once in
 * window_core_prompt_setup (live for the app lifetime, like sgl/sdtx). */
static struct
{
    sg_image img;
    sg_view view;
    sg_sampler smp;
    sgl_pipeline pip; /* alpha blend; the default sgl pipeline is opaque */
} prompt_icon;

/* Docs link under the prompt bubble, and its on-screen hit box in framebuffer px
 * (set each frame by window_core_draw_prompt) so a click can open it. */
static const char PROMPT_DOCS_URL[] = "https://picocomputer.github.io/"; /* opened on click */
static const char PROMPT_DOCS_TEXT[] = "picocomputer.github.io";         /* shown on screen */
static struct
{
    float x, y, w, h;
} prompt_url;

void window_core_prompt_setup(void)
{
    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_c64(),
        .logger.func = slog_func,
    });
    sgl_setup(&(sgl_desc_t){
        .max_vertices = 16384, /* the dashed border strokes many thick quads */
        .max_commands = 64,
        .color_format = (sg_pixel_format)sapp_color_format(),
        .depth_format = (sg_pixel_format)sapp_depth_format(),
        .sample_count = sapp_sample_count(),
        .logger.func = slog_func,
    });

    const sapp_image_desc *ico = &icon_desc()->images[2]; /* 64x64 */
    prompt_icon.img = sg_make_image(&(sg_image_desc){
        .width = ico->width,
        .height = ico->height,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = {.ptr = ico->pixels.ptr, .size = ico->pixels.size},
    });
    prompt_icon.view = sg_make_view(&(sg_view_desc){.texture.image = prompt_icon.img});
    prompt_icon.smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    prompt_icon.pip = sgl_make_pipeline(&(sg_pipeline_desc){
        .colors[0].blend = {
            .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_ONE,
            .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        },
    });
}

/* Textured quad for the masthead icon: a blend-enabled sgl pipeline over the
 * opaque default so the icon's transparent margin composites cleanly. Emitted
 * into the same sgl recording as the card, before sgl_draw. */
static void prompt_icon_draw(float x, float y, float sz)
{
    sgl_load_pipeline(prompt_icon.pip);
    sgl_enable_texture();
    sgl_texture(prompt_icon.view, prompt_icon.smp);
    sgl_begin_quads();
    sgl_c3b(255, 255, 255);
    sgl_v2f_t2f(x, y, 0.0f, 0.0f);
    sgl_v2f_t2f(x + sz, y, 1.0f, 0.0f);
    sgl_v2f_t2f(x + sz, y + sz, 1.0f, 1.0f);
    sgl_v2f_t2f(x, y + sz, 0.0f, 1.0f);
    sgl_end();
    sgl_disable_texture();
    sgl_load_default_pipeline();
}

/* Accumulate one sdtx line at an arbitrary glyph height (px); x_px/y_px is its
 * top-left. Sets its own canvas so the glyph size is independent of the bubble's
 * grid (sdtx bakes each glyph's position at emit time). The caller flushes with a
 * single sdtx_draw. */
static void prompt_text_line(const char *s, float gh, float x_px, float y_px,
                             float w, float h, const uint8_t col[3])
{
    float tcols = w / gh; /* canvas columns so each cell is gh window px */
    sdtx_canvas(tcols * 8.0f, tcols * 8.0f * h / w);
    sdtx_origin(0.0f, 0.0f);
    sdtx_color3b(col[0], col[1], col[2]);
    sdtx_pos(x_px / gh, y_px / gh);
    sdtx_puts(s);
}

void window_core_draw_prompt(const char *line1, const char *line2)
{
    float w = sapp_widthf(), h = sapp_heightf();
    if (w < 1.0f || h < 1.0f)
        return;

    const uint8_t ink[3] = {0xc2, 0xca, 0xd6};       /* dashes + text (soft light) */
    const uint8_t paper[3] = {0x26, 0x2b, 0x35};     /* dark card fill */
    const uint8_t title_col[3] = {0xe8, 0xec, 0xf4}; /* masthead + docs URL (bright) */

    /* Lay the two lines out on a 40-column grid mapped to the window; a square
     * glyph keeps the box and text proportional at any window aspect. */
    const int cols = 40;
    float glyph = w / (float)cols; /* window px per character cell */
    int len1 = (int)strlen(line1), len2 = (int)strlen(line2);
    int wide = len1 > len2 ? len1 : len2;

    float row_mid = (float)cols * 0.5f * h / w; /* grid row at the window center */
    float row1 = row_mid - 1.15f, row2 = row_mid + 0.15f; /* two centered lines */

    float pad_x = glyph * 2.0f, pad_y = glyph * 1.4f;
    float bw = wide * glyph + 2.0f * pad_x;
    float bh = (row2 + 1.0f - row1) * glyph + 2.0f * pad_y;
    float bx = (w - bw) * 0.5f, by = (h - bh) * 0.5f;
    float border = glyph * 0.42f; /* heavy */
    float rad = glyph * 1.3f;

    /* Masthead (icon + title) centered above the card; docs URL centered below.
     * All in window px on the same y-down grid as the card. */
    const char *emu_title = "RP6502-EMU";
    const char *docs_url = PROMPT_DOCS_TEXT;
    float icon_sz = glyph * 4.0f;   /* native 64px at a 640-wide window */
    float title_gh = glyph * 2.2f;  /* masthead title glyph height */
    float it_gap = glyph * 0.6f;    /* icon-to-title gap */
    float gap = glyph * 1.3f;       /* card-to-masthead spacing */
    float mast_w = icon_sz + it_gap + (float)strlen(emu_title) * title_gh;
    float mast_x = (w - mast_w) * 0.5f;
    float mast_top = by - gap - icon_sz;
    float title_x = mast_x + icon_sz + it_gap;
    float title_y = mast_top + (icon_sz - title_gh) * 0.5f;
    float url_gh = glyph;
    float url_w = (float)strlen(docs_url) * url_gh;
    float url_x = (w - url_w) * 0.5f;
    float url_y = by + bh + gap * 1.7f; /* sit a little below the card */
    prompt_url.x = url_x;
    prompt_url.y = url_y;
    prompt_url.w = url_w;
    prompt_url.h = url_gh;

    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f); /* top-left origin, y down, px units */
    prompt_round_rect(bx, by, bw, bh, rad, paper[0], paper[1], paper[2]);
    prompt_dashed_border(bx + border * 0.5f, by + border * 0.5f, bw - border,
                         bh - border, rad - border * 0.5f, border,
                         glyph * 1.0f, glyph * 0.7f, ink[0], ink[1], ink[2]);
    prompt_icon_draw(mast_x, mast_top, icon_sz);
    sgl_draw();

    /* Lines over the card, in the dash color, each centered on the same grid. */
    sdtx_canvas((float)cols * 8.0f, (float)cols * 8.0f * h / w);
    sdtx_origin(0.0f, 0.0f);
    sdtx_color3b(ink[0], ink[1], ink[2]);
    sdtx_pos((float)(cols - len1) * 0.5f, row1);
    sdtx_puts(line1);
    sdtx_pos((float)(cols - len2) * 0.5f, row2);
    sdtx_puts(line2);

    /* Accumulate the masthead title and docs URL into the same sdtx buffer, then
     * flush once: sdtx uploads its vertices on the first sdtx_draw of the frame
     * only, so a draw between blocks would drop everything emitted after it. */
    prompt_text_line(emu_title, title_gh, title_x, title_y, w, h, title_col);
    prompt_text_line(docs_url, url_gh, url_x, url_y, w, h, title_col);
    sdtx_draw();
}

static bool prompt_url_hit(float x, float y)
{
    return x >= prompt_url.x && x < prompt_url.x + prompt_url.w &&
           y >= prompt_url.y && y < prompt_url.y + prompt_url.h;
}

void window_core_event(const sapp_event *e)
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
            host_window_open_url(PROMPT_DOCS_URL);
            return;
        }
    }
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_handle_event(e))
        return; /* the debug UI consumed this event */
#endif
    input_event(e);
}

/* The process exit code after window_run returns. A ROM that halted the app
 * outside debug mode (exit_on_halt, i.e. !--debug/--dap) owns the code; a manual
 * window close, or debug mode where the DAP client carries the code instead,
 * stays 0. */
int window_core_exit_code(void)
{
    return (app.exit_on_halt && cpu_halted()) ? pro_get_exit_code() : 0;
}

void window_core_cleanup(void)
{
    if (aud_enabled())
        saudio_shutdown();
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
