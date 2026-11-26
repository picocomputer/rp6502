/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The sound on Linux and macOS, through sokol_audio (entry.h): PulseAudio
 * and CoreAudio, each with a thread of its own.
 *
 * 48000 Hz is asked for, which keeps the resampler's ratio near one, and
 * both keep it and resample in the OS. buffer_frames is what the callback is
 * asked for at a time; what the device holds beyond that is each backend's
 * own. The sizes are the least that measured clean:
 *
 * PulseAudio is asked to hold buffer_frames in all, and holds three
 * quarters of it in the stream, 8 ms at 512. That ran minutes without
 * running dry on a host with little else to do, where 256 did not, and ran
 * dry a few times a minute beside a compiler.
 *
 * CoreAudio's queue is two buffers of buffer_frames, a callback for each.
 * 512 is 94 callbacks a second with 10.7 ms in hand; fewer frames would be
 * more calls into the OS for nothing.
 *
 * Sokol wants buffer_frames a multiple of its packets, which only its push
 * model uses, so they are a frame each. */

#include "host/sokol/app/entry.h"
#include "host/sokol/app/app.h"
#include "sokol/sokol_audio.h"

static void (*audio_render)(float *dst, int frames, uint32_t rate);
static uint32_t audio_rate;

static void audio_cb(float *buffer, int num_frames, int num_channels)
{
    (void)num_channels;
    audio_render(buffer, num_frames, audio_rate);
}

bool host_audio_open(void (*render)(float *dst, int frames, uint32_t rate))
{
    audio_render = render;
    saudio_setup(&(saudio_desc){
        .sample_rate = 48000,
        .num_channels = 2,
        .buffer_frames = 512,
        .packet_frames = 1,
        .stream_cb = audio_cb,
        .logger.func = app_log,
    });
    if (!saudio_isvalid())
        return false;
    audio_rate = (uint32_t)saudio_sample_rate();
    return true;
}

void host_audio_close(void)
{
    if (saudio_isvalid())
        saudio_shutdown();
}
