/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "input.h"

#include "core/str/oem.h"
#include "core/sys/random.h"
#include "host/host.h"
#include "core/sys/version.h"
#include "core/aud/mix.h"
#include "core/sys/debug_log.h"
#include "osal/dir.h"
#include "osal/fs.h"
#include "core/sys/proc.h"
#include "core/rom/rom.h"
#include "core/sys/sst.h"
#include "core/sys/sys.h"
#include "core/wdc/phi2.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/vga/vga_emu.h"
#include "core/api/std.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "osal/os.h"

#include "libretro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 48000 is also core/aud/mix.c's default sink rate, so this core never calls
 * aud_set_sink_rate. Every voice is generated at the YM3812's 49716 Hz and
 * resampled to it. */
#define RETRO_AUD_RATE 48000
#define RETRO_AUD_FRAMES (RETRO_AUD_RATE / VGA_HZ)

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static uint32_t frame_buf[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];
static float audio_out[RETRO_AUD_FRAMES * 2];
static int16_t audio_buf[RETRO_AUD_FRAMES * 2];

static char *loaded_rom;  /* the OEM code page, absolute; owned here */
static char *loaded_path; /* as the frontend spelled it; owned here */
static bool machine_inited;
static int geom_w, geom_h;
static bool shutdown_sent;
static bool hint_shown;

/* Set by a proc_boot that succeeded, which loaded_rom and machine_inited do
 * not say: both are set before proc_boot can fail, and a frontend asks for a
 * savestate the moment the core loads content. */
static bool booted;

/* Taken once per session, so the random stream and the memory fill agree with
 * each other. */
static uint32_t run_seed;
static bool run_seed_taken;

/* libretro publishes no va_list form of its logger, so this function's own
 * va_list cannot be forwarded and the message is formatted first, into a
 * buffer that grows to its high-water mark. The first call passes a null
 * buffer and a zero size, which vsnprintf is defined to treat as a
 * measurement. */
static char *log_text;
static size_t log_cap;

void host_log(int level, const char *category, const char *fmt, ...)
{
    static const enum retro_log_level levels[] = {
        RETRO_LOG_DEBUG, RETRO_LOG_ERROR, RETRO_LOG_WARN, RETRO_LOG_INFO, RETRO_LOG_DEBUG};
    static const char *const names[] = RP6502_LOG_LEVEL_NAMES;
    va_list ap;
    va_start(ap, fmt);
    if (!log_cb)
    {
        fprintf(stderr, "%s %s: ", names[level], category);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
        return;
    }
    va_list sizing;
    va_copy(sizing, ap);
    int n = vsnprintf(log_text, log_cap, fmt, sizing);
    va_end(sizing);
    if (n >= 0 && (size_t)n >= log_cap)
    {
        log_cap = (size_t)n + 1;
        log_text = realloc(log_text, log_cap);
        vsnprintf(log_text, log_cap, fmt, ap);
    }
    va_end(ap);
    if (n >= 0)
        log_cb(levels[level], "%s: %s\n", category, log_text);
}

