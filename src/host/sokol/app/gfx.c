/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's frame is a texture, uploaded once for each new frame and
 * drawn in the window's own pass by one shader that scales it, and
 * sokol_letterbox.h fits the viewport. There is no offscreen pass: in WebGL a
 * pass into a texture asks the GPU process whether the target is complete, and
 * the page waits for the answer, which comes late whenever that process is
 * busy presenting.
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/run.h"
#include "core/vga/vga_emu.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/util/sokol_letterbox.h"
#ifdef EMU_WITH_DEBUGGER
#include "host/sokol/dbg/dbgui.h"
#include "core/dap/dbg.h"
#endif
#include <stddef.h>

static struct
{
    int frame_w, frame_h; /* last seen frame shape */
    bool fixed_aspect;
    /* The frame's texture, the staging buffer each upload goes through, and
     * the size they were made at. */
    sg_image img;
    sg_view view;
    sg_buffer staging;
    int img_w, img_h;
    sg_sampler nearest, linear;
    sg_shader shd;
    sg_pipeline pip;
    gfx_filter_t filter;
    float bg_r, bg_g, bg_b;
    /* The machine's frame on screen, whose shape everything here goes by,
     * the pointer's mapping too: what is shown, which can be a frame behind
     * the machine. */
    const run_frame_t *shown;
    bool ever_uploaded;
#ifdef EMU_WITH_DEBUGGER
    /* A window size the debugger's menu asked for, for gfx_canvas_changed,
     * which runs where the window may be resized; 0 for none. */
    int resize_w, resize_h;
#endif
} gfx;

void gfx_set_frame(const run_frame_t *frame)
{
    gfx.shown = frame;
}

static void canvas_size(int *w, int *h)
{
    *w = gfx.shown->canvas_w;
    *h = gfx.shown->canvas_h;
}

void gfx_set_bgcolor(uint8_t r, uint8_t g, uint8_t b)
{
    gfx.bg_r = r / 255.0f;
    gfx.bg_g = g / 255.0f;
    gfx.bg_b = b / 255.0f;
}

void gfx_set_filter(gfx_filter_t filter) { gfx.filter = filter; }

void gfx_set_fixed_aspect(bool fixed) { gfx.fixed_aspect = fixed; }

/* The shape the window keeps: the canvas when the frame is dynamic, and the
 * console when it is fixed. A savestate load sets the console it carries, so a
 * fixed frame moves to the restored aspect. */
static void frame_size(int *w, int *h)
{
    if (gfx.fixed_aspect)
    {
        *w = gfx.shown->console_w;
        *h = gfx.shown->console_h;
    }
    else
        canvas_size(w, h);
}

/* When the debugger's menu bar and dockspace are up, panels dock beside the
 * canvas and the canvas is letterboxed within the dockspace's central node, so
 * the window resizes freely with no aspect lock and no re-fit. */
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

