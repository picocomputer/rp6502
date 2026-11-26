/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The web player's sound, on the page's AudioContext (page.js makes it and
 * resumes it on a click).
 *
 * The machine runs on the page's thread, so the sound is made there too:
 * after each display callback the frames' sound is taken (aud_render, the
 * mixer's pull mode) and posted to an AudioWorklet on the browser's audio
 * thread, which plays from a queue of its own. The worklet never waits on
 * the page, and the queue covers the page's callbacks coming late. The page
 * keeps the queue at WEB_AUDIO_TARGET by stretching the sound a fraction of
 * a percent (aud_set_stretch), in proportion to how far it is off: the
 * dynamic rate control of RetroArch, with no integral to wind up.
 *
 * A queue that runs dry, as it does while the page is hidden and the
 * machine stops, ramps to silence over what is left of a quantum, and plays
 * again only once it holds the target, ramping in over its first quantum,
 * since a step clicks. One that grows past four times the target, after the
 * page stalled and caught up, lets its oldest sound go. Nothing is queued
 * while the context is suspended.
 *
 * A page without a secure context has no AudioWorklet, and plays from a
 * ScriptProcessorNode on the page's own thread instead, from the same queue,
 * in blocks of WEB_AUDIO_SCRIPT frames. */

#include "host/web/audio.h"
#include "core/aud/mix.h"
#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>

/* What the queue is kept at as a frame's sound comes, its least, and the
 * block a ScriptProcessorNode plays in, in the context's frames at 48 kHz:
 * 12.5 ms, which covers the page's callbacks coming late, and 21 ms. */
#define WEB_AUDIO_TARGET 600
#define WEB_AUDIO_SCRIPT 1024

/* The most the stretch is, and the fill off the target that takes it. */
#define WEB_AUDIO_STRETCH 0.005

/* A frame or a few of sound at the context's rate, as the pull mode takes
 * it. */
#define WEB_AUDIO_MAX 4096
static float web_audio_buf[WEB_AUDIO_MAX * 2];
static bool web_audio_on;
static double web_audio_fill = -1;

EM_JS(int, js_audio_setup, (int target, int script), {
    const ctx = Module.rp6502.audio;
    if (!ctx)
        return 0;
    const signal = Module.rp6502.signal;
    const a = Module.rp6502.sound = {
        ctx, port: null, pushed: 0, played: 0, dry: 0, playing: false, t: 0, target, script,
        stats: { at: performance.now(), dry: 0 },
    };
    // The worklet's source, as one function's text, so the player needs no
    // file of its own beside it.
    const source = () => {
        class Out extends AudioWorkletProcessor {
            constructor(options) {
                super();
                this.chunks = [];
                this.at = 0;
                this.queued = 0;
                this.played = 0;
                this.dry = 0;
                this.target = options.processorOptions.target;
                this.playing = false;
                this.rising = false;
                this.last = [0, 0];
                this.quanta = 0;
                // Past four times the target the oldest goes, so the sound is
                // never far behind the machine; what goes counts as played.
                this.port.onmessage = (e) => {
                    this.chunks.push(e.data);
                    this.queued += e.data.length / 2;
                    while (this.queued - (this.chunks[0].length - this.at) / 2 >= 4 * this.target) {
                        const gone = (this.chunks.shift().length - this.at) / 2;
                        this.queued -= gone;
                        this.played += gone;
                        this.at = 0;
                    }
                };
            }
            process(inputs, outputs) {
                const out = outputs[0];
                const L = out[0], R = out[1] || out[0], n = L.length;
                if (!this.playing && this.queued >= this.target)
                    this.playing = this.rising = true;
                let i = 0;
                if (this.playing) {
                    while (i < n && this.chunks.length) {
                        const c = this.chunks[0];
                        for (; i < n && this.at < c.length; this.at += 2, i++) {
                            L[i] = c[this.at];
                            R[i] = c[this.at + 1];
                        }
                        if (this.at >= c.length) {
                            this.chunks.shift();
                            this.at = 0;
                        }
                    }
                    if (this.rising)
                        for (let k = 0; k < i; k++) {
                            L[k] *= k / n;
                            R[k] *= k / n;
                        }
                    this.rising = false;
                    this.queued -= i;
                    this.played += i;
                    if (i) {
                        this.last[0] = L[i - 1];
                        this.last[1] = R[i - 1];
                    }
                }
                if (i < n) {
                    const m = n - i;
                    for (let k = 0; k < m; k++) {
                        const g = (m - 1 - k) / m;
                        L[i + k] = this.last[0] * g;
                        R[i + k] = this.last[1] * g;
                    }
                    if (this.playing)
                        this.dry++;
                    this.last[0] = this.last[1] = 0;
                    this.playing = false;
                }
                // What has played goes back to the page every 8 quanta,
                // about 21 ms.
                if (++this.quanta % 8 === 0)
                    this.port.postMessage([this.played, this.dry, this.playing, currentTime]);
                return true;
            }
        }
        registerProcessor('rp6502-out', Out);
    };
    // What the worklet had played at the context's time t, and whether it is
    // playing, so the page can count on from there.
    const heard = (e) => {
        [a.played, a.dry, a.playing, a.t] = e.data;
    };
    if (ctx.audioWorklet && typeof AudioWorkletNode === 'function') {
        const url = URL.createObjectURL(new Blob(['(' + source + ')()'], {type: 'text/javascript'}));
        ctx.audioWorklet.addModule(url).then(() => {
            URL.revokeObjectURL(url);
            if (signal.aborted)
                return;
            const node = new AudioWorkletNode(ctx, 'rp6502-out', {
                numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
                processorOptions: {target},
            });
            node.port.onmessage = heard;
            node.connect(ctx.destination);
            a.port = node.port;
            signal.addEventListener('abort', () => node.disconnect(), {once: true});
        }).catch((err) => console.error('rp6502: no AudioWorklet:', err));
    } else {
        // The same queue, played from the page's thread.
        const node = ctx.createScriptProcessor(script, 0, 2);
        const q = {chunks: [], at: 0, queued: 0, playing: false, rising: false, last: [0, 0]};
        a.port = {
            postMessage: (chunk) => {
                q.chunks.push(chunk);
                q.queued += chunk.length / 2;
                while (q.queued - (q.chunks[0].length - q.at) / 2 >= 4 * target) {
                    const gone = (q.chunks.shift().length - q.at) / 2;
                    q.queued -= gone;
                    a.played += gone;
                    q.at = 0;
                }
            },
        };
        node.onaudioprocess = (e) => {
            const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1);
            const n = L.length;
            if (!q.playing && q.queued >= target)
                q.playing = q.rising = true;
            let i = 0;
            if (q.playing) {
                while (i < n && q.chunks.length) {
                    const c = q.chunks[0];
                    for (; i < n && q.at < c.length; q.at += 2, i++) {
                        L[i] = c[q.at];
                        R[i] = c[q.at + 1];
                    }
                    if (q.at >= c.length) {
                        q.chunks.shift();
                        q.at = 0;
                    }
                }
                if (q.rising)
                    for (let k = 0; k < i; k++) {
                        L[k] *= k / n;
                        R[k] *= k / n;
                    }
                q.rising = false;
                q.queued -= i;
                a.played += i;
                a.t = ctx.currentTime;
                if (i) {
                    q.last[0] = L[i - 1];
                    q.last[1] = R[i - 1];
                }
            }
            if (i < n) {
                const m = n - i;
                for (let k = 0; k < m; k++) {
                    const g = (m - 1 - k) / m;
                    L[i + k] = q.last[0] * g;
                    R[i + k] = q.last[1] * g;
                }
                if (q.playing)
                    a.dry++;
                q.last[0] = q.last[1] = 0;
                q.playing = false;
            }
            a.playing = q.playing;
        };
        node.connect(ctx.destination);
        signal.addEventListener('abort', () => node.disconnect(), {once: true});
    }
    return ctx.sampleRate;
});

