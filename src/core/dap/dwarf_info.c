/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The abbreviation table and the DIE tree of each 32-bit DWARF5 compilation
 * unit, read into a C type graph, the variables of each unit, and each
 * function's locals with their lexical-scope PC ranges and frame base.
 *
 * The ELF image is freed once parsing finishes, so every string and every
 * expression byte the model keeps is copied onto the dwarf_info_t and a query
 * never reads the file again.
 */

#include "core/dap/dwarf_info.h"
#include "core/dap/dwarf_cursor.h"
#include "core/dap/dwarf_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    DW_TAG_array_type = 0x01,
    DW_TAG_enumeration_type = 0x04,
    DW_TAG_formal_parameter = 0x05,
    DW_TAG_lexical_block = 0x0b,
    DW_TAG_member = 0x0d,
    DW_TAG_pointer_type = 0x0f,
    DW_TAG_compile_unit = 0x11,
    DW_TAG_structure_type = 0x13,
    DW_TAG_subroutine_type = 0x15,
    DW_TAG_typedef = 0x16,
    DW_TAG_union_type = 0x17,
    DW_TAG_base_type = 0x24,
    DW_TAG_const_type = 0x26,
    DW_TAG_enumerator = 0x28,
    DW_TAG_subprogram = 0x2e,
    DW_TAG_subrange_type = 0x21,
    DW_TAG_variable = 0x34,
    DW_TAG_volatile_type = 0x35,
    DW_TAG_restrict_type = 0x37,
};
enum
{
    DW_AT_name = 0x03,
    DW_AT_byte_size = 0x0b,
    DW_AT_low_pc = 0x11,
    DW_AT_high_pc = 0x12,
    DW_AT_location = 0x02,
    DW_AT_const_value = 0x1c,
    DW_AT_upper_bound = 0x2f,
    DW_AT_count = 0x37,
    DW_AT_data_member_location = 0x38,
    DW_AT_encoding = 0x3e,
    DW_AT_frame_base = 0x40,
    DW_AT_type = 0x49,
};
enum
{
    DW_FORM_addr = 0x01,
    DW_FORM_block2 = 0x03,
    DW_FORM_block4 = 0x04,
    DW_FORM_data2 = 0x05,
    DW_FORM_data4 = 0x06,
    DW_FORM_data8 = 0x07,
    DW_FORM_string = 0x08,
    DW_FORM_block = 0x09,
    DW_FORM_block1 = 0x0a,
    DW_FORM_data1 = 0x0b,
    DW_FORM_flag = 0x0c,
    DW_FORM_sdata = 0x0d,
    DW_FORM_strp = 0x0e,
    DW_FORM_udata = 0x0f,
    DW_FORM_ref_addr = 0x10,
    DW_FORM_ref1 = 0x11,
    DW_FORM_ref2 = 0x12,
    DW_FORM_ref4 = 0x13,
    DW_FORM_ref8 = 0x14,
    DW_FORM_ref_udata = 0x15,
    DW_FORM_indirect = 0x16,
    DW_FORM_sec_offset = 0x17,
    DW_FORM_exprloc = 0x18,
    DW_FORM_flag_present = 0x19,
    DW_FORM_data16 = 0x1e,
    DW_FORM_line_strp = 0x1f,
    DW_FORM_strx = 0x1a,
    DW_FORM_addrx = 0x1b,
    DW_FORM_implicit_const = 0x21,
    DW_FORM_loclistx = 0x22,
    DW_FORM_rnglistx = 0x23,
    DW_FORM_strx1 = 0x25,
    DW_FORM_strx2 = 0x26,
    DW_FORM_strx3 = 0x27,
    DW_FORM_strx4 = 0x28,
    DW_FORM_addrx1 = 0x29,
    DW_FORM_addrx2 = 0x2a,
    DW_FORM_addrx3 = 0x2b,
    DW_FORM_addrx4 = 0x2c,
};
enum
{
    DW_AT_str_offsets_base = 0x72,
    DW_AT_addr_base = 0x73,
};
enum
{
    DW_OP_addr = 0x03,
    DW_OP_plus_uconst = 0x23,
    DW_OP_reg0 = 0x50,
    DW_OP_reg31 = 0x6f,
    DW_OP_regx = 0x90,
    DW_OP_fbreg = 0x91,
    DW_OP_addrx = 0xa1,
};
/* The DWARF register numbers llvm-mos gives its 16-bit pointer registers, from
 * MOSRegisterInfo.td. RS0, the first of them, is the soft stack pointer that
 * serves as the frame base. */
enum
{
    DW_MOS_RS0 = 0x30000,
    DW_MOS_IMAG_MAX = 0x80, /* the registers run RS0 to RS127 */
};

struct dtype
{
    int kind; /* a dw_kind_t */
    uint32_t size;
    int encoding;
    char *name;
    struct dtype *inner; /* the pointee of a pointer or the element of an array */
    uint32_t count;
    struct dmember
    {
        char *name;
        uint32_t offset;
        struct dtype *type;
    } *members;
    int nmembers;
    struct denum
    {
        char *name;
        int64_t value;
    } *enums;
    int nenums;
};

typedef struct
{
    char *name;
    uint16_t addr;
    struct dtype *type;
    bool addr_ok;
} gvar_t;

typedef struct
{
    char *name;
    struct dtype *type;
    uint8_t *loc;
    uint32_t loc_len;
    uint32_t lo, hi; /* the scope's PC range: an enclosing block, else the function */
} lvar_t;

typedef struct
{
    uint32_t lo, hi;
    uint8_t *fb; /* the DW_AT_frame_base expression */
    uint32_t fb_len;
    lvar_t *locals;
    int nlocals;
} func_t;

typedef struct
{
    char *name;
    uint16_t addr;
} sym_t;

struct dwarf_info
{
    char **strs;
    size_t nstrs;
    struct dtype **types; /* every dtype allocated, so that free can find them all */
    size_t ntypes;
    gvar_t *globals;
    int nglobals;
    func_t *funcs;
    int nfuncs;
    sym_t *syms; /* STT_OBJECT, the fallback for a global with no location */
    int nsyms;
    /* The zero-page address the linker gave each imaginary register, taken from
     * its absolute __rcN symbol. rc_addr[n] holds only when rc_known[n]. */
    uint16_t rc_addr[256];
    bool rc_known[256];
};

