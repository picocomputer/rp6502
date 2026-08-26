/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bench playing the frontend.
 *
 * A libretro core answers to a frontend, so a suite that asks it anything
 * has to be one. This is the smallest frontend that can hold a core to its
 * contract: it loads the shipped .so, hands over the five callbacks, records
 * what the core asked of it, and lets a case say what a device is doing.
 *
 * It opens the artifact rather than linking its objects. The export list,
 * the version script and the load itself are things a suite should be able
 * to be wrong about, and none of them exist in a pile of objects.
 *
 * RETRO_SO is the path to the library, from the build.
 */

#ifndef _TESTS_BENCH_RETRO_FE_H_
#define _TESTS_BENCH_RETRO_FE_H_

#include "libretro.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frontend's loader. Windows spells dlopen differently, and this is the
 * one place in the test tree that opens a library, so the difference lives
 * here and nowhere else. LoadLibrary resolves every import as it loads,
 * which is the RTLD_NOW the other branch asks for, and local is the only
 * scope Windows has. */
#ifdef _WIN32
#include <windows.h>

static void *fe_dl_open(const char *path)
{
    /* RETRO_SO is $<TARGET_FILE:...>: absolute, forward slashes, which
     * LoadLibrary takes as readily as backslashes. */
    return (void *)LoadLibraryA(path);
}

static void *fe_dl_sym(void *lib, const char *name)
{
    /* Through void*, which Windows promises works and which keeps
     * -Wcast-function-type off the typed fields FE_SYM writes. */
    return (void *)GetProcAddress((HMODULE)lib, name);
}

static void fe_dl_close(void *lib)
{
    FreeLibrary((HMODULE)lib);
}

static void fe_dl_error(void)
{
    fprintf(stderr, "retro_fe: %s: Windows error %lu\n", RETRO_SO,
            (unsigned long)GetLastError());
}
#else
#include <dlfcn.h>

static void *fe_dl_open(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void *fe_dl_sym(void *lib, const char *name)
{
    return dlsym(lib, name);
}

static void fe_dl_close(void *lib)
{
    dlclose(lib);
}

static void fe_dl_error(void)
{
    fprintf(stderr, "retro_fe: %s\n", dlerror());
}
#endif

#define FE_MAX_GEOM 32
#define FE_MAX_OPTS 32
#define FE_MAX_PORTS 4
#define FE_STATE_IDS 32

typedef struct
{
    unsigned width, height;
} fe_geom_t;

typedef struct
{
    /* The library and what it exports. */
    void *lib;
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_controller_port_device)(unsigned, unsigned);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    unsigned (*get_region)(void);

    /* What the core asked of us. */
    enum retro_pixel_format pixel_format;
    bool pixel_format_set;
    bool supports_no_game;
    fe_geom_t geom[FE_MAX_GEOM];
    int geom_count;
    struct retro_keyboard_callback keyboard;
    bool keyboard_set;
    bool shutdown;
    char option_key[FE_MAX_OPTS][64];
    int option_count;
    bool options_declared;
    int options_version_asked;
    bool variables_declared; /* the pre-versions form, for an old frontend */
    bool input_descriptors_set;
    bool controller_info_set;
    bool asked_for_bitmasks;
    int get_variable_calls;
    char message[256];   /* the last thing the core asked us to show */
    int message_count;

    /* What we answer when it asks. */
    unsigned options_version; /* what this frontend claims to speak */
    bool offer_bitmasks;
    unsigned message_version; /* 0 = only the old SET_MESSAGE */
    int max_users;            /* -1 = will not say */
    char option_text[FE_MAX_OPTS][256];
    const char *option_value[FE_MAX_OPTS];
    bool variables_dirty;
    char save_dir[1024];
    bool have_save_dir;

    /* What the devices are doing. A table per device, because their id
     * spaces overlap: RETRO_DEVICE_ID_ANALOG_X is 0 and so is
     * RETRO_DEVICE_ID_JOYPAD_B, and a stick deflected in one is not a
     * button held in the other. */
    int16_t input[FE_MAX_PORTS][8][FE_STATE_IDS];   /* joypad, by id */
    int16_t analog[FE_MAX_PORTS][8][FE_STATE_IDS];  /* sticks and triggers */
    int16_t pointer[8][FE_STATE_IDS];
    int16_t mouse[FE_STATE_IDS];
    unsigned port_device[FE_MAX_PORTS];

    /* What we were handed back. */
    const void *frame;
    unsigned frame_w, frame_h;
    size_t frame_pitch;
    uint32_t frame_copy[640 * 480];
    int video_calls, poll_calls, state_calls, audio_calls, mask_reads;
    int audio_peak; /* loudest sample handed over, to tell sound from silence */
    size_t audio_frames;
    bool state_read_before_poll;
} fe_t;

