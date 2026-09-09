/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * sokol_framebuffer.h does the upload and the prescaled blit, and
 * sokol_letterbox.h fits the viewport.
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/entry.h"
#include "core/vga/vga_emu.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/util/sokol_framebuffer.h"
#include "sokol/util/sokol_letterbox.h"
#ifdef EMU_WITH_DEBUGGER
#include "host/sokol/dbg/dbgui.h"
#include "core/dap/dbg.h"
#endif
#include <stddef.h>

static struct
{
    double scale;
    int tex_w, tex_h; /* last seen canvas size */
    sfb_framebuffer sfb;
    gfx_filter_t filter;
    float bg_r, bg_g, bg_b;
    uint32_t *fb; /* the caller's framebuffer, which vga renders into */
    bool ever_uploaded;
} gfx;



void gfx_set_bgcolor(uint8_t r, uint8_t g, uint8_t b)
{
    gfx.bg_r = r / 255.0f;
    gfx.bg_g = g / 255.0f;
    gfx.bg_b = b / 255.0f;
}

void gfx_set_filter(gfx_filter_t filter) { gfx.filter = filter; }

/* The largest integer scale at which the canvas still fits the window, taking
 * the smaller axis so pixels stay square; sfb's final linear pass absorbs the
 * fractional remainder. The cap bounds video memory, because sfb has none of
 * its own and a maximized window on a small canvas would otherwise ask for an
 * enormous render target: 640*6 by 480*6 in RGBA8 is a 42 MB ceiling. The floor
 * of 1 keeps a window smaller than the canvas off a zero-size target. */
#define WINDOW_PRESCALE_MAX 6

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

/* The debugger's menu bar and dockspace own the window layout when they are up:
 * panels dock beside the canvas and the central node letterboxes it, so the
 * window resizes freely with no aspect hint and no width re-fit, and its size
 * persists per debug session instead of tracking --scale. */
static bool overlay_active(void)
{
#ifdef EMU_WITH_DEBUGGER
    return dbg_is_active();
#else
    return false;
#endif
}

/* Framebuffer pixels reserved at the top of the window for the debugger's menu
 * bar, so the canvas is laid out below the menu rather than under it, and 0 when
 * the overlay is down. The overlay is given a dpi_scale of 1.0, so the height it
 * reports is already in framebuffer pixels. The clamp keeps at least one row for
 * the canvas. */
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

static int aspect_width(int h, int cw, int ch)
{
    return (int)((long)h * cw / ch);
}

static int scaled_canvas_height(double scale)
{
    return (int)(VGA_MAX_HEIGHT * scale + 0.5);
}

void gfx_set_scale(double scale)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    /* scaled_canvas_height is in logical canvas pixels and host_window_resize
     * takes physical framebuffer pixels, so the DPI factor converts between
     * them; it is 1.0 unless high_dpi is on. top_reserved_px is already
     * framebuffer pixels. */
    int h = (int)(scaled_canvas_height(scale) * sapp_dpi_scale() + 0.5f);
    int w = aspect_width(h, cw, ch);
    host_window_resize(w, h + top_reserved_px());
}

double gfx_get_scale(void)
{
    if (!sapp_isvalid())
        return 0.0;
    /* sapp_height and top_reserved_px are physical framebuffer pixels, so the
     * DPI factor divides out to leave the scale in the same logical units
     * --scale is given in. */
    return (double)(sapp_height() - top_reserved_px()) / (VGA_MAX_HEIGHT * sapp_dpi_scale());
}

/* The framebuffer-pixel rectangle, x and y from the top left, that the emulated
 * canvas draws into: the dockspace central node when the debugger overlay is up,
 * so docked panels take space beside the screen rather than over it, and
 * otherwise the whole window below the menu-bar strip. */
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
 * and given to slbx as borders against the full framebuffer. It is computed on
 * every call because input events arrive before the first frame and across menu
 * and dock changes. */
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


void gfx_setup(void)
{
    sfb_setup(&(sfb_desc){
        .logger.func = app_log,
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

void gfx_canvas_changed(void)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    if (cw != gfx.tex_w || ch != gfx.tex_h)
    {
        /* Whether the window is still within a pixel of the old canvas aspect,
         * which says the user has not resized it off-aspect. It has to be read
         * before tex_w and tex_h are updated. */
        int w = sapp_width(), h = sapp_height();
        double off = (double)w - (double)h * gfx.tex_w / gfx.tex_h;
        int at_aspect = off < 1.0 && off > -1.0;

        gfx.tex_w = cw;
        gfx.tex_h = ch;
        if (!overlay_active()) /* the debugger's window never tracks the canvas aspect */
        {
            /* Native X11 window managers honor the hint. WSLg ignores it, and
             * the blit letterboxes there instead. */
            host_window_set_aspect_hint(cw, ch);

            /* Only a window still at the old aspect is re-fitted; one the user
             * resized is left alone and letterboxed. There is no poll and snap
             * to enforce it afterwards, because WSLg restores its own geometry
             * and drops resize requests. The height is left as it was, and only
             * the width tracks the aspect. */
            int new_w = aspect_width(h, cw, ch);
            if (at_aspect && new_w != w)
                host_window_resize(new_w, h);
        }
    }
}

/* Computed by gfx_upload and read by gfx_blit, so a frame fits its viewport
 * once rather than twice. */
static slbx_viewport frame_vp;

void gfx_upload(bool new_frame)
{
    int cw, ch;
    vga_canvas_size(&cw, &ch);

    /* The window tracks the canvas aspect, because RP6502 pixels are square and
     * so the canvas aspect is the display aspect, and the viewport then fills
     * the region. When the window is off-aspect, because the window manager
     * ignored the hint or a resize is in progress, the difference becomes
     * letterbox or pillarbox against the clear color and nothing stretches. */
    frame_vp = canvas_viewport();
    int f = gfx.filter == GFX_FILTER_SHARP
                ? sharp_prescale(cw, ch, frame_vp.width, frame_vp.height)
                : 1;
    /* sfb_resize recreates its images only when the canvas or the prescale
     * factor changed. The cliprect has to be spelled out because sfb_resize
     * stores the raw desc value, so a zeroed rectangle on a resize that
     * recreates would make a 0 by 0 image. */
    bool recreated = sfb_resize(gfx.sfb, &(sfb_resize_desc){
        .width = cw,
        .height = ch,
        .prescale = f,
        .cliprect = {0, 0, cw, ch},
    });

    /* A duplicate present, which is what a display faster than 60 Hz asks for,
     * re-blits sfb's existing texture instead of uploading again. A resize that
     * recreated the images has to fill them whatever the machine did. */
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
    /* sfb's texture holds nothing until the first upload, so skipping the blit
     * leaves the pass showing only the clear color. A platform overlay suppresses
     * the canvas the same way, and its own text then draws with the pass's
     * full-window viewport still in effect. */
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

/* The window opens at the height --scale asks for and the width its canvas
 * aspect gives, so a 4:3 canvas opens 640x480 and a 16:9 canvas opens wider.
 * A window manager may restore a previous size instead, which is fine: the init
 * callback sets the aspect hint and the blit letterboxes either way. */
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
            /* The debugger's menu bar sits above the canvas, so the window opens
             * taller by the bar's height; without that the canvas-aspect window
             * squeezes the picture under the menu. It is sized up front from the
             * estimate because WSLg drops resize requests made after the window
             * is open. */
            win_h += (int)(dbgui_menu_bar_estimate() + 0.5f);
            /* The last debug session's window size is persisted with its layout,
             * and an explicit --scale asks for a size and wins over it. */
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
