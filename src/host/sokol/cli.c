/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
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
    o->fill_random = true;
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

/* One number grammar for the whole command line, and it is the script's: $FF
 * and 0xFF are hex, anything else is decimal. strtol's base 0 would make "010"
 * eight, which is not what someone typing a byte count means.
 *
 * A number the emulator cannot read is an error rather than a default. This is
 * input, not code — and the option that shows why is --seed, which exists only
 * to make a run repeatable: `--seed $SEED` with SEED unset used to become zero
 * and report the run as reproducible on a seed nobody chose. */
static bool cli_number(const char *s, long long *out)
{
    int base = 10;
    if (*s == '$')
        s++, base = 16;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2, base = 16;
    char *end;
    long long v = strtoll(s, &end, base);
    if (end == s || *end)
        return false;
    *out = v;
    return true;
}

static bool cli_bad(const char *opt, const char *value)
{
    fprintf(stderr, "rp6502-emu: bad %s '%s'\n", opt, value);
    return false;
}

/* --fill: "random", or the byte every cell gets. */
static bool parse_fill(const char *s, bool *random, uint8_t *value)
{
    if (!strcasecmp(s, "random"))
    {
        *random = true;
        return true;
    }
    long long v;
    if (!cli_number(s, &v) || v < 0 || v > 255)
        return false;
    *random = false;
    *value = (uint8_t)v;
    return true;
}

/* Long-option codes (>= 256 so they never collide with a short-option char). */
enum
{
    OPT_HELP = 256, OPT_SCREENSHOT, OPT_FRAMES, OPT_SCALE, OPT_FILTER, OPT_SCRIPT,
    OPT_ROM, OPT_BGCOLOR, OPT_PHI2, OPT_CP, OPT_SEED, OPT_FILL,
    OPT_MUTE, OPT_DEBUG, OPT_DAP, OPT_CREDITS, OPT_VERSION, OPT_INI,
    OPT_VSYNC, OPT_NO_VSYNC,
};
static const struct option longopts[] = {
    {"help",         no_argument,       NULL, OPT_HELP},
    {"screenshot",   required_argument, NULL, OPT_SCREENSHOT},
    {"frames",       required_argument, NULL, OPT_FRAMES},
    {"scale",        required_argument, NULL, OPT_SCALE},
    {"vsync",        no_argument,       NULL, OPT_VSYNC},
    {"no-vsync",     no_argument,       NULL, OPT_NO_VSYNC},
    {"filter",       required_argument, NULL, OPT_FILTER},
    {"script",       required_argument, NULL, OPT_SCRIPT},
    {"rom",          required_argument, NULL, OPT_ROM},
    {"bgcolor",      required_argument, NULL, OPT_BGCOLOR},
    {"phi2",         required_argument, NULL, OPT_PHI2},
    {"cp",           required_argument, NULL, OPT_CP},
    {"seed",         required_argument, NULL, OPT_SEED},
    {"fill",         required_argument, NULL, OPT_FILL},
    {"mute",         no_argument,       NULL, OPT_MUTE},
    {"debug",        no_argument,       NULL, OPT_DEBUG},
    {"dap",          no_argument,       NULL, OPT_DAP},
    {"credits",      no_argument,       NULL, OPT_CREDITS},
    {"version",      no_argument,       NULL, OPT_VERSION},
    {"ini",          required_argument, NULL, OPT_INI},
    {NULL, 0, NULL, 0},
};

