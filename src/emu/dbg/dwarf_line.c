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

/* ---- byte cursor over a buffer ---- */
typedef struct
{
    const uint8_t *p, *end;
    bool ok;
} cur;

static uint8_t u8(cur *c)
{
    if (c->p >= c->end) { c->ok = false; return 0; }
    return *c->p++;
}
static uint16_t u16(cur *c)
{
    uint16_t a = u8(c), b = u8(c);
    return (uint16_t)(a | (b << 8));
}
static uint32_t u32(cur *c)
{
    uint32_t a = u16(c), b = u16(c);
    return a | (b << 16);
}
static uint64_t uleb(cur *c)
{
    uint64_t v = 0;
    int shift = 0;
    for (;;)
    {
        uint8_t b = u8(c);
        if (!c->ok) break;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) { c->ok = false; break; }
    }
    return v;
}
static int64_t sleb(cur *c)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    do
    {
        b = u8(c);
        if (!c->ok) break;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (shift > 63) break; /* malformed: guard the next <<shift (UB at >=64) */
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}
static const char *cstr(cur *c)
{
    const char *s = (const char *)c->p;
    while (c->p < c->end && *c->p)
        c->p++;
    if (c->p >= c->end) { c->ok = false; return ""; }
    c->p++; /* skip NUL */
    return s;
}

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