static const struct retro_core_option_v2_definition option_defs[] = {
    {
        "rp6502_phi2", "CPU Speed", NULL,
        "How fast the 65C02 runs. The hardware is adjustable too, and a program "
        "written for a slower machine may want a slower one here.",
        NULL, NULL,
        {{"8000", "8.0 MHz"}, {"6000", "6.0 MHz"}, {"4000", "4.0 MHz"},
         {"2000", "2.0 MHz"}, {"1000", "1.0 MHz"}, {NULL, NULL}},
        "8000",
    },
    {
        "rp6502_code_page", "Code Page", NULL,
        "The OEM code page the terminal and the filesystem speak.",
        NULL, NULL,
        {{"437", "437 (US)"}, {"850", "850 (Latin-1)"}, {"852", "852 (Latin-2)"},
         {"858", "858 (Latin-1 + Euro)"}, {"866", "866 (Cyrillic)"}, {NULL, NULL}},
        "437",
    },
    {
        "rp6502_mem_fill", "Memory At Power-On", NULL,
        "What RAM holds before a program writes it. Real SRAM comes up random, "
        "and a program that reads what it never wrote should fail here the way "
        "it would on hardware.",
        NULL, NULL,
        {{"random", "Random"}, {"00", "Zeros"}, {"ff", "Ones"}, {NULL, NULL}},
        "random",
    },
    {NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};

static struct retro_core_options_v2 options_v2 = {
    NULL, (struct retro_core_option_v2_definition *)option_defs};

static const char *option_value(const char *key)
{
    struct retro_variable var = {key, NULL};
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        return var.value;
    return NULL;
}

/* An rp6502_phi2 or rp6502_code_page this core cannot read, or reads as out
 * of range, leaves that setting alone. An rp6502_mem_fill it cannot read is
 * the random fill. */
static void apply_options(bool started)
{
    const char *v = option_value("rp6502_phi2");
    long khz = v ? strtol(v, NULL, 10) : 0;
    if (khz >= PHI2_MIN_KHZ && khz <= PHI2_MAX_KHZ)
    {
        phi2_set_khz((uint16_t)khz);
        if (started)
            phi2_set_khz_run((uint16_t)khz);
    }

    v = option_value("rp6502_code_page");
    long cp = v ? strtol(v, NULL, 10) : 0;
    if (cp > 0 && cp <= UINT16_MAX)
    {
        oem_set_code_page((uint16_t)cp);
        if (started)
            oem_set_code_page_run((uint16_t)cp);
    }

    /* No second call for a machine already up, because these values are read
     * by sram_init and xram_init, which run on every boot. */
    v = option_value("rp6502_mem_fill");
    bool fill_random = true;
    uint8_t fill_value = 0x00;
    if (v && !strcmp(v, "00"))
        fill_random = false;
    else if (v && !strcmp(v, "ff"))
    {
        fill_random = false;
        fill_value = 0xFF;
    }
    sram_set_fill(fill_random, fill_value, host_seed());
    xram_set_fill(fill_random, fill_value, host_seed());
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

/* What each RetroPad button does on this machine, so a frontend's remapper and
 * its on-screen gamepad have a name to show instead of a number. The mapping
 * is positional, so each label is this machine's name for the button under the
 * same thumb rather than the RetroPad's own name for it. */
static const struct retro_input_descriptor input_descriptors[] = {
#define GAMEPAD_DESC(port)                                                             \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},         \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},     \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},     \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},   \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A"},                 \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B"},                 \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "X"},                 \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Y"},                 \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1"},                \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1"},                \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},               \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},               \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},               \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"},               \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},       \
    {port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},         \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,                    \
     RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"},                                    \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,                    \
     RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"},                                    \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,                   \
     RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"},                                   \
    {port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,                   \
     RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"}
    GAMEPAD_DESC(0), GAMEPAD_DESC(1), GAMEPAD_DESC(2), GAMEPAD_DESC(3),
#undef GAMEPAD_DESC
    {0, 0, 0, 0, NULL},
};

/* Declaring the four ports is how a frontend learns they exist. A lightgun is
 * offered beside the gamepads because the machine's tablet is an absolute
 * pointer and so is a gun, and a player who owns one has nowhere else to plug
 * it in. */
static const struct retro_controller_description gamepad_types[] = {
    {"Gamepad", RETRO_DEVICE_JOYPAD},
    {"Gamepad (Analog)", RETRO_DEVICE_ANALOG},
    {"Lightgun", RETRO_DEVICE_LIGHTGUN},
    {NULL, 0},
};

static const struct retro_controller_info controller_info[] = {
    {gamepad_types, 3}, {gamepad_types, 3}, {gamepad_types, 3}, {gamepad_types, 3}, {NULL, 0},
};

/* The same options a frontend too old for v2 can still read. A frontend is
 * sent one form or the other: v2 where it has it, and otherwise SET_VARIABLES,
 * which every frontend understood before core options had versions, including
 * the ones that speak the v1 in between. Both are built from the one list
 * above so that a new option cannot reach only half of them. */
static struct retro_variable variables[
    sizeof option_defs / sizeof *option_defs];
static char variable_text[sizeof option_defs / sizeof *option_defs][256];