/* A query reads the stored expression bytes with no cursor around them, so
 * LEB128 is decoded over a plain byte span here. */
static uint64_t uleb_raw(const uint8_t *p, const uint8_t *end, const uint8_t **out)
{
    uint64_t v = 0;
    int shift = 0;
    while (p < end)
    {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) break;
    }
    if (out) *out = p;
    return v;
}
static int64_t sleb_raw(const uint8_t *p, const uint8_t *end)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    while (p < end)
    {
        b = *p++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
        /* Shifting a 64-bit value by 64 or more is undefined, so a malformed
         * LEB128 that never terminates stops here. */
        if (shift > 63) break;
    }
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}

static char *intern(dwarf_info_t *di, const char *s)
{
    char *dup = strdup(s ? s : "");
    if (!dup) return (char *)"";
    char **ns = realloc(di->strs, (di->nstrs + 1) * sizeof(char *));
    if (!ns) { free(dup); return (char *)""; }
    di->strs = ns;
    di->strs[di->nstrs++] = dup;
    return dup;
}
static struct dtype *new_dtype(dwarf_info_t *di)
{
    struct dtype *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    struct dtype **nt = realloc(di->types, (di->ntypes + 1) * sizeof(struct dtype *));
    if (!nt) { free(t); return NULL; }
    di->types = nt;
    di->types[di->ntypes++] = t;
    return t;
}

typedef struct
{
    uint16_t attr;
    uint16_t form;
    int64_t implicit; /* a DW_FORM_implicit_const carries its value in the abbrev */
} ab_attr;
typedef struct
{
    uint32_t code;
    uint16_t tag;
    bool has_children;
    ab_attr *attrs;
    int nattrs;
} abbrev;
typedef struct
{
    abbrev *list;
    int n;
} abbrev_tab;

static void abbrev_free(abbrev_tab *t)
{
    for (int i = 0; i < t->n; i++)
        free(t->list[i].attrs);
    free(t->list);
    t->list = NULL;
    t->n = 0;
}
static const abbrev *abbrev_find(const abbrev_tab *t, uint32_t code)
{
    for (int i = 0; i < t->n; i++)
        if (t->list[i].code == code)
            return &t->list[i];
    return NULL;
}
static void abbrev_parse(abbrev_tab *t, const uint8_t *base, const uint8_t *end, uint32_t off)
{
    if (base > end || off > (uint32_t)(end - base))
        return;
    dwarf_cur c = {base + off, end, true};
    for (;;)
    {
        uint32_t code = (uint32_t)dwarf_uleb(&c);
        if (!c.ok || code == 0)
            break;
        uint16_t tag = (uint16_t)dwarf_uleb(&c);
        uint8_t children = dwarf_u8(&c);
        ab_attr *attrs = NULL;
        int n = 0;
        for (;;)
        {
            uint16_t at = (uint16_t)dwarf_uleb(&c);
            uint16_t fm = (uint16_t)dwarf_uleb(&c);
            int64_t imp = 0;
            if (fm == DW_FORM_implicit_const)
                imp = dwarf_sleb(&c);
            if (!c.ok || (at == 0 && fm == 0))
                break;
            ab_attr *na = realloc(attrs, (n + 1) * sizeof(ab_attr));
            if (!na) break;
            attrs = na;
            attrs[n].attr = at;
            attrs[n].form = fm;
            attrs[n].implicit = imp;
            n++;
        }
        abbrev *nl = realloc(t->list, (t->n + 1) * sizeof(abbrev));
        if (!nl) { free(attrs); break; }
        t->list = nl;
        t->list[t->n].code = code;
        t->list[t->n].tag = tag;
        t->list[t->n].has_children = children != 0;
        t->list[t->n].attrs = attrs;
        t->list[t->n].nattrs = n;
        t->n++;
    }
}

/* One DIE, alive only while its compilation unit is being parsed. */
typedef struct
{
    uint32_t off; /* of this DIE within .debug_info */
    uint16_t tag;
    int parent;
    char *name;
    uint32_t type_ref; /* an absolute .debug_info offset, 0 if none */
    bool has_type;
    const uint8_t *loc;
    uint32_t loc_len;
    const uint8_t *fb;
    uint32_t fb_len;
    uint32_t low_pc;
    bool has_low;
    uint32_t high_pc;
    bool has_high;
    bool high_is_addr;
    uint32_t byte_size;
    bool has_byte_size;
    int encoding;
    uint64_t count;
    bool has_count;
    uint32_t member_off;
    int64_t const_value;
    bool has_const_value;
} die_t;

typedef struct
{
    die_t *dies;
    int ndies;
    struct dtype **memo; /* the type built for each DIE, NULL until it is built */
    dwarf_info_t *di;
} cu_ctx;

typedef struct
{
    int kind;
    uint64_t u;
    int64_t s;
    const char *str;
    const uint8_t *block;
    uint32_t blen;
} formval;
enum { FV_U, FV_I, FV_STR, FV_BLOCK, FV_REF };

typedef struct
{
    uint8_t addr_size;
    uint32_t cu_off;                          /* the ref forms are relative to the CU header */
    const char *dstr; uint32_t dstr_size;     /* .debug_str */
    const char *dlstr; uint32_t dlstr_size;   /* .debug_line_str */
    const uint8_t *soff; uint32_t soff_size;  /* .debug_str_offsets */
    const uint8_t *daddr; uint32_t daddr_size;/* .debug_addr */
    uint32_t str_offsets_base;                /* byte offset of this CU's slice */
    uint32_t addr_base;                       /* byte offset of this CU's slice */
} read_ctx;

static uint32_t read_ux(dwarf_cur *c, int nbytes)
{
    uint32_t v = 0;
    for (int i = 0; i < nbytes; i++)
        v |= (uint32_t)dwarf_u8(c) << (8 * i);
    return v;
}
/* A strx form is an index into this CU's slice of .debug_str_offsets, and the
 * four-byte entry found there is an offset into .debug_str. */