static fe_t fe;

/* ---- the callbacks a core is given ---- */

static void fe_video(const void *data, unsigned width, unsigned height, size_t pitch)
{
    fe.video_calls++;
    fe.frame = data;
    fe.frame_w = width;
    fe.frame_h = height;
    fe.frame_pitch = pitch;
    if (data && (size_t)width * height <= sizeof fe.frame_copy / sizeof *fe.frame_copy)
        for (unsigned y = 0; y < height; y++)
            memcpy(&fe.frame_copy[(size_t)y * width],
                   (const uint8_t *)data + (size_t)y * pitch,
                   (size_t)width * sizeof(uint32_t));
}

static size_t fe_audio_batch(const int16_t *data, size_t frames)
{
    fe.audio_calls++;
    fe.audio_frames += frames;
    for (size_t i = 0; data && i < frames * 2; i++)
    {
        int v = data[i] < 0 ? -data[i] : data[i];
        if (v > fe.audio_peak)
            fe.audio_peak = v;
    }
    return frames;
}

static void fe_audio_sample(int16_t l, int16_t r)
{
    (void)l;
    (void)r;
    fe.audio_calls++;
    fe.audio_frames++;
}

static void fe_input_poll(void)
{
    fe.poll_calls++;
}

static int16_t fe_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    fe.state_calls++;
    if (fe.poll_calls == 0)
        fe.state_read_before_poll = true;
    if (port >= FE_MAX_PORTS)
        return 0;
    /* The whole pad at once, which is what a core asks for when the frontend
     * offered bitmasks. Assembled from the same buttons a case set, so a
     * suite says what is pressed once and both ways of reading agree. */
    if (device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_MASK)
    {
        fe.mask_reads++;
        int16_t mask = 0;
        for (unsigned b = 0; b <= RETRO_DEVICE_ID_JOYPAD_R3; b++)
            if (fe.input[port][0][b])
                mask |= (int16_t)(1 << b);
        return mask;
    }
    if (index >= 8 || id >= FE_STATE_IDS)
        return 0;
    switch (device)
    {
    case RETRO_DEVICE_ANALOG: return fe.analog[port][index][id];
    case RETRO_DEVICE_POINTER: return fe.pointer[index][id];
    case RETRO_DEVICE_MOUSE: return fe.mouse[id];
    default: return fe.input[port][index][id];
    }
}

