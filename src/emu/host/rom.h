/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HOST_ROM_H_
#define _EMU_HOST_ROM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ria/api/std.h"

/* Install a .rp6502 on the null drive, keyed by its host-path basename, so a
 * boot/exec ":name" resolves back to it. */
bool rom_install(const char *hostpath);
/* Map a boot/exec ROM path (":name" / drive path / bare) to the host file the
 * loader opens. */
bool rom_resolve(const char *path, char *out, size_t outsz);

/* Load a .rp6502 into ram[]/xram[]. The path may be a host path, a drive path
 * (MSC0:/...), or an overlay ROM name; rom_load resolves it. The program
 * memory-chunk records are streamed straight into ram[]/xram[]; the named assets
 * are NOT read — only the start of the asset directory is noted, so a ROM: read
 * scans the file for the entry on demand. Returns false (message on stderr) on
 * any format or CRC error. */
bool rom_load(const char *path);

/* ---- ROM: drive (rom.c): the .rp6502's bundled assets, read on demand from the
 * file. The loader names the backing file and notes where the asset directory
 * begins; a "ROM:name" open then scans the file for the entry — NO bytes are
 * copied into RAM, and the image may carry any number of assets. ---- */

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

#endif /* _EMU_HOST_ROM_H_ */