static const char *resolve_strx(const read_ctx *rc, uint64_t idx)
{
    uint64_t off = (uint64_t)rc->str_offsets_base + idx * 4;
    if (!rc->soff || off + 4 > rc->soff_size)
        return "";
    const uint8_t *p = rc->soff + off;
    uint32_t so = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    return (rc->dstr && so < rc->dstr_size) ? rc->dstr + so : "";
}
/* An addrx form is an index into this CU's slice of .debug_addr, whose entries
 * are one address each. */
static uint32_t resolve_addrx(const read_ctx *rc, uint64_t idx)
{
    uint64_t off = (uint64_t)rc->addr_base + idx * rc->addr_size;
    if (!rc->daddr || off + rc->addr_size > rc->daddr_size)
        return 0;
    const uint8_t *p = rc->daddr + off;
    uint32_t a = 0;
    for (uint32_t i = 0; i < rc->addr_size && i < 4; i++)
        a |= (uint32_t)p[i] << (8 * i);
    return a;
}

/* implicit is the value the abbreviation carries for DW_FORM_implicit_const and
 * is ignored for every other form. */
static void read_form(dwarf_cur *c, uint16_t form, int64_t implicit, const read_ctx *rc, formval *v)
{
    memset(v, 0, sizeof *v);
    switch (form)
    {
    case DW_FORM_addr:
        v->kind = FV_U;
        v->u = (rc->addr_size == 8) ? dwarf_u64(c) : dwarf_u32(c);
        break;
    case DW_FORM_data1: v->kind = FV_U; v->u = dwarf_u8(c); break;
    case DW_FORM_data2: v->kind = FV_U; v->u = dwarf_u16(c); break;
    case DW_FORM_data4: v->kind = FV_U; v->u = dwarf_u32(c); break;
    case DW_FORM_data8: v->kind = FV_U; v->u = dwarf_u64(c); break;
    case DW_FORM_sdata: v->kind = FV_I; v->s = dwarf_sleb(c); break;
    case DW_FORM_udata: v->kind = FV_U; v->u = dwarf_uleb(c); break;
    case DW_FORM_flag: v->kind = FV_U; v->u = dwarf_u8(c); break;
    case DW_FORM_flag_present: v->kind = FV_U; v->u = 1; break;
    case DW_FORM_implicit_const: v->kind = FV_I; v->s = implicit; break;
    case DW_FORM_sec_offset: v->kind = FV_U; v->u = dwarf_u32(c); break;
    case DW_FORM_loclistx:
    case DW_FORM_rnglistx: v->kind = FV_U; v->u = dwarf_uleb(c); break;
    case DW_FORM_strp:
    {
        uint32_t o = dwarf_u32(c);
        v->kind = FV_STR;
        v->str = (rc->dstr && o < rc->dstr_size) ? rc->dstr + o : "";
        break;
    }
    case DW_FORM_line_strp:
    {
        uint32_t o = dwarf_u32(c);
        v->kind = FV_STR;
        v->str = (rc->dlstr && o < rc->dlstr_size) ? rc->dlstr + o : "";
        break;
    }
    case DW_FORM_strx:
    case DW_FORM_strx1:
    case DW_FORM_strx2:
    case DW_FORM_strx3:
    case DW_FORM_strx4:
    {
        uint64_t idx = (form == DW_FORM_strx1) ? read_ux(c, 1)
                       : (form == DW_FORM_strx2) ? read_ux(c, 2)
                       : (form == DW_FORM_strx3) ? read_ux(c, 3)
                       : (form == DW_FORM_strx4) ? read_ux(c, 4)
                                                 : dwarf_uleb(c);
        v->kind = FV_STR;
        v->str = resolve_strx(rc, idx);
        break;
    }
    case DW_FORM_addrx:
    case DW_FORM_addrx1:
    case DW_FORM_addrx2:
    case DW_FORM_addrx3:
    case DW_FORM_addrx4:
    {
        uint64_t idx = (form == DW_FORM_addrx1) ? read_ux(c, 1)
                       : (form == DW_FORM_addrx2) ? read_ux(c, 2)
                       : (form == DW_FORM_addrx3) ? read_ux(c, 3)
                       : (form == DW_FORM_addrx4) ? read_ux(c, 4)
                                                  : dwarf_uleb(c);
        v->kind = FV_U;
        v->u = resolve_addrx(rc, idx);
        break;
    }
    case DW_FORM_string:
    {
        v->kind = FV_STR;
        v->str = (const char *)c->p;
        while (c->p < c->end && *c->p) c->p++;
        if (c->p < c->end) c->p++;
        break;
    }
    case DW_FORM_ref1: v->kind = FV_REF; v->u = rc->cu_off + dwarf_u8(c); break;
    case DW_FORM_ref2: v->kind = FV_REF; v->u = rc->cu_off + dwarf_u16(c); break;
    case DW_FORM_ref4: v->kind = FV_REF; v->u = rc->cu_off + dwarf_u32(c); break;
    case DW_FORM_ref8: v->kind = FV_REF; v->u = rc->cu_off + dwarf_u64(c); break;
    case DW_FORM_ref_udata: v->kind = FV_REF; v->u = rc->cu_off + dwarf_uleb(c); break;
    case DW_FORM_ref_addr: v->kind = FV_REF; v->u = dwarf_u32(c); break;
    case DW_FORM_exprloc:
    {
        uint64_t n = dwarf_uleb(c);
        if (n > (uint64_t)(c->end - c->p)) { c->ok = false; break; }
        v->kind = FV_BLOCK;
        v->block = c->p;
        v->blen = (uint32_t)n;
        c->p += n;
        break;
    }
    case DW_FORM_block1:
    case DW_FORM_block2:
    case DW_FORM_block4:
    case DW_FORM_block:
    {
        uint64_t n = (form == DW_FORM_block1) ? dwarf_u8(c)
                     : (form == DW_FORM_block2) ? dwarf_u16(c)
                     : (form == DW_FORM_block4) ? dwarf_u32(c)
                                                : dwarf_uleb(c);
        if (n > (uint64_t)(c->end - c->p)) { c->ok = false; break; }
        v->kind = FV_BLOCK;
        v->block = c->p;
        v->blen = (uint32_t)n;
        c->p += n;
        break;
    }
    case DW_FORM_data16:
        if ((uint64_t)16 > (uint64_t)(c->end - c->p)) { c->ok = false; break; }
        v->kind = FV_BLOCK;
        v->block = c->p;
        v->blen = 16;
        c->p += 16;
        break;
    default:
        /* An unknown form has no known size, so nothing after it can be read. */
        c->ok = false;
        break;
    }
}

