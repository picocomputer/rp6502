/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * DWARF .debug_line reader — see dwarf_line.h. Parses the ELF section table to
 * find .debug_line, then interprets the line-number program (the DWARF state
 * machine) into a flat, address-sorted row table. Defensive against truncated /
 * malformed input: any short read aborts that unit and returns what parsed.
 */

#include "emu/dbg/dwarf_line.h"
#include "emu/dbg/dwarf_cursor.h"
#include "emu/dbg/dwarf_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- one emitted line-table row ---- */
typedef struct
{
    uint32_t addr;
    const char *file; /* into dl->strs (NULL for a pure end-of-sequence marker) */
    int line;
    bool end_seq;
} dl_row;

typedef struct
{
    uint32_t addr;
    uint32_t size;
    const char *name;
} dl_func;

/* an allocatable ELF section (.text/.data/.bss/.zp/...) for the memory-map view */
typedef struct
{
    const char *name; /* into dl->strs */
    uint16_t addr;    /* 6502 load address */
    uint32_t size;
} dl_section;

#define DL_MAX_SECTIONS 32

struct dwarf_line
{
    dl_row *rows;
    size_t nrows;
    char **strs; /* owned file-path + symbol-name strings */
    size_t nstrs;
    dl_func *funcs; /* STT_FUNC symbols, sorted by addr */
    size_t nfuncs;
    dl_section sections[DL_MAX_SECTIONS];
    int nsections;
};

/* Byte cursor (dwarf_cur + dwarf_u8/dwarf_u16/dwarf_u32/dwarf_uleb/dwarf_sleb/dwarf_cstr) is in dwarf_cursor.c. */

