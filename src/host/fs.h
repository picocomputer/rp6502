/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* Files, as a host OS keeps them.
 *
 * The software hosts' seam: the emulator's drives are a real filesystem
 * underneath, and each OS answers these its own way -- host/posix/fs.c for the
 * POSIX family (with its byte transport beside it, fs_aio.c or fs_sync.c),
 * host/windows/fs.c, host/emsdk/fs.c. A Pico has its own storage
 * and a Pocket has the card, so neither implements any of it and neither
 * compiles a caller.
 */

#ifndef _HOST_FS_H_
#define _HOST_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

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
int fs_close(int fd); /* settles a still-in-flight fs_read/fs_write on this fd first */
int64_t fs_lseek(int fd, int64_t off, int whence);
int fs_ftruncate(int fd, int64_t length);
fs_io_result fs_read(int fd, char *buf, uint32_t count, uint32_t *got);
fs_io_result fs_write(int fd, const char *buf, uint32_t count, uint32_t *put);
void fs_sync(void);

#endif /* _HOST_FS_H_ */
