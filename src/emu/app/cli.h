/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_CLI_H_
#define _EMU_APP_CLI_H_

#include <stddef.h>

#include "emu/host/window.h" /* window_scale_filter_t */

#ifdef __cplusplus
extern "C"
{
#endif

/* Every option, as parsed from the command line; defaults pre-filled. */
typedef struct
{
    const char *rom, *shot, *input;
    bool tmpdrive;
    const char *installs[16];
    int n_installs;
    int bg_r, bg_g, bg_b;
    bool have_bg;
    int frames;
    double scale;
    bool have_scale;
    bool vsync; /* --no-vsync turns it off (default on) */
    window_scale_filter_t scale_filter;
    int phi2_khz;  /* 0 = leave at default */
    int code_page; /* 0 = leave at the default 437 */
    bool mute;
    bool debug;   /* --debug: on-screen machine debugger */
    bool dap;     /* --dap: also serve DAP on stdio (implies --debug) */
    bool credits;       /* --credits: print third-party notices and exit */
    const char *inidir; /* --ini: config file for the debugger UI layout (else default) */
    unsigned long long seed;
    bool have_seed;
    char **rom_args; /* words after "--", argv[1..] for the booted ROM (NULL = none given) */
    int n_rom_args;
} cli_options;

void cli_options_init(cli_options *o);

/* Parse argv (argv[0] is the program name, per the getopt convention) into o,
 * assigning only the options present so a later pass overrides an earlier one.
 * Returns 0, or 2 on a bad/unknown option (a message is printed; the caller
 * decides whether it is fatal). getopt_long accepts both "--opt value" and
 * "--opt=value", and permutes the lone positional (the ROM) to the tail.
 * Everything after a standalone "--" lands in rom_args (the booted ROM's
 * argv[1..]), never parsed as options. */
int cli_parse_args(int argc, char **argv, cli_options *o);

/* Print the option summary to stderr (argv0 names the program). */
void cli_usage(const char *argv0);

/* The path component after the last '/'. */
const char *cli_base_name(const char *p);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_APP_CLI_H_ */
