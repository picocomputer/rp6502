/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _OSAL_OS_H_
#define _OSAL_OS_H_

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Fresh entropy, different every call. The seed a run repeats under is
 * host_seed's, which draws from here once. */
uint32_t os_random(void);

/* Broken-down host time (local zone / UTC). False when t is out of the host's range. */
bool os_localtime(time_t t, struct tm *out);
bool os_gmtime(time_t t, struct tm *out);

/* Host-locale strftime (the C locale stays elsewhere in the process). */
void os_locale_reset(void); /* (re)load the environment locale */
size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm);
void os_tm_apply_zone(struct tm *tm, const struct tm *probe); /* copy tm_gmtoff/tm_zone where they exist */

/* One command-line argument, this process's argv encoding -> guest OEM. False
 * if it does not fit. Only a program with an argv asks: a libretro frontend
 * hands its paths over as UTF-8 and converts them with core's oem_from_utf8. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz);

/* ---- what only a host with a window, a console or a config dir answers ---- */
/* Declared here with the rest of the contract, so a seam that implements them
 * needs no header from the application that calls them. A host that is none of
 * those things leaves them unimplemented and nothing links against them.
 *
 * Real time, for pacing a machine against the display -- not host_clock_us
 * above, which a window deliberately lets drift when the host falls behind. */
uint64_t os_mono_ns(void);

/* Wait this long without running. A host that nothing else paces waits here
 * for the machine's next frame. */
void os_sleep_ns(uint64_t ns);

/* Reattach stdio to the parent console when launched from one; no-op where it
 * already does. Only Windows needs it, where the emulator is a GUI-subsystem
 * .exe and --help would otherwise vanish. */
void os_console_attach(void);

/* Where an application's config file goes, in the host's native path spelling
 * -- the machine has no config directory, an application does. This is also
 * where the literal "rp6502-emu" lives. ensure_parent_dir works in host path
 * encoding, not the guest OEM the drive speaks, so it is not the drive. */
char *os_config_dir(void);                       /* e.g. <APPDATA>/rp6502-emu or <XDG/HOME>/.../rp6502-emu; allocated, caller frees */
void os_ensure_parent_dir(const char *filepath); /* mkdir -p the directory that will hold filepath */

#endif /* _OSAL_OS_H_ */