static bool fe_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        fe.pixel_format = *(const enum retro_pixel_format *)data;
        fe.pixel_format_set = true;
        return fe.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888;

    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        fe.supports_no_game = *(const bool *)data;
        return true;

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        const struct retro_game_geometry *g = (const struct retro_game_geometry *)data;
        if (fe.geom_count < FE_MAX_GEOM)
        {
            fe.geom[fe.geom_count].width = g->base_width;
            fe.geom[fe.geom_count].height = g->base_height;
            fe.geom_count++;
        }
        return true;
    }

    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        fe.keyboard = *(const struct retro_keyboard_callback *)data;
        fe.keyboard_set = true;
        return true;

    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        fe.options_version_asked++;
        *(unsigned *)data = fe.options_version;
        return true;

    case RETRO_ENVIRONMENT_SET_VARIABLES:
    {
        const struct retro_variable *v = (const struct retro_variable *)data;
        fe.variables_declared = true;
        for (; v && v->key && fe.option_count < FE_MAX_OPTS; v++)
        {
            snprintf(fe.option_key[fe.option_count], sizeof fe.option_key[0], "%s", v->key);
            snprintf(fe.option_text[fe.option_count], sizeof fe.option_text[0], "%s",
                     v->value ? v->value : "");
            fe.option_count++;
        }
        return true;
    }

    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        fe.input_descriptors_set = true;
        return true;

    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        fe.controller_info_set = true;
        return true;

    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        fe.asked_for_bitmasks = true;
        return fe.offer_bitmasks;

    case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
        if (fe.max_users < 0)
            return false;
        *(unsigned *)data = (unsigned)fe.max_users;
        return true;

    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *(unsigned *)data = fe.message_version;
        return fe.message_version > 0;

    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
    {
        const struct retro_message_ext *m = (const struct retro_message_ext *)data;
        snprintf(fe.message, sizeof fe.message, "%s", m->msg ? m->msg : "");
        fe.message_count++;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
        const struct retro_message *m = (const struct retro_message *)data;
        snprintf(fe.message, sizeof fe.message, "%s", m->msg ? m->msg : "");
        fe.message_count++;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    {
        const struct retro_core_options_v2 *o = (const struct retro_core_options_v2 *)data;
        fe.options_declared = true;
        for (const struct retro_core_option_v2_definition *d = o->definitions;
             d && d->key && fe.option_count < FE_MAX_OPTS; d++)
        {
            snprintf(fe.option_key[fe.option_count], sizeof fe.option_key[0], "%s", d->key);
            fe.option_count++;
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *v = (struct retro_variable *)data;
        fe.get_variable_calls++;
        v->value = NULL;
        for (int i = 0; i < fe.option_count; i++)
            if (!strcmp(fe.option_key[i], v->key))
            {
                v->value = fe.option_value[i];
                break;
            }
        return v->value != NULL;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = fe.variables_dirty;
        fe.variables_dirty = false;
        return true;

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (!fe.have_save_dir)
            return false;
        *(const char **)data = fe.save_dir;
        return true;

    case RETRO_ENVIRONMENT_SHUTDOWN:
        fe.shutdown = true;
        return true;

    default:
        return false;
    }
}

/* ---- standing the core up ---- */

#define FE_SYM(field, name)                                    \
    do                                                         \
    {                                                          \
        *(void **)(&fe.field) = fe_dl_sym(fe.lib, name);           \
        if (!fe.field)                                         \
        {                                                      \
            fprintf(stderr, "retro_fe: %s is not exported\n", name); \
            exit(1);                                           \
        }                                                      \
    } while (0)

/* Every entry point, resolved up front: a core missing one is not a core,
 * and finding that out here beats finding it out in whichever case ran
 * first. */
static void fe_open_as(unsigned options_version, bool offer_bitmasks)
{
    memset(&fe, 0, sizeof fe);
    fe.options_version = options_version;
    fe.offer_bitmasks = offer_bitmasks;
    fe.message_version = 1;
    fe.max_users = -1; /* a frontend that will not say, unless a case does */
    fe.lib = fe_dl_open(RETRO_SO);
    if (!fe.lib)
    {
        fe_dl_error();
        exit(1);
    }
    FE_SYM(api_version, "retro_api_version");
    FE_SYM(set_environment, "retro_set_environment");
    FE_SYM(set_video_refresh, "retro_set_video_refresh");
    FE_SYM(set_audio_sample, "retro_set_audio_sample");
    FE_SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    FE_SYM(set_input_poll, "retro_set_input_poll");
    FE_SYM(set_input_state, "retro_set_input_state");
    FE_SYM(init, "retro_init");
    FE_SYM(deinit, "retro_deinit");
    FE_SYM(get_system_info, "retro_get_system_info");
    FE_SYM(get_system_av_info, "retro_get_system_av_info");
    FE_SYM(set_controller_port_device, "retro_set_controller_port_device");
    FE_SYM(load_game, "retro_load_game");
    FE_SYM(unload_game, "retro_unload_game");
    FE_SYM(reset, "retro_reset");
    FE_SYM(run, "retro_run");
    FE_SYM(serialize_size, "retro_serialize_size");
    FE_SYM(serialize, "retro_serialize");
    FE_SYM(unserialize, "retro_unserialize");
    FE_SYM(get_memory_data, "retro_get_memory_data");
    FE_SYM(get_memory_size, "retro_get_memory_size");
    FE_SYM(get_region, "retro_get_region");

    /* The frontend's own order: environment first, because that is where a
     * core declares what it needs, then the rest, then init. */
    fe.set_environment(fe_environment);
    fe.set_video_refresh(fe_video);
    fe.set_audio_sample(fe_audio_sample);
    fe.set_audio_sample_batch(fe_audio_batch);
    fe.set_input_poll(fe_input_poll);
    fe.set_input_state(fe_input_state);
    fe.init();
}

/* The frontend a case gets unless it wants an older one: current core
 * options, and the input bitmask a modern frontend offers. */
static void fe_open(void)
{
    fe_open_as(2, true);
}

static void fe_close(void)
{
    if (!fe.lib)
        return;
    fe.deinit();
    fe_dl_close(fe.lib);
    fe.lib = NULL;
}

static bool fe_load(const char *path)
{
    struct retro_game_info info;
    memset(&info, 0, sizeof info);
    info.path = path;
    fe.geom_count = 0;
    fe.shutdown = false;
    return fe.load_game(&info);
}

/* Advance the core, and count what it does with the frame it was asked for. */
static void fe_run(int frames)
{
    for (int i = 0; i < frames; i++)
        fe.run();
}

/* A key down and up through the callback the core registered. */
static void fe_key(unsigned keycode, uint32_t character, uint16_t mods)
{
    if (!fe.keyboard_set || !fe.keyboard.callback)
        return;
    fe.keyboard.callback(true, keycode, character, mods);
    fe.keyboard.callback(false, keycode, character, mods);
}

#endif /* _TESTS_BENCH_RETRO_FE_H_ */