static void declare_options(retro_environment_t cb)
{
    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
    {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
        return;
    }

    /* "label; first|second|third", the shape every frontend has understood
     * since before core options had versions. */
    size_t n = 0;
    for (const struct retro_core_option_v2_definition *d = option_defs; d->key; d++, n++)
    {
        size_t at = (size_t)snprintf(variable_text[n], sizeof variable_text[0],
                                     "%s; ", d->desc);
        /* The default goes first: that is how this form says which it is. */
        for (int pass = 0; pass < 2; pass++)
            for (const struct retro_core_option_value *v = d->values; v->value; v++)
            {
                bool is_default = d->default_value && !strcmp(v->value, d->default_value);
                if (is_default != (pass == 0))
                    continue;
                if (at < sizeof variable_text[0])
                    at += (size_t)snprintf(variable_text[n] + at,
                                           sizeof variable_text[0] - at, "%s%s",
                                           at && variable_text[n][at - 1] != ' ' ? "|" : "",
                                           v->value);
            }
        variables[n].key = d->key;
        variables[n].value = variable_text[n];
    }
    variables[n].key = NULL;
    variables[n].value = NULL;
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    declare_options(cb);
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)controller_info);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void)
{
    struct retro_log_callback logging;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;

    struct retro_keyboard_callback kb = {input_keyboard_event};
    if (environ_cb)
    {
        environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb);
        input_init(environ_cb);
    }
}

/* A frontend may call retro_init again after this, so the machine is stopped
 * here and the last session's ROM, geometry and seed are dropped. */
void retro_deinit(void)
{
    if (machine_inited)
    {
        sys_stop();
        sys_commit();
    }
    machine_inited = false;
    free(loaded_rom), loaded_rom = NULL;
    free(loaded_path), loaded_path = NULL;
    shutdown_sent = false;
    geom_w = geom_h = 0;
    hint_shown = false;
    run_seed_taken = false;
    input_reset();
    log_cb = NULL;
    /* A frontend that unloads this library drops every pointer with it, so
     * what the machine and this file hold is given back here rather than at
     * exit. retro_run cannot be in flight: a frontend calls this after the
     * game is unloaded, on the thread it calls everything else on. */
    aud_shutdown();
    free(log_text), log_text = NULL, log_cap = 0;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof *info);
    info->library_name = "Picocomputer 6502";
    info->library_version = version_bare();
    info->valid_extensions = "rp6502";
    /* A program's assets are never read into memory: a ROM: open scans the
     * .rp6502 for them on demand, so the file has to stay where it is. */
    info->need_fullpath = true;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof *info);
    /* The boot console is the largest canvas. A program that picks a smaller
     * one announces it with SET_GEOMETRY from retro_run as it comes up. */
    info->geometry.base_width = VGA_MAX_WIDTH;
    info->geometry.base_height = VGA_MAX_HEIGHT;
    info->geometry.max_width = VGA_MAX_WIDTH;
    info->geometry.max_height = VGA_MAX_HEIGHT;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = VGA_HZ;
    info->timing.sample_rate = RETRO_AUD_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    input_set_port_device(port, device);
}

/* A frontend binds the keyboard to its own gamepad and hotkeys and keeps the
 * mouse for its own cursor, so on a machine that is a computer both look
 * broken until the player turns that off, which RetroArch calls Game Focus.
 * A core can neither turn it on nor ask whether it is on, so the only thing
 * left is to say so on screen the first time a program wants either device. */
static void say_how_to_type(void)
{
    if (hint_shown || !environ_cb)
        return;
    hint_shown = true;

    static const char text[] =
        "Enable Game Focus for Keyboard and Mouse.";

    unsigned version = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version) &&
        version >= 1)
    {
        struct retro_message_ext msg = {
            .msg = text,
            .duration = 6000,
            .priority = 1,
            .level = RETRO_LOG_INFO,
            .target = RETRO_MESSAGE_TARGET_ALL,
            .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
            .progress = -1,
        };
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
        return;
    }

    /* A frontend from before SET_MESSAGE_EXT still has SET_MESSAGE, which
     * counts in frames rather than milliseconds. */
    struct retro_message msg = {.msg = text, .frames = 6 * VGA_HZ};
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

