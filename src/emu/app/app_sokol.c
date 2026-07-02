/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windowed presentation via sokol_app + sokol_gl. Built only when
 * EMU_WITH_SOKOL is defined; otherwise a stub keeps the headless
 * (--screenshot) build self-contained.
 */

#include "emu/app/window.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/hid/kbd.h"
#include "emu/hid/mou.h"
#include "emu/mon/rom.h"
#include "emu/host/msc.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"

#ifndef EMU_WITH_SOKOL

#include <stdio.h>

int emu_run_window(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
{
    (void)fb;
    (void)scale;
    (void)have_scale;
    (void)vsync;
    (void)exit_on_halt;
    fprintf(stderr, "rp6502-emu: built without window support; use --screenshot\n");
    return 1;
}

void emu_set_bgcolor(uint8_t r, uint8_t g, uint8_t b) { (void)r, (void)g, (void)b; }

/* Headless renders at native resolution (no canvas->window scaling), so the
 * filter is genuinely a no-op here. */
void emu_set_scale_filter(emu_scale_filter_t filter) { (void)filter; }

#else

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "util/sokol_gl.h"
#ifdef EMU_WITH_AUDIO
#include "sokol_audio.h"
#include "emu/app/audio_out.h"
#endif
#ifdef EMU_WITH_DEBUGGER
#include "emu/dbg/dbgui.h"
#include "emu/dbg/dap.h"
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* frame limiter (clock_gettime/clock_nanosleep) */
#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__linux__)
/* The window tracks the canvas aspect two ways via the X11 handle sokol exposes:
 * a one-shot resize when the canvas changes, and a PAspect size hint that asks
 * the window manager to keep that aspect during interactive resizes (we don't
 * fight the WM by snapping the size ourselves; the quad letterboxes whenever the
 * window ends up off-aspect anyway). Forward-declare the few Xlib bits we need
 * rather than pulling in <X11/Xlib.h> and its macro soup (Window is an XID =
 * unsigned long; X11 is already linked for the GL backend). */
typedef struct _XDisplay Display;
typedef struct
{
    long flags;
    int x, y, width, height;
    int min_width, min_height, max_width, max_height;
    int width_inc, height_inc;
    struct { int x, y; } min_aspect, max_aspect;
    int base_width, base_height, win_gravity;
} XSizeHints;
#define X_PASPECT (1L << 7) /* XSizeHints PAspect flag */
extern int XResizeWindow(Display *, unsigned long, unsigned, unsigned);
extern void XSetWMNormalHints(Display *, unsigned long, XSizeHints *);
extern int XFlush(Display *);

static void resize_window(int w, int h)
{
    Display *dpy = (Display *)sapp_x11_get_display();
    unsigned long win = (unsigned long)(uintptr_t)sapp_x11_get_window();
    if (dpy && win)
    {
        XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
        XFlush(dpy);
    }
}

/* Ask the WM to constrain interactive resizes to the canvas aspect (cw:ch). */
static void set_aspect_hint(int cw, int ch)
{
    Display *dpy = (Display *)sapp_x11_get_display();
    unsigned long win = (unsigned long)(uintptr_t)sapp_x11_get_window();
    if (!dpy || !win)
        return;
    XSizeHints h;
    memset(&h, 0, sizeof h);
    h.flags = X_PASPECT;
    h.min_aspect.x = h.max_aspect.x = cw;
    h.min_aspect.y = h.max_aspect.y = ch;
    XSetWMNormalHints(dpy, win, &h);
    XFlush(dpy);
}
#else
static void resize_window(int w, int h) { (void)w, (void)h; }
static void set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
#endif

static struct
{
    double scale;     /* requested window scale (may be fractional) */
    int tex_w, tex_h; /* current texture size = canvas native size */
    bool exit_on_halt; /* close the window when the program stops */
    bool vsync;        /* false: pace the loop to 60 Hz in software (sleep_until_ns) */
    sg_image img;
    sg_view view;
    sg_sampler smp;        /* NEAREST: the canvas texture / prescale source */
    sg_sampler smp_linear; /* LINEAR: the final downscale blit (sharp/linear) */
    sgl_pipeline pip;      /* swapchain-format sgl pipeline */
    sgl_pipeline pip_off;  /* RGBA8/none/1 sgl pipeline for the offscreen pass */
    sgl_context off_ctx;   /* sgl context whose formats match the offscreen RT */
    sg_image rt_img;       /* EMU_FILTER_SHARP: offscreen integer-prescale target */
    sg_view rt_att;        /* its color-attachment view (render into) */
    sg_view rt_tex;        /* its texture view (sample in the final pass) */
    int rt_w, rt_h;        /* current offscreen size (0 = not created yet) */
    emu_scale_filter_t filter; /* 0 == NEAREST default */
    float bg_r, bg_g, bg_b; /* letterbox/pillarbox fill (default black) */
    int title_variant;     /* last window-title state (running/stopped/mouse) */
    uint8_t mouse_buttons; /* host mouse button bitmap while captured */
    uint32_t *fb;          /* caller's framebuffer: vga renders in, frame_cb uploads */
} app;

/* Mouse sensitivity: host motion is converted to canvas pixels (so it's
 * window-scale independent) then multiplied by this. 2 makes paint, whose
 * MOUSE_DIV halves it, track the physical mouse ~1:1 on screen. */
#define EMU_MOUSE_GAIN 2.0f

/* Max emulated frames the pacer will run in one callback before dropping the
 * backlog (no fast-forward after a stall). Also the deepest frame-skip on a
 * sub-60 display: 6 supports presents down to ~10 Hz, caps catch-up to ~100 ms. */
#define EMU_MAX_SKIP 6

void emu_set_bgcolor(uint8_t r, uint8_t g, uint8_t b)
{
    app.bg_r = r / 255.0f;
    app.bg_g = g / 255.0f;
    app.bg_b = b / 255.0f;
}

void emu_set_scale_filter(emu_scale_filter_t filter) { app.filter = filter; }

/* (Re)create the streaming texture at the canvas's native size. The canvas can
 * change at runtime (xreg_vga_canvas), so the texture follows it. */
static void resize_canvas_texture(int w, int h)
{
    if (app.img.id)
    {
        sg_destroy_view(app.view);
        sg_destroy_image(app.img);
    }
    app.img = sg_make_image(&(sg_image_desc){
        .usage.stream_update = true,
        .width = w,
        .height = h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
    });
    app.view = sg_make_view(&(sg_view_desc){
        .texture.image = app.img,
    });
    app.tex_w = w;
    app.tex_h = h;
}

/* (Re)create the offscreen prescale target at exactly w x h (an integer
 * multiple of the canvas). Used only by EMU_FILTER_SHARP. Views reference the
 * image, so they are destroyed first; the offscreen sgl context is format-only
 * and is not touched here. */
static void resize_render_target(int w, int h)
{
    if (app.rt_img.id)
    {
        sg_destroy_view(app.rt_tex);
        sg_destroy_view(app.rt_att);
        sg_destroy_image(app.rt_img);
    }
    app.rt_img = sg_make_image(&(sg_image_desc){
        .usage.color_attachment = true,
        .width = w,
        .height = h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .sample_count = 1,
    });
    app.rt_att = sg_make_view(&(sg_view_desc){
        .color_attachment.image = app.rt_img,
    });
    app.rt_tex = sg_make_view(&(sg_view_desc){
        .texture.image = app.rt_img,
    });
    app.rt_w = w;
    app.rt_h = h;
}

/* Sharp-bilinear prescale factor: the largest integer by which the canvas
 * still fits the window (floor per axis, take the smaller for square pixels),
 * clamped to [1, EMU_PRESCALE_MAX]. The final LINEAR pass absorbs the leftover
 * fractional scale. The cap bounds VRAM (a maximized window on a tiny canvas
 * can't allocate an enormous target); 1 keeps a window-smaller-than-canvas from
 * hitting a zero-size target (the final pass then downscales). */
#define EMU_PRESCALE_MAX 6 /* 640*6 x 480*6 RGBA8 ~= 42 MB ceiling */

static int sharp_prescale(int cw, int ch, int aw, int ah)
{
    int fx = aw / cw, fy = ah / ch;
    int f = fx < fy ? fx : fy;
    if (f < 1)
        f = 1;
    if (f > EMU_PRESCALE_MAX)
        f = EMU_PRESCALE_MAX;
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
 * overlay is inactive). dbgui reports the bar height in ImGui points; scale by
 * the DPI factor to get framebuffer pixels, and never reserve the whole window. */
static int top_reserved_px(void)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        int px = (int)(dbgui_menu_height() * sapp_dpi_scale() + 0.5f);
        if (px < 0)
            px = 0;
        if (px > sapp_height() - 1)
            px = sapp_height() - 1;
        return px;
    }