/* ---- growable string + row pools on the dwarf_line_t ---- */
static const char *intern(dwarf_line_t *dl, const char *s)
{
    char *dup = strdup(s ? s : "");
    if (!dup)
        return "";
    char **ns = realloc(dl->strs, (dl->nstrs + 1) * sizeof(char *));
    if (!ns) { free(dup); return ""; }
    dl->strs = ns;
    dl->strs[dl->nstrs++] = dup;
    return dup;
}
static void push_row(dwarf_line_t *dl, uint32_t addr, const char *file, int line, bool end_seq)
{
    dl_row *nr = realloc(dl->rows, (dl->nrows + 1) * sizeof(dl_row));
    if (!nr)
        return;
    dl->rows = nr;
    dl->rows[dl->nrows].addr = addr;
    dl->rows[dl->nrows].file = file;
    dl->rows[dl->nrows].line = line;
    dl->rows[dl->nrows].end_seq = end_seq;
    dl->nrows++;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* True if one path is a trailing path-component suffix of the other, so a client
 * absolute path matches a relative DWARF path yet a/util.c != b/util.c. */
static bool path_suffix_match(const char *a, const char *b)
{
    size_t i = strlen(a), j = strlen(b);
    while (i > 0 && j > 0)
    {
        char ca = a[i - 1], cb = b[j - 1];
        bool sa = (ca == '/' || ca == '\\'), sb = (cb == '/' || cb == '\\');
        if (sa && sb) { i--; j--; continue; }
        if (sa || sb) break;
        if (ca != cb) return false;
        i--; j--;
    }
    return (i == 0 || a[i - 1] == '/' || a[i - 1] == '\\') &&
           (j == 0 || b[j - 1] == '/' || b[j - 1] == '\\');
}

/* DWARF line standard opcodes */
enum
{
    LNS_copy = 1,
    LNS_advance_pc = 2,
    LNS_advance_line = 3,
    LNS_set_file = 4,
    LNS_set_column = 5,
    LNS_negate_stmt = 6,
    LNS_set_basic_block = 7,
    LNS_const_add_pc = 8,
    LNS_fixed_advance_pc = 9,
    LNS_set_prologue_end = 10,
    LNS_set_epilogue_begin = 11,
    LNS_set_isa = 12,
};
enum
{
    LNE_end_sequence = 1,
    LNE_set_address = 2,
    LNE_define_file = 3,
};

/* DWARF5 line-header content-type codes (dir/file entry formats). */
enum
{
    DW_LNCT_path = 1,
    DW_LNCT_directory_index = 2,
    DW_LNCT_timestamp = 3,
    DW_LNCT_size = 4,
    DW_LNCT_MD5 = 5,
};
/* The forms that appear in a v5 line-header dir/file table. */
enum
{
    LF_data2 = 0x05,
    LF_data4 = 0x06,
    LF_data8 = 0x07,
    LF_string = 0x08,
    LF_block = 0x09,
    LF_data1 = 0x0b,
    LF_udata = 0x0f,
    LF_strp = 0x0e,
    LF_data16 = 0x1e,
    LF_line_strp = 0x1f,
    LF_strx = 0x1a,
    LF_strx1 = 0x25,
    LF_strx2 = 0x26,
};

/* Read one v5 line-header form value. String forms return a pointer (into
 * .debug_line_str/.debug_str, which live in the still-mapped ELF image) via
 * *sout; numeric forms via *uout. strx* aren't resolved (paths use line_strp). */
static void lnct_read(dwarf_cur *c, uint16_t form,
                      const uint8_t *lstr, uint32_t lstr_size,
                      const uint8_t *str, uint32_t str_size,
                      const char **sout, uint64_t *uout)
{
    switch (form)
    {
    case LF_string:
    {
        const char *s = (const char *)c->p;
        while (c->p < c->end && *c->p) c->p++;
        if (c->p < c->end) c->p++;
        *sout = s;
        break;
    }
    case LF_line_strp:
    {
        uint32_t o = dwarf_u32(c);
        *sout = (lstr && o < lstr_size) ? (const char *)(lstr + o) : "";
        break;
    }
    case LF_strp:
    {
        uint32_t o = dwarf_u32(c);
        *sout = (str && o < str_size) ? (const char *)(str + o) : "";
        break;
    }
    case LF_data1: *uout = dwarf_u8(c); break;
    case LF_data2: *uout = dwarf_u16(c); break;
    case LF_data4: *uout = dwarf_u32(c); break;
    case LF_data8: *uout = dwarf_u64(c); break;
    case LF_udata: *uout = dwarf_uleb(c); break;
    case LF_strx1: *uout = dwarf_u8(c); break;
    case LF_strx2: *uout = dwarf_u16(c); break;
    case LF_strx: *uout = dwarf_uleb(c); break;
    case LF_data16:
        for (int i = 0; i < 16; i++) (void)dwarf_u8(c);
        break;
    case LF_block:
    {
        uint64_t n = dwarf_uleb(c);
        if (n > (uint64_t)(c->end - c->p)) { c->ok = false; break; }
        c->p += n;
        break;
    }
    default:
        c->ok = false; /* unknown form: can't size the entry */
        break;
    }
}

/* Run the line-number program. files[] is indexed by the DWARF file register
 * directly (0-based, per DWARF5); unused slots are "". */
static void run_line_program(dwarf_line_t *dl, dwarf_cur *c, const uint8_t *unit_end,
                             const char *const *files, uint8_t min_inst, uint8_t default_is_stmt,
                             int8_t line_base, uint8_t line_range, uint8_t opcode_base,
                             const uint8_t *std_len)
{
    uint32_t address = 0;
    int file = 1, line = 1;
    bool is_stmt = default_is_stmt != 0;
    (void)is_stmt;

    while (c->p < unit_end && c->ok)
    {
        uint8_t op = dwarf_u8(c);
        if (op == 0)
        {
            /* extended opcode */
            uint64_t len = dwarf_uleb(c);
            if (len > (uint64_t)(unit_end - c->p)) { c->ok = false; break; }
            const uint8_t *next = c->p + len;
            uint8_t sub = dwarf_u8(c);
            if (sub == LNE_end_sequence)
            {
                push_row(dl, address, NULL, line, true);
                address = 0;
                file = 1;
                line = 1;
                is_stmt = default_is_stmt != 0;
            }
            else if (sub == LNE_set_address)
            {
                uint32_t a = 0;
                int nb = (int)len - 1; /* address size = operand bytes */
                for (int i = 0; i < nb; i++)
                {
                    uint8_t b = dwarf_u8(c);
                    if (i < 4)
                        a |= (uint32_t)b << (8 * i);
                }
                address = a;
            }
            /* LNE_define_file and any vendor opcodes: skip via next */
            c->p = next;
        }
        else if (op < opcode_base)
        {
            switch (op)
            {
            case LNS_copy:
                push_row(dl, address, (file >= 0 && file < 256) ? files[file] : "", line, false);
                break;
            case LNS_advance_pc:
                address += (uint32_t)(dwarf_uleb(c) * min_inst);
                break;
            case LNS_advance_line:
                line += (int)dwarf_sleb(c);
                break;
            case LNS_set_file:
                file = (int)dwarf_uleb(c);
                break;
            case LNS_set_column:
                (void)dwarf_uleb(c);
                break;
            case LNS_negate_stmt:
                is_stmt = !is_stmt;
                break;
            case LNS_set_basic_block:
                break;
            case LNS_const_add_pc:
                address += (uint32_t)(((255 - opcode_base) / line_range) * min_inst);
                break;
            case LNS_fixed_advance_pc:
                address += dwarf_u16(c);
                break;
            case LNS_set_prologue_end:
            case LNS_set_epilogue_begin:
                break;
            case LNS_set_isa:
                (void)dwarf_uleb(c);
                break;
            default:
                /* unknown standard opcode: skip its ULEB operands */
                for (int i = 0; i < std_len[op]; i++)
                    (void)dwarf_uleb(c);
                break;
            }
        }
        else
        {
            /* special opcode */
            int adj = op - opcode_base;
            address += (uint32_t)((adj / line_range) * min_inst);
            line += line_base + (adj % line_range);
            push_row(dl, address, (file >= 0 && file < 256) ? files[file] : "", line, false);
        }
    }
}

/* Parse the v5 dir/file tables (form-coded) into files[] (0-based). dirs point
 * into the still-mapped ELF image; file full-paths are interned on dl. */
static void parse_v5_tables(dwarf_line_t *dl, dwarf_cur *c, const char **files,
                            const uint8_t *lstr, uint32_t lstr_size,
                            const uint8_t *str, uint32_t str_size)
{
    /* directory_entry_format + directories */
    const char *dirs[64];
    int ndirs = 0;
    uint8_t dfmt_n = dwarf_u8(c);
    uint16_t dct[16], dfm[16];
    for (int i = 0; i < dfmt_n; i++)
    {
        uint16_t ct = (uint16_t)dwarf_uleb(c), fm = (uint16_t)dwarf_uleb(c);
        if (i < 16) { dct[i] = ct; dfm[i] = fm; }
    }
    uint64_t dcount = dwarf_uleb(c);
    for (uint64_t d = 0; d < dcount && c->ok; d++)
    {
        const char *dp = "";
        for (int i = 0; i < dfmt_n && c->ok; i++)
        {
            const char *s = ""; uint64_t u = 0;
            uint16_t fm = (i < 16) ? dfm[i] : 0;
            if (!fm) { c->ok = false; break; }
            lnct_read(c, fm, lstr, lstr_size, str, str_size, &s, &u);
            if ((i < 16 ? dct[i] : 0) == DW_LNCT_path) dp = s;
        }
        if (d < 64) dirs[ndirs++] = dp;
    }

    /* file_name_entry_format + file_names (0-based) */
    uint8_t ffmt_n = dwarf_u8(c);
    uint16_t fct[16], ffm[16];
    for (int i = 0; i < ffmt_n; i++)
    {
        uint16_t ct = (uint16_t)dwarf_uleb(c), fm = (uint16_t)dwarf_uleb(c);
        if (i < 16) { fct[i] = ct; ffm[i] = fm; }
    }
    uint64_t fcount = dwarf_uleb(c);
    for (uint64_t fi = 0; fi < fcount && c->ok; fi++)
    {
        const char *fp = ""; uint64_t didx = 0;
        for (int i = 0; i < ffmt_n && c->ok; i++)
        {
            const char *s = ""; uint64_t u = 0;
            uint16_t fm = (i < 16) ? ffm[i] : 0;
            if (!fm) { c->ok = false; break; }
            lnct_read(c, fm, lstr, lstr_size, str, str_size, &s, &u);
            uint16_t ct = (i < 16) ? fct[i] : 0;
            if (ct == DW_LNCT_path) fp = s;
            else if (ct == DW_LNCT_directory_index) didx = u;
        }
        char full[1024];
        if (fp[0] == '/' || didx >= (uint64_t)ndirs || didx >= 64)
            snprintf(full, sizeof full, "%s", fp);
        else
            snprintf(full, sizeof full, "%s/%s", dirs[didx], fp);
        if (fi < 256) files[fi] = intern(dl, full);
    }
}

/* Parse one line-number program unit at [c->p, unit_end). DWARF5-only. */
static void parse_unit(dwarf_line_t *dl, dwarf_cur *c, const uint8_t *unit_end,
                       const uint8_t *lstr, uint32_t lstr_size,
                       const uint8_t *str, uint32_t str_size)
{
    const char *files[256];
    for (int i = 0; i < 256; i++)
        files[i] = "";

    uint16_t version = dwarf_u16(c);
    if (version != 5) /* DWARF5-only (llvm-mos debug fork) */
        return;

    (void)dwarf_u8(c); /* address_size */
    (void)dwarf_u8(c); /* segment_selector_size */
    uint32_t header_len = dwarf_u32(c);
    if (!c->ok || c->p > unit_end || header_len > (uint32_t)(unit_end - c->p))
        return; /* header_length runs past the unit: corrupt prologue */
    const uint8_t *prog = c->p + header_len; /* program starts after the prologue */

    uint8_t min_inst = dwarf_u8(c);
    (void)dwarf_u8(c); /* maximum_operations_per_instruction */
    uint8_t default_is_stmt = dwarf_u8(c);
    int8_t line_base = (int8_t)dwarf_u8(c);
    uint8_t line_range = dwarf_u8(c);
    uint8_t opcode_base = dwarf_u8(c);
    if (!c->ok || min_inst == 0 || line_range == 0)
        return;
    uint8_t std_len[256];
    memset(std_len, 0, sizeof std_len);
    for (int i = 1; i < opcode_base && i < 256; i++)
        std_len[i] = dwarf_u8(c);

    parse_v5_tables(dl, c, files, lstr, lstr_size, str, str_size);
    if (!c->ok)
        return;

    c->p = prog;
    run_line_program(dl, c, unit_end, files, min_inst, default_is_stmt,
                     line_base, line_range, opcode_base, std_len);
}

static int row_cmp(const void *a, const void *b)
{
    const dl_row *ra = (const dl_row *)a, *rb = (const dl_row *)b;
    if (ra->addr != rb->addr)
        return (ra->addr > rb->addr) - (ra->addr < rb->addr);
    /* Same address: order an end-of-sequence marker BEFORE a real row so the
     * "largest addr <= target" search lands on the real row that actually covers
     * the address (the next sequence's start), not the previous sequence's end
     * marker — deterministic regardless of qsort stability (Windows/musl). */
    return (int)rb->end_seq - (int)ra->end_seq;
}
static int func_cmp(const void *a, const void *b)
{
    uint32_t x = ((const dl_func *)a)->addr, y = ((const dl_func *)b)->addr;
    return (x > y) - (x < y);
}

/* Parse .symtab STT_FUNC symbols into dl->funcs (names interned). */
static void parse_symbols(dwarf_line_t *dl, const uint8_t *buf, long sz,
                          uint32_t sym_off, uint32_t sym_size,
                          uint32_t str_off, uint32_t str_size)
{
    if (!sym_off || !str_off)
        return;
    if ((uint64_t)sym_off + sym_size > (uint64_t)sz ||
        (uint64_t)str_off + str_size > (uint64_t)sz)
        return;
    const char *strtab = (const char *)(buf + str_off);
    for (uint32_t o = 0; o + 16 <= sym_size; o += 16) /* Elf32_Sym = 16 bytes */
    {
        const uint8_t *s = buf + sym_off + o;
        uint32_t st_name = s[0] | (s[1] << 8) | (s[2] << 16) | ((uint32_t)s[3] << 24);
        uint32_t st_value = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
        uint32_t st_size = s[8] | (s[9] << 8) | (s[10] << 16) | ((uint32_t)s[11] << 24);
        uint8_t st_info = s[12];
        if ((st_info & 0xf) != 2 /*STT_FUNC*/ || st_name >= str_size)
            continue;
        const char *nm = strtab + st_name;
        if (!nm[0])
            continue;
        dl_func *nf = realloc(dl->funcs, (dl->nfuncs + 1) * sizeof(dl_func));
        if (!nf)
            break;
        dl->funcs = nf;
        dl->funcs[dl->nfuncs].addr = st_value;
        dl->funcs[dl->nfuncs].size = st_size;
        dl->funcs[dl->nfuncs].name = intern(dl, nm);
        dl->nfuncs++;
    }
    if (dl->nfuncs)
        qsort(dl->funcs, dl->nfuncs, sizeof(dl_func), func_cmp);
}

dwarf_line_t *dwarf_line_load(const char *elf_path)
{
    elf_image im;
    if (!elf_open(elf_path, &im))
        return NULL;

    /* section header: name(0) type(4) flags(8) addr(12) offset(16) size(20)... */
    uint32_t dl_off = 0, dl_size = 0;
    uint32_t sym_off = 0, sym_size = 0, str_off = 0, str_size = 0;
    uint32_t lstr_off = 0, lstr_size = 0, dstr_off = 0, dstr_size = 0;
    elf_find_section(&im, ".debug_line", &dl_off, &dl_size);
    elf_find_section(&im, ".debug_line_str", &lstr_off, &lstr_size);
    elf_find_section(&im, ".debug_str", &dstr_off, &dstr_size);
    elf_find_section(&im, ".symtab", &sym_off, &sym_size);
    elf_find_section(&im, ".strtab", &str_off, &str_size);
    if (dl_off == 0 || dl_size == 0 || (uint64_t)dl_off + dl_size > (uint64_t)im.size)
    {
        elf_close(&im);
        return NULL;
    }
    /* A string table whose [off,off+size) runs past EOF would let a v5 path form
     * dereference outside buf; neutralize it so those paths resolve to "". */
    if (lstr_off && (uint64_t)lstr_off + lstr_size > (uint64_t)im.size)
        lstr_off = lstr_size = 0;
    if (dstr_off && (uint64_t)dstr_off + dstr_size > (uint64_t)im.size)
        dstr_off = dstr_size = 0;
    const uint8_t *lstr = lstr_off ? im.buf + lstr_off : NULL;
    const uint8_t *dstr = dstr_off ? im.buf + dstr_off : NULL;

    dwarf_line_t *dl = calloc(1, sizeof *dl);
    if (!dl)
    {
        elf_close(&im);
        return NULL;
    }

    /* Allocatable sections (.text/.data/.bss/.zp/.noinit/...) for the memory-map
     * view: name + 6502 load address + size. SHF_ALLOC (flags bit 1) with a
     * non-zero size; names are interned since the image is freed below. */
    for (int i = 0; i < elf_section_count(&im) && dl->nsections < DL_MAX_SECTIONS; i++)
    {
        uint32_t flags = elf_shdr_u32(&im, i, 8), saddr = elf_shdr_u32(&im, i, 12), ssize = elf_shdr_u32(&im, i, 20);
        if (!(flags & 0x2) || ssize == 0)
            continue;
        dl->sections[dl->nsections].name = intern(dl, elf_section_name(&im, i));
        dl->sections[dl->nsections].addr = (uint16_t)saddr;
        dl->sections[dl->nsections].size = ssize;
        dl->nsections++;
    }

    /* Walk the (possibly multiple) units in .debug_line. */
    dwarf_cur c = {im.buf + dl_off, im.buf + dl_off + dl_size, true};
    while (c.p + 4 <= c.end && c.ok)
    {
        const uint8_t *unit_start = c.p;
        uint32_t unit_len = dwarf_u32(&c);
        if (unit_len == 0 || unit_len == 0xffffffffu)
            break; /* 0 / 64-bit DWARF: stop */
        const uint8_t *unit_end = unit_start + 4 + unit_len;
        if (unit_end > c.end)
            unit_end = c.end;
        dwarf_cur uc = {c.p, unit_end, true};
        parse_unit(dl, &uc, unit_end, lstr, lstr_size, dstr, dstr_size);
        c.p = unit_end;
    }
    parse_symbols(dl, im.buf, im.size, sym_off, sym_size, str_off, str_size);
    elf_close(&im);

    if (dl->nrows == 0)
    {
        dwarf_line_free(dl);
        return NULL;
    }
    qsort(dl->rows, dl->nrows, sizeof(dl_row), row_cmp);
    return dl;
}

void dwarf_line_free(dwarf_line_t *dl)
{
    if (!dl)
        return;
    for (size_t i = 0; i < dl->nstrs; i++)
        free(dl->strs[i]);
    free(dl->strs);
    free(dl->rows);
    free(dl->funcs);
    free(dl);
}

int dwarf_line_sections(const dwarf_line_t *dl, dwarf_section_t *out, int max)
{
    if (!dl)
        return 0;
    int n = 0;
    for (int i = 0; i < dl->nsections && n < max; i++)
    {
        out[n].name = dl->sections[i].name;
        out[n].addr = dl->sections[i].addr;
        out[n].size = dl->sections[i].size;
        n++;
    }
    return n;
}

bool dwarf_line_addr_to_src(const dwarf_line_t *dl, uint16_t addr, const char **file, int *line)
{
    if (!dl || dl->nrows == 0)
        return false;
    /* Largest row with row.addr <= addr; valid only if that row is not an
     * end-of-sequence marker (i.e. addr is inside a real [start,end) range). */
    size_t lo = 0, hi = dl->nrows, best = (size_t)-1;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (dl->rows[mid].addr <= addr)
        {
            best = mid;
            lo = mid + 1;
        }
        else
            hi = mid;
    }
    if (best == (size_t)-1)
        return false;
    const dl_row *r = &dl->rows[best];
    if (r->end_seq || !r->file)
        return false;
    if (file)
        *file = r->file;
    if (line)
        *line = r->line;
    return true;
}

