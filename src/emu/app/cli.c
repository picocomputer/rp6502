/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Command-line parsing: the getopt_long option table and the ROM-asset
 * tokenizer. main.c runs parse_args over the real command line and (via
 * merge_rom_args) over the launch ROM's "emulator" asset; getopt state is reset
 * between passes so the same parser can run several times.
 */

#include "emu/app/cli.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "getopt.h" /* vendored wingetopt (MSVC has no getopt) */
#else
#include <getopt.h>
#endif

const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void options_init(options *o)
{
    memset(o, 0, sizeof *o);
    o->frames = 120;
    o->scale = 2.0;
    o->scale_filter = EMU_FILTER_SHARP; /* default sharp */
}

/* "RRGGBB" (optional leading '#') -> three 0-255 channels. */
static bool parse_hex_color(const char *s, int *r, int *g, int *b)
{
    if (*s == '#')
        s++;
    if (strlen(s) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    long v = strtol(s, NULL, 16);
    *r = (int)((v >> 16) & 0xFF);
    *g = (int)((v >> 8) & 0xFF);
    *b = (int)(v & 0xFF);
    return true;
}

/* Long-option codes (>= 256 so they never collide with a short-option char). */
enum
{
    OPT_SCREENSHOT = 256, OPT_FRAMES, OPT_SCALE, OPT_SCALE_FILTER, OPT_INPUT,
    OPT_FS, OPT_TMPDRIVE, OPT_ROM, OPT_BGCOLOR, OPT_PHI2, OPT_CP, OPT_SEED,
    OPT_NO_AUDIO, OPT_DEBUG, OPT_DAP, OPT_CREDITS, OPT_INI,
};
static const struct option longopts[] = {
    {"screenshot",   required_argument, NULL, OPT_SCREENSHOT},
    {"frames",       required_argument, NULL, OPT_FRAMES},
    {"scale",        required_argument, NULL, OPT_SCALE},
    {"scale-filter", required_argument, NULL, OPT_SCALE_FILTER},
    {"input",        required_argument, NULL, OPT_INPUT},
    {"fs",           required_argument, NULL, OPT_FS},
    {"tmpdrive",     no_argument,       NULL, OPT_TMPDRIVE},
    {"rom",          required_argument, NULL, OPT_ROM},
    {"bgcolor",      required_argument, NULL, OPT_BGCOLOR},
    {"phi2",         required_argument, NULL, OPT_PHI2},
    {"cp",           required_argument, NULL, OPT_CP},
    {"seed",         required_argument, NULL, OPT_SEED},
    {"no-audio",     no_argument,       NULL, OPT_NO_AUDIO},
    {"debug",        no_argument,       NULL, OPT_DEBUG},
    {"dap",          no_argument,       NULL, OPT_DAP},
    {"credits",      no_argument,       NULL, OPT_CREDITS},
    {"ini",          required_argument, NULL, OPT_INI},
    {NULL, 0, NULL, 0},
};

/* Reset getopt's global state so the same parser can run over several argv sets
 * (the command line, then the ROM "emulator" asset). glibc/musl re-init when
 * optind is set to 0; the BSD-family getopt (and Windows/wingetopt, macOS) needs
 * optreset. */
static void getopt_reset(void)
{
#if defined(_WIN32) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    optind = 1;
    optreset = 1;
#else
    optind = 0;
#endif
}

int parse_args(int argc, char **argv, options *o)
{
    getopt_reset();
    opterr = 0; /* we print our own messages (the ':' optstring reports them) */
    int c;
    while ((c = getopt_long(argc, argv, ":", longopts, NULL)) != -1)
    {
        switch (c)
        {
        case OPT_SCREENSHOT: o->shot = optarg; break;
        case OPT_FRAMES: o->frames = atoi(optarg); break;
        case OPT_SCALE: o->scale = atof(optarg); break;
        case OPT_SCALE_FILTER:
            if (!strcmp(optarg, "nearest"))
                o->scale_filter = EMU_FILTER_NEAREST;
            else if (!strcmp(optarg, "linear"))
                o->scale_filter = EMU_FILTER_LINEAR;
            else if (!strcmp(optarg, "sharp"))
                o->scale_filter = EMU_FILTER_SHARP;
            else
            {
                fprintf(stderr, "rp6502-emu: bad --scale-filter '%s' "
                                "(want nearest|linear|sharp)\n", optarg);
                return 2;
            }
            break;
        case OPT_INPUT: o->input = optarg; break;
        case OPT_FS: o->fsdir = optarg; break;
        case OPT_TMPDRIVE: o->tmpdrive = true; break;
        case OPT_ROM:
            if (o->n_installs < (int)(sizeof(o->installs) / sizeof(o->installs[0])))
                o->installs[o->n_installs++] = optarg;
            break; /* overflow silently dropped */
        case OPT_BGCOLOR:
            if (!parse_hex_color(optarg, &o->bg_r, &o->bg_g, &o->bg_b))
            {
                fprintf(stderr, "rp6502-emu: bad --bgcolor (want RRGGBB)\n");
                return 2;
            }
            o->have_bg = true;
            break;
        case OPT_PHI2: o->phi2_khz = atoi(optarg); break;
        case OPT_CP: o->code_page = atoi(optarg); break;
        case OPT_SEED:
            o->seed = strtoull(optarg, NULL, 0);
            o->have_seed = true;
            break;
        case OPT_NO_AUDIO: o->no_audio = true; break;
        case OPT_DEBUG: o->debug = true; break;
        case OPT_DAP: o->dap = true; break;
        case OPT_CREDITS: o->credits = true; break;
        case OPT_INI: o->inidir = optarg; break;
        case ':':
            fprintf(stderr, "rp6502-emu: option '%s' requires a value\n",
                    argv[optind - 1]);
            return 2;
        case '?':
        default:
            fprintf(stderr, "rp6502-emu: unknown option '%s'\n", argv[optind - 1]);
            return 2;
        }
    }
    /* The lone positional is the ROM path; tolerate empty args (e.g. an unfilled
     * launch.json input) by taking the first non-empty one. */
    for (int i = optind; i < argc; i++)
        if (argv[i][0])
        {
            o->rom = argv[i];
            break;
        }
    return 0;
}

int tokenize_args(const char *s, char **argv, int max, char *store, size_t cap)
{
    int argc = 0;
    size_t w = 0;
    const char *p = s;
    while (*p && argc < max)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        size_t tok = w;
        char quote = 0;
        while (*p)
        {
            char c = *p;
            if (quote)
            {
                if (c == quote)
                {
                    quote = 0;
                    p++;
                    continue;
                }
            }
            else
            {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                    break;
                if (c == '"' || c == '\'')
                {
                    quote = c;
                    p++;
                    continue;
                }
            }
            if (w + 1 < cap)
                store[w++] = c;
            p++;
        }
        if (w < cap)
            store[w++] = 0;
        else
            store[cap - 1] = 0;
        argv[argc++] = &store[tok];
    }
    return argc;
}