void cli_usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "usage: %s [rom.rp6502] [options] [-- <args...>]\n"
            "  --help                    print this and exit\n"
            "  --screenshot <file.png>   render headlessly to PNG and exit\n"
            "  --frames <n>              frames to run before screenshot (default 120)\n"
            "  --scale <n>               window scale, fractional ok (default 1.5)\n"
            "  --vsync                   sync presentation to the display (default)\n"
            "  --no-vsync                present uncapped instead of syncing to the display\n"
            "  --filter <f>              nearest|linear|sharp (default sharp)\n"
            "  --script <file>           drive input and check results ('-' = stdin);\n"
            "                            always headless: the script is the only clock\n"
            "  --rom <file>              install a .rp6502 on the null drive, reached\n"
            "                            as :basename; repeatable, the first one boots\n"
            "  --bgcolor RRGGBB          letterbox/pillarbox fill color (default 000000)\n"
            "  --phi2 <khz>              6502 clock in kHz (100-8000, default 8000)\n"
            "  --cp <n>                  OEM code page (437/720/737/771/775/850/852/855/\n"
            "                            857/860-866/869, default 437)\n"
            "  --seed <n>                fixed RNG seed for reproducible runs\n"
            "                            (default: host entropy)\n"
            "  --fill random|<byte>      what RAM and XRAM hold at boot, as $FF or 255\n"
            "                            (default: random, like the hardware's)\n"
            "  --mute                    mute all audio (no synth, no OS audio device)\n"
            "  --debug                   on-screen machine debugger (CPU/VIA/disasm); holds\n"
            "                            the window open on stop for inspection; no window\n"
            "                            with --script\n"
            "  --dap                     act as a DAP debug adapter on stdio (implies --debug)\n"
            "  --credits                 print third-party credits/licenses and exit\n"
            "  --version                 print the version and exit\n"
            "  --ini <file>              config file for the debugger UI layout\n"
            "                            (ImGui format; e.g. the workspace .rp6502)\n"
            "  -- <args...>              pass the remaining words to the ROM as argv[1..]\n",
            argv0);
}


/* Reset getopt's global state so the parser starts clean each call. glibc/musl
 * re-init when optind is set to 0; the BSD-family getopt (and Windows/wingetopt,
 * macOS) needs optreset. */
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
        case OPT_HELP: o->help = true; break;
        case OPT_SCREENSHOT: o->screenshot = optarg; break;
        case OPT_FRAMES:
        {
            long long v;
            if (!cli_number(optarg, &v) || v < 1 || v > INT_MAX)
                return cli_bad("--frames", optarg), 2;
            o->frames = (int)v;
            o->have_frames = true;
            break;
        }
        case OPT_SCALE:
        {
            char *end;
            o->scale = strtod(optarg, &end);
            if (end == optarg || *end || !(o->scale > 0))
                return cli_bad("--scale", optarg), 2;
            o->have_scale = true;
            break;
        }
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
        case OPT_SCRIPT: o->script = optarg; break;
        case OPT_ROM:
        {
            int max = (int)(sizeof(o->installs) / sizeof(o->installs[0]));
            /* The one that vanished may be the one meant to boot. */
            if (o->n_installs == max)
            {
                fprintf(stderr, "rp6502-emu: too many --rom (max %d)\n", max);
                return 2;
            }
            o->installs[o->n_installs++] = optarg;
            break;
        }
        case OPT_BGCOLOR:
            if (!parse_hex_color(optarg, &o->bg_r, &o->bg_g, &o->bg_b))
            {
                fprintf(stderr, "rp6502-emu: bad --bgcolor (want RRGGBB)\n");
                return 2;
            }
            o->have_bg = true;
            break;
        case OPT_PHI2:
        {
            long long v;
            if (!cli_number(optarg, &v) || v < 1 || v > INT_MAX)
                return cli_bad("--phi2", optarg), 2;
            o->phi2_khz = (int)v; /* the range itself is cpu.c's to judge */
            break;
        }
        case OPT_CP:
        {
            long long v;
            if (!cli_number(optarg, &v) || v < 1 || v > INT_MAX)
                return cli_bad("--cp", optarg), 2;
            o->code_page = (int)v;
            break;
        }
        case OPT_SEED:
        {
            long long v;
            if (!cli_number(optarg, &v) || v < 0)
                return cli_bad("--seed", optarg), 2;
            o->seed = (unsigned long long)v;
            o->have_seed = true;
            break;
        }
        case OPT_FILL:
            if (!parse_fill(optarg, &o->fill_random, &o->fill_value))
            {
                fprintf(stderr, "rp6502-emu: bad --fill '%s' "
                                "(want random or a byte 0-255)\n", optarg);
                return 2;
            }
            break;
        case OPT_MUTE: o->mute = true; break;
        case OPT_DEBUG: o->debug = true; break;
        case OPT_DAP: o->dap = true; break;
        case OPT_CREDITS: o->credits = true; break;
        case OPT_VERSION: o->version = true; break;
        case OPT_INI: o->ini = optarg; break;
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
