/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared ELF32 image loader — see dwarf_elf.h.
 */

#include "emu/dbg/dwarf_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t elf_shdr_u32(const elf_image *im, int i, int field)
{
    const uint8_t *p = im->buf + im->e_shoff + (uint32_t)i * im->e_shentsize + field;
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

int elf_section_count(const elf_image *im) { return im->e_shnum; }

const char *elf_section_name(const elf_image *im, int i)
{
    uint32_t no = elf_shdr_u32(im, i, 0);
    return (no < (uint64_t)im->size - im->shstr_off) ? (const char *)(im->buf + im->shstr_off + no) : "";
}

bool elf_find_section(const elf_image *im, const char *name, uint32_t *off, uint32_t *size)
{
    for (int i = 0; i < im->e_shnum; i++)
        if (strcmp(elf_section_name(im, i), name) == 0)
        {
            if (off) *off = elf_shdr_u32(im, i, 16);
            if (size) *size = elf_shdr_u32(im, i, 20);
            return true;
        }
    if (off) *off = 0;
    if (size) *size = 0;
    return false;
}

bool elf_open(const char *path, elf_image *im)
{
    memset(im, 0, sizeof *im);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 64) { fclose(f); return false; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return false; }
    buf[sz] = 0; /* terminate any unterminated string at end of file */
    fclose(f);

    /* ELF32, little-endian only (the llvm-mos target). */
    if (memcmp(buf, "\x7f""ELF", 4) != 0 || buf[4] != 1 /*ELFCLASS32*/ || buf[5] != 1 /*little*/) { free(buf); return false; }
    im->buf = buf;
    im->size = sz;
    im->e_shoff = buf[32] | (buf[33] << 8) | (buf[34] << 16) | ((uint32_t)buf[35] << 24);
    im->e_shentsize = buf[46] | (buf[47] << 8);
    im->e_shnum = buf[48] | (buf[49] << 8);
    im->e_shstrndx = buf[50] | (buf[51] << 8);
    if (im->e_shoff == 0 || im->e_shentsize < 40 || im->e_shstrndx >= im->e_shnum ||
        (uint64_t)im->e_shoff + (uint64_t)im->e_shnum * im->e_shentsize > (uint64_t)sz)
    {
        elf_close(im);
        return false;
    }
    /* section-header string table's own sh_offset (field 16 of its entry) */
    im->shstr_off = elf_shdr_u32(im, im->e_shstrndx, 16);
    if (im->shstr_off >= (uint64_t)sz)
    {
        elf_close(im);
        return false;
    }
    return true;
}

void elf_close(elf_image *im)
{
    free(im->buf);
    memset(im, 0, sizeof *im);
}
