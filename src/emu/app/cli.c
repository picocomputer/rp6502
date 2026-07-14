/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
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

const char *cli_base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void cli_options_init(cli_options *o)
{
    memset(o, 0, sizeof *o);
    o->frames = 120;
    o->scale = 1.5;
    o->vsync = true;
    o->scale_filter = WINDOW_FILTER_SHARP;
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
    OPT_SCREENSHOT = 256, OPT_FRAMES, OPT_SCALE, OPT_FILTER, OPT_INPUT,
    OPT_TMPDRIVE, OPT_ROM, OPT_BGCOLOR, OPT_PHI2, OPT_CP, OPT_SEED,
    OPT_MUTE, OPT_DEBUG, OPT_DAP, OPT_CREDITS, OPT_INI, OPT_VSYNC, OPT_NO_VSYNC,
};
static const struct option longopts[] = {
    {"screenshot",   required_argument, NULL, OPT_SCREENSHOT},
    {"frames",       required_argument, NULL, OPT_FRAMES},
    {"scale",        required_argument, NULL, OPT_SCALE},
    {"vsync",        no_argument,       NULL, OPT_VSYNC},
    {"no-vsync",     no_argument,       NULL, OPT_NO_VSYNC},
    {"filter",       required_argument, NULL, OPT_FILTER},
    {"input",        required_argument, NULL, OPT_INPUT},
    {"tmpdrive",     no_argument,       NULL, OPT_TMPDRIVE},
    {"rom",          required_argument, NULL, OPT_ROM},
    {"bgcolor",      required_argument, NULL, OPT_BGCOLOR},
    {"phi2",         required_argument, NULL, OPT_PHI2},
    {"cp",           required_argument, NULL, OPT_CP},
    {"seed",         required_argument, NULL, OPT_SEED},
    {"mute",         no_argument,       NULL, OPT_MUTE},
    {"debug",        no_argument,       NULL, OPT_DEBUG},
    {"dap",          no_argument,       NULL, OPT_DAP},
    {"credits",      no_argument,       NULL, OPT_CREDITS},
    {"ini",          required_argument, NULL, OPT_INI},
    {NULL, 0, NULL, 0},
};

void cli_usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <rom.rp6502> [options] [-- <args...>]\n"
            "  --screenshot <file.png>   render headlessly to PNG and exit\n"
            "  --frames <n>              frames to run before screenshot (default 120)\n"
            "  --scale <n>               window scale, fractional ok (default 1.5)\n"
            "  --no-vsync                present uncapped instead of syncing to the display\n"
            "  --filter <f>              nearest|linear|sharp (default sharp)\n"
            "  --input <text>            queue keystrokes for stdin ('\\n' = Enter)\n"
            "  --tmpdrive                MSC0: = a fresh throwaway temp dir (isolate the ROM)\n"
            "  --rom <file>              install a .rp6502 on the null drive, reached\n"
            "                            as :basename; repeatable, the first one boots\n"
            "  --bgcolor RRGGBB          letterbox/pillarbox fill color (default 000000)\n"
            "  --phi2 <khz>              6502 clock in kHz (100-8000, default 8000)\n"
            "  --cp <n>                  OEM code page (437/720/737/771/775/850/852/855/\n"
            "                            857/860-866/869, default 437)\n"
            "  --seed <n>                fixed RNG seed for reproducible runs\n"
            "                            (default: host entropy)\n"
            "  --mute                    mute all audio (no synth, no OS audio device)\n"
            "  --debug                   on-screen machine debugger (CPU/VIA/disasm); holds\n"
            "                            the window open on stop for inspection\n"
            "  --dap                     act as a DAP debug adapter on stdio (implies --debug)\n"
            "  --credits                 print third-party credits/licenses and exit\n"
            "  --ini <file>              config file for the debugger UI layout\n"
            "                            (ImGui format; e.g. the workspace .rp6502)\n"
            "  -- <args...>              pass the remaining words to the ROM as argv[1..]\n"
            "A ROM's 'emulator' asset can preset these; the command line overrides it.\n",
            argv0);
}

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

int cli_parse_args(int argc, char **argv, cli_options *o)
{
    /* Split at the first standalone "--" before getopt sees it: the tail is the
     * ROM's argv[1..], never parsed as options. Truncating argc also confines
     * getopt's in-place permutation to the head, so the second pass over the
     * already-permuted real argv finds the separator and tail untouched.
     * (A literal "--" option value needs the --opt=-- form.) */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--"))
        {
            o->rom_args = &argv[i + 1];
            o->n_rom_args = argc - i - 1;
            argc = i;
            break;
        }
    getopt_reset();
    opterr = 0; /* we print our own messages (the ':' optstring reports them) */
    int c;
    while ((c = getopt_long(argc, argv, ":", longopts, NULL)) != -1)
    {
        switch (c)
        {
        case OPT_SCREENSHOT: o->shot = optarg; break;
        case OPT_FRAMES: o->frames = atoi(optarg); break;
        case OPT_SCALE: o->scale = atof(optarg); o->have_scale = true; break;
        case OPT_VSYNC: o->vsync = true; break;
        case OPT_NO_VSYNC: o->vsync = false; break;
        case OPT_FILTER:
            if (!strcmp(optarg, "nearest"))
                o->scale_filter = WINDOW_FILTER_NEAREST;
            else if (!strcmp(optarg, "linear"))
                o->scale_filter = WINDOW_FILTER_LINEAR;
            else if (!strcmp(optarg, "sharp"))
                o->scale_filter = WINDOW_FILTER_SHARP;
            else
            {
                fprintf(stderr, "rp6502-emu: bad --filter '%s' "
                                "(want nearest|linear|sharp)\n", optarg);
                return 2;
            }
            break;
        case OPT_INPUT: o->input = optarg; break;
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
        case OPT_MUTE: o->mute = true; break;
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

/* Tokenizer whitespace: space, tab, CR, LF only (not isspace's \v/\f). */
static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int cli_tokenize_args(const char *s, char **argv, int max, char *store, size_t cap)
{
    int argc = 0;
    size_t w = 0;
    const char *p = s;
    while (*p && argc < max)
    {
        while (is_ws(*p))
            p++;
        if (!*p)
            break;
        if (w >= cap)
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
                if (is_ws(c))
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
