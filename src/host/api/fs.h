/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

/* Files, as a host OS keeps them.
 *
 * What is left of the middle layer: the metadata and namespace calls
 * core/api/drive.c still stands on. Each of them dies with drive.c, when
 * every host answers dir_backend_t in its own vocabulary instead of being
 * translated up into this one.
 *
 * The files themselves left already -- a host implements core/api/fs.h's
 * driver directly.
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

/* The ROM loader's stream, and nothing else: a whole-file read of a .rp6502
 * for the record parser. Not a drive operation -- moving to host/os.h with
 * the rest of what a machine owes core directly. */
FILE *host_fs_fopen_rd(const char *path);

#endif /* _HOST_API_FS_H_ */
