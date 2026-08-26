/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* What a host may implement for the machine. Every host has a host.h of its
 * own that includes this one, so #include "host.h" from anywhere reaches the
 * host this build is for -- the host's directory is first on the include path.
 *
 * What every machine answers: the clock it runs on, the stream its rand()
 * reads, and the host OS calls the machine's own code makes. Files are not
 * here -- a Pico has its own storage and a Pocket has the card, so the
 * filesystem is the software hosts' seam and lives in host/fs.h. */

#ifndef _HOST_OS_H_
#define _HOST_OS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---- where a host puts things ---- */
/* A Pico has two memories and cares which one a byte lands in: cold paths
 * and tables belong in flash, an interrupt handler must not. A machine with
 * one memory ignores all of it, which is why these default to nothing and a
 * host that means something by them says so before including this. */
#ifndef HOST_IN_FLASH
#define HOST_IN_FLASH(group)
#endif
#ifndef HOST_NOT_IN_FLASH
#define HOST_NOT_IN_FLASH(group)
#endif
#ifndef HOST_UNINITIALIZED_RAM
#define HOST_UNINITIALIZED_RAM(name) name
#endif
#ifndef HOST_TIME_CRITICAL
#define HOST_TIME_CRITICAL(name) name
#endif
#ifndef HOST_ISR
#define HOST_ISR
#endif

/* ---- what a host has that others do not ---- */
/* Hardware the machine uses when it is there and stands in for when it is
 * not. A host that has one says so before including this; undefined is the
 * answer for everyone else. HOST_INTERP is the RP2350's SIO interpolators,
 * which mode4 walks its affine texture with. */
#ifndef HOST_INTERP
#define HOST_INTERP 0
#endif

/* The tallest terminal this machine's video can show, in rows of cells.
 * Only a device with an SXGA console reaches 32; every other target tops
 * out at 480 scanlines, where two more rows would be bought and never
 * shown -- and this sizes the largest thing term.c owns. */
#ifndef HOST_TERM_MAX_HEIGHT
#define HOST_TERM_MAX_HEIGHT 30
#endif

/* How long a path this machine keeps for the launcher chain. A host OS path
 * can be long; a Pico holds what its monitor accepts, and a soft CPU counting
 * static RAM holds less. Each machine that is not a host OS says so before
 * including this. */
#ifndef PROC_PATH_MAX
#define PROC_PATH_MAX 4096
#endif

/* Each console ring, in bytes; a power of two. Two of these exist, one for
 * what was typed and one for what the terminal answered, and neither is a
 * buffer anything waits on: a paste drips in behind com_keyboard_free and replies
 * arrive in bounded bursts. */
#ifndef COM_RING_SIZE
#define COM_RING_SIZE 256
#endif

/* ---- the machine's microsecond clock ---- */
/* Microseconds since the machine started: TIMER0 on a Pico, the run loop's own
 * counter in the emulator, the fabric's mtime on a Pocket. Machine time, not
 * the host's -- it stands still while the machine is halted, and it is
 * savestate state where a machine has savestates. Wall time is tim_get_time. */
uint64_t host_clock_us(void);

/* Deadlines, from the clock above. Inline rather than a translation unit on
 * five build lists for three adds. */
typedef uint64_t host_deadline_t;
static inline host_deadline_t host_deadline_us(uint64_t us) { return host_clock_us() + us; }
static inline host_deadline_t host_deadline_ms(uint64_t ms) { return host_clock_us() + ms * 1000; }
static inline bool host_deadline_passed(host_deadline_t d) { return host_clock_us() >= d; }

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
 * Real time, for pacing a frame loop against the display -- not host_clock_us
 * above, which a window deliberately lets drift when the host falls behind. The
 * sleep is a no-op where the present already paces. */
uint64_t host_mono_ns(void);
void host_sleep_until_ns(uint64_t target);

/* Reattach stdio to the parent console when launched from one; no-op where it
 * already does. Only Windows needs it, where the emulator is a GUI-subsystem
 * .exe and --help would otherwise vanish. */
void host_console_attach(void);

/* Where an application's config file goes, in the host's native path spelling
 * -- the machine has no config directory, an application does. This is also
 * where the literal "rp6502-emu" lives. ensure_parent_dir works in host path
 * encoding, not the guest OEM the fs_* seam speaks, so it is not fs_mkdir. */
bool host_config_dir(char *buf, size_t sz);        /* e.g. <APPDATA>/rp6502-emu or <XDG/HOME>/.../rp6502-emu */
void host_ensure_parent_dir(const char *filepath); /* mkdir -p the directory that will hold filepath */

#endif /* _HOST_OS_H_ */
