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
 * filesystem is the software hosts' seam and lives in core/fs.h. */

#ifndef _CORE_HOST_H_
#define _CORE_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

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
 * host entropy, which seeds that generator -- see core/emu/app/rand.h. */
uint64_t host_rand_64(void);

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

#endif /* _CORE_HOST_H_ */
