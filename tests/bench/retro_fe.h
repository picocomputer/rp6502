/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_BENCH_RETRO_FE_H_
#define _TESTS_BENCH_RETRO_FE_H_

#include "libretro.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

static void *fe_dl_open(const char *path)
{
    return (void *)LoadLibraryA(path);
}

static void *fe_dl_sym(void *lib, const char *name)
{
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
    bool variables_declared;
    bool input_descriptors_set;
    bool controller_info_set;
    bool asked_for_bitmasks;
    int get_variable_calls;
    char message[256];
    int message_count;

    unsigned options_version;
    bool offer_bitmasks;
    unsigned message_version;
    int max_users;
    char option_text[FE_MAX_OPTS][256];
    const char *option_value[FE_MAX_OPTS];
    bool variables_dirty;
    char save_dir[1024];
    bool have_save_dir;

    int16_t input[FE_MAX_PORTS][8][FE_STATE_IDS];
    int16_t analog[FE_MAX_PORTS][8][FE_STATE_IDS];
    int16_t pointer[8][FE_STATE_IDS];
    int16_t mouse[FE_STATE_IDS];
    int16_t lightgun[FE_MAX_PORTS][FE_STATE_IDS];
    unsigned port_device[FE_MAX_PORTS];

    int av_enable;
    bool av_enable_asked;

    int savestate_context;
    bool savestate_context_asked;
    bool savestate_context_refused;

    uint64_t serialization_quirks;
    bool serialization_quirks_set;

    const char *game_info_dir;
    bool memory_maps_set;
    unsigned memory_map_count;

    const void *frame;
    unsigned frame_w, frame_h;
    size_t frame_pitch;
    uint32_t frame_copy[640 * 480];
    int video_calls, poll_calls, state_calls, audio_calls, mask_reads;
    int audio_peak;
    size_t audio_frames;
    bool state_read_before_poll;
} fe_t;

static fe_t fe;

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
    case RETRO_DEVICE_LIGHTGUN: return fe.lightgun[port][id];
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

    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        fe.av_enable_asked = true;
        *(int *)data = fe.av_enable;
        return true;

    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
        fe.savestate_context_asked = true;
        if (fe.savestate_context_refused)
            return false;
        *(int *)data = fe.savestate_context;
        return true;

    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        fe.serialization_quirks = *(uint64_t *)data;
        fe.serialization_quirks_set = true;
        return true;

    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
    {
        static struct retro_game_info_ext ext;
        if (!fe.game_info_dir)
            return false;
        memset(&ext, 0, sizeof ext);
        ext.dir = fe.game_info_dir;
        *(struct retro_game_info_ext **)data = &ext;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    {
        const struct retro_memory_map *m = (const struct retro_memory_map *)data;
        fe.memory_maps_set = true;
        fe.memory_map_count = m ? m->num_descriptors : 0;
        return true;
    }

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

static void fe_open_as(unsigned options_version, bool offer_bitmasks)
{
    memset(&fe, 0, sizeof fe);
    fe.options_version = options_version;
    fe.offer_bitmasks = offer_bitmasks;
    fe.message_version = 1;
    fe.max_users = -1;
    fe.av_enable = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    fe.savestate_context = RETRO_SAVESTATE_CONTEXT_NORMAL;
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

    /* retro_set_environment is called before retro_init because libretro.h
     * guarantees that order, and the core's retro_init makes environment
     * calls. */
    fe.set_environment(fe_environment);
    fe.set_video_refresh(fe_video);
    fe.set_audio_sample(fe_audio_sample);
    fe.set_audio_sample_batch(fe_audio_batch);
    fe.set_input_poll(fe_input_poll);
    fe.set_input_state(fe_input_state);
    fe.init();
}

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

static void fe_run(int frames)
{
    for (int i = 0; i < frames; i++)
        fe.run();
}

static void fe_key(unsigned keycode, uint32_t character, uint16_t mods)
{
    if (!fe.keyboard_set || !fe.keyboard.callback)
        return;
    fe.keyboard.callback(true, keycode, character, mods);
    fe.keyboard.callback(false, keycode, character, mods);
}

#endif /* _TESTS_BENCH_RETRO_FE_H_ */
