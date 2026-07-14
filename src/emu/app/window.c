/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/app/window.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/hid/mou.h"
#include "emu/hid/tab.h"
#include "emu/mon/rom.h"
#include "emu/host/msc.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/main.h"
#include "emu/plat.h"
#include "emu/sys/vga.h"

#ifndef EMU_WITH_SOKOL

#include <stdio.h>

int window_run(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
{
    (void)fb;
    (void)scale;
    (void)have_scale;
    (void)vsync;
    (void)exit_on_halt;
    fprintf(stderr, "rp6502-emu: built without window support; use --screenshot\n");
    return 1;
}

void window_set_bgcolor(uint8_t r, uint8_t g, uint8_t b) { (void)r, (void)g, (void)b; }

/* Headless renders at native resolution (no canvas->window scaling), so the
 * filter is genuinely a no-op here. */
void window_set_scale_filter(window_scale_filter_t filter) { (void)filter; }

void window_set_scale(double scale) { (void)scale; }

double window_get_scale(void) { return 0.0; }

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
#include "util/sokol_debugtext.h"
#ifdef EMU_WITH_DEBUGGER
#include "emu/dbg/dbgui.h"
#include "emu/dbg/dap.h"
#endif
#include "emu/app/input.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)
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
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static void resize_window(int w, int h)
{
    HWND hwnd = (HWND)sapp_win32_get_hwnd();
    if (!hwnd)
        return;
    /* w,h are client (== framebuffer/physical) px; grow by this window's DPI
     * frame and keep the top-left corner. */
    RECT r = {0, 0, w, h};
    AdjustWindowRectExForDpi(&r,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
                             GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
#else
static void resize_window(int w, int h) { (void)w, (void)h; }
static void set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
#endif

#if defined(__ANDROID__)
#define MAX_ROMS 64
#define ROM_NAME_MAX 128
static char g_rom_files[MAX_ROMS][ROM_NAME_MAX];
static int g_rom_count = 0;
static int g_rom_selected_index = 0;
static bool g_android_menu_active = false;
static float g_last_menu_y = 0.0f;
static char g_rom_dir[256] = "";
static void detect_rom_directory(void);
static void android_scan_roms(void);
static void android_request_storage_permission(void);
#endif

static struct
{
    double scale;     /* requested window scale (may be fractional) */
    int tex_w, tex_h; /* current texture size = canvas native size */
    bool exit_on_halt; /* close the window when the program stops */
    bool vsync;        /* false: pace the loop to 60 Hz in software (sleep_until_ns) */
    bool flip_v;       /* true on GL-family backends, false on Metal/D3D11 */
    sg_image img;
    sg_view view;
    sg_sampler smp;        /* NEAREST: the canvas texture / prescale source */
    sg_sampler smp_linear; /* LINEAR: the final downscale blit (sharp/linear) */
    sgl_pipeline pip;      /* swapchain-format sgl pipeline */
    sgl_pipeline pip_off;  /* RGBA8/none/1 sgl pipeline for the offscreen pass */
    sgl_context off_ctx;   /* sgl context whose formats match the offscreen RT */
    sg_image rt_img;       /* WINDOW_FILTER_SHARP: offscreen integer-prescale target */
    sg_view rt_att;        /* its color-attachment view (render into) */
    sg_view rt_tex;        /* its texture view (sample in the final pass) */
    int rt_w, rt_h;        /* current offscreen size (0 = not created yet) */
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
 * multiple of the canvas). Used only by WINDOW_FILTER_SHARP. Views reference the
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
 * clamped to [1, WINDOW_PRESCALE_MAX]. The final LINEAR pass absorbs the leftover
 * fractional scale. The cap bounds VRAM (a maximized window on a tiny canvas
 * can't allocate an enormous target); 1 keeps a window-smaller-than-canvas from
 * hitting a zero-size target (the final pass then downscales). */
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
    /* scaled_canvas_height is logical canvas px; resize_window wants framebuffer
     * (== physical) px, so scale by the DPI factor (1.0 unless high_dpi is on).
     * top_reserved_px() is already framebuffer px. */
    int h = (int)(scaled_canvas_height(scale) * sapp_dpi_scale() + 0.5f);
    int w = aspect_width(h, cw, ch);
    resize_window(w, h + top_reserved_px());
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

/* On-screen pixels per canvas pixel (the aspect-fit scale). Host mouse motion is
 * divided by this to get canvas-space motion, so pointer speed doesn't change
 * with the window size. The canvas occupies the window below the debugger menu
 * bar, so its height excludes that reserved strip. */
float window_canvas_scale(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    int rx, ry, rw, rh;
    canvas_region(&rx, &ry, &rw, &rh);
    float sx = (float)rw / cw;
    float sy = (float)rh / ch;
    return (sx < sy ? sx : sy);
}

bool window_canvas_from_fb(float px, float py, int *cx, int *cy)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    int rx, ry, rw, rh;
    canvas_region(&rx, &ry, &rw, &rh);
    float scale = window_canvas_scale();
    if (scale <= 0.0f)
    {
        *cx = *cy = 0;
        return false;
    }
    /* The canvas is centered (letterboxed) within its region. */
    float ox = rx + (rw - cw * scale) * 0.5f;
    float oy = ry + (rh - ch * scale) * 0.5f;
    float fx = (px - ox) / scale;
    float fy = (py - oy) / scale;
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

/* Apply the tablet ROM's requested host cursor (control byte): TAB_CURSOR_OFF
 * hides it (the ROM draws its own), otherwise show that shape. Applied every
 * frame — the debugger's simgui also sets the cursor every frame, so a one-shot
 * would be overwritten. Over a debugger panel ImGui owns the shape, so we only
 * keep the pointer visible there. */
/* Whether the host pointer is over the drawn canvas, set by the input layer. The
 * tablet only owns the host cursor while true; in the letterbox (or a debugger
 * panel, handled below) the system cursor shows. Defaults true so a freshly
 * mapped tablet shows its cursor before the first motion. */
static bool pointer_on_canvas = true;

void window_set_pointer_on_canvas(bool on)
{
    pointer_on_canvas = on;
}

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

static void init_cb(void)
{
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
    sgl_setup(&(sgl_desc_t){
        .logger.func = slog_func,
    });
#if defined(__ANDROID__)
    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_c64(),
        .logger.func = slog_func,
    });
#endif
    sg_backend b = sg_query_backend();
    app.flip_v = (b == SG_BACKEND_GLCORE) || (b == SG_BACKEND_GLES3);
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
    vga_canvas_size(&cw, &ch);
    resize_canvas_texture(cw, ch);
    if (!overlay_active())
        set_aspect_hint(cw, ch);
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
        dbgui_init();
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
        start_ns = os_mono_ns();
    }
    uint64_t target = (os_mono_ns() - start_ns) * VGA_HZ / 1000000000ull;
#if defined(__ANDROID__)
    if (g_android_menu_active)
    {
        done = target;
    }
#endif
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
            int new_w = aspect_width(h, cw, ch);
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
        /* dpi_scale 1.0: render the overlay at native resolution so the 13px
         * bitmap font lands 1:1 (crisp) instead of being magnified/blurred. */
        dbgui_new_frame(sapp_width(), sapp_height(), sapp_frame_duration(), 1.0f);
        dbgui_draw();
    }
#endif

    /* After the debugger set its cursor (simgui, every frame) so the tablet's
     * cursor wins over the canvas. */
    update_cursor();

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
    if (ever_uploaded && app.filter == WINDOW_FILTER_SHARP)
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
#if defined(__ANDROID__)
    if (ever_uploaded && !g_android_menu_active)
#else
    if (ever_uploaded)
#endif
    {
        sgl_viewport(vx, vy, vw, vh, true); /* the canvas region (central node when docked) */
        sgl_load_pipeline(app.pip);
        sgl_enable_texture();
        if (app.filter == WINDOW_FILTER_SHARP)
            sgl_texture(app.rt_tex, app.smp_linear);
        else if (app.filter == WINDOW_FILTER_LINEAR)
            sgl_texture(app.view, app.smp_linear);
        else
            sgl_texture(app.view, app.smp);
        sgl_begin_quads();
        /* GL-family backends sample uploaded rows upside down vs the emulator's
         * row-0-at-top framebuffer; Metal/D3D11 do not. */
        float tv_top = app.flip_v ? 0.0f : 1.0f;
        float tv_bot = app.flip_v ? 1.0f : 0.0f;
        sgl_v2f_t2f(-qx, qy, 0.0f, tv_top);
        sgl_v2f_t2f(qx, qy, 1.0f, tv_top);
        sgl_v2f_t2f(qx, -qy, 1.0f, tv_bot);
        sgl_v2f_t2f(-qx, -qy, 0.0f, tv_bot);
        sgl_end();
    }

    sg_begin_pass(&(sg_pass){
        .action = {.colors[0] = {.load_action = SG_LOADACTION_CLEAR,
                                 .clear_value = {app.bg_r, app.bg_g, app.bg_b, 1}}},
        .swapchain = sglue_swapchain(),
    });
    sgl_draw(); /* drains the default context's geometry */
#if defined(__ANDROID__)
    if (g_android_menu_active)
    {
        sdtx_canvas(320.0f, 240.0f);
        sdtx_origin(2.0f, 2.0f);
        sdtx_color3b(255, 255, 0); // Yellow
        sdtx_puts("PICOCOMPUTER 6502 - ROM SELECT\n");
        sdtx_puts("==============================\n\n");
        
        if (g_rom_count == 0)
        {
            sdtx_color3b(255, 100, 100); // Red
            sdtx_puts("No ROM files (.rp6502) found.\n\n");
            sdtx_color3b(200, 200, 200);
            sdtx_puts("Please copy ROMs to folder:\n");
            sdtx_printf("%s/\n\n", g_rom_dir);
            sdtx_color3b(255, 255, 0); // Yellow
            sdtx_puts("Press SELECT/START/HOME to request\n");
            sdtx_puts("SD Card folder access permission");
        }
        else
        {
            sdtx_color3b(200, 200, 200);
            for (int i = 0; i < g_rom_count; i++)
            {
                if (i == g_rom_selected_index)
                {
                    sdtx_color3b(100, 255, 100); // Green selection cursor
                    sdtx_printf("> %s\n", g_rom_files[i]);
                    sdtx_color3b(200, 200, 200);
                }
                else
                {
                    sdtx_printf("  %s\n", g_rom_files[i]);
                }
            }
            sdtx_puts("\n\nUse DPAD Up/Down to navigate\n");
            sdtx_puts("Press A to Boot Selected ROM");
        }
    }
    sdtx_draw();
#endif
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

static void event_cb(const sapp_event *e)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_handle_event(e))
        return; /* the debug UI consumed this event */
