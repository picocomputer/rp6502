/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_ROM_ROM_H_
#define _CORE_ROM_ROM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/api/std.h"

typedef struct
{
    uint32_t addr, len, crc;
} rom_record_t;

/* The packer caps every memory chunk at 1024 bytes and never lets one cross a
 * 64 KB page (tools/rp6502.py), so no record in a .rp6502 is larger than this.
 * A machine's record buffer is this big and the pump refuses anything bigger. */
#define ROM_RECORD_MAX 1024

/* One .rp6502 being read, one record per rom_pump_next. rom_pump_open,
 * rom_pump_open_fd and rom_pump_next each take the caller's own
 * ROM_RECORD_MAX bytes as buf; the pump reads both the header lines and the
 * record data into it. */
typedef struct
{
    int fd;
    uint32_t pos;      /* file offset of the next unread line */
    uint32_t prog_end; /* the program records end here; 0 means run to EOF */
    uint32_t assets_start;
    bool vec_lo, vec_hi; /* a CRC-checked record covered $FFFC, and one covered $FFFD */
} rom_pump_t;

typedef enum
{
    ROM_PUMP_RECORD,
    ROM_PUMP_SKIP,  /* a comment or blank line */
    /* no more program records; a classic image cannot tell its end from a
     * read failure */
    ROM_PUMP_EOF,
    ROM_PUMP_ERROR,
} rom_pump_result;

bool rom_pump_open(rom_pump_t *p, const char *path, uint8_t *buf, api_errno *err);
bool rom_pump_open_fd(rom_pump_t *p, int fd, uint8_t *buf, api_errno *err);
rom_pump_result rom_pump_next(rom_pump_t *p, uint8_t *buf, rom_record_t *rec, api_errno *err);
bool rom_pump_complete(const rom_pump_t *p); /* both reset-vector bytes arrived */
void rom_pump_close(rom_pump_t *p);

/* An installed ":name" and its host file (alias.c).
 *
 * host is a host path (osal/fs.h), and the install stores its absolute form.
 * name is in the code page, without the ":", and rom_alias_insert takes it
 * from the last part of host. An install replaces an earlier one of the same
 * name. Each insert returns the installed name, a string held in the alias
 * table, or NULL when it fails.
 *
 * rom_alias_resolve returns the absolute host path installed under path,
 * without copying it, or NULL when path is not an installed ":name".
 * rom_alias_open opens a ROM image for reading by any name rom_load accepts:
 * the host path of an install, or else path itself through fs_rom_open. */
const char *rom_alias_insert(const char *host);
const char *rom_alias_insert_as(const char *host, const char *name);
bool rom_alias_remove(const char *name);
const char *rom_alias_resolve(const char *path);
int rom_alias_open(const char *path, api_errno *err);

/* Load a .rp6502 into ram[]/xram[]. The path may be a drive path or an
 * installed ":name", which rom_load resolves. The named assets are not read:
 * only the start of the asset directory is kept, so a ROM: open scans the file
 * for the entry on demand. Returns false, after printing the reason on the
 * console, on any format or CRC error. */
bool rom_load(const char *path);

/* The loader hands its descriptor and asset-directory offset to the ROM: drive,
 * which owns the descriptor from here; rom_assets_reset closes it. */
void rom_asset_adopt(int fd, uint32_t assets_start);
bool rom_asset_find(const char *name, uint32_t *base, uint32_t *len);
int rom_asset_fd(void);
uint32_t rom_asset_dir(void);

bool rom_std_handles(const char *path);
int rom_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result rom_std_close(int desc, api_errno *err);
std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
int rom_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
bool rom_std_ident(int desc, sst_cursor_t *c);
int rom_std_reopen(sst_cursor_t *c, api_errno *err);
void rom_assets_reset(void);

/* Read a named asset into buf, NUL-terminated and truncated to bufsz-1. Returns
 * the number of bytes read, or -1 if no ROM is loaded, the asset is absent, buf
 * is unusable, or the seek or read fails. */
long rom_read_asset(const char *name, char *buf, size_t bufsz);

/* Bumped on each successful rom_load, so the debugger's ROM Help viewer can
 * tell that the ROM changed while its window was open. */
uint32_t rom_generation(void);

/* 1 byte for whether an image is open, 4 for the asset directory offset, 4 for
 * the image length, 4 for the generation. The path is not among them, because
 * proc already carries the running program's name; the length is the
 * cross-check that the file on disk is still the one the blob was made from. */
#define ASSET_SST_SIZE 13
void asset_sst_save(sst_cursor_t *c, unsigned flags);
bool asset_sst_load(sst_cursor_t *c, unsigned flags);

#define ASSET_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(ASET, 1, ASSET_SST_SIZE, asset_sst_save, asset_sst_load))

#define ROM_STD_DRIVER           \
    {                               \
        .handles = rom_std_handles, \
        .open = rom_std_open,       \
        .close = rom_std_close,     \
        .read = rom_std_read,       \
        .lseek = rom_std_lseek,     \
        .ident = rom_std_ident,     \
        .reopen = rom_std_reopen,   \
    }

#endif /* _CORE_ROM_ROM_H_ */
