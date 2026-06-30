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

#include "emu/api/api.h" /* io_result */

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
 * file. The loader names the backing file with fs_set_rom_src and notes where the
 * asset directory begins; a "ROM:name" open then scans the file for the entry —
 * NO bytes are copied into RAM, and the image may carry any number of assets. ---- */
void fs_set_rom_src(const char *hostpath);
long fs_read_rom_asset(const char *name, void *buf, size_t max); /* host-side; -1 if no such asset */

/* Run ROM: reads as non-blocking POSIX AIO (the real-time window) vs synchronous
 * (headless/tests, for determinism). Off by default; mirrors msc_set_async. */
void rom_set_async(bool on);

/* ---- std file driver hooks (registered in std.c's table). ROM: asset windows;
 * overlay reuses the read-only window side. ---- */
bool rom_std_handles(const char *path);
void *rom_std_open(const char *path, uint8_t flags);
void *rom_window_open(const char *hostpath, size_t base, size_t len); /* a read-only [base,len) window */
void rom_window_close(void *desc);
io_result rom_window_read(void *desc, void *buf, size_t n, size_t *got); /* IO_PENDING while the AIO read is in flight */
long rom_window_lseek(void *desc, long off, int whence);
void rom_assets_reset(void); /* forget the asset directory (a new program replaces it) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_ROM_H_ */