#endif
    return 0;
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

/* On-screen pixels per canvas pixel (the aspect-fit scale). Host mouse motion is
 * divided by this to get canvas-space motion, so pointer speed doesn't change
 * with the window size. The canvas occupies the window below the debugger menu
 * bar, so its height excludes that reserved strip. */
static float canvas_scale(void)
{
    int cw, ch;
    emu_canvas_size(&cw, &ch);
    int rx, ry, rw, rh;
    canvas_region(&rx, &ry, &rw, &rh);
    float sx = (float)rw / cw;
    float sy = (float)rh / ch;
    return (sx < sy ? sx : sy);
}

/* Reflect run + mouse-capture state in the window title, only when it changes.
 * When a program has mapped the mouse, the title carries the capture hint. */
static void update_title(void)
{
    int v;
    const char *t;
    if (emu_cpu_halted)
    {
        v = 1;
        t = "Picocomputer 6502 (stopped)";
    }
    else if (mou_is_mapped() && sapp_mouse_locked())
    {
        v = 3;
        t = "Picocomputer 6502  -  Esc releases mouse";
    }
    else if (mou_is_mapped())
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

static void init_cb(void)
{
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
#ifdef EMU_WITH_AUDIO
    if (emu_audio_enabled()) /* --mute opens no OS audio device */
        saudio_setup(&(saudio_desc){
            .num_channels = 2,
            .logger.func = slog_func,
        });
#endif
    sgl_setup(&(sgl_desc_t){
        .logger.func = slog_func,
    });
    app.smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    app.smp_linear = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, /* clamp: the LINEAR tap must not wrap edges */
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    app.pip = sgl_make_pipeline(&(sg_pipeline_desc){
        .colors[0].blend.enabled = false,
    });
    /* The offscreen prescale pass renders into an RGBA8 / no-depth / 1-sample
     * target. Its sgl context (and the pipeline bound to it) must carry those
     * exact formats: the default context carries the SWAPCHAIN format, which is
     * BGRA8 on D3D11/Metal and would fail sokol_gfx validation when flushed into
     * the RGBA8 offscreen pass. These are format-only and size-independent —
     * created once, never recreated on resize. */
    app.off_ctx = sgl_make_context(&(sgl_context_desc_t){
        .color_format = SG_PIXELFORMAT_RGBA8,
        .depth_format = SG_PIXELFORMAT_NONE,
        .sample_count = 1,
    });
    app.pip_off = sgl_context_make_pipeline(app.off_ctx, &(sg_pipeline_desc){
        .colors[0].blend.enabled = false,
    });
    int cw, ch;
    emu_canvas_size(&cw, &ch);
    resize_canvas_texture(cw, ch);
    if (!overlay_active())
        set_aspect_hint(cw, ch);
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init();
#endif
}

/* Absolute monotonic nanosecond clock + a sleep-until-absolute-deadline, for the
 * frame pacer. now_ns() is portable (POSIX clock_gettime; QPC on Windows). The
 * sleep is the no-vsync software pacer; it's a no-op on the web (RAF paces) and
 * Windows (D3D11 Present paces). */
static uint64_t now_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void sleep_until_ns(uint64_t target)
{
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    struct timespec until = {.tv_sec = (time_t)(target / 1000000000ull),
                             .tv_nsec = (long)(target % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
#else
    (void)target;
#endif
}

static void frame_cb(void)
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
        start_ns = now_ns();
    }
    uint64_t target = (now_ns() - start_ns) * EMU_VGA_HZ / 1000000000ull;
    uint64_t behind = target > done ? target - done : 0;
    if (behind > EMU_MAX_SKIP) /* hopelessly behind: drop the deficit, resync */
    {
        done += behind - EMU_MAX_SKIP;
        behind = EMU_MAX_SKIP;
    }
    for (uint64_t i = 0; i < behind; i++)
    {
        if (i + 1 < behind)
            emu_run_frame_norender(); /* catch-up frame: CPU/timing only, no pixels */
        else
            emu_run_frame(); /* the frame we'll present: render it */
        done++;
    }

#ifdef EMU_WITH_AUDIO
    audio_out_pump(); /* feed the host audio device from this frame's samples */
#endif

    /* Reflect the run state in the title so the user knows the run is done (exec
     * un-halts within a frame, so this only trips on a real exit), and close the
     * window if asked, so a launcher can run a ROM and return. */
    /* Release a captured mouse if the program gave up the device (exec'd away,
     * unmapped); then refresh the title (run state + capture hint). */
    if (sapp_mouse_locked() && !mou_is_mapped())
        sapp_lock_mouse(false);
    update_title();
    if (emu_cpu_halted && app.exit_on_halt)
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
                    emu_vga_frame_count, bench_total,
                    (double)emu_vga_frame_count / bench_total);
            sapp_request_quit();
        }
    }

    int cw, ch;
    emu_canvas_size(&cw, &ch);
    if (cw != app.tex_w || ch != app.tex_h)
    {
        /* Before tex_w/tex_h update, note whether the window is still within <1px
         * of the OLD canvas aspect, i.e. the user hasn't resized it off-aspect. */
        int w = sapp_width(), h = sapp_height();
        double off = (double)w - (double)h * app.tex_w / app.tex_h;
        int at_aspect = off < 1.0 && off > -1.0;

        resize_canvas_texture(cw, ch);
        if (!overlay_active()) /* the debug workbench never tracks the canvas aspect */
        {
            /* Ask the WM to keep the new aspect on interactive resize. WSLg ignores
             * this (the quad below letterboxes instead); native X11/other WMs honor it. */
            set_aspect_hint(cw, ch);

            /* Re-fit the window width to the new aspect ONLY if it was still pristine;
             * a window the user has resized off-aspect is left alone (and letterboxed).
             * We don't poll-and-snap to enforce it: programmatic resizes are unreliable
             * under WSLg (it restores geometry and drops requests). Height is left
             * as-is; only the width tracks the aspect. */
            int new_w = (int)((long)h * cw / ch);
            if (at_aspect && new_w != w)
                resize_window(new_w, h);
        }
    }

    /* Upload the new frame from the window's framebuffer (zero copy), but
     * only when one was produced this callback; a duplicate present (behind == 0,
     * e.g. a display faster than 60 Hz) re-blits the existing texture below
     * without re-uploading. sokol's swapchain double-buffers the present. */
    static bool ever_uploaded;
    if (behind > 0)
    {
        sg_update_image(app.img, &(sg_image_data){
            .mip_levels[0] = {.ptr = app.fb, .size = (size_t)cw * ch * sizeof(uint32_t)},
        });
        ever_uploaded = true;
    }

