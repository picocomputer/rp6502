/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_PLAT_H_
#define _EMU_PLAT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "api/std.h"
#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T fs_ssize_t;
#else
#include <sys/types.h>
typedef ssize_t fs_ssize_t;
#endif

#ifdef __cplusplus
extern "C"
{
#endif

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
FILE *fs_fopen_rd(const char *path); /* guest-encoding; read-only binary stream */
int fs_open(const char *path, int flags, int mode);
int fs_close(int fd); /* reaps a still-in-flight fs_read/fs_write on this fd first */
int64_t fs_lseek(int fd, int64_t off, int whence);
int fs_ftruncate(int fd, int64_t length);
std_rw_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got);
std_rw_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put);
void fs_sync(void);

/* ---- other host-OS primitives (host/posix/os.c or host/win/os.c, one compiled) ---- */
uint64_t os_entropy_64(void);            /* seed material from the host RNG/clocks */
uint64_t os_mono_ns(void);               /* monotonic clock, nanoseconds */
void os_sleep_until_ns(uint64_t target); /* frame pacer; no-op where the present already paces */

/* Broken-down host time (local zone / UTC). */
void os_localtime(time_t t, struct tm *out);
void os_gmtime(time_t t, struct tm *out);

/* Host-locale strftime (the C locale stays elsewhere in the process). */
void os_locale_reset(void); /* (re)load the environment locale */
size_t os_strftime_local(char *buf, size_t max, const char *fmt, const struct tm *tm);
void os_tm_apply_zone(struct tm *tm, const struct tm *probe); /* copy tm_gmtoff/tm_zone where they exist */

/* App config location, in the host's native path spelling. */
bool os_config_dir(char *buf, size_t sz);        /* e.g. <APPDATA>/rp6502-emu or <XDG/HOME>/.../rp6502-emu */
void os_ensure_parent_dir(const char *filepath); /* mkdir -p the directory that will hold filepath */

/* One command-line argument, host argv encoding -> guest OEM. False if it
 * does not fit. */
bool os_argv_to_oem(const char *arg, char *dst, size_t dstsz);

/* Test-only host helpers (the tests drive the rest of the seam directly). */
bool os_make_tmpdir(char *buf, size_t sz);           /* a fresh empty temp dir, '/'-separated */
void os_setenv(const char *name, const char *value); /* setenv(name, value, 1) in the host spelling */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_PLAT_H_ */
