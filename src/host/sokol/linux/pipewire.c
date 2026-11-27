/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The sound on Linux through PipeWire's own API (pipewire.h).
 *
 * The Pulse protocol that PipeWire also serves holds a stream to at least
 * 768 frames of buffer, 16 ms, before the graph's own quantum. A stream of
 * PipeWire's own is a node in the graph: its process callback runs on the
 * graph's realtime thread each cycle (PW_STREAM_FLAG_RT_PROCESS) and fills
 * the buffer it is handed with what the cycle asks for, so what it adds is
 * a quantum.
 *
 * libpipewire is loaded when it is chosen, not linked, so the emulator
 * starts on a machine without it. The spa headers are macros and inline
 * functions, built in; only the pw_ functions below come from the library.
 *
 * When the daemon goes away, as it does when it restarts, the stream is
 * made again once a second, as the Pulse driver does. */

#include "host/sokol/linux/pipewire.h"
#include <dlfcn.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#define PIPEWIRE_RATE 48000
/* The quantum asked of the graph: 480 frames at 48 kHz, 10 ms, the period
 * the other desktops run at. The graph runs at the smallest any of its nodes
 * asks for, within its own bounds, so this is the most this stream adds.
 *
 * NOT YET TUNED: waiting on someone with a real Linux machine or a Pi to
 * measure it. 256/48000 (5.3 ms) ran clean against a private PipeWire with a
 * null sink, which says nothing about real hardware under load. Lower it
 * only once underruns have been counted on real hardware. */
#define PIPEWIRE_LATENCY "480/48000"
#define PIPEWIRE_STRIDE (2 * sizeof(float))

#define PIPEWIRE_FUNCTIONS(X) \
    X(init)                   \
    X(thread_loop_new)        \
    X(thread_loop_get_loop)   \
    X(thread_loop_start)      \
    X(thread_loop_stop)       \
    X(thread_loop_destroy)    \
    X(properties_new)         \
    X(stream_new_simple)      \
    X(stream_connect)         \
    X(stream_destroy)         \
    X(stream_dequeue_buffer)  \
    X(stream_queue_buffer)

/* pw.init is pw_init, and so on. */
static struct
{
#define X(name) __typeof__(pw_##name) *name;
    PIPEWIRE_FUNCTIONS(X)
#undef X
} pw;

static struct
{
    void (*render)(float *dst, int frames, uint32_t rate);
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct spa_source *retry;
    bool dropping;
    /* Where the graph says how much each cycle wants (pipewire_process). */
    struct spa_io_position *position;
    struct spa_io_rate_match *rate_match;
} pipewire;

/* Once for the run. A library that will not load is not tried again, and
 * one that did stays loaded, since pw_init leaves it in use. */
