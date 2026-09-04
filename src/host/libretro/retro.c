/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine as a libretro core.
 *
 * A frontend owns the loop, so this host has none: retro_run advances the
 * machine exactly one 60 Hz frame and hands over the picture and the sound
 * it made. That is the whole of the difference from the desktop hosts, whose
 * window.c does the pacing the frontend does here.
 *
 * There is no monitor on this host and no debugger. A .rp6502 is what runs,
 * and when it stops, the core is done.
 */

#include "core/sys/config.h"
#include "input.h"

#include "core/str/oem.h"
#include "core/sys/random.h"
#include "host/host.h"
#include "core/sys/version.h"
#include "core/aud/mix.h"
#include "core/sys/com.h"
#include "osal/dir.h"
#include "osal/fs.h"
#include "core/sys/proc.h"
#include "core/rom/rom.h"
#include "core/sys/sys.h"
#include "core/wdc/phi2.h"
#include "core/wdc/resb.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/vga/vga_emu.h"
#include "osal/os.h"

#include "libretro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The rate this core declares in av_info. The machine makes every voice at
 * a YM3812's 49716 Hz and resamples to this, which is also the mixer's
 * default sink rate, so this core never has to say so. */
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

static char *loaded_rom;  /* OEM, absolute, for retro_reset; owned */
static char *loaded_path; /* as the frontend spelled it; owned */
static bool machine_inited;
static int geom_w, geom_h;
static bool shutdown_sent;
static bool hint_shown;

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

/* One finished line to the frontend, or to stderr when it gave no logger.
 * A core writing stderr is antisocial but better than a diagnostic nobody
 * ever sees; the frontend's log is where this is meant to land. */
static void retro_say(enum retro_log_level level, const char *msg)
{
    if (log_cb)
        log_cb(level, "%s\n", msg);
    else
        fprintf(stderr, "rp6502: %s\n", msg);
}

/* This core's own diagnostics, which are about the frontend rather than about
 * the machine: a game that is not one, a pixel format it will not show. */
__printflike(1, 2) static void retro_log(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    retro_say(RETRO_LOG_ERROR, msg);
}

/* The machine's console, line by line. com_tx_write is where every
 * terminal-bound byte passes once, so this is the whole of what the machine
 * says: the terminal a frontend renders is a picture of it, and its log is
 * the only place the text itself can go. A line too long for the buffer is
 * split rather than truncated. */
static void retro_tx_tap(const char *buf, int len)
{
    static char line[256];
    static size_t used;
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        if (c == '\r')
            continue;
        if (c == '\n' || used == sizeof line - 1)
        {
            line[used] = 0;
            used = 0;
            if (line[0])
                retro_say(RETRO_LOG_INFO, line);
        }
        if (c != '\n')
            line[used++] = c;
    }
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

/* The config half of the settings pattern, applied before every boot the way
 * the desktop applies its command line before its one. A value we cannot
 * read is the default — a frontend is not a place to refuse to start over a
 * bad number.
 *
 * `started` says the drivers have already adopted their config once, so a
 * setting has to be given to the running machine as well: cpu_init and
 * oem_init only read the config half at cold boot, and a program restarted
 * after someone changed a setting should come up with the setting. */
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

    /* Read by the fills, which every boot runs, so this one needs no second
     * telling. */
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

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

/* What each RetroPad button does on this machine, so a frontend's remapper
 * and its on-screen gamepad have something to say instead of a number. The
 * machine's gamepad is a modern one and the mapping is positional, so the labels
 * are the machine's own names for the buttons under the same thumbs. */
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

/* The machine has four gamepads and reads them as one modern controller each.
 * Saying so is how a frontend knows the ports exist at all. */
static const struct retro_controller_description gamepad_types[] = {
    {"Gamepad", RETRO_DEVICE_JOYPAD},
    {"Gamepad (Analog)", RETRO_DEVICE_ANALOG},
    {NULL, 0},
};

static const struct retro_controller_info controller_info[] = {
    {gamepad_types, 2}, {gamepad_types, 2}, {gamepad_types, 2}, {gamepad_types, 2}, {NULL, 0},
};

/* The same options a frontend too old for v2 can still read. Two forms cover
 * every frontend there is: v2 is what a current one wants, and SET_VARIABLES
 * is what every version understood before options had versions at all — a
 * frontend that speaks the v1 in between speaks this too. Built from the one
 * list above so a new option cannot reach half the frontends. */
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
    com_set_tx_tap(retro_tx_tap);

    struct retro_keyboard_callback kb = {input_keyboard_event};
    if (environ_cb)
    {
        environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb);
        input_init(environ_cb);
    }
}

/* The other half of retro_init. A frontend may init again afterwards, and
 * what it gets then should be this library as it was loaded, not as the
 * last session left it. */
void retro_deinit(void)
{
    if (machine_inited)
    {
        sys_stop();
        sys_commit(); /* no more frames after this to do it in */
    }
    machine_inited = false;
    free(loaded_rom), loaded_rom = NULL;
    free(loaded_path), loaded_path = NULL;
    shutdown_sent = false;
    geom_w = geom_h = 0;
    hint_shown = false;
    input_reset();
    com_set_tx_tap(NULL);
    log_cb = NULL;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof *info);
    info->library_name = "Picocomputer 6502";
    /* The frontend prints this beside the name, so it supplies the word the
     * stamp has in front of a tagged build. */
    info->library_version = version_bare();
    info->valid_extensions = "rp6502";
    /* A program's assets are never read into memory: a ROM: open scans the
     * file for them on demand, so the file has to stay where it is. */
    info->need_fullpath = true;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof *info);
    /* The boot console is the largest canvas; a program that picks a smaller
     * one says so with SET_GEOMETRY as it comes up. */
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

