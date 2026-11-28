/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The sound on Linux (entry.h): through PipeWire's own API (pipewire.c)
 * when the Pulse server is PipeWire, else through PulseAudio's.
 *
 * The Pulse server the stream would play on is asked what it is, so that
 * what is installed beside it never decides: PipeWire's Pulse protocol adds
 * 16 ms that its own API does not, and a PipeWire installed beside a real
 * PulseAudio, or with no sound card, as under WSLg, is not where the sound
 * goes. With no Pulse server, PipeWire is tried, and if PipeWire will not
 * play, Pulse is, waiting for a server to come.
 *
 * The stream asks the server for AUDIO_TARGET of latency, 512 frames at
 * 48 kHz, 10.7 ms, and lets the server size the sink's own latency to suit
 * (PA_STREAM_ADJUST_LATENCY). The server calls for sound on its mainloop's
 * thread as it wants it, and the callback makes it on the spot. 512 frames
 * held to a 10 ms period, as WASAPI's, ran without the server running dry;
 * 256 did not beside a compiler. A stream named to no device plays on the
 * default one, and moves with it.
 *
 * When the server goes away, as it does when it restarts, the thread
 * connects again once a second. */

#include "core/sys/debug_log.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/linux/pipewire.h"
#include <pulse/pulseaudio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AUDIO_RATE 48000
#define AUDIO_TARGET 512
/* The most frames made at once, which one call for sound never comes near. */
#define AUDIO_MOST 4096

static struct
{
    void (*render)(float *dst, int frames, uint32_t rate);
    pa_threaded_mainloop *loop;
    pa_context *context;
    pa_stream *stream;
    pa_time_event *retry;
    bool stopping;
    bool pipewire;
    float stereo[AUDIO_MOST * 2];
} audio;

static void audio_connect(bool first);

/* The server wants nbytes: frames made and written, in pieces the stream's
 * own buffer can take. */
static void audio_write(pa_stream *s, size_t nbytes, void *user)
{
    (void)user;
    const size_t frame = 2 * sizeof(float);
    while (nbytes >= frame)
    {
        void *buf = NULL;
        size_t n = nbytes;
        if (pa_stream_begin_write(s, &buf, &n) < 0 || !buf)
            return;
        size_t frames = n / frame;
        if (frames > AUDIO_MOST)
            frames = AUDIO_MOST;
        if (!frames)
        {
            pa_stream_cancel_write(s);
            return;
        }
        audio.render(audio.stereo, (int)frames, AUDIO_RATE);
        memcpy(buf, audio.stereo, frames * frame);
        pa_stream_write(s, buf, frames * frame, NULL, 0, PA_SEEK_RELATIVE);
        nbytes -= frames * frame;
    }
}

static void audio_retry(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv, void *user)
{
    (void)tv, (void)user;
    api->time_free(e);
    audio.retry = NULL;
    if (!audio.stopping)
        audio_connect(false);
}

/* Gone: the stream and the context let go, and a connection tried again in
 * a second. */
static void audio_lost(void)
{
    if (audio.stream)
    {
        pa_stream_set_write_callback(audio.stream, NULL, NULL);
        pa_stream_set_state_callback(audio.stream, NULL, NULL);
        pa_stream_unref(audio.stream);
        audio.stream = NULL;
    }
    if (audio.context)
    {
        pa_context_set_state_callback(audio.context, NULL, NULL);
        pa_context_disconnect(audio.context);
        pa_context_unref(audio.context);
        audio.context = NULL;
    }
    if (!audio.stopping && !audio.retry)
    {
        pa_mainloop_api *api = pa_threaded_mainloop_get_api(audio.loop);
        struct timeval tv;
        pa_gettimeofday(&tv);
        pa_timeval_add(&tv, 1000000);
        audio.retry = api->time_new(api, &tv, audio_retry, NULL);
    }
}

static void audio_stream_state(pa_stream *s, void *user)
{
    (void)user;
    if (pa_stream_get_state(s) == PA_STREAM_FAILED)
        audio_lost();
}