/* The two static location expressions llvm-mos emits are DW_OP_addr with
 * addr_size bytes and DW_OP_addrx with a ULEB index, either one optionally
 * followed by DW_OP_plus_uconst. The four-byte MOS address is truncated to
 * sixteen bits because the rp6502 is unbanked. */
static bool resolve_static_loc(const uint8_t *loc, uint32_t len, const read_ctx *rc, uint16_t *out)
{
    if (!loc || len < 1)
        return false;
    const uint8_t *end = loc + len;
    const uint8_t *p;
    uint32_t addr;
    if (loc[0] == DW_OP_addr)
    {
        addr = 0;
        for (uint32_t k = 0; 1 + k < len && k < 4; k++)
            addr |= (uint32_t)loc[1 + k] << (8 * k);
        p = loc + 1 + rc->addr_size;
    }
    else if (loc[0] == DW_OP_addrx)
    {
        uint64_t idx = uleb_raw(loc + 1, end, &p);
        addr = resolve_addrx(rc, idx);
    }
    else
        return false;
    if (p < end && *p == DW_OP_plus_uconst)
        addr += (uint32_t)uleb_raw(p + 1, end, NULL);
    *out = (uint16_t)addr;
    return true;
}

static int die_index_by_off(const cu_ctx *cu, uint32_t off)
{
    /* The DIEs were appended in increasing offset order, so a binary search
     * over off is sound. */
    int lo = 0, hi = cu->ndies;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (cu->dies[mid].off < off) lo = mid + 1;
        else hi = mid;
    }
    if (lo < cu->ndies && cu->dies[lo].off == off)
        return lo;
    return -1;
}

static struct dtype *void_type(dwarf_info_t *di)
{
    struct dtype *t = new_dtype(di);
    if (t) { t->kind = DW_KIND_VOID; t->name = (char *)"void"; }
    return t;
}

static struct dtype *build_type(cu_ctx *cu, int idx, int depth);

static uint32_t array_count(cu_ctx *cu, int arr_idx)
{
    /* A multi-dimensional array is one DIE with a subrange child per dimension,
     * so the element count is the product of them all. */
    uint32_t total = 0;
    for (int i = 0; i < cu->ndies; i++)
    {
        if (cu->dies[i].parent != arr_idx) continue;
        if (cu->dies[i].tag != DW_TAG_subrange_type) continue;
        uint32_t m = (cu->dies[i].has_count ? (uint32_t)cu->dies[i].count : 0);
        if (!m) m = 1;
        uint64_t prod = total ? (uint64_t)total * m : m;
        total = prod > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)prod;
    }
    return total; /* zero only when the DIE has no subrange child at all */
}

static struct dtype *build_type(cu_ctx *cu, int idx, int depth)
{
    if (idx < 0 || idx >= cu->ndies)
        return NULL;
    if (cu->memo[idx])
        return cu->memo[idx];
    dwarf_info_t *di = cu->di;
    /* The memo below guards against a cycle but not against depth, so a long
     * chain of types would exhaust the C stack without this bound. */
    if (depth > 256)
        return void_type(di);
    die_t *d = &cu->dies[idx];

    /* A typedef or a qualifier maps straight through to the type underneath,
     * whose kind and members are what formatting a value needs. The name of the
     * typedef is lost. */
    if (d->tag == DW_TAG_typedef || d->tag == DW_TAG_const_type ||
        d->tag == DW_TAG_volatile_type || d->tag == DW_TAG_restrict_type)
    {
        cu->memo[idx] = void_type(di); /* a placeholder, so a cycle terminates */
        struct dtype *under = d->has_type ? build_type(cu, die_index_by_off(cu, d->type_ref), depth + 1)
                                          : void_type(di);
        cu->memo[idx] = under;
        return under;
    }

    struct dtype *t = new_dtype(di);
    if (!t) return NULL;
    cu->memo[idx] = t; /* memoized before recursing, so a cycle terminates */

    switch (d->tag)
    {
    case DW_TAG_base_type:
        t->kind = DW_KIND_BASE;
        t->size = d->has_byte_size ? d->byte_size : 0;
        t->encoding = d->encoding;
        t->name = d->name ? d->name : (char *)"?";
        break;
    case DW_TAG_pointer_type:
    {
        t->kind = DW_KIND_POINTER;
        t->size = d->has_byte_size ? d->byte_size : 2;
        t->inner = d->has_type ? build_type(cu, die_index_by_off(cu, d->type_ref), depth + 1) : NULL;
        const char *pt = t->inner && t->inner->name ? t->inner->name : "void";
        char buf[160];
        snprintf(buf, sizeof buf, "%s *", pt);
        t->name = intern(di, buf);
        break;
    }
    case DW_TAG_array_type:
    {
        t->kind = DW_KIND_ARRAY;
        t->inner = d->has_type ? build_type(cu, die_index_by_off(cu, d->type_ref), depth + 1) : NULL;
        t->count = array_count(cu, idx);
        uint64_t asz = (uint64_t)(t->inner ? t->inner->size : 0) * t->count;
        t->size = asz > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)asz;
        const char *et = t->inner && t->inner->name ? t->inner->name : "?";
        char buf[160];
        if (t->count)
            snprintf(buf, sizeof buf, "%s [%u]", et, t->count);
        else
            snprintf(buf, sizeof buf, "%s []", et);
        t->name = intern(di, buf);
        break;
    }
    case DW_TAG_structure_type:
    case DW_TAG_union_type:
    {
        bool uni = d->tag == DW_TAG_union_type;
        t->kind = uni ? DW_KIND_UNION : DW_KIND_STRUCT;
        t->size = d->has_byte_size ? d->byte_size : 0;
        char buf[160];
        snprintf(buf, sizeof buf, "%s %s", uni ? "union" : "struct", d->name ? d->name : "");
        t->name = intern(di, buf);
        for (int i = 0; i < cu->ndies; i++)
        {
            if (cu->dies[i].parent != idx || cu->dies[i].tag != DW_TAG_member)
                continue;
            struct dmember *nm = realloc(t->members, (t->nmembers + 1) * sizeof *nm);
            if (!nm) break;
            t->members = nm;
            t->members[t->nmembers].name = cu->dies[i].name ? cu->dies[i].name : (char *)"";
            t->members[t->nmembers].offset = cu->dies[i].member_off;
            t->members[t->nmembers].type =
                cu->dies[i].has_type ? build_type(cu, die_index_by_off(cu, cu->dies[i].type_ref), depth + 1) : NULL;
            t->nmembers++;
        }
        break;
    }
    case DW_TAG_enumeration_type:
    {
        t->kind = DW_KIND_ENUM;
        t->size = d->has_byte_size ? d->byte_size : 2;
        char buf[160];
        snprintf(buf, sizeof buf, "enum %s", d->name ? d->name : "");
        t->name = intern(di, buf);
        for (int i = 0; i < cu->ndies; i++)
        {
            if (cu->dies[i].parent != idx || cu->dies[i].tag != DW_TAG_enumerator)
                continue;
            struct denum *ne = realloc(t->enums, (t->nenums + 1) * sizeof *ne);
            if (!ne) break;
            t->enums = ne;
            t->enums[t->nenums].name = cu->dies[i].name ? cu->dies[i].name : (char *)"";
            t->enums[t->nenums].value = cu->dies[i].const_value;
            t->nenums++;
        }
        break;
    }
    case DW_TAG_subroutine_type:
        t->kind = DW_KIND_FUNC;
        t->size = 2;
        t->name = (char *)"()";
        break;
    default:
        t->kind = DW_KIND_UNKNOWN;
        t->name = d->name ? d->name : (char *)"?";
        break;
    }
    return t;
}