#endif
    input_event(e);
}

static void cleanup_cb(void)
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
    sgl_shutdown();
    sg_shutdown();
}

int window_run(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt)
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
#if defined(__ANDROID__)
    (void)win_w; (void)win_h; (void)vsync;
#else
    /* D3D11 (Windows) leaves the backbuffer at LOGICAL size unless high_dpi is
     * requested, so a DPI-scaled display DWM-stretches (smears) the menu/canvas;
     * ask for a native-resolution backbuffer there. X11 already renders native
     * and ignores this flag; macOS Retina would flip to 2x, so leave it off
     * outside Windows. */
    bool high_dpi = false;
#if defined(_WIN32)
    high_dpi = true;
#endif
    sapp_run(&(sapp_desc){
        .init_cb = init_cb,
        .frame_cb = frame_cb,
        .event_cb = event_cb,
        .cleanup_cb = cleanup_cb,
        .width = win_w,
        .height = win_h,
        .high_dpi = high_dpi,
        .swap_interval = vsync ? 1 : 0, /* off: present uncapped (driver may ignore) */
        .window_title = "Picocomputer 6502",
        .logger.func = slog_func,
    });
#endif
    return 0;
}

#if defined(__ANDROID__)
#include <android/input.h>
#include <android/keycodes.h>
#include "emu/hid/pad.h"
#include <dirent.h>

