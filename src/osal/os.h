/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* What a host may implement for the machine. Every host has a host.h of its
 * own that includes this one, so #include "host/host.h"
#include "osal/os.h" from anywhere reaches the
 * host this build is for -- the host's directory is first on the include path.
 *
 * What every machine answers: the clock it runs on, the stream its rand()
 * reads, and the host OS calls the machine's own code makes. Files are not
 * here -- a Pico has its own storage and a Pocket has the card, so the
 * filesystem driver is osal/fs.h, which each host implements. */

#ifndef _OSAL_OS_H_
#define _OSAL_OS_H_

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* The stream the 6502's rand() reads. A Pico has a hardware RNG; the emulator
 * and a Pocket run a generator of their own so a run can be reproduced. Not
 * host entropy, which seeds that generator -- see the seed material below. */
uint64_t host_rand_64(void);

/* Seed material for that generator, from the host RNG or its clocks. Only a
 * machine that runs a generator of its own asks for this. */
uint64_t host_entropy_64(void);

/* Broken-down host time (local zone / UTC). False when t is out of the host's range. */
bool host_localtime(time_t t, struct tm *out);
bool host_gmtime(time_t t, struct tm *out);

/* Host-locale strftime (the C locale stays elsewhere in the process). */
void host_locale_reset(void); /* (re)load the environment locale */
size_t host_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm);
void host_tm_apply_zone(struct tm *tm, const struct tm *probe); /* copy tm_gmtoff/tm_zone where they exist */

/* One command-line argument, host argv encoding -> guest OEM. False if it
 * does not fit. */
bool host_argv_to_oem(const char *arg, char *dst, size_t dstsz);

/* ---- what only a host with a window, a console or a config dir answers ---- */
/* Declared here with the rest of the contract, so a seam that implements them
 * needs no header from the application that calls them. A host that is none of
 * those things leaves them unimplemented and nothing links against them.
 *
 * Real time, for pacing a machine against the display -- not host_clock_us
 * above, which a window deliberately lets drift when the host falls behind. The
 * sleep is a no-op where the present already paces. */
uint64_t host_mono_ns(void);
void host_sleep_until_ns(uint64_t target);

/* Reattach stdio to the parent console when launched from one; no-op where it
 * already does. Only Windows needs it, where the emulator is a GUI-subsystem
 * .exe and --help would otherwise vanish. */
void host_console_attach(void);

/* The two filesystem calls core makes directly rather than through a driver.
 * A path is spelled the way the 6502 spells it, both ways.
 *
 * realpath answers absolutely, which is what argv[0] needs to survive a chdir;
 * fopen_rd hands back a stream for the ROM loader's record parser, which reads
 * a whole file rather than serving a program.
 *
 * realpath allocates its answer, because how long a path the OS will hand back
 * is the OS's to decide and not a caller's to guess. The caller frees; NULL is
 * a path that does not resolve. */
char *host_fs_realpath(const char *path);
FILE *host_fs_fopen_rd(const char *path);

/* Where an application's config file goes, in the host's native path spelling
 * -- the machine has no config directory, an application does. This is also
 * where the literal "rp6502-emu" lives. ensure_parent_dir works in host path
 * encoding, not the guest OEM the drive speaks, so it is not the drive. */
char *host_config_dir(void);                       /* e.g. <APPDATA>/rp6502-emu or <XDG/HOME>/.../rp6502-emu; allocated, caller frees */
void host_ensure_parent_dir(const char *filepath); /* mkdir -p the directory that will hold filepath */

#endif /* _OSAL_OS_H_ */