static void parse_cu(dwarf_info_t *di, const uint8_t *info,
                     const uint8_t *cu_data, const uint8_t *cu_end,
                     const abbrev_tab *ab, read_ctx rc)
{
    cu_ctx cu;
    memset(&cu, 0, sizeof cu);
    cu.di = di;

    dwarf_cur c = {cu_data, cu_end, true};
    int stack[64];
    const int stack_max = (int)(sizeof stack / sizeof stack[0]);
    int depth = 0;
    int current_parent = -1;

    while (c.p < cu_end && c.ok)
    {
        uint32_t off = (uint32_t)(c.p - info);
        uint32_t code = (uint32_t)dwarf_uleb(&c);
        if (code == 0)
        {
            if (depth == 0) break;
            /* The depth counts every level, including those past the end of
             * stack, because clamping a push without clamping its pop would put
             * the rest of the unit under the wrong parents. Only a level still
             * within stack restores an exact parent. */
            depth--;
            if (depth < stack_max)
                current_parent = stack[depth];
            continue;
        }
        const abbrev *a = abbrev_find(ab, code);
        if (!a)
        {
            c.ok = false;
            break;
        }

        die_t d;
        memset(&d, 0, sizeof d);
        d.off = off;
        d.tag = a->tag;
        d.parent = current_parent;

        for (int i = 0; i < a->nattrs && c.ok; i++)
        {
            uint16_t at = a->attrs[i].attr;
            uint16_t fm = a->attrs[i].form;
            int64_t implicit = a->attrs[i].implicit;
            if (fm == DW_FORM_indirect)
                fm = (uint16_t)dwarf_uleb(&c);
            formval v;
            read_form(&c, fm, implicit, &rc, &v);
            if (!c.ok) break;
            switch (at)
            {
            case DW_AT_str_offsets_base:
                rc.str_offsets_base = (uint32_t)v.u; /* it governs the DIEs that follow */
                break;
            case DW_AT_addr_base:
                rc.addr_base = (uint32_t)v.u;
                break;
            case DW_AT_name:
                if (v.kind == FV_STR) d.name = intern(di, v.str);
                break;
            case DW_AT_type:
                d.type_ref = (uint32_t)v.u;
                d.has_type = true;
                break;
            case DW_AT_location:
                if (v.kind == FV_BLOCK) { d.loc = v.block; d.loc_len = v.blen; }
                break;
            case DW_AT_frame_base:
                if (v.kind == FV_BLOCK) { d.fb = v.block; d.fb_len = v.blen; }
                break;
            case DW_AT_low_pc:
                d.low_pc = (uint32_t)v.u;
                d.has_low = true;
                break;
            case DW_AT_high_pc:
                d.high_pc = (uint32_t)v.u;
                d.has_high = true;
                d.high_is_addr = (fm == DW_FORM_addr);
                break;
            case DW_AT_byte_size:
                d.byte_size = (uint32_t)v.u;
                d.has_byte_size = true;
                break;
            case DW_AT_encoding:
                d.encoding = (int)v.u;
                break;
            case DW_AT_count:
                d.count = v.u;
                d.has_count = true;
                break;
            case DW_AT_upper_bound:
                if (!d.has_count)
                {
                    int64_t ub = (v.kind == FV_I) ? v.s : (int64_t)v.u;
                    if (ub >= 0) { d.count = (uint64_t)ub + 1; d.has_count = true; }
                    /* A negative upper bound marks a flexible array, so the
                     * count stays unset. */
                }
                break;
            case DW_AT_data_member_location:
                if (v.kind == FV_BLOCK)
                {
                    if (v.blen >= 1 && v.block[0] == DW_OP_plus_uconst)
                        d.member_off = (uint32_t)uleb_raw(v.block + 1, v.block + v.blen, NULL);
                }
                else
                    d.member_off = (v.kind == FV_I) ? (uint32_t)v.s : (uint32_t)v.u;
                break;
            case DW_AT_const_value:
                d.const_value = (v.kind == FV_I) ? v.s : (int64_t)v.u;
                d.has_const_value = true;
                break;
            default:
                break;
            }
        }
        if (!c.ok)
            break;

        die_t *nd = realloc(cu.dies, (cu.ndies + 1) * sizeof(die_t));
        if (!nd) break;
        cu.dies = nd;
        cu.dies[cu.ndies] = d;
        int idx = cu.ndies;
        cu.ndies++;

        if (a->has_children)
        {
            if (depth < stack_max)
                stack[depth] = current_parent;
            depth++;
            current_parent = idx;
        }
    }

    if (cu.ndies == 0)
        return;

    cu.memo = calloc(cu.ndies, sizeof(struct dtype *));
    if (!cu.memo) { free(cu.dies); return; }

    int cu_root = -1;
    for (int i = 0; i < cu.ndies; i++)
        if (cu.dies[i].tag == DW_TAG_compile_unit) { cu_root = i; break; }

    for (int i = 0; i < cu.ndies; i++)
    {
        die_t *d = &cu.dies[i];
        if (d->tag != DW_TAG_variable || d->parent != cu_root || !d->name)
            continue;
        gvar_t g;
        memset(&g, 0, sizeof g);
        g.name = d->name;
        g.type = d->has_type ? build_type(&cu, die_index_by_off(&cu, d->type_ref), 0) : NULL;
        uint16_t gabs;
        if (resolve_static_loc(d->loc, d->loc_len, &rc, &gabs))
        {
            g.addr = gabs;
            g.addr_ok = true;
        }
        else
        {
            /* A DW_TAG_variable with no DW_AT_location: an extern
             * declaration, or a tentative definition the compiler left
             * unplaced. Its address comes from .symtab instead. */
            for (int s = 0; s < di->nsyms; s++)
                if (strcmp(di->syms[s].name, d->name) == 0)
                {
                    g.addr = di->syms[s].addr;
                    g.addr_ok = true;
                    break;
                }
        }
        gvar_t *ng = realloc(di->globals, (di->nglobals + 1) * sizeof(gvar_t));
        if (!ng) break;
        di->globals = ng;
        di->globals[di->nglobals++] = g;
    }

    for (int i = 0; i < cu.ndies; i++)
    {
        die_t *sp = &cu.dies[i];
        if (sp->tag != DW_TAG_subprogram || !sp->has_low || !sp->has_high)
            continue;
        func_t fn;
        memset(&fn, 0, sizeof fn);
        fn.lo = sp->low_pc;
        fn.hi = sp->high_is_addr ? sp->high_pc : sp->low_pc + sp->high_pc;
        if (sp->fb && sp->fb_len)
        {
            fn.fb = malloc(sp->fb_len);
            if (fn.fb) { memcpy(fn.fb, sp->fb, sp->fb_len); fn.fb_len = sp->fb_len; }
        }
        for (int j = 0; j < cu.ndies; j++)
        {
            die_t *v = &cu.dies[j];
            if (v->tag != DW_TAG_variable && v->tag != DW_TAG_formal_parameter)
                continue;
            if (!v->name || !v->loc || !v->loc_len)
                continue;
            int p = v->parent;
            bool desc = false;
            while (p >= 0)
            {
                if (p == i) { desc = true; break; }
                p = cu.dies[p].parent;
            }
            if (!desc)
                continue;
            /* The scope is the nearest enclosing DIE that carries a PC range,
             * and the function itself when no block does. */
            uint32_t lo = fn.lo, hi = fn.hi;
            for (int q = v->parent; q >= 0; q = cu.dies[q].parent)
            {
                if (cu.dies[q].has_low && cu.dies[q].has_high)
                {
                    lo = cu.dies[q].low_pc;
                    hi = cu.dies[q].high_is_addr ? cu.dies[q].high_pc
                                                 : cu.dies[q].low_pc + cu.dies[q].high_pc;
                    break;
                }
                if (q == i) break;
            }
            lvar_t lv;
            memset(&lv, 0, sizeof lv);
            lv.name = v->name;
            lv.type = v->has_type ? build_type(&cu, die_index_by_off(&cu, v->type_ref), 0) : NULL;
            /* An addrx location has to become an absolute DW_OP_addr here,
             * while this unit's .debug_addr slice is still known; a query has no
             * unit context to resolve it with. */
            uint16_t sabs;
            if (v->loc[0] == DW_OP_addrx && resolve_static_loc(v->loc, v->loc_len, &rc, &sabs))
            {
                lv.loc = malloc(3);
                if (!lv.loc) continue;
                lv.loc[0] = DW_OP_addr;
                lv.loc[1] = (uint8_t)(sabs & 0xff);
                lv.loc[2] = (uint8_t)(sabs >> 8);
                lv.loc_len = 3;
            }
            else
            {
                lv.loc = malloc(v->loc_len);
                if (!lv.loc) continue;
                memcpy(lv.loc, v->loc, v->loc_len);
                lv.loc_len = v->loc_len;
            }
            lv.lo = lo;
            lv.hi = hi;
            lvar_t *nl = realloc(fn.locals, (fn.nlocals + 1) * sizeof(lvar_t));
            if (!nl) { free(lv.loc); break; }
            fn.locals = nl;
            fn.locals[fn.nlocals++] = lv;
        }
        func_t *nf = realloc(di->funcs, (di->nfuncs + 1) * sizeof(func_t));
        if (!nf) { free(fn.fb); free(fn.locals); break; }
        di->funcs = nf;
        di->funcs[di->nfuncs++] = fn;
    }

    free(cu.memo);
    free(cu.dies);
}

