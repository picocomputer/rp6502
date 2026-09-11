/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Only the ELF header and the section names are checked to be in bounds, so
 * each reader bounds-checks the [off, off+size) span of the sections it uses.
 */

#ifndef _CORE_DAP_DWARF_ELF_H_
#define _CORE_DAP_DWARF_ELF_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint8_t *buf; /* the whole file, with a NUL byte at [size]; freed by elf_close */
    long size;
    uint32_t e_shoff, shstr_off;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} elf_image;

/* Every failure path frees what it took, so a false return leaves *im zeroed. */
bool elf_open(const char *path, elf_image *im);
void elf_close(elf_image *im);

int elf_section_count(const elf_image *im);
/* The field is a byte offset within the section header entry, not an index. */
uint32_t elf_shdr_u32(const elf_image *im, int i, int field);
/* An offset past the end of the image reads as the empty string. */
const char *elf_section_name(const elf_image *im, int i);
bool elf_find_section(const elf_image *im, const char *name, uint32_t *off, uint32_t *size);

#endif /* _CORE_DAP_DWARF_ELF_H_ */
