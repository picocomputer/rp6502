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
#include <time.h>
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

/* The host OS-primitive seam. api/hostfs.c is portable policy; the platform impl
 * (posix/ or win/, one compiled) fills these. Paths cross the seam in the guest's
 * encoding (OEM code page); posix uses the bytes verbatim, win converts OEM<->UTF-16
 * (emu/api/oem.h). Fallible calls set errno and return false/NULL/-1 so the
 * msc_errno_to_api_errno funnel is unchanged. */

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
int fs_open(const char *path, int flags, int mode);
int fs_close(int fd);
fs_ssize_t fs_read(int fd, void *buf, size_t n);
fs_ssize_t fs_write(int fd, const void *buf, size_t n);
int64_t fs_lseek(int fd, int64_t off, int whence);
int fs_ftruncate(int fd, int64_t length);
fs_ssize_t fs_pread(int fd, void *buf, size_t n, int64_t off);

/* ---- misc primitives ---- */
void fs_localtime(time_t t, struct tm *out);
void fs_gmtime(time_t t, struct tm *out);
int fs_strcasecmp(const char *a, const char *b);
int fs_strncasecmp(const char *a, const char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_PLAT_H_ */