static void parse_objects(dwarf_info_t *di, const uint8_t *buf, long sz,
                          uint32_t sym_off, uint32_t sym_size,
                          uint32_t str_off, uint32_t str_size)
{
    if (!sym_off || !str_off) return;
    if ((uint64_t)sym_off + sym_size > (uint64_t)sz ||
        (uint64_t)str_off + str_size > (uint64_t)sz)
        return;
    const char *strtab = (const char *)(buf + str_off);
    for (uint32_t o = 0; o + 16 <= sym_size; o += 16)
    {
        const uint8_t *s = buf + sym_off + o;
        uint32_t st_name = s[0] | (s[1] << 8) | (s[2] << 16) | ((uint32_t)s[3] << 24);
        uint32_t st_value = s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24);
        uint8_t st_info = s[12];
        if (st_name >= str_size)
            continue;
        const char *nm = strtab + st_name;
        if (!nm[0])
            continue;
        /* The absolute __rcN symbols, of whatever type, give the zero-page
         * address of each imaginary register. The 16-bit register RSn is the
         * pair rc[2n] and rc[2n+1]. */
        if (nm[0] == '_' && nm[1] == '_' && nm[2] == 'r' && nm[3] == 'c' && nm[4])
        {
            char *endp;
            long n = strtol(nm + 4, &endp, 10);
            if (*endp == 0 && n >= 0 && n < 256)
            {
                di->rc_addr[n] = (uint16_t)st_value;
                di->rc_known[n] = true;
            }
            continue;
        }
        if ((st_info & 0xf) != 1 /*STT_OBJECT*/)
            continue;
        sym_t *ns = realloc(di->syms, (di->nsyms + 1) * sizeof(sym_t));
        if (!ns) break;
        di->syms = ns;
        di->syms[di->nsyms].name = intern(di, nm);
        di->syms[di->nsyms].addr = (uint16_t)st_value;
        di->nsyms++;
    }
}

