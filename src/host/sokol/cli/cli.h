/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_SOKOL_CLI_CLI_H_
#define _HOST_SOKOL_CLI_CLI_H_

#include <stddef.h>
#include <stdint.h>

#include "host/sokol/app/gfx.h"
#include <stdio.h>

typedef struct
{
    const char *rom, *screenshot, *script;
    bool help;
    const char *installs[16];
    int n_installs;
    int bg_r, bg_g, bg_b;
    bool have_bg;
    int frames;
    bool have_frames;
    double scale;
    bool have_scale;
    gfx_filter_t scale_filter;
    int phi2_khz;  /* 0 leaves the machine's default */
    int code_page; /* 0 leaves the machine's default */
    bool crc;
    bool headless;
    bool console; /* --stdin */
    bool unpaced; /* --phi2 0 */
    bool mute;
    bool debug;
    bool dap;
    bool credits;
    bool version;
    const char *ini;
    unsigned long long seed;
    bool have_seed;
    bool fill_random;
    uint8_t fill_value;
    char **rom_args; /* the words after "--"; NULL when none was given */
    int n_rom_args;
} cli_options;

void cli_options_init(cli_options *o);

/* Parse argv (argv[0] is the program name) into o, assigning only the options
 * that are present. Returns 0, or 2 after printing a message about a bad or
 * unknown option. Everything after a standalone "--" lands in rom_args rather
 * than being parsed. */
int cli_parse_args(int argc, char **argv, cli_options *o);

/* Print the options to out. The script verbs are script_usage()'s, because
 * cli.c is also linked without script.c; --help prints both. */
void cli_usage(FILE *out, const char *argv0);

#endif /* _HOST_SOKOL_CLI_CLI_H_ */
