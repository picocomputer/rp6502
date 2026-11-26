/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The sound on macOS, through the default output AudioUnit (entry.h).
 *
 * The pattern is the one each desktop's driver follows (windows/audio.c,
 * linux/audio.c): the device pulls, on a thread of its own, and render()
 * makes the sound it asks for on the spot, from what the machine has
 * stamped; the mixer keeps 2 ms in hand of that and trims to the device's
 * clock (core/aud/mix.c). The driver's whole part is to ask for as little
 * buffering as runs clean and to keep the device's thread fed.
 *
 * Here the device's thread is CoreAudio's real-time IO thread, which calls
 * audio_cb for each IO buffer. The IO buffer of the device that is the
 * default when the sound opens is set to AUDIO_FRAMES, 512 of its frames,
 * 10.7 ms at 48 kHz, the period the other desktops run at, where
 * sokol_audio's AudioQueue held two buffers of 512, 21 ms, before the
 * device's own; a device that becomes the default later keeps its own size,
 * 512 on most. The default output unit follows the default device as it
 * changes, and converts from the format given here, 32-bit float stereo,
 * interleaved, at 48 kHz, to whatever rate the device runs at, so the
 * mixer always makes 48000 Hz.
 *
 * This was written without a Mac to run it on. Signs of trouble and where
 * to look: sound that crackles or drops out often is the IO thread not fed
 * in time, and AUDIO_FRAMES is the first thing to raise (1024); no sound
 * at all is host_audio_open failing, which returns false at the AudioUnit
 * call that failed and logs nothing, so a breakpoint or a print there says
 * which; sound at the wrong speed or pitch cannot be this driver's rate,
 * which is fixed, and is the mixer's to look at (core/aud/mix.c). */

#include "host/sokol/app/entry.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AUDIO_RATE 48000
#define AUDIO_FRAMES 512
/* The most frames CoreAudio asks for at once, which an IO buffer of
 * AUDIO_FRAMES never comes near. */
#define AUDIO_MOST 4096

static struct
{
    void (*render)(float *dst, int frames, uint32_t rate);
    AudioComponentInstance unit;
    uint32_t rate;
} audio;

/* CoreAudio's IO thread, real-time: no locks but the mixer's own, no
 * allocation, nothing that waits. A request larger than AUDIO_MOST, which
 * should never come, is made in pieces. */
static OSStatus audio_cb(void *user, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                         UInt32 bus, UInt32 frames, AudioBufferList *data)
{
    (void)user, (void)flags, (void)ts, (void)bus;
    float *out = (float *)data->mBuffers[0].mData;
    while (frames)
    {
        const UInt32 n = frames < AUDIO_MOST ? frames : AUDIO_MOST;
        audio.render(out, (int)n, audio.rate);
        out += n * 2;
        frames -= n;
    }
    return noErr;
}

bool host_audio_open(void (*render)(float *dst, int frames, uint32_t rate))
{
    audio.render = render;
    audio.rate = AUDIO_RATE;
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &audio.unit) != noErr)
        return false;
    /* What the callback gives: 32-bit float stereo, interleaved. */
    AudioStreamBasicDescription fmt = {
        .mSampleRate = AUDIO_RATE,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 2 * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 2 * sizeof(float),
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32,
    };
    AURenderCallbackStruct cb = {.inputProc = audio_cb};
    /* The IO buffer, on the device the unit plays to; a device that will not
     * take it keeps its own, and the sound still plays. */
    UInt32 frames = AUDIO_FRAMES;
    AudioUnitSetProperty(audio.unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0,
                         &frames, sizeof frames);
    if (AudioUnitSetProperty(audio.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                             &fmt, sizeof fmt) != noErr ||
        AudioUnitSetProperty(audio.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(audio.unit) != noErr)
    {
        AudioComponentInstanceDispose(audio.unit);
        audio.unit = NULL;
        return false;
    }
    if (AudioOutputUnitStart(audio.unit) != noErr)
    {
        AudioUnitUninitialize(audio.unit);
        AudioComponentInstanceDispose(audio.unit);
        audio.unit = NULL;
        return false;
    }
    return true;
}

void host_audio_close(void)
{
    if (!audio.unit)
        return;
    AudioOutputUnitStop(audio.unit);
    AudioUnitUninitialize(audio.unit);
    AudioComponentInstanceDispose(audio.unit);
    audio.unit = NULL;
}
