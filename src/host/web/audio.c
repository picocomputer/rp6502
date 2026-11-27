/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The web player's sound, made by the worker's machine (link.h) and played by
 * an AudioWorklet on the page's AudioContext, which page.js makes and resumes
 * on a click. The two talk over a port of their own, so the sound never waits
 * on the page.
 *
 * After each step that ran frames, their sound is taken (aud_render, the
 * mixer's pull mode) and posted to the worklet, which plays from a queue of
 * its own on the browser's audio thread. The worklet tells the worker how
 * much it holds after every quantum it plays, so the worker knows the queue
 * as it is rather than guessing it from the clock. The queue's least over
 * half a second is kept at WEB_AUDIO_FLOOR by stretching the sound a fraction
 * of a percent (aud_set_stretch), in proportion to how far it is off: the
 * dynamic rate control of RetroArch, with no integral to wind up. The least
 * is what matters: it is what is left just before the next frame's sound
 * comes, so it is the margin against a frame that comes late.
 *
 * A queue that runs dry ramps to silence over what is left of a quantum, and
 * plays again once it holds the floor, ramping in over its first quantum,
 * since a step clicks. One that grows past WEB_AUDIO_MOST, after the machine
 * caught up on a stall, lets its oldest sound go. Nothing is queued while the
 * context is not running. */

#include "host/web/audio.h"
#include "core/aud/mix.h"
#include "osal/os.h"
#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>

/* The queue's least and its most, in the context's frames at 48 kHz: 5.3 ms,
 * which covers a frame's sound coming a little late, and 100 ms. A browser
 * plays a device's period at a time, 10 ms on Windows, and the least is
 * measured after that, so the floor needs no room for it. */
#define WEB_AUDIO_FLOOR 256
#define WEB_AUDIO_MOST 4800

/* The most the stretch is, and how far off the floor, in frames, takes it. */
#define WEB_AUDIO_STRETCH 0.005
#define WEB_AUDIO_SPAN 512.0

/* How long each least is taken over. */
#define WEB_AUDIO_WINDOW_NS 500000000ull

/* A frame or a few of sound at the context's rate, as the pull mode takes
 * it. */
#define WEB_AUDIO_MAX 4096
static float web_audio_buf[WEB_AUDIO_MAX * 2];
static bool web_audio_on;
static int web_audio_least = -1;
static uint64_t web_audio_since;

/* The context's rate, which page.js gave the worker in its settings, or 0
 * for a page with no sound. */
EM_JS(int, js_audio_rate, (void), {
    return Module.rp6502.sound?.rate || 0;
});

/* The frames' sound, posted to the worklet while its context runs; and the
 * queue's least since the last call, or -1 when the worklet has not said or
 * its context is not running. */
EM_JS(int, js_audio_push, (const float *buf, int frames), {
    const s = Module.rp6502.sound;
    if (!s.running)
        return -1;
    if (frames > 0)
        s.push(HEAPF32.subarray(buf >> 2, (buf >> 2) + frames * 2), frames);
    const low = s.low;
    s.low = -1;
    return low;
});

void web_audio_setup(void)
{
    if (!aud_enabled())
        return;
    const int rate = js_audio_rate();
    if (!rate)
        return;
    aud_set_sink_rate((uint32_t)rate);
    aud_set_streaming(false);
    web_audio_on = true;
}

void web_audio_frame(void)
{
    if (!web_audio_on)
        return;
    const int made = aud_render(web_audio_buf, WEB_AUDIO_MAX);
    const int low = js_audio_push(web_audio_buf, made);
    if (low < 0)
        return;
    if (web_audio_least < 0 || low < web_audio_least)
        web_audio_least = low;
    const uint64_t now = os_mono_ns();
    if (now - web_audio_since < WEB_AUDIO_WINDOW_NS)
        return;
    web_audio_since = now;
    double off = (web_audio_least - WEB_AUDIO_FLOOR) / WEB_AUDIO_SPAN;
    web_audio_least = -1;
    if (off > 1)
        off = 1;
    if (off < -1)
        off = -1;
    aud_set_stretch(off * WEB_AUDIO_STRETCH);
}

EMSCRIPTEN_KEEPALIVE int web_audio_floor(void)
{
    return WEB_AUDIO_FLOOR;
}

EMSCRIPTEN_KEEPALIVE int web_audio_most(void)
{
    return WEB_AUDIO_MOST;
}