dwarf_info_t *dwarf_info_load(const char *elf_path)
{
    elf_image im;
    if (!elf_open(elf_path, &im))
        return NULL;

    uint32_t info_off = 0, info_size = 0, abbrev_off = 0, abbrev_size = 0;
    uint32_t str_off = 0, str_size = 0, lstr_off = 0, lstr_size = 0;
    uint32_t soff_off = 0, soff_size = 0, addr_off = 0, addr_size_sec = 0;
    uint32_t sym_off = 0, sym_size = 0, symstr_off = 0, symstr_size = 0;
    elf_find_section(&im, ".debug_info", &info_off, &info_size);
    elf_find_section(&im, ".debug_abbrev", &abbrev_off, &abbrev_size);
    elf_find_section(&im, ".debug_str", &str_off, &str_size);
    elf_find_section(&im, ".debug_line_str", &lstr_off, &lstr_size);
    elf_find_section(&im, ".debug_str_offsets", &soff_off, &soff_size);
    elf_find_section(&im, ".debug_addr", &addr_off, &addr_size_sec);
    elf_find_section(&im, ".symtab", &sym_off, &sym_size);
    elf_find_section(&im, ".strtab", &symstr_off, &symstr_size);
    if (!info_off || !info_size || !abbrev_off ||
        (uint64_t)info_off + info_size > (uint64_t)im.size ||
        (uint64_t)abbrev_off + abbrev_size > (uint64_t)im.size)
    {
        elf_close(&im);
        return NULL;
    }
    /* read_form bounds a strp or line_strp offset against the section size
     * alone, so a section that runs past the end of the file would let one read
     * outside the image. Dropping such a section makes those forms resolve to
     * the empty string instead. */
    if (str_off && (uint64_t)str_off + str_size > (uint64_t)im.size)
        str_off = str_size = 0;
    if (lstr_off && (uint64_t)lstr_off + lstr_size > (uint64_t)im.size)
        lstr_off = lstr_size = 0;
    if (soff_off && (uint64_t)soff_off + soff_size > (uint64_t)im.size)
        soff_off = soff_size = 0;
    if (addr_off && (uint64_t)addr_off + addr_size_sec > (uint64_t)im.size)
        addr_off = addr_size_sec = 0;

    dwarf_info_t *di = calloc(1, sizeof *di);
    if (!di) { elf_close(&im); return NULL; }

    parse_objects(di, im.buf, im.size, sym_off, sym_size, symstr_off, symstr_size);

    const char *dstr = str_off ? (const char *)(im.buf + str_off) : "";
    const char *dlstr = lstr_off ? (const char *)(im.buf + lstr_off) : NULL;
    const uint8_t *soff = soff_off ? im.buf + soff_off : NULL;
    const uint8_t *daddr = addr_off ? im.buf + addr_off : NULL;
    const uint8_t *info = im.buf + info_off;
    const uint8_t *info_end = info + info_size;
    const uint8_t *ab_base = im.buf + abbrev_off;
    const uint8_t *ab_end = ab_base + abbrev_size;

    dwarf_cur c = {info, info_end, true};
    while (c.p + 4 <= info_end && c.ok)
    {
        const uint8_t *unit_start = c.p;
        uint32_t cu_off = (uint32_t)(unit_start - info);
        uint32_t unit_len = dwarf_u32(&c);
        if (unit_len == 0 || unit_len == 0xffffffffu)
            break;
        const uint8_t *unit_end = unit_start + 4 + unit_len;
        if (unit_end > info_end) unit_end = info_end;

        uint16_t version = dwarf_u16(&c);
        if (version != 5) /* version 5 is what the llvm-mos debug fork emits */
        {
            c.p = unit_end;
            continue;
        }
        /* A version 5 header continues with unit_type, address_size, and the
         * offset of this unit's abbreviations. */
        uint8_t unit_type = dwarf_u8(&c);
        uint8_t addr_size = dwarf_u8(&c);
        uint32_t ab_off = dwarf_u32(&c);
        if (unit_type != 0x01 /*DW_UT_compile*/)
        {
            c.p = unit_end; /* a skeleton or split unit has further header fields */
            continue;
        }
        if (!c.ok) break;

        abbrev_tab ab;
        memset(&ab, 0, sizeof ab);
        abbrev_parse(&ab, ab_base, ab_end, ab_off);

        read_ctx rc;
        memset(&rc, 0, sizeof rc);
        rc.addr_size = addr_size;
        rc.cu_off = cu_off;
        rc.dstr = dstr; rc.dstr_size = str_size;
        rc.dlstr = dlstr; rc.dlstr_size = lstr_size;
        rc.soff = soff; rc.soff_size = soff_size;
        rc.daddr = daddr; rc.daddr_size = addr_size_sec;
        /* Without an explicit base, a CU's slice starts just past the eight-byte
         * section header. */
        rc.str_offsets_base = 8;
        rc.addr_base = 8;
        parse_cu(di, info, c.p, unit_end, &ab, rc);
        abbrev_free(&ab);

        c.p = unit_end;
    }

    elf_close(&im);

    if (di->nglobals == 0 && di->nfuncs == 0)
    {
        dwarf_info_free(di);
        return NULL;
    }
    return di;
}

