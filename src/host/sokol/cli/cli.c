/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/cli.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include "getopt.h" /* vendored wingetopt; MSVC has no getopt */
#else
#include <getopt.h>
#endif

void cli_options_init(cli_options *o)
{
    memset(o, 0, sizeof *o);
    o->frames = 120;
    o->scale = 1.5;
    o->scale_filter = GFX_FILTER_SHARP;
}

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

/* A command-line number: $FF and 0xFF are hex and everything else is
 * decimal, because strtol's base 0 would read "010" as eight, which is not
 * what someone typing a byte count means. */
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


/* Long-option codes, above 255 so they cannot collide with a short-option
 * character. */
enum
{
    OPT_HELP = 256, OPT_SCREENSHOT, OPT_FRAMES, OPT_SCALE, OPT_FILTER, OPT_SCRIPT,
    OPT_INSTALL, OPT_SAVE_DIR, OPT_BGCOLOR, OPT_PHI2, OPT_CP, OPT_MEMSEED, OPT_MEMFILL,
    OPT_MUTE, OPT_DEBUG, OPT_DAP, OPT_CREDITS, OPT_VERSION, OPT_INI,
    OPT_CRC, OPT_HEADLESS, OPT_STDIN, OPT_ASPECT,
};
static const struct option longopts[] = {
    {"help",         no_argument,       NULL, OPT_HELP},
    {"screenshot",   required_argument, NULL, OPT_SCREENSHOT},
    {"crc",          no_argument,       NULL, OPT_CRC},
    {"frames",       required_argument, NULL, OPT_FRAMES},
    {"scale",        required_argument, NULL, OPT_SCALE},
    {"filter",       required_argument, NULL, OPT_FILTER},
    {"aspect",       required_argument, NULL, OPT_ASPECT},
    {"script",       required_argument, NULL, OPT_SCRIPT},
    {"headless",     no_argument,       NULL, OPT_HEADLESS},
    {"stdin",        no_argument,       NULL, OPT_STDIN},
    {"install",      required_argument, NULL, OPT_INSTALL},
    {"save-dir",     required_argument, NULL, OPT_SAVE_DIR},
    {"bgcolor",      required_argument, NULL, OPT_BGCOLOR},
    {"phi2",         required_argument, NULL, OPT_PHI2},
    {"cp",           required_argument, NULL, OPT_CP},
    {"memseed",      required_argument, NULL, OPT_MEMSEED},
    {"memfill",      required_argument, NULL, OPT_MEMFILL},
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
            "  --crc                     render headlessly, print the canvas CRC-32 and exit\n"
            "  --frames <n>              frames to run before the screenshot or crc (default 120)\n"
            "  --scale <n>               window scale, fractional ok (default 1.5)\n"
            "  --filter <f>              nearest|linear|sharp (default sharp)\n"
            "  --aspect <a>              4:3|16:9|16:10: a 640x480, 640x360 or 640x400\n"
            "                            console, and a window that keeps its shape\n"
            "                            (default: the window takes each canvas's shape)\n"
            "  --script <file>           drive input and check results ('-' = stdin);\n"
            "                            always headless: the script is the only clock\n"
            "  --headless                no window and no picture: host stdin, stdout and\n"
            "                            stderr are the program's; exits with its exit code\n"
            "  --stdin                   host stdin is the machine's console, and a terminal\n"
            "                            there is the console: keys raw, screen drawn on it,\n"
            "                            Ctrl-\\ the way out. Implied by --headless\n"
            "  --install <file>          install a file on the null drive, reached as\n"
            "                            :basename, which a program runs or reads;\n"
            "                            repeatable, and the first one boots when no\n"
            "                            ROM is named\n"
            "  --save-dir <folder>       the folder behind SAVE:, made by the first save\n"
            "                            (default: this OS's folder for saved data)\n"
            "  --bgcolor RRGGBB          letterbox/pillarbox fill color, # optional\n"
            "                            (default 000000)\n"
            "  --phi2 <khz>              6502 clock in kHz (100-8000, default 8000);\n"
            "                            0 runs unpaced, warping time\n"
            "  --cp <n>                  OEM code page (437/720/737/771/775/850/852/855/\n"
            "                            857/860-866/869, default 437)\n"
            "  --memseed <n>             seed of the random RAM and XRAM fill,\n"
            "                            0-4294967295 (default: host entropy)\n"
            "  --memfill <byte>          fill RAM and XRAM with one byte, as $FF or 255,\n"
            "                            in place of the random fill of --memseed\n"
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


/* glibc and musl re-initialize getopt when optind is set to 0. The BSD-family
 * getopt, which macOS, the BSDs and the vendored wingetopt all use, needs
 * optreset instead. */
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
    /* The tail after a standalone "--" is the ROM's argv[1..]. getopt_long
     * permutes argv in place, so argc is truncated at the separator to keep
     * that tail where rom_args points. A literal "--" as an option value has
     * to be written in the --opt=-- form. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--"))
        {
            o->rom_args = &argv[i + 1];
            o->n_rom_args = argc - i - 1;
            argc = i;
            break;
        }
    getopt_reset();
    opterr = 0;
    int c;
    /* The leading ':' in the optstring reports a missing value as ':', not '?' */
    while ((c = getopt_long(argc, argv, ":", longopts, NULL)) != -1)
    {
        switch (c)
        {
        case OPT_HELP: o->help = true; break;
        case OPT_SCREENSHOT: o->screenshot = optarg; break;
        case OPT_CRC: o->crc = true; break;
        case OPT_HEADLESS: o->headless = true; break;
        case OPT_STDIN: o->console = true; break;
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
        case OPT_FILTER:
            if (!strcmp(optarg, "nearest"))
                o->scale_filter = GFX_FILTER_NEAREST;
            else if (!strcmp(optarg, "linear"))
                o->scale_filter = GFX_FILTER_LINEAR;
            else if (!strcmp(optarg, "sharp"))
                o->scale_filter = GFX_FILTER_SHARP;
            else
            {
                fprintf(stderr, "rp6502-emu: bad --filter '%s' "
                                "(want nearest|linear|sharp)\n", optarg);
                return 2;
            }
            break;
        case OPT_ASPECT:
            /* The console canvas takes the shape asked for at 640 wide, which
             * holds 30, 22 or 25 rows of 80 columns, and the window keeps
             * that shape. */
            if (!strcmp(optarg, "4:3"))
                o->console_h = 480;
            else if (!strcmp(optarg, "16:9"))
                o->console_h = 360;
            else if (!strcmp(optarg, "16:10"))
                o->console_h = 400;
            else
            {
                fprintf(stderr, "rp6502-emu: bad --aspect '%s' "
                                "(want 4:3|16:9|16:10)\n", optarg);
                return 2;
            }
            o->console_w = 640;
            o->fixed_aspect = true;
            break;
        case OPT_SCRIPT: o->script = optarg; break;
        case OPT_INSTALL:
        {
            int max = (int)(sizeof(o->installs) / sizeof(o->installs[0]));
            if (o->n_installs == max)
            {
                fprintf(stderr, "rp6502-emu: too many --install (max %d)\n", max);
                return 2;
            }
            o->installs[o->n_installs++] = optarg;
            break;
        }
        case OPT_SAVE_DIR: o->save_dir = optarg; break;
        case OPT_BGCOLOR:
            if (!parse_hex_color(optarg, &o->bg_r, &o->bg_g, &o->bg_b))
            {
                fprintf(stderr, "rp6502-emu: bad --bgcolor (want RRGGBB or #RRGGBB)\n");
                return 2;
            }
            o->have_bg = true;
            break;
        case OPT_PHI2:
        {
            long long v;
            if (!cli_number(optarg, &v) || v < 0 || v > INT_MAX)
                return cli_bad("--phi2", optarg), 2;
            o->unpaced = v == 0;
            if (v)
                o->phi2_khz = (int)v; /* the caller range-checks */
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
        case OPT_MEMSEED:
        {
            long long v;
            if (!cli_number(optarg, &v) || v < 0 || v > UINT32_MAX)
                return cli_bad("--memseed", optarg), 2;
            o->memseed = (uint32_t)v;
            o->have_memseed = true;
            break;
        }
        case OPT_MEMFILL:
        {
            long long v;
            if (!cli_number(optarg, &v) || v < 0 || v > 255)
                return cli_bad("--memfill", optarg), 2;
            o->memfill = (uint8_t)v;
            o->have_memfill = true;
            break;
        }
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
    if (o->have_memseed && o->have_memfill)
    {
        fprintf(stderr, "rp6502-emu: --memseed and --memfill cannot both be given\n");
        return 2;
    }
    if (o->headless)
        o->console = true;
    /* The first non-empty positional is the ROM path. Empty words are skipped
     * because a launcher can pass an unfilled input, such as a launch.json
     * variable nobody answered, as an empty argument. */
    for (int i = optind; i < argc; i++)
        if (argv[i][0])
        {
            o->rom = argv[i];
            break;
        }
    return 0;
}