// Define the Android gamepad buttons state tracking variables
static uint8_t g_android_button0 = 0;
static uint8_t g_android_button1 = 0;
static uint8_t g_android_dpad = 0;
static int g_android_lx = 0;
static int g_android_ly = 0;
static int g_android_rx = 0;
static int g_android_ry = 0;
static int g_android_lt = 0;
static int g_android_rt = 0;



// Forward declarations from emulator codebase (using clean includes)

#include <jni.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/native_activity.h>
#include <errno.h>

static void android_request_storage_permission(void)
{
    ANativeActivity* activity = (ANativeActivity*)sapp_android_get_native_activity();
    if (!activity) return;

    JavaVM* jvm = activity->vm;
    JNIEnv* env = NULL;
    (*jvm)->GetEnv(jvm, (void**)&env, JNI_VERSION_1_6);
    if (!env)
    {
        (*jvm)->AttachCurrentThread(jvm, &env, NULL);
    }
    if (!env) return;

    jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
    
    jstring action_str = (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
    jstring uri_str = (*env)->NewStringUTF(env, "package:com.picocomputer.rp6502");
    
    jmethodID uri_parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jobject uri_obj = (*env)->CallStaticObjectMethod(env, uri_class, uri_parse, uri_str);
    
    jmethodID intent_init = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;)V");
    jobject intent_obj = (*env)->NewObject(env, intent_class, intent_init, action_str);
    
    jmethodID set_data_method = (*env)->GetMethodID(env, intent_class, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;");
    (*env)->CallObjectMethod(env, intent_obj, set_data_method, uri_obj);
    
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID start_activity_method = (*env)->GetMethodID(env, activity_class, "startActivity", "(Landroid/content/Intent;)V");
    (*env)->CallVoidMethod(env, activity->clazz, start_activity_method, intent_obj);
}

static void detect_rom_directory(void)
{
    // 1. Try physical SD Card first: scan /storage/
    DIR* dir = opendir("/storage");
    if (dir)
    {
        struct dirent* de;
        while ((de = readdir(dir)) != NULL)
        {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0 ||
                strcmp(de->d_name, "self") == 0 ||
                strcmp(de->d_name, "emulated") == 0)
            {
                continue;
            }
            
            // Try /storage/ID/Download/rp6502
            char path[512];
            snprintf(path, sizeof(path), "/storage/%s/Download/rp6502", de->d_name);
            DIR* d = opendir(path);
            if (d)
            {
                closedir(d);
                strncpy(g_rom_dir, path, sizeof(g_rom_dir) - 1);
                g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
                closedir(dir);
                return;
            }
            
            // Try /storage/ID/rp6502
            snprintf(path, sizeof(path), "/storage/%s/rp6502", de->d_name);
            d = opendir(path);
            if (d)
            {
                closedir(d);
                strncpy(g_rom_dir, path, sizeof(g_rom_dir) - 1);
                g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
                closedir(dir);
                return;
            }
        }
        closedir(dir);
    }
    
    // 2. Try internal storage Download/rp6502 next
    DIR* d = opendir("/sdcard/Download/rp6502");
    if (d)
    {
        closedir(d);
        strcpy(g_rom_dir, "/sdcard/Download/rp6502");
        return;
    }
    
    // 3. Try to create internal storage Download/rp6502
    if (mkdir("/sdcard/Download/rp6502", 0777) == 0 || errno == EEXIST)
    {
        d = opendir("/sdcard/Download/rp6502");
        if (d)
        {
            closedir(d);
            strcpy(g_rom_dir, "/sdcard/Download/rp6502");
            return;
        }
    }
    
    // 4. Fallback to app internal data path
    const void* native_act = sapp_android_get_native_activity();
    if (native_act)
    {
        ANativeActivity* activity = (ANativeActivity*)native_act;
        if (activity->internalDataPath)
        {
            strncpy(g_rom_dir, activity->internalDataPath, sizeof(g_rom_dir) - 1);
            g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
            return;
        }
        else if (activity->externalDataPath)
        {
            strncpy(g_rom_dir, activity->externalDataPath, sizeof(g_rom_dir) - 1);
            g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
            return;
        }
    }
    
    // Absolute fallback
    strcpy(g_rom_dir, ".");
}