bool dwarf_line_src_to_addr(const dwarf_line_t *dl, const char *file, int line,
                            uint16_t *addr, int *bound_line)
{
    if (!dl || !file)
        return false;
    /* Among rows at/after the requested line pick the smallest (line, then
     * address) so a breakpoint binds to the next code line. Basename must match;
     * a full path-suffix match is preferred (disambiguates same-named files),
     * falling back to basename so a client absolute path still binds to the
     * build-relative DWARF path. */
    const char *want = base_name(file);
    bool sfound = false, bfound = false;
    int sline = 0, bline = 0;
    uint32_t saddr = 0, baddr = 0;
    for (size_t i = 0; i < dl->nrows; i++)
    {
        const dl_row *r = &dl->rows[i];
        if (r->end_seq || !r->file || r->line < line)
            continue;
        if (strcmp(base_name(r->file), want) != 0)
            continue;
        if (!bfound || r->line < bline || (r->line == bline && r->addr < baddr))
        {
            bfound = true; bline = r->line; baddr = r->addr;
        }
        if (path_suffix_match(r->file, file) &&
            (!sfound || r->line < sline || (r->line == sline && r->addr < saddr)))
        {
            sfound = true; sline = r->line; saddr = r->addr;
        }
    }
    if (!sfound && !bfound)
        return false;
    if (addr)
        *addr = (uint16_t)(sfound ? saddr : baddr);
    if (bound_line)
        *bound_line = sfound ? sline : bline;
    return true;
}

const char *dwarf_line_addr_to_func(const dwarf_line_t *dl, uint16_t addr)
{
    if (!dl || dl->nfuncs == 0)
        return NULL;
    /* Largest function symbol with addr in [value, value+size) (or nearest
     * preceding when size is 0). */
    size_t lo = 0, hi = dl->nfuncs, best = (size_t)-1;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (dl->funcs[mid].addr <= addr)
        {
            best = mid;
            lo = mid + 1;
        }
        else
            hi = mid;
    }
    if (best == (size_t)-1)
        return NULL;
    const dl_func *fn = &dl->funcs[best];
    if (fn->size != 0 && addr >= fn->addr + fn->size)
        return NULL;
    return fn->name;
}

bool dwarf_line_func_addr(const dwarf_line_t *dl, const char *name, uint16_t *addr)
{
    if (!dl || !name)
        return false;
    for (size_t i = 0; i < dl->nfuncs; i++)
        if (strcmp(dl->funcs[i].name, name) == 0)
        {
            if (addr)
                *addr = (uint16_t)dl->funcs[i].addr;
            return true;
        }
    return false;
}