#ifdef EMU_WITH_DEBUGGER
void gfx_set_scale(double scale)
{
    int fw, fh;
    frame_size(&fw, &fh);
    /* scaled_canvas_height is in logical canvas pixels and host_window_resize
     * takes physical framebuffer pixels, so the DPI factor converts between
     * them. It is 1.0 unless high_dpi is on (Windows, macOS, web) or Xft.dpi is
     * above 96 (X11, where sokol ignores high_dpi). top_reserved_px is already
     * framebuffer pixels. */
    int h = (int)(scaled_canvas_height(scale) * sapp_dpi_scale() + 0.5f);
    gfx.resize_w = aspect_width(h, fw, fh);
    gfx.resize_h = h + top_reserved_px();
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
#endif

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

/* The w:h aspect fit centered within the framebuffer-pixel rectangle at x, y,
 * given to slbx as borders against the full framebuffer. */
static slbx_viewport aspect_fit(int x, int y, int w, int h, int aw, int ah)
{
    return slbx_letterbox(sapp_width(), sapp_height(),
                          &(slbx_letterbox_desc){
                              .content_aspect_ratio = (float)aw / (float)ah,
                              .border = {
                                  .left = x,
                                  .right = sapp_width() - (x + w),
                                  .top = y,
                                  .bottom = sapp_height() - (y + h),
                              },
                          });
}

/* The viewport the canvas draws into: the frame fitted to canvas_region, and
 * the canvas fitted to that frame. A canvas at the frame's aspect, which every
 * canvas is when the frame is dynamic, is fitted once, so it lands exactly where
 * a single fit puts it; the cross-multiplied test is exact where a float
 * comparison of the two aspects might not be. It is computed on every call
 * because input events arrive before the first frame and across menu and dock
 * changes. */
static slbx_viewport canvas_viewport(void)
{
    int cw, ch, fw, fh;
    canvas_size(&cw, &ch);
    frame_size(&fw, &fh);
    int rx, ry, rw, rh;
    canvas_region(&rx, &ry, &rw, &rh);
    slbx_viewport vp = aspect_fit(rx, ry, rw, rh, fw, fh);
    if ((long)cw * fh != (long)ch * fw)
        vp = aspect_fit(vp.x, vp.y, vp.width, vp.height, cw, ch);
    return vp;
}

float gfx_canvas_width(void)
{
    return (float)canvas_viewport().width;
}

bool gfx_canvas_from_fb(float px, float py, int *cx, int *cy)
{
    int cw, ch;
    canvas_size(&cw, &ch);
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


/* The shader draws one triangle that covers the viewport, and fs_params is
 * the canvas size in pixels and the scale, the window pixels one canvas pixel
 * covers along each axis. Sharp gives the real scale, which keeps each canvas
 * pixel a solid block and blends only across the one window pixel where two
 * meet, so pixel widths stay even at any scale. Linear and nearest give a
 * scale of 1, which samples where the pixel lands, with the linear or the
 * nearest sampler. Below a scale of 1 sharp is linear too. */
typedef struct
{
    float size[2];
    float scale[2];
} gfx_fs_params_t;

#if defined(SOKOL_GLES3)
static const char gfx_vs_src[] =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main()\n"
    "{\n"
    "    float x = float((gl_VertexID & 1) << 1);\n"
    "    float y = float(gl_VertexID & 2);\n"
    "    gl_Position = vec4(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.5, 1.0);\n"
    "    uv = vec2(x, 1.0 - y);\n"
    "}\n";
static const char gfx_fs_src[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 fs_params[1];\n"
    "uniform highp sampler2D tex_smp;\n"
    "in vec2 uv;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "void main()\n"
    "{\n"
    "    vec2 size = fs_params[0].xy, scale = fs_params[0].zw;\n"
    "    vec2 texel = uv * size;\n"
    "    vec2 d = fract(texel) - 0.5;\n"
    "    vec2 edge = 0.5 - 0.5 / scale;\n"
    "    vec2 at = floor(texel) + 0.5 + (d - clamp(d, -edge, edge)) * scale;\n"
    "    frag_color = vec4(texture(tex_smp, at / size).rgb, 1.0);\n"
    "}\n";
#elif defined(SOKOL_D3D11)
static const char gfx_vs_src[] =
    "struct vs_out { float2 uv : TEXCOORD0; float4 pos : SV_Position; };\n"
    "vs_out vs_main(uint vid : SV_VertexID)\n"
    "{\n"
    "    vs_out o;\n"
    "    float x = (vid & 1) ? 2.0 : 0.0;\n"
    "    float y = (vid & 2) ? 2.0 : 0.0;\n"
    "    o.pos = float4(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.5, 1.0);\n"
    "    o.uv = float2(x, 1.0 - y);\n"
    "    return o;\n"
    "}\n";
static const char gfx_fs_src[] =
    "cbuffer fs_params : register(b0) { float4 params; };\n"
    "Texture2D<float4> tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 fs_main(float2 uv : TEXCOORD0) : SV_Target0\n"
    "{\n"
    "    float2 size = params.xy, scale = params.zw;\n"
    "    float2 texel = uv * size;\n"
    "    float2 d = frac(texel) - 0.5;\n"
    "    float2 edge = 0.5 - 0.5 / scale;\n"
    "    float2 at = floor(texel) + 0.5 + (d - clamp(d, -edge, edge)) * scale;\n"
    "    return float4(tex.Sample(smp, at / size).rgb, 1.0);\n"
    "}\n";
#elif defined(SOKOL_METAL)
static const char gfx_vs_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_out { float4 pos [[position]]; float2 uv [[user(locn0)]]; };\n"
    "vertex vs_out vs_main(uint vid [[vertex_id]])\n"
    "{\n"
    "    vs_out o;\n"
    "    float x = (vid & 1) ? 2.0 : 0.0;\n"
    "    float y = (vid & 2) ? 2.0 : 0.0;\n"
    "    o.pos = float4(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.5, 1.0);\n"
    "    o.uv = float2(x, 1.0 - y);\n"
    "    return o;\n"
    "}\n";
static const char gfx_fs_src[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_in { float2 uv [[user(locn0)]]; };\n"
    "struct fs_params { float4 params; };\n"
    "fragment float4 fs_main(fs_in in [[stage_in]], constant fs_params& p [[buffer(0)]],\n"
    "                        texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])\n"
    "{\n"
    "    float2 size = p.params.xy, scale = p.params.zw;\n"
    "    float2 texel = in.uv * size;\n"
    "    float2 d = fract(texel) - 0.5;\n"
    "    float2 edge = 0.5 - 0.5 / scale;\n"
    "    float2 at = floor(texel) + 0.5 + (d - clamp(d, -edge, edge)) * scale;\n"
    "    return float4(tex.sample(smp, at / size).rgb, 1.0);\n"
    "}\n";
#else
#error "gfx.c has shaders for SOKOL_GLES3, SOKOL_D3D11 and SOKOL_METAL"
#endif

static sg_shader gfx_make_shader(void)
{
    return sg_make_shader(&(sg_shader_desc){
        .vertex_func = {.source = gfx_vs_src, .entry = "vs_main"},
        .fragment_func = {.source = gfx_fs_src, .entry = "fs_main"},
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .layout = SG_UNIFORMLAYOUT_STD140,
            .size = sizeof(gfx_fs_params_t),
            .hlsl_register_b_n = 0,
            .msl_buffer_n = 0,
            .glsl_uniforms[0] = {
                .type = SG_UNIFORMTYPE_FLOAT4,
                .array_count = 1,
                .glsl_name = "fs_params",
            },
        },
        .views[0].texture = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .image_type = SG_IMAGETYPE_2D,
            .sample_type = SG_IMAGESAMPLETYPE_FLOAT,
            .hlsl_register_t_n = 0,
            .msl_texture_n = 0,
        },
        .samplers[0] = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .sampler_type = SG_SAMPLERTYPE_FILTERING,
            .hlsl_register_s_n = 0,
            .msl_sampler_n = 0,
        },
        .texture_sampler_pairs[0] = {
            .stage = SG_SHADERSTAGE_FRAGMENT,
            .view_slot = 0,
            .sampler_slot = 0,
            .glsl_name = "tex_smp",
        },
        .label = "gfx-shader",
    });
}

static sg_sampler gfx_make_sampler(sg_filter filter)
{
    return sg_make_sampler(&(sg_sampler_desc){
        .min_filter = filter,
        .mag_filter = filter,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
}

static void gfx_destroy_image(void)
{
    sg_destroy_view(gfx.view);
    sg_destroy_image(gfx.img);
    sg_destroy_buffer(gfx.staging);
}

/* The texture and its staging buffer at the canvas size. A copy into a
 * texture is the upload sokol_gfx offers for one kept across frames, which a
 * duplicate present re-draws. */
static void gfx_make_image(int w, int h)
{
    gfx.staging = sg_make_buffer(&(sg_buffer_desc){
        .usage = {.staging_buffer = true, .write_transient = true, .copy_src = true},
        .size = (size_t)w * h * sizeof(uint32_t),
    });
    gfx.img = sg_make_image(&(sg_image_desc){
        .usage.copy_dst = true,
        .width = w,
        .height = h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
    });
    gfx.view = sg_make_view(&(sg_view_desc){.texture.image = gfx.img});
    gfx.img_w = w;
    gfx.img_h = h;
}

void gfx_setup(void)
{
    gfx.shd = gfx_make_shader();
    gfx.pip = sg_make_pipeline(&(sg_pipeline_desc){.shader = gfx.shd});
    gfx.nearest = gfx_make_sampler(SG_FILTER_NEAREST);
    gfx.linear = gfx_make_sampler(SG_FILTER_LINEAR);
    int cw, ch;
    canvas_size(&cw, &ch);
    gfx_make_image(cw, ch);
    frame_size(&gfx.frame_w, &gfx.frame_h);
    if (!overlay_active())
        host_window_set_aspect(gfx.frame_w, gfx.frame_h, false);
}

void gfx_canvas_changed(void)
{
#ifdef EMU_WITH_DEBUGGER
    if (gfx.resize_w)
    {
        host_window_resize(gfx.resize_w, gfx.resize_h);
        gfx.resize_w = gfx.resize_h = 0;
    }
#endif
    int fw, fh;
    frame_size(&fw, &fh);
    /* A new size at the same aspect, such as 320x240 after 640x480, needs
     * nothing of the window. */
    if ((long)fw * gfx.frame_h == (long)fh * gfx.frame_w)
        return;

    /* Whether the window's height is within 2 px of what the old frame aspect
     * gives its width, which says the user has not resized it off-aspect. A
     * re-fit rounds the height to a whole point, and a point is two pixels on a
     * 2x display. It has to be read before frame_w and frame_h are updated. */
    double off = sapp_height() - (double)sapp_width() * gfx.frame_h / gfx.frame_w;
    bool at_aspect = off < 2.0 && off > -2.0;

    gfx.frame_w = fw;
    gfx.frame_h = fh;
    /* The debugger's window never tracks the frame aspect. Native X11 window
     * managers honor the lock. WSLg ignores it, and the blit letterboxes there
     * instead.
     *
     * Only a window still at the old aspect is re-fitted; one the user resized
     * is left alone and letterboxed. There is no poll and snap to enforce it
     * afterwards, because WSLg restores its own geometry and drops resize
     * requests. */
    if (!overlay_active())
        host_window_set_aspect(fw, fh, at_aspect);
}

/* Computed by gfx_upload and read by gfx_blit, so a video frame fits its
 * viewport once rather than twice. */
static slbx_viewport canvas_vp;

void gfx_upload(bool new_frame)
{
    int cw, ch;
    canvas_size(&cw, &ch);

    /* The window tracks the frame aspect. RP6502 pixels are square, so the
     * canvas aspect is the display aspect, and a canvas the shape of the frame
     * fills the region. When the window is off-aspect, because the window
     * manager ignored the lock or a resize is in progress, or the canvas is not
     * the shape of a fixed frame, the difference becomes letterbox or pillarbox
     * against the clear color and nothing stretches. */
    canvas_vp = canvas_viewport();
    bool recreated = cw != gfx.img_w || ch != gfx.img_h;
    if (recreated)
    {
        gfx_destroy_image();
        gfx_make_image(cw, ch);
    }

    /* A duplicate present, which is what a display faster than 60 Hz asks for,
     * re-draws the texture it has instead of uploading again. A texture made
     * for a new canvas size has to be filled whatever the machine did. */
    if (new_frame || recreated)
    {
        sg_write_buffer_transient(&(sg_write_buffer_desc){
            .src.data = {.ptr = gfx.shown->pixels, .size = (size_t)cw * ch * sizeof(uint32_t)},
            .dst.buffer = gfx.staging,
        });
        sg_copy_buffer_to_image(&(sg_copy_buffer_to_image_desc){
            .src.buffer = gfx.staging,
            .dst.image = gfx.img,
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
    /* The texture holds nothing until the first upload, so skipping the blit
     * leaves the pass showing only the clear color. A platform overlay suppresses
     * the canvas the same way, and its own text then draws with the pass's
     * full-window viewport still in effect. */
    if (!gfx.ever_uploaded || canvas_vp.width <= 0 || canvas_vp.height <= 0 ||
        host_window_menu_active())
        return;
    sg_apply_viewport(canvas_vp.x, canvas_vp.y, canvas_vp.width, canvas_vp.height, true);
    sg_apply_pipeline(gfx.pip);
    sg_apply_bindings(&(sg_bindings){
        .views[0] = gfx.view,
        .samplers[0] = gfx.filter == GFX_FILTER_NEAREST ? gfx.nearest : gfx.linear,
    });
    gfx_fs_params_t params = {
        .size = {(float)gfx.img_w, (float)gfx.img_h},
        .scale = {1.0f, 1.0f},
    };
    if (gfx.filter == GFX_FILTER_SHARP)
    {
        const float sx = (float)canvas_vp.width / gfx.img_w;
        const float sy = (float)canvas_vp.height / gfx.img_h;
        params.scale[0] = sx > 1.0f ? sx : 1.0f;
        params.scale[1] = sy > 1.0f ? sy : 1.0f;
    }
    sg_apply_uniforms(0, &SG_RANGE(params));
    sg_draw(0, 3, 1);
}

void gfx_end_pass(void)
{
    sg_end_pass();
    sg_commit();
}

void gfx_shutdown(void)
{
    gfx_destroy_image();
    sg_destroy_pipeline(gfx.pip);
    sg_destroy_shader(gfx.shd);
    sg_destroy_sampler(gfx.nearest);
    sg_destroy_sampler(gfx.linear);
}

/* The window opens at the height --scale asks for and the width its frame
 * aspect gives, so a 4:3 frame opens 640x480 and a 16:9 frame opens wider.
 * A window manager may restore a previous size instead, which is fine: the init
 * callback sets the aspect lock and the blit letterboxes either way. */
void gfx_prepare(double scale, bool have_scale, int *out_w, int *out_h)
{
    int fw, fh;
    frame_size(&fw, &fh);
    int canvas_h = scaled_canvas_height(scale);
    int win_w = aspect_width(canvas_h, fw, fh);
    int win_h = canvas_h;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active())
    {
        /* The debugger's menu bar sits above the canvas, so the window opens
         * taller by the bar's height; without that the frame-aspect window
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
