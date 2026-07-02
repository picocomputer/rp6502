/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ROM loader (rom.c): load a .rp6502 into ram[]/xram[] and serve its bundled
 * assets on demand as the ROM: drive (read-only file windows). Also the shared
 * CRC-32/ISO-HDLC the loader and the PNG writer use.
 */

#ifndef _EMU_ROM_H_
#define _EMU_ROM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emu/api/std.h" /* std_driver_t */

#ifdef __cplusplus
extern "C"
{
#endif

/* CRC-32/ISO-HDLC (zlib). Shared by the ROM loader and the PNG writer. */
uint32_t emu_crc32(uint32_t crc, const void *buf, size_t len);

/* Load a .rp6502 into ram[]/xram[]. The path may be a host path, a drive path
 * (MSC0:/...), or an overlay ROM name; emu_rom_load resolves it. The program
 * memory-chunk records are streamed straight into ram[]/xram[]; the named assets
 * are NOT read — only the start of the asset directory is noted, so a ROM: read
 * scans the file for the entry on demand. Returns false (message on stderr) on
 * any format or CRC error. */
bool emu_rom_load(const char *path);

/* ---- ROM: drive (rom.c): the .rp6502's bundled assets, read on demand from the
 * file. The loader names the backing file and notes where the asset directory
 * begins; a "ROM:name" open then scans the file for the entry — NO bytes are
 * copied into RAM, and the image may carry any number of assets. ---- */
long rom_read_asset(const char *name, void *buf, size_t max); /* host-side; -1 if no such asset */

/* Run ROM: reads as non-blocking POSIX AIO (the real-time window) vs synchronous
 * (headless/tests, for determinism). Off by default; mirrors msc_set_async. */
void rom_set_async(bool on);

/* The ROM: file driver (read-only asset windows), for std.c's table. */
bool rom_std_handles(const char *path);
int rom_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result rom_std_close(int desc, api_errno *err);
std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
int rom_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err);
void rom_assets_reset(void); /* forget the asset directory (a new program replaces it) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_ROM_H_ */