/* The frames' sound, posted; and how many frames the queue holds now. */
EM_JS(int, js_audio_push, (const float *buf, int frames), {
    const a = Module.rp6502.sound;
    if (!a || !a.port)
        return -1;
    // A context not yet resumed, or suspended, plays nothing, and nothing is
    // queued for it.
    if (a.ctx.state !== 'running') {
        a.pushed = a.played;
        a.playing = false;
        return -1;
    }
    // The queue as it stands before this frame's sound: what was posted less
    // what had played at the last word from the player, and what a playing
    // player has played since.
    const since = a.playing ? Math.max(0, a.ctx.currentTime - a.t) * a.ctx.sampleRate : 0;
    const fill = Math.max(0, Math.round(a.pushed - a.played - since));
    if (frames > 0) {
        a.port.postMessage(HEAPF32.slice(buf >> 2, (buf >> 2) + frames * 2));
        a.pushed += frames;
    }
    // A page that wants to watch the queue, as the developer's run page does
    // (.vscode/run.py), is told once a second.
    const now = performance.now();
    if (typeof globalThis.rp6502_audio_stats === 'function' && now - a.stats.at >= 1000) {
        globalThis.rp6502_audio_stats({fill, dry: a.dry, rate: a.ctx.sampleRate,
                                       target: a.target, worklet: !!a.ctx.audioWorklet});
        a.stats.at = now;
    }
    return fill;
});

void web_audio_setup(void)
{
    if (!aud_enabled())
        return;
    const int rate = js_audio_setup(WEB_AUDIO_TARGET, WEB_AUDIO_SCRIPT);
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
    const int fill = js_audio_push(web_audio_buf, made);
    if (fill < 0)
        return;
    /* The queue rises a frame's worth at each post and falls between, so the
     * fill the stretch goes by is smoothed over about a second. */
    web_audio_fill = web_audio_fill < 0 ? fill : web_audio_fill + (fill - web_audio_fill) / 60;
    double off = (web_audio_fill - WEB_AUDIO_TARGET) / WEB_AUDIO_TARGET;
    if (off > 1)
        off = 1;
    if (off < -1)
        off = -1;
    aud_set_stretch(off * WEB_AUDIO_STRETCH);
}