#ifdef EMU_WITH_DEBUGGER
    /* Build the debugger windows first (between ImGui new-frame and render) so the
     * dockspace central-node rect is known before the canvas is laid out into it.
     * simgui has its own sokol-gfx pipeline, separate from sgl. */
    if (dbg_is_active())
    {
        dbgui_new_frame(sapp_width(), sapp_height(), sapp_frame_duration(), sapp_dpi_scale());
        dbgui_draw();
    }
#endif

    /* Aspect-preserving quad fitted into the canvas region (the dockspace central
     * node when the debugger is up, else the whole window; a normal run has no
     * overlay and fills the window). The window tracks the canvas aspect (RP6502
     * square pixels -> canvas aspect = display aspect), so the quad normally fills
     * the region; if it is off-aspect (the WM ignored the aspect hint, or
     * mid-resize) it letterboxes/pillarboxes against the clear so content never
     * stretches. */
    int vx, vy, vw, vh;
    canvas_region(&vx, &vy, &vw, &vh); /* central node when docked, else below the menu */
    float qx = 1.0f, qy = 1.0f;
    if ((long)vw * ch > (long)vh * cw)
        qx = (float)((double)vh * cw / ((double)vw * ch)); /* too wide -> pillarbox */
    else
        qy = (float)((double)vw * ch / ((double)vh * cw)); /* too tall -> letterbox */
    /* PASS 1 (sharp only): point-prescale the canvas into the offscreen target
     * at an integer multiple, filling it edge to edge (no letterbox). The final
     * pass LINEAR-downscales that — crisp pixels with smooth motion at any
     * window size. The target is (re)created here whenever its size changes,
     * which catches both a canvas change (cw/ch) and a window resize (factor).
     * The offscreen pass uses its own RGBA8-format sgl context; the default
     * context carries the (possibly BGRA8) swapchain format. */
    if (ever_uploaded && app.filter == EMU_FILTER_SHARP)
    {
        int f = sharp_prescale(cw, ch, vw, vh);
        int want_w = cw * f, want_h = ch * f;
        if (want_w != app.rt_w || want_h != app.rt_h)
            resize_render_target(want_w, want_h);

        sgl_set_context(app.off_ctx);
        sgl_defaults();
        sgl_load_pipeline(app.pip_off);
        sgl_viewport(0, 0, app.rt_w, app.rt_h, true);
        sgl_enable_texture();
        sgl_texture(app.view, app.smp); /* NEAREST source = the canvas texture */
        sgl_begin_quads();
        /* Full-target quad. V is NOT flipped here (unlike the final quad):
         * rendering into an offscreen target and sampling it back inverts Y
         * once, so the final pass's own flip is what lands the image upright. */
        sgl_v2f_t2f(-1.0f, 1.0f, 0.0f, 1.0f);
        sgl_v2f_t2f(1.0f, 1.0f, 1.0f, 1.0f);
        sgl_v2f_t2f(1.0f, -1.0f, 1.0f, 0.0f);
        sgl_v2f_t2f(-1.0f, -1.0f, 0.0f, 0.0f);
        sgl_end();

        sg_begin_pass(&(sg_pass){
            .action = {.colors[0] = {.load_action = SG_LOADACTION_DONTCARE}},
            .attachments = {.colors[0] = app.rt_att},
        });
        sgl_context_draw(app.off_ctx); /* drains only off_ctx's geometry */
        sg_end_pass();

        sgl_set_context(SGL_DEFAULT_CONTEXT);
    }

    /* FINAL PASS (all modes): blit to the swapchain with the letterbox quad.
     * sharp samples the prescaled target (LINEAR); linear samples the canvas
     * (LINEAR); nearest samples the canvas (NEAREST). */
    sgl_defaults();
    /* Until the first frame has been uploaded the canvas texture is undefined;
     * emit no geometry so the swapchain pass below shows only the clear color. */
    if (ever_uploaded)
    {
        sgl_viewport(vx, vy, vw, vh, true); /* the canvas region (central node when docked) */
        sgl_load_pipeline(app.pip);
        sgl_enable_texture();
        if (app.filter == EMU_FILTER_SHARP)
            sgl_texture(app.rt_tex, app.smp_linear);
        else if (app.filter == EMU_FILTER_LINEAR)
            sgl_texture(app.view, app.smp_linear);
        else
            sgl_texture(app.view, app.smp);
        sgl_begin_quads();
        /* Texture v flipped so framebuffer row 0 is at the top. */
        sgl_v2f_t2f(-qx, qy, 0.0f, 0.0f);
        sgl_v2f_t2f(qx, qy, 1.0f, 0.0f);
        sgl_v2f_t2f(qx, -qy, 1.0f, 1.0f);
        sgl_v2f_t2f(-qx, -qy, 0.0f, 1.0f);
        sgl_end();
    }

    sg_begin_pass(&(sg_pass){
        .action = {.colors[0] = {.load_action = SG_LOADACTION_CLEAR,
                                 .clear_value = {app.bg_r, app.bg_g, app.bg_b, 1}}},
        .swapchain = sglue_swapchain(),
    });
    sgl_draw(); /* drains the default context's geometry */
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
        sleep_until_ns(start_ns + (done + 1) * (1000000000ull / EMU_VGA_HZ));
}