void dwarf_info_free(dwarf_info_t *di)
{
    if (!di) return;
    for (size_t i = 0; i < di->nstrs; i++)
        free(di->strs[i]);
    free(di->strs);
    for (size_t i = 0; i < di->ntypes; i++)
    {
        free(di->types[i]->members);
        free(di->types[i]->enums);
        free(di->types[i]);
    }
    free(di->types);
    free(di->globals);
    for (int i = 0; i < di->nfuncs; i++)
    {
        free(di->funcs[i].fb);
        for (int j = 0; j < di->funcs[i].nlocals; j++)
            free(di->funcs[i].locals[j].loc);
        free(di->funcs[i].locals);
    }
    free(di->funcs);
    free(di->syms);
    free(di);
}

int dwarf_info_globals(const dwarf_info_t *di, dwarf_var_t *out, int max)
{
    if (!di) return 0;
    int n = 0;
    for (int i = 0; i < di->nglobals && n < max; i++)
    {
        out[n].name = di->globals[i].name;
        out[n].addr = di->globals[i].addr;
        out[n].type = di->globals[i].type;
        out[n].addr_ok = di->globals[i].addr_ok;
        n++;
    }
    return n;
}

/* llvm-mos writes DW_AT_frame_base as DW_OP_regx RSn; the short forms
 * DW_OP_reg0 through DW_OP_reg31 are also accepted and all read as RS0. RSn is
 * the zero-page pair rc[2n] and rc[2n+1], whose address comes from the linker's
 * __rc(2n) symbol, or from 2n itself when the symbol is absent. */
static bool frame_base_value(const dwarf_info_t *di, const func_t *fn,
                             uint8_t (*readmem)(uint16_t), uint16_t *out)
{
    if (!fn->fb || fn->fb_len < 1)
        return false;
    uint8_t op = fn->fb[0];
    int rs = -1; /* the n of RSn */
    if (op == DW_OP_regx)
    {
        uint64_t reg = uleb_raw(fn->fb + 1, fn->fb + fn->fb_len, NULL);
        if (reg >= DW_MOS_RS0 && reg < DW_MOS_RS0 + DW_MOS_IMAG_MAX)
            rs = (int)(reg - DW_MOS_RS0);
    }
    else if (op >= DW_OP_reg0 && op <= DW_OP_reg31)
    {
        rs = 0;
    }
    if (rs < 0)
        return false;
    int rc = rs * 2;
    uint32_t zp = (rc < 256 && di->rc_known[rc]) ? di->rc_addr[rc] : (uint32_t)rc;
    if (zp > 0xFE)
        return false;
    *out = (uint16_t)(readmem((uint16_t)zp) | (readmem((uint16_t)(zp + 1)) << 8));
    return true;
}

static const func_t *find_func(const dwarf_info_t *di, uint16_t pc)
{
    for (int i = 0; i < di->nfuncs; i++)
        if (pc >= di->funcs[i].lo && pc < di->funcs[i].hi)
            return &di->funcs[i];
    return NULL;
}

bool dwarf_info_frame_base(const dwarf_info_t *di, uint16_t pc,
                           uint8_t (*readmem)(uint16_t addr), uint16_t *out)
{
    if (!di || !readmem)
        return false;
    const func_t *fn = find_func(di, pc);
    if (!fn)
        return false;
    return frame_base_value(di, fn, readmem, out);
}

int dwarf_info_locals(const dwarf_info_t *di, uint16_t pc, uint16_t frame_base,
                      bool base_ok, dwarf_var_t *out, int max)
{
    if (!di)
        return 0;
    const func_t *fn = find_func(di, pc);
    if (!fn)
        return 0;

    int n = 0;
    for (int i = 0; i < fn->nlocals && n < max; i++)
    {
        const lvar_t *v = &fn->locals[i];
        if (pc < v->lo || pc >= v->hi)
            continue;
        dwarf_var_t r;
        r.name = v->name;
        r.type = v->type;
        r.addr = 0;
        r.addr_ok = false;
        if (v->loc_len >= 1)
        {
            uint8_t op = v->loc[0];
            if (op == DW_OP_addr && v->loc_len >= 2) /* the opcode plus at least one address byte */
            {
                uint32_t a = 0;
                for (uint32_t k = 0; k + 1 <= v->loc_len - 1 && k < 4; k++)
                    a |= (uint32_t)v->loc[1 + k] << (8 * k);
                r.addr = (uint16_t)a;
                r.addr_ok = true;
            }
            else if (op == DW_OP_fbreg && base_ok)
            {
                int64_t off = sleb_raw(v->loc + 1, v->loc + v->loc_len);
                r.addr = (uint16_t)((int32_t)frame_base + (int32_t)off);
                r.addr_ok = true;
            }
        }
        out[n++] = r;
    }
    return n;
}

dw_kind_t dwarf_type_kind(const dtype_t *t) { return t ? (dw_kind_t)t->kind : DW_KIND_UNKNOWN; }
uint32_t dwarf_type_size(const dtype_t *t) { return t ? t->size : 0; }
const char *dwarf_type_name(const dtype_t *t) { return t && t->name ? t->name : "?"; }
int dwarf_type_encoding(const dtype_t *t) { return t ? t->encoding : 0; }

const dtype_t *dwarf_type_pointee(const dtype_t *t) { return t ? t->inner : NULL; }

const dtype_t *dwarf_type_element(const dtype_t *t, uint32_t *count)
{
    if (!t) return NULL;
    if (count) *count = t->count;
    return t->inner;
}

int dwarf_type_member_count(const dtype_t *t) { return t ? t->nmembers : 0; }

bool dwarf_type_member(const dtype_t *t, int i, const char **name,
                       uint32_t *offset, const dtype_t **type)
{
    if (!t || i < 0 || i >= t->nmembers)
        return false;
    if (name) *name = t->members[i].name;
    if (offset) *offset = t->members[i].offset;
    if (type) *type = t->members[i].type;
    return true;
}

const char *dwarf_type_enum_name(const dtype_t *t, int64_t value)
{
    if (!t) return NULL;
    for (int i = 0; i < t->nenums; i++)
        if (t->enums[i].value == value)
            return t->enums[i].name;
    return NULL;
}
