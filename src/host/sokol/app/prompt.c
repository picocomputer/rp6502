/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The drop-a-ROM card, drawn rather than shipped as an image so it is sharp at
 * any window size: a rounded box, a heavy dashed border, the app icon, two
 * message lines and a link. sokol_gl for the vectors, sokol_debugtext for the
 * text, both in the pass the application has already begun.
 */

#include "host/sokol/app/prompt.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/icon.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/util/sokol_debugtext.h"
#include "sokol/util/sokol_gl.h"
#include "sokol/sokol_log.h"
#include "core/sys/version.h"
#include <math.h>
#include <string.h>

/* One convex filled rounded rect as a center-fan of triangles (sokol_gl, in the
 * current pass, window-pixel coords per the ortho set in prompt_draw). */
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
 * prompt_setup (live for the app lifetime, like sgl/sdtx). */
static struct
{
    sg_image img;
    sg_view view;
    sg_sampler smp;
    sgl_pipeline pip; /* alpha blend; the default sgl pipeline is opaque */
} prompt_icon;

/* Docs link under the prompt bubble, and its on-screen hit box in framebuffer px
 * (set each frame by prompt_draw) so a click can open it. */
static const char PROMPT_DOCS_URL[] = "https://picocomputer.github.io/"; /* opened on click */
static const char PROMPT_DOCS_TEXT[] = "picocomputer.github.io";         /* shown on screen */
static struct
{
    float x, y, w, h;
} prompt_url;

void prompt_setup(void)
{
    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_c64(),
        .logger.func = slog_func,
    });
    /* No formats: they default to the environment sg_setup was given, which is
     * sglue_environment() and therefore the swapchain's. Naming them here meant
     * casting sapp_color_format(), and sokol_app's pixel format is a different
     * enum from sokol_gfx's — sokol_glue translates between them for exactly
     * this reason. The cast read R8/R16SN out of an RGBA8 swapchain, and the
     * pipeline was rejected on the first frame that drew this screen. */
    sgl_setup(&(sgl_desc_t){
        .max_vertices = 16384, /* the dashed border strokes many thick quads */
        .max_commands = 64,
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

void prompt_draw(const char *line1, const char *line2)
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

    float gamepad_x = glyph * 2.0f, gamepad_y = glyph * 1.4f;
    float bw = wide * glyph + 2.0f * gamepad_x;
    float bh = (row2 + 1.0f - row1) * glyph + 2.0f * gamepad_y;
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
    const char *ver = version_string();
    float ver_gh = glyph;
    float ver_x = (w - (float)strlen(ver) * ver_gh) * 0.5f;
    float ver_y = by + bh + gap * 1.7f; /* sit a little below the card */
    float url_gh = glyph;
    float url_w = (float)strlen(docs_url) * url_gh;
    float url_x = (w - url_w) * 0.5f;
    float url_y = ver_y + ver_gh * 1.6f; /* the version takes the line above */
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
    /* Dash ink, not the bright title color: the URL below is the only thing
     * here that does anything when clicked, and should look like it. */
    prompt_text_line(ver, ver_gh, ver_x, ver_y, w, h, ink);
    prompt_text_line(docs_url, url_gh, url_x, url_y, w, h, title_col);
    sdtx_draw();
}

bool prompt_url_hit(float x, float y)
{
    return x >= prompt_url.x && x < prompt_url.x + prompt_url.w &&
           y >= prompt_url.y && y < prompt_url.y + prompt_url.h;
}

void prompt_url_open(void)
{
    host_window_open_url(PROMPT_DOCS_URL);
}
