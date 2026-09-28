/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "sokol/sokol_app.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdint.h>

/* Firefox and Safari slow animation frames for a hidden or offscreen page
 * rather than stopping them. */
#define LIVE_MS 250.0

static int32_t away;
static double callback_ms, run_ms;

/* A cross-origin frame cannot read the page around it, but an
 * IntersectionObserver with no root measures the canvas against the top-level
 * viewport from inside any frame. The audio callback keeps firing after
 * animation frames stop, so it pulls only while the machine is running, and
 * each start or stop is ramped over one buffer because a step clicks. */
EM_JS(void, js_away_setup, (int32_t *away, const double *run_ms, double live_ms), {
    let inView = true;
    const update = () => { HEAP32[away >> 2] = document.hidden || !inView; };
    document.addEventListener('visibilitychange', update);
    new IntersectionObserver((entries) => {
        inView = entries[entries.length - 1].isIntersecting;
        update();
    }).observe(Module.canvas);
    update();

    const node = Module._saudio_node;
    if (!node)
        return;
    const pull = node.onaudioprocess;
    const last = [0, 0];
    let held = false;
    node.onaudioprocess = (e) => {
        // saudio_shutdown frees the buffer a pull writes, and an event
        // queued before the shutdown can still arrive after it.
        if (Module._saudio_node !== node)
            return;
        const out = e.outputBuffer;
        const live = !HEAP32[away >> 2] && performance.now() - HEAPF64[run_ms >> 3] < live_ms;
        if (live)
            pull(e);
        for (let c = 0; c < out.numberOfChannels; c++) {
            const d = out.getChannelData(c);
            const n = d.length;
            if (!live) {
                for (let i = 0; i < n; i++)
                    d[i] = last[c] * (n - 1 - i) / n;
                last[c] = 0;
            } else {
                if (held)
                    for (let i = 0; i < n; i++)
                        d[i] *= i / n;
                last[c] = d[n - 1];
            }
        }
        held = !live;
    };
});

static void entry_frame(void)
{
    const double now = emscripten_performance_now();
    const double gap = now - callback_ms;
    callback_ms = now;
    if (away || gap >= LIVE_MS)
        return;
    run_ms = now;
    app_frame();
}

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
void host_window_init(void) { js_away_setup(&away, &run_ms, LIVE_MS); }
bool host_window_menu_active(void) { return false; }
void host_window_menu_draw(void) {}
void host_window_files_dropped(void) {}
void host_window_open_url(const char *url) { (void)url; }
bool entry_wait_for_rom(void) { return false; }

int entry_run(uint32_t *fb, double scale, bool have_scale, bool exit_on_halt)
{
    int win_w, win_h;
    app_prepare(fb, scale, have_scale, exit_on_halt, &win_w, &win_h);
    sapp_run(&(sapp_desc){
        .init_cb = app_init,
        .frame_cb = entry_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .width = win_w,
        .height = win_h,
        .swap_interval = 1,
        .enable_clipboard = true,
        .clipboard_size = 65536,
        .logger.func = app_log,
    });
    return app_exit_code();
}