/* A frontend offers no way to name a seed, so it comes from the OS. */
uint32_t host_seed(void)
{
    if (!run_seed_taken)
    {
        run_seed = os_random();
        run_seed_taken = true;
    }
    return run_seed;
}

/* A string in the guest's code page, allocated to fit: UTF-8 to OEM only ever
 * contracts, so the argument's own length bounds the result. The caller frees.
 *
 * This is core's conversion rather than os_argv_to_oem because a frontend
 * hands its paths over as UTF-8 on Windows as anywhere else, while the sokol
 * emulator's ANSI main() is given the OS's own code page instead. */
static char *argv_to_oem(const char *arg)
{
    size_t sz = strlen(arg) + 1;
    char *oem = malloc(sz);
    if (oem && oem_from_utf8(arg, oem, sz) >= sz)
    {
        free(oem);
        oem = NULL;
    }
    return oem;
}

/* Where the frontend wants a program's saves to go. The drive is still the
 * whole host filesystem, as on every other host; this only sets the directory
 * a program starts in. */
static void enter_save_directory(const char *content_path)
{
    const char *dir = NULL;
    char *own = NULL;
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || !dir || !*dir)
    {
        /* No save directory: the program's own folder, which is where the SDK
         * puts what it ships beside a ROM. GET_GAME_INFO_EXT is asked for it
         * rather than cutting the path, because the frontend also knows when
         * the program came out of an archive, in which case the path points
         * inside the zip and has no directory to cut. */
        struct retro_game_info_ext *ext = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &ext) &&
            ext && ext->dir && *ext->dir)
        {
            char *oem_dir = argv_to_oem(ext->dir);
            if (oem_dir)
            {
                api_errno err;
                drive_chdir(oem_dir, &err);
                free(oem_dir);
            }
            return;
        }
        if (!content_path)
            return;
        own = strdup(content_path);
        if (!own)
            return;
        char *slash = strrchr(own, '/');
#ifdef _WIN32
        char *back = strrchr(own, '\\');
        if (back > slash)
            slash = back;
#endif
        if (!slash)
        {
            free(own);
            return;
        }
        *slash = 0;
        dir = own;
    }
    char *oem = argv_to_oem(dir);
    if (oem)
    {
        api_errno err;
        drive_chdir(oem, &err);
    }
    free(oem), free(own);
}

/* The first load cold-boots the machine; every load after it refills RAM, so
 * a program never sees what the last one left behind. */
static bool boot(const char *rom_oem)
{
    apply_options(machine_inited);
    unsigned flags = PROC_UNCHAIN;
    if (machine_inited)
        flags |= PROC_REFILL;
    else
    {
        sys_init();
        machine_inited = true;
    }
    booted = false;
    if (!proc_boot(rom_oem, 0, NULL, flags))
        return false;
    vga_set_framebuffer(frame_buf);
    sys_commit();
    booted = true;
    shutdown_sent = false;
    geom_w = geom_h = 0;
    return true;
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->path)
    {
        RP6502_LOG(retro, ERROR, "this core plays a .rp6502 program; there is nothing to run");
        return false;
    }

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        RP6502_LOG(retro, ERROR, "this frontend cannot show XRGB8888");
        return false;
    }

    /* Made absolute before anything moves, because the frontend's path is
     * relative to a directory enter_save_directory is about to leave, and
     * retro_reset has to find the same file again from wherever the program
     * has since gone. */
    char *given = argv_to_oem(game->path);
    if (!given)
    {
        RP6502_LOG(retro, ERROR, "cannot take the ROM path");
        return false;
    }
    char *abs = os_dir_realpath(given);
    free(loaded_rom);
    loaded_rom = abs ? abs : given;
    if (abs)
        free(given);

    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)input_descriptors);

    free(loaded_path);
    loaded_path = strdup(game->path);
    if (!loaded_rom || !loaded_path)
    {
        RP6502_LOG(retro, ERROR, "cannot take the ROM path");
        return false;
    }
    enter_save_directory(loaded_path);

    if (!boot(loaded_rom))
        return false;

    /* Two blocks in one flat map, XRAM above the 6502's own space. A blank
     * addrspace on both is what keeps them in one namespace; naming them would
     * make two spaces that each start at zero. libretro.h defines a select of
     * 0 as start and len being the whole mapping, and requires a power-of-two
     * len in that case, which both of these are. */
    static struct retro_memory_descriptor descs[2];
    descs[0] = (struct retro_memory_descriptor){
        .flags = RETRO_MEMDESC_SYSTEM_RAM, .ptr = sram, .start = 0x00000, .len = 0x10000};
    descs[1] = (struct retro_memory_descriptor){
        .flags = RETRO_MEMDESC_VIDEO_RAM, .ptr = (void *)xram, .start = 0x10000, .len = 0x10000};
    struct retro_memory_map map = {descs, sizeof descs / sizeof *descs};
    environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);

    /* The blob is one fixed size and holds no pointer. Sent from here rather
     * than from retro_init, below every path above that can refuse, so a core
     * that never stood a program up never makes the claim. */
    uint64_t quirks = 0;
    environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type;
    (void)info;
    (void)num;
    return false;
}

