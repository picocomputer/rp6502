/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/cli/state.h"

#include "core/aud/mix.h"
#include "core/hid/keyboard.h"
#include "core/hid/vtkeys.h"
#include "core/sys/sst.h"
#include "osal/fs.h"
#include "osal/dir.h"

#include <stdio.h>
#include <stdlib.h>

static bool state_threaded_audio;

static char state_slot_path[1024];

void state_slot_init(const char *rom)
{
    if (!rom || !*rom)
        return;
    char *abs = os_dir_realpath(rom);
    snprintf(state_slot_path, sizeof state_slot_path, "%s.sst", abs ? abs : rom);
    free(abs);
}

const char *state_slot(void)
{
    return state_slot_path[0] ? state_slot_path : NULL;
}

void state_audio_is_threaded(bool on) { state_threaded_audio = on; }

/* Hold the audio device's thread out of the engines while the machine is
 * copied or replaced. The wait is bounded because a sink that has stopped
 * calling back must not hold a save forever, and giving up costs one engine
 * read taken mid-sample, which is the same machine a sample either side. It
 * spins rather than sleeps because the device buffer is 512 frames at 48 kHz,
 * about eleven milliseconds, and the callback is already running. */
static void state_park(void)
{
    if (!state_threaded_audio || !aud_enabled())
        return;
    aud_park_request();
    for (long spin = 0; spin < 100000000L && !aud_parked(); spin++)
        ;
}

static void state_unpark(void)
{
    if (state_threaded_audio && aud_enabled())
        aud_park_release();
}

bool state_save_file(const char *path, const char **why)
{
    size_t len = sst_size();
    void *buf = malloc(len);
    if (!buf)
    {
        *why = "out of memory";
        return false;
    }
    state_park();
    fs_std_settle();
    const char *bad = sst_save(buf, len, 0);
    state_unpark();
    if (bad)
    {
        free(buf);
        *why = bad;
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        free(buf);
        *why = "cannot open the file to write";
        return false;
    }
    bool ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    free(buf);
    if (!ok)
    {
        *why = "cannot write the whole blob";
        return false;
    }
    return true;
}

bool state_load_file(const char *path, const char **why)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        *why = "cannot open the file to read";
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        *why = "cannot measure the file";
        return false;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        *why = "cannot measure the file";
        return false;
    }
    void *buf = malloc((size_t)len ? (size_t)len : 1);
    if (!buf)
    {
        fclose(f);
        *why = "out of memory";
        return false;
    }
    bool read = fread(buf, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (!read)
    {
        free(buf);
        *why = "cannot read the whole file";
        return false;
    }

    /* A paste is a queue of keys the outgoing machine was going to receive
     * and the incoming one never asked for. Cancelling it on every load is
     * what lets vtkeys carry no savestate chunk. */
    vtkeys_paste_cancel();
    state_park();
    fs_std_settle();
    const char *bad = sst_load(buf, (size_t)len, 0, NULL);
    state_unpark();
    free(buf);
    if (bad)
    {
        *why = bad;
        return false;
    }
    return true;
}