/* Parse one line-number program unit at [c->p, unit_end). DWARF 2-4 only. */
static void parse_unit(dwarf_line_t *dl, cur *c, const uint8_t *unit_end)
{
    uint16_t version = u16(c);
    if (version < 2 || version > 4)
        return; /* DWARF 5 has a different prologue; not emitted with -gdwarf-4 */
    uint32_t header_len = u32(c);
    if (!c->ok || c->p > unit_end || header_len > (uint32_t)(unit_end - c->p))
        return; /* header_length runs past the unit: corrupt prologue */
    const uint8_t *prog = c->p + header_len; /* program starts after the prologue */

    uint8_t min_inst = u8(c);
    if (version >= 4)
        (void)u8(c); /* maximum_operations_per_instruction */
    uint8_t default_is_stmt = u8(c);
    int8_t line_base = (int8_t)u8(c);
    uint8_t line_range = u8(c);
    uint8_t opcode_base = u8(c);
    if (!c->ok || min_inst == 0 || line_range == 0)
        return;
    uint8_t std_len[256];
    memset(std_len, 0, sizeof std_len);
    for (int i = 1; i < opcode_base && i < 256; i++)
        std_len[i] = u8(c);

    /* include_directories (1-based), terminated by an empty string. */
    const char *dirs[64];
    int ndirs = 0;
    dirs[0] = ""; /* index 0 = compilation directory (unknown here) */
    for (;;)
    {
        const char *d = cstr(c);
        if (!c->ok || d[0] == 0)
            break;
        if (++ndirs < 64)
            dirs[ndirs] = d;
    }

    /* file_names (1-based), each {name, dir_index, mtime, length}. Resolve each
     * to a full path interned on dl. files[0] is unused. */
    const char *files[256];
    int nfiles = 0;
    files[0] = "";
    for (;;)
    {
        const char *name = cstr(c);
        if (!c->ok || name[0] == 0)
            break;
        uint64_t di = uleb(c);
        (void)uleb(c); /* mtime */
        (void)uleb(c); /* length */
        char full[1024];
        if (name[0] == '/' || di == 0 || di >= (uint64_t)(ndirs + 1) || di >= 64)
            snprintf(full, sizeof full, "%s", name);
        else
            snprintf(full, sizeof full, "%s/%s", dirs[di], name);
        if (++nfiles < 256)
            files[nfiles] = intern(dl, full);
    }
    if (!c->ok)
        return;

    /* Run the line program. */
    c->p = prog;
    uint32_t address = 0;
    int file = 1, line = 1;
    bool is_stmt = default_is_stmt != 0;
    (void)is_stmt;

    while (c->p < unit_end && c->ok)
    {
        uint8_t op = u8(c);
        if (op == 0)
        {
            /* extended opcode */
            uint64_t len = uleb(c);
            if (len > (uint64_t)(unit_end - c->p)) { c->ok = false; break; }
            const uint8_t *next = c->p + len;
            uint8_t sub = u8(c);
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
                    uint8_t b = u8(c);
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
                push_row(dl, address, (file >= 0 && file < nfiles + 1 && file < 256) ? files[file] : "", line, false);
                break;
            case LNS_advance_pc:
                address += (uint32_t)(uleb(c) * min_inst);
                break;
            case LNS_advance_line:
                line += (int)sleb(c);
                break;
            case LNS_set_file:
                file = (int)uleb(c);
                break;
            case LNS_set_column:
                (void)uleb(c);
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
                address += u16(c);
                break;
            case LNS_set_prologue_end:
            case LNS_set_epilogue_begin:
                break;
            case LNS_set_isa:
                (void)uleb(c);
                break;
            default:
                /* unknown standard opcode: skip its ULEB operands */
                for (int i = 0; i < std_len[op]; i++)
                    (void)uleb(c);
                break;
            }
        }
        else
        {
            /* special opcode */
            int adj = op - opcode_base;
            address += (uint32_t)((adj / line_range) * min_inst);
            line += line_base + (adj % line_range);
            push_row(dl, address, (file >= 0 && file < nfiles + 1 && file < 256) ? files[file] : "", line, false);
        }
    }
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
    FILE *f = fopen(elf_path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 64)
    {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = 0; /* terminate any unterminated string at end of file */
    fclose(f);

    /* ELF32, little-endian only (the llvm-mos target). */
    if (memcmp(buf, "\x7f""ELF", 4) != 0 || buf[4] != 1 /*ELFCLASS32*/ || buf[5] != 1 /*little*/)
    {
        free(buf);
        return NULL;
    }
    uint32_t e_shoff = buf[32] | (buf[33] << 8) | (buf[34] << 16) | ((uint32_t)buf[35] << 24);
    uint16_t e_shentsize = buf[46] | (buf[47] << 8);
    uint16_t e_shnum = buf[48] | (buf[49] << 8);
    uint16_t e_shstrndx = buf[50] | (buf[51] << 8);
    if (e_shoff == 0 || e_shentsize < 40 || e_shstrndx >= e_shnum ||
        (uint64_t)e_shoff + (uint64_t)e_shnum * (uint64_t)e_shentsize > (uint64_t)sz)
    {
        free(buf);
        return NULL;
    }

#define SH_U32(idx, off)                                              \
    ((uint32_t)(buf[e_shoff + (idx) * e_shentsize + (off)] |           \
                (buf[e_shoff + (idx) * e_shentsize + (off) + 1] << 8) | \
                (buf[e_shoff + (idx) * e_shentsize + (off) + 2] << 16) | \
                ((uint32_t)buf[e_shoff + (idx) * e_shentsize + (off) + 3] << 24)))

    /* section header: name(0) type(4) flags(8) addr(12) offset(16) size(20)... */
    uint32_t shstr_off = SH_U32(e_shstrndx, 16);
    if (shstr_off >= (uint64_t)sz)
    {
        free(buf);
        return NULL;
    }
    const char *shstr = (const char *)(buf + shstr_off);

/* Section name at shstr+SH_U32(i,0), clamped so strcmp never walks past buf. */
#define SH_NAME(idx)                                            \
    ((SH_U32(idx, 0) < (uint64_t)sz - shstr_off)                \
         ? shstr + SH_U32(idx, 0)                               \
         : "")

    uint32_t dl_off = 0, dl_size = 0;
    uint32_t sym_off = 0, sym_size = 0, str_off = 0, str_size = 0;
    for (uint16_t i = 0; i < e_shnum; i++)
    {
        const char *nm = SH_NAME(i);
        if (strcmp(nm, ".debug_line") == 0)
        {
            dl_off = SH_U32(i, 16);
            dl_size = SH_U32(i, 20);
        }
        else if (strcmp(nm, ".symtab") == 0)
        {
            sym_off = SH_U32(i, 16);
            sym_size = SH_U32(i, 20);
        }
        else if (strcmp(nm, ".strtab") == 0)
        {
            str_off = SH_U32(i, 16);
            str_size = SH_U32(i, 20);
        }
    }
    if (dl_off == 0 || dl_size == 0 || (uint64_t)dl_off + dl_size > (uint64_t)sz)
    {
        free(buf);
        return NULL;
    }

    dwarf_line_t *dl = calloc(1, sizeof *dl);
    if (!dl)
    {
        free(buf);
        return NULL;
    }

    /* Allocatable sections (.text/.data/.bss/.zp/.noinit/...) for the memory-map
     * view: name + 6502 load address + size. SHF_ALLOC (flags bit 1) with a
     * non-zero size; names are interned since buf is freed below. */
    for (uint16_t i = 0; i < e_shnum && dl->nsections < DL_MAX_SECTIONS; i++)
    {
        uint32_t flags = SH_U32(i, 8), saddr = SH_U32(i, 12), ssize = SH_U32(i, 20);
        if (!(flags & 0x2) || ssize == 0)
            continue;
        dl->sections[dl->nsections].name = intern(dl, SH_NAME(i));
        dl->sections[dl->nsections].addr = (uint16_t)saddr;
        dl->sections[dl->nsections].size = ssize;
        dl->nsections++;
    }

    /* Walk the (possibly multiple) units in .debug_line. */
    cur c = {buf + dl_off, buf + dl_off + dl_size, true};
    while (c.p + 4 <= c.end && c.ok)
    {
        const uint8_t *unit_start = c.p;
        uint32_t unit_len = u32(&c);
        if (unit_len == 0 || unit_len == 0xffffffffu)
            break; /* 0 / 64-bit DWARF: stop */
        const uint8_t *unit_end = unit_start + 4 + unit_len;
        if (unit_end > c.end)
            unit_end = c.end;
        cur uc = {c.p, unit_end, true};
        parse_unit(dl, &uc, unit_end);
        c.p = unit_end;
    }
    parse_symbols(dl, buf, sz, sym_off, sym_size, str_off, str_size);
    free(buf);

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

/* The allocatable ELF sections (load address + size), for the memory-map view. */
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
