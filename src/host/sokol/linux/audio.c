/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The sound on Linux, through PulseAudio's own API, which PipeWire also
 * serves (entry.h).
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

#include "host/sokol/app/entry.h"
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

bool host_audio_open(void (*render)(float *dst, int frames, uint32_t rate))
{
    audio.render = render;
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

void host_audio_close(void)
{
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
