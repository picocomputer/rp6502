/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/prompt.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "sokol/sokol_app.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdint.h>

/* Firefox and Safari slow animation frames for a hidden or offscreen page
 * rather than stopping them. */
#define LIVE_MS 250.0

static int32_t away, pointer;
static double callback_ms, run_ms;
static bool inited;

/* A cross-origin frame cannot read the page around it, but an
 * IntersectionObserver with no root measures the canvas against the top-level
 * viewport from inside any frame. The audio callback keeps firing after
 * animation frames stop, so it pulls only while the machine is running, and
 * each start or stop is ramped over one buffer because a step clicks. */
EM_JS(void, js_away_setup, (int32_t *away, const double *run_ms, double live_ms), {
    const signal = Module.rp6502.signal;
    if (signal.aborted)
        return;
    let inView = true;
    const update = () => { HEAP32[away >> 2] = document.hidden || !inView; };
    document.addEventListener('visibilitychange', update, {signal});
    const io = new IntersectionObserver((entries) => {
        inView = entries[entries.length - 1].isIntersecting;
        update();
    });
    io.observe(Module.canvas);
    signal.addEventListener('abort', () => io.disconnect());
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

/* pointer is 1 while the program maps the mouse and 2 while it maps the
 * tablet. An event that the program does not use is not cancelled, so the page
 * scrolls, zooms and shows its context menu. The wheel is cancelled for a
 * mouse program only while the pointer is locked. sokol_app has no callback
 * for contextmenu or for buttons above 3, so those are cancelled here. */
EM_JS(void, js_pointer_setup, (const int32_t *pointer), {
    const signal = Module.rp6502.signal;
    const canvas = Module.canvas;
    const used = () => HEAP32[pointer >> 2];
    const locked = () => canvas.getRootNode().pointerLockElement === canvas;
    const cancel = (test) => (e) => {
        if (test())
            e.preventDefault();
    };
    for (const type of ['mousedown', 'mouseup', 'contextmenu'])
        canvas.addEventListener(type, cancel(used), {signal});
    canvas.addEventListener('wheel', cancel(() => (used() & 2) || locked()), {signal});
    for (const type of ['touchstart', 'touchmove', 'touchend'])
        canvas.addEventListener(type, cancel(() => used() & 2), {signal});
});

/* touch-action applies from the start of a touch, so it is set when the
 * program maps or unmaps the tablet, before the next touch. */
EM_JS(void, js_touch_action, (int32_t used), {
    Module.canvas.style.touchAction = (used & 2) ? "none" : "";
});

/* After destroy(), app_init fails on the lost WebGL context, and app_frame
 * must not run again. */
EM_JS(int, js_destroyed, (void), {
    return Module.rp6502.signal.aborted;
});

EM_JS(int, js_focused, (void), {
    return Module.rp6502.focused();
});

/* In a shadow root, document.pointerLockElement is the host element. */
EM_JS(int, js_canvas_locked, (void), {
    return Module.canvas.getRootNode().pointerLockElement === Module.canvas;
});

bool entry_canvas_locked(void) { return js_canvas_locked(); }

/* sokol_app adds its key and paste listeners to window, once for each player
 * on the page. Only the player with the focus handles these events and cancels
 * their default action. A key up is always handled, so a key held while the
 * focus moves is released. */
static void entry_input(const sapp_event *e)
{
    if (!inited)
        return;
    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
    case SAPP_EVENTTYPE_CHAR:
    case SAPP_EVENTTYPE_CLIPBOARD_PASTED:
        if (!js_focused())
            return;
        /* A cancelled keydown of a character key makes no keypress, so no
         * CHAR event. */
        if (e->type == SAPP_EVENTTYPE_CHAR ||
            (e->type == SAPP_EVENTTYPE_KEY_DOWN && e->key_code >= SAPP_KEYCODE_WORLD_1))
            sapp_consume_event();
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if (e->key_code >= SAPP_KEYCODE_WORLD_1 && js_focused())
            sapp_consume_event();
        break;
    default:
        break;
    }
    app_input(e);
}

static void entry_init(void)
{
    if (js_destroyed())
        return;
    app_init();
    inited = true;
}

static void entry_cleanup(void)
{
    if (inited)
        app_cleanup();
}

static void entry_frame(void)
{
    if (!inited || js_destroyed())
        return;
    const double now = emscripten_performance_now();
    const double gap = now - callback_ms;
    callback_ms = now;
    if (away || gap >= LIVE_MS)
        return;
    run_ms = now;
    app_frame();
    const int32_t used = (mouse_is_mapped() ? 1 : 0) | (tablet_is_mapped() ? 2 : 0);
    if (used != pointer)
    {
        pointer = used;
        js_touch_action(used);
    }
}

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
void host_window_init(void)
{
    js_away_setup(&away, &run_ms, LIVE_MS);
    js_pointer_setup(&pointer);
}
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
        .init_cb = entry_init,
        .frame_cb = entry_frame,
        .event_cb = entry_input,
        .cleanup_cb = entry_cleanup,
        .width = win_w,
        .height = win_h,
        .swap_interval = 1,
        .enable_clipboard = true,
        .clipboard_size = 65536,
        .logger.func = app_log,
        /* entry_input and js_pointer_setup cancel the events that the
         * program uses. */
        .html5 = {
            .bubble_mouse_events = true,
            .bubble_touch_events = true,
            .bubble_wheel_events = true,
            .bubble_key_events = true,
            .bubble_char_events = true,
        },
    });
    return app_exit_code();
}
