/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/web/link.h"
#include "host/web/audio.h"
#include "host/sokol/app/seam_machine.h"
#include "osal/os.h"
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

EM_JS(int, js_link_machine, (void), {
    return Module.rp6502.role === 'machine' ? 1 : 0;
});

bool link_machine(void)
{
    static int machine = -1;
    if (machine < 0)
        machine = js_link_machine();
    return machine;
}

/* page.js keeps the batch and posts it once the script that added to it is
 * done. */
EM_JS(void, js_link_post, (int kind, const void *body, int len), {
    Module.rp6502.link.post(kind, HEAPU8.subarray(body, body + len));
});

bool link_post(seam_kind_t kind, const void *body, size_t len)
{
    if (len > SEAM_MOST)
        return false;
    js_link_post((int)kind, body, (int)len);
    return true;
}

/* ---------------- what the other half posted ---------------- */

/* A batch is the records seam.c keeps: a kind and a length, each 32 bits,
 * and the body padded to eight. The script copies one here and delivers it. */
static uint8_t *link_in;
static size_t link_in_size;

EMSCRIPTEN_KEEPALIVE uint8_t *link_inbox(size_t n)
{
    if (n > link_in_size)
    {
        free(link_in);
        link_in = malloc(n);
        link_in_size = link_in ? n : 0;
    }
    return link_in;
}

EMSCRIPTEN_KEEPALIVE void link_deliver(size_t n)
{
    for (size_t at = 0; at + 8 <= n;)
    {
        uint32_t head[2];
        memcpy(head, link_in + at, sizeof head);
        seam_deliver((seam_kind_t)head[0], link_in + at + 8, head[1]);
        at += 8 + ((head[1] + 7) & ~7u);
    }
}

/* ---------------- the page's frames ---------------- */

EMSCRIPTEN_KEEPALIVE uint32_t *link_frame_at(void)
{
    return run_web_frame_pixels();
}

/* The worker's machine is up, so the page's display callbacks mean
 * something to it from here on. */
EMSCRIPTEN_KEEPALIVE void link_up(void)
{
    run_web_up();
}

EMSCRIPTEN_KEEPALIVE void link_frame_done(int w, int h, int cw, int ch, int last)
{
    run_web_frame_done(w, h, cw, ch, last != 0);
}

EM_JS(void, js_link_frame, (const uint32_t *px, int w, int h, int cw, int ch, int last), {
    Module.rp6502.link.frame(HEAPU8.slice(px, px + w * h * 4).buffer, w, h, cw, ch, last);
});

void link_post_frame(const run_frame_t *f)
{
    js_link_frame(f->pixels, f->canvas_w, f->canvas_h, f->console_w, f->console_h, f->last);
}

/* ---------------- the worker's machine ---------------- */

/* The page's display callbacks are stamped on the page's clock, and the
 * worker's starts at another moment. */
EMSCRIPTEN_KEEPALIVE void link_clock(double page_minus_worker_ms)
{
    run_web_clock((int64_t)(page_minus_worker_ms * 1e6));
}

EM_JS(void, js_link_arm, (double ms), {
    Module.rp6502.link.arm(ms);
});

/* Whatever woke the machine: it runs what is due, plays what it made, and
 * sleeps until the next frame comes due or something wakes it sooner. */
EMSCRIPTEN_KEEPALIVE void link_wake(void)
{
    const uint64_t wait = run_web_step();
    web_audio_frame();
    js_link_arm((double)wait / 1e6);
}

EM_JS(void, js_link_running, (void), {
    Module.rp6502.running = true;
    postMessage({up: true});
});

int link_run(uint32_t *fb, bool exit_on_halt)
{
    js_link_running();
    run_prepare(fb, exit_on_halt);
    machine_set_window(true);
    web_audio_setup();
    run_start();
    link_wake();
    return 0;
}