/* ------------------------------------------------------------------ */
/* Content                                                             */
/* ------------------------------------------------------------------ */

/* Say once, on screen, how to type.
 *
 * A frontend binds the keyboard to its own gamepad and hotkeys, so on a machine
 * that is a computer the keyboard looks broken until the player turns that
 * off — Game Focus, in RetroArch. The core cannot turn it on and there is
 * no environment call to ask, so the honest thing is to tell them. Once per
 * session: it is an instruction, not a status. */
static void say_how_to_type(void)
{
    if (hint_shown || !environ_cb)
        return;
    hint_shown = true;

    static const char text[] =
        "Keyboard: turn on Game Focus to type (Scroll Lock in RetroArch)";

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

    /* A frontend from before that call still has the old one, which counts
     * in frames rather than milliseconds. */
    struct retro_message msg = {.msg = text, .frames = 6 * VGA_HZ};
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

/* The seed for this run, decided once: a frontend offers no --seed, so it is
 * the OS's, and it is asked for both the stream and the memory fill. */
static uint32_t run_seed;
static bool run_seed_taken;

uint32_t host_seed(void)
{
    if (!run_seed_taken)
    {
        run_seed = os_random();
        run_seed_taken = true;
    }
    return run_seed;
}

/* argv in the guest's code page, allocated to fit: the conversion only ever
 * contracts, so the argument's own length is the bound. The caller frees.
 *
 * A frontend hands its paths over as UTF-8, on Windows as anywhere else, so
 * this is core's conversion and not an OS call. The emulator beside this one
 * needs os_argv_to_oem because an ANSI main() is given its own code page. */
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

/* Where the frontend wants a program's saves to go. MSC0: is still the whole
 * host filesystem, as on every other host; this is only where a program
 * starts out. */
static void enter_save_directory(const char *content_path)
{
    const char *dir = NULL;
    char *own = NULL;
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || !dir || !*dir)
    {
        /* No save directory: the program's own folder, which is where the SDK
         * puts what it ships beside a ROM. */
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

/* Stand a program up: a fresh machine, the image, then run. The first load
 * cold-boots; every one after is the same fresh-machine sequence the test
 * bench boots with. */
static bool boot(const char *rom_oem)
{
    apply_options(machine_inited);
    unsigned flags = PROC_UNCHAIN;
    if (machine_inited)
        flags |= PROC_REFILL; /* every load after the first is a fresh machine */
    else
    {
        sys_init();
        machine_inited = true;
    }
    if (!proc_boot(rom_oem, 0, NULL, flags))
        return false;
    vga_set_framebuffer(frame_buf);
    sys_commit();
    shutdown_sent = false;
    geom_w = geom_h = 0; /* the first frame announces whatever canvas it is */
    return true;
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->path)
    {
        retro_log("this core plays a .rp6502 program; there is nothing to run");
        return false;
    }

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        retro_log("this frontend cannot show XRGB8888");
        return false;
    }

    /* Absolute before anything moves: the frontend's path is relative to a
     * directory we are about to leave, and retro_reset has to find the same
     * file again from wherever the program has since gone. */
    char *given = argv_to_oem(game->path);
    if (!given)
    {
        retro_log("cannot take the ROM path");
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
        retro_log("cannot take the ROM path");
        return false;
    }
    enter_save_directory(loaded_path);

    if (!boot(loaded_rom))
        return false;
    say_how_to_type();
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
        sys_commit(); /* the frontend may never call us again */
    }
    free(loaded_rom), loaded_rom = NULL;
    free(loaded_path), loaded_path = NULL;
}

/* A reset is the boot a load does, and all of it: the program left the
 * machine in some directory of its own, and a fresh start does not inherit
 * where it got to. */
void retro_reset(void)
{
    if (!loaded_rom)
        return;
    enter_save_directory(loaded_path);
    boot(loaded_rom);
}

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

/* The machine paints RGBA8 (0xAABBGGRR); libretro asked for XRGB8888
 * (0x00RRGGBB). Red and blue trade places, which is its own inverse, so the
 * pixels the tests hash are the pixels a frontend gets. In place is safe:
 * every visible scanline is repainted before it is handed over again. */
static void swizzle(uint32_t *px, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t v = px[i];
        px[i] = (v & 0x0000FF00u) | ((v & 0x000000FFu) << 16) | ((v >> 16) & 0xFFu);
    }
}

/* One frame's worth per call, which is what a frontend syncing on sound
 * waits for, as the int16 pairs libretro takes. A silent machine generates
 * silence rather than nothing: the standing BEL is always the installed
 * device. */
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

    /* The frontend paces us: one frame per call, as fast as this can run it. */
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
        /* Only remember having said it if it was heard. A frontend without
         * this call is told again on the next frame, which costs nothing and
         * is the only way it can ever learn. */
        if (environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom))
        {
            geom_w = w;
            geom_h = h;
        }
    }

    swizzle(frame_buf, (size_t)w * (size_t)h);
    video_cb(frame_buf, (unsigned)w, (unsigned)h, (size_t)w * sizeof *frame_buf);

    push_audio();

    /* The program stopped and there is no monitor here to fall back to, so
     * the core is finished. The frame above is the last thing it drew. */
    if (!resb_running() && !shutdown_sent)
    {
        shutdown_sent = true;
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Memory, and the things this core does not do                        */
/* ------------------------------------------------------------------ */

void *retro_get_memory_data(unsigned id)
{
    switch (id)
    {
    case RETRO_MEMORY_SYSTEM_RAM: return sram;
    /* xram is volatile because the machine's own readers race the bus with
     * it; a frontend reading it between frames does not. */
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

/* No savestates yet. Answering zero is how a core says so; answering anything
 * else promises rewind and netplay this machine cannot keep. */
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