static void android_scan_roms(void)
{
    detect_rom_directory();
    chdir(g_rom_dir);

    g_rom_count = 0;
    DIR* d = opendir(".");
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL)
    {
        size_t len = strlen(de->d_name);
        if (de->d_name[0] != '.' && len > 7 && strcasecmp(de->d_name + len - 7, ".rp6502") == 0)
        {
            strncpy(g_rom_files[g_rom_count], de->d_name, ROM_NAME_MAX - 1);
            g_rom_files[g_rom_count][ROM_NAME_MAX - 1] = '\0';
            g_rom_count++;
            if (g_rom_count >= MAX_ROMS) break;
        }
    }
    closedir(d);
}

int rp6502_android_input_hook(const AInputEvent* event)
{
    int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_KEY)
    {
        int32_t key_code = AKeyEvent_getKeyCode(event);
        int32_t action = AKeyEvent_getAction(event);
        bool down = (action == AKEY_EVENT_ACTION_DOWN);
        
        // Handle menu navigation if menu is active
        if (g_android_menu_active)
        {
            if (down)
            {
                switch (key_code)
                {
                    case AKEYCODE_DPAD_UP:
                        g_rom_selected_index--;
                        if (g_rom_selected_index < 0) g_rom_selected_index = g_rom_count - 1;
                        return 1;
                    case AKEYCODE_DPAD_DOWN:
                        g_rom_selected_index++;
                        if (g_rom_selected_index >= g_rom_count) g_rom_selected_index = 0;
                        return 1;
                    case AKEYCODE_BUTTON_A:
                        if (g_rom_count > 0)
                        {
                            rom_load(g_rom_files[g_rom_selected_index]);
                            main_init();
                            // Reset key and button states to prevent stuck inputs after closing the menu
                            g_android_button0 = 0;
                            g_android_button1 = 0;
                            g_android_dpad = 0;
                            g_android_lx = 0;
                            g_android_ly = 0;
                            g_android_rx = 0;
                            g_android_ry = 0;
                            g_android_lt = 0;
                            g_android_rt = 0;
                            pad_connect(0, true);
                            g_android_menu_active = false;
                        }
                        return 1;
                    case AKEYCODE_BUTTON_SELECT:
                    case AKEYCODE_BUTTON_START:
                    case AKEYCODE_BUTTON_MODE:
                        // Request permission page and refresh the list
                        android_request_storage_permission();
                        android_scan_roms();
                        return 1;
                }
            }
            // Block all other keys from propagating when menu is active
            return 1;
        }
        
        switch (key_code)
        {
            // Retroid Pocket 3+ physical gamepad mapping
            case AKEYCODE_BUTTON_A:
                if (down) g_android_button0 |= 0x01; else g_android_button0 &= ~0x01;
                break;
            case AKEYCODE_BUTTON_B:
                if (down) g_android_button0 |= 0x02; else g_android_button0 &= ~0x02;
                break;
            case AKEYCODE_BUTTON_X:
                if (down) g_android_button0 |= 0x08; else g_android_button0 &= ~0x08;
                break;
            case AKEYCODE_BUTTON_Y:
                if (down) g_android_button0 |= 0x10; else g_android_button0 &= ~0x10;
                break;
            case AKEYCODE_BUTTON_L1:
                if (down) g_android_button0 |= 0x40; else g_android_button0 &= ~0x40;
                break;
            case AKEYCODE_BUTTON_R1:
                if (down) g_android_button0 |= 0x80; else g_android_button0 &= ~0x80;
                break;
                
            case AKEYCODE_BUTTON_L2:
                if (down) g_android_button1 |= 0x01; else g_android_button1 &= ~0x01;
                break;
            case AKEYCODE_BUTTON_R2:
                if (down) g_android_button1 |= 0x02; else g_android_button1 &= ~0x02;
                break;
            case AKEYCODE_BUTTON_SELECT:
                if (down) g_android_button1 |= 0x04; else g_android_button1 &= ~0x04;
                // Toggle ROM select menu when SELECT + START are both pressed
                if (down && (g_android_button1 & 0x08))
                {
                    g_android_menu_active = true;
                    android_scan_roms();
                }
                break;
            case AKEYCODE_BUTTON_START:
                if (down) g_android_button1 |= 0x08; else g_android_button1 &= ~0x08;
                // Toggle ROM select menu when SELECT + START are both pressed
                if (down && (g_android_button1 & 0x04))
                {
                    g_android_menu_active = true;
                    android_scan_roms();
                }
                break;
            case AKEYCODE_BUTTON_MODE: // Home button
                if (down) g_android_button1 |= 0x10; else g_android_button1 &= ~0x10;
                if (down)
                {
                    g_android_menu_active = true;
                    android_scan_roms();
                }
                break;
            case AKEYCODE_BUTTON_THUMBL:
                if (down) g_android_button1 |= 0x20; else g_android_button1 &= ~0x20;
                break;
            case AKEYCODE_BUTTON_THUMBR:
                if (down) g_android_button1 |= 0x40; else g_android_button1 &= ~0x40;
                break;
                
            case AKEYCODE_DPAD_UP:
                if (down) g_android_dpad |= 0x01; else g_android_dpad &= ~0x01;
                break;
            case AKEYCODE_DPAD_DOWN:
                if (down) g_android_dpad |= 0x02; else g_android_dpad &= ~0x02;
                break;
            case AKEYCODE_DPAD_LEFT:
                if (down) g_android_dpad |= 0x04; else g_android_dpad &= ~0x04;
                break;
            case AKEYCODE_DPAD_RIGHT:
                if (down) g_android_dpad |= 0x08; else g_android_dpad &= ~0x08;
                break;
                
            default:
                return 0; // Not handled
        }
        pad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                        g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt, false);
        return 1; // Handled
    }
    else if (type == AINPUT_EVENT_TYPE_MOTION)
    {
        // Handle menu navigation if menu is active
        if (g_android_menu_active)
        {
            float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
            float stick_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
            
            float input_y = 0.0f;
            if (hat_y < -0.5f || stick_y < -0.5f) input_y = -1.0f;
            else if (hat_y > 0.5f || stick_y > 0.5f) input_y = 1.0f;
            
            if (input_y == -1.0f && g_last_menu_y != -1.0f)
            {
                g_rom_selected_index--;
                if (g_rom_selected_index < 0) g_rom_selected_index = g_rom_count - 1;
            }
            else if (input_y == 1.0f && g_last_menu_y != 1.0f)
            {
                g_rom_selected_index++;
                if (g_rom_selected_index >= g_rom_count) g_rom_selected_index = 0;
            }
            g_last_menu_y = input_y;
            return 1; // Consume event
        }
        
        // Read Hat/D-pad axes
        float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        
        g_android_dpad = 0;
        if (hat_x < -0.5f) g_android_dpad |= 0x04; // LEFT
        if (hat_x > 0.5f)  g_android_dpad |= 0x08; // RIGHT
        if (hat_y < -0.5f) g_android_dpad |= 0x01; // UP
        if (hat_y > 0.5f)  g_android_dpad |= 0x02; // DOWN
        
        // Read Analog Stick axes
        float lx_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
        float ly_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
        float rx_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
        float ry_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
        
        g_android_lx = (int)(lx_val * 127.0f);
        g_android_ly = (int)(ly_val * 127.0f);
        g_android_rx = (int)(rx_val * 127.0f);
        g_android_ry = (int)(ry_val * 127.0f);
        
        // Read Trigger axes
        float lt_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
        float rt_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
        g_android_lt = (int)(lt_val * 255.0f);
        g_android_rt = (int)(rt_val * 255.0f);
        
        pad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                        g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt, false);
        return 1; // Handled
    }
    return 0; // Not handled
}