static void audio_context_state(pa_context *c, void *user)
{
    (void)user;
    switch (pa_context_get_state(c))
    {
    case PA_CONTEXT_READY:
    {
        const pa_sample_spec spec = {.format = PA_SAMPLE_FLOAT32NE, .rate = AUDIO_RATE, .channels = 2};
        const uint32_t bytes = AUDIO_TARGET * 2 * sizeof(float);
        const pa_buffer_attr attr = {
            .maxlength = (uint32_t)-1,
            .tlength = bytes,
            .prebuf = (uint32_t)-1,
            .minreq = (uint32_t)-1,
            .fragsize = (uint32_t)-1,
        };
        audio.stream = pa_stream_new(c, "Picocomputer 6502", &spec, NULL);
        if (!audio.stream)
        {
            audio_lost();
            return;
        }
        pa_stream_set_state_callback(audio.stream, audio_stream_state, NULL);
        pa_stream_set_write_callback(audio.stream, audio_write, NULL);
        if (pa_stream_connect_playback(audio.stream, NULL, &attr,
                                       PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE |
                                           PA_STREAM_INTERPOLATE_TIMING,
                                       NULL, NULL) < 0)
            audio_lost();
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        audio_lost();
        break;
    default:
        break;
    }
}

/* The first connection may start a server, as a client does; the tries
 * after it only look for one, since a server that will not start once will
 * not start each second. */
static void audio_connect(bool first)
{
    pa_mainloop_api *api = pa_threaded_mainloop_get_api(audio.loop);
    audio.context = pa_context_new(api, "Picocomputer 6502");
    if (!audio.context)
    {
        audio_lost();
        return;
    }
    pa_context_set_state_callback(audio.context, audio_context_state, NULL);
    if (pa_context_connect(audio.context, NULL, first ? PA_CONTEXT_NOFLAGS : PA_CONTEXT_NOAUTOSPAWN,
                           NULL) < 0)
        audio_lost();
}

static bool audio_pulse_open(void)
{
    audio.loop = pa_threaded_mainloop_new();
    if (!audio.loop)
        return false;
    pa_threaded_mainloop_lock(audio.loop);
    audio_connect(true);
    const bool started = pa_threaded_mainloop_start(audio.loop) >= 0;
    pa_threaded_mainloop_unlock(audio.loop);
    if (!started)
    {
        pa_threaded_mainloop_free(audio.loop);
        audio.loop = NULL;
    }
    return started;
}

static void audio_server_info(pa_context *c, const pa_server_info *info, void *user)
{
    (void)c;
    /* libpulse calls back with no info when the request fails, which is no
     * answer. */
    if (!info || !info->server_name)
        return;
    RP6502_LOG(audio, INFO, "Pulse server is %s", info->server_name);
    *(int *)user = strstr(info->server_name, "PipeWire") ? 1 : 0;
}

/* Whether the Pulse server is PipeWire: 1 if it is, 0 if it is not, -1 if
 * none answers. Asked on a mainloop of its own, ahead of the driver's, and
 * the way the driver first connects, so a server that one would start, this
 * starts. */
static int audio_server_is_pipewire(void)
{
    int is = -1;
    pa_mainloop *loop = pa_mainloop_new();
    pa_context *c = pa_context_new(pa_mainloop_get_api(loop), "Picocomputer 6502");
    pa_operation *op = NULL;
    pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL);
    for (;;)
    {
        const pa_context_state_t state = pa_context_get_state(c);
        if (!PA_CONTEXT_IS_GOOD(state))
            break;
        if (state == PA_CONTEXT_READY && !op)
            op = pa_context_get_server_info(c, audio_server_info, &is);
        if (op && pa_operation_get_state(op) != PA_OPERATION_RUNNING)
            break;
        pa_mainloop_iterate(loop, 1, NULL);
    }
    if (op)
        pa_operation_unref(op);
    pa_context_disconnect(c);
    pa_context_unref(c);
    pa_mainloop_free(loop);
    return is;
}

bool host_audio_open(void (*render)(float *dst, int frames, uint32_t rate))
{
    audio.render = render;
    const int is = audio_server_is_pipewire();
    if (is)
    {
        audio.pipewire = pipewire_open(render);
        if (audio.pipewire)
        {
            RP6502_LOG(audio, INFO, "PipeWire, %s",
                       is > 0 ? "the Pulse server's own" : "with no Pulse server");
            return true;
        }
        RP6502_LOG(audio, WARN, "PipeWire would not play, so Pulse");
    }
    else
        RP6502_LOG(audio, INFO, "PulseAudio, the Pulse server");
    return audio_pulse_open();
}

void host_audio_close(void)
{
    if (audio.pipewire)
    {
        pipewire_close();
        return;
    }
    if (!audio.loop)
        return;
    pa_threaded_mainloop_lock(audio.loop);
    audio.stopping = true;
    if (audio.retry)
    {
        pa_threaded_mainloop_get_api(audio.loop)->time_free(audio.retry);
        audio.retry = NULL;
    }
    audio_lost();
    pa_threaded_mainloop_unlock(audio.loop);
    pa_threaded_mainloop_stop(audio.loop);
    pa_threaded_mainloop_free(audio.loop);
    audio.loop = NULL;
}
