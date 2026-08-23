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
 * Not every host implements all of it. Every host answers the clock, but the
 * filesystem and OS calls below are the software hosts' seam; a Pico has its own
 * storage and a Pocket has the card, and neither defines these. A declaration
 * nobody calls costs nothing, and the alternative is a second contract header to
 * remember. */

#ifndef _CORE_HOST_H_
#define _CORE_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* ---- the machine's microsecond clock ---- */
/* Microseconds since the machine started. A Pico reads TIMER0, the emulator its
 * own run clock, a Pocket the fabric's mtime -- so this is machine time, not the
 * host's: it stands still while the machine is halted, and it is savestate state
 * where a machine has savestates. Wall time is tim_get_time. */
uint64_t host_clock_us(void);

/* Deadlines, from the clock above. No host implements these; they are the same
 * arithmetic everywhere, and inline because the alternative is a translation
 * unit on five build lists for three adds. */
typedef uint64_t host_deadline_t;
static inline host_deadline_t host_deadline_us(uint64_t us) { return host_clock_us() + us; }
static inline host_deadline_t host_deadline_ms(uint64_t ms) { return host_clock_us() + ms * 1000; }
static inline bool host_deadline_passed(host_deadline_t d) { return host_clock_us() >= d; }

/* ---- directory enumeration ---- */
void *dir_open(const char *path); /* opaque stream, or NULL + errno */
/* 1 = an entry (name + is_dir filled), 0 = end of directory, -1 = error (errno). */
int dir_read(void *d, char *name, size_t namesz, bool *is_dir);
void dir_rewind(void *d);
void dir_close(void *d);

/* ---- file metadata (richer than struct stat so each OS fills it faithfully) ---- */
struct fs_meta
{
    bool is_dir;
    bool is_readonly; /* POSIX: !(st_mode & S_IWUSR);  Win: FILE_ATTRIBUTE_READONLY */
    bool is_hidden;   /* POSIX: basename[0]=='.';      Win: FILE_ATTRIBUTE_HIDDEN   */
    uint64_t size;
    time_t mtime;
    time_t crtime; /* POSIX: st_ctime (change time);  Win: real creation time       */
};
bool fs_stat(const char *path, struct fs_meta *out);
bool fs_freespace(const char *path, uint64_t *total_bytes, uint64_t *avail_bytes);

/* ---- attribute / time mutators ---- */
bool fs_set_readonly(const char *path, bool readonly);
bool fs_set_mtime(const char *path, time_t mtime); /* sets last-modified only */

/* ---- namespace mutators / queries ---- */
bool fs_mkdir(const char *path);
bool fs_chdir(const char *path);
bool fs_getcwd(char *buf, size_t sz); /* guest-encoding, '/'-separated */
bool fs_realpath(const char *path, char *out, size_t outsz); /* absolute, '/'-separated */
bool fs_rename(const char *oldp, const char *newp); /* replaces an existing target */
bool fs_remove(const char *path);     /* a file or an empty directory */

/* ---- byte I/O (POSIX O_* flags; binary on Windows) ---- */
/* A transfer's outcome. The guest's stdio dispatcher has the same three states
 * and its own spelling of them; the host answers in its own so that a contract
 * about files does not depend on a guest API to say "that worked". */
typedef enum
{
    FS_IO_OK,      /* completed, success */
    FS_IO_ERROR,   /* failed, check errno */
    FS_IO_PENDING, /* incomplete, would block */
} fs_io_result;

FILE *fs_fopen_rd(const char *path); /* guest-encoding; read-only binary stream */
int fs_open(const char *path, int flags, int mode);
int fs_close(int fd); /* reaps a still-in-flight fs_read/fs_write on this fd first */
int64_t fs_lseek(int fd, int64_t off, int whence);
int fs_ftruncate(int fd, int64_t length);
fs_io_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got);
fs_io_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put);
void fs_sync(void);

/* The machine's random stream, which the 6502's rand() reads. A Pico has a
 * hardware RNG; the emulator and a Pocket run a generator of their own so a run
 * can be reproduced, which is why this is not host entropy -- that seeds the
 * emulator's generator and is declared by emu/app/rand.h, one layer up. */
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
