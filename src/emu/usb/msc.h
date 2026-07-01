/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's FatFs backend for MSC0: — a RAM block device (the diskio, called
 * by FatFs) plus the drive glue that lets --tmpdrive present a real, ephemeral
 * FatFs instead of the native host filesystem. The 6502's file/dir syscalls route
 * here (via std.c / host/dir.c) when the FatFs backend is active, running the
 * SHARED ria/api/fat.c driver over the RAM disk — the same code as the firmware.
 */

#ifndef _EMU_USB_MSC_H_
#define _EMU_USB_MSC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emu/api/api.h"  /* io_result */
#include "emu/host/dir.h" /* fs_info_t */

#ifdef __cplusplus
extern "C"
{
#endif

void emu_ramdisk_reset(void);   /* wipe the RAM disk to an unformatted state */
bool emu_ramdrive_mount(void);   /* --tmpdrive: format + mount a fresh RAM FatFs, make it the backend */
void emu_ramdrive_unmount(void); /* deactivate the FatFs backend (tests; the drive is session-lived otherwise) */
bool emu_fat_active(void);      /* true once the FatFs backend is the active MSC0: drive */

/* FatFs file driver, in the emu std.c driver-table shape (adapts the shared
 * fat_std_* to the emu's void* descriptor, io_result and POSIX errno). */
bool fat_std_handles_(const char *path);
void *fat_std_open_(const char *path, uint8_t flags);
void fat_std_close_(void *desc);
io_result fat_std_read_(void *desc, void *buf, size_t n, size_t *got);
io_result fat_std_write_(void *desc, const void *buf, size_t n, size_t *put);
void fat_std_sync_(void *desc);
long fat_std_lseek_(void *desc, long off, int whence);

/* FatFs directory / metadata ops, in the host/dir.c fs_* shape. */
int fat_stat(const char *path, fs_info_t *info);
int fat_opendir(const char *path);
int fat_readdir(int des, fs_info_t *info);
int fat_closedir(int des);
long fat_telldir(int des);
int fat_seekdir(int des, long off);
int fat_rewinddir(int des);
int fat_getfree(const char *path, uint32_t *free_sectors, uint32_t *total_sectors);
int fat_chmod(const char *path, uint8_t attr, uint8_t mask);
int fat_utime(const char *path, uint16_t fdate, uint16_t ftime);
int fat_getlabel(const char *path, char *label, size_t sz);
int fat_setlabel(const char *path);
int fat_unlink(const char *path);
int fat_rename(const char *oldp, const char *newp);
int fat_mkdir(const char *path);
int fat_chdir(const char *path);
int fat_chdrive(const char *drive);
size_t fat_getcwd(char *out, size_t outsz);

void fat_dir_reset(void); /* close open FatFs directories (machine reset) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_USB_MSC_H_ */
