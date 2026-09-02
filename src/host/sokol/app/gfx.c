/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The canvas: where the machine's picture lands in the window, and how it gets
 * there. Size, aspect, letterbox, the scaling filter and the upload are all
 * here, so there is one answer to where a canvas pixel is on screen -- the
 * render pass and the input layer read the same map.
 *
 * sokol_framebuffer.h does the upload and the prescaled blit and
 * sokol_letterbox.h fits the viewport, so this is arithmetic and bookkeeping
 * around those two.
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/entry.h"
#include "core/vga/vga_emu.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"
#include "sokol/util/sokol_framebuffer.h"
#include "sokol/util/sokol_letterbox.h"
#ifdef EMU_WITH_DEBUGGER
#include "host/sokol/dbg/dbgui.h"
#include "core/dap/dbg.h"
#endif
#include <stddef.h>

static struct
{
    double scale;     /* requested window scale (may be fractional) */
    int tex_w, tex_h; /* last seen canvas native size (sfb tracks it lazily) */
    sfb_framebuffer sfb;          /* presents fb: upload, prescale, letterboxed blit */
    gfx_filter_t filter; /* 0 == NEAREST default */
    float bg_r, bg_g, bg_b; /* letterbox/pillarbox fill (default black) */
    uint32_t *fb;          /* caller's framebuffer: vga renders in, frame_cb uploads */
    bool ever_uploaded; /* sfb's texture is undefined before the first */
} gfx;



void gfx_set_bgcolor(uint8_t r, uint8_t g, uint8_t b)
{
    gfx.bg_r = r / 255.0f;
    gfx.bg_g = g / 255.0f;
    gfx.bg_b = b / 255.0f;
}

void gfx_set_filter(gfx_filter_t filter) { gfx.filter = filter; }

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
 * rows at scale, rounded); inverse of gfx_get_scale. */
static int scaled_canvas_height(double scale)
{
    return (int)(VGA_MAX_HEIGHT * scale + 0.5);
}

void gfx_set_scale(double scale)
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

double gfx_get_scale(void)
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
float gfx_canvas_scale(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    slbx_viewport vp = canvas_viewport();
    return (float)vp.width / cw;
}

bool gfx_canvas_from_fb(float px, float py, int *cx, int *cy)
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


/* The framebuffer object and the first canvas size, from the sokol init
 * callback once sg_setup has run. */
void gfx_setup(void)
{
    sfb_setup(&(sfb_desc){
        .logger.func = slog_func,
    });
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    gfx.sfb = sfb_make_framebuffer(&(sfb_framebuffer_desc){
        .width = cw,
        .height = ch,
    });
    gfx.tex_w = cw;
    gfx.tex_h = ch;
    if (!overlay_active())
        host_window_set_aspect_hint(cw, ch);
}

/* The canvas the machine renders can change size mid-run (a program picks a
 * new mode). Notice it, keep the WM hint honest, and re-fit a window the user
 * has not resized off-aspect. */
void gfx_canvas_changed(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    if (cw != gfx.tex_w || ch != gfx.tex_h)
    {
        /* Before tex_w/tex_h update, note whether the window is still within <1px
         * of the OLD canvas aspect, i.e. the user hasn't resized it off-aspect. */
        int w = sapp_width(), h = sapp_height();
        double off = (double)w - (double)h * gfx.tex_w / gfx.tex_h;
        int at_aspect = off < 1.0 && off > -1.0;

        gfx.tex_w = cw;
        gfx.tex_h = ch;
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
}

/* The viewport this frame's blit fills, computed by gfx_upload and read by
 * gfx_blit -- one canvas_viewport() per frame, not two. */
static slbx_viewport frame_vp;

void gfx_upload(bool new_frame)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);

    /* Aspect-preserving viewport fitted into the canvas region (the dockspace
     * central node when the debugger is up, else the whole window). The window
     * tracks the canvas aspect (RP6502 square pixels -> canvas aspect = display
     * aspect), so the viewport normally fills the region; if it is off-aspect
     * (the WM ignored the aspect hint, or mid-resize) it letterboxes/pillarboxes
     * against the clear so content never stretches. */
    frame_vp = canvas_viewport();
    int f = gfx.filter == GFX_FILTER_SHARP
                ? sharp_prescale(cw, ch, frame_vp.width, frame_vp.height)
                : 1;
    /* Lazy: recreates sfb's images only when the canvas or the sharp prescale
     * factor changed. cliprect must be spelled out -- sfb_resize stores the raw
     * desc value, and a zeroed rect on a recreating resize makes a 0x0 image. */
    bool recreated = sfb_resize(gfx.sfb, &(sfb_resize_desc){
        .width = cw,
        .height = ch,
        .prescale = f,
        .cliprect = {0, 0, cw, ch},
    });

    /* Upload the new frame from the window's framebuffer, but only when one was
     * produced this callback; a duplicate present (no new frame, e.g. a display
     * faster than 60 Hz) re-blits sfb's existing texture without re-uploading.
     * A recreating resize must repopulate regardless. */
    if (new_frame || recreated)
    {
        sfb_update(gfx.sfb, &(sfb_update_desc){
            .pixels = {.ptr = gfx.fb, .size = (size_t)cw * ch * sizeof(uint32_t)},
        });
        gfx.ever_uploaded = true;
    }
}

void gfx_begin_pass(void)
{
    sg_begin_pass(&(sg_pass){
        .action = {.colors[0] = {.load_action = SG_LOADACTION_CLEAR,
                                 .clear_value = {gfx.bg_r, gfx.bg_g, gfx.bg_b, 1}}},
        .swapchain = sglue_swapchain(),
    });
}

void gfx_blit(void)
{
    /* Until the first frame has been uploaded sfb's texture is undefined; skip
     * the blit so the pass shows only the clear color. host_window_menu_active()
     * also suppresses the canvas while the Android ROM menu is up -- its sdtx
     * overlay then draws with the pass's full-window viewport still in effect. */
    if (!gfx.ever_uploaded || frame_vp.width <= 0 || frame_vp.height <= 0 ||
        host_window_menu_active())
        return;
    sg_apply_viewport(frame_vp.x, frame_vp.y, frame_vp.width, frame_vp.height, true);
    if (gfx.filter == GFX_FILTER_NEAREST)
        sfb_render_ex(gfx.sfb, &(sfb_render_desc){.use_nearest_filter = true});
    else
        sfb_render(gfx.sfb);
}

void gfx_end_pass(void)
{
    sg_end_pass();
    sg_commit();
}

void gfx_shutdown(void)
{
    sfb_shutdown();
}

/* Open at a fixed height with the width set to the canvas aspect (square
 * pixels: display aspect = cw/ch), so a 4:3 canvas opens 640x480 and a 16:9
 * canvas opens wider. The WM may restore a previous size instead; that is fine
 * -- the init callback sets the aspect hint and the blit letterboxes either
 * way. */
void gfx_prepare(uint32_t *fb, double scale, bool have_scale, int *out_w, int *out_h)
{
    (void)have_scale;
    gfx.fb = fb;
    gfx.scale = scale;
        int cw, ch;
        vga_canvas_size(&cw, &ch);
        int canvas_h = scaled_canvas_height(gfx.scale);
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
