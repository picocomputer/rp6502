/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared ELF32 image loader for the DWARF readers (.debug_info / .debug_line /
 * .debug_frame). Reads a little-endian ELF32 file into memory, validates the
 * header and section-header string table, and exposes section-header field/name
 * accessors plus a by-name lookup. It guarantees only that the header and
 * section names stay in-bounds; each reader keeps its own per-section
 * [off, off+size) bounds checks. The image owns the file bytes until elf_close.
 */

#ifndef _EMU_DBG_DWARF_ELF_H_
#define _EMU_DBG_DWARF_ELF_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint8_t *buf; /* whole file image, NUL-terminated at [size]; freed by elf_close */
    long size;
    uint32_t e_shoff, shstr_off;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} elf_image;

/* Load + validate an ELF32 LE file. False (nothing to free) if it can't be read
 * or isn't a usable ELF32; on success *im owns the image. */
bool elf_open(const char *path, elf_image *im);
void elf_close(elf_image *im);

int elf_section_count(const elf_image *im);
/* Raw 32-bit section-header field (field = byte offset within the entry). */
uint32_t elf_shdr_u32(const elf_image *im, int i, int field);
/* Section name, clamped so it never walks past the image. */
const char *elf_section_name(const elf_image *im, int i);
/* First section named `name` -> its file offset + size; false (and 0/0) if absent. */
bool elf_find_section(const elf_image *im, const char *name, uint32_t *off, uint32_t *size);

#endif /* _EMU_DBG_DWARF_ELF_H_ */
