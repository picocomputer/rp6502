/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/prompt.h"
#include "host/sokol/app/input.h"
#include "core/hid/usage.h"
#include "core/hid/mouse.h"
#include "core/term/vterm_mouse.h"
#include "host/host.h"
#include "host/web/audio.h"
#include "sokol/sokol_app.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdint.h>

/* Firefox and Safari slow animation frames for a hidden or offscreen page
 * rather than stopping them. */
#define LIVE_MS 250.0

static int32_t away, pointer;
static double callback_ms;
static bool inited;

/* A cross-origin frame cannot read the page around it, but an
 * IntersectionObserver with no root measures the canvas against the top-level
 * viewport from inside any frame. While the page is away the machine does not
 * run and no sound is made, which the sound's queue rides out (audio.c). */
EM_JS(void, js_away_setup, (int32_t *away), {
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
});

/* pointer is 1 while the program maps the mouse, 2 while the tablet has
 * the pointer (input_pointer_to_tablet): a program maps it, or the console's
 * terminal claims it with no mouse mapped, and 4 while the tablet also uses
 * the wheel (input_wheel_to_tablet). An event that nothing uses is not
 * cancelled, so the page scrolls, zooms and shows its context menu. The wheel
 * is cancelled for a mouse program only while the pointer is locked.
 * sokol_app has no callback for contextmenu or for buttons above 3, so those
 * are cancelled here. */
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
    canvas.addEventListener('wheel', cancel(() => (used() & 4) || locked()), {signal});
    for (const type of ['touchstart', 'touchmove', 'touchend'])
        canvas.addEventListener(type, cancel(() => used() & 2), {signal});
});

/* touch-action applies from the start of a touch, so it is set when the
 * tablet gets or loses the pointer, before the next touch. */
EM_JS(void, js_touch_action, (int32_t used), {
    Module.canvas.style.touchAction = (used & 2) ? "none" : "";
});

/* The player box takes the frame's shape, and sokol_app reads the new canvas
 * size before this returns, so the next picture is fitted to the new box
 * rather than the old one. */
EM_JS(void, js_aspect, (int w, int h), {
    Module.rp6502.aspect(w, h);
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

/* Bit 0 is NumLock, bit 1 Caps Lock and bit 2 Scroll Lock, as the page last
 * saw them in a key or pointer event. */
EM_JS(int, js_locks, (void), {
    return Module.rp6502.locks();
});

uint8_t host_locks(void)
{
    const int seen = js_locks();
    uint8_t locks = 0;
    if (seen & 1)
        locks |= KEYBOARD_LED_NUMLOCK;
    if (seen & 2)
        locks |= KEYBOARD_LED_CAPSLOCK;
    if (seen & 4)
        locks |= KEYBOARD_LED_SCROLLLOCK;
    return locks;
}

/* Whether a key down is left to the browser, which then makes the keypress
 * that types its character, or the paste event of a paste key. The keypad's
 * digits are characters only with NumLock on and Shift up. */
static bool entry_key_types(const sapp_event *e)
{
    /* A paste key makes the paste event that pastes (input.c); the keys
     * input_key_withheld names are the player's alone. */
    if (input_paste_key(e))
        return true;
    if (input_key_withheld(e))
        return false;
    if (e->key_code < SAPP_KEYCODE_WORLD_1)
        return true;
    if ((e->key_code >= SAPP_KEYCODE_KP_DIVIDE && e->key_code <= SAPP_KEYCODE_KP_ADD) ||
        e->key_code == SAPP_KEYCODE_KP_EQUAL)
        return true;
    if (e->key_code >= SAPP_KEYCODE_KP_0 && e->key_code <= SAPP_KEYCODE_KP_DECIMAL)
        return (host_locks() & KEYBOARD_LED_NUMLOCK) && !(e->modifiers & SAPP_MODIFIER_SHIFT);
    return false;
}

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
        if (e->type == SAPP_EVENTTYPE_CHAR || !entry_key_types(e))
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
    web_audio_setup();
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
    /* The player's own focus, which the page keeps for each player on it,
     * is told to a program that asks (?1004) as it changes. */
    vterm_focus(js_focused());
    app_frame();
    web_audio_frame();
    const int32_t used = (mouse_is_mapped() ? 1 : 0) | (input_pointer_to_tablet() ? 2 : 0) |
                         (input_wheel_to_tablet() ? 4 : 0);
    if (used != pointer)
    {
        pointer = used;
        js_touch_action(used);
    }
}

/* The page sizes the box, so there is no window to re-fit. */
void host_window_set_aspect(int w, int h, bool refit)
{
    (void)refit;
    js_aspect(w, h);
}

void host_window_init(void)
{
    js_away_setup(&away);
    js_pointer_setup(&pointer);
}
bool host_window_menu_active(void) { return false; }
void host_window_menu_draw(void) {}
void host_window_files_dropped(void) {}
void host_window_open_url(const char *url) { (void)url; }
bool entry_wait_for_rom(void) { return false; }

/* The clipboard is the browser's. A page cannot ask for it: the browser
 * hands its text over only in a paste event, where input.c pastes it. So
 * there is none to give here. A copy writes it. */
const char *host_clipboard_get(void)
{
    return NULL;
}

EM_JS_DEPS(entry_clipboard, "$UTF8ToString");
EM_JS(int, js_clipboard_write, (const char *utf8), {
    if (!navigator.clipboard)
        return 0;
    navigator.clipboard.writeText(UTF8ToString(utf8)).catch(() => {});
    return 1;
});

/* A page served without a secure context, as over plain http, has no
 * clipboard to write: then this is false, and the copy goes to the
 * machine's buffer (clip.h), though a paste key still pastes the browser's
 * clipboard. */
bool host_clipboard_set(const char *utf8)
{
    return js_clipboard_write(utf8) != 0;
}

/* The browser's platform: a Mac, or an iPhone or iPad with a keyboard. */
EM_JS(int, js_mac_keys, (void), {
    const p = (navigator.userAgentData && navigator.userAgentData.platform) ||
              navigator.platform || "";
    return /^(mac|iphone|ipad)/i.test(p) ? 1 : 0;
});

bool host_mac_keys(void)
{
    static int mac = -1;
    if (mac < 0)
        mac = js_mac_keys();
    return mac;
}

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
