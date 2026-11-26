/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The sound on Windows, through WASAPI in shared mode, which mixes it with
 * every other program's.
 *
 * The engine takes a period, 10 ms, at a time, from a buffer of about two
 * and a fifth of them, and wakes the device's thread after each. The thread
 * keeps a period and 2 ms in that buffer: the engine wants no more, and
 * filling all of it, as sokol_audio once did, kept 17 ms there on average where
 * this keeps 7. A shorter period, which IAudioClient3 asks for, is offered
 * by few drivers, none of them the NVIDIA HDMI or the USB Audio 2.0 class
 * driver this was measured on, so nothing here counts on one.
 *
 * The thread is registered with MMCSS as Pro Audio, so Windows runs it ahead
 * of ordinary threads, and writes in the engine's own format, 32-bit float
 * at its rate, stereo into its first two channels. When the default device
 * goes away or changes, as headphones come or go, it opens the one that is
 * the default now. */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define COBJMACROS
#include <windows.h>
#include <avrt.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include "host/sokol/app/entry.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const CLSID audio_CLSID_MMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID audio_IID_IMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID audio_IID_IAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID audio_IID_IAudioRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const IID audio_IID_IMMNotificationClient = {0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
static const IID audio_IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID audio_SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* The most frames the engine is asked to take at once, which its buffer
 * never comes near. */
#define AUDIO_MOST 4096

/* The margin kept past a period, for the thread waking late: 2 ms. */
#define AUDIO_MARGIN_HZ 500

static struct
{
    void (*render)(float *dst, int frames, uint32_t rate);
    HANDLE thread;
    HANDLE stop;    /* the thread is to end */
    HANDLE changed; /* the default device changed */
    HANDLE ready;   /* the first device has been opened, or could not be */
    uint32_t rate; /* the device's, the thread's alone */
    float stereo[AUDIO_MOST * 2];
} audio;

/* ---------------- the default device changing ----------------
 *
 * IMMNotificationClient, as C: a vtable and a struct that starts with it. It
 * lives as long as the thread, which registers it and unregisters it. */

static HRESULT STDMETHODCALLTYPE note_query(IMMNotificationClient *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &audio_IID_IUnknown) || IsEqualIID(riid, &audio_IID_IMMNotificationClient))
    {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE note_ref(IMMNotificationClient *self)
{
    (void)self;
    return 1;
}

static HRESULT STDMETHODCALLTYPE note_state(IMMNotificationClient *self, LPCWSTR id, DWORD state)
{
    (void)self, (void)id, (void)state;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE note_device(IMMNotificationClient *self, LPCWSTR id)
{
    (void)self, (void)id;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE note_default(IMMNotificationClient *self, EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)self, (void)id;
    if (flow == eRender && role == eConsole)
        SetEvent(audio.changed);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE note_property(IMMNotificationClient *self, LPCWSTR id, const PROPERTYKEY key)
{
    (void)self, (void)id, (void)key;
    return S_OK;
}

static IMMNotificationClientVtbl note_vtbl = {
    note_query, note_ref, note_ref, note_state, note_device, note_device, note_default, note_property,
};
static IMMNotificationClient note = {&note_vtbl};

/* ---------------- a device ---------------- */

typedef struct
{
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    HANDLE event;
    UINT32 frames;  /* the engine's buffer */
    UINT32 keep;    /* what is kept in it: a period, and a margin */
    WORD channels;
} audio_device_t;

static void audio_close(audio_device_t *d)
{
    if (d->client)
        IAudioClient_Stop(d->client);
    if (d->render)
        IAudioRenderClient_Release(d->render);
    if (d->client)
        IAudioClient_Release(d->client);
    if (d->device)
        IMMDevice_Release(d->device);
    if (d->event)
        CloseHandle(d->event);
    memset(d, 0, sizeof *d);
}

/* Whether a format is 32-bit float, which the engine mixes in. */
static bool audio_float(const WAVEFORMATEX *f)
{
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return f->wBitsPerSample == 32;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->wBitsPerSample == 32 &&
           IsEqualGUID(&((const WAVEFORMATEXTENSIBLE *)f)->SubFormat, &audio_SUBTYPE_IEEE_FLOAT);
}

/* The default device, opened in the engine's own format. */
static bool audio_open(IMMDeviceEnumerator *e, audio_device_t *d)
{
    memset(d, 0, sizeof *d);
    WAVEFORMATEX *mix = NULL;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eConsole, &d->device)))
        goto fail;
    d->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!d->event)
        goto fail;
    if (FAILED(IMMDevice_Activate(d->device, &audio_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&d->client)) ||
        FAILED(IAudioClient_GetMixFormat(d->client, &mix)) || !audio_float(mix) || mix->nChannels < 1)
        goto fail;
    REFERENCE_TIME def, least;
    if (FAILED(IAudioClient_Initialize(d->client, AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, NULL)) ||
        FAILED(IAudioClient_GetDevicePeriod(d->client, &def, &least)))
        goto fail;
    const UINT32 period = (UINT32)((uint64_t)def * mix->nSamplesPerSec / 10000000);
    d->keep = period + mix->nSamplesPerSec / AUDIO_MARGIN_HZ;
    d->channels = mix->nChannels;
    audio.rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);
    mix = NULL;
    if (FAILED(IAudioClient_GetBufferSize(d->client, &d->frames)) ||
        FAILED(IAudioClient_GetService(d->client, &audio_IID_IAudioRenderClient, (void **)&d->render)) ||
        FAILED(IAudioClient_SetEventHandle(d->client, d->event)))
        goto fail;
    if (d->keep > d->frames)
        d->keep = d->frames;
    /* The engine starts with what is kept, in silence, rather than empty. */
    BYTE *buf;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(d->render, d->keep, &buf)))
        IAudioRenderClient_ReleaseBuffer(d->render, d->keep, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(IAudioClient_Start(d->client)))
        goto fail;
    return true;
