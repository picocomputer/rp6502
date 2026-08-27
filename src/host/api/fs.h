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

#ifndef _HOST_API_FS_H_
#define _HOST_API_FS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* A buffer for a host path. This is the OS's limit and not the API's: a
 * host cwd, a realpath, or a path a frontend hands over arrives at whatever
 * length the OS allows, and is only then measured against what a 6502 can
 * hold. Paths that came from the 6502 use core/api/fs.h's FS_MAX_PATH. */
#define HOST_MAX_PATH 4096

/* ---- file metadata (richer than struct stat so each OS fills it faithfully) ---- */
struct host_fs_meta
{
    bool is_dir;
    bool is_readonly; /* POSIX: !(st_mode & S_IWUSR);  Win: FILE_ATTRIBUTE_READONLY */
    bool is_hidden;   /* POSIX: basename[0]=='.';      Win: FILE_ATTRIBUTE_HIDDEN   */
    uint64_t size;
    time_t mtime;
    time_t crtime; /* POSIX: st_ctime (change time);  Win: real creation time       */
};
bool host_fs_stat(const char *path, struct host_fs_meta *out);
bool host_fs_freespace(const char *path, uint64_t *total_bytes, uint64_t *avail_bytes);

/* ---- attribute / time mutators ---- */
bool host_fs_set_readonly(const char *path, bool readonly);
bool host_fs_set_mtime(const char *path, time_t mtime); /* sets last-modified only */

/* ---- namespace mutators / queries ---- */
bool host_fs_mkdir(const char *path);
bool host_fs_chdir(const char *path);
bool host_fs_getcwd(char *buf, size_t sz); /* guest-encoding, '/'-separated */
bool host_fs_realpath(const char *path, char *out, size_t outsz); /* absolute, '/'-separated */
bool host_fs_rename(const char *oldp, const char *newp); /* replaces an existing target */
bool host_fs_remove(const char *path);     /* a file or an empty directory */

/* ---- byte I/O ---- */
/* A transfer's outcome. The guest's stdio dispatcher has the same three states
 * and its own spelling of them; the host answers in its own so that a contract
 * about files does not depend on a guest API to say "that worked". */
typedef enum
{
    HOST_IO_OK,      /* completed, success */
    HOST_IO_ERROR,   /* failed, check errno */
    HOST_IO_PENDING, /* incomplete, would block */
} host_io_result;

/* Open flags are the rp6502 SDK's own bits, not POSIX's. A firmware whose
 * filesystem is FatFs has no fcntl.h to borrow O_* from, and the low two bits
 * already mirror FA_READ/FA_WRITE -- so the host that has POSIX is the one
 * that translates. APPEND is not here: it is a seek to the end after the
 * open, which the layer above does once for everyone. */
#define HOST_FS_RD 0x01
#define HOST_FS_WR 0x02
#define HOST_FS_CREAT 0x10
#define HOST_FS_TRUNC 0x20
#define HOST_FS_EXCL 0x80

FILE *host_fs_fopen_rd(const char *path); /* guest-encoding; read-only binary stream */
int host_fs_open(const char *path, uint8_t flags);
int host_fs_close(int fd); /* settles a still-in-flight host_fs_read/host_fs_write on this fd first */
host_io_result host_fs_read(int fd, char *buf, uint32_t count, uint32_t *got);
host_io_result host_fs_write(int fd, const char *buf, uint32_t count, uint32_t *put);

int64_t host_fs_size(int fd); /* the file's length, or -1 + errno */
int64_t host_fs_tell(int fd); /* where the pointer is, or -1 + errno */

/* Move the pointer to an absolute position, answering where it landed (or -1
 * + errno). This is f_lseek's contract, because it is the narrower of the two:
 * a writable file is extended to pos, a read-only one stops at its end, and a
 * volume with no room left stops wherever it ran out. A host with POSIX can
 * say all of that; FatFs cannot say anything wider. */
int64_t host_fs_seek(int fd, uint64_t pos);

bool host_fs_fsync(int fd); /* this file, to the medium */
void host_fs_persist(void); /* the drive, to wherever it really lives */

#endif /* _HOST_API_FS_H_ */
