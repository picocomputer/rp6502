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

/* A .rp6502 memory record: where it goes, how much of it there is, and what
 * it should add up to. The three numbers are the file format's, not any
 * machine's reading of it; the pump decides the rules about them and each
 * loader does its own moving of bytes and its own complaining. */
typedef struct
{
    uint32_t addr, len, crc;
} rom_record_t;

/* The largest record the format produces: the packer caps every memory chunk
 * at 1024 bytes and never crosses a 64 KB page (tools/rp6502.py). A machine's
 * record buffer is this big and the pump refuses anything bigger. */
#define ROM_RECORD_MAX 1024

/* The record pump: the .rp6502 stream read through the fs seam's ROM
 * descriptor, one record per step. A machine that must not stall its walks
 * steps it once per pass; a machine that can block loops it. The machine
 * deposits the bytes; the pump owns the format. buf is the machine's own
 * ROM_RECORD_MAX bytes -- the firmware passes mbuf. */
typedef struct
{
    int fd;                /* the loader's descriptor, fs_rom_open's */
    uint32_t pos;          /* file offset of the next unread line */
    uint32_t prog_end;     /* records end here; 0 = classic, run to EOF */
    uint32_t assets_start; /* asset directory offset; 0 = no assets */
    bool vec_lo, vec_hi;   /* the reset-vector bytes seen land ($FFFC/$FFFD) */
} rom_pump_t;

typedef enum
{
    ROM_PUMP_RECORD, /* rec + buf hold one deposited-ready record */
    ROM_PUMP_SKIP,   /* a comment or blank line; nothing to deposit */
    ROM_PUMP_EOF,    /* the program section is done */
    ROM_PUMP_ERROR,  /* *err says; the image is not loadable */
} rom_pump_result;

bool rom_pump_open(rom_pump_t *p, const char *path, uint8_t *buf, api_errno *err);
bool rom_pump_open_fd(rom_pump_t *p, int fd, uint8_t *buf, api_errno *err); /* a descriptor the machine already holds */
rom_pump_result rom_pump_next(rom_pump_t *p, uint8_t *buf, rom_record_t *rec, api_errno *err);
bool rom_pump_complete(const rom_pump_t *p); /* both reset-vector bytes arrived */
void rom_pump_close(rom_pump_t *p);

/* The null drive as a map (alias.c): an installed ":name" aliasing the host
 * file that backs it. Nil unless the host defines ROM_ALIAS_MAX; the seam's
 * fs_rom_open resolves through it.
 *
 * insert copies the path it is given. resolve borrows: the answer is the
 * install's own string, or the path itself where nothing claims it, so a
 * caller neither sizes a buffer nor frees anything. */
bool rom_alias_insert(const char *hostpath);
const char *rom_alias_resolve(const char *path);

/* Load a .rp6502 into ram[]/xram[]. The path may be a host path, a drive path
 * (MSC0:/...), or an overlay ROM name; rom_load resolves it. The program
 * memory-chunk records are streamed straight into ram[]/xram[]; the named assets
 * are NOT read — only the start of the asset directory is noted, so a ROM: read
 * scans the file for the entry on demand. Returns false (message to the log
 * sink) on any format or CRC error. */
bool rom_load(const char *path);

/* ---- ROM: drive (asset.c): the .rp6502's bundled assets, read on demand
 * from the file through the loader's descriptor. The loader adopts the
 * descriptor and the directory offset into the driver; a "ROM:name" open then
 * scans the file for the entry — NO bytes are copied into RAM, and the image
 * may carry any number of assets. ---- */

/* The loader hands its descriptor and asset-directory offset to the driver;
 * the driver owns the descriptor from here (rom_assets_reset closes it). */
void rom_asset_adopt(int fd, uint32_t assets_start);
bool rom_asset_find(const char *name, uint32_t *base, uint32_t *len);
int rom_asset_fd(void);       /* the adopted descriptor */
uint32_t rom_asset_dir(void); /* directory offset; 0 = classic, no assets */

/* The ROM: file driver (read-only asset windows), for std.c's table. */
bool rom_std_handles(const char *path);
int rom_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result rom_std_close(int desc, api_errno *err);
std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
int rom_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
void rom_assets_reset(void); /* forget the asset directory (a new program replaces it) */

/* Read a named asset from the loaded ROM into buf (NUL-terminated, truncated to
 * bufsz-1). Returns bytes read, or -1 if no ROM is loaded or the asset is absent.
 * Host-side reader for the debugger's ROM Help viewer. */
long rom_read_asset(const char *name, char *buf, size_t bufsz);

/* Bumped on each successful rom_load; the ROM Help viewer watches it to re-read
 * the help asset when the loaded ROM changes while the window is open. */
uint32_t rom_generation(void);

/* This driver's stdio row: the std_driver_t initializer core/api/std.c
 * builds this machine's table from. Read-only: the ROM a program is running out of. */
#define ROM_STD_DRIVER           \
    {                               \
        .handles = rom_std_handles, \
        .open = rom_std_open,       \
        .close = rom_std_close,     \
        .read = rom_std_read,       \
        .lseek = rom_std_lseek,     \
    }

#endif /* _CORE_ROM_ROM_H_ */