static bool pipewire_load(void)
{
    static int loaded;
    if (!loaded)
    {
        void *lib = dlopen("libpipewire-0.3.so.0", RTLD_NOW);
        loaded = -1;
        if (!lib)
            return false;
#define X(name)                                                    \
    if (!(pw.name = (__typeof__(pw.name))dlsym(lib, "pw_" #name))) \
        return false;
        PIPEWIRE_FUNCTIONS(X)
#undef X
        pw.init(NULL, NULL);
        loaded = 1;
    }
    return loaded > 0;
}

/* The frames this cycle wants, which is what pw_buffer.requested says from
 * 0.3.49 on, worked out the way the stream works it out so that the
 * libpipewire of Ubuntu 22.04, which the release is built against, does
 * too: the resampler's ask when the graph runs at another rate, else the
 * quantum. */
static void pipewire_process(void *user)
{
    (void)user;
    struct pw_buffer *b = pw.stream_dequeue_buffer(pipewire.stream);
    if (!b)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    uint32_t frames = pipewire.rate_match ? pipewire.rate_match->size
                                          : (uint32_t)pipewire.position->clock.duration;
    if (frames > d->maxsize / PIPEWIRE_STRIDE)
        frames = d->maxsize / PIPEWIRE_STRIDE;
    pipewire.render(d->data, (int)frames, PIPEWIRE_RATE);
    d->chunk->offset = 0;
    d->chunk->stride = PIPEWIRE_STRIDE;
    d->chunk->size = frames * PIPEWIRE_STRIDE;
    pw.stream_queue_buffer(pipewire.stream, b);
}

static void pipewire_io_changed(void *user, uint32_t id, void *area, uint32_t size)
{
    (void)user, (void)size;
    if (id == SPA_IO_Position)
        pipewire.position = area;
    else if (id == SPA_IO_RateMatch)
        pipewire.rate_match = area;
}

static void pipewire_retry_soon(void)
{
    struct timespec second = {.tv_sec = 1};
    pw_loop_update_timer(pw.thread_loop_get_loop(pipewire.loop), pipewire.retry, &second, NULL,
                         false);
}

/* Unconnected is the daemon gone or the node taken out of the graph, and an
 * error is the stream refused; either way the stream is made again. A stream
 * being let go goes unconnected too, which is not that. */
static void pipewire_state_changed(void *user, enum pw_stream_state old,
                                   enum pw_stream_state state, const char *error)
{
    (void)user, (void)old, (void)error;
    if (!pipewire.dropping &&
        (state == PW_STREAM_STATE_UNCONNECTED || state == PW_STREAM_STATE_ERROR))
        pipewire_retry_soon();
}

static void pipewire_drop(void)
{
    pipewire.dropping = true;
    pw.stream_destroy(pipewire.stream);
    pipewire.dropping = false;
    pipewire.stream = NULL;
    pipewire.position = NULL;
    pipewire.rate_match = NULL;
}

static const struct pw_stream_events pipewire_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = pipewire_state_changed,
    .io_changed = pipewire_io_changed,
    .process = pipewire_process,
};

/* The stream on the default sink, moving with it. False when the daemon
 * does not answer. */
static bool pipewire_connect(void)
{
    struct pw_properties *props = pw.properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game",
        PW_KEY_APP_NAME, "Picocomputer 6502", PW_KEY_NODE_LATENCY, PIPEWIRE_LATENCY, NULL);
    pipewire.stream = pw.stream_new_simple(pw.thread_loop_get_loop(pipewire.loop),
                                           "Picocomputer 6502", props, &pipewire_events, NULL);
    if (!pipewire.stream)
        return false;
    uint8_t pod[256];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod, sizeof pod);
    const struct spa_pod *params[] = {spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = PIPEWIRE_RATE,
                                 .channels = 2,
                                 .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}))};
    if (pw.stream_connect(pipewire.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                              PW_STREAM_FLAG_RT_PROCESS,
                          params, 1) < 0)
    {
        pipewire_drop();
        return false;
    }
    return true;
}

/* On the loop's thread, a second after the stream was lost: the old one let
 * go and a new one made, and made again in a second if the daemon is not
 * back. */
static void pipewire_retry(void *user, uint64_t expirations)
{
    (void)user, (void)expirations;
    if (pipewire.stream)
        pipewire_drop();
    if (!pipewire_connect())
        pipewire_retry_soon();
}

bool pipewire_open(void (*render)(float *dst, int frames, uint32_t rate))
{
    if (!pipewire_load())
        return false;
    pipewire.render = render;
    pipewire.loop = pw.thread_loop_new("rp6502-audio", NULL);
    if (!pipewire.loop)
        return false;
    if (!pipewire_connect())
    {
        pw.thread_loop_destroy(pipewire.loop);
        pipewire.loop = NULL;
        return false;
    }
    pipewire.retry =
        pw_loop_add_timer(pw.thread_loop_get_loop(pipewire.loop), pipewire_retry, NULL);
    if (pw.thread_loop_start(pipewire.loop) < 0)
    {
        pipewire_drop();
        pw_loop_destroy_source(pw.thread_loop_get_loop(pipewire.loop), pipewire.retry);
        pw.thread_loop_destroy(pipewire.loop);
        pipewire.loop = NULL;
        return false;
    }
    return true;
}

void pipewire_close(void)
{
    pw.thread_loop_stop(pipewire.loop);
    if (pipewire.stream)
        pipewire_drop();
    pw_loop_destroy_source(pw.thread_loop_get_loop(pipewire.loop), pipewire.retry);
    pw.thread_loop_destroy(pipewire.loop);
}
