/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Command-line parsing (cli.c): the option table + getopt_long parser, shared by
 * main.c's bootstrap and its ROM "emulator" asset merge. parse_args fills an
 * options struct, assigning only the options present so a later pass overrides an
 * earlier one; tokenize_args splits the ROM asset string into argv tokens.
 */

#ifndef _EMU_CLI_H_
#define _EMU_CLI_H_

#include <stddef.h>

#include "emu/app/window.h" /* emu_scale_filter_t */

#ifdef __cplusplus
extern "C"
{
#endif

/* Every option, as parsed; defaults pre-filled. Two passes fill one of these:
 * the ROM's "emulator" asset first, then the real command line (last wins). */
typedef struct
{
    const char *rom, *shot, *input, *fsdir;
    bool tmpdrive;
    const char *installs[16];
    int n_installs;
    int bg_r, bg_g, bg_b;
    bool have_bg;
    int frames;
    double scale;
    emu_scale_filter_t scale_filter;
    int phi2_khz;  /* 0 = leave at default */
    int code_page; /* 0 = leave at the default 437 */
    bool no_audio;
    bool debug;   /* --debug: on-screen machine debugger (dbg engine active) */
    bool dap;     /* --dap: also serve DAP on stdio (implies --debug) */
    bool credits;       /* --credits: print third-party notices and exit */
    const char *inidir; /* --ini: config file for the debugger UI layout (else default) */
    unsigned long long seed;
    bool have_seed;
} options;

void options_init(options *o);

/* Parse argv (argv[0] is the program name, per the getopt convention) into o,
 * assigning only the options present so a later pass overrides an earlier one.
 * Returns 0, or 2 on a bad/unknown option (a message is printed; the caller
 * decides whether it is fatal). getopt_long accepts both "--opt value" and
 * "--opt=value", and permutes the lone positional (the ROM) to the tail. */
int parse_args(int argc, char **argv, options *o);

/* Split a command string into argv tokens, honoring "..."/'...' quoting (a quote
 * groups; unquoted whitespace separates). Token text is written into store[];
 * argv[] is filled with pointers into it. Returns the token count. */
int tokenize_args(const char *s, char **argv, int max, char *store, size_t cap);

/* The path component after the last '/'. */
const char *base_name(const char *p);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_CLI_H_ */