void retro_unload_game(void)
{
    if (machine_inited)
    {
        sys_stop();
        sys_commit();
    }
    free(loaded_rom), loaded_rom = NULL;
    free(loaded_path), loaded_path = NULL;
}

/* The save directory is entered again because a chdir moves the whole host
 * process and nothing in a reboot puts it back, so a program that changed
 * directory would leave the next run starting there. */
void retro_reset(void)
{
    if (!loaded_rom)
        return;
    enter_save_directory(loaded_path);
    boot(loaded_rom);
}

/* The machine paints RGBA8 (0xAABBGGRR) and libretro was asked for XRGB8888
 * (0x00RRGGBB), so red and blue trade places. Swizzling in place is safe
 * because vga repaints every visible scanline of the canvas before the buffer
 * is handed over again. */
static void swizzle(uint32_t *px, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t v = px[i];
        px[i] = (v & 0x0000FF00u) | ((v & 0x000000FFu) << 16) | ((v >> 16) & 0xFFu);
    }
}

/* One frame's worth per call, which is what a frontend syncing on sound waits
 * for. A silent machine renders silence rather than nothing. */
static void push_audio(void)
{
    aud_render(audio_out, RETRO_AUD_FRAMES);
    for (int i = 0; i < RETRO_AUD_FRAMES * 2; i++)
        audio_buf[i] = (int16_t)(audio_out[i] * 32767.0f);
    audio_batch_cb(audio_buf, RETRO_AUD_FRAMES);
}