fail:
    if (mix)
        CoTaskMemFree(mix);
    audio_close(d);
    return false;
}

/* The engine's buffer, filled to what is kept: stereo made and spread over
 * its channels, the rest silent. The engine takes a period at a time and
 * wakes the thread after each, so a period and a margin in hand is enough,
 * and the rest of its buffer would be latency for nothing. False when the
 * device has gone. */
static bool audio_fill(audio_device_t *d)
{
    UINT32 padding;
    if (FAILED(IAudioClient_GetCurrentPadding(d->client, &padding)))
        return false;
    UINT32 frames = padding < d->keep ? d->keep - padding : 0;
    if (frames > AUDIO_MOST)
        frames = AUDIO_MOST;
    if (!frames)
        return true;
    BYTE *buf;
    if (FAILED(IAudioRenderClient_GetBuffer(d->render, frames, &buf)))
        return false;
    audio.render(audio.stereo, (int)frames, audio.rate);
    float *out = (float *)buf;
    for (UINT32 i = 0; i < frames; i++)
    {
        if (d->channels == 1)
            *out++ = (audio.stereo[i * 2] + audio.stereo[i * 2 + 1]) * 0.5f;
        else
        {
            *out++ = audio.stereo[i * 2];
            *out++ = audio.stereo[i * 2 + 1];
            for (WORD c = 2; c < d->channels; c++)
                *out++ = 0.0f;
        }
    }
    return SUCCEEDED(IAudioRenderClient_ReleaseBuffer(d->render, frames, 0));
}

static DWORD WINAPI audio_main(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    IMMDeviceEnumerator *e = NULL;
    if (SUCCEEDED(CoCreateInstance(&audio_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &audio_IID_IMMDeviceEnumerator, (void **)&e)))
        IMMDeviceEnumerator_RegisterEndpointNotificationCallback(e, &note);
    bool first = true;
    while (e && WaitForSingleObject(audio.stop, 0) != WAIT_OBJECT_0)
    {
        audio_device_t d;
        ResetEvent(audio.changed);
        const bool opened = audio_open(e, &d);
        if (first)
        {
            first = false;
            SetEvent(audio.ready);
        }
        if (!opened)
        {
            /* No device, or one that would not open: try again when the
             * default changes, or in a second. */
            const HANDLE wait[2] = {audio.stop, audio.changed};
            WaitForMultipleObjects(2, wait, FALSE, 1000);
            continue;
        }
        const HANDLE wait[3] = {audio.stop, audio.changed, d.event};
        for (;;)
        {
            const DWORD got = WaitForMultipleObjects(3, wait, FALSE, 1000);
            if (got == WAIT_OBJECT_0 || got == WAIT_OBJECT_0 + 1 || got == WAIT_FAILED)
                break;
            if (!audio_fill(&d))
                break;
        }
        audio_close(&d);
    }
    if (first)
        SetEvent(audio.ready);
    if (e)
    {
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(e, &note);
        IMMDeviceEnumerator_Release(e);
    }
    if (task)
        AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
    return 0;
}

bool host_audio_open(void (*render)(float *dst, int frames, uint32_t rate))
{
    audio.render = render;
    audio.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    audio.changed = CreateEventW(NULL, TRUE, FALSE, NULL);
    audio.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!audio.stop || !audio.changed || !audio.ready)
        return false;
    audio.thread = CreateThread(NULL, 0, audio_main, NULL, 0, NULL);
    if (!audio.thread)
        return false;
    /* The first device is opened before the machine starts. */
    WaitForSingleObject(audio.ready, INFINITE);
    return true;
}

void host_audio_close(void)
{
    if (!audio.thread)
        return;
    SetEvent(audio.stop);
    WaitForSingleObject(audio.thread, INFINITE);
    CloseHandle(audio.thread);
    audio.thread = NULL;
}