/* All host key/char translation lives in kbd.c; the window just forwards. */
void kbd_event(const sapp_event *e);

static void event_cb(const sapp_event *e)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_handle_event(e))
        return; /* the debug UI consumed this event */
#endif
    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
        /* Esc releases a captured mouse (a capture concern) instead of being
         * typed; every other key/char is translated by kbd.c. */
        if (e->key_code == SAPP_KEYCODE_ESCAPE && sapp_mouse_locked())
        {
            sapp_lock_mouse(false); /* matches the browser's pointer-lock exit */
            break;
        }
        kbd_event(e);
        break;
    case SAPP_EVENTTYPE_KEY_UP:
    case SAPP_EVENTTYPE_CHAR:
        kbd_event(e);
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        if (!sapp_mouse_locked())
        {
            /* First click captures the mouse (only once a program wants it);
             * the click itself is consumed by the capture. */
            if (mou_is_mapped())
                sapp_lock_mouse(true);
        }
        else if (e->mouse_button >= 0 && e->mouse_button <= 2)
        {
            app.mouse_buttons |= (uint8_t)(1u << e->mouse_button);
            mou_host_buttons(app.mouse_buttons);
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (sapp_mouse_locked() && e->mouse_button >= 0 && e->mouse_button <= 2)
        {
            app.mouse_buttons &= (uint8_t)~(1u << e->mouse_button);
            mou_host_buttons(app.mouse_buttons);
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (sapp_mouse_locked())
        {
            float s = canvas_scale();
            if (s > 0.0f)
                mou_host_move(e->mouse_dx / s * EMU_MOUSE_GAIN,
                              e->mouse_dy / s * EMU_MOUSE_GAIN);
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        if (sapp_mouse_locked())
            mou_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        break;
    default:
        break;
    }
}

static void cleanup_cb(void)
{
#ifdef EMU_WITH_AUDIO
    if (emu_audio_enabled())
        saudio_shutdown();
#endif
#ifdef EMU_WITH_DEBUGGER
    dap_stop(); /* notify any attached DAP client before the window goes away */
    if (dbg_is_active())
        dbgui_discard();
#endif
    sgl_shutdown();
    sg_shutdown();
}

int emu_run_window(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
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
    /* Under the real-time window, file I/O runs as non-blocking POSIX AIO so the
     * 6502 keeps clocking while it completes (read_xram is background DMA into
     * XRAM) — both the MSC0: drive and ROM: asset reads. Headless/--screenshot
     * never reach here and stay synchronous. */
    host_msc_set_async(true);
    rom_set_async(true);
    /* Open at a fixed height with the width set to the canvas aspect (square
     * pixels: display aspect = cw/ch), so a 4:3 canvas opens 640x480 and a 16:9
     * canvas opens wider. The WM may restore a previous size instead; that's fine
     * — init_cb sets the aspect hint and the quad letterboxes either way. */
    int cw, ch;
    emu_canvas_size(&cw, &ch);
    int canvas_h = (int)(VGA_MAX_HEIGHT * app.scale + 0.5);
    int win_w = (int)((long)canvas_h * cw / ch);
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
    sapp_run(&(sapp_desc){
        .init_cb = init_cb,
        .frame_cb = frame_cb,
        .event_cb = event_cb,
        .cleanup_cb = cleanup_cb,
        .width = win_w,
        .height = win_h,
        .swap_interval = vsync ? 1 : 0, /* off: present uncapped (driver may ignore) */
        .window_title = "Picocomputer 6502",
        .logger.func = slog_func,
    });
    return 0;
}

#endif /* EMU_WITH_SOKOL */