void retro_run(void)
{
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    {
        const char *v = option_value("rp6502_phi2");
        long khz = v ? strtol(v, NULL, 10) : 0;
        if (khz >= PHI2_MIN_KHZ && khz <= PHI2_MAX_KHZ)
            phi2_set_khz_run((uint16_t)khz);
        v = option_value("rp6502_code_page");
        long cp = v ? strtol(v, NULL, 10) : 0;
        if (cp > 0 && cp <= UINT16_MAX)
            oem_set_code_page_run((uint16_t)cp);
    }

    input_poll_cb();
    input_poll(input_state_cb);
    /* Not the gamepad or the tablet: a frontend polls pads and the pointer
     * with Game Focus on or off, and withholds only the keyboard and the
     * mouse. */
    if (!hint_shown && (std_console_asked() || keyboard_is_mapped() ||
                        mouse_is_mapped()))
        say_how_to_type();

    /* What the frontend will actually use this frame. A frontend without the
     * call leaves video and audio both on. Only the raster is skipped; the
     * beam, vsync and the 6502 run either way, because libretro.h requires the
     * next frame's video to be no different for the flag. video_cb is still
     * called exactly once below, and the frontend discards the frame. */
    int av = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av))
        av = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    bool want_video = (av & RETRO_AV_ENABLE_VIDEO) != 0;
    vga_set_scanout(want_video);

    vga_run_frame();

    int w, h;
    vga_canvas_size(&w, &h);
    if (w != geom_w || h != geom_h)
    {
        struct retro_game_geometry geom = {
            .base_width = (unsigned)w,
            .base_height = (unsigned)h,
            .max_width = VGA_MAX_WIDTH,
            .max_height = VGA_MAX_HEIGHT,
            .aspect_ratio = 4.0f / 3.0f,
        };
        /* The new geometry is remembered only if the frontend took it. A
         * frontend without this call is told again next frame, which costs
         * nothing and is the only way it can ever learn. */
        if (environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom))
        {
            geom_w = w;
            geom_h = h;
        }
    }

    if (want_video)
        swizzle(frame_buf, (size_t)w * (size_t)h);
    video_cb(frame_buf, (unsigned)w, (unsigned)h, (size_t)w * sizeof *frame_buf);

    /* RETRO_AV_ENABLE_AUDIO clear only discards this frame's sound, and
     * libretro.h requires the next frame to sound no different for it, so the
     * mixer is still clocked; aud_render is what advances it.
     * RETRO_AV_ENABLE_HARD_DISABLE_AUDIO is the frontend promising it will
     * never want audio again, which is the one that allows synthesizing
     * nothing. */
    if (!(av & RETRO_AV_ENABLE_HARD_DISABLE_AUDIO))
        push_audio();

    /* The program stopped and this host has no monitor to fall back to, so the
     * core is finished. The frame above is the last thing it drew. */
    if (proc_exited() && !shutdown_sent)
    {
        shutdown_sent = true;
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

void *retro_get_memory_data(unsigned id)
{
    switch (id)
    {
    case RETRO_MEMORY_SYSTEM_RAM: return sram;
    /* xram is volatile because on real hardware something else writes it
     * while the machine reads. A frontend reading it between frames races
     * nothing, so casting the qualifier away here is safe. */
    case RETRO_MEMORY_VIDEO_RAM: return (void *)xram;
    default: return NULL;
    }
}

size_t retro_get_memory_size(unsigned id)
{
    switch (id)
    {
    case RETRO_MEMORY_SYSTEM_RAM: return 0x10000;
    case RETRO_MEMORY_VIDEO_RAM: return 0x10000;
    default: return 0;
    }
}

/* What the frontend is going to do with the blob it is asking for, in the two
 * facts this core can act on. GET_SAVESTATE_CONTEXT is marked experimental and a
 * frontend may not have it; RETRO_AV_ENABLE_FAST_SAVESTATES is the deprecated
 * spelling of that same-binary guarantee, so it answers when the newer call
 * does not. */
static unsigned savestate_flags(void)
{
    enum retro_savestate_context ctx = RETRO_SAVESTATE_CONTEXT_NORMAL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &ctx))
        switch (ctx)
        {
        case RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE:
        case RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_BINARY: return SST_TRUSTED;
        case RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY: return SST_SHARED;
        default: return 0;
        }
    int av = 0;
    if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) &&
        (av & RETRO_AV_ENABLE_FAST_SAVESTATES))
        return SST_TRUSTED;
    return 0;
}

size_t retro_serialize_size(void) { return sst_size(); }

bool retro_serialize(void *data, size_t size)
{
    if (!booted)
        return false;
    fs_std_settle();
    const char *why = sst_save(data, size, savestate_flags());
    if (why)
        RP6502_LOG(retro, ERROR, "cannot save state: %s", why);
    return why == NULL;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (!booted)
        return false;
    fs_std_settle();
    unsigned flags = savestate_flags();
    const char *why = sst_load(data, size, flags, loaded_rom);
    if (why)
    {
        RP6502_LOG(retro, ERROR, "cannot load state: %s", why);
        /* A refused load rolls itself back, but a trusted load has no
         * rollback and a rollback can itself fail. Either leaves no machine
         * standing, and only this file has a ROM to stand one up with. */
        if (!sys_active() && loaded_rom)
            boot(loaded_rom);
        return false;
    }
    /* Two things no savestate row can put back: shutdown_sent belongs to this
     * file, and the frontend's devices belong to the frontend. Neither is done
     * under either flag, because a same-session load has nothing to
     * re-announce, and injecting this peer's idea of who is plugged in into a
     * rolled-back netplay state is itself a desync. */
    if (!flags)
    {
        shutdown_sent = proc_exited();
        input_state_restored();
    }
    /* The picture and the geometry need no repair: no savestate row touches
     * the framebuffer, and the next retro_run announces whatever canvas it
     * finds. */
    return true;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