// Global framebuffer for Android
static uint32_t android_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

#include <unistd.h>
#include <sys/stat.h>
#include <android/native_activity.h>

sapp_desc sokol_main(int argc, char* argv[])
{
    (void)argc; (void)argv;
    
    detect_rom_directory();
    chdir(g_rom_dir);
    
    // Initialize host MSC & ROM loader async modes
    msc_set_async(true);
    rom_set_async(true);
    
    // Initialize emulator
    main_init();
    vga_set_framebuffer(android_fb);
    
    // Try to load a default rom (boot.rp6502) if it exists, otherwise activate the menu
    if (rom_load("boot.rp6502"))
    {
        g_android_menu_active = false;
    }
    else
    {
        g_android_menu_active = true;
        android_scan_roms();
    }
    
    // Connect gamepad player 0
    pad_connect(0, true);
    
    // Setup app state
    app.fb = android_fb;
    app.scale = 1.0;
    app.vsync = true;
    app.exit_on_halt = false;
    
    return (sapp_desc){
        .init_cb = init_cb,
        .frame_cb = frame_cb,
        .event_cb = event_cb,
        .cleanup_cb = cleanup_cb,
        .width = 640,
        .height = 480,
        .window_title = "Picocomputer 6502",
        .logger.func = slog_func,
    };
}
#endif

#endif /* EMU_WITH_SOKOL */
